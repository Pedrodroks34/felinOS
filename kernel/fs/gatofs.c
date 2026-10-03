#include "fs/gatofs.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/format.h"
#include "drivers/rtc.h"
#include "bcache.h"
#include "sched.h"

#define BS GATOFS_BLOCK
#define SPB (BS / ATA_SECTOR_SIZE)
#define NDIRECT 12
#define PPB (BS / 4)
#define IND_SLOT 12
#define DIND_SLOT 13
#define INO_SIZE 128
#define IPB (BS / INO_SIZE)
#define DE_SIZE 64
#define CACHE_N 256
#define MAX_FD 32
#define MAGIC "GATOFS01"

#define JRNL_MAX 8
#define JRNL_MAGIC "GJRNL01"

struct vsuper {
    char magic[8];
    uint32_t version, block_size, total_blocks, inode_count;
    uint32_t free_blocks, free_inodes;
    uint32_t bm_start, bm_blocks, itab_start, itab_blocks, data_start;
    uint32_t root_ino;
    uint32_t jrnl_start, jrnl_blocks;
    char label[32];
};

struct jrnl_header {
    char magic[8];
    uint32_t count;
    uint32_t checksum;
    uint32_t target[JRNL_MAX];
    uint8_t pad[512 - (8 + 4 + 4 + JRNL_MAX * 4)];
};

struct vinode {
    uint16_t type, mode;
    uint32_t size, mtime, nblocks;
    uint32_t blk[14];
    uint16_t uid, gid;
    uint8_t pad[44];
};

struct vdirent {
    uint32_t ino;
    uint8_t type, pad;
    char name[58];
};

struct cslot { uint32_t blk; int valid, dirty; uint8_t buf[BS]; };
struct vfd { int used; uint32_t ino; uint32_t pos; int flags; };

static struct ata_device *dev;
static struct vsuper sb;
static struct cslot *cache;
static struct vfd fds[MAX_FD];
static uint32_t blk_hint, ino_hint;
static int mounted;
static uint32_t pend_blk[JRNL_MAX];
static int pend_n;

const char *gatofs_strerror(int e) {
    switch (e) {
    case GATOFS_ENOENT: return "no such file or directory";
    case GATOFS_EEXIST: return "already exists";
    case GATOFS_ENOTDIR: return "not a directory";
    case GATOFS_EISDIR: return "is a directory";
    case GATOFS_ENOSPC: return "no space left";
    case GATOFS_EIO: return "i/o error";
    case GATOFS_EINVAL: return "invalid argument";
    case GATOFS_ENOTEMPTY: return "directory not empty";
    case GATOFS_ENOMOUNT: return "GatoFS not mounted";
    case GATOFS_ENAMETOOLONG: return "name too long";
    }
    return "error";
}

/* ---- raw block I/O ---- */
static int dread(uint32_t b, void *buf) {
    return bcache_read(dev, b * SPB, SPB, buf) < 0 ? -1 : 0;
}
static int dwrite(uint32_t b, const void *buf) {
    return bcache_write(dev, b * SPB, SPB, buf) < 0 ? -1 : 0;
}

static uint32_t jrnl_csum(uint32_t h, const uint8_t *buf) {
    for (uint32_t i = 0; i < BS; i++) { h ^= buf[i]; h *= 16777619u; }
    return h;
}

static void commit_pending(void) {
    static uint8_t payload[JRNL_MAX][BS];
    if (!pend_n) return;
    for (int i = 0; i < pend_n; i++)
        memcpy(payload[i], cache[pend_blk[i] % CACHE_N].buf, BS);
    for (int i = 0; i < pend_n; i++) dwrite(sb.jrnl_start + 1 + i, payload[i]);
    ata_flush(dev);

    struct jrnl_header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, JRNL_MAGIC, 8);
    h.count = (uint32_t)pend_n;
    uint32_t sum = 0;
    for (int i = 0; i < pend_n; i++) { h.target[i] = pend_blk[i]; sum = jrnl_csum(sum, payload[i]); }
    h.checksum = sum;
    ata_write_sectors(dev, sb.jrnl_start * SPB, 1, &h);
    ata_flush(dev);

    for (int i = 0; i < pend_n; i++) dwrite(pend_blk[i], payload[i]);
    ata_flush(dev);

    memset(&h, 0, sizeof(h));
    ata_write_sectors(dev, sb.jrnl_start * SPB, 1, &h);

    for (int i = 0; i < pend_n; i++) cache[pend_blk[i] % CACHE_N].dirty = 0;
    pend_n = 0;
}

static void jrnl_replay(void) {
    static uint8_t buf[JRNL_MAX][BS];
    struct jrnl_header h;
    if (ata_read_sectors(dev, sb.jrnl_start * SPB, 1, &h) < 0) return;
    if (memcmp(h.magic, JRNL_MAGIC, 8) != 0) return;
    if (h.count == 0 || h.count > JRNL_MAX) return;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < h.count; i++) {
        if (dread(sb.jrnl_start + 1 + i, buf[i]) < 0) return;
        sum = jrnl_csum(sum, buf[i]);
    }
    if (sum != h.checksum) return;
    for (uint32_t i = 0; i < h.count; i++) if (h.target[i] >= sb.total_blocks) return;
    for (uint32_t i = 0; i < h.count; i++) dwrite(h.target[i], buf[i]);
    ata_flush(dev);
    memset(&h, 0, sizeof(h));
    ata_write_sectors(dev, sb.jrnl_start * SPB, 1, &h);
    ata_flush(dev);
}

/* metadata cache: direct-mapped, write-through via a small write-ahead journal. */
static uint8_t *cget(uint32_t b) {
    struct cslot *s = &cache[b % CACHE_N];
    if (s->valid && s->blk == b) return s->buf;
    if (s->valid && s->dirty) commit_pending();
    s->valid = 0;
    if (dread(b, s->buf) < 0) return 0;
    s->blk = b; s->valid = 1;
    return s->buf;
}
static int cput(uint32_t b) {
    struct cslot *s = &cache[b % CACHE_N];
    if (!s->dirty) {
        if (pend_n >= JRNL_MAX) commit_pending();
        pend_blk[pend_n++] = b;
        s->dirty = 1;
    }
    return 0;
}

