#include "sh/cmds.h"
#include "sh/shell.h"
#include "sched.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/format.h"

#define PASSWD_PATH "/etc/passwd"
#define PASSWD_LINE_MAX 160

struct passwd_entry {
    char name[32];
    uint16_t uid;
    uint16_t gid;
    char home[VFS_PATH_MAX];
    char pass[32];
};

static void passwd_ensure_seeded(void) {
    void *buf;
    uint32_t size;

    if (vfs_load(PASSWD_PATH, &buf, &size) == 0) {
        kfree(buf);
        return;
    }
    const char *seed = "root:0:0:/root:\n";
    vfs_save(PASSWD_PATH, seed, (uint32_t)strlen(seed));
    vfs_chmod(PASSWD_PATH, 0644);
}

static int passwd_parse_line(const char *line, struct passwd_entry *e) {
    const char *p = line;
    const char *colon;
    size_t n;

    memset(e, 0, sizeof(*e));
    colon = strchr(p, ':');
    if (!colon) {
        return -1;
    }
    n = (size_t)(colon - p);
    if (n >= sizeof(e->name)) {
        n = sizeof(e->name) - 1;
    }
    memcpy(e->name, p, n);
    e->name[n] = 0;
    p = colon + 1;

    colon = strchr(p, ':');
    if (!colon) {
        return -1;
    }
    e->uid = (uint16_t)strtol(p, NULL, 10);
    p = colon + 1;

    colon = strchr(p, ':');
    if (!colon) {
        return -1;
    }
    e->gid = (uint16_t)strtol(p, NULL, 10);
    p = colon + 1;

    colon = strchr(p, ':');
    if (!colon) {
        return -1;
    }
    n = (size_t)(colon - p);
    if (n >= sizeof(e->home)) {
        n = sizeof(e->home) - 1;
    }
    memcpy(e->home, p, n);
    e->home[n] = 0;
    p = colon + 1;

    n = strlen(p);
    if (n && p[n - 1] == '\n') {
        n--;
    }
    if (n >= sizeof(e->pass)) {
        n = sizeof(e->pass) - 1;
    }
    memcpy(e->pass, p, n);
    e->pass[n] = 0;
    return 0;
}

static int passwd_find_by_name(const char *name, struct passwd_entry *out) {
    void *buf;
    uint32_t size;
    int found = 0;

    passwd_ensure_seeded();
    if (vfs_load(PASSWD_PATH, &buf, &size) < 0) {
        return -1;
    }
    struct stream *s = stream_new();
    st_write(s, (const char *)buf, size);
    kfree(buf);

    char line[PASSWD_LINE_MAX];
    while (st_getline(s, line, sizeof(line)) > 0) {
        struct passwd_entry e;
        if (passwd_parse_line(line, &e) == 0 && e.name[0] && strcmp(e.name, name) == 0) {
            *out = e;
            found = 1;
            break;
        }
    }
    stream_free(s);
    return found ? 0 : -1;
}

static int passwd_find_by_uid(uint16_t uid, struct passwd_entry *out) {
    void *buf;
    uint32_t size;
    int found = 0;

    passwd_ensure_seeded();
    if (vfs_load(PASSWD_PATH, &buf, &size) < 0) {
        return -1;
    }
    struct stream *s = stream_new();
    st_write(s, (const char *)buf, size);
    kfree(buf);

    char line[PASSWD_LINE_MAX];
    while (st_getline(s, line, sizeof(line)) > 0) {
        struct passwd_entry e;
        if (passwd_parse_line(line, &e) == 0 && e.name[0] && e.uid == uid) {
            *out = e;
            found = 1;
            break;
        }
    }
    stream_free(s);
    return found ? 0 : -1;
}

static uint16_t passwd_next_uid(void) {
    void *buf;
    uint32_t size;
    uint16_t max_uid = 999;

    passwd_ensure_seeded();
    if (vfs_load(PASSWD_PATH, &buf, &size) < 0) {
        return 1000;
    }
    struct stream *s = stream_new();
    st_write(s, (const char *)buf, size);
    kfree(buf);

    char line[PASSWD_LINE_MAX];
    while (st_getline(s, line, sizeof(line)) > 0) {
        struct passwd_entry e;
        if (passwd_parse_line(line, &e) == 0 && e.name[0] && e.uid > max_uid) {
            max_uid = e.uid;
        }
    }
    stream_free(s);
    return (uint16_t)(max_uid + 1);
}

