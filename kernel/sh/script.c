#include "sh/cmds.h"
#include "sh/shell.h"
#include "sched.h"
#include "user.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/format.h"

#define SCRIPT_MAX_LINES 512
#define SCRIPT_LINE_LEN  SHELL_LINE_MAX
#define SCRIPT_MAX_WORDS 64
#define SCRIPT_LOOP_GUARD 1000000

struct script_lines {
    char *text[SCRIPT_MAX_LINES];
    int count;
};

struct script_state {
    struct script_lines *sl;
    int idx;
};

static void free_lines(struct script_lines *sl) {
    for (int i = 0; i < sl->count; i++) {
        kfree(sl->text[i]);
    }
}

static int load_lines(const char *path, struct script_lines *sl) {
    void *buf;
    uint32_t size;

    sl->count = 0;
    if (vfs_load(path, &buf, &size) < 0) {
        return -1;
    }
    struct stream *s = stream_new();
    st_write(s, (const char *)buf, size);
    kfree(buf);

    char line[SCRIPT_LINE_LEN];
    while (sl->count < SCRIPT_MAX_LINES && st_getline(s, line, sizeof(line)) > 0) {
        char *trimmed = line;

        while (*trimmed == ' ' || *trimmed == '\t') {
            trimmed++;
        }
        char *hashpos = NULL;
        int q = 0;
        for (char *p = trimmed; *p; p++) {
            if (*p == '\'' || *p == '"') {
                q = q ? 0 : *p;
            } else if (*p == '#' && !q && (p == trimmed || p[-1] == ' ' || p[-1] == '\t')) {
                hashpos = p;
                break;
            }
        }
        if (hashpos) {
            *hashpos = 0;
        }
        size_t len = strlen(trimmed);
        while (len && (trimmed[len - 1] == ' ' || trimmed[len - 1] == '\t')) {
            trimmed[--len] = 0;
        }
        sl->text[sl->count++] = strdup(trimmed);
    }
    stream_free(s);
    return 0;
}

static int line_is(const char *line, const char *kw) {
    size_t klen = strlen(kw);

    if (strncmp(line, kw, klen) != 0) {
        return 0;
    }
    char c = line[klen];
    return c == 0 || c == ' ' || c == '\t';
}

static int run_stmt(struct script_state *st);
static int run_block_until(struct script_state *st, const char **terms, int nterm, int *matched);
static void skip_stmt(struct script_state *st);
static void skip_block_until(struct script_state *st, const char **terms, int nterm, int *matched);

static int run_block_until(struct script_state *st, const char **terms, int nterm, int *matched) {
    int status = 0;

    while (st->idx < st->sl->count) {
        const char *line = st->sl->text[st->idx];

        if (!line[0]) {
            st->idx++;
            continue;
        }
        for (int i = 0; i < nterm; i++) {
            if (strcmp(line, terms[i]) == 0) {
                if (matched) {
                    *matched = i;
                }
                return status;
            }
        }
        status = run_stmt(st);
    }
    if (matched) {
        *matched = -1;
    }
    return status;
}

static void skip_block_until(struct script_state *st, const char **terms, int nterm, int *matched) {
    while (st->idx < st->sl->count) {
        const char *line = st->sl->text[st->idx];

        if (!line[0]) {
            st->idx++;
            continue;
        }
        for (int i = 0; i < nterm; i++) {
            if (strcmp(line, terms[i]) == 0) {
                if (matched) {
                    *matched = i;
                }
                return;
            }
        }
        skip_stmt(st);
    }
    if (matched) {
        *matched = -1;
    }
}

static void skip_stmt(struct script_state *st) {
    const char *line = st->sl->text[st->idx];
    const char *fi_only[1] = { "fi" };
    const char *done_only[1] = { "done" };
    const char *else_fi[2] = { "else", "fi" };
    int matched;

    if (line_is(line, "if")) {
        st->idx++;
        if (st->idx < st->sl->count && strcmp(st->sl->text[st->idx], "then") == 0) {
            st->idx++;
        }
        skip_block_until(st, else_fi, 2, &matched);
        if (matched == 0) {
            st->idx++;
            skip_block_until(st, fi_only, 1, &matched);
        }
        if (st->idx < st->sl->count && strcmp(st->sl->text[st->idx], "fi") == 0) {
            st->idx++;
        }
        return;
    }
    if (line_is(line, "for") || line_is(line, "while")) {
        st->idx++;
        if (st->idx < st->sl->count && strcmp(st->sl->text[st->idx], "do") == 0) {
            st->idx++;
        }
        skip_block_until(st, done_only, 1, &matched);
        if (st->idx < st->sl->count && strcmp(st->sl->text[st->idx], "done") == 0) {
            st->idx++;
        }
        return;
    }
    st->idx++;
}

