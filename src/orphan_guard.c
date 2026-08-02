#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "orphan_guard.skel.h"

#define MAX_TRACKED   65536
#define MAX_CANDIDATES 4096
#define GRACE_WINDOW_SEC 2   /* time to wait after parent exit before first checking shm */
#define KILL_GRACE_SEC   3   /* time to wait after SIGTERM before escalating to SIGKILL */
#define MIN_CHILD_AGE_SEC 2  /* ignore a "parent exited" event for a child younger than this */

struct event {
    unsigned int type;
    unsigned int pid;
    unsigned int ppid;
    unsigned long long ts_ns;
    unsigned long long start_time;
    char comm[16];
};

struct child_rec { int pid; int ppid; unsigned long long start_time; time_t seen_at; };
static struct child_rec children[MAX_TRACKED];
static int n_children = 0;       /* number of valid entries, capped at MAX_TRACKED */
static int next_child_slot = 0;  /* ponytail: circular overwrite once full, no LRU/hash map yet */

struct candidate {
    int pid;
    unsigned long long start_time; /* recorded at add_candidate time, re-checked before acting */
    time_t deadline;      /* when to do the first shm check */
    int sigterm_sent;
    time_t kill_deadline;  /* when to escalate to SIGKILL if still alive+holding shm */
};
static struct candidate candidates[MAX_CANDIDATES];
static int n_candidates = 0;

/* Parse /proc/<pid>/stat field 22 (starttime, in clock ticks since boot).
 * This is the standard technique for detecting PID reuse: a pid whose
 * start time changed is a *different* process than the one we tracked,
 * even though the number is the same. Returns 0 if the process is gone. */
static unsigned long long get_start_time(int pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[1024];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    fclose(f);

    /* comm is the only field that can contain spaces/parens, so anchor on
     * the *last* ')' before splitting the remaining fields by space. */
    char *rparen = strrchr(line, ')');
    if (!rparen) return 0;

    /* p currently sits on the space right after ')' (i.e. just before
     * field 3 / state). Field 22 (starttime) starts 20 fields later. */
    char *p = rparen + 1;
    for (int i = 0; i < 20; i++) {
        p = strchr(p, ' ');
        if (!p) return 0;
        p++;
    }
    return strtoull(p, NULL, 10);
}

/* Safe by default: the agent only ever *logs* what it would do unless the
 * operator explicitly opts into destructive action with --enforce. This is
 * the same posture Kubernetes itself expects from anything with the power
 * to kill processes automatically -- a cluster admin should be able to run
 * this in a new environment for a while, read the audit log, and convince
 * themselves it's doing the right thing before it's ever allowed to act. */
static int dry_run = 1;
static int exempt_pids[256];
static int n_exempt = 0;

/* Structured, timestamped audit log. Every line the agent emits about a
 * candidate/action goes through here, in one consistent, greppable/
 * machine-parseable format: ISO8601 timestamp, a severity level, the pid
 * in question, a short action tag, whether this run is enforcing or just
 * observing, and a free-text detail for a human reading it live. */
static void audit_log(const char *level, int pid, const char *action, const char *detail) {
    time_t now = time(NULL);
    char ts[32];
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    printf("%s level=%s pid=%d action=%s dry_run=%s detail=\"%s\"\n",
           ts, level, pid, action, dry_run ? "true" : "false", detail);
}

static int is_exempt(int pid) {
    for (int i = 0; i < n_exempt; i++)
        if (exempt_pids[i] == pid) return 1;
    return 0;
}

static void track_child(int pid, int ppid, unsigned long long start_time) {
    /* ponytail: circular overwrite once MAX_TRACKED is reached instead of a
     * proper LRU/hash map. Fine for a single scoped pod; swap for a hash
     * map keyed by pid if this is ever run unscoped across a whole node. */
    children[next_child_slot].pid = pid;
    children[next_child_slot].ppid = ppid;
    children[next_child_slot].start_time = start_time;
    children[next_child_slot].seen_at = time(NULL);
    next_child_slot = (next_child_slot + 1) % MAX_TRACKED;
    if (n_children < MAX_TRACKED) n_children++;
}

/* Returns 1 if the process currently holds an open fd backed by /dev/shm,
 * 0 if not, -1 if the process no longer exists. */
static int holds_shm_fd(int pid) {
    char dirpath[64];
    snprintf(dirpath, sizeof(dirpath), "/proc/%d/fd", pid);
    DIR *d = opendir(dirpath);
    if (!d) return -1;

    int found = 0;
    struct dirent *ent;
    char linkpath[320], target[256];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(linkpath, sizeof(linkpath), "%s/%s", dirpath, ent->d_name);
        ssize_t n = readlink(linkpath, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = 0;
            if (strstr(target, "/dev/shm/") != NULL) {
                found = 1;
                break;
            }
        }
    }
    closedir(d);
    return found;
}

