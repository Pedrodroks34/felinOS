#include "sh/cmds.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/format.h"
#include "drivers/ata.h"

static int match_pattern(const char *pattern, const char *text) {
    if (*pattern == '\0') {
        return *text == '\0';
    }
    if (*pattern == '*') {
        while (*pattern == '*') {
            pattern++;
        }
        if (*pattern == '\0') {
            return 1;
        }
        while (*text) {
            if (match_pattern(pattern, text)) {
                return 1;
            }
            text++;
        }
        return 0;
    }
    if (*text == '\0') {
        return 0;
    }
    if (*pattern == '?' || *pattern == *text) {
        return match_pattern(pattern + 1, text + 1);
    }
    return 0;
}

static void fix_cwd(void) {
    char path[VFS_PATH_MAX];
    struct vfs_stat st;

    strlcpy(path, vfs_getcwd(), sizeof(path));
    while (strcmp(path, "/") != 0 && vfs_stat(path, &st) < 0) {
        char *slash = strrchr(path, '/');
        if (slash == path) {
            path[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
    vfs_chdir(path);
}

static void list_entry_long(struct stream *out, const char *name, const struct vfs_stat *st, int slash) {
    char mode[12];
    char time[32];

    cmd_mode_string(st->type, st->mode, mode);
    cmd_format_time(st->mtime, time, sizeof(time));
    st_printf(out, "%s %-6llu %s %s%s\n", mode, st->size, time, name,
              (slash && st->type == VFS_DIR) ? "/" : "");
}

static int list_dir(struct stream *out, const char *path, int long_format, int all) {
    struct vfs_dirent *list = NULL;
    int count = 0;
    char base[VFS_PATH_MAX];
    int r = vfs_list(path, &list, &count);

    if (r < 0) {
        return r;
    }
    r = vfs_normalize(path, base, sizeof(base));
    if (r < 0) {
        kfree(list);
        return r;
    }

    if (long_format) {
        struct vfs_stat st;
        if (all) {
            char up[VFS_PATH_MAX];
            snprintf(up, sizeof(up), "%s/..", base);
            if (vfs_stat(base, &st) == 0) {
                list_entry_long(out, ".", &st, 0);
            }
            if (vfs_stat(up, &st) == 0) {
                list_entry_long(out, "..", &st, 0);
            }
        }
        for (int i = 0; i < count; i++) {
            char full[VFS_PATH_MAX];
            if (vfs_join(full, sizeof(full), base, list[i].name) < 0 || vfs_stat(full, &st) < 0) {
                continue;
            }
            list_entry_long(out, list[i].name, &st, 1);
        }
    } else {
        int column = 0;
        if (all) {
            st_puts(out, ".  ..  ");
            column = 7;
        }
        for (int i = 0; i < count; i++) {
            int is_dir = list[i].type == VFS_DIR;
            int len = (int)strlen(list[i].name) + (is_dir ? 1 : 0);
            if (column + len + 2 > 80 && column > 0) {
                st_putc(out, '\n');
                column = 0;
            }
            st_printf(out, "%s%s  ", list[i].name, is_dir ? "/" : "");
            column += len + 2;
        }
        if (count || all) {
            st_putc(out, '\n');
        }
    }
    kfree(list);
    return VFS_OK;
}

int cmd_ls(int argc, char **argv, struct stream *in, struct stream *out) {
    int long_format = cmd_has_flag(argc, argv, "-l");
    int all = cmd_has_flag(argc, argv, "-a");
    int paths = 0;
    int status = 0;

    for (int i = 1; i < argc; i++) {
        if (!cmd_is_flag(argv[i])) {
            paths++;
        }
    }

    if (paths == 0) {
        int r = list_dir(out, ".", long_format, all);
        if (r < 0) {
            cmd_vfs_error(out, "ls", ".", r);
            return 1;
        }
        return 0;
    }

    int printed = 0;
    for (int i = 1; i < argc; i++) {
        if (cmd_is_flag(argv[i])) {
            continue;
        }
        struct vfs_stat st;
        int r = vfs_stat(argv[i], &st);
        if (r < 0) {
            cmd_vfs_error(out, "ls", argv[i], r);
            status = 1;
            continue;
        }
        if (st.type != VFS_DIR) {
            if (long_format) {
                list_entry_long(out, argv[i], &st, 1);
            } else {
                st_printf(out, "%s\n", argv[i]);
            }
            continue;
        }
        if (paths > 1) {
            if (printed) {
                st_putc(out, '\n');
            }
            st_printf(out, "%s:\n", argv[i]);
        }
        r = list_dir(out, argv[i], long_format, all);
        if (r < 0) {
            cmd_vfs_error(out, "ls", argv[i], r);
            status = 1;
        }
        printed = 1;
    }
    return status;
}

int cmd_cd(int argc, char **argv, struct stream *in, struct stream *out) {
    const char *target = (argc > 1) ? argv[1] : "/root";
    int r = vfs_chdir(target);

    if (r < 0) {
        cmd_vfs_error(out, "cd", target, r);
        return 1;
    }
    return 0;
}

int cmd_pwd(int argc, char **argv, struct stream *in, struct stream *out) {
    st_printf(out, "%s\n", vfs_getcwd());
    return 0;
}

int cmd_mkdir(int argc, char **argv, struct stream *in, struct stream *out) {
    int parents = cmd_has_flag(argc, argv, "-p");
    int made = 0;
    int status = 0;

    for (int i = 1; i < argc; i++) {
        if (cmd_is_flag(argv[i])) {
            continue;
        }
        made++;
        int r = parents ? vfs_mkpath(argv[i]) : vfs_mkdir(argv[i]);
        if (r < 0) {
            cmd_vfs_error(out, "mkdir", argv[i], r);
            status = 1;
        }
    }

    if (!made) {
        cmd_error(out, "mkdir", NULL, "missing operand");
        return 1;
    }
    return status;
}

int cmd_rmdir(int argc, char **argv, struct stream *in, struct stream *out) {
    int status = 0;
    int given = 0;

    for (int i = 1; i < argc; i++) {
        if (cmd_is_flag(argv[i])) {
            continue;
        }
        given++;
        int r = vfs_rmdir(argv[i]);
        if (r < 0) {
            cmd_vfs_error(out, "rmdir", argv[i], r);
            status = 1;
        }
    }
    fix_cwd();

    if (!given) {
        cmd_error(out, "rmdir", NULL, "missing operand");
        return 1;
    }
    return status;
}

int cmd_touch(int argc, char **argv, struct stream *in, struct stream *out) {
    int given = 0;
    int status = 0;

    for (int i = 1; i < argc; i++) {
        if (cmd_is_flag(argv[i])) {
            continue;
        }
        given++;
        struct vfs_stat st;
        int r;
        if (vfs_stat(argv[i], &st) == 0) {
            r = vfs_touch(argv[i]);
        } else {
            struct vfs_file *f;
            r = vfs_open(argv[i], VFS_O_WRITE | VFS_O_CREATE, &f);
            if (r == 0) {
                vfs_close(f);
            }
        }
        if (r < 0) {
            cmd_vfs_error(out, "touch", argv[i], r);
            status = 1;
        }
    }

    if (!given) {
        cmd_error(out, "touch", NULL, "missing operand");
        return 1;
    }
    return status;
}

int cmd_rm(int argc, char **argv, struct stream *in, struct stream *out) {
    int recursive = cmd_has_flag(argc, argv, "-r");
    int force = cmd_has_flag(argc, argv, "-f");
    int given = 0;
    int status = 0;

    for (int i = 1; i < argc; i++) {
        if (cmd_is_flag(argv[i])) {
            continue;
        }
        given++;
        char abs[VFS_PATH_MAX];
        struct vfs_stat st;
        if (vfs_normalize(argv[i], abs, sizeof(abs)) < 0 || vfs_stat(abs, &st) < 0) {
            if (!force) {
                cmd_error(out, "rm", argv[i], "no such file or directory");
                status = 1;
            }
            continue;
        }
        if (strcmp(abs, "/") == 0) {
            cmd_error(out, "rm", argv[i], "refusing to remove the root directory");
            status = 1;
            continue;
        }
        if (st.type == VFS_DIR && !recursive) {
            cmd_error(out, "rm", argv[i], "is a directory");
            status = 1;
            continue;
        }
        int r = (st.type == VFS_DIR) ? vfs_remove_tree(abs) : vfs_unlink(abs);
        if (r < 0) {
            cmd_vfs_error(out, "rm", argv[i], r);
            status = 1;
        }
    }
    fix_cwd();

    if (!given && !force) {
        cmd_error(out, "rm", NULL, "missing operand");
        return 1;
    }
    return status;
}

static int dest_for(const char *src, const char *dest, int dest_is_dir, char *target, size_t size) {
    if (!dest_is_dir) {
        return vfs_normalize(dest, target, size);
    }
    char sabs[VFS_PATH_MAX];
    char dabs[VFS_PATH_MAX];
    int r = vfs_normalize(src, sabs, sizeof(sabs));
    if (r < 0) {
        return r;
    }
    r = vfs_normalize(dest, dabs, sizeof(dabs));
    if (r < 0) {
        return r;
    }
    return vfs_join(target, size, dabs, vfs_basename(sabs));
}

int cmd_cp(int argc, char **argv, struct stream *in, struct stream *out) {
    int recursive = cmd_has_flag(argc, argv, "-r");
    char *paths[SHELL_MAX_ARGS];
    int count = 0;

    for (int i = 1; i < argc; i++) {
        if (!cmd_is_flag(argv[i])) {
            paths[count++] = argv[i];
        }
    }
    if (count < 2) {
        cmd_error(out, "cp", NULL, "usage: cp [-r] <source...> <dest>");
        return 1;
    }

    const char *dest = paths[count - 1];
    struct vfs_stat dst;
    int dest_is_dir = vfs_stat(dest, &dst) == 0 && dst.type == VFS_DIR;

    if (count > 2 && !dest_is_dir) {
        cmd_error(out, "cp", dest, "target is not a directory");
        return 1;
    }

    int status = 0;
    for (int i = 0; i < count - 1; i++) {
        struct vfs_stat st;
        int r = vfs_stat(paths[i], &st);
        if (r < 0) {
            cmd_vfs_error(out, "cp", paths[i], r);
            status = 1;
            continue;
        }
        if (st.type == VFS_DIR && !recursive) {
            cmd_error(out, "cp", paths[i], "omitting directory, use -r");
            status = 1;
            continue;
        }
        char target[VFS_PATH_MAX];
        r = dest_for(paths[i], dest, dest_is_dir, target, sizeof(target));
        if (r == 0) {
            r = (st.type == VFS_DIR) ? vfs_copy_tree(paths[i], target) : vfs_copy_file(paths[i], target);
        }
        if (r == VFS_EINVAL) {
            cmd_error(out, "cp", paths[i], "source and destination are the same or overlap");
            status = 1;
        } else if (r < 0) {
            cmd_vfs_error(out, "cp", paths[i], r);
            status = 1;
        }
    }
    return status;
}

int cmd_mv(int argc, char **argv, struct stream *in, struct stream *out) {
    char *paths[SHELL_MAX_ARGS];
    int count = 0;

    for (int i = 1; i < argc; i++) {
        if (!cmd_is_flag(argv[i])) {
            paths[count++] = argv[i];
        }
    }
    if (count < 2) {
        cmd_error(out, "mv", NULL, "usage: mv <source...> <dest>");
        return 1;
    }

    const char *dest = paths[count - 1];
    struct vfs_stat dst;
    int dest_is_dir = vfs_stat(dest, &dst) == 0 && dst.type == VFS_DIR;
    int status = 0;

    for (int i = 0; i < count - 1; i++) {
        char sabs[VFS_PATH_MAX];
        char target[VFS_PATH_MAX];
        struct vfs_stat st;
        int r = vfs_normalize(paths[i], sabs, sizeof(sabs));
        if (r == 0) {
            r = vfs_stat(sabs, &st);
        }
        if (r < 0) {
            cmd_vfs_error(out, "mv", paths[i], r);
            status = 1;
            continue;
        }
        if (strcmp(sabs, "/") == 0) {
            cmd_error(out, "mv", paths[i], "cannot move the root directory");
            status = 1;
            continue;
        }
        r = dest_for(paths[i], dest, dest_is_dir, target, sizeof(target));
        if (r < 0) {
            cmd_vfs_error(out, "mv", dest, r);
            status = 1;
            continue;
        }

        struct vfs_stat existing;
        if (!dest_is_dir && st.type == VFS_FILE && vfs_stat(target, &existing) == 0 &&
            existing.type == VFS_FILE && strcmp(sabs, target) != 0) {
            vfs_unlink(target);
        }

        r = vfs_rename(sabs, target);
        if (r == VFS_EXDEV) {
            r = vfs_copy_tree(sabs, target);
            if (r == 0) {
                r = (st.type == VFS_DIR) ? vfs_remove_tree(sabs) : vfs_unlink(sabs);
                fix_cwd();
            }
        }
        if (r < 0) {
            cmd_vfs_error(out, "mv", paths[i], r);
            status = 1;
        }
    }
    return status;
}

int cmd_cat(int argc, char **argv, struct stream *in, struct stream *out) {
    int numbered = cmd_has_flag(argc, argv, "-n");
    uint32_t len = 0;
    char *text = cmd_collect_input(argc, argv, 1, in, out, "cat", &len);

    if (!text) {
        return 1;
    }
    if (!numbered) {
        st_write(out, text, len);
        if (len && text[len - 1] != '\n') {
            st_putc(out, '\n');
        }
        kfree(text);
        return 0;
    }

    char **lines;
    int count = cmd_split_lines(text, &lines);
    for (int i = 0; i < count; i++) {
        st_printf(out, "%6u  %s\n", (uint32_t)(i + 1), lines[i]);
    }
    kfree(lines);
    kfree(text);
    return 0;
}

static void tree_walk(struct stream *out, char *path, size_t len, const char *prefix,
                      uint32_t *dirs, uint32_t *files) {
    struct vfs_dirent *list = NULL;
    int count = 0;

    if (vfs_list(path, &list, &count) < 0) {
        return;
    }
    for (int i = 0; i < count; i++) {
        int last = (i == count - 1);
        int is_dir = list[i].type == VFS_DIR;
        st_printf(out, "%s%s%s%s\n", prefix, last ? "`-- " : "|-- ",
                  list[i].name, is_dir ? "/" : "");

        if (!is_dir) {
            (*files)++;
            continue;
        }
        (*dirs)++;

        char next[256];
        size_t nl = len;
        size_t name_len = strlen(list[i].name);
        if (strlen(prefix) + 4 >= sizeof(next) || len + 1 + name_len >= VFS_PATH_MAX) {
            continue;
        }
        snprintf(next, sizeof(next), "%s%s", prefix, last ? "    " : "|   ");
        if (nl > 1) {
            path[nl++] = '/';
        }
        strcpy(path + nl, list[i].name);
        nl += name_len;
        tree_walk(out, path, nl, next, dirs, files);
        path[len] = '\0';
    }
    kfree(list);
}

int cmd_tree(int argc, char **argv, struct stream *in, struct stream *out) {
    const char *target = ".";
    for (int i = 1; i < argc; i++) {
        if (!cmd_is_flag(argv[i])) {
            target = argv[i];
            break;
        }
    }

    char path[VFS_PATH_MAX];
    struct vfs_stat st;
    int r = vfs_normalize(target, path, sizeof(path));
    if (r == 0) {
        r = vfs_stat(path, &st);
    }
    if (r < 0) {
        cmd_vfs_error(out, "tree", target, r);
        return 1;
    }
    if (st.type != VFS_DIR) {
        st_printf(out, "%s\n", vfs_basename(path));
        return 0;
    }

    st_printf(out, "%s\n", path);

    uint32_t dirs = 0;
    uint32_t files = 0;
    tree_walk(out, path, strlen(path), "", &dirs, &files);
    st_printf(out, "\n%u directories, %u files\n", dirs, files);
    return 0;
}

static void find_walk(struct stream *out, char *path, size_t len, const char *pattern, char type_filter) {
    struct vfs_stat st;

    if (vfs_stat(path, &st) < 0) {
        return;
    }
    const char *name = (len == 1) ? "/" : vfs_basename(path);
    int type_ok = (type_filter == 0) ||
                  (type_filter == 'f' && st.type == VFS_FILE) ||
                  (type_filter == 'd' && st.type == VFS_DIR);
    int name_ok = (pattern == NULL) || match_pattern(pattern, name);

    if (type_ok && name_ok) {
        st_printf(out, "%s\n", path);
    }
    if (st.type != VFS_DIR) {
        return;
    }

    struct vfs_dirent *list = NULL;
    int count = 0;
    if (vfs_list(path, &list, &count) < 0) {
        return;
    }
    for (int i = 0; i < count; i++) {
        size_t nl = len;
        if (len + 1 + strlen(list[i].name) >= VFS_PATH_MAX) {
            continue;
        }
        if (nl > 1) {
            path[nl++] = '/';
        }
        strcpy(path + nl, list[i].name);
        nl += strlen(list[i].name);
        find_walk(out, path, nl, pattern, type_filter);
        path[len] = '\0';
    }
    kfree(list);
}

int cmd_find(int argc, char **argv, struct stream *in, struct stream *out) {
    const char *start = ".";
    const char *pattern = NULL;
    char type_filter = 0;
    int have_start = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            pattern = argv[++i];
        } else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            type_filter = argv[++i][0];
        } else if (!have_start && !cmd_is_flag(argv[i])) {
            start = argv[i];
            have_start = 1;
        }
    }

    char path[VFS_PATH_MAX];
    struct vfs_stat st;
    int r = vfs_normalize(start, path, sizeof(path));
    if (r == 0) {
        r = vfs_stat(path, &st);
    }
    if (r < 0) {
        cmd_vfs_error(out, "find", start, r);
        return 1;
    }
    find_walk(out, path, strlen(path), pattern, type_filter);
    return 0;
}

