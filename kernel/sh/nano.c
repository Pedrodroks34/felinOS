#include "sh/cmds.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/format.h"
#include "fs/gatofs.h"
#include "drivers/screen.h"
#include "drivers/vga.h"
#include "drivers/input.h"
#include "console.h"

#define EDITOR_ROWS     21
#define TEXT_TOP        1
#define STATUS_ROW      22
#define HELP_ROW_A      23
#define HELP_ROW_B      24
#define LINE_CAPACITY   512

struct editor {
    char **lines;
    int count;
    int capacity;
    int cx;
    int cy;
    int row_offset;
    int col_offset;
    int modified;
    int ram_only;
    char filename[VFS_PATH_MAX];
    char status[80];
    char clipboard[LINE_CAPACITY];
};

static int editor_grow(struct editor *e, int needed) {
    if (e->capacity >= needed) {
        return 0;
    }
    int cap = e->capacity ? e->capacity : 32;
    while (cap < needed) {
        cap *= 2;
    }
    char **lines = (char **)krealloc(e->lines, sizeof(char *) * cap);
    if (!lines) {
        return -1;
    }
    e->lines = lines;
    e->capacity = cap;
    return 0;
}

static char *line_new(const char *text) {
    char *line = (char *)kmalloc(LINE_CAPACITY);
    if (!line) {
        return NULL;
    }
    strlcpy(line, text ? text : "", LINE_CAPACITY);
    return line;
}

static int editor_insert_line(struct editor *e, int index, const char *text) {
    if (editor_grow(e, e->count + 1) < 0) {
        return -1;
    }
    for (int i = e->count; i > index; i--) {
        e->lines[i] = e->lines[i - 1];
    }
    e->lines[index] = line_new(text);
    if (!e->lines[index]) {
        return -1;
    }
    e->count++;
    return 0;
}

static void editor_remove_line(struct editor *e, int index) {
    if (index < 0 || index >= e->count) {
        return;
    }
    kfree(e->lines[index]);
    for (int i = index; i < e->count - 1; i++) {
        e->lines[i] = e->lines[i + 1];
    }
    e->count--;
}

static void editor_load(struct editor *e, const uint8_t *data, uint32_t size) {
    if (!data || size == 0) {
        editor_insert_line(e, 0, "");
        return;
    }

    char line[LINE_CAPACITY];
    uint32_t pos = 0;

    while (pos < size) {
        uint32_t len = 0;
        while (pos < size && data[pos] != '\n' && len < LINE_CAPACITY - 1) {
            line[len++] = (char)data[pos++];
        }
        line[len] = '\0';
        while (pos < size && data[pos] != '\n') {
            pos++;
        }
        if (pos < size) {
            pos++;
        }
        editor_insert_line(e, e->count, line);
    }

    if (e->count == 0) {
        editor_insert_line(e, 0, "");
    }
}

static int editor_save(struct editor *e) {
    uint32_t total = 0;
    for (int i = 0; i < e->count; i++) {
        total += (uint32_t)strlen(e->lines[i]) + 1;
    }

    char *buf = (char *)kmalloc(total ? total : 1);
    if (!buf) {
        snprintf(e->status, sizeof(e->status), "Cannot write %s: out of memory", e->filename);
        return -1;
    }

    uint32_t pos = 0;
    for (int i = 0; i < e->count; i++) {
        uint32_t len = (uint32_t)strlen(e->lines[i]);
        memcpy(buf + pos, e->lines[i], len);
        pos += len;
        buf[pos++] = '\n';
    }

    int result = vfs_save(e->filename, buf, total);
    kfree(buf);

    if (result < 0) {
        snprintf(e->status, sizeof(e->status), "Cannot write %s: %s", e->filename, vfs_strerror(result));
        return -1;
    }

    const char *fs = vfs_fs_of(e->filename);
    e->modified = 0;
    e->ram_only = fs && strcmp(fs, "ramfs") == 0;
    if (e->ram_only) {
        snprintf(e->status, sizeof(e->status), "Wrote %d lines, %u bytes to %s (RAM only, not on disk)",
                 e->count, total, e->filename);
    } else {
        snprintf(e->status, sizeof(e->status), "Wrote %d lines, %u bytes to %s",
                 e->count, total, e->filename);
    }
    return 0;
}