static void cache_flush(void) {
    commit_pending();
}

static void sb_flush(void) {
    uint8_t *p = cget(0);
    if (!p) return;
    memcpy(p, &sb, sizeof(sb));
    cput(0);
    cache_flush();
}

/* ---- inodes ---- */
static int iread(uint32_t ino, struct vinode *in) {
    if (ino >= sb.inode_count) return -1;
    uint8_t *p = cget(sb.itab_start + ino / IPB);
    if (!p) return -1;
    memcpy(in, p + (ino % IPB) * INO_SIZE, sizeof(*in));
    return 0;
}
static int iwrite(uint32_t ino, const struct vinode *in) {
    if (ino >= sb.inode_count) return -1;
    uint32_t b = sb.itab_start + ino / IPB;
    uint8_t *p = cget(b);
    if (!p) return -1;
    memcpy(p + (ino % IPB) * INO_SIZE, in, sizeof(*in));
    return cput(b);
}

static uint32_t ialloc(uint8_t type) {
    if (!sb.free_inodes) return 0;
    for (uint32_t n = 0; n < sb.inode_count; n++) {
        uint32_t ino = 1 + (ino_hint - 1 + n) % (sb.inode_count - 1);
        struct vinode in;
        if (iread(ino, &in) < 0) return 0;
        if (in.type == 0) {
            memset(&in, 0, sizeof(in));
            in.type = type;
            in.mode = type == GATOFS_DIR ? 0755 : 0644;
            in.mtime = rtc_unix();
            struct task *ct = sched_current();
            in.uid = ct ? (uint16_t)ct->uid : 0;
            in.gid = ct ? (uint16_t)ct->gid : 0;
            if (iwrite(ino, &in) < 0) return 0;
            sb.free_inodes--;
            ino_hint = ino + 1;
            return ino;
        }
    }
    return 0;
}

