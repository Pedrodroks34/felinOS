#include "sh/cmds.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/format.h"
#include "drivers/rtc.h"

void cmd_error(struct stream *out, const char *cmd, const char *target, const char *msg) {
    if (target) {
        st_printf(stream_console(), "%s: %s: %s\n", cmd, target, msg);
    } else {
        st_printf(stream_console(), "%s: %s\n", cmd, msg);
    }
}

void cmd_vfs_error(struct stream *out, const char *cmd, const char *target, int err) {
    cmd_error(out, cmd, target, vfs_strerror(err));
}

int cmd_is_flag(const char *arg) {
    return arg[0] == '-' && arg[1] != '\0';
}

int cmd_has_flag(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            return 1;
        }
        if (argv[i][0] == '-' && argv[i][1] != '-' && flag[0] == '-' && flag[1] != '-') {
            if (strchr(argv[i] + 1, flag[1])) {
                return 1;
            }
        }
    }
    return 0;
}

char *cmd_collect_input(int argc, char **argv, int start, struct stream *in,
                        struct stream *out, const char *cmdname, uint32_t *len_out) {
    int files = 0;

    for (int i = start; i < argc; i++) {
        if (!cmd_is_flag(argv[i])) {
            files++;
        }
    }

    if (files == 0) {
        return st_read_all(in ? in : stream_console(), len_out);
    }

    uint32_t cap = 256;
    uint32_t used = 0;
    char *buffer = (char *)kmalloc(cap);
    if (!buffer) {
        if (len_out) {
            *len_out = 0;
        }
        return NULL;
    }
    buffer[0] = '\0';

    for (int i = start; i < argc; i++) {
        if (cmd_is_flag(argv[i])) {
            continue;
        }
        void *data;
        uint32_t size;
        int r = vfs_load(argv[i], &data, &size);
        if (r < 0) {
            cmd_vfs_error(out, cmdname, argv[i], r);
            continue;
        }
        if (size) {
            uint32_t need = used + size + 2;
            if (need > cap) {
                uint32_t ncap = cap * 2 > need ? cap * 2 : need;
                char *grown = (char *)krealloc(buffer, ncap);
                if (!grown) {
                    kfree(data);
                    kfree(buffer);
                    if (len_out) {
                        *len_out = 0;
                    }
                    return NULL;
                }
                buffer = grown;
                cap = ncap;
            }
            memcpy(buffer + used, data, size);
            used += size;
            if (buffer[used - 1] != '\n') {
                buffer[used++] = '\n';
            }
        }
        kfree(data);
    }

    buffer[used] = '\0';
    if (len_out) {
        *len_out = used;
    }
    return buffer;
}

int cmd_split_lines(char *text, char ***lines_out) {
    uint32_t count = 1;
    for (char *p = text; *p; p++) {
        if (*p == '\n') {
            count++;
        }
    }

    char **lines = (char **)kmalloc(sizeof(char *) * (count + 1));
    if (!lines) {
        *lines_out = NULL;
        return 0;
    }

    int n = 0;
    char *start = text;
    for (char *p = text;; p++) {
        if (*p == '\n' || *p == '\0') {
            int last = (*p == '\0');
            *p = '\0';
            lines[n++] = start;
            start = p + 1;
            if (last) {
                break;
            }
        }
    }

    if (n > 0 && lines[n - 1][0] == '\0') {
        n--;
    }

    lines[n] = NULL;
    *lines_out = lines;
    return n;
}

void cmd_format_size64(uint64_t bytes, char *buf, uint32_t size) {
    const uint64_t k = 1024ull;
    const uint64_t m = k * 1024ull;
    const uint64_t g = m * 1024ull;

    if (bytes >= g) {
        snprintf(buf, size, "%u.%uG", (uint32_t)(bytes / g), (uint32_t)((bytes % g) / 107374182ull));
    } else if (bytes >= m) {
        snprintf(buf, size, "%u.%uM", (uint32_t)(bytes / m), (uint32_t)((bytes % m) / 104857ull));
    } else if (bytes >= k) {
        snprintf(buf, size, "%u.%uK", (uint32_t)(bytes / k), (uint32_t)((bytes % k) / 102ull));
    } else {
        snprintf(buf, size, "%uB", (uint32_t)bytes);
    }
}

void cmd_format_size(uint32_t bytes, char *buf, uint32_t size) {
    cmd_format_size64(bytes, buf, size);
}

void cmd_format_time(uint32_t stamp, char *buf, uint32_t size) {
    struct rtc_time t;
    unix_to_time(stamp, &t);
    snprintf(buf, size, "%04u-%02u-%02u %02u:%02u",
             t.year, t.month, t.day, t.hour, t.minute);
}

void cmd_mode_string(uint8_t type, uint16_t mode, char *buf) {
    static const char *rwx[8] = { "---", "--x", "-w-", "-wx",
                                  "r--", "r-x", "rw-", "rwx" };
    switch (type) {
    case VFS_DIR: buf[0] = 'd'; break;
    case VFS_CHR: buf[0] = 'c'; break;
    case VFS_BLK: buf[0] = 'b'; break;
    default:      buf[0] = '-'; break;
    }
    strcpy(buf + 1, rwx[(mode >> 6) & 7]);
    strcpy(buf + 4, rwx[(mode >> 3) & 7]);
    strcpy(buf + 7, rwx[mode & 7]);
    buf[10] = '\0';
}