static const char *type_name(uint8_t type) {
    switch (type) {
    case VFS_DIR: return "directory";
    case VFS_CHR: return "character device";
    case VFS_BLK: return "block device";
    default:      return "regular file";
    }
}

int cmd_stat(int argc, char **argv, struct stream *in, struct stream *out) {
    int given = 0;
    int status = 0;

    for (int i = 1; i < argc; i++) {
        if (cmd_is_flag(argv[i])) {
            continue;
        }
        given++;
        char path[VFS_PATH_MAX];
        struct vfs_stat st;
        int r = vfs_normalize(argv[i], path, sizeof(path));
        if (r == 0) {
            r = vfs_stat(path, &st);
        }
        if (r < 0) {
            cmd_vfs_error(out, "stat", argv[i], r);
            status = 1;
            continue;
        }

        char mode[12];
        char time[32];
        cmd_mode_string(st.type, st.mode, mode);
        cmd_format_time(st.mtime, time, sizeof(time));

        st_printf(out, "  File: %s\n", path);
        st_printf(out, "  Type: %s\n", type_name(st.type));
        st_printf(out, "  Size: %llu bytes\n", st.size);
        st_printf(out, "  Mode: %s (%o)\n", mode, st.mode);
        st_printf(out, "Modify: %s\n", time);
        st_printf(out, " Inode: %u\n", st.ino);
        if (st.type == VFS_DIR) {
            struct vfs_dirent *list = NULL;
            int count = 0;
            if (vfs_list(path, &list, &count) == 0) {
                st_printf(out, "Entries: %u\n", (uint32_t)count);
                kfree(list);
            }
        }
        if (i + 1 < argc) {
            st_putc(out, '\n');
        }
    }

    if (!given) {
        cmd_error(out, "stat", NULL, "missing operand");
        return 1;
    }
    return status;
}