/* ---- block bitmap ---- */
static int bm_set(uint32_t b, int v) {
    uint32_t bb = sb.bm_start + b / (BS * 8);
    uint8_t *p = cget(bb);
    if (!p) return -1;
    uint32_t bit = b % (BS * 8);
    if (v) p[bit >> 3] |= (uint8_t)(1u << (bit & 7));
    else p[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
    return cput(bb);
}

static uint32_t balloc_scan(uint32_t from, uint32_t to) {
    uint32_t b = from;
    while (b < to) {
        uint8_t *p = cget(sb.bm_start + b / (BS * 8));
        if (!p) return 0;
        uint32_t bit = b % (BS * 8), end = (BS * 8) - bit;
        if (end > to - b) end = to - b;
        for (uint32_t i = 0; i < end; ) {
            uint32_t bt = bit + i;
            if ((bt & 7) == 0 && p[bt >> 3] == 0xFF) { i += 8; continue; }
            if (!(p[bt >> 3] & (1u << (bt & 7)))) {
                uint32_t got = b + i;
                if (bm_set(got, 1) < 0) return 0;
                sb.free_blocks--;
                blk_hint = got + 1;
                return got;
            }
            i++;
        }
        b += end;
    }
    return 0;
}

static uint32_t balloc(void) {
    if (!sb.free_blocks) return 0;
    uint32_t h = blk_hint < sb.data_start || blk_hint >= sb.total_blocks ? sb.data_start : blk_hint;
    uint32_t r = balloc_scan(h, sb.total_blocks);
    return r ? r : balloc_scan(sb.data_start, h);
}

static void bfree(uint32_t b) {
    if (b < sb.data_start || b >= sb.total_blocks) return;
    if (bm_set(b, 0) == 0) sb.free_blocks++;
    if (b < blk_hint) blk_hint = b;
}

/* ---- block mapping ---- */
/* Returns slot value in table block `t`, allocating (zeroed table or data) if asked. */
static uint32_t slot(uint32_t t, uint32_t i, int alloc, int zero, struct vinode *in, int *isnew) {
    if (i >= PPB) return 0;
    uint8_t *p = cget(t);
    if (!p) return 0;
    uint32_t v = ((uint32_t *)p)[i];
    if (v || !alloc) return v;
    v = balloc();
    if (!v) return 0;
    if (zero) {
        uint8_t *q = cget(v);
        if (!q) return 0;
        memset(q, 0, BS);
        cput(v);
    } else if (isnew) *isnew = 1;
    p = cget(t);
    if (!p) return 0;
    ((uint32_t *)p)[i] = v;
    cput(t);
    in->nblocks++;
    return v;
}

static uint32_t topslot(struct vinode *in, int i, int alloc, int zero, int *isnew) {
    if (in->blk[i] || !alloc) return in->blk[i];
    uint32_t v = balloc();
    if (!v) return 0;
    if (zero) {
        uint8_t *q = cget(v);
        if (!q) return 0;
        memset(q, 0, BS);
        cput(v);
    } else if (isnew) *isnew = 1;
    in->blk[i] = v;
    in->nblocks++;
    return v;
}

static uint32_t bmap(struct vinode *in, uint32_t idx, int alloc, int *isnew) {
    if (isnew) *isnew = 0;
    if (idx < NDIRECT) return topslot(in, idx, alloc, 0, isnew);
    idx -= NDIRECT;
    if (idx < PPB) {
        uint32_t t = topslot(in, IND_SLOT, alloc, 1, 0);
        return t ? slot(t, idx, alloc, 0, in, isnew) : 0;
    }
    idx -= PPB;
    if (idx >= PPB * PPB) return 0;
    uint32_t t1 = topslot(in, DIND_SLOT, alloc, 1, 0);
    if (!t1) return 0;
    uint32_t t2 = slot(t1, idx / PPB, alloc, 1, in, 0);
    return t2 ? slot(t2, idx % PPB, alloc, 0, in, isnew) : 0;
}

static void free_table(uint32_t t, int depth) {
    for (uint32_t i = 0; i < PPB; i++) {
        uint8_t *p = cget(t);
        if (!p) return;
        uint32_t v = ((uint32_t *)p)[i];
        if (!v) continue;
        if (depth > 1) free_table(v, depth - 1);
        bfree(v);
    }
}

static void itrunc(struct vinode *in) {
    for (int i = 0; i < NDIRECT; i++) if (in->blk[i]) bfree(in->blk[i]);
    if (in->blk[IND_SLOT]) { free_table(in->blk[IND_SLOT], 1); bfree(in->blk[IND_SLOT]); }
    if (in->blk[DIND_SLOT]) { free_table(in->blk[DIND_SLOT], 2); bfree(in->blk[DIND_SLOT]); }
    memset(in->blk, 0, sizeof(in->blk));
    in->size = 0;
    in->nblocks = 0;
}

/* ---- file data ---- */
static int pread_i(struct vinode *in, uint32_t off, void *buf, uint32_t len) {
    if (off >= in->size) return 0;
    if (len > in->size - off) len = in->size - off;
    uint8_t tmp[BS];
    uint8_t *out = buf;
    uint32_t done = 0;
    while (done < len) {
        uint32_t o = off + done, bo = o % BS, n = BS - bo;
        if (n > len - done) n = len - done;
        uint32_t b = bmap(in, o / BS, 0, 0);
        if (!b) memset(out + done, 0, n);
        else if (n == BS) { if (dread(b, out + done) < 0) return done ? (int)done : GATOFS_EIO; }
        else {
            if (dread(b, tmp) < 0) return done ? (int)done : GATOFS_EIO;
            memcpy(out + done, tmp + bo, n);
        }
        done += n;
    }
    return (int)done;
}

/* writes and updates in->size; caller stores inode */
static int pwrite_i(struct vinode *in, uint32_t off, const void *buf, uint32_t len) {
    const uint8_t *src = buf;
    uint8_t tmp[BS];
    uint32_t done = 0;
    if (len > 0xFFFFFFFFu - off) len = 0xFFFFFFFFu - off;
    while (done < len) {
        uint32_t o = off + done, bo = o % BS, n = BS - bo;
        if (n > len - done) n = len - done;
        int isnew;
        uint32_t b = bmap(in, o / BS, 1, &isnew);
        if (!b) return done ? (int)done : GATOFS_ENOSPC;
        if (n == BS) {
            if (dwrite(b, src + done) < 0) return done ? (int)done : GATOFS_EIO;
        } else {
            if (isnew) memset(tmp, 0, BS);
            else if (dread(b, tmp) < 0) return done ? (int)done : GATOFS_EIO;
            memcpy(tmp + bo, src + done, n);
            if (dwrite(b, tmp) < 0) return done ? (int)done : GATOFS_EIO;
        }
        done += n;
        if (o + n > in->size) in->size = o + n;
    }
    return (int)done;
}

/* ---- directories ---- */
static int dir_find(uint32_t dino, const char *name, uint32_t *ino, uint8_t *type) {
    struct vinode d;
    if (iread(dino, &d) < 0) return GATOFS_EIO;
    struct vdirent buf[BS / DE_SIZE];
    for (uint32_t off = 0; off < d.size; off += BS) {
        int r = pread_i(&d, off, buf, BS);
        if (r < 0) return r;
        for (int i = 0; i < r / DE_SIZE; i++)
            if (buf[i].ino && strcmp(buf[i].name, name) == 0) {
                if (ino) *ino = buf[i].ino;
                if (type) *type = buf[i].type;
                return 0;
            }
    }
    return GATOFS_ENOENT;
}

static int dir_add(uint32_t dino, const char *name, uint32_t ino, uint8_t type) {
    struct vinode d;
    if (iread(dino, &d) < 0) return GATOFS_EIO;
    struct vdirent e;
    memset(&e, 0, sizeof(e));
    e.ino = ino; e.type = type;
    strlcpy(e.name, name, sizeof(e.name));
    struct vdirent buf[BS / DE_SIZE];
    uint32_t pos = d.size;
    for (uint32_t off = 0; off < d.size; off += BS) {
        int r = pread_i(&d, off, buf, BS);
        if (r < 0) return r;
        for (int i = 0; i < r / DE_SIZE; i++)
            if (!buf[i].ino) { pos = off + i * DE_SIZE; off = d.size; break; }
    }
    int w = pwrite_i(&d, pos, &e, DE_SIZE);
    if (w != DE_SIZE) return w < 0 ? w : GATOFS_ENOSPC;
    d.mtime = rtc_unix();
    return iwrite(dino, &d) < 0 ? GATOFS_EIO : 0;
}

static int dir_del(uint32_t dino, const char *name) {
    struct vinode d;
    if (iread(dino, &d) < 0) return GATOFS_EIO;
    struct vdirent buf[BS / DE_SIZE];
    for (uint32_t off = 0; off < d.size; off += BS) {
        int r = pread_i(&d, off, buf, BS);
        if (r < 0) return r;
        for (int i = 0; i < r / DE_SIZE; i++)
            if (buf[i].ino && strcmp(buf[i].name, name) == 0) {
                struct vdirent z;
                memset(&z, 0, sizeof(z));
                if (pwrite_i(&d, off + i * DE_SIZE, &z, DE_SIZE) != DE_SIZE) return GATOFS_EIO;
                d.mtime = rtc_unix();
                return iwrite(dino, &d) < 0 ? GATOFS_EIO : 0;
            }
    }
    return GATOFS_ENOENT;
}

static int dir_empty(uint32_t dino) {
    struct vinode d;
    if (iread(dino, &d) < 0) return 0;
    struct vdirent buf[BS / DE_SIZE];
    for (uint32_t off = 0; off < d.size; off += BS) {
        int r = pread_i(&d, off, buf, BS);
        if (r < 0) return 0;
        for (int i = 0; i < r / DE_SIZE; i++) if (buf[i].ino) return 0;
    }
    return 1;
}

/* ---- path resolution ---- */
/* Resolves path; if want_parent, stops before last component (copied to leaf). */
static int resolve(const char *path, int want_parent, uint32_t *out, char *leaf) {
    if (!mounted) return GATOFS_ENOMOUNT;
    uint32_t stack[32], depth = 0, cur = sb.root_ino;
    const char *p = path;
    char comp[GATOFS_NAME_MAX + 2];
    if (leaf) leaf[0] = 0;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;
        int n = 0;
        while (p[n] && p[n] != '/') n++;
        if (n > GATOFS_NAME_MAX) return GATOFS_ENAMETOOLONG;
        memcpy(comp, p, n); comp[n] = 0;
        p += n;
        const char *q = p;
        while (*q == '/') q++;
        int last = !*q;
        if (want_parent && last) {
            if (!strcmp(comp, ".") || !strcmp(comp, "..")) return GATOFS_EINVAL;
            strlcpy(leaf, comp, GATOFS_NAME_MAX + 1);
            break;
        }
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) { if (depth) cur = stack[--depth]; continue; }
        uint32_t ino; uint8_t type;
        int r = dir_find(cur, comp, &ino, &type);
        if (r < 0) return r;
        if (!last && type != GATOFS_DIR) return GATOFS_ENOTDIR;
        if (depth < 32) stack[depth++] = cur;
        cur = ino;
    }
    *out = cur;
    return 0;
}

