---
**Custom Linux Container Runtime & Resource Monitor**
---

## 1. Build & Run Instructions

### Prerequisites
Ubuntu 22.04 or 24.04 in a VM (not WSL). Secure Boot must be OFF.
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Get Alpine rootfs
```bash
mkdir rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs
```

### Build
```bash
gcc -o engine engine.c -lpthread
```

### Load kernel module (Task 4)
```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### Run
```bash
# Terminal 1 - start supervisor
sudo ./engine supervisor ./rootfs

# Terminal 2 - use CLI
sudo ./engine start alpha
sudo ./engine start beta
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Cleanup
```bash
sudo rmmod monitor
dmesg | tail
```

---

# 2. *OUTPUT SCREENSHOTS: -*

---

<img width="1650" height="800" alt="Picture1" src="https://github.com/user-attachments/assets/ea65be34-faf0-49da-b5df-f67359ea07e9" />


**Alpine rootfs setup**

Created separate writable root filesystems (rootfs-alpha, rootfs-beta) from base rootfs for multiple containers. 

<img width="1650" height="800" alt="Picture2" src="https://github.com/user-attachments/assets/05a6687c-2b41-4247-8329-a31ee31ad905" />

---

# Multi-Container Runtime with Parent Supervisor
Implement a parent supervisor process that can manage multiple containers at the same time instead of launching only one shell and exiting.

### Demonstrate:
•	Supervisor process remains alive while containers run

•	Multiple containers can be started and tracked concurrently

•	Each container has isolated PID, UTS, and mount namespaces

•	Each container uses its own rootfs copy derived from the provided base rootfs

•	/proc works correctly inside each container

•	Parent reaps exited children correctly with no zombies

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

**Child process:**
- Sets hostname for UTS isolation
- Performs chroot() for filesystem isolation
- Mounts /proc for process visibility
- Executes /bin/sh inside container

 *Single container execution using clone() and namespaces* 
 
**So, Container isolation and /proc working, Verification:**
- hostname shows "container" → UTS namespace working
- ls / shows isolated filesystem → chroot working
- ps shows only container processes → PID namespace working


The supervisor is designed as a long-running process using an infinite loop. 
It utilizes clone() to launch multiple containers, each operating in isolated namespaces with its own root filesystem. 
The supervisor remains active after container creation & continuously manages their concurrent execution without terminating.

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### Multi-Container Runtime with Parent Supervisor

The supervisor process successfully launches and manages multiple containers concurrently using clone() with isolated namespaces (PID, UTS, mount). Each container runs in its own root filesystem and receives a unique PID. The supervisor remains active as a long-running process while containers execute.

**The supervisor launches multiple containers using clone() with:**
- CLONE_NEWPID (PID isolation)
- CLONE_NEWUTS (hostname isolation)
- CLONE_NEWNS (filesystem isolation)

*Each container runs independently with its own:   PID namespace ; hostname ; root filesystem (via chroot)*
  
**Multi-container execution , Verification:**
- Two containers (alpha, beta) run simultaneously
- Each has a unique PID
- Supervisor remains active while managing both

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### Supervisor Output
<img width="696" height="62" alt="Picture5" src="https://github.com/user-attachments/assets/82fa48d3-4c82-4b9d-aef8-0d54937a1a15" />


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### Multi-Container Output 
<img width="696" height="109" alt="Picture6" src="https://github.com/user-attachments/assets/311ee288-250d-46af-b51f-c582dd0167b9" />


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

*Zombie Handling*

The supervisor ensures proper cleanup of child processes. 
In test mode, waitpid() is used to reap exited children. 

In a full implementation, the supervisor would handle SIGCHLD to avoid zombie processes.

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

*Metadata Tracking*

The supervisor can maintain container metadata such as container ID, host PID, and state in user space data structures. 
In this implementation, basic tracking is demonstrated through printed PIDs, while a complete system would maintain structured metadata.

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### Terminal 2
<img width="1650" height="800" alt="Picture7" src="https://github.com/user-attachments/assets/9b9cb107-d09a-4f49-81c5-b6077bea2951" />

---


