# "Why not just fix this in containerd/CRI when the pod goes down?" — the answer

This is the question a KubeCon reviewer (or any senior systems engineer) will
ask first. Answering it well requires being precise about what container
runtimes actually do at process-exit time, not hand-waving "eBPF is more
powerful." The precise answer below is the load-bearing argument of this
whole project — get this wrong and the rest of the talk doesn't matter.

## The question, stated as sharply as a reviewer will state it

> "When a container's main process dies, containerd/CRI already tears down
> the pod's cgroup and cleans up its resources. Just hook pod teardown and
> reclaim /dev/shm there. Why does this need to run inside the kernel via
> eBPF at all?"

## The answer has two independent parts. Both are necessary; neither alone is sufficient.

### Part 1: this leak usually does NOT involve the pod dying at all

The premise of the question is that the *pod* is going down, and cleanup
piggybacks on that teardown. But Phantom Memory's defining, worst
characteristic is that it happens **while the pod keeps running** — this
is exactly what makes it dangerous and hard to catch with existing
tooling.

- `/dev/shm` in Kubernetes is a pod-scoped `emptyDir` volume
  (`medium: Memory`). It is only reclaimed by the kubelet/runtime when the
  **pod's sandbox** is torn down — not when one process inside the
  container dies.
- Our own reproductions never killed the pod. In every case (the
  synthetic `zombie_maker.py`, the real PyTorch DataLoader test, the real
  Ray test), the container's actual PID 1 kept running the entire time.
  Only an internal process — a training script launched as a non-PID-1
  child, a Ray driver, a DataLoader worker's parent — died. The pod stayed
  `Running`, `Ready`, passing its liveness probe, for as long as anyone
  let it.
- This is precisely the scenario the original paper's reviewers flagged
  as under-explained, and precisely why standard monitoring misses it:
  process-tree tools see a "healthy" container the whole time. There is
  no pod-teardown event for containerd to hook, because the pod never
  tears down. That's the whole bug.

So "hook pod teardown" doesn't fire in the case that matters. You'd need
the runtime to notice a **sub-container-level** process death and decide
whether to act — which is a fundamentally different, harder question than
"clean up when the container exits," and leads directly into Part 2.

### Part 2: even if you *could* hook it, the runtime has no way to decide correctly

