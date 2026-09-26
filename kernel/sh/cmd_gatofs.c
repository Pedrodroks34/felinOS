#include "sh/cmds.h"
#include "fs/gatofs.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/format.h"
#include "drivers/ata.h"

#define DEFAULT_MOUNT "/mnt/gato"

static int fail(struct stream *out, const char *what, int err) {
    st_printf(out, "gatofs: %s: %s\n", what, vfs_strerror(err));
    return 1;
}

static int need_mount(struct stream *out, struct vfs_mount_info *mi) {
    if (vfs_mount_of("gatofs", mi) == 0) return 0;
    st_puts(out, "gatofs: not mounted (use: gatofs format <dev> / gatofs mount <dev>)\n");
    return 1;
}

static int vpath(char *out, size_t size, const struct vfs_mount_info *mi, const char *path) {
    int root = strcmp(mi->path, "/") == 0;
    int n = snprintf(out, size, "%s%s%s", root ? "" : mi->path, path[0] == '/' ? "" : "/", path);
    return (n < 0 || (size_t)n >= size) ? VFS_ENAMETOOLONG : VFS_OK;
}

static int attach(struct stream *out, const char *disk, const char *dir) {
    int r = vfs_mkpath(dir);
    if (r < 0) return fail(out, dir, r);
    r = vfs_mount("gatofs", disk, dir);
    if (r == VFS_EINVAL) return fail(out, disk ? disk : dir, VFS_EINVAL);
    if (r < 0) return fail(out, disk ? disk : dir, r);
    st_printf(out, "mounted on %s.\n", dir);
    return 0;
}

static uint32_t pat(uint32_t off) { return (off >> 2) * 2654435761u ^ 0xA5A5A5A5u; }

static int gen_or_verify(int verify, int argc, char **argv, struct stream *out) {
    struct vfs_mount_info mi;
    char full[VFS_PATH_MAX];
    if (argc < 4) { st_puts(out, "usage: gatofs gen|verify <path> <MB> [startMB]\n"); return 1; }
    uint32_t mb = strtou32(argv[3], 10), start = argc > 4 ? strtou32(argv[4], 10) : 0;
    if (!mb || start + mb > 4095) { st_puts(out, "gatofs: range must stay under 4095 MB\n"); return 1; }
    if (need_mount(out, &mi)) return 1;
    int r = vpath(full, sizeof(full), &mi, argv[2]);
    if (r < 0) return fail(out, argv[2], r);
    struct vfs_file *f;
    r = vfs_open(full, verify ? VFS_O_READ : (VFS_O_WRITE | VFS_O_CREATE), &f);
    if (r < 0) return fail(out, argv[2], r);
    uint32_t *buf = kmalloc(65536);
    if (!buf) { vfs_close(f); return fail(out, "memory", VFS_ENOMEM); }
    uint32_t off = start << 20, total = mb << 20, done = 0;
    vfs_seek(f, off);
    while (done < total) {
        if (verify) {
            int n = vfs_read(f, buf, 65536);
            if (n != 65536) { st_printf(out, "FAIL: short read %d at %u MB\n", n, (off + done) >> 20); kfree(buf); vfs_close(f); return 1; }
            for (uint32_t i = 0; i < 16384; i++)
                if (buf[i] != pat(off + done + i * 4)) {
                    st_printf(out, "FAIL: mismatch at byte %u\n", off + done + i * 4);
                    kfree(buf); vfs_close(f); return 1;
                }
        } else {
            for (uint32_t i = 0; i < 16384; i++) buf[i] = pat(off + done + i * 4);
            int n = vfs_write(f, buf, 65536);
            if (n != 65536) { st_printf(out, "write stopped at %u MB: %s\n", (off + done) >> 20, vfs_strerror(n < 0 ? n : VFS_ENOSPC)); kfree(buf); vfs_close(f); return 1; }
        }
        done += 65536;
        if ((done & 0x3FFFFFF) == 0) st_printf(out, "  %u MB\n", done >> 20);
    }
    kfree(buf);
    vfs_close(f);
    st_printf(out, "%s ok: %u MB\n", verify ? "verify" : "gen", mb);
    return 0;
}

