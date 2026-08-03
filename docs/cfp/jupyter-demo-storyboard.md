# Live Demo Storyboard: "The Phantom Memory Catch"

Goal: ~8-10 minutes on stage, visually dramatic, technically honest (every
beat below reuses a mechanism we've already tested and proven working —
this is a presentation wrapper around `repro/pytorch/stuck_worker_test.sh`,
not a new untested trick).

## Screen layout (before the talk starts)

Split into 3 panes, all visible to the audience simultaneously:

```
+-------------------------------+-------------------------------+
|                               |  Pane B: process tree          |
|  Pane A: Jupyter Notebook     |  watch -n0.5 'ps -eo            |
|  (the "victim" — a normal-    |   pid,ppid,stat,args'           |
|  looking training notebook)   |                                 |
|                               +-------------------------------+
|                               |  Pane C: orphan-guard audit log|
|                               |  tail -f daemon.log             |
|                               |  + df -h /dev/shm (refreshed)  |
+-------------------------------+-------------------------------+
```

Pane A is what the audience looks at first — it needs to look completely
ordinary. Panes B/C are the "reveal" — kept visible the whole time so the
audience can watch cause and effect land in real time, not just take your
word for it afterward.

## Pre-show setup (do this before walking on stage, not live)

1. Jupyter server running inside a pod with `/dev/shm` mounted
   (`emptyDir: {medium: Memory}`), matching `repro/pytorch/pytorch-pod.yaml`.
2. `orphan-guard` already built and running in `--enforce` mode, scoped to
   this pod's cgroup, logging to a file tailed in Pane C. **Do not start
   it live on stage** — cgroup-id lookup + daemon startup is unglamorous
   dead air; get it running beforehand and just point at the log.
3. Have `repro/pytorch/dataloader_running.py`'s logic already loaded as
   the first few cells of the notebook (DataLoader, `num_workers=4`,
   `persistent_workers=True`, an infinite batch-fetch loop that prints
   progress) so Pane A's output already shows a believable, boring
   "training in progress" scroll before you're on stage.
4. Rehearse the full sequence at least twice end to end with a stopwatch.
   The whole payoff hinges on ~2 real seconds of dead air between "kernel
   restart" and "reclaimed" — if that pause is unrehearsed it reads as a
   glitch, not a feature. Know what you're saying during that 2 seconds.

## Beat-by-beat script

**Beat 1 — establish normalcy (30s)**
> "This is an ordinary PyTorch training notebook. DataLoader, four
> workers, nothing unusual." *(gesture at Pane A's scrolling batch
> output; glance at Pane C, `/dev/shm` climbing to a steady, boring
> number)*

**Beat 2 — name the real failure mode (30s)**
> "PyTorch DataLoader workers hanging on a slow read, a stuck decode, a
> flaky network mount — this is one of the most commonly reported issues
> in PyTorch's own tracker, for almost a decade." *(optional: have
> `docs/cfp/evidence-citations.md`'s PyTorch issue list ready as a backup
> slide, don't dwell on it live)* "Let's make that happen on purpose."

**Beat 3 — freeze a worker (30-45s)**
In a new notebook cell, find one DataLoader worker pid (a direct child of
the kernel process) and `SIGSTOP` it — the same technique validated in
`repro/pytorch/stuck_worker_test.sh`:
```python
import os, signal, subprocess
kernel_pid = os.getpid()
workers = subprocess.check_output(
    f"ps --ppid {kernel_pid} -o pid=", shell=True
).split()
victim = int(workers[0])
os.kill(victim, signal.SIGSTOP)
print(f"worker {victim} frozen — it can no longer run its own os.getppid() self-check")
```
> "That worker is now completely stuck. It cannot run one more line of
> Python — including the one line of Python that would have saved it."

**Beat 4 — the crash (the "everyone in the audience has done this" moment, 15s)**
Do **not** click Kernel → Restart Kernel — confirmed in testing that it
calls `killpg()` on the kernel's whole process group, which would kill the
frozen worker directly and skip orphan-guard entirely. Instead, stay in
Pane A and run this in a new notebook cell:
```python
import os, signal
os.kill(os.getpid(), signal.SIGKILL)
```
> "In dev, everyone clicks 'Restart Kernel'. But in production, the Linux
> OOM-killer isn't that graceful. It doesn't kill the whole process
> group — it just snipes the heaviest process with SIGKILL. Like this."

**Beat 5 — show the damage (20s)**
Point at Pane B: the frozen worker (state `T`) is now `ppid=1`, still
holding memory. Point at Pane C's `df -h /dev/shm`: still non-zero.
> "The pod is still `Running`. Nothing paged anyone. And this process
> cannot even run the self-check that would normally save it — it's
> frozen."

**Beat 6 — the catch (the payoff, ~5-10s of real dead air)**
Just... wait, pointing at Pane C. Let the audience watch the log lines
land in real time:
```
... level=INFO   action=WATCH         detail="orphaned, will check /dev/shm holding in 2s"
... level=ALERT  action=SIGTERM_SENT  detail="orphaned worker holds /dev/shm"
... level=ACTION action=SIGKILL_SENT  detail="grace period expired, still alive and holding /dev/shm"
```
Then `df -h /dev/shm` ticks back to baseline in Pane C, and the frozen
process disappears from Pane B.
> "SIGTERM didn't work — it's frozen, it can't respond to anything but
> SIGKILL. The agent knows that, waits out the grace period exactly like
> Kubernetes itself waits before force-killing a pod, and reclaims it.
> About two seconds, start to finish. No pod restart. No page. No one
> touched a terminal."

**Beat 7 — the honest coda (15s)**
> "This isn't a trick specific to this demo — the exact same escalation
> caught a real orphaned Ray worker pool with zero setup tricks, and in
> 15 back-to-back trials with a real Redis and Celery worker pool running
> in the same scope the whole time, it never once touched either of
> them."

## What to have ready as a fallback

- **A screen recording of the exact sequence above**, captured during
  rehearsal, in case live conference wifi/AV/cluster access fails. This
  is not optional — build it the same day you rehearse, from a real run,
  not a mockup.
- **A dry-run pass ready to demo instead** if the venue's policy is
  nervous about a live "agent that kills processes" — same storyboard,
  same log lines, just `SIGTERM_WOULD_SEND`/`SIGKILL_WOULD_SEND` instead
  of the enforcing versions. Slightly less dramatic (the frozen worker
  stays frozen on screen) but zero destructive risk if something about
  the venue's demo environment is unfamiliar.

## Build status

This storyboard is written against mechanisms we've already tested end to
end on the real cluster: a real `jupyter_client.KernelManager`-launched
kernel running a real PyTorch DataLoader (`persistent_workers=True`,
`num_workers=4`), one worker frozen with `SIGSTOP`, then a direct
`SIGKILL` to the kernel pid — reproducing Beat 4 exactly (not the UI
button, which was tried and ruled out: it calls `killpg()` and kills the
frozen worker directly, so orphan-guard never gets a chance to catch it).
First attempt at this exact scenario missed the frozen worker (root cause:
the daemon's own anti-false-positive age guard was keyed off the child's
age instead of the exiting parent's — fixed in `src/orphan_guard.c`, see
git history). Re-run after the fix: both healthy self-healing and the
frozen-worker catch (`WATCH` → `SIGTERM_SENT` → `SIGKILL_SENT`) confirmed
working. Remaining: rehearsing the *visual* Jupyter-notebook-UI version
(this was validated via the underlying `jupyter_client` API, not by typing
into an actual browser-rendered notebook) — do that pass before the
venue-specific rehearsal.
