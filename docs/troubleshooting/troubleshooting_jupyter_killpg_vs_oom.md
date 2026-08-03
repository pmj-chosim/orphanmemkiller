# Case Report: Jupyter E2E — killpg() Masking vs. the Parent-Age Detection Gap

**Date:** 2026-08-03
**Scope:** Live validation on `aks-zombietensor-test` (AKS), real `jupyter_client.KernelManager` kernel + real `persistent_workers=True` PyTorch DataLoader.

## TL;DR

Two independent, stacked pitfalls in one demo scenario — a framework-level
self-defense we didn't account for, and a detector heuristic keyed on the
wrong process. Both found and fixed via live testing, not inference.

## Pitfall 1 — `restart_kernel()` isn't a valid OOM stand-in

`jupyter_client` starts every kernel as its own session/process-group
leader. `KernelManager.restart_kernel(now=True)` — the same call the
notebook UI's "Restart Kernel" button triggers server-side — sends its
kill via `killpg()` on that group, not a single `SIGKILL` to one pid.

**Consequence:** a frozen (`SIGSTOP`'d) DataLoader worker sharing the
kernel's process group dies *directly* from the `killpg()`, before
orphan-guard ever gets a chance to see an orphan. The demo would silently
prove nothing. This is not a detector bug — it's a wrong test design. A
real OOM-killer targets one `task_struct`, never a process group.

**Fix:** demo/test now calls `os.kill(kernel_pid, signal.SIGKILL)` on the
kernel pid only. See `repro/jupyter/jupyter_worker_freeze_test.py`.

## Pitfall 2 — the age guard was gated on the wrong process

With the SIGKILL-to-kernel-pid fix in place, the frozen worker was
*still* missed on first re-run.

```
SKIP_HANDOFF pid=<frozen-worker> "only 1s old, likely process-launch
handoff noise, not a real orphan"
```

**Root cause:** the existing anti-false-positive guard (added earlier for
a `runc exec` double-fork handoff false positive — see
`troubleshooting_exec_handoff_false_positive.txt`) checked *the child's*
age against a 2s floor. `persistent_workers=True` DataLoader workers are
forked once, right before use — so a worker can legitimately be <2s old
even when its parent (the kernel) has been alive for much longer before
crashing. Child age and "is this handoff noise" are not the same
question; the guard was answering the wrong one.

**Fix:** gate on the age of the *exiting parent* instead
(`MIN_PARENT_AGE_SEC`, `src/orphan_guard.c`) — a `runc` handoff shim is
young at its own death (milliseconds old); a real crashing parent
(Jupyter kernel, DataLoader main process) is not, regardless of how young
its children are.

## Verification (live, `aks-zombietensor-test`, this session)

```
13:19:52  WATCH         x4 (4 DataLoader workers, kernel just SIGKILL'd)
13:19:54  SIGTERM_SENT  x4
13:19:57  SIGKILL_SENT   1  (the frozen worker — SIGTERM can't reach a stopped process)
13:19:57  RESOLVED       3  (self-healed via PyTorch's own os.getppid() check)
```

Frozen worker reclaimed in ~5s from kernel death; 3 healthy workers
self-healed on their own, as expected; zero false positives on unrelated
`kubectl exec` handoff noise in the same log.

## Takeaway

When a live test breaks a detector, check for *two* possible culprits
before touching detector code: (1) is the test harness itself hiding the
real failure mode behind some *other* self-defense mechanism, and only
then (2) is the detector's own heuristic wrong. Fixing pitfall 2 without
first catching pitfall 1 would have produced a "working" demo that never
actually exercised orphan-guard at all.