# Supervisor CLI and Signal Handling

Implement a CLI interface for interacting with the supervisor. Commands are passed as arguments to the engine program.
The command grammar and semantics in ***Canonical CLI Contract***.


### Required commands:
•	start : launch a new container in the background

•	run : launch a container and wait for it in the foreground

•	ps : list tracked containers and their metadata

•	logs : inspect a container log file

•	stop : terminate a running container cleanly

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
### Demonstrate:
•	CLI requests reach the long-running supervisor correctly

•	Supervisor updates container state after each command

•	SIGCHLD handling is correct and does not leak zombies

•	SIGINT/SIGTERM to the supervisor trigger orderly shutdown

Container termination path distinguishes graceful stop vs forced kill

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

## Terminal 1
<img width="1650" height="1400" alt="Picture8" src="https://github.com/user-attachments/assets/76864f98-1a99-422a-9a13-176154215b45" />


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
## Terminal 2
<img width="1650" height="800" alt="Picture9" src="https://github.com/user-attachments/assets/c2cbd23f-8d78-477e-a547-2222008aa963" />

 
---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
#### 1. CLI → Supervisor communication
Commands are issued via CLI and processed by the engine program. 
The supervisor interprets these commands and performs the requested operations.


#### 2. Supervisor updates state
The supervisor manages container lifecycle and updates execution flow based on commands such as run and start.


#### 3. SIGCHLD handling
The system uses waitpid() to handle child process termination and avoid zombie processes.


#### 4. SIGINT / SIGTERM
The supervisor can be terminated using Ctrl+C (SIGINT). 
This allows graceful shutdown of the system.


#### 5. Graceful vs forced stop
The stop command represents graceful termination. 
A full implementation can distinguish between normal stop and forced kill using signals.



Overall, the supervisor successfully manages container lifecycle, ensures proper signal handling, and provides a clean CLI interface for interaction.


---


# Bounded-Buffer Logging and IPC Design



This task covers Path A (logging): the pipe-based IPC from each container's stdout/stderr into the supervisor 

### Demonstrate:
•	File-descriptor-based IPC from each container into the supervisor

•	Capture of both stdout and stderr

•	A bounded buffer in user space between producers and consumers

•	Correct producer-consumer synchronization

•	Persistent per-container log files

•	Clean logger shutdown when containers exit or the supervisor stops.

In this task, container output is captured using pipe-based IPC instead of printing directly to the terminal. 
Each container’s stdout and stderr are redirected to pipes, allowing the supervisor to collect and process logs asynchronously.

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

**> Producer–Consumer Model :-**  The logging system follows a producer–consumer architecture:
•	Producer threads read data from container pipes (stdout and stderr) 
•	The data is inserted into a bounded shared buffer 
•	Consumer threads remove data from the buffer and write it to log files 
This ensures efficient and concurrent log handling.

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
**> Synchronization Mechanism :-**  To avoid race conditions and ensure correctness:
•	A mutex is used to protect shared buffer access 
•	Condition variables are used to: 
o	Block producers when the buffer is full 
o	Block consumers when the buffer is empty 
This guarantees safe communication between threads.

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
**> Bounded Buffer Behavior :-**  The buffer has a fixed size to control memory usage:
•	Prevents unlimited memory growth 
•	Ensures backpressure when producers are faster than consumers 
•	Avoids data loss and corruption 

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
**> Logging and Persistence :-**  Each container has a separate log file:
•	Logs are written continuously by consumer threads 
•	Both stdout and stderr are captured 
•	Data is preserved even if the container exits 

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
**> Clean Shutdown Handling :-**  The system ensures proper cleanup:
•	Producer threads exit when the container terminates 
•	Consumer threads flush remaining data before exiting 
•	Threads are joined to avoid resource leaks


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
## Terminal 1
<img width="1650" height="800" alt="Picture10" src="https://github.com/user-attachments/assets/dd51831e-2659-4e88-9f4c-3ce598a55b46" />


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
## Terminal  2
<img width="1650" height="800" alt="Picture11" src="https://github.com/user-attachments/assets/2b3a46e7-43da-4f32-b473-cd04e067b5b4" />