static int do_if(struct script_state *st) {
    char cond[SCRIPT_LINE_LEN];

    strlcpy(cond, st->sl->text[st->idx] + 2, sizeof(cond));
    st->idx++;

    if (st->idx >= st->sl->count || strcmp(st->sl->text[st->idx], "then") != 0) {
        return 1;
    }
    st->idx++;

    int cond_status = shell_run_line(cond);
    const char *else_fi[2] = { "else", "fi" };
    const char *fi_only[1] = { "fi" };
    int matched = -1;
    int status = 0;

    if (cond_status == 0) {
        status = run_block_until(st, else_fi, 2, &matched);
        if (matched == 0) {
            st->idx++;
            skip_block_until(st, fi_only, 1, &matched);
        }
    } else {
        skip_block_until(st, else_fi, 2, &matched);
        if (matched == 0) {
            st->idx++;
            status = run_block_until(st, fi_only, 1, &matched);
        }
    }
    if (st->idx < st->sl->count && strcmp(st->sl->text[st->idx], "fi") == 0) {
        st->idx++;
    }
    return status;
}

static int do_for(struct script_state *st) {
    char rest[SCRIPT_LINE_LEN];

    strlcpy(rest, st->sl->text[st->idx] + 4, sizeof(rest));
    st->idx++;

    char *p = rest;
    while (*p == ' ') {
        p++;
    }
    char *var = p;
    while (*p && *p != ' ') {
        p++;
    }
    if (*p) {
        *p++ = 0;
    }
    while (*p == ' ') {
        p++;
    }

    int valid = strncmp(p, "in", 2) == 0 && (p[2] == ' ' || p[2] == 0);
    if (valid) {
        p += 2;
        while (*p == ' ') {
            p++;
        }
    }

    char *words[SCRIPT_MAX_WORDS];
    int nwords = 0;

    if (valid) {
        char *wp = p;
        while (*wp && nwords < SCRIPT_MAX_WORDS) {
            while (*wp == ' ') {
                wp++;
            }
            if (!*wp) {
                break;
            }
            words[nwords++] = wp;
            while (*wp && *wp != ' ') {
                wp++;
            }
            if (*wp) {
                *wp++ = 0;
            }
        }
    }

    const char *done_only[1] = { "done" };
    int matched;

    if (st->idx >= st->sl->count || strcmp(st->sl->text[st->idx], "do") != 0) {
        return 1;
    }
    st->idx++;
    int body_start = st->idx;

    if (!valid) {
        skip_block_until(st, done_only, 1, &matched);
        if (st->idx < st->sl->count && strcmp(st->sl->text[st->idx], "done") == 0) {
            st->idx++;
        }
        return 1;
    }

    int status = 0;
    if (nwords == 0) {
        st->idx = body_start;
        skip_block_until(st, done_only, 1, &matched);
    } else {
        for (int i = 0; i < nwords; i++) {
            shell_setenv(var, words[i]);
            st->idx = body_start;
            status = run_block_until(st, done_only, 1, &matched);
        }
    }
    if (st->idx < st->sl->count && strcmp(st->sl->text[st->idx], "done") == 0) {
        st->idx++;
    }
    return status;
}

static int do_while(struct script_state *st) {
    char cond[SCRIPT_LINE_LEN];

    strlcpy(cond, st->sl->text[st->idx] + 5, sizeof(cond));
    st->idx++;

    if (st->idx >= st->sl->count || strcmp(st->sl->text[st->idx], "do") != 0) {
        return 1;
    }
    st->idx++;
    int body_start = st->idx;
    const char *done_only[1] = { "done" };
    int matched;
    int status = 0;
    int guard = 0;

    for (;;) {
        sched_yield();
        if (guard++ > SCRIPT_LOOP_GUARD) {
            break;
        }
        if (shell_run_line(cond) != 0) {
            st->idx = body_start;
            skip_block_until(st, done_only, 1, &matched);
            break;
        }
        st->idx = body_start;
        status = run_block_until(st, done_only, 1, &matched);
    }
    if (st->idx < st->sl->count && strcmp(st->sl->text[st->idx], "done") == 0) {
        st->idx++;
    }
    return status;
}

