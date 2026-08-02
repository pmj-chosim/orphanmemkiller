# OSS Japan 2026 CFP Draft
Deadline: 2026-08-24 23:59 JST | Event: 2026-12-07~09, Tokyo

---

## Title (pick one, or mix)

**A. Phantom Memory: The /dev/shm Leak Your Framework's Own Watchdog Can't Fix**

B. Catching Orphaned AI Workers with eBPF — Even When PyTorch Already Tried

C. Phantom Memory: An eBPF Safety Net for Silent Shared-Memory Leaks in AI Workloads

(Recommend A — it front-loads the counterintuitive hook: "your framework already tries to fix this, and still can't," which is a stronger draw for a technical audience than a generic "eBPF finds memory leaks" framing.)

---

## Abstract (~400 words, adjust to the actual field's word limit)

AI training and serving pipelines lean on multiprocessing everywhere: PyTorch DataLoader workers, Ray's distributed object store, LLM-serving engines splitting work across processes. All of them pass tensors between processes through `/dev/shm`. All of them assume a clean shutdown will free that memory.

Production doesn't do clean shutdowns. A parent process dies — SIGKILL, OOM-kill, an uncaught exception — and its worker children get reparented to PID 1. If those workers are still holding open file descriptors or memory mappings into `/dev/shm`, that memory does not come back. Standard monitoring cannot see it: the process tree looks normal (workers just have a new, boring parent), and the backing file is often already `unlink()`'d, so `ls /dev/shm` shows nothing while `df` quietly climbs. We call this Phantom Memory.

We built and tested an eBPF-based detector (CO-RE, `libbpf`, cgroup-scoped) that watches `sched_process_fork`/`sched_process_exit` to reconstruct parent/child relationships independent of the kernel's own reparenting timing, and reconciles that against which processes still hold `/dev/shm` — checking both open file descriptors *and* memory mappings, since a common and entirely valid pattern (`mmap()` then `close()` the fd) hides the reference from the first check alone. On detection, it does what Kubernetes itself does when terminating a pod: SIGTERM first, a grace period, then SIGKILL only if the process is still alive and still holding the memory.

We tested this against three independent scenarios, not just one synthetic script. PyTorch's DataLoader turned out to already defend itself — a built-in `os.getppid()` check reaps orphaned workers in under half a second, regardless of whether the parent died from an exception, `os._exit()`, or an external SIGKILL simulating a real OOM-kill. That defense has a hole, though: a worker that's genuinely stuck (blocked I/O, deep in a native extension call) can never run that check. We froze a worker to simulate exactly that, and our detector reclaimed it correctly where the library-level watchdog structurally could not. Ray, by contrast, has no equivalent self-healing — a single `kill -9` on the driver process leaves its plasma-store workers orphaned indefinitely, and our detector catches that cleanly with no tricks required.

We'll walk through the real debugging path that got us here, including two bugs discovered only by testing against real workloads instead of one synthetic case — a units mismatch that silently disabled our own PID-reuse safety check, and a coverage gap where our detector missed a real leak because it only checked file descriptors, not memory mappings. Measured overhead against a live PyTorch DataLoader benchmark was statistically indistinguishable from a probe-free baseline.

---

## Key takeaways (bullet form, for the "what will attendees learn" field)

- Why "just restart the process" and "the container runtime already handles this" are both wrong answers, and the precise kernel-level reason why (subreaper semantics reap zombies, not live orphans — no layer in the stack makes the call to kill a live process just because its parent died)
- Why even a well-maintained framework's own defense (PyTorch's ppid check) isn't sufficient, with a live demonstration of the exact gap
- A working, safety-conscious remediation design: SIGTERM-first, grace period, escalate only if needed, full audit logging, dry-run mode
- Concrete, reproducible evidence across two independent real-world workloads (PyTorch DataLoader, Ray), not just a synthetic demo
- Real troubleshooting stories: how a nanoseconds-vs-clock-ticks unit mismatch silently broke a safety feature, and how testing against a second real workload (Ray) exposed a detection blind spot (mmap vs. fd) the first workload never would have

---

## Speaker bio (reuse from Ubucon Korea submission, lightly trimmed)

Modernization Solutions Architect at Megazone Cloud and Microsoft Certified Trainer (MCT). Core developer of HoneyBeePF, an open-source Aya/Rust-based eBPF agent that has been mentioned by eBPF Foundation chair Bill Mulligan. Focused on the gap between Kubernetes orchestration and Linux kernel technology — infrastructure observability that turns low-level kernel behavior into something operations engineers can act on. Has spoken at .NET Conf, Korea MCT Summit, and other conferences, with a focus on making complex low-level kernel concepts into practical playbooks engineers can use immediately.

---

## TODO before submitting

- [ ] Confirm actual Sessionize form fields/word limits for OSS Japan 2026 and fit this draft to them
- [ ] Decide whether to mention HoneyBeePF tie-in for this specific talk, or keep this project standalone (bio still references HoneyBeePF as a separate credential — fine either way, just be consistent with what you present)
- [ ] GitHub repo isn't public yet (Task #11) — either publish before submitting so the CFP can link it, or omit the link and add it once accepted
- [ ] Track selects "session level" (mark Intermediate/Advanced — this assumes eBPF/kernel familiarity)
- [ ] If there's a separate "notes to reviewers" field, consider adding: demo is fully scripted and reproducible on a single-node CPU-only AKS cluster, no GPU dependency