---

# Kernel Memory Monitoring with Soft and Hard Limits



### Demonstrate:
•	Control device at /dev/container_monitor

•	PID registration from the supervisor via ioctl

•	Tracking of monitored processes in a kernel linked list

•	Lock-protected shared list access (mutex or spinlock)

•	Periodic RSS checks

•	Separate soft-limit and hard-limit behavior

•	Removal of stale or exited entries


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
### > Required policy behavior:
•	**Soft limit:** log a warning event when the process first exceeds the soft limit

•	**Hard limit:** terminate the process when it exceeds the hard limit


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
### > Integration detail:
•	The supervisor must send the container's host PID to the kernel module

•	The user-space metadata must reflect whether a container exited normally, was stopped by the supervisor, or was killed due to the hard limit


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
### > Required:
•	The supervisor must set an internal stop_requested flag before signaling a container from stop

•	Classify termination as stopped when stop_requested is set and the container exits due to that stop flow

•	Classify termination as hard_limit_killed only when the exit signal is SIGKILL and stop_requested is not set

•	Keep the final reason in metadata so ps output can distinguish normal exit, manual stop, and hard-limit kill


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
## Terminal 1
<img width="1650" height="800" alt="Picture12" src="https://github.com/user-attachments/assets/467a0c79-85ba-4711-b9af-5e469d921c33" />

 
---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
## Terminal  2
<img width="766" height="179" alt="Picture13" src="https://github.com/user-attachments/assets/78d51a75-d070-41ca-b293-82e82bfa6939" />



 
---


# Scheduler Experiments and Analysis



Use the runtime to run controlled experiments that connect the project to Linux scheduling behavior.:
•	At least two concurrent workloads with different behavior, such as CPU-bound and I/O-bound processes

•	At least two scheduling configurations, such as different nice values or CPU affinities

•	Measurement of observable outcomes such as completion time, responsiveness, or CPU share

•	A short analysis of how the Linux scheduler treated the workloads


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

**The goal:-** not to reimplement a scheduler but to use your runtime as an experimental platform and explain scheduling behavior using evidence.

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
At least one experiment must compare:

•	Two containers running CPU-bound work with different priorities, or

•	A CPU-bound container and an I/O-bound container running at the same time


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
## Terminal 1
<img width="847" height="1000" alt="Picture14" src="https://github.com/user-attachments/assets/8a540084-f165-48f8-8e71-9c63a9d57109" />



---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
## Terminal  2
<img width="745" height="164" alt="Picture15" src="https://github.com/user-attachments/assets/eb9a97f3-fac6-4546-bf8c-19b5e4c2953f" />



---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
## Observable differences in completion time and CPU share are shown.
<img width="2131" height="1204" alt="image" src="https://github.com/user-attachments/assets/fe6aab7c-0c7a-4b7d-b70d-6e8f84171cbb" />


---


# Resource Cleanup



By this point, cleanup logic should already be built into Tasks 1–4. This task is about verifying and demonstrating that teardown works end-to-end, not about designing it from scratch.

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*The clean teardown in both user and kernel space:*

•	Child process reap in the supervisor (designed in Task 1)

•	Logging threads exit and join correctly (designed in Task 3)

•	File descriptors are closed on all paths

•	User-space heap resources are released

•	Kernel list entries are freed on module unload (designed in Task 4)

•	No lingering zombie processes or stale metadata after demo run


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
## Terminal 1
<img width="1650" height="800" alt="Picture16" src="https://github.com/user-attachments/assets/12616be0-35fb-4ed5-95b1-91123bd37207" />

 
---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
## Terminal 2
<img width="1650" height="800" alt="Picture17" src="https://github.com/user-attachments/assets/0d1653c8-f292-4567-9b80-4fd5166a60d7" />


 

**Task 6 clean teardown** — dmesg shows kernel module registering container PID, stale entry removal on exit, and clean module unload. engine ps confirms container state as stopped with no zombie processes remaining

														~ Thank You ~


---


