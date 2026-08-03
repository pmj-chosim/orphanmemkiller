# External Evidence: This Is a Real, Widely-Documented Problem

Compiled 2026-08-02 to back the CFP/paper with citations beyond our own
repro. Organized by what each source actually proves — don't overclaim
past what's quoted.

## 1. The single strongest citation: Ray's own docs admit the exact gap we found

From Ray's official documentation, ["Debugging Memory Issues"](https://docs.ray.io/en/latest/ray-observability/user-guides/debug-apps/debug-memory.html):

> "One of the common problems of the Linux out-of-memory killer is that
> SIGKILL kills processes without Ray noticing it. Since SIGKILL cannot be
> handled by processes, Ray has difficulty raising a proper error message
> and taking proper actions for fault tolerance."

> "If the Linux out-of-memory killer terminates Tasks or Actors, Ray
> Worker processes are unable to catch and display an exact root cause
> because SIGKILL cannot be handled by processes."

Ray's own stated mitigation (since Ray 2.2): an **application-level memory
monitor that tries to kill workers *before* the OS OOM-killer does** — a
userspace watchdog racing the kernel's own OOM killer. This is Ray's
official documentation conceding, in its own words, the identical
structural weakness our project addresses: a userspace defense that only
works if it gets to run before something faster and unkillable (SIGKILL)
happens first. This directly corroborates our own empirical finding
(Task #6: a plain `kill -9` on a Ray driver leaves `ray::IDLE` workers
orphaned indefinitely, no self-healing) — and it comes from Ray itself,
not from us.

**Use this as the primary citation for "why isn't a per-framework fix
enough" in the talk.**

## 2. PyTorch DataLoader worker hangs/deadlocks: a real, recurring, *unsolved* problem across nearly a decade

Not "shared memory leak" reports specifically — these are reports of
workers getting **stuck**, which is the exact precondition for our
"frozen worker" gap (Task #5's SIGSTOP test is a clean, deterministic
stand-in for this real, messier phenomenon):

- [pytorch/pytorch#1579](https://github.com/pytorch/pytorch/issues/1579) — DataLoader hangs with `num_workers > 0`
- [pytorch/pytorch#1595](https://github.com/pytorch/pytorch/issues/1595) — DataLoader workers deadlocked
- [pytorch/pytorch#15808](https://github.com/pytorch/pytorch/issues/15808) — DataLoader freezes randomly with `num_workers > 0`
- [pytorch/pytorch#75147](https://github.com/pytorch/pytorch/issues/75147) — potential deadlock, DataLoader gets stuck iterating
- [pytorch/pytorch#130610](https://github.com/pytorch/pytorch/issues/130610) — DataLoader hangs with fork + `pin_memory=True` (2024 — this is not a stale, long-fixed issue)
- [pytorch/pytorch#51344](https://github.com/pytorch/pytorch/issues/51344) — DataLoader freezes in a **Jupyter notebook** specifically — directly supports the notebook demo framing

Span: issue numbers from the 1000s (early PyTorch) through 130000s
(2024) — this is not a problem that got fixed years ago; new reports of
the same underlying class of issue keep appearing.

## 3. PyTorch `/dev/shm` exhaustion tied to DataLoader workers: also recurring, also multi-year

- [pytorch/pytorch#14768](https://github.com/pytorch/pytorch/issues/14768) — "unable to open shared memory object ... in read-write mode"
- [pytorch/pytorch#108861](https://github.com/pytorch/pytorch/issues/108861) — worker killed by signal: Bus error, "possible that dataloader's workers are out of shared memory"
- [pytorch/pytorch#5040](https://github.com/pytorch/pytorch/issues/5040) — request for a clearer error instead of a raw "Bus error" when shm runs out
- [pytorch/pytorch#28820](https://github.com/pytorch/pytorch/issues/28820), [#5301](https://github.com/pytorch/pytorch/issues/5301) — "DataLoader worker exited unexpectedly"
- [flairNLP/flair#1559](https://github.com/flairNLP/flair/issues/1559) — "Embedding Dataloader's workers are out of shared memory" (a downstream library hitting the same root issue)

## 4. The Python core bug tracker: shared-memory lifecycle management is fragile at the language level, not just in any one framework

- [python/cpython#89372](https://github.com/python/cpython/issues/89372) / [bpo-45209](https://bugs.python.org/issue45209) — `resource_tracker` warns about leaked `shared_memory` objects at shutdown (the exact warning message we saw in every one of our own repro logs)
- [bpo-38119](https://bugs.python.org/issue38119) — a related, opposite-direction bug: `resource_tracker` can destroy a shared memory segment via `shm_unlink()` as soon as *any* process with a handle exits, even if other processes still need it. Precise wording from the report: *"The resource tracker currently destroys (via `_posixshmem.shm_unlink`) shared memory segments on posix systems when any independently created Python process with a handle on a shared memory segment exits (gracefully or otherwise)."*

**Use carefully**: bpo-38119 is not a "leak" report, it's the opposite
failure mode (over-eager cleanup destroying memory too early). Cite it
as evidence that shared-memory lifecycle management across process
boundaries in Python's multiprocessing is a longstanding, actively
fragile area generally — not as a second leak citation. Don't conflate
the two directions of the bug in the talk.

## 5. Operational/production evidence: `/dev/shm` counts against container memory limits, and it surprises people in production

Blog-level evidence (weaker than the above, but useful for the "this
actually bites real operators" framing): a documented case of a pod using
`/dev/shm` for IPC with a 1Gi `sizeLimit`, where writing large files to
`/dev/shm` triggered `OOMKilled` even though heap usage looked completely
normal — matching our own point that `/dev/shm` usage is invisible to
standard heap/RSS-based monitoring.

**Checked and rejected as a citation**: the arXiv paper (Dec 2024, 2412.14701),
*"Taming the Memory Beast: Strategies for Reliable ML Training on
Kubernetes"* — read in full. It covers Kubernetes-level resource
management only (requests/limits, QoS classes, pod eviction, ephemeral
storage, OOM troubleshooting at the *scheduler/orchestration* level). It
does not discuss orphaned processes, PID 1 reparenting, `/dev/shm`,
multiprocessing workers, SIGKILL, or the OOM-killer's interaction with
child processes anywhere in the text. Not on-topic for this project —
do not cite it.

---

## How to use this evidence honestly in the talk/paper

- The Ray docs quote is the load-bearing citation — official,
  authoritative, directly on-topic, and it's the vendor admitting the gap
  in their own words. Lead with it.
- The PyTorch issue list proves **frequency and persistence** (multi-year,
  still recurring), which is exactly what the original eBPF'26 reviewers
  asked for — not a frequency *rate* (we don't have production telemetry
  for that, and shouldn't claim one), but frequency *evidence*: this
  class of problem keeps getting independently reported by different
  users over nearly a decade, across multiple frameworks.
- Don't claim any of these GitHub issues are literally "the same bug" as
  Phantom Memory — most of them are about hangs or plain shm exhaustion,
  not specifically about orphan-after-parent-death. The honest claim is
  narrower and still strong: *the preconditions for our failure mode
  (workers that hang/block, and `/dev/shm` exhaustion tied to worker
  lifecycle) are independently, repeatedly, and currently being reported
  in exactly the frameworks we tested.*
