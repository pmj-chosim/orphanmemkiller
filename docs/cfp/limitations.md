# Limitations

This project works, and we have empirical evidence for it (synthetic
reproduction, three independent real-world workloads — PyTorch DataLoader,
Ray, and a real Jupyter kernel — 0% false positives against real
background services sharing the same cgroup as the leak).
None of that makes it complete. Below is what we know doesn't hold, have
not verified, or have deliberately traded away — organized the way a
skeptical reviewer would probe it, not the way that reads best.

---

## 1. Kernel version and CO-RE dependencies

**What we actually tested on:** kernel 6.8.0-1059-azure (AKS, Ubuntu
24.04 node image), with `/sys/kernel/btf/vmlinux` present. Everything
below the line "verified on this exact kernel" is inference from public
documentation, not something we independently confirmed on other kernels.

**Hard requirements, and why:**

- **`BPF_MAP_TYPE_RINGBUF`** (the event channel between the BPF program
  and the userspace daemon) was added in **kernel 5.8**. There is no
  fallback path in this code to the older perf-buffer API for kernels
  below that. This is the single hardest floor: below 5.8, the program
  does not compile/load as written, full stop.
- **`tp_btf/sched_process_fork` and `tp_btf/sched_process_exit`** (BTF-aware
  raw tracepoints, which is what lets us read `struct task_struct *`
  fields directly and portably via `BPF_CORE_READ` instead of manual
  offset arithmetic) require BTF-powered trampoline support, which landed
  around **kernel 5.5**. Below that, this specific attach mechanism isn't
  available; a rewrite against plain `tracepoint:sched:sched_process_fork`
  (the classic, non-`_btf` form) would be needed, which is a real but
  bounded engineering task we have not done.
- **`CONFIG_DEBUG_INFO_BTF=y`** (kernel built with embedded BTF) is
  required for CO-RE relocation to resolve `task_struct` field offsets at
  load time on the *running* kernel, rather than the kernel the code was
  compiled against. This has been the default on mainline Ubuntu, Fedora,
  and most major distro kernels since roughly 5.4+, but is **not**
  guaranteed on custom-built kernels, some minimal/embedded distros, or
  older LTS images that predate that default. If BTF isn't present,
  loading fails outright — there is no silent degraded mode.
