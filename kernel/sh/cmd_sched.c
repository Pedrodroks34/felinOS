#include "sh/cmds.h"
#include "sched.h"
#include "user.h"
#include "console.h"
#include "drivers/input.h"
#include "drivers/pit.h"
#include "lib/string.h"

static int is_number(const char *s) {
    if (*s == '-' || *s == '+') {
        s++;
    }
    if (!*s) {
        return 0;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
    }
    return 1;
}

int cmd_ps(int argc, char **argv, struct stream *in, struct stream *out) {
    struct task_info info[SCHED_MAX_TASKS];
    int n = sched_snapshot(info, SCHED_MAX_TASKS);
    int self = sched_current()->pid;

    st_printf(out, "%5s %5s %-9s %3s %9s %6s %-6s %s\n",
              "PID", "PPID", "STATE", "NI", "TIME", "%CPU", "WAIT", "NAME");
    for (int i = 0; i < n; i++) {
        struct task_info *t = &info[i];
        uint32_t tenths = t->lifetime_ticks ? (t->cpu_ticks * 1000u) / t->lifetime_ticks : 0;
        const char *state = (t->pid == self) ? "running" : sched_state_name(t->state);
        if (t->state == TASK_RUNNING && t->pid != self) {
            state = "ready";
        }
        st_printf(out, "%5d %5d %-9s %3d %5u.%02u %3u.%u %-6s %s%s\n",
                  t->pid, t->ppid, state, t->nice,
                  t->cpu_ticks / PIT_FREQUENCY,
                  (t->cpu_ticks % PIT_FREQUENCY) * 100u / PIT_FREQUENCY,
                  tenths / 10u, tenths % 10u,
                  t->wait_reason ? t->wait_reason : "-",
                  t->name, t->user ? "" : " [kernel]");
    }
    return 0;
}

int cmd_kill(int argc, char **argv, struct stream *in, struct stream *out) {
    int sig = SCHED_SIGTERM;
    int first = 1;
    int status = 0;

    if (argc > 1 && argv[1][0] == '-' && is_number(argv[1] + 1)) {
        sig = atoi(argv[1] + 1);
        first = 2;
    }
    if (first >= argc) {
        cmd_error(out, "kill", NULL, "usage: kill [-signal] <pid...>");
        return 1;
    }
    if (sig < 0 || sig > 31) {
        cmd_error(out, "kill", argv[1], "invalid signal");
        return 1;
    }
    for (int i = first; i < argc; i++) {
        if (!is_number(argv[i])) {
            cmd_error(out, "kill", argv[i], "invalid pid");
            status = 1;
            continue;
        }
        if (task_kill(atoi(argv[i]), sig) != 0) {
            cmd_error(out, "kill", argv[i], "no such user process");
            status = 1;
        }
    }
    return status;
}

int cmd_renice(int argc, char **argv, struct stream *in, struct stream *out) {
    int status = 0;

    if (argc < 3 || !is_number(argv[1])) {
        cmd_error(out, "renice", NULL, "usage: renice <nice> <pid...>");
        return 1;
    }
    int nice = atoi(argv[1]);
    if (nice < SCHED_MIN_NICE || nice > SCHED_MAX_NICE) {
        st_printf(out, "renice: nice must be between %d and %d\n", SCHED_MIN_NICE, SCHED_MAX_NICE);
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        if (!is_number(argv[i]) || task_set_nice(atoi(argv[i]), nice) != 0) {
            cmd_error(out, "renice", argv[i], "no such process");
            status = 1;
        }
    }
    return status;
}

int cmd_sched(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc >= 2) {
        if (strcmp(argv[1], "quantum") != 0 || argc != 3 || !is_number(argv[2]) || atoi(argv[2]) < 1) {
            cmd_error(out, "sched", NULL, "usage: sched [quantum <ticks>]");
            return 1;
        }
        sched_set_quantum((uint32_t)atoi(argv[2]));
    }

    struct sched_stats s;
    sched_get_stats(&s);
    uint32_t ms = s.quantum * (1000u / PIT_FREQUENCY);
    uint32_t idle_tenths = s.total_ticks ? (s.idle_ticks * 1000u) / s.total_ticks : 0;

    st_printf(out, "policy:            preemptive round-robin, time slice weighted by nice\n");
    st_printf(out, "timer:             %u Hz, one tick every %u ms\n", (uint32_t)PIT_FREQUENCY, 1000u / PIT_FREQUENCY);
    st_printf(out, "default quantum:   %u ticks (%u ms), nice %d..%d\n", s.quantum, ms,
              SCHED_MIN_NICE, SCHED_MAX_NICE);
    st_printf(out, "tasks:             %u (%u waiting in the run queue)\n", s.tasks, s.ready);
    st_printf(out, "context switches:  %u (%u by timer preemption)\n", s.switches, s.preemptions);
    st_printf(out, "cpu time:          %u ticks total, %u.%u%% idle\n", s.total_ticks,
              idle_tenths / 10u, idle_tenths % 10u);
    return 0;
}

/* Previous sample of each task's CPU time, so %CPU can be a rate rather than
 * the cumulative share ps(1) shows. Indexed by slot in the snapshot, which is
 * stable within a boot: a task keeps its entry until it is reaped. */
struct top_prev {
    int pid;
    uint32_t ticks;
    int valid;
};

