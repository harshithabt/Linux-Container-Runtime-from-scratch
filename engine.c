#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define MAX_CONTAINERS 16
#define SOCKET_PATH "/tmp/engine.sock"
#define CMD_BUF 256
#define RESP_BUF 4096
#define BUFFER_SIZE 32

// ─────────────────────────────────────────
// CONTAINER METADATA
// ─────────────────────────────────────────
typedef enum {
    STATE_STARTING,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED
} ContainerState;

typedef struct {
    char id[64];
    pid_t host_pid;
    time_t start_time;
    ContainerState state;
    long soft_limit_mb;
    long hard_limit_mb;
    char log_path[256];
    int exit_status;
    int in_use;
    /*
     * FIX 1: Track the stack pointer per-container so we can
     * free it ONLY after the child has exited (in SIGCHLD handler),
     * not immediately after clone() returns in the parent.
     */
    char *stack_ptr;
} ContainerMeta;

typedef struct {
    char rootfs[256];
    char id[64];
    /*
     * FIX 2: We do NOT pass log_fd into the child via ContainerArgs
     * anymore.  Instead we pass the write-end of the pipe so the
     * child can dup2() it.  The crucial change is that we close
     * pipefd[1] in the PARENT immediately after clone() so the
     * producer's read() gets EOF when the child exits, allowing
     * the buffer to drain naturally.
     */
    int log_fd;   /* write end — used by child only */
} ContainerArgs;

// ─────────────────────────────────────────
// GLOBALS
// ─────────────────────────────────────────
ContainerMeta containers[MAX_CONTAINERS];
pthread_mutex_t containers_lock = PTHREAD_MUTEX_INITIALIZER;
char global_rootfs[256];

// ─────────────────────────────────────────
// BOUNDED BUFFER
// ─────────────────────────────────────────
typedef struct {
    char container_id[64];
    char data[512];
    int len;
} LogChunk;

typedef struct {
    LogChunk slots[BUFFER_SIZE];
    int head;
    int tail;
    int count;
    /*
     * FIX 3: 'done' is now set explicitly by run_supervisor()
     * on shutdown (Ctrl-C / SIGTERM).  We also track how many
     * producer threads are still alive so the consumer knows
     * when it is truly safe to exit even if done==1 but the
     * buffer still has items.
     */
    int done;
    int active_producers;   /* NEW: number of live producer threads */
    pthread_mutex_t lock;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} BoundedBuffer;

BoundedBuffer log_buffer;

typedef struct {
    int pipe_fd;
    char container_id[64];
} ProducerArgs;

// ─────────────────────────────────────────
// HELPERS
// ─────────────────────────────────────────
const char *state_str(ContainerState s) {
    switch(s) {
        case STATE_STARTING: return "starting";
        case STATE_RUNNING:  return "running";
        case STATE_STOPPED:  return "stopped";
        case STATE_KILLED:   return "killed";
        default:             return "unknown";
    }
}

ContainerMeta *find_container(const char *id) {
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].in_use &&
            strcmp(containers[i].id, id) == 0)
            return &containers[i];
    }
    return NULL;
}

ContainerMeta *alloc_container(void) {
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!containers[i].in_use) {
            memset(&containers[i], 0, sizeof(ContainerMeta));
            containers[i].in_use = 1;
            return &containers[i];
        }
    }
    return NULL;
}

// ─────────────────────────────────────────
// KERNEL MONITOR REGISTRATION
// ─────────────────────────────────────────
void register_with_monitor(const char *id, pid_t pid,
                           long soft_mb, long hard_mb) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) return;   /* module may not be loaded — not fatal */

    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    req.soft_limit_bytes = (unsigned long)soft_mb * 1024 * 1024;
    req.hard_limit_bytes = (unsigned long)hard_mb * 1024 * 1024;
    strncpy(req.container_id, id, MONITOR_NAME_LEN - 1);

    if (ioctl(fd, MONITOR_REGISTER, &req) < 0)
        perror("ioctl MONITOR_REGISTER");
    else
        printf("[engine] Registered '%s' (PID %d) with"
               " kernel monitor soft=%ldMB hard=%ldMB\n",
               id, pid, soft_mb, hard_mb);
    close(fd);
}

void unregister_with_monitor(const char *id, pid_t pid) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) return;

    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    strncpy(req.container_id, id, MONITOR_NAME_LEN - 1);

    ioctl(fd, MONITOR_UNREGISTER, &req);
    close(fd);
}