static void editor_draw(struct editor *e) {
    char buf[128];

    snprintf(buf, sizeof(buf), "  FelinOS nano   %s%s",
             e->filename[0] ? e->filename : "new buffer",
             e->modified ? "   [modified]" : "");
    screen_line(0, buf, 0x70);

    for (int row = 0; row < EDITOR_ROWS; row++) {
        int index = e->row_offset + row;
        if (index >= e->count) {
            screen_line(TEXT_TOP + row, "", 0x07);
            continue;
        }
        const char *line = e->lines[index];
        int len = (int)strlen(line);
        if (e->col_offset < len) {
            screen_line(TEXT_TOP + row, line + e->col_offset, 0x07);
        } else {
            screen_line(TEXT_TOP + row, "", 0x07);
        }
    }

    if (e->status[0]) {
        screen_line(STATUS_ROW, e->status, 0x0E);
    } else {
        snprintf(buf, sizeof(buf), "  line %d/%d   column %d",
                 e->cy + 1, e->count, e->cx + 1);
        screen_line(STATUS_ROW, buf, 0x08);
    }

    screen_line(HELP_ROW_A, "^O Write out   ^X Exit        ^K Cut line    ^U Paste line", 0x70);
    screen_line(HELP_ROW_B, "^W Search      ^A Line start  ^E Line end    ^G Help", 0x70);

    screen_cursor((size_t)(e->cx - e->col_offset), (size_t)(TEXT_TOP + e->cy - e->row_offset));
}

static void editor_scroll(struct editor *e) {
    if (e->cy < e->row_offset) {
        e->row_offset = e->cy;
    }
    if (e->cy >= e->row_offset + EDITOR_ROWS) {
        e->row_offset = e->cy - EDITOR_ROWS + 1;
    }
    if (e->cx < e->col_offset) {
        e->col_offset = e->cx;
    }
    if (e->cx >= e->col_offset + 80) {
        e->col_offset = e->cx - 79;
    }
}

static int editor_prompt(struct editor *e, const char *message, char *buf, int size) {
    int len = 0;
    buf[0] = '\0';

    for (;;) {
        char line[128];
        snprintf(line, sizeof(line), "%s%s", message, buf);
        screen_line(STATUS_ROW, line, 0x70);
        screen_cursor(strlen(message) + (size_t)len, STATUS_ROW);

        int key = input_getkey();
        if (key == '\n') {
            return len;
        }
        if (key == 3 || key == 27) {
            return -1;
        }
        if (key == '\b') {
            if (len > 0) {
                buf[--len] = '\0';
            }
            continue;
        }
        if (key >= 32 && key < 127 && len + 1 < size) {
            buf[len++] = (char)key;
            buf[len] = '\0';
        }
    }
}

static void editor_search(struct editor *e) {
    char needle[64];

    if (editor_prompt(e, "Search: ", needle, sizeof(needle)) <= 0) {
        e->status[0] = '\0';
        return;
    }

    for (int i = 0; i < e->count; i++) {
        int index = (e->cy + 1 + i) % e->count;
        char *hit = strstr(e->lines[index], needle);
        if (hit) {
            e->cy = index;
            e->cx = (int)(hit - e->lines[index]);
            snprintf(e->status, sizeof(e->status), "Found on line %d", index + 1);
            return;
        }
    }
    snprintf(e->status, sizeof(e->status), "Not found: %s", needle);
}

static void editor_insert_char(struct editor *e, char c) {
    char *line = e->lines[e->cy];
    int len = (int)strlen(line);

    if (len + 1 >= LINE_CAPACITY) {
        return;
    }
    memmove(line + e->cx + 1, line + e->cx, (size_t)(len - e->cx + 1));
    line[e->cx] = c;
    e->cx++;
    e->modified = 1;
}

static void editor_newline(struct editor *e) {
    char *line = e->lines[e->cy];
    const char *rest = line + e->cx;

    if (editor_insert_line(e, e->cy + 1, rest) < 0) {
        return;
    }
    line[e->cx] = '\0';
    e->cy++;
    e->cx = 0;
    e->modified = 1;
}

static void editor_backspace(struct editor *e) {
    if (e->cx > 0) {
        char *line = e->lines[e->cy];
        int len = (int)strlen(line);
        memmove(line + e->cx - 1, line + e->cx, (size_t)(len - e->cx + 1));
        e->cx--;
        e->modified = 1;
        return;
    }

    if (e->cy == 0) {
        return;
    }

    char *previous = e->lines[e->cy - 1];
    int plen = (int)strlen(previous);
    char *current = e->lines[e->cy];

    if (plen + (int)strlen(current) < LINE_CAPACITY - 1) {
        strcat(previous, current);
    }
    editor_remove_line(e, e->cy);
    e->cy--;
    e->cx = plen;
    e->modified = 1;
}

static void editor_delete(struct editor *e) {
    char *line = e->lines[e->cy];
    int len = (int)strlen(line);

    if (e->cx < len) {
        memmove(line + e->cx, line + e->cx + 1, (size_t)(len - e->cx));
        e->modified = 1;
        return;
    }
    if (e->cy + 1 < e->count) {
        char *next = e->lines[e->cy + 1];
        if (len + (int)strlen(next) < LINE_CAPACITY - 1) {
            strcat(line, next);
        }
        editor_remove_line(e, e->cy + 1);
        e->modified = 1;
    }
}

static void editor_clamp(struct editor *e) {
    if (e->cy < 0) {
        e->cy = 0;
    }
    if (e->cy >= e->count) {
        e->cy = e->count - 1;
    }
    int len = (int)strlen(e->lines[e->cy]);
    if (e->cx > len) {
        e->cx = len;
    }
    if (e->cx < 0) {
        e->cx = 0;
    }
}