static void du_line(struct stream *out, int human, uint64_t size, const char *label, const char *suffix) {
    if (human) {
        char buf[16];
        cmd_format_size64(size, buf, sizeof(buf));
        st_printf(out, "%-8s  %s%s\n", buf, label, suffix);
    } else {
        st_printf(out, "%-8llu  %s%s\n", size, label, suffix);
    }
}

int cmd_du(int argc, char **argv, struct stream *in, struct stream *out) {
    int human = cmd_has_flag(argc, argv, "-h");
    const char *target = ".";

    for (int i = 1; i < argc; i++) {
        if (!cmd_is_flag(argv[i])) {
            target = argv[i];
            break;
        }
    }

    char path[VFS_PATH_MAX];
    struct vfs_stat st;
    int r = vfs_normalize(target, path, sizeof(path));
    if (r == 0) {
        r = vfs_stat(path, &st);
    }
    if (r < 0) {
        cmd_vfs_error(out, "du", target, r);
        return 1;
    }

    if (st.type == VFS_DIR) {
        struct vfs_dirent *list = NULL;
        int count = 0;
        if (vfs_list(path, &list, &count) == 0) {
            for (int i = 0; i < count; i++) {
                char child[VFS_PATH_MAX];
                if (list[i].type != VFS_DIR || vfs_join(child, sizeof(child), path, list[i].name) < 0) {
                    continue;
                }
                du_line(out, human, vfs_tree_size(child), list[i].name, "/");
            }
            kfree(list);
        }
    }
    du_line(out, human, vfs_tree_size(path), target, "");
    return 0;
}