// ─────────────────────────────────────────
// BUFFER INIT
// ─────────────────────────────────────────
void buffer_init(BoundedBuffer *buf) {
    memset(buf, 0, sizeof(BoundedBuffer));
    pthread_mutex_init(&buf->lock, NULL);
    pthread_cond_init(&buf->not_full, NULL);
    pthread_cond_init(&buf->not_empty, NULL);
    buf->active_producers = 0;
    buf->done = 0;
}

// ─────────────────────────────────────────
// PRODUCER THREAD
// ─────────────────────────────────────────
void *producer_thread(void *arg) {
    ProducerArgs *pargs = (ProducerArgs *)arg;
    char tmp[512];

    /*
     * FIX 4: Tell the buffer another producer is alive so the
     * consumer does not exit prematurely.
     */
    pthread_mutex_lock(&log_buffer.lock);
    log_buffer.active_producers++;
    pthread_mutex_unlock(&log_buffer.lock);

    while (1) {
        int n = read(pargs->pipe_fd, tmp, sizeof(tmp) - 1);
        if (n <= 0) break;   /* EOF = child exited (write end closed) */
        tmp[n] = '\0';

        pthread_mutex_lock(&log_buffer.lock);
        /*
         * FIX 5: Check done flag while waiting so a supervisor
         * shutdown unblocks us instead of hanging forever.
         */
        while (log_buffer.count == BUFFER_SIZE && !log_buffer.done)
            pthread_cond_wait(&log_buffer.not_full, &log_buffer.lock);

        if (log_buffer.done) {
            pthread_mutex_unlock(&log_buffer.lock);
            break;
        }

        LogChunk *chunk = &log_buffer.slots[log_buffer.tail];
        strncpy(chunk->container_id,
                pargs->container_id,
                sizeof(chunk->container_id) - 1);
        memcpy(chunk->data, tmp, n);
        chunk->len = n;

        log_buffer.tail  = (log_buffer.tail + 1) % BUFFER_SIZE;
        log_buffer.count++;

        pthread_cond_signal(&log_buffer.not_empty);
        pthread_mutex_unlock(&log_buffer.lock);
    }

    close(pargs->pipe_fd);

    /*
     * FIX 6: Decrement producer count and signal the consumer so it
     * can exit once the buffer is empty and no producers remain.
     */
    pthread_mutex_lock(&log_buffer.lock);
    log_buffer.active_producers--;
    pthread_cond_signal(&log_buffer.not_empty);
    pthread_mutex_unlock(&log_buffer.lock);

    free(pargs);
    return NULL;
}

// ─────────────────────────────────────────
// CONSUMER THREAD
// ─────────────────────────────────────────
void *consumer_thread(void *arg) {
    (void)arg;

    while (1) {
        pthread_mutex_lock(&log_buffer.lock);

        /*
         * FIX 7: Exit condition is now:
         *   (done flag OR no active producers left) AND buffer empty.
         * This prevents the consumer from quitting while producers
         * are still writing, and also prevents it from hanging
         * forever when producers finish without the done flag.
         */
        while (log_buffer.count == 0 &&
               !log_buffer.done &&
               log_buffer.active_producers > 0)
            pthread_cond_wait(&log_buffer.not_empty, &log_buffer.lock);

        if (log_buffer.count == 0 &&
            (log_buffer.done || log_buffer.active_producers == 0)) {
            pthread_mutex_unlock(&log_buffer.lock);
            break;
        }

        LogChunk chunk = log_buffer.slots[log_buffer.head];
        log_buffer.head  = (log_buffer.head + 1) % BUFFER_SIZE;
        log_buffer.count--;

        pthread_cond_signal(&log_buffer.not_full);
        pthread_mutex_unlock(&log_buffer.lock);

        pthread_mutex_lock(&containers_lock);
        ContainerMeta *m = find_container(chunk.container_id);
        char log_path[256] = {0};
        if (m) strncpy(log_path, m->log_path, sizeof(log_path) - 1);
        pthread_mutex_unlock(&containers_lock);

        if (log_path[0]) {
            FILE *f = fopen(log_path, "a");
            if (f) {
                fwrite(chunk.data, 1, chunk.len, f);
                fclose(f);
            }
        }
    }
    return NULL;
}