static void top_bar(char *buf, int size, int percent) {
    const int width = 10;
    int filled = (percent * width) / 100;
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;
    int n = 0;
    for (int i = 0; i < width && n < size - 2; i++) {
        buf[n++] = (i < filled) ? '#' : '.';
    }
    buf[n] = '\0';
}

static void top_draw(struct stream *out, struct top_prev *prev, int have_prev) {
    struct task_info info[SCHED_MAX_TASKS];
    struct sched_stats stats;
    int n = sched_snapshot(info, SCHED_MAX_TASKS);
    uint32_t now = pit_ticks();
    int self = sched_current()->pid;

    sched_get_stats(&stats);

    uint32_t secs = pit_uptime_seconds();
    st_printf(out, "\nGato top -- up %u:%02u:%02u, %u task(s), %u in the run queue\n",
              secs / 3600u, (secs % 3600u) / 60u, secs % 60u, stats.tasks, stats.ready);

    st_printf(out, "%5s %5s %-9s %3s %6s %9s %-11s %s\n",
              "PID", "PPID", "STATE", "NI", "%CPU", "TIME", "CPU", "NAME");

    for (int i = 0; i < n; i++) {
        struct task_info *t = &info[i];

        /* Look the task up in the previous sample to turn the tick delta into
         * a percentage of elapsed wall time. */
        uint32_t elapsed = 0;
        int percent = 0;
        for (int j = 0; j < n; j++) {
            if (!prev[j].valid || prev[j].pid != t->pid) {
                continue;
            }
            elapsed = now - prev[j].ticks;
            uint32_t used = t->cpu_ticks - prev[j].ticks;
            percent = elapsed ? (int)((used * 100u) / elapsed) : 0;
            break;
        }
        if (!have_prev) {
            /* First frame has no baseline, so fall back to the lifetime share,
             * the same number ps(1) prints. */
            percent = t->lifetime_ticks ? (int)((t->cpu_ticks * 100u) / t->lifetime_ticks) : 0;
        }

        char bar[16];
        top_bar(bar, sizeof(bar), percent);

        const char *state = (t->pid == self) ? "running" : sched_state_name(t->state);
        if (t->state == TASK_RUNNING && t->pid != self) {
            state = "ready";
        }

        st_printf(out, "%5d %5d %-9s %3d %6d %5u.%02u %-11s %s\n",
                  t->pid, t->ppid, state, t->nice, percent,
                  t->cpu_ticks / PIT_FREQUENCY,
                  (t->cpu_ticks % PIT_FREQUENCY) * 100u / PIT_FREQUENCY,
                  bar, t->name);
    }

    uint32_t idle_pct = stats.total_ticks ? (stats.idle_ticks * 100u) / stats.total_ticks : 0;
    st_printf(out, "\n%u%% idle, %u context switches (%u preempted), quantum %u ticks\n",
              idle_pct, stats.switches, stats.preemptions, stats.quantum);

    /* Record this frame as the baseline for the next one. */
    for (int i = 0; i < n; i++) {
        prev[i].pid = info[i].pid;
        prev[i].ticks = info[i].cpu_ticks;
        prev[i].valid = 1;
    }
}

int cmd_top(int argc, char **argv, struct stream *in, struct stream *out) {
    (void)in;
    int iterations = 0;     /* 0 = forever, until a key is pressed */

    if (argc >= 2) {
        if (!is_number(argv[1]) || atoi(argv[1]) < 1) {
            cmd_error(out, "top", argv[1], "iteration count must be a positive number");
            return 1;
        }
        iterations = atoi(argv[1]);
    }

    struct top_prev prev[SCHED_MAX_TASKS];
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        prev[i].valid = 0;
    }

    /* Anything already typed was meant for the shell, not for quitting top. */
    input_flush();

    int frame = 0;
    for (;;) {
        top_draw(out, prev, frame > 0);
        frame++;

        if (iterations && frame >= iterations) {
            break;
        }
        /* Sleep in slices so a keypress is noticed promptly instead of after
         * the whole delay elapses. */
        for (int waited = 0; waited < 20; waited++) {
            if (input_poll() >= 0) {
                /* input_poll() already took the key; asking for another one
                 * would block the shell until something else was typed. The
                 * rest of whatever line that key belonged to goes too, the
                 * same as top/q/less do when they are dismissed mid-line. */
                input_flush();
                return 0;
            }
            sleep_ms(100);
        }
    }
    return 0;
}

int cmd_fg(int argc, char **argv, struct stream *in, struct stream *out) {
    struct task_info info[SCHED_MAX_TASKS];
    int me = sched_current()->pid;
    int pid = -1;

    if (argc >= 2) {
        if (!is_number(argv[1])) {
            cmd_error(out, "fg", argv[1], "invalid pid");
            return 1;
        }
        pid = atoi(argv[1]);
    } else {
        int n = sched_snapshot(info, SCHED_MAX_TASKS);
        for (int i = 0; i < n; i++) {
            if (info[i].ppid == me && info[i].user && info[i].state != TASK_ZOMBIE) {
                pid = info[i].pid;
            }
        }
    }
    struct task *t = pid > 0 ? task_find(pid) : NULL;
    if (!t || t->ppid != me || !t->user) {
        cmd_error(out, "fg", argc >= 2 ? argv[1] : NULL, "no such background job");
        return 1;
    }
    st_printf(out, "%s\n", t->name);
    return user_wait_foreground(pid);
}