int cmd_nano(int argc, char **argv, struct stream *in, struct stream *out) {
    struct editor e;
    memset(&e, 0, sizeof(e));

    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!cmd_is_flag(argv[i])) {
            target = argv[i];
            break;
        }
    }

    if (target) {
        strlcpy(e.filename, target, sizeof(e.filename));
        struct vfs_stat st;
        void *data = NULL;
        uint32_t size = 0;
        int found = vfs_stat(target, &st) == 0;
        if (found && st.type == VFS_DIR) {
            cmd_error(out, "nano", target, "is a directory");
            return 1;
        }
        if (found) {
            int r = vfs_load(target, &data, &size);
            if (r < 0) {
                cmd_vfs_error(out, "nano", target, r);
                return 1;
            }
        }
        editor_load(&e, (const uint8_t *)data, size);
        if (data) {
            kfree(data);
        }
        if (!found) {
            snprintf(e.status, sizeof(e.status), "New file");
        }
    } else {
        editor_insert_line(&e, 0, "");
        snprintf(e.status, sizeof(e.status), "New buffer, save with ^O");
    }

    screen_enter();
    int running = 1;

    while (running) {
        editor_clamp(&e);
        editor_scroll(&e);
        editor_draw(&e);

        int key = input_getkey();
        e.status[0] = '\0';

        switch (key) {
            case KEY_UP:
                e.cy--;
                break;
            case KEY_DOWN:
                e.cy++;
                break;
            case KEY_LEFT:
                if (e.cx > 0) {
                    e.cx--;
                } else if (e.cy > 0) {
                    e.cy--;
                    e.cx = (int)strlen(e.lines[e.cy]);
                }
                break;
            case KEY_RIGHT:
                if (e.cx < (int)strlen(e.lines[e.cy])) {
                    e.cx++;
                } else if (e.cy + 1 < e.count) {
                    e.cy++;
                    e.cx = 0;
                }
                break;
            case KEY_HOME:
            case 1:
                e.cx = 0;
                break;
            case KEY_END:
            case 5:
                e.cx = (int)strlen(e.lines[e.cy]);
                break;
            case KEY_PGUP:
                e.cy -= EDITOR_ROWS;
                break;
            case KEY_PGDN:
                e.cy += EDITOR_ROWS;
                break;
            case KEY_DELETE:
                editor_delete(&e);
                break;
            case '\b':
                editor_backspace(&e);
                break;
            case '\n':
                editor_newline(&e);
                break;
            case '\t':
                for (int i = 0; i < 4; i++) {
                    editor_insert_char(&e, ' ');
                }
                break;
            case 15: {
                if (!e.filename[0]) {
                    char name[VFS_PATH_MAX];
                    if (editor_prompt(&e, "File name to write: ", name, sizeof(name)) <= 0) {
                        break;
                    }
                    strlcpy(e.filename, name, sizeof(e.filename));
                }
                editor_save(&e);
                break;
            }
            case 24: {
                if (e.modified) {
                    char answer[8];
                    int len = editor_prompt(&e, "Save modified buffer? (y/n) ", answer, sizeof(answer));
                    if (len < 0) {
                        break;
                    }
                    if (answer[0] == 'y' || answer[0] == 'Y') {
                        if (!e.filename[0]) {
                            char name[VFS_PATH_MAX];
                            if (editor_prompt(&e, "File name to write: ", name, sizeof(name)) <= 0) {
                                break;
                            }
                            strlcpy(e.filename, name, sizeof(e.filename));
                        }
                        if (editor_save(&e) < 0) {
                            break;
                        }
                    }
                }
                running = 0;
                break;
            }
            case 11:
                strlcpy(e.clipboard, e.lines[e.cy], LINE_CAPACITY);
                if (e.count > 1) {
                    editor_remove_line(&e, e.cy);
                } else {
                    e.lines[0][0] = '\0';
                }
                e.cx = 0;
                e.modified = 1;
                snprintf(e.status, sizeof(e.status), "Line cut");
                break;
            case 21:
                if (e.clipboard[0]) {
                    editor_insert_line(&e, e.cy, e.clipboard);
                    e.cy++;
                    e.modified = 1;
                }
                break;
            case 23:
                editor_search(&e);
                break;
            case 7:
                snprintf(e.status, sizeof(e.status),
                         "^O write  ^X exit  ^K cut  ^U paste  ^W search  arrows move");
                break;
            case 3:
                snprintf(e.status, sizeof(e.status), "line %d of %d, column %d",
                         e.cy + 1, e.count, e.cx + 1);
                break;
            default:
                if (key >= 32 && key < 127) {
                    editor_insert_char(&e, (char)key);
                }
                break;
        }
    }

    screen_leave();

    if (e.ram_only) {
        st_puts(out, "nano: saved in RAM only, this file lives on a ramfs mount and is lost on reboot\n");
    }

    for (int i = 0; i < e.count; i++) {
        kfree(e.lines[i]);
    }
    kfree(e.lines);
    return 0;
}
