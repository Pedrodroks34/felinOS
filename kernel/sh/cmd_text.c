#include "sh/cmds.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/format.h"
#include "drivers/input.h"
#include "drivers/ata.h"
#include "console.h"

static int flag_value(int argc, char **argv, const char *flag, int fallback) {
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            return atoi(argv[i + 1]);
        }
    }
    return fallback;
}

static int is_consumed(int argc, char **argv, int index, const char *flag) {
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0 && i + 1 == index) {
            return 1;
        }
    }
    return 0;
}

int cmd_echo(int argc, char **argv, struct stream *in, struct stream *out) {
    int no_newline = 0;
    int escapes = 0;
    int start = 1;

    while (start < argc && (strcmp(argv[start], "-n") == 0 || strcmp(argv[start], "-e") == 0)) {
        if (strcmp(argv[start], "-n") == 0) {
            no_newline = 1;
        } else {
            escapes = 1;
        }
        start++;
    }

    for (int i = start; i < argc; i++) {
        if (i > start) {
            st_putc(out, ' ');
        }
        if (!escapes) {
            st_puts(out, argv[i]);
            continue;
        }
        for (const char *p = argv[i]; *p; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                switch (*p) {
                    case 'n': st_putc(out, '\n'); break;
                    case 't': st_putc(out, '\t'); break;
                    case 'r': st_putc(out, '\r'); break;
                    case '0': st_putc(out, '\0'); break;
                    case '\\': st_putc(out, '\\'); break;
                    default: st_putc(out, *p); break;
                }
                continue;
            }
            st_putc(out, *p);
        }
    }

    if (!no_newline) {
        st_putc(out, '\n');
    }
    return 0;
}

int cmd_printf(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        cmd_error(out, "printf", NULL, "missing format");
        return 1;
    }

    int arg = 2;
    for (const char *p = argv[1]; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': st_putc(out, '\n'); break;
                case 't': st_putc(out, '\t'); break;
                case 'r': st_putc(out, '\r'); break;
                case '\\': st_putc(out, '\\'); break;
                default: st_putc(out, *p); break;
            }
            continue;
        }
        if (*p == '%' && p[1]) {
            p++;
            if (*p == 's') {
                st_puts(out, (arg < argc) ? argv[arg++] : "");
            } else if (*p == 'd' || *p == 'i') {
                st_printf(out, "%d", (arg < argc) ? atoi(argv[arg++]) : 0);
            } else if (*p == 'x') {
                st_printf(out, "%x", (arg < argc) ? (uint32_t)atoi(argv[arg++]) : 0);
            } else if (*p == 'c') {
                st_putc(out, (arg < argc) ? argv[arg++][0] : ' ');
            } else if (*p == '%') {
                st_putc(out, '%');
            }
            continue;
        }
        st_putc(out, *p);
    }
    return 0;
}

int cmd_head(int argc, char **argv, struct stream *in, struct stream *out) {
    int count = flag_value(argc, argv, "-n", 10);
    uint32_t len = 0;
    char *text = NULL;

    int start = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            start = i + 2;
        }
    }

    text = cmd_collect_input(argc, argv, start, in, out, "head", &len);
    if (!text) {
        return 1;
    }

    char **lines;
    int total = cmd_split_lines(text, &lines);
    for (int i = 0; i < total && i < count; i++) {
        st_printf(out, "%s\n", lines[i]);
    }
    kfree(lines);
    kfree(text);
    return 0;
}

int cmd_tail(int argc, char **argv, struct stream *in, struct stream *out) {
    int count = flag_value(argc, argv, "-n", 10);
    int start = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            start = i + 2;
        }
    }

    uint32_t len = 0;
    char *text = cmd_collect_input(argc, argv, start, in, out, "tail", &len);
    if (!text) {
        return 1;
    }

    char **lines;
    int total = cmd_split_lines(text, &lines);
    int first = total - count;
    if (first < 0) {
        first = 0;
    }
    for (int i = first; i < total; i++) {
        st_printf(out, "%s\n", lines[i]);
    }
    kfree(lines);
    kfree(text);
    return 0;
}

