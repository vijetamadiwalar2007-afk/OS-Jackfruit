<img width="1536" height="1024" alt="image" src="https://github.com/user-attachments/assets/e0e57e97-c77e-400e-bf97-9a96624a572b" /># Multi-Container Runtime

## 1. Team Information

| Name | SRN |
|------|-----|
| Naga Tejaswini | PES2UG24AM095|
| Vijeta Madiwalar | PES2UG24AM808 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build
```bash
cd boilerplate
make
```

This builds:
- `engine` — user-space supervisor and CLI
- `monitor.ko` — kernel memory monitor module
- `memory_hog` — memory workload for testing limits
- `cpu_hog` — CPU workload for scheduling experiments

### Prepare rootfs
```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta

gcc -O0 -static -o memory_hog memory_hog.c
gcc -O0 -static -o cpu_hog cpu_hog.c
cp memory_hog rootfs-alpha/
cp memory_hog rootfs-beta/
cp cpu_hog rootfs-alpha/
cp cpu_hog rootfs-beta/
```

### Load kernel module
```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
sudo dmesg | tail -3
```

### Start supervisor
```bash
sudo ./engine supervisor ./rootfs-base
```

### Launch containers (in another terminal)
```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh -c "echo hello"
sudo ./engine start beta ./rootfs-beta /bin/sh -c "echo world"
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Run memory limit test
```bash
sudo ./engine start mem1 ./rootfs-alpha /memory_hog 70 --soft-mib 40 --hard-mib 64
sudo dmesg | tail -10
```

### Run scheduling experiment
```bash
sudo ./engine start high ./rootfs-alpha /cpu_hog 2000000000 --nice -5
sudo ./engine start low ./rootfs-beta /cpu_hog 2000000000 --nice 10
sudo ./engine logs high
sudo ./engine logs low
```

### Clean up
```bash
sudo ./engine stop alpha
sudo ./engine stop beta
# Ctrl+C to stop supervisor
sudo rmmod monitor
sudo dmesg | tail -3
```

---

## 3. Demo Screenshots

### Screenshot 1 — Multi-container supervision
Two containers (alpha, beta) running simultaneously under one supervisor process.

(Terminal 1)

<img width="840" height="175" alt="Screenshot 2026-04-15 095925" src="https://github.com/user-attachments/assets/efb4690e-2d2e-4a25-b596-8d26a2616f70" />

(Terminal 2)

<img width="1050" height="95" alt="WhatsApp Image 2026-04-14 at 12 12 22 PM" src="https://github.com/user-attachments/assets/a48265d5-7deb-4e5c-abd7-9bbfc62238d1" />


### Screenshot 2 — Metadata tracking
Output of `engine ps` showing container ID, PID, state, soft/hard limits, and log file path.

<img width="938" height="220" alt="WhatsApp Image 2026-04-15 at 11 44 12 AM" src="https://github.com/user-attachments/assets/c5feeb0d-656c-4349-81f2-d15d9a173467" />

### Screenshot 3 — Bounded-buffer logging
Log file contents captured through the producer-consumer logging pipeline.


<img width="921" height="238" alt="WhatsApp Image 2026-04-15 at 11 46 55 AM" src="https://github.com/user-attachments/assets/8547e365-62a8-4693-9c05-ea59843b9b52" />


### Screenshot 4 — CLI and IPC
A CLI `stop` command sent to the supervisor over a UNIX domain socket, with the supervisor responding.


<img width="1108" height="84" alt="WhatsApp Image 2026-04-15 at 11 59 18 AM" src="https://github.com/user-attachments/assets/182d7fb6-9164-4334-89b5-40268dc475ea" />

<img width="1536" height="1024" alt="image" src="https://github.com/user-attachments/assets/c9aa1066-6af1-4a13-aa65-df037ebcb3c7" />


### Screenshot 5 — Soft-limit warning
`dmesg` output showing the kernel module warning when a container exceeds its soft memory limit.


<img width="1228" height="239" alt="WhatsApp Image 2026-04-15 at 12 00 18 PM" src="https://github.com/user-attachments/assets/5febe3ae-3a78-4c4f-b3d4-a6232704d3f0" />


### Screenshot 6 — Hard-limit enforcement
`dmesg` output showing SIGKILL sent when a container exceeds its hard memory limit, with `ps` showing state as `killed`.
<img width="1228" height="239" alt="WhatsApp Image 2026-04-15 at 12 00 18 PM" src="https://github.com/user-attachments/assets/d911eb70-db5c-4b04-ad33-eb18f19fc65e" />



### Screenshot 7 — Scheduling experiment
Log output from two containers running the same CPU workload at different nice values (-5 vs 10), showing different completion times.

<img width="942" height="166" alt="image" src="https://github.com/user-attachments/assets/00e95f57-d885-40a4-81f3-8ba32a75091d" />
<img width="1536" height="1024" alt="image" src="https://github.com/user-attachments/assets/cf6a755e-d12b-4347-b950-52ef924fbd9d" />




### Screenshot 8 — Clean teardown
`ps aux` showing no zombie engine processes, and `dmesg` confirming clean module unload.
<img width="1536" height="1024" alt="image" src="https://github.com/user-attachments/assets/e663dd24-951e-4d95-adc3-f88e9ae38b2f" />



---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Our runtime achieves process and filesystem isolation using Linux namespaces and `chroot`. When `spawn_container()` calls `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS`, the kernel creates a new process that sees an independent PID space (PID 1 inside the container), its own hostname, and its own mount namespace.

At the kernel level, namespaces are implemented as reference-counted kernel objects. Each `task_struct` holds pointers to its namespace objects (`nsproxy`). When `clone()` is called with namespace flags, the kernel allocates new namespace structs and wires them into the child's `nsproxy`, leaving the parent's namespaces untouched.

`chroot()` changes the process's root directory pointer (`fs->root`) in the VFS layer, so all path resolution starts from the container's assigned rootfs directory. This prevents the container from accessing host files by absolute path.

What the host kernel still shares with all containers: the same kernel code, the same physical memory manager, the same scheduler, the same network stack (since we do not use CLONE_NEWNET), and the same hardware. Namespaces isolate the view of resources, not the resources themselves.

### 4.2 Supervisor and Process Lifecycle

A long-running parent supervisor is essential because Linux requires a parent process to call `wait()` on exited children — otherwise they become zombies occupying PID table slots indefinitely.

When `engine start` is issued, the CLI process connects to the supervisor's UNIX socket, sends a command string, and immediately exits. The supervisor calls `clone()` to create the container child, records its PID and metadata, and installs a `SIGCHLD` handler. When the container exits, the kernel delivers `SIGCHLD` to the supervisor. The handler calls `waitpid(-1, WNOHANG)` in a loop to reap all exited children without blocking.

The supervisor distinguishes termination causes using the `stop_requested` flag — if set before signaling, the termination is classified as `stopped`; if `SIGKILL` arrives without `stop_requested`, it is classified as `hard_limit_killed`.

### 4.3 IPC, Threads, and Synchronization

Our project uses two distinct IPC mechanisms:

**Path A (logging):** Each container's stdout and stderr are connected to the supervisor via `pipe()`. Before `clone()`, we create a pipe and `dup2()` the write end onto the child's stdout/stderr. A dedicated producer thread per container reads from this pipe and inserts log entries into the bounded buffer.

**Path B (control):** The CLI client connects to a UNIX domain socket (`/tmp/engine.sock`). The CLI sends a command string, reads the response, and exits.

**Bounded buffer synchronization:** The shared buffer uses a `pthread_mutex_t` to protect all access, and two `pthread_cond_t` variables — `not_full` (producer waits here when buffer is full) and `not_empty` (consumer waits here when buffer is empty).

Without these primitives, the following race conditions would occur:
- Two producer threads could simultaneously write to the same buffer slot, corrupting log data
- A consumer could read an entry before the producer finishes writing it (torn read)
- The consumer could spin indefinitely on an empty buffer wasting CPU

We chose mutex + condition variables over semaphores because condition variables allow us to atomically release the lock and sleep, which is necessary for the wait-until-not-full and wait-until-not-empty patterns.

**Container metadata** is protected by a separate `g_meta_mu` mutex, distinct from the buffer lock, preventing slow log writes from blocking container state updates.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical memory pages currently mapped and present in a process's page tables. It excludes swapped-out pages and memory that has been `malloc()`'d but never written (not yet faulted in). This is why our `memory_hog` uses `memset()` — without touching each page, the kernel uses lazy allocation and RSS stays low even after `malloc()`.

Soft and hard limits serve different policy goals. The soft limit is a warning threshold — the process continues running but the operator is notified. The hard limit is an enforcement threshold — the process is killed unconditionally when exceeded.

Enforcement belongs in kernel space for two reasons. First, a user-space monitor could be killed or starved by the very process it monitors. Second, the kernel has direct atomic access to `task_struct` and `mm_struct` for RSS measurement and can send signals without scheduling delays. Our kernel module uses a 1-second timer to check RSS via `get_mm_rss(task->mm)` and sends `SIGKILL` via `send_sig()` when the hard limit is exceeded.

### 4.5 Scheduling Behavior

Linux uses the Completely Fair Scheduler (CFS), which tracks a virtual runtime (`vruntime`) for each process. The scheduler always picks the process with the lowest `vruntime` to run next. Nice values influence the weight assigned to each process — lower nice values get higher weight, meaning their `vruntime` advances more slowly, so they get scheduled more often.

Our experiment ran two containers with the same workload at different nice values:

| Container | Nice value | Completion time |
|-----------|-----------|----------------|
| high      | -5        | ~2.2 seconds   |
| low       | +10       | ~2.9 seconds   |

The high-priority container completed faster because CFS gave it a larger share of CPU time. With nice -5, its weight is approximately 3x that of a nice 0 process, while nice +10 has approximately 0.25x the weight.

The I/O-bound vs CPU-bound comparison shows a different effect: I/O-bound processes voluntarily yield the CPU while waiting for I/O, keeping their `vruntime` low. CFS schedules them quickly when their I/O completes, improving responsiveness while CPU-bound processes accumulate vruntime and are periodically preempted.

---

## 5. Design Decisions and Tradeoffs

### Namespace isolation
**Choice:** Used `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` without `CLONE_NEWNET`.
**Tradeoff:** Containers share the host network stack and can interfere with each other's network connections.
**Justification:** Adding network namespaces requires configuring virtual ethernet pairs and routing, which is outside the project scope. PID, UTS, and mount isolation are sufficient for the required demonstrations.

### Supervisor architecture
**Choice:** Single-process supervisor with a non-blocking `accept()` loop and per-container producer threads.
**Tradeoff:** The main loop busy-polls with `usleep(10000)` when no connections arrive, wasting some CPU.
**Justification:** The simple accept loop is easy to reason about for correctness, and the 10ms polling interval is short enough for interactive use without significant CPU overhead.

### IPC and logging
**Choice:** UNIX domain socket for control (Path B) and pipes for logging (Path A).
**Tradeoff:** The socket-based control channel is synchronous — only one CLI command is processed at a time.
**Justification:** Container operations are inherently sequential, so synchronous processing is correct and simpler to implement than a concurrent command dispatcher.

### Kernel monitor
**Choice:** Kernel timer firing every 1 second for RSS checks.
**Tradeoff:** A process could exceed its hard limit and allocate significant memory in the window before the next timer fires.
**Justification:** 1-second granularity is sufficient for demonstration. A production system would use memory cgroups for immediate enforcement.

### Scheduling experiments
**Choice:** Used nice values rather than CPU affinity for scheduling experiments.
**Tradeoff:** Nice values only influence priority within CFS and do not guarantee CPU share on a lightly loaded system.
**Justification:** Nice values are the standard Linux mechanism for user-space priority adjustment and directly demonstrate CFS weight-based scheduling.

---

## 6. Scheduler Experiment Results

### Experiment A: CPU-bound workloads at different nice values

Both containers ran `cpu_hog 2000000000` (2 billion arithmetic iterations) simultaneously.

| Run | Container | Nice value | Completion time |
|-----|-----------|-----------|----------------|
| 1   | high      | -5        | 2.21 seconds   |
| 1   | low       | +10       | 2.92 seconds   |
| 2   | high      | -5        | 2.42 seconds   |
| 2   | low       | +10       | 2.45 seconds   |

The high-priority container consistently completed faster when both ran simultaneously. CFS assigns higher weight to lower nice values, so `high` received more CPU time slices per scheduling period, completing the same work in less wall-clock time.

### Experiment B: CPU-bound vs I/O-bound

The CPU-bound container (`cpu_hog`) ran alongside the I/O-bound container (`io_work`). The I/O-bound container showed better responsiveness because it voluntarily yields the CPU during I/O waits, keeping its `vruntime` low. The CPU-bound container accumulated high `vruntime` and was periodically preempted.

This demonstrates CFS's dual goal of fairness (both processes make progress) and responsiveness (I/O-bound processes are not starved by CPU hogs).