int cmd_df(int argc, char **argv, struct stream *in, struct stream *out) {
    int human = cmd_has_flag(argc, argv, "-h");
    struct vfs_mount_info mi;

    st_printf(out, "%-12s %10s %10s %10s  %s\n",
              "Filesystem", "Size", "Used", "Avail", "Mounted on");

    for (int i = 0; vfs_mount_info(i, &mi) == 0; i++) {
        struct vfs_statfs sf;
        const char *name = strcmp(mi.source, "none") == 0 ? mi.type : mi.source;
        vfs_statfs(mi.path, &sf);
        if (human) {
            char a[16], b[16], c[16];
            cmd_format_size64(sf.total, a, sizeof(a));
            cmd_format_size64(sf.used, b, sizeof(b));
            cmd_format_size64(sf.avail, c, sizeof(c));
            st_printf(out, "%-12s %10s %10s %10s  %s\n", name, a, b, c, mi.path);
        } else {
            st_printf(out, "%-12s %10llu %10llu %10llu  %s\n", name, sf.total, sf.used, sf.avail, mi.path);
        }
    }
    return 0;
}

int cmd_mount(int argc, char **argv, struct stream *in, struct stream *out) {
    const char *type = NULL;
    const char *operands[2];
    int count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            type = argv[++i];
        } else if (count < 2) {
            operands[count++] = argv[i];
        }
    }

    if (!type && count == 0) {
        struct vfs_mount_info mi;
        for (int i = 0; vfs_mount_info(i, &mi) == 0; i++) {
            st_printf(out, "%s on %s type %s (rw)\n", mi.source, mi.path, mi.type);
        }
        return 0;
    }
    if (!type || count == 0) {
        cmd_error(out, "mount", NULL, "usage: mount [-t <type> [source] <dir>]");
        return 1;
    }

    int known = 0;
    for (int i = 0; i < vfs_fs_count(); i++) {
        if (strcmp(vfs_fs_name(i), type) == 0) {
            known = 1;
        }
    }
    if (!known) {
        cmd_error(out, "mount", type, "unknown filesystem type");
        return 1;
    }

    const char *source = (count == 2) ? operands[0] : NULL;
    const char *dir = operands[count - 1];
    int r = vfs_mount(type, source, dir);
    if (r < 0) {
        cmd_vfs_error(out, "mount", dir, r);
        return 1;
    }
    return 0;
}