int cmd_wc(int argc, char **argv, struct stream *in, struct stream *out) {
    int want_lines = cmd_has_flag(argc, argv, "-l");
    int want_words = cmd_has_flag(argc, argv, "-w");
    int want_bytes = cmd_has_flag(argc, argv, "-c");

    if (!want_lines && !want_words && !want_bytes) {
        want_lines = want_words = want_bytes = 1;
    }

    uint32_t len = 0;
    char *text = cmd_collect_input(argc, argv, 1, in, out, "wc", &len);
    if (!text) {
        return 1;
    }

    uint32_t lines = 0;
    uint32_t words = 0;
    int in_word = 0;

    for (uint32_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            lines++;
        }
        if (isspace((unsigned char)text[i])) {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }
    if (len && text[len - 1] != '\n') {
        lines++;
    }

    if (want_lines) {
        st_printf(out, "%8u", lines);
    }
    if (want_words) {
        st_printf(out, "%8u", words);
    }
    if (want_bytes) {
        st_printf(out, "%8u", len);
    }
    st_putc(out, '\n');

    kfree(text);
    return 0;
}

static int line_matches(const char *line, const char *pattern, int ignore_case) {
    if (!ignore_case) {
        return strstr(line, pattern) != NULL;
    }
    size_t plen = strlen(pattern);
    if (plen == 0) {
        return 1;
    }
    for (const char *p = line; *p; p++) {
        size_t i = 0;
        while (i < plen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)pattern[i])) {
            i++;
        }
        if (i == plen) {
            return 1;
        }
    }
    return 0;
}

int cmd_grep(int argc, char **argv, struct stream *in, struct stream *out) {
    int ignore_case = cmd_has_flag(argc, argv, "-i");
    int invert = cmd_has_flag(argc, argv, "-v");
    int numbered = cmd_has_flag(argc, argv, "-n");
    int count_only = cmd_has_flag(argc, argv, "-c");

    const char *pattern = NULL;
    int pattern_index = 0;

    for (int i = 1; i < argc; i++) {
        if (!cmd_is_flag(argv[i])) {
            pattern = argv[i];
            pattern_index = i;
            break;
        }
    }
    if (!pattern) {
        cmd_error(out, "grep", NULL, "missing pattern");
        return 1;
    }

    uint32_t len = 0;
    char *text = cmd_collect_input(argc, argv, pattern_index + 1, in, out, "grep", &len);
    if (!text) {
        return 1;
    }

    char **lines;
    int total = cmd_split_lines(text, &lines);
    uint32_t matches = 0;

    for (int i = 0; i < total; i++) {
        int hit = line_matches(lines[i], pattern, ignore_case);
        if (invert) {
            hit = !hit;
        }
        if (!hit) {
            continue;
        }
        matches++;
        if (count_only) {
            continue;
        }
        if (numbered) {
            st_printf(out, "%u:%s\n", (uint32_t)(i + 1), lines[i]);
        } else {
            st_printf(out, "%s\n", lines[i]);
        }
    }

    if (count_only) {
        st_printf(out, "%u\n", matches);
    }

    kfree(lines);
    kfree(text);
    return matches ? 0 : 1;
}

int cmd_sort(int argc, char **argv, struct stream *in, struct stream *out) {
    int reverse = cmd_has_flag(argc, argv, "-r");
    int numeric = cmd_has_flag(argc, argv, "-n");
    int unique = cmd_has_flag(argc, argv, "-u");

    uint32_t len = 0;
    char *text = cmd_collect_input(argc, argv, 1, in, out, "sort", &len);
    if (!text) {
        return 1;
    }

    char **lines;
    int total = cmd_split_lines(text, &lines);

    for (int i = 1; i < total; i++) {
        char *key = lines[i];
        int j = i - 1;
        while (j >= 0) {
            int cmp;
            if (numeric) {
                int a = atoi(lines[j]);
                int b = atoi(key);
                cmp = (a > b) - (a < b);
            } else {
                cmp = strcmp(lines[j], key);
            }
            if (reverse) {
                cmp = -cmp;
            }
            if (cmp <= 0) {
                break;
            }
            lines[j + 1] = lines[j];
            j--;
        }
        lines[j + 1] = key;
    }

    for (int i = 0; i < total; i++) {
        if (unique && i > 0 && strcmp(lines[i], lines[i - 1]) == 0) {
            continue;
        }
        st_printf(out, "%s\n", lines[i]);
    }

    kfree(lines);
    kfree(text);
    return 0;
}