/* Returns 1 if the process currently holds a *memory mapping* backed by
 * /dev/shm, 0 if not, -1 if the process no longer exists.
 *
 * Necessary in addition to holds_shm_fd(): a common, entirely valid pattern
 * is mmap() the /dev/shm-backed file then close() the fd immediately --
 * the mapping alone keeps the pages alive, so nothing shows up under
 * /proc/<pid>/fd at all. Confirmed empirically on Ray: after killing the
 * driver, four orphaned `ray::IDLE` workers held tmpfs pages that `df -h
 * /dev/shm` reported as in-use, `ls /dev/shm` showed nothing (the backing
 * file was unlink()ed), and /proc/<pid>/fd had no /dev/shm entry either --
 * only /proc/<pid>/maps showed the mapping:
 *   rw-s 00000000 00:168 3   /dev/shm/plasmahELf7T (deleted)
 * holds_shm_fd() alone would have silently and permanently missed this. */
static int holds_shm_map(int pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int found = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "/dev/shm/") != NULL) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int holds_shm(int pid) {
    int viaFd = holds_shm_fd(pid);
    if (viaFd < 0) return -1; /* process gone */
    if (viaFd > 0) return 1;
    return holds_shm_map(pid) > 0;
}

/* True only if `pid` is alive AND is still the exact process generation we
 * recorded (guards against acting on an unrelated process that later
 * reused the same pid number during our multi-second grace window). */
static int same_process(int pid, unsigned long long recorded_start_time) {
    unsigned long long cur = get_start_time(pid);
    return cur != 0 && cur == recorded_start_time;
}

static void add_candidate(int pid, unsigned long long start_time) {
    if (is_exempt(pid)) {
        audit_log("INFO", pid, "SKIP_ALLOWLIST", "pid is on the allowlist, not a candidate");
        return;
    }
    if (n_candidates < MAX_CANDIDATES) {
        candidates[n_candidates].pid = pid;
        candidates[n_candidates].start_time = start_time;
        candidates[n_candidates].deadline = time(NULL) + GRACE_WINDOW_SEC;
        candidates[n_candidates].sigterm_sent = 0;
        candidates[n_candidates].kill_deadline = 0;
        n_candidates++;
        char detail[96];
        snprintf(detail, sizeof(detail), "orphaned, will check /dev/shm holding in %ds", GRACE_WINDOW_SEC);
        audit_log("INFO", pid, "WATCH", detail);
    }
}

static int handle_event(void *ctx, void *data, size_t len) {
    struct event *e = data;
    if (e->type == 1) {
        /* Note: we deliberately re-read the start time ourselves via
         * /proc rather than trust e->start_time. The kernel's
         * task_struct->start_time (what BPF reads) is nanoseconds since
         * boot; /proc/<pid>/stat field 22 (what we re-check against
         * later) is clock ticks since boot. Comparing the two directly
         * always mismatches -- caught this exact bug when every
         * candidate was silently dropped before ever reaching the shm
         * check. Using get_start_time() on both ends keeps units
         * consistent. */
        track_child((int)e->pid, (int)e->ppid, get_start_time((int)e->pid));
    } else if (e->type == 2) {
        time_t now = time(NULL);
        for (int i = 0; i < n_children; i++) {
            if (children[i].ppid != (int)e->pid) continue;

            /* Guard against a false positive we hit in practice: container
             * runtimes (confirmed with `kubectl exec` / runc) commonly
             * double-fork internally and have the intermediate process
             * exit right after the real target starts, as normal, healthy
             * process handoff -- not a crash. A child born a moment ago
             * whose "parent" immediately exits is far more likely to be
             * that handoff than a real orphan. A real leak in every
             * workload we tested (zombie_maker.py, PyTorch DataLoader,
             * Ray) involved a worker that had been running for seconds
             * before the parent died, not microseconds. */
            if (now - children[i].seen_at < MIN_CHILD_AGE_SEC) {
                char detail[96];
                snprintf(detail, sizeof(detail),
                         "only %lds old, likely process-launch handoff noise, not a real orphan",
                         (long)(now - children[i].seen_at));
                audit_log("INFO", children[i].pid, "SKIP_HANDOFF", detail);
                continue;
            }

            add_candidate(children[i].pid, children[i].start_time);
        }
    }
    return 0;
}