/* ---- create / remove ---- */
static int create_node(const char *path, uint8_t type, uint32_t *ino_out) {
    uint32_t parent; char leaf[GATOFS_NAME_MAX + 1];
    int r = resolve(path, 1, &parent, leaf);
    if (r < 0) return r;
    if (!leaf[0]) return GATOFS_EINVAL;
    struct vinode pi;
    if (iread(parent, &pi) < 0) return GATOFS_EIO;
    if (pi.type != GATOFS_DIR) return GATOFS_ENOTDIR;
    if (dir_find(parent, leaf, 0, 0) == 0) return GATOFS_EEXIST;
    uint32_t ino = ialloc(type);
    if (!ino) return GATOFS_ENOSPC;
    r = dir_add(parent, leaf, ino, type);
    if (r < 0) {
        struct vinode z; memset(&z, 0, sizeof(z));
        iwrite(ino, &z); sb.free_inodes++;
        return r;
    }
    if (ino_out) *ino_out = ino;
    return 0;
}

static int remove_ino(uint32_t ino, int recursive);

static int rm_cb_collect(uint32_t dino, int recursive) {
    struct vinode d;
    if (iread(dino, &d) < 0) return GATOFS_EIO;
    for (uint32_t off = 0; off < d.size; off += DE_SIZE) {
        struct vdirent e;
        if (pread_i(&d, off, &e, DE_SIZE) != DE_SIZE) continue;
        if (!e.ino) continue;
        int r = remove_ino(e.ino, recursive);
        if (r < 0) return r;
    }
    return 0;
}

static int remove_ino(uint32_t ino, int recursive) {
    struct vinode in;
    if (iread(ino, &in) < 0) return GATOFS_EIO;
    if (in.type == GATOFS_DIR) {
        if (!recursive && !dir_empty(ino)) return GATOFS_ENOTEMPTY;
        int r = rm_cb_collect(ino, 1);
        if (r < 0) return r;
    }
    itrunc(&in);
    in.type = 0;
    if (iwrite(ino, &in) < 0) return GATOFS_EIO;
    sb.free_inodes++;
    if (ino < ino_hint) ino_hint = ino;
    return 0;
}

int gatofs_remove(const char *path, int recursive) {
    uint32_t parent, ino; char leaf[GATOFS_NAME_MAX + 1];
    int r = resolve(path, 1, &parent, leaf);
    if (r < 0) return r;
    if (!leaf[0]) return GATOFS_EINVAL;
    uint8_t type;
    r = dir_find(parent, leaf, &ino, &type);
    if (r < 0) return r;
    for (int i = 0; i < MAX_FD; i++) if (fds[i].used && fds[i].ino == ino) return GATOFS_EINVAL;
    if (type == GATOFS_DIR && !recursive && !dir_empty(ino)) return GATOFS_ENOTEMPTY;
    r = dir_del(parent, leaf);
    if (r < 0) return r;
    r = remove_ino(ino, recursive);
    sb_flush();
    return r;
}

int gatofs_mkdir(const char *path) {
    int r = create_node(path, GATOFS_DIR, 0);
    if (r == 0) sb_flush();
    return r;
}

int gatofs_mkdirs(const char *path) {
    char part[GATOFS_NAME_MAX * 4 + 8];
    if (strlen(path) >= sizeof(part)) return GATOFS_ENAMETOOLONG;
    for (size_t i = 1; ; i++) {
        if (path[i] == '/' || path[i] == 0) {
            memcpy(part, path, i); part[i] = 0;
            int r = gatofs_mkdir(part);
            if (r < 0 && r != GATOFS_EEXIST) return r;
            if (path[i] == 0) return 0;
        }
    }
}

int gatofs_rename(const char *from, const char *to) {
    uint32_t sp, dp, ino; char sl[GATOFS_NAME_MAX + 1], dl[GATOFS_NAME_MAX + 1];
    uint8_t type;
    int r = resolve(from, 1, &sp, sl);
    if (r < 0) return r;
    r = resolve(to, 1, &dp, dl);
    if (r < 0) return r;
    if (!sl[0] || !dl[0]) return GATOFS_EINVAL;
    r = dir_find(sp, sl, &ino, &type);
    if (r < 0) return r;
    if (dir_find(dp, dl, 0, 0) == 0) return GATOFS_EEXIST;
    if (type == GATOFS_DIR) {           /* refuse moving a dir into itself */
        size_t fl = strlen(from);
        while (fl > 1 && from[fl - 1] == '/') fl--;
        if (!strncmp(to, from, fl) && (to[fl] == '/' || to[fl] == 0)) return GATOFS_EINVAL;
    }
    r = dir_add(dp, dl, ino, type);
    if (r < 0) return r;
    r = dir_del(sp, sl);
    sb_flush();
    return r;
}

