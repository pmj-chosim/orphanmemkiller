# orphan-guard (working name)

An eBPF-based detector and remediator for **Phantom Memory**: orphaned
worker processes — reparented to PID 1 after their parent dies — that keep
holding `/dev/shm` shared-memory segments the kernel will never reclaim on
its own, while the pod they're in keeps running and reporting healthy.

Status as of 2026-08-02: core detection + remediation loop is implemented,
tested against a synthetic reproduction and two independent real-world
workloads (PyTorch DataLoader, Ray), validated for false positives against
real background services (Redis, Celery) sharing the same cgroup as the
leak, and defaults to a safe, log-only posture. Not yet packaged as an
installable artifact — see **Known limitations** below.

## Why this exists

AI training/serving pipelines lean on multiprocessing everywhere —
PyTorch DataLoader workers, Ray's distributed object store, LLM-serving
engines splitting work across processes — and all of them pass data
between processes through `/dev/shm`. When a parent process dies
(SIGKILL, OOM-kill, a crash) mid-flight, its worker children are
reparented to PID 1. If they're still holding a file descriptor or a
memory mapping into `/dev/shm`, that memory does not come back — and
because the *pod* itself never goes down, standard process-tree
monitoring sees nothing wrong.

## Why eBPF, and not just a containerd/CRI hook?

This is the first question any experienced reviewer will ask. Short
version — full argument in [`docs/cfp/why-ebpf-not-container-runtime.md`](docs/cfp/why-ebpf-not-container-runtime.md):

1. **The premise doesn't hold.** containerd hooks *pod* teardown. Phantom
   Memory's defining trait is that the pod never tears down — only an
   internal process dies. There's no pod-teardown event to hook.
2. **Even if there were, subreaper semantics don't help.** `PR_SET_CHILD_SUBREAPER`
   (what `containerd-shim`/`tini`/`systemd` use) reaps already-dead
   zombies — it is not a mechanism for killing children that are still
   alive. We verified this directly: every orphan we've reproduced showed
   up in `ps` fully alive (`S`/`R`), not `<defunct>`.
3. **Deciding whether a live orphan is a real leak needs a policy call the
   runtime has no basis for making.** A process that intentionally
   double-forks and detaches from its launcher (a legitimate, common Unix
   daemonization pattern) looks *identical* to a leaked worker from the
   kernel's point of view. We built exactly this as an adversarial test
   case (`repro/legit-daemon-pod.yaml` — a double-fork daemon that holds
   `/dev/shm` forever, on purpose) and confirmed the detector correctly
   leaves it alone because it's out of the watched cgroup's scope — but
   the runtime itself has no generic way to make that call.
4. **Remediation has to work without the target's cooperation.** PyTorch's
   own `os.getppid()` watchdog is a legitimate userspace fix, and it
   works — until the worker it's supposed to protect is blocked in native
   code and can never run that check. We demonstrated this concretely
   (see `repro/pytorch/stuck_worker_test.sh`): freeze a worker with
   `SIGSTOP` so it can never self-check, and orphan-guard still reclaims
   it (`SIGKILL` works on a stopped process; `SIGTERM` doesn't, which the
   escalation logic accounts for).

## How it works

- **`src/orphan_guard.bpf.c`** (clang `-target bpf`, CO-RE via `vmlinux.h`
  + `bpf_core_read.h`): hooks `tp_btf/sched_process_fork` and
  `tp_btf/sched_process_exit`, scoped to one container via cgroup id
  (`bpf_get_current_cgroup_id()`). Emits fork/exit events over a ring
  buffer. Filters out thread creation (`CLONE_THREAD`) so internal
  library threads (e.g. BLAS) don't show up as bogus fork events.
  Deliberately does **not** walk `task->children` at exit time
  (reparenting timing relative to the exit tracepoint isn't guaranteed
  across kernels) and does **not** use PID-namespace inode comparison
  (empirically unreliable on `kubectl exec` + `setns(CLONE_NEWPID)` —
  unexplained, abandoned in favor of cgroup scoping).
- **`src/orphan_guard.c`** (userspace, libbpf skeleton): reconstructs
  parent/child relationships from fork events, and on a tracked parent's
  exit, checks each child for a live `/dev/shm` reference — both open
  file descriptors *and* memory mappings (a process that `mmap()`s then
  `close()`s the fd is invisible to an fd-only check; this is exactly how
  Ray's plasma workers behave). If found, after a grace window: SIGTERM,
  wait, re-verify via `/proc/<pid>/stat` start time (guards against PID
  reuse during the grace window), escalate to SIGKILL only if still
  alive and still holding `/dev/shm`.
- A newly-tracked child is ignored for its first ~2 seconds
  (`MIN_CHILD_AGE_SEC`): container runtimes commonly double-fork
  internally and have the intermediate process exit right after the real
  target starts — normal, healthy launch handoff, not a crash. Confirmed
  in practice: without this guard, the agent killed its own benchmark
  process.

### Safety policy

- **Dry-run is the default, not opt-in.** Pass `--enforce` to actually
  send signals; without it, the agent only logs what it would do.
- **Structured, timestamped audit log** — every decision point emits one
  line: `<ISO8601> level=<INFO|ALERT|ACTION> pid=<n> action=<TAG>
  dry_run=<bool> detail="..."`. `SIGTERM_SENT` vs `SIGTERM_WOULD_SEND`
  (etc.) make it unambiguous whether an action actually happened.
- **`--exempt <pid>`**: manual allowlist for when you already know the pid.
- **`--namespace <ns> --pod-name <name>`**: at startup, checks the target
  pod's annotations via the in-cluster Kubernetes API (needs RBAC to `get`
  pods — see `build/orphan-guard-rbac.yaml`). If
  `orphan-guard.io/exempt: "true"` is present, forces permanent dry-run
  for that run regardless of `--enforce` — the real answer to "what if a
  legitimately long-lived, intentionally-detached daemon lives in the
  same cgroup we're watching." Verified against `legit-daemon-pod`:
  annotated -> forced dry-run even with `--enforce`; unannotated
  (`zombie-pod`) -> `--enforce` behaves normally. One-shot check at
  startup, not continuously re-polled — see Known limitations.