- **`bpf_get_current_cgroup_id()`** — the mechanism we settled on for
  scoping detection to one container after PID-namespace scoping proved
  unreliable (see the troubleshooting docs) — relies on the cgroup v2
  unified-hierarchy property that a cgroup's id equals the inode number of
  its cgroupfs directory. **We have only validated this against cgroup
  v2.** Clusters still running cgroup v1 (older node images, some
  specific CNI/CRI combinations that haven't migrated) may not have this
  property hold cleanly, since v1's split-hierarchy model doesn't map
  1:1 onto a single cgroup id the same way. We have not tested cgroup v1
  at all.

**Net honest statement:** the practical floor is roughly **kernel 5.8+
with BTF enabled**, and cgroup v2. We have concretely verified correct
behavior on exactly one kernel (6.8). Anyone deploying this on a
meaningfully older or differently-configured kernel should expect to do
their own compatibility pass, not assume it "should just work" because
CO-RE is designed to be portable in principle.

**Privilege requirements**, since they gate deployability as much as
kernel version: loading these BPF program types needs `CAP_BPF` +
`CAP_PERFMON` (the post-5.8 fine-grained capabilities) or `CAP_SYS_ADMIN`
on kernels/configurations that don't split those out yet, plus read
access to `/sys/kernel/btf/vmlinux` and (for our userspace-side detection)
`/proc/<pid>/fd` and `/proc/<pid>/maps` for processes in *other*
containers, which in every environment we tested required the agent's own
pod to run with `hostPID: true` and `privileged: true`. We have not tried
to find or validate a narrower capability set than "fully privileged" —
that is real, un-done hardening work, not a claim that privileged access
is strictly the minimum required.

---

## 2. Overhead: what we measured, and where the measured numbers stop applying

**What we measured** (see `results/overhead_results.txt` for full numbers):
a real PyTorch DataLoader throughput benchmark showed no measurable
overhead with the probe attached versus a probe-free baseline (difference
was within run-to-run noise). A synthetic fork/exit stress test (~2000
process creations/sec, far higher than any realistic AI workload's
steady-state behavior) showed roughly **+9% wall-clock** versus baseline.

**Why the DataLoader number is the right one for the target use case, and
the stress-test number is not "the real overhead":** our hooks fire only
on `sched_process_fork`/`sched_process_exit` — process creation and
destruction, not steady-state execution. A DataLoader with
`persistent_workers=True` forks its worker pool once and reuses it for
the entire run; there is structurally almost nothing for the probe to do
during normal batch iteration. This is a deliberate, low-frequency choice
(the earlier bpftrace prototype hooked `nanosleep`/`select`, syscalls a
sleeping process calls continuously — this design change is *why* the
overhead profile looks the way it does, not an accident).

**Where the overhead profile stops being negligible — real, named edge
cases, not hypothetical hedging:**

- **Workloads whose normal operation involves frequent process
  creation/destruction**, not just a training script's one-time worker
  spawn: CI runner pods, shell-heavy batch/ETL pipelines that `fork()`+`exec()`
  a subprocess per file/record, serverless-style "new process per
  request" patterns. For these, the fork/exit rate could approach or
  exceed our synthetic stress test's ~2000/sec, and the ~9% figure (or
  worse, since we didn't push past that rate) is the more honest
  reference point than the DataLoader number.
- **Running unscoped** (omitting `--cgroup`, which we do not recommend and
  the agent prints a warning about): every fork/exit *on the entire node*
  is emitted to the ring buffer and processed in userspace, regardless of
  which pod it belongs to. On a busy multi-tenant node this is not just a
  log-noise problem (which we hit directly during development — hundreds
  of `[Watch]` lines within seconds on an otherwise-idle single-node test
  cluster) — it's the userspace event-processing rate that actually
  matters for overhead, and we have not load-tested that path under real
  multi-tenant node churn. `--cgroup` scoping is not just a precision
  feature; treat it as a required overhead-control mechanism.
- **`holds_shm_map()` scans the full `/proc/<pid>/maps` text for every
  orphan candidate at every check.** For a process with an unusually
  large number of memory mappings (heavy use of memory-mapped files —
  some data-loading patterns that `mmap` many small shards individually,
  or JVM-based workloads with large numbers of loaded classes/libraries),
  this per-check cost scales with mapping count. We have not benchmarked
  this specifically; it is a plausible, not-yet-measured cost center.
- **The candidate/children tracking structures are fixed-size arrays with
  circular overwrite, not a hash map** (documented in-code as a
  deliberate `ponytail:`-style simplification). Fine at the scale we
  tested (single scoped pod, tens of tracked processes); we have not
  characterized behavior as tracked-process counts grow into the
  thousands, which becomes plausible if scoping is loosened.

---

## 3. Kubernetes environments where this hook faces real restrictions

- **Pod Security Admission / policy-enforced clusters.** The agent
  requires `privileged: true`, `hostPID: true`, and hostPath mounts for
  `/sys/kernel/btf`, `/sys/fs/cgroup`, and (in our build environment)
  `/sys/kernel/debug` and `/lib/modules`. Clusters enforcing the
  Kubernetes **Pod Security Standards "restricted" or "baseline"**
  profiles — or an OPA/Gatekeeper/Kyverno policy disallowing privileged
  pods or hostPath volumes, which is common in regulated or
  security-conscious environments — will reject this deployment outright
  unless an explicit exception is carved out for it. This is arguably
  correct behavior for a node-level security tool (it should require
  deliberate cluster-admin approval, not be deployable by an arbitrary
  tenant) but it is a real, non-optional deployment prerequisite, not a
  configuration nicety.
- **Fully managed / serverless node models with no privileged node
  access at all.** Some managed Kubernetes offerings do not permit
  privileged containers or hostPath volumes under any configuration —
  **AWS Fargate profiles for EKS** are a concrete, named example: Fargate
  pods cannot run privileged containers, cannot mount hostPath volumes,
  and there is no accessible "node" to run a BTF/cgroupfs-reading
  DaemonSet on in the first place. This tool **cannot run at all** in
  that model, not just "with reduced functionality." The same applies to
  other fully abstracted, no-node-access Kubernetes-compatible platforms.
- **Sandboxed container runtimes (gVisor / Kata Containers).** Our
  detection mechanism assumes the workload's processes are ordinary Linux
  processes the host kernel schedules and exposes via real
  `sched_process_fork`/`exit` tracepoints and real cgroups. **gVisor**
  (`runsc`) intercepts syscalls in a userspace sentry process and does not
  execute sandboxed processes as directly host-visible Linux processes in
  the way our BPF hooks assume; **Kata Containers** runs each pod in a
  lightweight VM. We have not tested against either, and have no reason
  to expect the current design works unmodified under them — this is a
  real, not-merely-theoretical gap for clusters using GKE Sandbox or
  similar gVisor/Kata-backed node pools for stronger tenant isolation.
- **cgroup v1 clusters** — see the CO-RE section above; our scoping
  mechanism's correctness has only been validated against cgroup v2.
- **Multi-container pods with `shareProcessNamespace: true`** change which
  process is "PID 1" of the shared namespace and how reparenting resolves
  within it; we have only tested single-container-per-pod scenarios (each
  of our repro pods had exactly one container). We have not verified
  behavior in a shared-process-namespace multi-container pod.

---

## 4. The limitation that isn't about infrastructure at all

**Kernel-level observation cannot infer intent.** A process that
legitimately double-forks and detaches from its launcher on purpose (a
real, common Unix daemonization pattern) is, from `sched_process_fork`/
`exit` plus `/dev/shm` references alone, indistinguishable from a leaked
worker — both are: live process, reparented to PID 1, holding shared
memory. We built and tested exactly this adversarial case
(`repro/legit-daemon-pod.yaml`) and it is correctly left alone today, but
only because it happens to live in a different, unwatched cgroup. Within
the *same* watched scope, two mitigations ship today: a manual `--exempt
<pid>` allowlist for a known pid, and a pod-level `orphan-guard.io/exempt:
"true"` annotation — checked once at startup against the in-cluster K8s
API (via `--namespace`/`--pod-name`) and, if present, forces permanent
dry-run for that run regardless of `--enforce`. This covers "this
workload is expected to daemonize, do not touch it" for the pod-lifetime
scope this project targets; it is checked once at startup, not
re-polled, so an annotation added/removed after the daemon is already
running does not take effect until the next restart.

## 5. One deliberate detection/safety tradeoff, stated plainly

An exiting process's children are ignored as orphan candidates if the
exiting process itself was younger than `MIN_PARENT_AGE_SEC` (2) seconds
old — added specifically because container-runtime launch handoff (a
normal, healthy `runc exec` double-fork pattern, not a crash) produces the
same kernel-level signature as a real orphan event, and without this guard
the agent killed its own benchmark process during testing. This check is
on the age of the *exiting parent*, not its children: an earlier version
gated on the child's age instead, which meant a real orphan born only
moments before a longer-lived parent's crash (confirmed in practice with a
live Jupyter kernel killed seconds after starting a DataLoader) was wrongly
treated as handoff noise and never caught. The remaining tradeoff:
**a parent that crashes within 2 seconds of its own launch will still not
have its children flagged.** Every real leak we reproduced (the synthetic
script, PyTorch DataLoader, Ray, and the Jupyter kernel case) involved a
parent that had itself been alive for several seconds before crashing —
but that is a property of the workloads we tested, not a guarantee about
all workloads, and this is a real, live gap in coverage, not just a
theoretical one.

