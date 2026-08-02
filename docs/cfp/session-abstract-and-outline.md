# Session Abstract & Outline (refined draft)
Audience: AI infra admins / MLOps engineers (practitioner track, not pure academic)

---

## Title

**Phantom Memory: The /dev/shm Leak Your Framework's Own Watchdog Can't Fix**

## Abstract (submission-ready, ~350 words)

Ray's own documentation says it plainly: *"SIGKILL kills processes
without Ray noticing it... Ray has difficulty raising a proper error
message and taking proper actions for fault tolerance."* That's not a bug
report — it's the maintainers of one of the most widely used distributed
AI frameworks admitting, in their own docs, that their userspace defense
against OOM-kills has a hole big enough to drive a leak through.

We built and tested an eBPF-based agent that catches exactly this class
of failure — orphaned AI worker processes that keep holding `/dev/shm`
after their parent dies, invisible to standard monitoring because the
*pod itself never goes down*. We didn't stop at one synthetic demo. We
tested it against real `torch.utils.data.DataLoader` workers and a real
Ray driver, with real background services (Redis, a Celery worker pool)
running in the *same* cgroup as every test, to prove it doesn't touch
anything it shouldn't.

The results surprised us. PyTorch already defends itself — a built-in
`os.getppid()` check reaps orphaned workers in under a second, regardless
of whether the parent died from an exception, `os._exit()`, or an
external SIGKILL simulating a real OOM-kill. That defense has exactly one
hole: a worker that's genuinely stuck — blocked I/O, deep in a native
extension call, which PyTorch's own multi-year-recurring hang reports
show is not rare — can never run that check. We froze a worker to
simulate exactly that, live, and our agent reclaimed it correctly where
the library-level watchdog structurally could not. Ray, true to its own
documentation, has no equivalent self-healing at all — one `kill -9` on
the driver leaves its object-store workers orphaned indefinitely.

We'll walk through the real engineering path that got us here — cgroup
scoping instead of PID-namespace tricks that turned out to be unreliable
on real clusters, catching shared memory held via `mmap()` after the file
descriptor is already closed (which our first version silently missed),
and a false-positive bug where our own safety net briefly killed our own
benchmark. Live demo included: a Jupyter notebook, a stuck DataLoader
worker, a kernel restart, and a kernel-level safety net that closes the
gap in about two seconds.

## Three takeaways for the audience (what you do differently at work Monday)

1. **`/dev/shm` is a monitoring blind spot you can audit today.** It's a
   pod-scoped volume that only gets reclaimed on full pod teardown — not
   when an internal training/serving process dies. If your ML pods mount
   `emptyDir: {medium: Memory}` for tensor IPC (PyTorch, Ray, and most
   multiprocessing-heavy Python ML code do), check whether your existing
   monitoring would actually catch this pattern — ours didn't, until we
   built something that watches kernel events directly instead of
   process-tree health.
2. **Don't trust a framework's built-in self-healing as your only line of
   defense — know exactly where its blind spot is.** PyTorch's own
   watchdog is real and it works, most of the time; Ray's own docs
   admit theirs doesn't cover SIGKILL/OOM at all. Either way, the defense
   only runs if the worker gets to execute code — audit whether your
   specific workload has long blocking I/O or native-extension calls
   where that assumption breaks.
3. **A kernel-level safety net is deployable today without touching your
   training code or waiting on upstream framework fixes** — and it should
   ship dry-run-by-default with a structured audit log, not "on" by
   default. We'll show the exact configuration flag pattern (`--enforce`
   required to arm it, full audit trail either way) that made this
   something we'd actually trust rolling out gradually in a real cluster.

## Session outline (assume a 35-40 min practitioner slot)

| Time | Segment |
|---|---|
| 0:00–2:00 | Cold open: the Ray docs quote on screen. "This is Ray's own documentation. Let's talk about why." |
| 2:00–8:00 | The problem: Phantom Memory — what it is, why it's invisible to standard monitoring (the pod never dies), the external evidence (PyTorch's multi-year hang reports, `/dev/shm` exhaustion issues, Ray's own SIGKILL admission) |
| 8:00–13:00 | Why not just fix this in containerd/CRI? The subreaper argument (zombies vs. live orphans), and why "is this a leak or an intentional daemon" is a policy question the runtime can't answer generically (the `legit-daemon-pod` adversarial test) |
| 13:00–23:00 | **Live demo** (see `docs/cfp/jupyter-demo-storyboard.md`): Jupyter notebook, PyTorch DataLoader, a worker gets stuck, kernel restart, eBPF agent catches it in ~2 seconds, `/dev/shm` and process tree shown side by side with the audit log |
| 23:00–28:00 | How it actually works: CO-RE, cgroup scoping (and the PID-namespace approach that turned out not to work), catching `mmap`-held memory (the Ray bug we found and fixed), SIGTERM-first/grace/SIGKILL escalation, dry-run-by-default |
| 28:00–32:00 | Results: PyTorch vs. Ray comparison, 15/15 repeatability with zero false positives against real Redis/Celery in the same cgroup, overhead numbers (no measurable DataLoader throughput impact) |
| 32:00–35:00 | Honest limitations (kernel/CO-RE floor, where this can't run at all — Fargate, gVisor/Kata, restrictive Pod Security Admission), the 3 takeaways, where to find the code |
| 35:00–40:00 | Q&A |

## Open items before submitting

- [ ] Confirm actual word-limit/field structure for the target CFP form (OSS Japan Sessionize, or whichever venue this goes to first) and trim the abstract to fit
- [ ] Decide whether the live demo travels well for a given venue's AV setup, or needs a pre-recorded backup (see the demo storyboard doc for a recording plan)
- [ ] Read the "Taming the Memory Beast" arXiv paper in full before citing it (see `evidence-citations.md` — scope not yet independently verified)