static void sweep_candidates(void) {
    time_t now = time(NULL);
    for (int i = 0; i < n_candidates; i++) {
        struct candidate *c = &candidates[i];
        if (c->pid == 0) continue;

        if (!same_process(c->pid, c->start_time)) {
            c->pid = 0; /* exited (or the pid was reused by someone else) -- either way, stop */
            continue;
        }

        if (!c->sigterm_sent) {
            if (now < c->deadline) continue;
            int shm = holds_shm(c->pid);
            if (shm <= 0) { c->pid = 0; continue; } /* not holding shm, not our problem */

            audit_log("ALERT", c->pid, dry_run ? "SIGTERM_WOULD_SEND" : "SIGTERM_SENT",
                       "orphaned worker holds /dev/shm");
            if (!dry_run) kill(c->pid, SIGTERM);
            c->sigterm_sent = 1;
            c->kill_deadline = now + KILL_GRACE_SEC;
        } else {
            if (now < c->kill_deadline) continue;
            if (same_process(c->pid, c->start_time) && holds_shm(c->pid) > 0) {
                audit_log("ACTION", c->pid, dry_run ? "SIGKILL_WOULD_SEND" : "SIGKILL_SENT",
                           "grace period expired, still alive and holding /dev/shm");
                if (!dry_run) kill(c->pid, SIGKILL);
            } else {
                audit_log("INFO", c->pid, "RESOLVED",
                           "exited or released /dev/shm during grace period");
            }
            c->pid = 0;
        }
    }
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --cgroup <inode> [--enforce] [--exempt <pid> ...]\n\n"
        "  --cgroup <inode>   Scope detection to one container's cgroup (from\n"
        "                      `stat -c %%i /sys/fs/cgroup/.../cri-containerd-<id>.scope`\n"
        "                      on the host). Strongly recommended -- omitting this\n"
        "                      watches the whole node.\n"
        "  --enforce           Actually send SIGTERM/SIGKILL to confirmed orphans.\n"
        "                      Without this flag the agent only logs what it would\n"
        "                      do (dry-run is the default, not opt-in).\n"
        "  --dry-run           Explicit no-op; dry-run is already the default.\n"
        "  --exempt <pid>      Never act on this pid (repeatable). A manual\n"
        "                      allowlist; see the README for label/annotation-based\n"
        "                      exemption, which does not exist yet.\n"
        "  --help              Show this message.\n",
        prog);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0); /* line-buffer stdout even when redirected to a file/pipe */
    unsigned long long target_cgroup = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--dry-run") == 0) dry_run = 1;  /* explicit no-op: this is the default */
        else if (strcmp(argv[i], "--enforce") == 0) dry_run = 0;
        else if (strcmp(argv[i], "--exempt") == 0 && i + 1 < argc) {
            if (n_exempt < 256) exempt_pids[n_exempt++] = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cgroup") == 0 && i + 1 < argc) {
            target_cgroup = strtoull(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "unknown argument: %s\n\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (target_cgroup == 0) {
        fprintf(stderr, "warning: no --cgroup given, running UNSCOPED (whole node). "
                         "Use --cgroup <inode> (from `stat -c %%i "
                         "/sys/fs/cgroup/.../cri-containerd-<id>.scope` on the host) "
                         "to scope to one container.\n");
    }

    struct orphan_guard_bpf *skel = orphan_guard_bpf__open_and_load();
    if (!skel) {
        fprintf(stderr, "failed to open/load BPF skeleton\n");
        return 1;
    }

    if (target_cgroup != 0) {
        __u32 key = 0;
        int fd = bpf_map__fd(skel->maps.target_cgroup);
        if (bpf_map_update_elem(fd, &key, &target_cgroup, BPF_ANY)) {
            fprintf(stderr, "failed to set target_cgroup map\n");
            return 1;
        }
    }

    if (orphan_guard_bpf__attach(skel)) {
        fprintf(stderr, "failed to attach BPF programs\n");
        return 1;
    }

    struct ring_buffer *rb = ring_buffer__new(bpf_map__fd(skel->maps.events), handle_event, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "failed to create ring buffer\n");
        return 1;
    }

    audit_log("INFO", 0, "STARTUP",
               dry_run ? "running in dry-run mode: will log detections only, no signals sent (pass --enforce to arm)"
                       : "running in ENFORCE mode: will send SIGTERM/SIGKILL to confirmed orphans");
    fflush(stdout);
    while (1) {
        int err = ring_buffer__poll(rb, 500);
        if (err < 0 && err != -EINTR) {
            fprintf(stderr, "ring_buffer__poll error: %d\n", err);
            break;
        }
        sweep_candidates();
    }

    ring_buffer__free(rb);
    orphan_guard_bpf__destroy(skel);
    return 0;
}