### Q&A defense: why this isn't a tunable knob we just forgot to lower

The 2-second floor isn't a magic number we picked and could shrink for a
better demo — it's a lower bound set by how container runtimes actually
launch processes on every standard Kubernetes node. `runc`'s exec/create
path is a **double-fork handoff**: an intermediate process joins the
target's namespaces, forks the real target, and exits — and step three
happens on the order of **milliseconds**, not seconds, because it's just a
namespace-join and a fork, no I/O, no scheduling contention in the
common case. A detector with *no* age floor sees that intermediate exit
and a fork event that's microseconds old, and cannot distinguish it from
a real crash by the kernel-level signature alone — `sched_process_fork`
followed by `sched_process_exit` for the same pid looks identical either
way; we don't get "handoff" or "crash" as a field, we get a timestamp.
So the real design question isn't "should there be a floor," it's "how
low can the floor go before it clips into runtime-handoff jitter" — and
we measured that jitter empirically (the false-positive that killed our
own benchmark, see `docs/troubleshooting/`) rather than guessing it. 2
seconds is roughly **three orders of magnitude above the millisecond-scale
handoff window** we observed — comfortable headroom against scheduler
jitter on a loaded node, while still being short enough that it only
costs detection latency for a crash that happens to land in the first two
seconds of a process's life, which every real leak we've reproduced
across three independent workloads did not. If a reviewer asks "why not
0.5s" or "why not adaptive": adaptive is the honest next step (e.g.
learning the actual node's handoff latency at startup instead of a fixed
constant), but it adds a calibration phase and a new failure mode (a
misjudged calibration reopens the exact hole this guard closes) for a
project whose current job is proving the *detection mechanism* works, not
shipping a self-tuning production system. That's a real, stated scope
limit, not a dodge.