static int passwd_append(const struct passwd_entry *e) {
    char line[PASSWD_LINE_MAX];
    struct vfs_file *f;

    snprintf(line, sizeof(line), "%s:%u:%u:%s:%s\n",
             e->name, (unsigned)e->uid, (unsigned)e->gid, e->home, e->pass);
    passwd_ensure_seeded();
    if (vfs_open(PASSWD_PATH, VFS_O_WRITE | VFS_O_APPEND, &f) < 0) {
        return -1;
    }
    int len = (int)strlen(line);
    int r = vfs_write(f, line, (uint32_t)len);
    vfs_close(f);
    return r == len ? 0 : -1;
}

static int passwd_set_password(const char *name, const char *newpass) {
    void *buf;
    uint32_t size;

    passwd_ensure_seeded();
    if (vfs_load(PASSWD_PATH, &buf, &size) < 0) {
        return -1;
    }
    struct stream *in = stream_new();
    st_write(in, (const char *)buf, size);
    kfree(buf);

    struct stream *out = stream_new();
    char line[PASSWD_LINE_MAX];
    int found = 0;
    while (st_getline(in, line, sizeof(line)) > 0) {
        struct passwd_entry e;
        if (passwd_parse_line(line, &e) == 0 && e.name[0] && strcmp(e.name, name) == 0) {
            st_printf(out, "%s:%u:%u:%s:%s\n", e.name, (unsigned)e.uid, (unsigned)e.gid, e.home, newpass);
            found = 1;
        } else if (line[0]) {
            st_printf(out, "%s\n", line);
        }
    }
    stream_free(in);

    int r = -1;
    if (found) {
        r = vfs_save(PASSWD_PATH, st_data(out), st_len(out));
    }
    stream_free(out);
    return found ? r : -1;
}

static void apply_identity(const struct passwd_entry *e) {
    struct task *t = sched_current();

    t->uid = e->uid;
    t->gid = e->gid;
    shell_setenv("USER", e->name);
    shell_setenv("HOME", e->home);
    vfs_chdir(e->home);
}

int cmd_login(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        cmd_error(out, "login", NULL, "usage: login <name>");
        return 1;
    }
    struct passwd_entry e;
    if (passwd_find_by_name(argv[1], &e) < 0) {
        cmd_error(out, "login", argv[1], "no such user");
        return 1;
    }
    if (e.pass[0]) {
        char pwbuf[SHELL_LINE_MAX];
        shell_readline("Password: ", pwbuf);
        if (strcmp(pwbuf, e.pass) != 0) {
            st_puts(out, "login: incorrect password\n");
            return 1;
        }
    }
    apply_identity(&e);
    st_printf(out, "Welcome, %s.\n", e.name);
    return 0;
}

int cmd_su(int argc, char **argv, struct stream *in, struct stream *out) {
    const char *target = argc > 1 ? argv[1] : "root";
    struct passwd_entry e;

    if (passwd_find_by_name(target, &e) < 0) {
        cmd_error(out, "su", target, "no such user");
        return 1;
    }
    struct task *t = sched_current();
    if (t->uid != 0 && e.pass[0]) {
        char pwbuf[SHELL_LINE_MAX];
        shell_readline("Password: ", pwbuf);
        if (strcmp(pwbuf, e.pass) != 0) {
            st_puts(out, "su: authentication failure\n");
            return 1;
        }
    }
    apply_identity(&e);
    return 0;
}