int cmd_umount(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        cmd_error(out, "umount", NULL, "usage: umount <dir>");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int r = vfs_umount(argv[i]);
        if (r < 0) {
            cmd_vfs_error(out, "umount", argv[i], r);
            status = 1;
        }
    }
    return status;
}

int cmd_chmod(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 3) {
        cmd_error(out, "chmod", NULL, "usage: chmod <octal> <path...>");
        return 1;
    }

    uint16_t mode = (uint16_t)strtol(argv[1], NULL, 8);
    int status = 0;

    for (int i = 2; i < argc; i++) {
        int r = vfs_chmod(argv[i], mode & 0777);
        if (r < 0) {
            cmd_vfs_error(out, "chmod", argv[i], r);
            status = 1;
        }
    }
    return status;
}

int cmd_file(int argc, char **argv, struct stream *in, struct stream *out) {
    int given = 0;
    int status = 0;

    for (int i = 1; i < argc; i++) {
        if (cmd_is_flag(argv[i])) {
            continue;
        }
        given++;
        struct vfs_stat st;
        int r = vfs_stat(argv[i], &st);
        if (r < 0) {
            cmd_vfs_error(out, "file", argv[i], r);
            status = 1;
            continue;
        }
        if (st.type == VFS_DIR) {
            st_printf(out, "%s: directory\n", argv[i]);
            continue;
        }
        if (st.type == VFS_CHR || st.type == VFS_BLK) {
            st_printf(out, "%s: %s\n", argv[i], type_name(st.type));
            continue;
        }

        struct vfs_file *f;
        r = vfs_open(argv[i], VFS_O_READ, &f);
        if (r < 0) {
            cmd_vfs_error(out, "file", argv[i], r);
            status = 1;
            continue;
        }
        uint8_t sample[4096];
        int n = vfs_read(f, sample, sizeof(sample));
        vfs_close(f);
        if (n < 0) {
            cmd_vfs_error(out, "file", argv[i], n);
            status = 1;
            continue;
        }
        if (n == 0) {
            st_printf(out, "%s: empty\n", argv[i]);
            continue;
        }

        uint32_t printable = 0;
        for (int j = 0; j < n; j++) {
            uint8_t c = sample[j];
            if (isprint(c) || c == '\n' || c == '\t' || c == '\r') {
                printable++;
            }
        }
        uint64_t total = st.size ? st.size : (uint64_t)n;
        if (printable * 100 / (uint32_t)n >= 95) {
            st_printf(out, "%s: ASCII text, %llu bytes\n", argv[i], total);
        } else {
            st_printf(out, "%s: binary data, %llu bytes\n", argv[i], total);
        }
    }

    if (!given) {
        cmd_error(out, "file", NULL, "missing operand");
        return 1;
    }
    return status;
}