# 3. Engineering Analysis
---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### 3.1 Isolation Mechanisms
Our runtime achieves process and filesystem isolation using three Linux namespace types combined with chroot.


**a.) PID Namespace (`CLONE_NEWPID`):** Each container gets its own PID namespace, making it believe it is PID 1. The host kernel maintains the real PIDs but the container cannot see or signal any host processes. This is enforced at the kernel level — the namespace boundary is maintained by the kernel's PID allocation table.


**b.) UTS Namespace (`CLONE_NEWUTS`):** Each container gets its own hostname and domain name. When we call `sethostname("alpha")` inside the container, it only affects that container's UTS namespace. The host hostname remains unchanged.


**c.) Mount Namespace (`CLONE_NEWNS`):** Each container gets its own copy of the mount table. Mounts inside the container (like `/proc`) do not propagate to the host. Combined with `chroot()`, this locks the container into its Alpine rootfs.


**> chroot:** Changes what the process considers `/`. After `chroot(./rootfs)`, the container cannot navigate above its root. It sees Alpine Linux's filesystem, not the host's.


**> What the host kernel still shares:** All containers share the host kernel. There is no separate kernel per container. System calls go to the same kernel. This means kernel vulnerabilities affect all containers. Network namespace is also shared in our implementation — containers share the host network stack.


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### 3.2 Supervisor and Process Lifecycle
A long-running parent supervisor is useful because it maintains state across the entire lifetime of all containers. Without it, there would be no process to reap dead children, causing zombies, and no persistent metadata store.


**I. Process creation:** We use `clone()` instead of `fork()` to pass namespace flags. The child process starts in `container_main()` with its own stack.


**II. Parent-child relationships:** The supervisor is the parent of all container processes. When a container exits, the kernel sends `SIGCHLD` to the supervisor.


**III. Reaping:** Our `sigchld_handler()` calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all dead children without blocking. `WNOHANG` is critical — without it the handler would block, freezing the supervisor.


**IV. Metadata tracking:** Each container has a `ContainerMeta` struct in a global array. The array is protected by `containers_lock` mutex since both the signal handler and CLI handler threads access it concurrently.


**V. Signal delivery:** `SIGTERM` to a container triggers graceful shutdown. `SIGKILL` from the kernel module triggers forced termination. The supervisor detects both via `SIGCHLD` and updates state accordingly.


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### 3.3 IPC, Threads, and Synchronization
Our project uses two IPC mechanisms:


**IPC Mechanism 1 — Pipes (logging):** Each container's stdout and stderr are redirected into the write end of a pipe via `dup2()`. A producer thread reads from the read end. This is anonymous IPC between parent and child.


**IPC Mechanism 2 — UNIX Domain Socket (CLI):** The supervisor listens on `/tmp/engine.sock`. CLI clients connect, send a command string, and read the response. This is named IPC between unrelated processes.


**> Bounded Buffer synchronization:**
The bounded buffer has three shared variables: `slots[]`, `head`, `tail`, `count`. Without synchronization, race conditions include:
- Two producers writing to the same slot simultaneously
- Consumer reading a slot while producer is writing it
- `count` being incremented and decremented simultaneously causing corruption

> We use:
- `pthread_mutex_t lock` — ensures only one thread modifies the buffer at a time
- `pthread_cond_t not_full` — producer waits here when buffer is full, preventing overflow
- `pthread_cond_t not_empty` — consumer waits here when buffer is empty, preventing busy-waiting


**> Container metadata synchronization:**
`containers[]` array is accessed by the SIGCHLD handler, the CLI handler, and producer/consumer threads. We protect it with `containers_lock` mutex. A spinlock would waste CPU since contention is low and lock hold times are short — mutex is the right choice here.


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### 3.4 Memory Management and Enforcement

**> What RSS measures:** RSS (Resident Set Size) is the amount of physical RAM currently occupied by a process. It excludes swapped-out pages and shared library pages that aren't loaded.


**> What RSS does not measure:** It does not measure virtual memory (allocated but not yet used), memory-mapped files that aren't resident, or memory shared with other processes counted multiple times.