/* ---- fsck ---- */
static int rb_test(uint8_t *rb, uint32_t b) { return (rb[b >> 3] >> (b & 7)) & 1; }
static void rb_set(uint8_t *rb, uint32_t b) { rb[b >> 3] |= (uint8_t)(1u << (b & 7)); }

static void fsck_block(uint8_t *rb, uint32_t b, uint32_t ino, struct gatofs_fsck_result *res,
                        gatofs_fsck_cb cb, void *ctx) {
    char msg[96];
    if (b < sb.data_start || b >= sb.total_blocks) {
        res->errors++;
        snprintf(msg, sizeof(msg), "inode %u: block %u out of range", ino, b);
        cb(msg, ctx);
        return;
    }
    if (rb_test(rb, b)) {
        res->errors++;
        snprintf(msg, sizeof(msg), "inode %u: block %u already used (cross-linked)", ino, b);
        cb(msg, ctx);
        return;
    }
    rb_set(rb, b);
    res->blocks_checked++;
}

static void fsck_inode_blocks(uint8_t *rb, struct vinode *in, uint32_t ino,
                               struct gatofs_fsck_result *res, gatofs_fsck_cb cb, void *ctx) {
    for (int i = 0; i < NDIRECT; i++)
        if (in->blk[i]) fsck_block(rb, in->blk[i], ino, res, cb, ctx);
    if (in->blk[IND_SLOT]) {
        fsck_block(rb, in->blk[IND_SLOT], ino, res, cb, ctx);
        uint8_t *p = cget(in->blk[IND_SLOT]);
        if (p) {
            uint32_t e[PPB];
            memcpy(e, p, BS);
            for (uint32_t i = 0; i < PPB; i++) if (e[i]) fsck_block(rb, e[i], ino, res, cb, ctx);
        }
    }
    if (in->blk[DIND_SLOT]) {
        fsck_block(rb, in->blk[DIND_SLOT], ino, res, cb, ctx);
        uint8_t *p = cget(in->blk[DIND_SLOT]);
        if (p) {
            uint32_t t1[PPB];
            memcpy(t1, p, BS);
            for (uint32_t i = 0; i < PPB; i++) {
                if (!t1[i]) continue;
                fsck_block(rb, t1[i], ino, res, cb, ctx);
                uint8_t *q = cget(t1[i]);
                if (!q) continue;
                uint32_t t2[PPB];
                memcpy(t2, q, BS);
                for (uint32_t j = 0; j < PPB; j++) if (t2[j]) fsck_block(rb, t2[j], ino, res, cb, ctx);
            }
        }
    }
}

static void fsck_dir(struct vinode *d, uint32_t dino, struct gatofs_fsck_result *res,
                      gatofs_fsck_cb cb, void *ctx, int repair) {
    struct vdirent buf[BS / DE_SIZE];
    char msg[96];
    for (uint32_t off = 0; off < d->size; off += BS) {
        int n = pread_i(d, off, buf, BS);
        if (n <= 0) continue;
        int changed = 0;
        for (int i = 0; i < n / DE_SIZE; i++) {
            if (!buf[i].ino) continue;
            struct vinode t;
            int bad = buf[i].ino >= sb.inode_count || iread(buf[i].ino, &t) < 0 || t.type == 0;
            if (!bad) continue;
            res->errors++;
            snprintf(msg, sizeof(msg), "dir %u: entry '%s' -> invalid inode %u", dino, buf[i].name, buf[i].ino);
            cb(msg, ctx);
            if (repair) { memset(&buf[i], 0, sizeof(buf[i])); changed = 1; res->fixed++; }
        }
        if (changed) pwrite_i(d, off, buf, BS);
    }
}

int gatofs_fsck(int repair, gatofs_fsck_cb cb, void *ctx, struct gatofs_fsck_result *res) {
    if (!mounted) return GATOFS_ENOMOUNT;
    memset(res, 0, sizeof(*res));
    uint8_t *rb = kmalloc(sb.bm_blocks * BS);
    if (!rb) return GATOFS_ENOSPC;
    memset(rb, 0, sb.bm_blocks * BS);
    for (uint32_t b = 0; b < sb.data_start; b++) rb_set(rb, b);

    char msg[96];
    for (uint32_t ino = 1; ino < sb.inode_count; ino++) {
        struct vinode in;
        if (iread(ino, &in) < 0) continue;
        if (in.type == 0) continue;
        if (in.type != GATOFS_FILE && in.type != GATOFS_DIR) {
            res->errors++;
            snprintf(msg, sizeof(msg), "inode %u: invalid type %u", ino, in.type);
            cb(msg, ctx);
            continue;
        }
        res->inodes_checked++;
        fsck_inode_blocks(rb, &in, ino, res, cb, ctx);
        if (in.type == GATOFS_DIR) fsck_dir(&in, ino, res, cb, ctx, repair);
    }

    uint32_t free_count = 0;
    for (uint32_t bb = sb.bm_start; bb < sb.itab_start; bb++) {
        uint8_t *p = cget(bb);
        if (!p) continue;
        uint32_t base = (bb - sb.bm_start) * BS * 8;
        int changed = 0;
        for (uint32_t i = 0; i < BS && base + i * 8 < sb.total_blocks; i++) {
            for (int j = 0; j < 8; j++) {
                uint32_t bit = base + i * 8 + j;
                if (bit >= sb.total_blocks) break;
                int disk_used = (p[i] >> j) & 1;
                int should_use = rb_test(rb, bit);
                if (disk_used != should_use) {
                    res->errors++;
                    snprintf(msg, sizeof(msg), "block %u marked %s on disk, should be %s",
                             bit, disk_used ? "used" : "free", should_use ? "used" : "free");
                    cb(msg, ctx);
                    if (repair) {
                        if (should_use) p[i] |= (uint8_t)(1u << j);
                        else p[i] &= (uint8_t)~(1u << j);
                        changed = 1;
                        res->fixed++;
                    }
                }
                if (!should_use) free_count++;
            }
        }
        if (changed) cput(bb);
    }

    if (sb.free_blocks != free_count) {
        res->errors++;
        snprintf(msg, sizeof(msg), "free block count %u on disk, should be %u", sb.free_blocks, free_count);
        cb(msg, ctx);
        if (repair) { sb.free_blocks = free_count; res->fixed++; }
    }

    uint32_t free_ino = 0;
    for (uint32_t ino = 1; ino < sb.inode_count; ino++) {
        struct vinode in;
        if (iread(ino, &in) == 0 && in.type == 0) free_ino++;
    }
    if (sb.free_inodes != free_ino) {
        res->errors++;
        snprintf(msg, sizeof(msg), "free inode count %u on disk, should be %u", sb.free_inodes, free_ino);
        cb(msg, ctx);
        if (repair) { sb.free_inodes = free_ino; res->fixed++; }
    }

    if (repair) sb_flush();
    kfree(rb);
    return 0;
}