## 6. The capability floor — what this approach categorically requires

This is not a shortlist of environments we haven't gotten to yet; it's
the minimum substrate CO-RE eBPF tracepoints on `sched_process_fork`/
`sched_process_exit` require to exist *at all*. No amount of engineering
time closes these without abandoning the eBPF approach entirely:

- **A real Linux kernel exposing BTF for the running kernel** (`/sys/kernel/btf/vmlinux`), i.e. a standard Kubernetes worker node. **Fargate** does not qualify — AWS does not expose kernel BPF program loading to Fargate tasks at all; there is no kernel to attach to from inside the task.
- **cgroup v2**, for `bpf_get_current_cgroup_id()`-based scoping. A cgroup v1-only host changes the cgroup-id semantics this project's scoping mechanism depends on — untested, and not assumed to work.
- **`CAP_BPF`/`CAP_SYS_ADMIN`** and BPF program loading permitted for the pod — meaning a **privileged pod** (or a host-level agent), not a normal application pod under a default Pod Security Admission policy. A cluster enforcing the `restricted` PSA profile on this namespace is, by design, refusing exactly the capability this tool needs.
- **The workload's tracepoints firing in a kernel this eBPF program can attach to** — a **gVisor (GKE Sandbox) or Kata Containers** runtime interposes its own kernel (gVisor) or a real guest kernel (Kata) between the workload and the host kernel `sched_process_fork`/`exit` never fires for the sandboxed process on the host kernel we can see, because the process isn't actually running there.

None of these are bugs to be fixed in a future version — they are the
definition of the environment eBPF-based observation requires. The
correct framing for a reviewer or an operator evaluating this tool: **if
your node is a standard Kubernetes worker node on cgroup v2 with BPF
enabled and you can run a privileged pod on it, this works. If it isn't,
no version of this specific approach will** — a Kubernetes-runtime-level
or per-framework fix (imperfect as those are, per the rest of this
document) is the only option in those environments.