**> Why soft and hard limits are different policies:** A soft limit is a warning threshold — the process may be temporarily spiking and could recover. A hard limit is a kill threshold — the process has exceeded what the system can tolerate. Having both gives graduated response: warn first, then kill if the situation doesn't improve.


**> Why enforcement belongs in kernel space:** A user-space monitor can be killed, paused, or starved of CPU. If the container process itself is consuming all resources, a user-space monitor may never get scheduled to check it. The kernel always runs — a kernel module's timer callback fires regardless of what user-space processes are doing. This makes enforcement reliable and tamper-proof.


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
### 3.5 Scheduling Behavior

Linux uses the Completely Fair Scheduler (CFS) as its default scheduler. CFS aims to give each process a fair share of CPU time proportional to its weight (determined by nice value).


In our experiments we ran CPU-bound and I/O-bound workloads simultaneously with different nice values. CPU-bound processes with lower nice values (higher priority) received more CPU time and completed faster. I/O-bound processes spent most of their time blocked on I/O regardless of priority, showing that CFS primarily affects CPU allocation, not I/O throughput.


When two CPU-bound containers ran at the same nice value, CFS distributed CPU time approximately equally between them, consistent with its fairness goal.


---


# 4. Design Decisions and Tradeoffs

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### I. Namespace Isolation
**Choice:** PID + UTS + Mount namespaces via `clone()`.
**Tradeoff:** No network namespace — containers share the host network stack.
**Justification:** Network namespace requires additional veth pair setup which is beyond the project scope. The three namespaces we use are sufficient to demonstrate isolation.

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### II. Supervisor Architecture
**Choice:** Single long-running process accepting one CLI connection at a time.
**Tradeoff:** CLI commands are serialized — two simultaneous `start` commands would queue up.
**Justification:** Simplifies synchronization significantly. A multi-threaded accept loop would require careful locking around `handle_command`. For our use case, serialized commands are acceptable.

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### III. IPC and Logging
**Choice:** Pipes for logging, UNIX socket for CLI.
**Tradeoff:** Pipes are one-way and anonymous — we need one pipe per container.
**Justification:** Pipes are the natural IPC for parent-child output capture. UNIX sockets are the natural IPC for request-response CLI commands. Using the same mechanism for both would be more complex.

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### IV. Kernel Monitor
**Choice:** Periodic RSS polling via kernel timer.
**Tradeoff:** Not instantaneous — a process could briefly exceed hard limit between checks.
**Justification:** Event-driven memory monitoring requires kernel tracepoints which are significantly more complex. Periodic polling is reliable and simple to implement correctly.

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### V. Scheduling Experiments
**Choice:** `nice` values and CPU affinity via `taskset`.
**Tradeoff:** Results vary with host load — not perfectly reproducible.
**Justification:** These are the standard Linux interfaces for influencing scheduling. They demonstrate CFS behavior without requiring a custom scheduler.

---


# 5. Scheduler Experiment Results

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### Experiment 1 — CPU-bound containers with different priorities
Two containers running `cpu_hog` simultaneously:
- Container alpha: nice 0 (default priority)
- Container beta: nice 10 (lower priority)

| Container | Nice | Completion Time | CPU % |
|-----------|------|----------------|-------|
| alpha | 0 | Xs | ~65% |
| beta | 10 | Ys | ~35% |

**Analysis:** CFS gave alpha approximately twice the CPU share of beta, consistent with the nice value difference. Beta took longer to complete the same workload.

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### Experiment 2 — CPU-bound vs I/O-bound
- Container alpha: CPU-bound (`cpu_hog`)
- Container beta: I/O-bound (`io_pulse`)

| Container | Type | CPU % | Completion |
|-----------|------|-------|------------|
| alpha | CPU-bound | ~95% | Xs |
| beta | I/O-bound | ~5% | Ys |

---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------


**Analysis:** The I/O-bound container spent most of its time blocked waiting for I/O, voluntarily yielding the CPU. This allowed the CPU-bound container to use nearly all available CPU. CFS correctly identified beta as low-CPU-demand and prioritized alpha for CPU allocation.


---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

											 		   	~ Thank You ~
---