int cmd_uniq(int argc, char **argv, struct stream *in, struct stream *out) {
    int show_count = cmd_has_flag(argc, argv, "-c");
    uint32_t len = 0;
    char *text = cmd_collect_input(argc, argv, 1, in, out, "uniq", &len);

    if (!text) {
        return 1;
    }

    char **lines;
    int total = cmd_split_lines(text, &lines);
    int i = 0;

    while (i < total) {
        int run = 1;
        while (i + run < total && strcmp(lines[i], lines[i + run]) == 0) {
            run++;
        }
        if (show_count) {
            st_printf(out, "%7u %s\n", (uint32_t)run, lines[i]);
        } else {
            st_printf(out, "%s\n", lines[i]);
        }
        i += run;
    }

    kfree(lines);
    kfree(text);
    return 0;
}

int cmd_cut(int argc, char **argv, struct stream *in, struct stream *out) {
    char delim = '\t';
    int field = 1;
    int start = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            delim = argv[i + 1][0];
            start = i + 2;
            i++;
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            field = atoi(argv[i + 1]);
            start = i + 2;
            i++;
        } else if (argv[i][0] == '-' && argv[i][1] == 'd' && argv[i][2]) {
            delim = argv[i][2];
            start = i + 1;
        } else if (argv[i][0] == '-' && argv[i][1] == 'f' && argv[i][2]) {
            field = atoi(argv[i] + 2);
            start = i + 1;
        }
    }
    if (field < 1) {
        field = 1;
    }

    uint32_t len = 0;
    char *text = cmd_collect_input(argc, argv, start, in, out, "cut", &len);
    if (!text) {
        return 1;
    }

    char **lines;
    int total = cmd_split_lines(text, &lines);

    for (int i = 0; i < total; i++) {
        const char *p = lines[i];
        int current = 1;
        const char *field_start = p;

        while (*p && current < field) {
            if (*p == delim) {
                current++;
                field_start = p + 1;
            }
            p++;
        }
        if (current < field) {
            st_putc(out, '\n');
            continue;
        }
        while (*p && *p != delim) {
            p++;
        }
        st_write(out, field_start, (uint32_t)(p - field_start));
        st_putc(out, '\n');
    }

    kfree(lines);
    kfree(text);
    return 0;
}

int cmd_tr(int argc, char **argv, struct stream *in, struct stream *out) {
    int del = cmd_has_flag(argc, argv, "-d");
    const char *set1 = NULL;
    const char *set2 = NULL;

    for (int i = 1; i < argc; i++) {
        if (cmd_is_flag(argv[i])) {
            continue;
        }
        if (!set1) {
            set1 = argv[i];
        } else if (!set2) {
            set2 = argv[i];
        }
    }
    if (!set1 || (!del && !set2)) {
        cmd_error(out, "tr", NULL, "usage: tr [-d] <set1> [set2]");
        return 1;
    }

    uint32_t len = 0;
    char *text = st_read_all(in ? in : stream_console(), &len);
    if (!text) {
        return 1;
    }

    size_t s1len = strlen(set1);
    size_t s2len = set2 ? strlen(set2) : 0;

    for (uint32_t i = 0; i < len; i++) {
        char *hit = strchr(set1, text[i]);
        if (!hit || text[i] == '\0') {
            st_putc(out, text[i]);
            continue;
        }
        if (del) {
            continue;
        }
        size_t index = (size_t)(hit - set1);
        if (index >= s2len) {
            index = s2len ? s2len - 1 : 0;
        }
        st_putc(out, set2[index]);
    }

    (void)s1len;
    kfree(text);
    return 0;
}