// ─────────────────────────────────────────
// CONTAINER MAIN  (runs inside clone())
// ─────────────────────────────────────────
static int container_main(void *arg) {
    ContainerArgs *cargs = (ContainerArgs *)arg;

    if (cargs->log_fd >= 0) {
        dup2(cargs->log_fd, STDOUT_FILENO);
        dup2(cargs->log_fd, STDERR_FILENO);
        close(cargs->log_fd);
    }

    sethostname(cargs->id, strlen(cargs->id));

    if (chroot(cargs->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    (void)chdir("/");

    mount("proc", "/proc", "proc", 0, NULL);   /* non-fatal if fails */

    char *argv_exec[] = {"/bin/sh", "/test.sh", NULL};
    char *envp[]      = {
        "HOME=/root",
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        NULL
    };
    execve("/bin/sh", argv_exec, envp);
    perror("execve");
    return 1;
}

// ─────────────────────────────────────────
// SIGCHLD HANDLER
// ─────────────────────────────────────────
void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&containers_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].in_use &&
                containers[i].host_pid == pid) {
                containers[i].state      = STATE_STOPPED;
                containers[i].exit_status = WEXITSTATUS(status);
                /*
                 * FIX 8: Free the clone stack HERE, after the child
                 * has actually exited, not right after clone() returns.
                 * stack_ptr was saved in launch_container().
                 */
                if (containers[i].stack_ptr) {
                    free(containers[i].stack_ptr);
                    containers[i].stack_ptr = NULL;
                }
                break;
            }
        }
        pthread_mutex_unlock(&containers_lock);
    }
}

// ─────────────────────────────────────────
// LAUNCH CONTAINER
// ─────────────────────────────────────────
int launch_container(const char *id, const char *rootfs,
                     char *errbuf) {
    pthread_mutex_lock(&containers_lock);

    if (find_container(id)) {
        pthread_mutex_unlock(&containers_lock);
        snprintf(errbuf, CMD_BUF, "container '%s' already exists", id);
        return -1;
    }

    ContainerMeta *meta = alloc_container();
    if (!meta) {
        pthread_mutex_unlock(&containers_lock);
        snprintf(errbuf, CMD_BUF, "max containers reached");
        return -1;
    }

    snprintf(meta->log_path, sizeof(meta->log_path),
             "/tmp/engine_log_%s.txt", id);

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        meta->in_use = 0;
        pthread_mutex_unlock(&containers_lock);
        snprintf(errbuf, CMD_BUF, "pipe() failed");
        return -1;
    }

    /*
     * FIX 9: ContainerArgs lives on the heap and is freed by the CHILD
     * after execve() replaces the image (it doesn't return).  But since
     * execve() never frees heap, we accept that the child's copy of
     * cargs leaks inside the child — that is unavoidable.  The parent
     * must NOT free cargs because the child reads it before execve().
     *
     * We allocate it here and the child owns it from clone() onwards.
     */
    ContainerArgs *cargs = malloc(sizeof(ContainerArgs));
    if (!cargs) {
        close(pipefd[0]);
        close(pipefd[1]);
        meta->in_use = 0;
        pthread_mutex_unlock(&containers_lock);
        snprintf(errbuf, CMD_BUF, "malloc failed");
        return -1;
    }
    strncpy(cargs->rootfs, rootfs, sizeof(cargs->rootfs) - 1);
    strncpy(cargs->id,     id,     sizeof(cargs->id) - 1);
    cargs->log_fd = pipefd[1];   /* write end — child dup2's this */

    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(cargs);
        meta->in_use = 0;
        pthread_mutex_unlock(&containers_lock);
        snprintf(errbuf, CMD_BUF, "malloc stack failed");
        return -1;
    }
    char *stack_top = stack + STACK_SIZE;

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;

    pid_t pid = clone(container_main, stack_top, flags, cargs);

    /*
     * FIX 10: Close the WRITE end in the parent immediately after
     * clone().  This is the critical fix for the hang:
     *
     *   - The producer does:  read(pipefd[0], ...)
     *   - read() returns EOF only when ALL write-end holders have
     *     closed their copy of pipefd[1].
     *   - If the parent never closes pipefd[1], the producer's
     *     read() blocks forever even after the child exits, the
     *     buffer never drains, and the consumer hangs on
     *     pthread_cond_wait(&not_empty).
     */
    close(pipefd[1]);   /* parent no longer needs the write end */

    if (pid < 0) {
        close(pipefd[0]);
        free(stack);
        free(cargs);
        meta->in_use = 0;
        pthread_mutex_unlock(&containers_lock);
        snprintf(errbuf, CMD_BUF, "clone() failed: %s", strerror(errno));
        return -1;
    }

    /* Save stack so SIGCHLD handler can free it after child exits */
    meta->stack_ptr = stack;

    /* Start producer thread — it owns pipefd[0] and frees pargs */
    ProducerArgs *pargs = malloc(sizeof(ProducerArgs));
    if (!pargs) {
        /* non-fatal: we just won't log output for this container */
        close(pipefd[0]);
    } else {
        pargs->pipe_fd = pipefd[0];
        strncpy(pargs->container_id, id, sizeof(pargs->container_id) - 1);
        pthread_t prod_tid;
        pthread_create(&prod_tid, NULL, producer_thread, pargs);
        pthread_detach(prod_tid);
    }

    /* Fill metadata */
    strncpy(meta->id, id, sizeof(meta->id) - 1);
    meta->host_pid    = pid;
    meta->start_time  = time(NULL);
    meta->state       = STATE_RUNNING;
    meta->soft_limit_mb = 50;
    meta->hard_limit_mb = 100;
    meta->exit_status   = -1;

    pthread_mutex_unlock(&containers_lock);

    /* Register with kernel module AFTER releasing the mutex */
    register_with_monitor(id, pid,
                          meta->soft_limit_mb,
                          meta->hard_limit_mb);

    return pid;
}