int cmd_passwd(int argc, char **argv, struct stream *in, struct stream *out) {
    struct task *t = sched_current();
    const char *target = argc > 1 ? argv[1] : shell_getenv("USER");

    if (!target) {
        target = "root";
    }
    struct passwd_entry e;
    if (passwd_find_by_name(target, &e) < 0) {
        cmd_error(out, "passwd", target, "no such user");
        return 1;
    }
    if (t->uid != 0 && e.uid != t->uid) {
        st_puts(out, "passwd: permission denied\n");
        return 1;
    }
    if (t->uid != 0 && e.pass[0]) {
        char pwbuf[SHELL_LINE_MAX];
        shell_readline("Current password: ", pwbuf);
        if (strcmp(pwbuf, e.pass) != 0) {
            st_puts(out, "passwd: authentication failure\n");
            return 1;
        }
    }
    char newpass[SHELL_LINE_MAX];
    char confirm[SHELL_LINE_MAX];
    shell_readline("New password: ", newpass);
    shell_readline("Confirm password: ", confirm);
    if (strcmp(newpass, confirm) != 0) {
        st_puts(out, "passwd: passwords do not match\n");
        return 1;
    }
    if (passwd_set_password(e.name, newpass) < 0) {
        st_puts(out, "passwd: could not update\n");
        return 1;
    }
    st_puts(out, "Password updated.\n");
    return 0;
}

int cmd_useradd(int argc, char **argv, struct stream *in, struct stream *out) {
    struct task *t = sched_current();

    if (t->uid != 0) {
        st_puts(out, "useradd: permission denied\n");
        return 1;
    }
    if (argc < 2) {
        cmd_error(out, "useradd", NULL, "usage: useradd <name> [uid]");
        return 1;
    }
    struct passwd_entry existing;
    if (passwd_find_by_name(argv[1], &existing) == 0) {
        cmd_error(out, "useradd", argv[1], "already exists");
        return 1;
    }
    struct passwd_entry e;
    memset(&e, 0, sizeof(e));
    strlcpy(e.name, argv[1], sizeof(e.name));
    e.uid = argc > 2 ? (uint16_t)strtol(argv[2], NULL, 10) : passwd_next_uid();
    e.gid = e.uid;
    snprintf(e.home, sizeof(e.home), "/home/%s", argv[1]);

    if (passwd_append(&e) < 0) {
        st_puts(out, "useradd: could not write /etc/passwd\n");
        return 1;
    }
    vfs_mkdir("/home");
    vfs_mkdir(e.home);
    vfs_chown(e.home, e.uid, e.gid);
    st_printf(out, "Added user %s (uid=%u gid=%u home=%s)\n", e.name, (unsigned)e.uid, (unsigned)e.gid, e.home);
    return 0;
}

int cmd_id(int argc, char **argv, struct stream *in, struct stream *out) {
    struct task *t = sched_current();
    struct passwd_entry e;
    const char *name = passwd_find_by_uid(t->uid, &e) == 0 ? e.name : "?";

    st_printf(out, "uid=%u(%s) gid=%u\n", (unsigned)t->uid, name, (unsigned)t->gid);
    return 0;
}

int cmd_chown(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 3) {
        cmd_error(out, "chown", NULL, "usage: chown <user>[:group] <path...>");
        return 1;
    }
    char spec[64];
    strlcpy(spec, argv[1], sizeof(spec));
    char *colon = strchr(spec, ':');
    uint16_t uid = VFS_CHOWN_KEEP;
    uint16_t gid = VFS_CHOWN_KEEP;

    if (colon) {
        *colon = 0;
    }
    if (spec[0]) {
        struct passwd_entry e;
        if (isdigit((unsigned char)spec[0])) {
            uid = (uint16_t)strtol(spec, NULL, 10);
        } else if (passwd_find_by_name(spec, &e) == 0) {
            uid = e.uid;
            if (!colon) {
                gid = e.gid;
            }
        } else {
            cmd_error(out, "chown", spec, "no such user");
            return 1;
        }
    }
    if (colon && colon[1]) {
        if (isdigit((unsigned char)colon[1])) {
            gid = (uint16_t)strtol(colon + 1, NULL, 10);
        } else {
            struct passwd_entry e;
            if (passwd_find_by_name(colon + 1, &e) == 0) {
                gid = e.gid;
            }
        }
    }
    int status = 0;
    for (int i = 2; i < argc; i++) {
        int r = vfs_chown(argv[i], uid, gid);
        if (r < 0) {
            cmd_vfs_error(out, "chown", argv[i], r);
            status = 1;
        }
    }
    return status;
}