/* ---- info / stat / readdir ---- */
int gatofs_stat(const char *path, struct gatofs_stat *st) {
    uint32_t ino;
    int r = resolve(path, 0, &ino, 0);
    if (r < 0) return r;
    struct vinode in;
    if (iread(ino, &in) < 0) return GATOFS_EIO;
    st->type = (uint8_t)in.type; st->mode = in.mode; st->size = in.size;
    st->mtime = in.mtime; st->blocks = in.nblocks; st->ino = ino;
    st->uid = in.uid; st->gid = in.gid;
    return 0;
}

int gatofs_readdir(const char *path, gatofs_dir_cb cb, void *ctx) {
    uint32_t dino;
    int r = resolve(path, 0, &dino, 0);
    if (r < 0) return r;
    struct vinode d;
    if (iread(dino, &d) < 0) return GATOFS_EIO;
    if (d.type != GATOFS_DIR) return GATOFS_ENOTDIR;
    struct vdirent *buf = kmalloc(BS);
    if (!buf) return GATOFS_ENOSPC;
    for (uint32_t off = 0; off < d.size; off += BS) {
        int n = pread_i(&d, off, buf, BS);
        if (n < 0) { kfree(buf); return n; }
        for (int i = 0; i < n / DE_SIZE; i++)
            if (buf[i].ino && cb(buf[i].name, buf[i].type, buf[i].ino, ctx)) { kfree(buf); return 0; }
    }
    kfree(buf);
    return 0;
}

int gatofs_info(struct gatofs_info *o) {
    if (!mounted) return GATOFS_ENOMOUNT;
    strlcpy(o->label, sb.label, sizeof(o->label));
    o->total_blocks = sb.total_blocks; o->free_blocks = sb.free_blocks;
    o->total_inodes = sb.inode_count; o->free_inodes = sb.free_inodes;
    strlcpy(o->dev, dev->name, sizeof(o->dev));
    return 0;
}

int gatofs_mounted(void) { return mounted; }
void gatofs_sync(void) { if (mounted) sb_flush(); }

/* ---- format / mount ---- */
int gatofs_format(struct ata_device *d, const char *label) {
    if (!d || d->type != ATA_TYPE_ATA) return GATOFS_EINVAL;
    uint32_t total = d->sectors / SPB;
    if (total > (1u << 25)) total = 1u << 25;      /* LBA28 limit: 128 GB */
    if (total < 64) return GATOFS_EINVAL;
    if (mounted) gatofs_unmount();
    if (!cache) {
        cache = kzalloc(sizeof(struct cslot) * CACHE_N);
        if (!cache) return GATOFS_ENOSPC;
    }
    for (int i = 0; i < CACHE_N; i++) cache[i].valid = cache[i].dirty = 0;
    dev = d;
    bcache_invalidate(dev);
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, MAGIC, 8);
    sb.version = 1; sb.block_size = BS; sb.total_blocks = total;
    sb.inode_count = total / 16 < 1024 ? 1024 : total / 16;
    sb.inode_count = (sb.inode_count + IPB - 1) / IPB * IPB;
    sb.jrnl_start = 1;
    sb.jrnl_blocks = 1 + JRNL_MAX;
    sb.bm_start = sb.jrnl_start + sb.jrnl_blocks;
    sb.bm_blocks = (total + BS * 8 - 1) / (BS * 8);
    sb.itab_start = sb.bm_start + sb.bm_blocks;
    sb.itab_blocks = sb.inode_count / IPB;
    sb.data_start = sb.itab_start + sb.itab_blocks;
    if (sb.data_start + 16 > total) return GATOFS_EINVAL;
    sb.root_ino = 1;
    if (label) strlcpy(sb.label, label, sizeof(sb.label));
    else strlcpy(sb.label, "GatoFS", sizeof(sb.label));

    static uint8_t zero[BS];
    memset(zero, 0, BS);
    for (uint32_t b = sb.jrnl_start; b < sb.data_start; b++)
        if (dwrite(b, zero) < 0) return GATOFS_EIO;
    for (int i = 0; i < CACHE_N; i++) cache[i].valid = cache[i].dirty = 0;
    pend_n = 0;
    sb.free_blocks = total - sb.data_start;
    sb.free_inodes = sb.inode_count - 2;
    mounted = 1;                         /* needed by helpers below */
    for (uint32_t b = 0; b < sb.data_start; b++) if (bm_set(b, 1) < 0) { mounted = 0; return GATOFS_EIO; }
    struct vinode root;
    memset(&root, 0, sizeof(root));
    root.type = GATOFS_DIR; root.mode = 0755; root.mtime = rtc_unix();
    if (iwrite(1, &root) < 0) { mounted = 0; return GATOFS_EIO; }
    blk_hint = sb.data_start; ino_hint = 2;
    memset(fds, 0, sizeof(fds));
    sb_flush();
    return 0;
}