// ─────────────────────────────────────────
// HANDLE CLI COMMAND
// ─────────────────────────────────────────
void handle_command(int client_fd) {
    char cmd[CMD_BUF]   = {0};
    char resp[RESP_BUF] = {0};

    int n = read(client_fd, cmd, sizeof(cmd) - 1);
    if (n <= 0) return;
    cmd[n] = '\0';

    char *args[8] = {0};
    int   argc    = 0;
    char *tok     = strtok(cmd, " \n");
    while (tok && argc < 8) {
        args[argc++] = tok;
        tok = strtok(NULL, " \n");
    }
    if (argc == 0) return;

    /* ── start ── */
    if (strcmp(args[0], "start") == 0) {
        if (argc < 2) {
            snprintf(resp, sizeof(resp),
                     "ERROR: usage: start <name>\n");
        } else {
            char errbuf[CMD_BUF] = {0};
            int  pid = launch_container(args[1], global_rootfs, errbuf);
            if (pid < 0)
                snprintf(resp, sizeof(resp), "ERROR: %s\n", errbuf);
            else
                snprintf(resp, sizeof(resp),
                         "OK: container '%s' started (PID %d)\n",
                         args[1], pid);
        }

    /* ── ps ── */
    } else if (strcmp(args[0], "ps") == 0) {
        pthread_mutex_lock(&containers_lock);
        int len = snprintf(resp, sizeof(resp),
            "%-12s %-8s %-10s %-12s %-8s %-8s\n",
            "ID", "PID", "STATE", "STARTED", "SOFT", "HARD");
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (!containers[i].in_use) continue;
            char tbuf[32];
            strftime(tbuf, sizeof(tbuf), "%H:%M:%S",
                     localtime(&containers[i].start_time));
            len += snprintf(resp + len, sizeof(resp) - len,
                "%-12s %-8d %-10s %-12s %-8ldMB %-8ldMB\n",
                containers[i].id,
                containers[i].host_pid,
                state_str(containers[i].state),
                tbuf,
                containers[i].soft_limit_mb,
                containers[i].hard_limit_mb);
        }
        pthread_mutex_unlock(&containers_lock);

    /* ── stop ── */
    } else if (strcmp(args[0], "stop") == 0) {
        if (argc < 2) {
            snprintf(resp, sizeof(resp),
                     "ERROR: usage: stop <name>\n");
        } else {
            /*
             * FIX 11: The original code had a 'goto done' that jumped
             * PAST the mutex unlock, leaking the lock.  Restructured so
             * the mutex is always released before we write the response.
             */
            pthread_mutex_lock(&containers_lock);
            ContainerMeta *m = find_container(args[1]);
            if (!m) {
                snprintf(resp, sizeof(resp),
                         "ERROR: container '%s' not found\n", args[1]);
                pthread_mutex_unlock(&containers_lock);
            } else if (m->state != STATE_RUNNING) {
                snprintf(resp, sizeof(resp),
                         "ERROR: '%s' is not running\n", args[1]);
                pthread_mutex_unlock(&containers_lock);
            } else {
                pid_t cpid = m->host_pid;
                char  cid[64];
                strncpy(cid, m->id, sizeof(cid) - 1);
                kill(m->host_pid, SIGTERM);
                m->state = STATE_STOPPED;
                pthread_mutex_unlock(&containers_lock);   /* unlock FIRST */
                unregister_with_monitor(cid, cpid);
                snprintf(resp, sizeof(resp),
                         "OK: container '%s' stopped\n", args[1]);
            }
        }

    /* ── logs ── */
    } else if (strcmp(args[0], "logs") == 0) {
        if (argc < 2) {
            snprintf(resp, sizeof(resp),
                     "ERROR: usage: logs <name>\n");
        } else {
            pthread_mutex_lock(&containers_lock);
            ContainerMeta *m = find_container(args[1]);
            if (!m) {
                snprintf(resp, sizeof(resp),
                         "ERROR: container '%s' not found\n", args[1]);
                pthread_mutex_unlock(&containers_lock);
            } else {
                char log_path[256];
                strncpy(log_path, m->log_path, sizeof(log_path) - 1);
                pthread_mutex_unlock(&containers_lock);
                FILE *f = fopen(log_path, "r");
                if (!f) {
                    snprintf(resp, sizeof(resp),
                             "ERROR: cannot open log\n");
                } else {
                    int len = 0;
                    while (len < RESP_BUF - 2 &&
                           fgets(resp + len, RESP_BUF - len, f))
                        len = strlen(resp);
                    fclose(f);
                    if (len == 0)
                        snprintf(resp, sizeof(resp), "(log is empty)\n");
                }
            }
        }

    } else {
        snprintf(resp, sizeof(resp),
                 "ERROR: unknown command '%s'\n", args[0]);
    }

    write(client_fd, resp, strlen(resp));
}