## Building

Requires a privileged pod (or host) with clang/llvm/libbpf-dev/bpftool and
BTF/debugfs/modules mounted — see `build/bpf-builder-pod.yaml` for a
reference environment.

```bash
# generate vmlinux.h from the live kernel's BTF (exact match, no fetching a
# generic header for the "wrong" kernel version)
bpftool btf dump file /sys/kernel/btf/vmlinux format c > src/vmlinux.h

cd src
clang -g -O2 -target bpf -D__TARGET_ARCH_x86 -I. -c orphan_guard.bpf.c -o orphan_guard.bpf.o
bpftool gen skeleton orphan_guard.bpf.o > orphan_guard.skel.h
gcc -g -O2 -Wall -I. -o orphan_guard orphan_guard.c -lbpf -lelf -lz
```

## Running

```bash
# find the target container's cgroup id (run on the host / a privileged
# pod that can see the host cgroupfs)
CID=$(kubectl get pod <pod> -o jsonpath='{.status.containerStatuses[0].containerID}' | sed 's#containerd://##')
CGPATH=$(find /sys/fs/cgroup -iname "*${CID}*")
CGID=$(stat -c %i "$CGPATH")

./orphan_guard --cgroup $CGID              # dry-run (default): logs only
./orphan_guard --cgroup $CGID --enforce    # actually remediates
./orphan_guard --help                      # full flag reference
```

## Reproducing the evaluation

Each real-world repro is self-contained under `repro/`:

- **`repro/`** — the original synthetic leak (`zombie-pod.yaml` +
  `leak_maker.py`), the adversarial false-positive control
  (`legit-daemon-pod.yaml` + `legit_daemon.py`, an intentional double-fork
  daemon), and `fork_stress.sh` (overhead microbenchmark).
- **`repro/pytorch/`** — real `torch.utils.data.DataLoader` reproductions.
  Notably: a plain uncaught exception and even an external `kill -9`
  (`dataloader_running.py` + `oom_kill_test.sh`, simulating a real
  OOM-killer) do **not** leak — PyTorch's own `os.getppid()` watchdog
  self-heals in well under a second either way. The leak only persists
  when a worker is frozen and genuinely cannot run that check
  (`stuck_worker_test.sh`), which `orphan-guard` still catches.
  `bench_dataloader.py` is the throughput benchmark used for overhead
  numbers (see `results/overhead_results.txt`).
- **`repro/ray/`** — a real Ray driver (`ray_driver.py`) that `ray.put()`s
  into the plasma object store; a plain `kill -9` on the driver orphans
  the `ray::IDLE` worker pool with no self-healing at all, and
  `orphan-guard` reclaims it cleanly.
- **`repro/celery/`** — the adversarial false-positive test: a real
  `redis-server` + Celery worker pool (concurrency=4) running in the
  *same* cgroup as a repeatedly-triggered leak. `repeatability_trial.sh`
  runs the leak-and-recover cycle N times while confirming Redis/Celery
  are never touched (15/15 clean in the last run, plus a functional
  post-check that Celery still processes real tasks afterward). A
  separate N=15 statistical run measuring time-to-reclaim is in
  [`results/repeatability_stats.txt`](results/repeatability_stats.txt)
  (100% success, mean 2.11s, stdev 0.31s — SIGTERM alone was sufficient
  in every trial, SIGKILL escalation was never needed in this run).

See [`docs/troubleshooting/`](docs/troubleshooting/) for the detailed, warts-and-all debugging
stories behind several of these (a nanoseconds-vs-clock-ticks unit bug
that silently disabled a safety check, a coverage gap between `/proc/*/fd`
and `/proc/*/maps`, and a test harness bug that made a *working* detector
look broken).

## Known limitations

Full writeup — kernel version/CO-RE floors, overhead edge cases, and which
Kubernetes environments this cannot run in at all (Fargate, gVisor/Kata,
restrictive Pod Security Admission) — in
[`docs/cfp/limitations.md`](docs/cfp/limitations.md). Summary:

- **Kernel-level observation alone can't infer intent.** A legitimately
  long-lived, intentionally-detached daemon and a leaked worker look
  identical from `sched_process_fork`/`exit` + `/dev/shm` references
  alone. Mitigated via `--exempt <pid>` (manual) and annotation-based
  exemption (`--namespace`/`--pod-name`, checked once at startup via the
  in-cluster API) — but the annotation check is one-shot at startup, not
  a continuous watch, so annotating a pod *after* the agent has already
  started for it has no effect until the agent restarts. A real
  production version would want a periodic re-check or a K8s watch
  instead of a one-time API call.
- **Children are tracked in a fixed-size circular buffer**, not a proper
  hash map — fine for a single scoped pod, would need revisiting for an
  unscoped, node-wide deployment.
- **The PID-namespace inconsistency we hit and abandoned** (in favor of
  cgroup scoping) was never fully root-caused. Worth revisiting.
- **Not yet packaged** as a container image, Helm chart, or systemd unit —
  currently a manually-built binary you run with an explicit `--cgroup`.
- **A 2-second minimum child age is a deliberate false-positive/detection-latency
  tradeoff.** A crash that happens to occur within 2 seconds of a worker's
  own birth will currently be missed. Every real leak we reproduced took
  seconds, not milliseconds, to matter — but this is a real, documented
  gap, not a free lunch.