int gatofs_mount(struct ata_device *d) {
    if (!d || d->type != ATA_TYPE_ATA) return GATOFS_EINVAL;
    if (!cache) {
        cache = kzalloc(sizeof(struct cslot) * CACHE_N);
        if (!cache) return GATOFS_ENOSPC;
    }
    uint8_t *tmp = kmalloc(BS);
    if (!tmp) return GATOFS_ENOSPC;
    struct ata_device *old = dev;
    dev = d;
    bcache_invalidate(dev);
    int ok = dread(0, tmp) == 0 && memcmp(tmp, MAGIC, 8) == 0;
    if (ok) {
        memcpy(&sb, tmp, sizeof(sb));
        ok = sb.block_size == BS && sb.total_blocks <= d->sectors / SPB && sb.root_ino == 1;
    }
    kfree(tmp);
    if (!ok) { dev = old; return GATOFS_EINVAL; }
    jrnl_replay();
    for (int i = 0; i < CACHE_N; i++) cache[i].valid = cache[i].dirty = 0;
    pend_n = 0;
    memset(fds, 0, sizeof(fds));
    blk_hint = sb.data_start; ino_hint = 2;
    mounted = 1;
    return 0;
}

void gatofs_unmount(void) {
    if (!mounted) return;
    sb_flush();
    bcache_invalidate(dev);
    mounted = 0;
}

int gatofs_probe(void) {
    for (int i = 0; i < ata_device_count(); i++)
        if (gatofs_mount(ata_get_device(i)) == 0) return 0;
    return -1;
}

static int disk_is_blank(struct ata_device *d) {
    uint8_t *buf = kmalloc(BS);
    if (!buf) return 0;
    int blank = 1;
    for (uint32_t lba = 0; blank && lba < 2048; lba += SPB) {
        if (ata_read_sectors(d, lba, SPB, buf) < 0) { blank = 0; break; }
        for (uint32_t i = 0; i < BS; i++)
            if (buf[i]) { blank = 0; break; }
    }
    kfree(buf);
    return blank;
}

int gatofs_autoformat(void) {
    struct ata_device *best = 0;
    for (int i = 0; i < ata_device_count(); i++) {
        struct ata_device *d = ata_get_device(i);
        if (d->type != ATA_TYPE_ATA || d->sectors < 2048) continue;
        if (best && d->sectors <= best->sectors) continue;
        if (disk_is_blank(d)) best = d;
    }
    if (!best) return GATOFS_ENOENT;
    return gatofs_format(best, 0);
}

/* ---- file handles ---- */
/* Open file by inode number (for writeback) */
int gatofs_open_by_ino(uint32_t ino, int flags) {
    int fd = -1;
    for (int i = 0; i < MAX_FD; i++) if (!fds[i].used) { fd = i; break; }
    if (fd < 0) return GATOFS_ENOSPC;

    struct vinode in;
    if (iread(ino, &in) < 0) return GATOFS_EIO;
    if (in.type == GATOFS_DIR) return GATOFS_EISDIR;
    if ((flags & VF_TRUNC) && (flags & VF_WRITE)) {
        itrunc(&in);
        in.mtime = rtc_unix();
        if (iwrite(ino, &in) < 0) return GATOFS_EIO;
    }
    fds[fd].used = 1; fds[fd].ino = ino; fds[fd].flags = flags;
    fds[fd].pos = (flags & VF_APPEND) ? in.size : 0;
    return fd;
}

int gatofs_open(const char *path, int flags) {
    int fd = -1;
    for (int i = 0; i < MAX_FD; i++) if (!fds[i].used) { fd = i; break; }
    if (fd < 0) return GATOFS_ENOSPC;
    uint32_t ino;
    int changed = 0;
    int r = resolve(path, 0, &ino, 0);
    if (r == GATOFS_ENOENT && (flags & VF_CREATE)) {
        r = create_node(path, GATOFS_FILE, &ino);
        changed = 1;
    }
    if (r < 0) return r;
    struct vinode in;
    if (iread(ino, &in) < 0) return GATOFS_EIO;
    if (in.type == GATOFS_DIR) return GATOFS_EISDIR;
    if ((flags & VF_TRUNC) && (flags & VF_WRITE)) {
        itrunc(&in);
        in.mtime = rtc_unix();
        if (iwrite(ino, &in) < 0) return GATOFS_EIO;
        changed = 1;
    }
    fds[fd].used = 1; fds[fd].ino = ino; fds[fd].flags = flags;
    fds[fd].pos = (flags & VF_APPEND) ? in.size : 0;
    if (changed) sb_flush();
    return fd;
}

static struct vfd *getfd(int fd) {
    if (fd < 0 || fd >= MAX_FD || !fds[fd].used) return 0;
    return &fds[fd];
}

int gatofs_read(int fd, void *buf, uint32_t len) {
    struct vfd *f = getfd(fd);
    if (!f || !(f->flags & VF_READ)) return GATOFS_EINVAL;
    struct vinode in;
    if (iread(f->ino, &in) < 0) return GATOFS_EIO;
    int r = pread_i(&in, f->pos, buf, len);
    if (r > 0) f->pos += r;
    return r;
}

int gatofs_write(int fd, const void *buf, uint32_t len) {
    struct vfd *f = getfd(fd);
    if (!f || !(f->flags & VF_WRITE)) return GATOFS_EINVAL;
    struct vinode in;
    if (iread(f->ino, &in) < 0) return GATOFS_EIO;
    if (f->flags & VF_APPEND) f->pos = in.size;
    int r = pwrite_i(&in, f->pos, buf, len);
    if (r > 0) { f->pos += r; }
    in.mtime = rtc_unix();
    if (iwrite(f->ino, &in) < 0) return GATOFS_EIO;
    sb_flush();
    return r;
}

/* Frees one data block and clears its map entry. bfree() touches the block
 * bitmap through the metadata cache, so any buffer held across the call has
 * to be re-fetched with cget() afterwards. */
