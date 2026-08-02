#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

char LICENSE[] SEC("license") = "GPL";

#define EVENT_FORK 1
#define EVENT_EXIT 2

struct event {
    __u32 type;
    __u32 pid;
    __u32 ppid;
    __u64 ts_ns;
    __u64 start_time; /* task->start_time of `pid`, for PID-reuse detection */
    char comm[16];
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} events SEC(".maps");

/* Single-entry config map: the target cgroup id (the cri-containerd
 * cgroupfs directory's inode number, e.g. via `stat -c %i
 * /sys/fs/cgroup/.../cri-containerd-<id>.scope`). We only act on
 * fork/exit inside this cgroup.
 *
 * We tried PID-namespace scoping first (nsproxy->pid_ns_for_children) and
 * abandoned it: empirically, /proc/<pid>/ns/pid inode numbers read back
 * inconsistent values even for a plain fork() parent/child pair on this
 * cluster (kubectl exec + setns(CLONE_NEWPID) interaction we did not fully
 * root-cause -- see Limitations). Also, reading nsproxy from the *exiting*
 * task in sched_process_exit is separately unreliable since
 * exit_task_namespaces() clears task->nsproxy earlier in do_exit(), before
 * this tracepoint fires (also confirmed empirically). Cgroup id via
 * bpf_get_current_cgroup_id() is the standard, well-supported mechanism
 * container-aware eBPF tools (Cilium, Falco) use for exactly this kind of
 * scoping, and does not require walking task_struct fields at all. */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} target_cgroup SEC(".maps");

static __always_inline int in_target_cgroup(void)
{
    __u32 key = 0;
    __u64 *target = bpf_map_lookup_elem(&target_cgroup, &key);
    if (!target || *target == 0)
        return 1; /* unconfigured: fall back to unscoped (explicit opt-in) */

    return bpf_get_current_cgroup_id() == *target;
}

/* We deliberately do NOT walk task->children at exit time: reparenting
 * (forget_original_parent) timing relative to the sched_process_exit
 * tracepoint is not guaranteed across kernel versions, so the child list
 * could already be empty by the time this fires. Instead we track
 * parent/child relationships ourselves from fork events and reconcile
 * against exit events in userspace. */

SEC("tp_btf/sched_process_fork")
int BPF_PROG(on_fork, struct task_struct *parent, struct task_struct *child)
{
    if (!in_target_cgroup())
        return 0;

    /* Skip new-thread events (CLONE_THREAD): a real new process has
     * pid == tgid (it is its own thread-group leader). A newly created
     * thread's pid (tid) differs from its tgid (the already-existing
     * process it joined). Without this, numpy/OpenBLAS-style internal
     * worker threads show up as bogus self-referential "pid==ppid" fork
     * events. */
    if (BPF_CORE_READ(child, pid) != BPF_CORE_READ(child, tgid))
        return 0;

    struct event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return 0;

    e->type = EVENT_FORK;
    e->pid = BPF_CORE_READ(child, tgid);
    e->ppid = BPF_CORE_READ(parent, tgid);
    e->ts_ns = bpf_ktime_get_ns();
    e->start_time = BPF_CORE_READ(child, start_time);
    BPF_CORE_READ_STR_INTO(&e->comm, child, comm);

    bpf_ringbuf_submit(e, 0);
    return 0;
}

SEC("tp_btf/sched_process_exit")
int BPF_PROG(on_exit, struct task_struct *task)
{
    if (!in_target_cgroup())
        return 0;

    __u32 pid = BPF_CORE_READ(task, tgid);
    __u32 tid = BPF_CORE_READ(task, pid);

    /* Only report whole-process exit (thread-group leader), not every
     * thread's exit, so we don't fan out orphan checks per-thread. */
    if (pid != tid)
        return 0;

    struct event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return 0;

    e->type = EVENT_EXIT;
    e->pid = pid;
    e->ppid = BPF_CORE_READ(task, real_parent, tgid);
    e->ts_ns = bpf_ktime_get_ns();
    e->start_time = BPF_CORE_READ(task, start_time);
    BPF_CORE_READ_STR_INTO(&e->comm, task, comm);

    bpf_ringbuf_submit(e, 0);
    return 0;
}