static int run_stmt(struct script_state *st) {
    const char *line = st->sl->text[st->idx];

    if (line_is(line, "if")) {
        return do_if(st);
    }
    if (line_is(line, "for")) {
        return do_for(st);
    }
    if (line_is(line, "while")) {
        return do_while(st);
    }
    char buf[SCRIPT_LINE_LEN];
    strlcpy(buf, line, sizeof(buf));
    st->idx++;
    return shell_run_line(buf);
}

int script_execute_lines(struct script_lines *sl, int argc, char **argv) {
    struct shell_params params;

    params.argc = argc > 10 ? 10 : argc;
    for (int i = 0; i < params.argc; i++) {
        params.argv[i] = argv[i];
    }
    const struct shell_params *old = shell_set_params(&params);

    struct script_state st;
    st.sl = sl;
    st.idx = 0;

    int status = 0;
    while (st.idx < sl->count) {
        status = run_stmt(&st);
    }
    shell_set_params(old);
    return status;
}

int script_run_file(const char *path, int argc, char **argv) {
    struct script_lines sl;

    if (load_lines(path, &sl) < 0) {
        return -1;
    }
    int status = script_execute_lines(&sl, argc, argv);

    free_lines(&sl);
    return status;
}

struct script_ctx {
    char path[VFS_PATH_MAX];
    int argc;
    char *argv[10];
};

static void script_task_entry(void *arg) {
    struct script_ctx *ctx = (struct script_ctx *)arg;
    char saved_cwd[VFS_PATH_MAX];

    strlcpy(saved_cwd, vfs_getcwd(), sizeof(saved_cwd));
    int status = script_run_file(ctx->path, ctx->argc, ctx->argv);
    vfs_chdir(saved_cwd);

    for (int i = 0; i < ctx->argc; i++) {
        kfree(ctx->argv[i]);
    }
    kfree(ctx);
    task_exit(status);
}

int script_run_command(const char *path, int argc, char **argv, int background) {
    struct script_ctx *ctx = (struct script_ctx *)kmalloc(sizeof(struct script_ctx));

    if (!ctx) {
        return 1;
    }
    strlcpy(ctx->path, path, sizeof(ctx->path));
    ctx->argc = argc > 10 ? 10 : argc;
    for (int i = 0; i < ctx->argc; i++) {
        ctx->argv[i] = strdup(argv[i]);
    }
    struct task *t = task_new("script", script_task_entry, ctx, 16384, vmm_kernel_space(), 1);

    if (!t) {
        for (int i = 0; i < ctx->argc; i++) {
            kfree(ctx->argv[i]);
        }
        kfree(ctx);
        return 1;
    }
    task_start(t);
    if (background) {
        st_printf(stream_console(), "[%d] %s\n", t->pid, t->name);
        return 0;
    }
    return user_wait_foreground(t->pid);
}

int script_resolve_path(const char *name, char *out, size_t size) {
    char path[VFS_PATH_MAX];

    if (strchr(name, '/')) {
        if (vfs_normalize(name, path, sizeof(path)) < 0) {
            return 0;
        }
    } else {
        if (strlen(name) > 200) {
            return 0;
        }
        snprintf(path, sizeof(path), "/bin/%s", name);
    }
    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0 || st.type != VFS_FILE) {
        return 0;
    }
    strlcpy(out, path, size);
    return 1;
}

int cmd_source(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        cmd_error(out, "source", NULL, "usage: source <path> [args...]");
        return 1;
    }
    char path[VFS_PATH_MAX];

    if (strchr(argv[1], '/')) {
        if (vfs_normalize(argv[1], path, sizeof(path)) < 0) {
            cmd_error(out, "source", argv[1], "no such file");
            return 1;
        }
    } else {
        strlcpy(path, argv[1], sizeof(path));
    }
    int status = script_run_file(path, argc - 1, argv + 1);

    if (status < 0) {
        cmd_error(out, "source", argv[1], "no such file");
        return 1;
    }
    return status;
}