int cmd_rev(int argc, char **argv, struct stream *in, struct stream *out) {
    uint32_t len = 0;
    char *text = cmd_collect_input(argc, argv, 1, in, out, "rev", &len);

    if (!text) {
        return 1;
    }

    char **lines;
    int total = cmd_split_lines(text, &lines);

    for (int i = 0; i < total; i++) {
        size_t l = strlen(lines[i]);
        for (size_t j = l; j > 0; j--) {
            st_putc(out, lines[i][j - 1]);
        }
        st_putc(out, '\n');
    }

    kfree(lines);
    kfree(text);
    return 0;
}

int cmd_tee(int argc, char **argv, struct stream *in, struct stream *out) {
    int append = cmd_has_flag(argc, argv, "-a");
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!cmd_is_flag(argv[i])) {
            path = argv[i];
            break;
        }
    }
    if (!path) {
        cmd_error(out, "tee", NULL, "missing file operand");
        return 1;
    }

    uint32_t len = 0;
    char *text = st_read_all(in ? in : stream_console(), &len);
    if (!text) {
        return 1;
    }

    int r = append ? vfs_append(path, text, len) : vfs_save(path, text, len);
    if (r < 0) {
        cmd_vfs_error(out, "tee", path, r);
        kfree(text);
        return 1;
    }
    st_write(out, text, len);

    kfree(text);
    return 0;
}

int cmd_nl(int argc, char **argv, struct stream *in, struct stream *out) {
    uint32_t len = 0;
    char *text = cmd_collect_input(argc, argv, 1, in, out, "nl", &len);

    if (!text) {
        return 1;
    }

    char **lines;
    int total = cmd_split_lines(text, &lines);
    for (int i = 0; i < total; i++) {
        st_printf(out, "%6u  %s\n", (uint32_t)(i + 1), lines[i]);
    }

    kfree(lines);
    kfree(text);
    return 0;
}

int cmd_tac(int argc, char **argv, struct stream *in, struct stream *out) {
    uint32_t len = 0;
    char *text = cmd_collect_input(argc, argv, 1, in, out, "tac", &len);

    if (!text) {
        return 1;
    }

    char **lines;
    int total = cmd_split_lines(text, &lines);
    for (int i = total - 1; i >= 0; i--) {
        st_printf(out, "%s\n", lines[i]);
    }

    kfree(lines);
    kfree(text);
    return 0;
}

int cmd_more(int argc, char **argv, struct stream *in, struct stream *out) {
    uint32_t len = 0;
    char *text = cmd_collect_input(argc, argv, 1, in, out, "more", &len);

    if (!text) {
        return 1;
    }

    char **lines;
    int total = cmd_split_lines(text, &lines);
    int page = 22;

    for (int i = 0; i < total; i++) {
        st_printf(out, "%s\n", lines[i]);
        if ((i + 1) % page == 0 && i + 1 < total) {
            st_printf(out, "-- more -- (%u%%) press space to continue, q to quit",
                      (uint32_t)((i + 1) * 100 / total));
            int key = input_getkey();
            st_puts(out, "\r                                                          \r");
            if (key == 'q' || key == 3) {
                break;
            }
        }
    }

    kfree(lines);
    kfree(text);
    return 0;
}