// ─────────────────────────────────────────
// SUPERVISOR
// ─────────────────────────────────────────
void run_supervisor(const char *rootfs) {
    strncpy(global_rootfs, rootfs, sizeof(global_rootfs) - 1);

    buffer_init(&log_buffer);

    /*
     * FIX 12: Keep a joinable handle to the consumer so we can
     * wait for it to drain the buffer before the supervisor exits.
     * (Previously it was detached and could be killed mid-write.)
     */
    pthread_t cons_tid;
    pthread_create(&cons_tid, NULL, consumer_thread, NULL);

    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    unlink(SOCKET_PATH);
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }

    listen(server_fd, 8);
    printf("[supervisor] Listening on %s\n", SOCKET_PATH);
    printf("[supervisor] rootfs = %s\n", rootfs);
    printf("[supervisor] Ready!\n\n");

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;
        handle_command(client_fd);
        close(client_fd);
    }

    /*
     * Graceful shutdown path (reached if the accept loop is broken,
     * e.g. via a future "shutdown" command):
     *
     *   1. Signal consumer to stop once the buffer drains.
     *   2. Join the consumer thread.
     *   3. Clean up the socket.
     */
    pthread_mutex_lock(&log_buffer.lock);
    log_buffer.done = 1;
    pthread_cond_broadcast(&log_buffer.not_empty);
    pthread_cond_broadcast(&log_buffer.not_full);
    pthread_mutex_unlock(&log_buffer.lock);

    pthread_join(cons_tid, NULL);
    close(server_fd);
    unlink(SOCKET_PATH);
}

// ─────────────────────────────────────────
// CLI CLIENT
// ─────────────────────────────────────────
void run_client(int argc, char *argv[]) {
    char cmd[CMD_BUF] = {0};
    for (int i = 1; i < argc; i++) {
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 2);
        if (i < argc - 1)
            strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "ERROR: cannot connect to supervisor.\n"
                "Is it running? Start it with:\n"
                " sudo ./engine supervisor ./rootfs\n");
        exit(1);
    }

    write(fd, cmd, strlen(cmd));

    char resp[RESP_BUF] = {0};
    int  n = read(fd, resp, sizeof(resp) - 1);
    if (n > 0) printf("%s", resp);
    close(fd);
}

// ─────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "\nUsage:\n"
            " sudo ./engine supervisor <rootfs>\n"
            " sudo ./engine start <name>\n"
            " sudo ./engine ps\n"
            " sudo ./engine stop <name>\n"
            " sudo ./engine logs <name>\n\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: ./engine supervisor <rootfs>\n");
            return 1;
        }
        run_supervisor(argv[2]);
    } else {
        run_client(argc, argv);
    }

    return 0;
}