Suppose containerd did get a signal on every process exit inside a
container (it doesn't, by design — more on that below). It would face the
same problem we do: **a live, still-running child process, reparented to
PID 1, is not automatically a leak.**

The relevant kernel mechanism is `PR_SET_CHILD_SUBREAPER` (used by
`containerd-shim`, `tini`, `systemd`, and similar init-adjacent processes).
Per `prctl(2)`, a subreaper's job is to **reap zombies** — i.e., call
`wait()` on children that have already exited, collecting their exit
status and freeing their process-table entry. It is not a mechanism for
killing children that are still alive. We verified this distinction
directly in our own repro: the orphaned worker processes in every test
showed up in `ps` as fully alive and running (`S`/`R` state), not
`<defunct>` zombies — reparenting to PID 1 happened, but nothing about
that reparenting terminates or reclaims anything on its own. A subreaper
that only reaps zombies structurally cannot help here regardless of where
it runs.

So the real design question a containerd-level hook would have to answer
is: *given a live, orphaned process holding `/dev/shm`, should it be
killed?* And the honest answer is: **it depends on the application, and
the runtime has no way to know.** A process that legitimately
double-forks and detaches from its launcher on purpose (a real, common
Unix daemonization pattern — we built exactly this as an adversarial test
case, `legit-daemon-pod`, and it holds `/dev/shm` forever by design) looks
*identical*, from the container runtime's point of view, to a leaked
worker: live process, reparented to PID 1, holding shared memory. There is
no generic, safe, one-size-fits-all policy the runtime could bake in
without either (a) never killing anything, which solves nothing, or
(b) killing legitimate long-running daemons, which breaks real workloads.

## So why does this argue *for* eBPF specifically, rather than just "some userspace agent"?

Because the constraint isn't "must run in the kernel" for its own sake —
it's that the detection logic needs three properties simultaneously, and
eBPF is the mechanism that gets all three without modifying the runtime
or the workload:

1. **It has to observe events the container runtime's own architecture
   doesn't surface.** containerd/CRI's contract with Kubernetes is
   pod/container lifecycle (create, start, stop, remove) — it is
   deliberately not in the business of tracking every `fork()`/`exit()` of
   every process *inside* a container's own PID namespace; that's
   explicitly the workload's own business per the container abstraction.
   Building that visibility into containerd would mean growing a new,
   permanent, security-sensitive surface into every CRI implementation
   just for this one use case. eBPF tracepoints
   (`sched_process_fork`/`sched_process_exit`) already give this
   visibility for free, from outside the runtime, with no changes to
   containerd, no changes to the workload, and no new attack surface added
   to the container runtime itself.
2. **It has to apply a workload-specific policy, not a universal one.**
   Since Part 2 shows there's no safe universal rule, the decision needs
   to be configurable per workload (kill vs. alert-only, an allowlist for
   known-legitimate daemons, grace periods). That's an operator-facing
   policy layer, which belongs beside the cluster's own observability
   stack — not hardcoded into every container runtime's core teardown
   path, which would need to ship and version that same policy layer N
   times over (once per runtime implementation) instead of once.
3. **It has to act without needing the target process's cooperation.**
   We demonstrated this concretely: PyTorch's own `os.getppid()` watchdog
   is a legitimate userspace fix, and it works — right up until the
   process it's supposed to protect is blocked in native code or stuck
   and can never run that check. Our eBPF-based remediation doesn't care;
   `bpf_send_signal`/an external `SIGKILL` from a compiled, verified BPF
   program works regardless of whether the target is responsive. A
   containerd-level hook watching for the same condition would face
   exactly the same limits as any other userspace watcher unless it were
   *also* kernel-resident — which is just eBPF (or an equivalent
   in-kernel mechanism) by another name.

## The one-paragraph version, if you only get 30 seconds

"containerd hooks pod teardown; this leak's defining trait is that the pod
never tears down — the container stays healthy while an internal worker
gets silently orphaned. Even if you taught the runtime to watch every
internal process exit, subreaper semantics only reap already-dead
zombies, not kill live orphans — and deciding whether a live orphan is a
real leak or an intentional long-running daemon needs a policy call the
runtime has no basis for making generically. That policy has to live in a
separate, configurable layer that observes kernel-level events the
runtime doesn't expose, and act without depending on the target process's
own cooperation — which is exactly what eBPF gives you, and exactly what
baking it into every CRI implementation would not."

## What NOT to say (over-claims that will get punished on cross-examination)

- Don't say "the container runtime can't see process exits inside a
  container" — it *can*, in principle, via the same kernel it's already
  running on; the actual reason it doesn't build this in is architectural
  scope, not technical impossibility. Say "isn't architected to" or
  "deliberately doesn't," not "can't."
- Don't claim eBPF is the *only* possible implementation — a privileged
  sidecar polling `/proc` could theoretically approximate this (badly:
  polling interval blind spots, higher overhead, race conditions on PID
  reuse — several of which we hit and fixed during this project). The
  honest claim is that eBPF is the *right-fit* mechanism for the three
  properties above (in-kernel event visibility without runtime changes,
  policy configurability, cooperation-independence) — not that it's
  logically the sole possible one.
- Don't claim this is fully "production ready" — it's a working,
  empirically-validated research prototype with an honest Limitations
  section (kernel-observation alone can't infer intent; needs an
  allowlist for legitimate long-running orphans; not yet packaged as an
  installable artifact). Overclaiming maturity is a bigger credibility
  risk than the gap itself.