int cmd_hexdump(int argc, char **argv, struct stream *in, struct stream *out) {
    uint32_t limit = (uint32_t)flag_value(argc, argv, "-n", 256);
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        if (cmd_is_flag(argv[i]) || is_consumed(argc, argv, i, "-n")) {
            continue;
        }
        target = argv[i];
        break;
    }

    uint8_t *data = NULL;
    uint32_t size = 0;
    uint8_t *owned = NULL;

    if (target) {
        struct vfs_file *f;
        int r = vfs_open(target, VFS_O_READ, &f);
        if (r < 0) {
            cmd_vfs_error(out, "hexdump", target, r);
            return 1;
        }
        uint32_t want = limit > 1048576u ? 1048576u : limit;
        owned = (uint8_t *)kmalloc(want ? want : 1);
        if (!owned) {
            vfs_close(f);
            return 1;
        }
        while (size < want) {
            int n = vfs_read(f, owned + size, want - size);
            if (n < 0) {
                cmd_vfs_error(out, "hexdump", target, n);
                kfree(owned);
                vfs_close(f);
                return 1;
            }
            if (n == 0) {
                break;
            }
            size += (uint32_t)n;
        }
        vfs_close(f);
        data = owned;
    } else {
        uint32_t len = 0;
        char *text = st_read_all(in ? in : stream_console(), &len);
        if (!text) {
            return 1;
        }
        owned = (uint8_t *)text;
        data = owned;
        size = len;
    }

    if (size > limit) {
        size = limit;
    }

    for (uint32_t offset = 0; offset < size; offset += 16) {
        st_printf(out, "%08x  ", offset);
        for (uint32_t i = 0; i < 16; i++) {
            if (offset + i < size) {
                st_printf(out, "%02x ", data[offset + i]);
            } else {
                st_puts(out, "   ");
            }
            if (i == 7) {
                st_putc(out, ' ');
            }
        }
        st_puts(out, " |");
        for (uint32_t i = 0; i < 16 && offset + i < size; i++) {
            uint8_t c = data[offset + i];
            st_putc(out, isprint(c) ? (char)c : '.');
        }
        st_puts(out, "|\n");
    }
    st_printf(out, "%08x\n", size);

    if (owned) {
        kfree(owned);
    }
    return 0;
}

int cmd_strings(int argc, char **argv, struct stream *in, struct stream *out) {
    int minimum = flag_value(argc, argv, "-n", 4);
    uint32_t len = 0;
    char *text = NULL;
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        if (cmd_is_flag(argv[i]) || is_consumed(argc, argv, i, "-n")) {
            continue;
        }
        target = argv[i];
        break;
    }

    if (target) {
        void *data;
        int r = vfs_load(target, &data, &len);
        if (r < 0) {
            cmd_vfs_error(out, "strings", target, r);
            return 1;
        }
        text = (char *)data;
    } else {
        text = st_read_all(in ? in : stream_console(), &len);
        if (!text) {
            return 1;
        }
    }

    uint32_t run = 0;
    for (uint32_t i = 0; i <= len; i++) {
        int printable = (i < len) && isprint((unsigned char)text[i]);
        if (printable) {
            run++;
            continue;
        }
        if ((int)run >= minimum) {
            st_write(out, text + i - run, run);
            st_putc(out, '\n');
        }
        run = 0;
    }

    kfree(text);
    return 0;
}

int cmd_diff(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 3) {
        cmd_error(out, "diff", NULL, "usage: diff <file1> <file2>");
        return 1;
    }

    void *da;
    void *db;
    uint32_t sa;
    uint32_t sb;
    int r = vfs_load(argv[1], &da, &sa);

    if (r < 0) {
        cmd_vfs_error(out, "diff", argv[1], r);
        return 2;
    }
    r = vfs_load(argv[2], &db, &sb);
    if (r < 0) {
        cmd_vfs_error(out, "diff", argv[2], r);
        kfree(da);
        return 2;
    }

    char *ta = (char *)da;
    char *tb = (char *)db;

    char **la;
    char **lb;
    int na = cmd_split_lines(ta, &la);
    int nb = cmd_split_lines(tb, &lb);
    int differences = 0;
    int max = (na > nb) ? na : nb;

    for (int i = 0; i < max; i++) {
        const char *left = (i < na) ? la[i] : NULL;
        const char *right = (i < nb) ? lb[i] : NULL;

        if (left && right && strcmp(left, right) == 0) {
            continue;
        }
        differences++;
        st_printf(out, "%uc%u\n", (uint32_t)(i + 1), (uint32_t)(i + 1));
        if (left) {
            st_printf(out, "< %s\n", left);
        }
        if (left && right) {
            st_puts(out, "---\n");
        }
        if (right) {
            st_printf(out, "> %s\n", right);
        }
    }

    kfree(la);
    kfree(lb);
    kfree(ta);
    kfree(tb);
    return differences ? 1 : 0;
}