static void unmap_i(struct vinode *in, uint32_t idx) {
    if (idx < NDIRECT) {
        if (in->blk[idx]) {
            bfree(in->blk[idx]);
            in->blk[idx] = 0;
            in->nblocks--;
        }
        return;
    }
    idx -= NDIRECT;
    if (idx < PPB) {
        uint32_t t = in->blk[IND_SLOT];
        if (!t) return;
        uint8_t *p = cget(t);
        if (!p) return;
        uint32_t b = ((const uint32_t *)p)[idx];
        if (!b) return;
        bfree(b);
        p = cget(t);
        if (!p) return;
        ((uint32_t *)p)[idx] = 0;
        cput(t);
        in->nblocks--;
        return;
    }
    idx -= PPB;
    if (idx >= PPB * PPB) return;
    uint32_t t1 = in->blk[DIND_SLOT];
    if (!t1) return;
    uint8_t *q = cget(t1);
    if (!q) return;
    uint32_t t2 = ((const uint32_t *)q)[idx / PPB];
    if (!t2) return;
    uint8_t *p = cget(t2);
    if (!p) return;
    uint32_t b = ((const uint32_t *)p)[idx % PPB];
    if (!b) return;
    bfree(b);
    p = cget(t2);
    if (!p) return;
    ((uint32_t *)p)[idx % PPB] = 0;
    cput(t2);
    in->nblocks--;
}

/* True when an indirect block holds no live pointers, which means the block
 * itself can go back to the free list. */
static int table_empty(uint32_t t) {
    uint8_t *p = cget(t);
    if (!p) return 0;
    const uint32_t *e = (const uint32_t *)p;
    for (uint32_t i = 0; i < PPB; i++) {
        if (e[i]) return 0;
    }
    return 1;
}

int gatofs_truncate(int fd, uint32_t size) {
    struct vfd *f = getfd(fd);
    if (!f || !(f->flags & VF_WRITE)) return GATOFS_EINVAL;

    struct vinode in;
    if (iread(f->ino, &in) < 0) return GATOFS_EIO;
    if (in.type == GATOFS_DIR) return GATOFS_EISDIR;

    if (size < in.size) {
        /* Release every block that now sits entirely past the new end. A
         * block that still holds a surviving byte is kept, so shrinking
         * inside a block does not throw it away and reallocate it. */
        uint32_t keep = (size + BS - 1) / BS;
        uint32_t had = (in.size + BS - 1) / BS;
        for (uint32_t i = keep; i < had; i++) {
            unmap_i(&in, i);
        }
        if (in.blk[IND_SLOT] && table_empty(in.blk[IND_SLOT])) {
            bfree(in.blk[IND_SLOT]);
            in.blk[IND_SLOT] = 0;
        }
        if (in.blk[DIND_SLOT] && table_empty(in.blk[DIND_SLOT])) {
            bfree(in.blk[DIND_SLOT]);
            in.blk[DIND_SLOT] = 0;
        }
    }
    /* Growing just moves the end: pread_i() already answers unallocated
     * blocks with zeroes, so the hole costs nothing until it is written. */
    in.size = size;
    in.mtime = rtc_unix();
    if (iwrite(f->ino, &in) < 0) return GATOFS_EIO;
    if (f->pos > in.size) f->pos = in.size;
    sb_flush();
    return 0;
}

int gatofs_seek(int fd, uint32_t pos) {
    struct vfd *f = getfd(fd);
    if (!f) return GATOFS_EINVAL;
    f->pos = pos;
    return 0;
}
uint32_t gatofs_tell(int fd) { struct vfd *f = getfd(fd); return f ? f->pos : 0; }
uint32_t gatofs_size(int fd) {
    struct vfd *f = getfd(fd);
    struct vinode in;
    if (!f || iread(f->ino, &in) < 0) return 0;
    return in.size;
}
int gatofs_close(int fd) {
    struct vfd *f = getfd(fd);
    if (!f) return GATOFS_EINVAL;
    f->used = 0;
    return 0;
}

int gatofs_load(const char *path, void **buf, uint32_t *size) {
    int fd = gatofs_open(path, VF_READ);
    if (fd < 0) return fd;
    uint32_t sz = gatofs_size(fd);
    uint8_t *m = kmalloc(sz ? sz : 1);
    if (!m) { gatofs_close(fd); return GATOFS_ENOSPC; }
    uint32_t got = 0;
    while (got < sz) {
        uint32_t chunk = sz - got > (1u << 20) ? (1u << 20) : sz - got;
        int r = gatofs_read(fd, m + got, chunk);
        if (r <= 0) { kfree(m); gatofs_close(fd); return r < 0 ? r : GATOFS_EIO; }
        got += r;
    }
    gatofs_close(fd);
    *buf = m; *size = sz;
    return 0;
}

int gatofs_save(const char *path, const void *buf, uint32_t size) {
    int fd = gatofs_open(path, VF_WRITE | VF_CREATE | VF_TRUNC);
    if (fd < 0) return fd;
    uint32_t put = 0;
    while (put < size) {
        uint32_t chunk = size - put > (1u << 20) ? (1u << 20) : size - put;
        int r = gatofs_write(fd, (const uint8_t *)buf + put, chunk);
        if (r <= 0) { gatofs_close(fd); return r < 0 ? r : GATOFS_ENOSPC; }
        put += r;
    }
    gatofs_close(fd);
    return 0;
}

int gatofs_chmod(const char *path, uint16_t mode) {
    uint32_t ino;
    int r = resolve(path, 0, &ino, 0);
    if (r < 0) return r;
    struct vinode in;
    if (iread(ino, &in) < 0) return GATOFS_EIO;
    in.mode = mode & 0777;
    if (iwrite(ino, &in) < 0) return GATOFS_EIO;
    sb_flush();
    return 0;
}

int gatofs_chown(const char *path, uint16_t uid, uint16_t gid) {
    uint32_t ino;
    int r = resolve(path, 0, &ino, 0);
    if (r < 0) return r;
    struct vinode in;
    if (iread(ino, &in) < 0) return GATOFS_EIO;
    if (uid != 0xFFFF) in.uid = uid;
    if (gid != 0xFFFF) in.gid = gid;
    if (iwrite(ino, &in) < 0) return GATOFS_EIO;
    sb_flush();
    return 0;
}

int gatofs_touch(const char *path) {
    uint32_t ino;
    int r = resolve(path, 0, &ino, 0);
    if (r < 0) return r;
    struct vinode in;
    if (iread(ino, &in) < 0) return GATOFS_EIO;
    in.mtime = rtc_unix();
    if (iwrite(ino, &in) < 0) return GATOFS_EIO;
    sb_flush();
    return 0;
}