static void fsck_report(const char *msg, void *ctx) {
    st_printf((struct stream *)ctx, "  %s\n", msg);
}

int cmd_gatofs(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        st_puts(out, "usage: gatofs format|mount|umount|df|ls|cat|write|mkdir|rm|mv|stat|import|export|gen|verify|fsck ...\n");
        return 1;
    }
    const char *c = argv[1];
    struct vfs_mount_info mi;
    char full[VFS_PATH_MAX];
    char full2[VFS_PATH_MAX];
    int r;

    if (!strcmp(c, "format")) {
        int force = argc > 2 && !strcmp(argv[2], "-f");
        int a = 2 + force;
        struct ata_device *d = argc > a ? ata_find(argv[a]) : 0;
        if (!d || d->type != ATA_TYPE_ATA) { st_puts(out, "usage: gatofs format [-f] <disk> [label]   (see lsblk)\n"); return 1; }
        if (gatofs_mounted()) { st_puts(out, "gatofs: a volume is already mounted; only one at a time (see: mount, gatofs umount)\n"); return 1; }
        if (!force) {
            uint8_t sec[512];
            if (ata_read_sectors(d, 0, 1, sec) == 0 && sec[510] == 0x55 && sec[511] == 0xAA) {
                st_puts(out, "gatofs: disk has a partition table/boot sector; use -f to overwrite\n");
                return 1;
            }
        }
        st_printf(out, "formatting %s ...\n", d->name);
        gatofs_fs_lock();
        r = gatofs_format(d, argc > a + 1 ? argv[a + 1] : 0);
        gatofs_fs_unlock();
        if (r < 0) return fail(out, "format", r);
        st_puts(out, "done, ");
        return attach(out, d->name, DEFAULT_MOUNT);
    }
    if (!strcmp(c, "mount")) {
        if (vfs_mount_of("gatofs", &mi) == 0) {
            st_printf(out, "gatofs: already mounted on %s\n", mi.path);
            return 1;
        }
        return attach(out, argc > 2 ? argv[2] : NULL, argc > 3 ? argv[3] : DEFAULT_MOUNT);
    }
    if (!strcmp(c, "umount")) {
        if (need_mount(out, &mi)) return 1;
        r = vfs_umount(mi.path);
        return r < 0 ? fail(out, mi.path, r) : 0;
    }
    if (need_mount(out, &mi)) return 1;

    if (!strcmp(c, "df")) {
        struct gatofs_info i;
        gatofs_fs_lock();
        gatofs_info(&i);
        gatofs_fs_unlock();
        st_printf(out, "%s [%s]  total %u MB  used %u MB  free %u MB  inodes free %u/%u\n", i.label, i.dev,
                  i.total_blocks >> 8, (i.total_blocks - i.free_blocks) >> 8, i.free_blocks >> 8,
                  i.free_inodes, i.total_inodes);
        return 0;
    }
    if (!strcmp(c, "ls")) {
        int lng = 0;
        const char *p = "/";
        for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "-l")) lng = 1; else p = argv[i]; }
        r = vpath(full, sizeof(full), &mi, p);
        if (r < 0) return fail(out, p, r);
        struct vfs_dirent *list = NULL;
        int count = 0;
        r = vfs_list(full, &list, &count);
        if (r < 0) return fail(out, p, r);
        for (int i = 0; i < count; i++) {
            int dir = list[i].type == VFS_DIR;
            if (!lng) { st_printf(out, "%s%s\n", list[i].name, dir ? "/" : ""); continue; }
            struct vfs_stat st;
            char sz[16], tm[32];
            if (vfs_join(full2, sizeof(full2), full, list[i].name) < 0 || vfs_stat(full2, &st) < 0) continue;
            cmd_format_size64(st.size, sz, sizeof(sz));
            cmd_format_time(st.mtime, tm, sizeof(tm));
            st_printf(out, "%c %8s %s %s%s\n", dir ? 'd' : '-', sz, tm, list[i].name, dir ? "/" : "");
        }
        kfree(list);
        return 0;
    }
    if (!strcmp(c, "cat") && argc > 2) {
        r = vpath(full, sizeof(full), &mi, argv[2]);
        if (r < 0) return fail(out, argv[2], r);
        struct vfs_file *f;
        r = vfs_open(full, VFS_O_READ, &f);
        if (r < 0) return fail(out, argv[2], r);
        char buf[512];
        int n;
        while ((n = vfs_read(f, buf, sizeof(buf))) > 0) st_write(out, buf, n);
        vfs_close(f);
        return 0;
    }
    if (!strcmp(c, "write") && argc > 3) {
        char text[512] = "";
        for (int i = 3; i < argc; i++) { if (i > 3) strlcpy(text + strlen(text), " ", 2); strlcpy(text + strlen(text), argv[i], sizeof(text) - strlen(text)); }
        strlcpy(text + strlen(text), "\n", sizeof(text) - strlen(text));
        r = vpath(full, sizeof(full), &mi, argv[2]);
        if (r == 0) r = vfs_save(full, text, (uint32_t)strlen(text));
        return r < 0 ? fail(out, argv[2], r) : 0;
    }
    if (!strcmp(c, "mkdir") && argc > 2) {
        int p = !strcmp(argv[2], "-p");
        if (p && argc < 4) return 1;
        r = vpath(full, sizeof(full), &mi, argv[2 + p]);
        if (r == 0) r = p ? vfs_mkpath(full) : vfs_mkdir(full);
        return r < 0 ? fail(out, argv[2 + p], r) : 0;
    }
    if (!strcmp(c, "rm") && argc > 2) {
        int rec = !strcmp(argv[2], "-r");
        if (rec && argc < 4) return 1;
        struct vfs_stat st;
        r = vpath(full, sizeof(full), &mi, argv[2 + rec]);
        if (r == 0) {
            if (vfs_stat(full, &st) == 0 && st.type == VFS_DIR) r = rec ? vfs_remove_tree(full) : vfs_rmdir(full);
            else r = vfs_unlink(full);
        }
        return r < 0 ? fail(out, argv[2 + rec], r) : 0;
    }
    if (!strcmp(c, "mv") && argc > 3) {
        r = vpath(full, sizeof(full), &mi, argv[2]);
        if (r == 0) r = vpath(full2, sizeof(full2), &mi, argv[3]);
        if (r == 0) r = vfs_rename(full, full2);
        return r < 0 ? fail(out, argv[2], r) : 0;
    }
    if (!strcmp(c, "stat") && argc > 2) {
        struct gatofs_stat s;
        gatofs_fs_lock();
        r = gatofs_stat(argv[2], &s);
        gatofs_fs_unlock();
        if (r < 0) return fail(out, argv[2], r);
        st_printf(out, "type %s  size %u bytes  blocks %u (4K)  inode %u  mode %o\n",
                  s.type == GATOFS_DIR ? "dir" : "file", s.size, s.blocks, s.ino, s.mode);
        return 0;
    }
    if (!strcmp(c, "import") && argc > 3) {
        r = vpath(full, sizeof(full), &mi, argv[3]);
        if (r == 0) r = vfs_copy_file(argv[2], full);
        return r < 0 ? fail(out, argv[3], r) : 0;
    }
    if (!strcmp(c, "export") && argc > 3) {
        r = vpath(full, sizeof(full), &mi, argv[2]);
        if (r == 0) r = vfs_copy_file(full, argv[3]);
        return r < 0 ? fail(out, argv[2], r) : 0;
    }
    if (!strcmp(c, "fsck")) {
        int repair = argc > 2 && !strcmp(argv[2], "-y");
        struct gatofs_fsck_result res;
        gatofs_fs_lock();
        r = gatofs_fsck(repair, fsck_report, out, &res);
        gatofs_fs_unlock();
        if (r < 0) return fail(out, "fsck", r);
        st_printf(out, "checked %u inodes, %u blocks: %u problem(s), %u fixed\n",
                  res.inodes_checked, res.blocks_checked, res.errors, res.fixed);
        if (res.errors && !repair) { st_puts(out, "run 'gatofs fsck -y' to repair\n"); return 1; }
        return 0;
    }
    if (!strcmp(c, "gen")) return gen_or_verify(0, argc, argv, out);
    if (!strcmp(c, "verify")) return gen_or_verify(1, argc, argv, out);

    st_printf(out, "gatofs: unknown or incomplete subcommand '%s'\n", c);
    return 1;
}
