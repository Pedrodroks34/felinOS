#include "sh/shell.h"
#include "user.h"
#include "sh/cmds.h"
#include "console.h"
#include "drivers/vga.h"
#include "drivers/serial.h"
#include "drivers/input.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/format.h"

#define ENV_MAX      32
#define ALIAS_MAX    16
#define HISTORY_MAX  32
#define PIPE_MAX     8
#define NAME_MAX     32
#define VALUE_MAX    192

struct env_entry {
    char name[NAME_MAX];
    char value[VALUE_MAX];
    int used;
};

struct alias_entry {
    char name[NAME_MAX];
    char value[VALUE_MAX];
    int used;
};

static struct env_entry env_table[ENV_MAX];
static struct alias_entry alias_table[ALIAS_MAX];
static char history[HISTORY_MAX][SHELL_LINE_MAX];
static int history_count;
static int last_status;

static const struct shell_params *current_params;

const struct shell_params *shell_set_params(const struct shell_params *p) {
    const struct shell_params *old = current_params;

    current_params = p;
    return old;
}
static int exit_requested;
static int background_request;

static const struct command commands[] = {
    { "ls", "list directory contents", "ls [-l] [-a] [path...]", cmd_ls },
    { "cd", "change the working directory", "cd [path]", cmd_cd },
    { "pwd", "print the working directory", "pwd", cmd_pwd },
    { "mkdir", "create directories", "mkdir [-p] <dir...>", cmd_mkdir },
    { "rmdir", "remove empty directories", "rmdir <dir...>", cmd_rmdir },
    { "touch", "create empty files or update timestamps", "touch <file...>", cmd_touch },
    { "rm", "remove files and directories", "rm [-r] [-f] <path...>", cmd_rm },
    { "cp", "copy files and directories", "cp [-r] <source...> <dest>", cmd_cp },
    { "mv", "move or rename files", "mv <source...> <dest>", cmd_mv },
    { "cat", "concatenate and print files", "cat [-n] [file...]", cmd_cat },
    { "tree", "show a directory tree", "tree [path]", cmd_tree },
    { "find", "search for files by name", "find [path] [-name pattern] [-type f|d]", cmd_find },
    { "stat", "show file metadata", "stat <path...>", cmd_stat },
    { "du", "show disk usage of a tree", "du [-h] [path]", cmd_du },
    { "df", "show filesystem usage", "df [-h]", cmd_df },
    { "mount", "list or add mounted filesystems", "mount [-t <type> [source] <dir>]", cmd_mount },
    { "umount", "unmount a filesystem", "umount <dir>", cmd_umount },
    { "chmod", "change file mode bits", "chmod <octal> <path...>", cmd_chmod },
    { "chown", "change file owner and group", "chown <user>[:group] <path...>", cmd_chown },
    { "file", "identify file type", "file <path...>", cmd_file },
    { "echo", "print arguments", "echo [-n] [-e] [text...]", cmd_echo },
    { "printf", "print formatted text", "printf <format> [args...]", cmd_printf },
    { "head", "print the first lines", "head [-n count] [file...]", cmd_head },
    { "tail", "print the last lines", "tail [-n count] [file...]", cmd_tail },
    { "wc", "count lines, words and bytes", "wc [-l] [-w] [-c] [file...]", cmd_wc },
    { "grep", "search text for a pattern", "grep [-i] [-v] [-n] [-c] <pattern> [file...]", cmd_grep },
    { "sort", "sort lines", "sort [-r] [-n] [-u] [file...]", cmd_sort },
    { "uniq", "filter repeated lines", "uniq [-c] [file...]", cmd_uniq },
    { "cut", "extract fields from lines", "cut -d <delim> -f <n> [file...]", cmd_cut },
    { "tr", "translate or delete characters", "tr [-d] <set1> [set2]", cmd_tr },
    { "rev", "reverse lines character-wise", "rev [file...]", cmd_rev },
    { "tee", "copy input to a file and output", "tee [-a] <file>", cmd_tee },
    { "nl", "number lines", "nl [file...]", cmd_nl },
    { "tac", "print lines in reverse order", "tac [file...]", cmd_tac },
    { "more", "page through text", "more [file...]", cmd_more },
    { "hexdump", "dump bytes in hex", "hexdump [-n bytes] <file|/dev/hdX>", cmd_hexdump },
    { "strings", "print printable sequences", "strings [-n min] <file>", cmd_strings },
    { "diff", "compare two files line by line", "diff <file1> <file2>", cmd_diff },
    { "nano", "edit a text file", "nano [file]", cmd_nano },
    { "exec", "run a user-mode program (ELF)", "exec <program> [args...]", cmd_exec },
    { "help", "list available commands", "help [command]", cmd_help },
    { "man", "show the manual for a command", "man <command>", cmd_man },
    { "which", "locate a command", "which <command...>", cmd_which },
    { "uname", "print system information", "uname [-a] [-r] [-m] [-n]", cmd_uname },
    { "uptime", "show how long the system has run", "uptime", cmd_uptime },
    { "date", "show the current date and time", "date [-u]", cmd_date },
    { "cal", "display a calendar", "cal [month] [year]", cmd_cal },
    { "free", "show memory usage", "free [-h]", cmd_free },
    { "lsmem", "show memory layout", "lsmem", cmd_lsmem },
    { "ps", "list tasks and processes", "ps", cmd_ps },
    { "kill", "send a signal to a process", "kill [-signal] <pid...>", cmd_kill },
    { "renice", "change the scheduling weight of a process", "renice <nice> <pid...>", cmd_renice },
    { "sched", "show or tune the scheduler", "sched [quantum <ticks>]", cmd_sched },
    { "fg", "bring a background process to the foreground", "fg [pid]", cmd_fg },
    { "vmm", "inspect and drive the virtual memory manager", "vmm [stat|regions|spaces|map|alloc|free|list|touch|swapout|reclaim|growheap|test]", cmd_vmm },
    { "pmap", "show the address space layout and page mappings", "pmap [-a]", cmd_pmap },
    { "vmstat", "show virtual memory counters or one page", "vmstat [hex address]", cmd_vmstat },
    { "mkswap", "create a swap area on a disk", "mkswap <disk> [MB] [-f]", cmd_mkswap },
    { "swapon", "enable swapping to a disk", "swapon [disk]", cmd_swapon },
    { "swapoff", "page everything back in and disable swap", "swapoff", cmd_swapoff },
    { "swapinfo", "show swap device usage", "swapinfo", cmd_swapinfo },
    { "memtest", "allocate, write and verify virtual memory", "memtest [MB]", cmd_memtest },
    { "dmesg", "print kernel log messages", "dmesg [-c]", cmd_dmesg },
    { "whoami", "print the current user", "whoami", cmd_whoami },
    { "id", "print current uid and gid", "id", cmd_id },
    { "login", "log in as another user", "login <name>", cmd_login },
    { "su", "switch user", "su [name]", cmd_su },
    { "passwd", "change a user's password", "passwd [name]", cmd_passwd },
    { "useradd", "create a new user account", "useradd <name> [uid]", cmd_useradd },
    { "source", "run a vsh script in the current shell", "source <path> [args...]", cmd_source },
    { ".", "alias for source", ". <path> [args...]", cmd_source },
    { "credits", "show project credits", "credits", cmd_credits },
    { "hostname", "show or set the hostname", "hostname [name]", cmd_hostname },
    { "clear", "clear the screen", "clear", cmd_clear },
    { "color", "set console colors", "color <fg> <bg>", cmd_color },
    { "history", "show command history", "history [-c]", cmd_history },
    { "alias", "define or list aliases", "alias [name=value]", cmd_alias },
    { "unalias", "remove an alias", "unalias <name>", cmd_unalias },
    { "env", "list environment variables", "env", cmd_env },
    { "export", "set an environment variable", "export NAME=value", cmd_export },
    { "set", "set an environment variable", "set NAME=value", cmd_export },
    { "unset", "remove an environment variable", "unset <name>", cmd_unset },
    { "sleep", "wait for a number of seconds", "sleep <seconds>", cmd_sleep },
    { "beep", "sound the PC speaker", "beep [frequency] [ms]", cmd_beep },
    { "time", "time the execution of a command", "time <command...>", cmd_time },
    { "sync", "flush pending writes", "sync", cmd_sync },
    { "true", "return success", "true", cmd_true },
    { "false", "return failure", "false", cmd_false },
    { "test", "evaluate a condition", "test <expr>", cmd_test },
    { "expr", "evaluate an integer expression", "expr <a> <op> <b>", cmd_expr },
    { "seq", "print a sequence of numbers", "seq [first] [step] <last>", cmd_seq },
    { "yes", "print a string repeatedly", "yes [-n count] [string...]", cmd_yes },
    { "basename", "strip directory from a path", "basename <path>", cmd_basename },
    { "dirname", "strip the last path component", "dirname <path>", cmd_dirname },
    { "lscpu", "show CPU information", "lscpu", cmd_lscpu },
    { "smp", "show application processor status", "smp", cmd_smp },
    { "lspci", "list PCI devices", "lspci [-v]", cmd_lspci },
    { "lsblk", "list block devices", "lsblk", cmd_lsblk },
    { "blkid", "show block device identifiers", "blkid", cmd_blkid },
    { "bcache", "show the ATA buffer cache stats", "bcache", cmd_bcache },
    { "fdisk", "inspect disk partition tables", "fdisk -l [device]", cmd_fdisk },
    { "gatofs", "GatoFS disk filesystem", "gatofs format|mount|umount|df|ls|cat|write|mkdir|rm|mv|stat|import|export|gen|verify", cmd_gatofs },
    { "fat32", "create and inspect FAT32 volumes", "fat32 info|format <dev>", cmd_fat32 },
    { "dd", "copy raw sectors between disk and files", "dd if=<src> of=<dst> [bs=512] [count=n] [skip=n] [seek=n]", cmd_dd },
    { "acpi", "show ACPI tables and power management", "acpi", cmd_acpi },
    { "reboot", "restart the machine", "reboot", cmd_reboot },
    { "poweroff", "power the machine off", "poweroff", cmd_poweroff },
    { "shutdown", "power the machine off", "shutdown", cmd_poweroff },
    { "halt", "halt the CPU", "halt", cmd_halt },
    { "exit", "leave the current shell session", "exit", cmd_exit },
    { "ifconfig", "show or set the network interface", "ifconfig [ip netmask [gateway]]", cmd_ifconfig },
    { "ping", "send ICMP echo requests to a host", "ping <host> [count]", cmd_ping },
    { "dhcp", "obtain an address lease via DHCP", "dhcp", cmd_dhcp },
    { "dns", "resolve a hostname to an IPv4 address", "dns <hostname>", cmd_dns },
    { "simplecc", "simple C compiler", "simplecc", cmd_simplecc },
    { "test", "run automated test suite", "test", cmd_test }
};

static const int command_count = (int)(sizeof(commands) / sizeof(commands[0]));

const struct command *shell_command_table(void) {
    return commands;
}

int shell_command_count(void) {
    return command_count;
}

const struct command *shell_find_command(const char *name) {
    for (int i = 0; i < command_count; i++) {
        if (strcmp(commands[i].name, name) == 0) {
            return &commands[i];
        }
    }
    return NULL;
}

int shell_last_status(void) {
    return last_status;
}

void shell_request_exit(void) {
    exit_requested = 1;
}

int shell_take_background(void) {
    int requested = background_request;

    background_request = 0;
    return requested;
}

const char *shell_getenv(const char *name) {
    for (int i = 0; i < ENV_MAX; i++) {
        if (env_table[i].used && strcmp(env_table[i].name, name) == 0) {
            return env_table[i].value;
        }
    }
    return NULL;
}

int shell_setenv(const char *name, const char *value) {
    for (int i = 0; i < ENV_MAX; i++) {
        if (env_table[i].used && strcmp(env_table[i].name, name) == 0) {
            strlcpy(env_table[i].value, value, VALUE_MAX);
            return 0;
        }
    }
    for (int i = 0; i < ENV_MAX; i++) {
        if (!env_table[i].used) {
            env_table[i].used = 1;
            strlcpy(env_table[i].name, name, NAME_MAX);
            strlcpy(env_table[i].value, value, VALUE_MAX);
            return 0;
        }
    }
    return -1;
}

int shell_unsetenv(const char *name) {
    for (int i = 0; i < ENV_MAX; i++) {
        if (env_table[i].used && strcmp(env_table[i].name, name) == 0) {
            env_table[i].used = 0;
            return 0;
        }
    }
    return -1;
}

int shell_env_count(void) {
    return ENV_MAX;
}

const char *shell_env_name(int index) {
    if (index < 0 || index >= ENV_MAX || !env_table[index].used) {
        return NULL;
    }
    return env_table[index].name;
}

const char *shell_env_value(int index) {
    if (index < 0 || index >= ENV_MAX || !env_table[index].used) {
        return NULL;
    }
    return env_table[index].value;
}

int shell_set_alias(const char *name, const char *value) {
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (alias_table[i].used && strcmp(alias_table[i].name, name) == 0) {
            strlcpy(alias_table[i].value, value, VALUE_MAX);
            return 0;
        }
    }
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (!alias_table[i].used) {
            alias_table[i].used = 1;
            strlcpy(alias_table[i].name, name, NAME_MAX);
            strlcpy(alias_table[i].value, value, VALUE_MAX);
            return 0;
        }
    }
    return -1;
}

int shell_remove_alias(const char *name) {
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (alias_table[i].used && strcmp(alias_table[i].name, name) == 0) {
            alias_table[i].used = 0;
            return 0;
        }
    }
    return -1;
}

const char *shell_get_alias(const char *name) {
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (alias_table[i].used && strcmp(alias_table[i].name, name) == 0) {
            return alias_table[i].value;
        }
    }
    return NULL;
}

int shell_alias_count(void) {
    return ALIAS_MAX;
}

const char *shell_alias_name(int index) {
    if (index < 0 || index >= ALIAS_MAX || !alias_table[index].used) {
        return NULL;
    }
    return alias_table[index].name;
}

const char *shell_alias_value(int index) {
    if (index < 0 || index >= ALIAS_MAX || !alias_table[index].used) {
        return NULL;
    }
    return alias_table[index].value;
}

int shell_history_count(void) {
    return history_count;
}

const char *shell_history_get(int index) {
    if (index < 0 || index >= history_count) {
        return NULL;
    }
    return history[index];
}

void shell_history_clear(void) {
    history_count = 0;
}

static void history_add(const char *line) {
    if (!*line) {
        return;
    }
    if (history_count > 0 && strcmp(history[history_count - 1], line) == 0) {
        return;
    }
    if (history_count == HISTORY_MAX) {
        for (int i = 1; i < HISTORY_MAX; i++) {
            strlcpy(history[i - 1], history[i], SHELL_LINE_MAX);
        }
        history_count--;
    }
    strlcpy(history[history_count++], line, SHELL_LINE_MAX);
}

static void serial_cursor_forward(uint32_t n) {
    char buf[24];
    if (n == 0) {
        return;
    }
    snprintf(buf, sizeof(buf), "\x1b[%uC", n);
    serial_write(buf);
}

static void editor_redraw(const char *prompt, const char *buf, size_t pos,
                          size_t *home_row, size_t home_col, size_t *last_len) {
    uint32_t scroll_before = vga_scroll_count();
    size_t len = strlen(buf);

    vga_set_cursor(home_col, *home_row);
    for (size_t i = 0; i < len; i++) {
        vga_putchar(buf[i]);
    }
    for (size_t i = len; i < *last_len; i++) {
        vga_putchar(' ');
    }

    uint32_t scrolled = vga_scroll_count() - scroll_before;
    if (scrolled) {
        *home_row = (*home_row > scrolled) ? *home_row - scrolled : 0;
    }
    *last_len = len;

    size_t absolute = home_col + pos;
    size_t row = *home_row + absolute / VGA_WIDTH;
    size_t col = absolute % VGA_WIDTH;
    vga_set_cursor(col, row);

    serial_write("\r\x1b[K");
    serial_write(prompt);
    serial_write(buf);
    serial_write("\r");
    serial_cursor_forward((uint32_t)(strlen(prompt) + pos));
}

struct complete_ctx {
    const char *tail;
    size_t tlen;
    char match[VFS_NAME_MAX];
    int matches;
};

static int complete_cb(const char *name, uint8_t type, void *ctx) {
    struct complete_ctx *cc = (struct complete_ctx *)ctx;

    if (strncmp(name, cc->tail, cc->tlen) == 0) {
        strlcpy(cc->match, name, sizeof(cc->match));
        cc->matches++;
    }
    return 0;
}

static void complete_token(char *buf, size_t *len, size_t *pos, int first_token) {
    size_t start = *pos;
    while (start > 0 && buf[start - 1] != ' ') {
        start--;
    }

    char fragment[VFS_NAME_MAX];
    size_t flen = *pos - start;
    if (flen >= VFS_NAME_MAX) {
        return;
    }
    memcpy(fragment, buf + start, flen);
    fragment[flen] = '\0';

    const char *match = NULL;
    int matches = 0;
    struct complete_ctx cc;

    if (first_token) {
        for (int i = 0; i < command_count; i++) {
            if (strncmp(commands[i].name, fragment, flen) == 0) {
                match = commands[i].name;
                matches++;
            }
        }
    } else {
        char dirpath[VFS_PATH_MAX];
        char *slash = strrchr(fragment, '/');

        cc.tail = fragment;
        strcpy(dirpath, ".");
        if (slash) {
            strlcpy(dirpath, fragment, sizeof(dirpath));
            dirpath[slash - fragment + 1] = '\0';
            cc.tail = slash + 1;
        }
        cc.tlen = strlen(cc.tail);
        cc.match[0] = '\0';
        cc.matches = 0;
        if (vfs_readdir(dirpath, complete_cb, &cc) < 0) {
            return;
        }
        match = cc.match;
        matches = cc.matches;
        if (matches == 1) {
            flen = cc.tlen;
        }
    }

    if (matches != 1 || !match) {
        return;
    }

    size_t mlen = strlen(match);
    size_t extra = mlen - flen;
    if (*len + extra + 1 >= SHELL_LINE_MAX) {
        return;
    }

    memmove(buf + *pos + extra, buf + *pos, *len - *pos + 1);
    memcpy(buf + *pos, match + flen, extra);
    *len += extra;
    *pos += extra;
}

void shell_readline(const char *prompt, char *buf) {
    size_t len = 0;
    size_t pos = 0;
    size_t last_len = 0;
    int hist_index = history_count;
    char saved[SHELL_LINE_MAX];

    buf[0] = '\0';
    saved[0] = '\0';

    console_write(prompt);

    size_t home_row, home_col;
    vga_get_cursor(&home_col, &home_row);
    uint32_t epoch = console_epoch();

    for (;;) {
        int key = input_getkey();

        if (console_epoch() != epoch) {
            size_t cur_col, cur_row;
            vga_get_cursor(&cur_col, &cur_row);
            if (cur_col != 0) {
                console_putchar('\n');
            }
            console_write(prompt);
            vga_get_cursor(&home_col, &home_row);
            last_len = 0;
            epoch = console_epoch();
        }

        if (key == '\n') {
            vga_set_cursor(home_col, home_row);
            for (size_t i = 0; i < len; i++) {
                vga_putchar(buf[i]);
            }
            console_putchar('\n');
            return;
        }

        switch (key) {
            case '\b':
                if (pos > 0) {
                    memmove(buf + pos - 1, buf + pos, len - pos + 1);
                    pos--;
                    len--;
                }
                break;
            case KEY_DELETE:
                if (pos < len) {
                    memmove(buf + pos, buf + pos + 1, len - pos);
                    len--;
                }
                break;
            case KEY_LEFT:
                if (pos > 0) {
                    pos--;
                }
                break;
            case KEY_RIGHT:
                if (pos < len) {
                    pos++;
                }
                break;
            case KEY_HOME:
            case 1:
                pos = 0;
                break;
            case KEY_END:
            case 5:
                pos = len;
                break;
            case KEY_UP:
                if (hist_index > 0) {
                    if (hist_index == history_count) {
                        strlcpy(saved, buf, SHELL_LINE_MAX);
                    }
                    hist_index--;
                    strlcpy(buf, history[hist_index], SHELL_LINE_MAX);
                    len = strlen(buf);
                    pos = len;
                }
                break;
            case KEY_DOWN:
                if (hist_index < history_count) {
                    hist_index++;
                    if (hist_index == history_count) {
                        strlcpy(buf, saved, SHELL_LINE_MAX);
                    } else {
                        strlcpy(buf, history[hist_index], SHELL_LINE_MAX);
                    }
                    len = strlen(buf);
                    pos = len;
                }
                break;
            case '\t': {
                int first = 1;
                for (size_t i = 0; i < pos; i++) {
                    if (buf[i] == ' ') {
                        first = 0;
                        break;
                    }
                }
                complete_token(buf, &len, &pos, first);
                break;
            }
            case 3:
                buf[0] = '\0';
                len = 0;
                pos = 0;
                console_write("^C\n");
                return;
            case 11:
                buf[pos] = '\0';
                len = pos;
                break;
            case 21:
                memmove(buf, buf + pos, len - pos + 1);
                len -= pos;
                pos = 0;
                break;
            case 12:
                console_clear();
                console_write(prompt);
                vga_get_cursor(&home_col, &home_row);
                last_len = 0;
                epoch = console_epoch();
                break;
            default:
                if (key >= 32 && key < 127 && len + 1 < SHELL_LINE_MAX) {
                    memmove(buf + pos + 1, buf + pos, len - pos + 1);
                    buf[pos] = (char)key;
                    pos++;
                    len++;
                }
                break;
        }

        editor_redraw(prompt, buf, pos, &home_row, home_col, &last_len);
    }
}

static int is_quote(char c) {
    return c == '\'' || c == '"';
}

static int is_all_digits(const char *s) {
    if (!*s) {
        return 0;
    }
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }
    return 1;
}

static void expand_variable(const char *name, char *dest, size_t *dlen, size_t max) {
    char numbuf[16];
    const char *value = NULL;

    if (strcmp(name, "?") == 0) {
        itoa(last_status, numbuf, 10);
        value = numbuf;
    } else if (strcmp(name, "#") == 0) {
        itoa(current_params ? current_params->argc - 1 : 0, numbuf, 10);
        value = numbuf;
    } else if (strcmp(name, "PWD") == 0) {
        value = vfs_getcwd();
    } else if (name[0] && is_all_digits(name)) {
        int idx = atoi(name);
        if (current_params && idx >= 0 && idx < current_params->argc) {
            value = current_params->argv[idx];
        }
    } else {
        value = shell_getenv(name);
    }

    if (!value) {
        return;
    }
    while (*value && *dlen + 1 < max) {
        dest[(*dlen)++] = *value++;
    }
}

static int tokenize(char *text, char **argv, char *infile, char *outfile, int *append) {
    int argc = 0;
    char *p = text;
    static char token[SHELL_LINE_MAX];

    infile[0] = '\0';
    outfile[0] = '\0';
    *append = 0;

    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }

        int redirect_in = 0;
        int redirect_out = 0;

        if (*p == '<') {
            redirect_in = 1;
            p++;
        } else if (*p == '>') {
            redirect_out = 1;
            p++;
            if (*p == '>') {
                *append = 1;
                p++;
            }
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        size_t tlen = 0;
        char quote = 0;

        while (*p) {
            if (quote) {
                if (*p == quote) {
                    quote = 0;
                    p++;
                    continue;
                }
            } else {
                if (*p == ' ' || *p == '\t' || *p == '<' || *p == '>') {
                    break;
                }
                if (is_quote(*p)) {
                    quote = *p;
                    p++;
                    continue;
                }
            }

            if (*p == '\\' && p[1]) {
                if (quote == '\'') {
                    if (tlen + 1 < SHELL_LINE_MAX) {
                        token[tlen++] = *p++;
                    }
                    continue;
                }
                if (quote == '"' && p[1] != '"' && p[1] != '\\' && p[1] != '$') {
                    if (tlen + 1 < SHELL_LINE_MAX) {
                        token[tlen++] = *p++;
                    }
                    continue;
                }
                p++;
                if (tlen + 1 < SHELL_LINE_MAX) {
                    token[tlen++] = *p++;
                }
                continue;
            }

            if (*p == '$' && quote != '\'') {
                p++;
                char name[NAME_MAX];
                size_t nlen = 0;
                if (*p == '?') {
                    name[nlen++] = *p++;
                } else if (*p == '#') {
                    name[nlen++] = *p++;
                } else {
                    while ((isalnum(*p) || *p == '_') && nlen + 1 < NAME_MAX) {
                        name[nlen++] = *p++;
                    }
                }
                name[nlen] = '\0';
                expand_variable(name, token, &tlen, SHELL_LINE_MAX);
                continue;
            }

            if (tlen + 1 < SHELL_LINE_MAX) {
                token[tlen++] = *p++;
            } else {
                p++;
            }
        }
        token[tlen] = '\0';

        if (redirect_in) {
            strlcpy(infile, token, VFS_PATH_MAX);
        } else if (redirect_out) {
            strlcpy(outfile, token, VFS_PATH_MAX);
        } else if (argc < SHELL_MAX_ARGS - 1) {
            argv[argc] = (char *)kmalloc(tlen + 1);
            if (!argv[argc]) {
                break;
            }
            memcpy(argv[argc], token, tlen + 1);
            argc++;
        }
    }

    argv[argc] = NULL;
    return argc;
}

static void free_argv(char **argv, int argc) {
    for (int i = 0; i < argc; i++) {
        kfree(argv[i]);
    }
}

static int apply_alias(char **argv, int argc) {
    if (argc == 0) {
        return argc;
    }
    const char *alias = shell_get_alias(argv[0]);
    if (!alias) {
        return argc;
    }

    char expansion[VALUE_MAX];
    strlcpy(expansion, alias, sizeof(expansion));

    char *words[SHELL_MAX_ARGS];
    int wcount = 0;
    char *p = expansion;

    while (*p && wcount < SHELL_MAX_ARGS - 1) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        words[wcount++] = p;
        while (*p && *p != ' ') {
            p++;
        }
        if (*p) {
            *p++ = '\0';
        }
    }
    if (wcount == 0) {
        return argc;
    }

    char *newargv[SHELL_MAX_ARGS];
    int newc = 0;
    for (int i = 0; i < wcount && newc < SHELL_MAX_ARGS - 1; i++) {
        newargv[newc++] = strdup(words[i]);
    }
    for (int i = 1; i < argc && newc < SHELL_MAX_ARGS - 1; i++) {
        newargv[newc++] = argv[i];
    }

    kfree(argv[0]);
    for (int i = 0; i < newc; i++) {
        argv[i] = newargv[i];
    }
    argv[newc] = NULL;
    return newc;
}

struct membuf {
    uint8_t *data;
    uint32_t len;
    uint32_t pos;
};

static int membuf_read(struct vfs_file *f, void *buf, uint32_t len) {
    struct membuf *m = (struct membuf *)f->priv;
    uint32_t avail = m->len - m->pos;
    uint32_t n = len < avail ? len : avail;

    if (n > 0) {
        memcpy(buf, m->data + m->pos, n);
        m->pos += n;
    }
    return (int)n;
}

static void membuf_close(struct vfs_file *f) {
    struct membuf *m = (struct membuf *)f->priv;

    kfree(m->data);
    kfree(m);
}

static const struct fs_ops membuf_ops = {
    .name = "membuf",
    .read = membuf_read,
    .close = membuf_close,
};

static struct vfs_mount membuf_mount = {
    .ops = &membuf_ops,
};

static struct vfs_file *membuf_from_stream(struct stream *s) {
    uint32_t len = st_len(s);
    uint8_t *copy = (uint8_t *)kmalloc(len ? len : 1);
    struct membuf *m;
    struct vfs_file *f;

    if (!copy) {
        return NULL;
    }
    if (len) {
        memcpy(copy, st_data(s), len);
    }
    m = (struct membuf *)kmalloc(sizeof(struct membuf));
    f = (struct vfs_file *)kmalloc(sizeof(struct vfs_file));
    if (!m || !f) {
        kfree(copy);
        kfree(m);
        kfree(f);
        return NULL;
    }
    m->data = copy;
    m->len = len;
    m->pos = 0;
    f->mnt = &membuf_mount;
    f->priv = m;
    f->pos = 0;
    f->flags = VFS_O_READ;
    f->refs = 1;
    return f;
}

static void drain_pipe_into_stream(struct vfs_file *f, struct stream *s) {
    char buf[256];
    int n;

    while ((n = vfs_read(f, buf, sizeof(buf))) > 0) {
        st_write(s, buf, (uint32_t)n);
    }
}

static int stage_is_external(const char *text) {
    char scratch[SHELL_LINE_MAX];
    char *argv[SHELL_MAX_ARGS];
    char infile[VFS_PATH_MAX];
    char outfile[VFS_PATH_MAX];
    int append = 0;

    strncpy(scratch, text, sizeof(scratch) - 1);
    scratch[sizeof(scratch) - 1] = 0;

    int argc = tokenize(scratch, argv, infile, outfile, &append);
    if (argc == 0) {
        free_argv(argv, argc);
        return 0;
    }
    argc = apply_alias(argv, argc);
    int external = argc > 0 && !shell_find_command(argv[0]);

    free_argv(argv, argc);
    return external;
}

static int stage_is_background(const char *text) {
    char scratch[SHELL_LINE_MAX];
    char *argv[SHELL_MAX_ARGS];
    char infile[VFS_PATH_MAX];
    char outfile[VFS_PATH_MAX];
    int append = 0;

    strncpy(scratch, text, sizeof(scratch) - 1);
    scratch[sizeof(scratch) - 1] = 0;

    int argc = tokenize(scratch, argv, infile, outfile, &append);
    int bg = argc > 1 && strcmp(argv[argc - 1], "&") == 0;

    free_argv(argv, argc);
    return bg;
}

static int reap_chain(struct task **chain, int *n) {
    int status = 0;

    for (int i = 0; i < *n; i++) {
        status = user_wait_foreground(chain[i]->pid);
    }
    *n = 0;
    return status;
}

static int run_stage(char *text, struct stream *in, struct stream *out,
                      struct vfs_file *fin, struct vfs_file *fout,
                      int defer, struct task **spawned) {
    char *argv[SHELL_MAX_ARGS];
    char infile[VFS_PATH_MAX];
    char outfile[VFS_PATH_MAX];
    int append = 0;

    int argc = tokenize(text, argv, infile, outfile, &append);
    if (argc == 0) {
        free_argv(argv, argc);
        if (fin) {
            vfs_close(fin);
        }
        if (fout) {
            vfs_close(fout);
        }
        return 0;
    }

    argc = apply_alias(argv, argc);

    int background = 0;
    if (argc > 1 && strcmp(argv[argc - 1], "&") == 0) {
        kfree(argv[argc - 1]);
        argc--;
        argv[argc] = NULL;
        background = 1;
    }

    const struct command *cmd = shell_find_command(argv[0]);
    if (!cmd) {
        struct vfs_file *use_fin = fin;
        struct vfs_file *use_fout = fout;

        if (infile[0]) {
            if (use_fin) {
                vfs_close(use_fin);
                use_fin = NULL;
            }
            int r = vfs_open(infile, VFS_O_READ, &use_fin);
            if (r < 0) {
                st_printf(stream_console(), "%s: %s: %s\n", argv[0], infile, vfs_strerror(r));
                if (use_fout) {
                    vfs_close(use_fout);
                }
                free_argv(argv, argc);
                return 1;
            }
        }
        if (outfile[0]) {
            if (use_fout) {
                vfs_close(use_fout);
                use_fout = NULL;
            }
            int oflags = VFS_O_WRITE | VFS_O_CREATE | (append ? VFS_O_APPEND : VFS_O_TRUNC);
            int r = vfs_open(outfile, oflags, &use_fout);
            if (r < 0) {
                st_printf(stream_console(), "%s: cannot write to %s: %s\n", argv[0], outfile, vfs_strerror(r));
                if (use_fin) {
                    vfs_close(use_fin);
                }
                free_argv(argv, argc);
                return 1;
            }
        }
        if (!user_program_exists(argv[0])) {
            if (use_fin) {
                vfs_close(use_fin);
            }
            if (use_fout) {
                vfs_close(use_fout);
            }
            char script_path[VFS_PATH_MAX];
            if (script_resolve_path(argv[0], script_path, sizeof(script_path))) {
                int status = script_run_command(script_path, argc, argv, background);
                free_argv(argv, argc);
                return status;
            }
            st_printf(stream_console(), "%s: command not found\n", argv[0]);
            free_argv(argv, argc);
            return 127;
        }
        struct task *t = user_spawn_piped(argc, argv, use_fin, use_fout);
        if (!t) {
            st_printf(stream_console(), "%s: cannot execute\n", argv[0]);
            free_argv(argv, argc);
            return 126;
        }
        if (background) {
            st_printf(stream_console(), "[%d] %s\n", t->pid, t->name);
            free_argv(argv, argc);
            return 0;
        }
        if (defer && spawned) {
            *spawned = t;
            free_argv(argv, argc);
            return 0;
        }
        int status = user_wait_foreground(t->pid);
        free_argv(argv, argc);
        return status;
    }

    if (fin) {
        vfs_close(fin);
    }
    if (fout) {
        vfs_close(fout);
    }

    struct stream *actual_in = in;
    struct stream *file_in = NULL;

    if (infile[0]) {
        void *data;
        uint32_t size;
        int r = vfs_load(infile, &data, &size);
        if (r < 0) {
            st_printf(stream_console(), "%s: %s: %s\n", argv[0], infile, vfs_strerror(r));
            free_argv(argv, argc);
            return 1;
        }
        file_in = stream_new();
        if (file_in) {
            st_write(file_in, (const char *)data, size);
            actual_in = file_in;
        }
        kfree(data);
    }

    struct stream *actual_out = out;
    struct stream *file_out = NULL;

    if (outfile[0]) {
        file_out = stream_new();
        actual_out = file_out;
    }

    background_request = background;
    int status = cmd->fn(argc, argv, actual_in, actual_out);
    background_request = 0;

    if (file_out) {
        int r = append ? vfs_append(outfile, st_data(file_out), st_len(file_out))
                       : vfs_save(outfile, st_data(file_out), st_len(file_out));
        if (r < 0) {
            st_printf(stream_console(), "%s: cannot write to %s: %s\n", argv[0], outfile, vfs_strerror(r));
            status = 1;
        }
        stream_free(file_out);
    }

    if (file_in) {
        stream_free(file_in);
    }

    free_argv(argv, argc);
    return status;
}

static int run_pipeline(char *segment) {
    char *stages[PIPE_MAX];
    int count = 0;
    char quote = 0;

    stages[count++] = segment;
    for (char *p = segment; *p; p++) {
        if (quote) {
            if (*p == quote) {
                quote = 0;
            }
            continue;
        }
        if (is_quote(*p)) {
            quote = *p;
            continue;
        }
        if (*p == '|' && count < PIPE_MAX) {
            *p = '\0';
            stages[count++] = p + 1;
        }
    }

    struct stream *carry_stream = NULL;
    struct vfs_file *carry_pipe = NULL;
    struct task *chain[PIPE_MAX];
    int chain_n = 0;
    int status = 0;

    for (int i = 0; i < count; i++) {
        int is_last = (i == count - 1);
        int is_ext = stage_is_external(stages[i]);

        struct stream *stage_in = carry_stream;
        struct vfs_file *fin = carry_pipe;

        carry_stream = NULL;
        carry_pipe = NULL;

        if (fin && !is_ext) {
            stage_in = stream_new();
            drain_pipe_into_stream(fin, stage_in);
            vfs_close(fin);
            fin = NULL;
            reap_chain(chain, &chain_n);
        }
        if (stage_in && is_ext) {
            fin = membuf_from_stream(stage_in);
            stream_free(stage_in);
            stage_in = NULL;
        }

        struct vfs_file *fout = NULL;
        int defer = 0;

        if (is_ext && !is_last) {
            struct vfs_file *w = NULL;

            if (user_pipe_create(&carry_pipe, &w) == 0) {
                fout = w;
                defer = 1;
            }
        }

        struct stream *stage_out;
        if (is_ext) {
            stage_out = NULL;
        } else {
            stage_out = is_last ? stream_console() : stream_new();
        }

        struct task *spawned = NULL;
        status = run_stage(stages[i], stage_in, stage_out, fin, fout, defer, &spawned);

        if (spawned) {
            chain[chain_n++] = spawned;
        }
        if (stage_in && stage_in != carry_stream) {
            stream_free(stage_in);
        }
        if (!is_ext) {
            if (!is_last) {
                carry_stream = stage_out;
            }
        }
    }

    if (chain_n && !stage_is_background(stages[count - 1])) {
        reap_chain(chain, &chain_n);
    }
    if (carry_stream) {
        stream_free(carry_stream);
    }
    if (carry_pipe) {
        vfs_close(carry_pipe);
    }
    return status;
}

int shell_run_line(const char *line) {
    char work[SHELL_LINE_MAX];
    strlcpy(work, line, sizeof(work));

    char *p = work;
    char *segment = work;
    char quote = 0;
    int skip_next = 0;
    int status = 0;
    int pending = 0;

    for (;;) {
        char op = 0;

        while (*p) {
            if (quote) {
                if (*p == quote) {
                    quote = 0;
                }
                p++;
                continue;
            }
            if (is_quote(*p)) {
                quote = *p;
                p++;
                continue;
            }
            if (*p == ';') {
                op = ';';
                break;
            }
            if (*p == '&' && p[1] == '&') {
                op = '&';
                break;
            }
            if (*p == '|' && p[1] == '|') {
                op = 'o';
                break;
            }
            p++;
        }

        char *next = NULL;
        if (op == ';') {
            *p = '\0';
            next = p + 1;
        } else if (op == '&' || op == 'o') {
            *p = '\0';
            next = p + 2;
        }

        int run = 1;
        if (skip_next) {
            run = 0;
        }
        if (run) {
            int empty = 1;
            for (char *q = segment; *q; q++) {
                if (*q != ' ' && *q != '\t') {
                    empty = 0;
                    break;
                }
            }
            if (!empty) {
                status = run_pipeline(segment);
                last_status = status;
                pending = 1;
            }
        }

        if (!next) {
            break;
        }

        if (op == '&') {
            skip_next = (pending && status != 0) || skip_next;
        } else if (op == 'o') {
            skip_next = (pending && status == 0) || skip_next;
        } else {
            skip_next = 0;
        }

        segment = next;
        p = next;
    }

    return status;
}

static void build_prompt(char *buf, size_t size) {
    const char *path = vfs_getcwd();
    const char *user = shell_getenv("USER");
    const char *host = shell_getenv("HOSTNAME");

    snprintf(buf, size, "%s@%s:%s$ ",
             user ? user : "root",
             host ? host : "felinos",
             path);
}

void shell_init(void) {
    vfs_chdir("/");
    history_count = 0;
    last_status = 0;
    exit_requested = 0;

    shell_setenv("USER", "root");
    shell_setenv("HOSTNAME", "felinos");
    shell_setenv("HOME", "/root");
    shell_setenv("SHELL", "vsh");
    shell_setenv("OS", "FelinOS");
    shell_setenv("KERNEL", "Gato");
    shell_setenv("TERM", "vga");

    shell_set_alias("ll", "ls -l");
    shell_set_alias("la", "ls -a");
    shell_set_alias("xxd", "hexdump");
    shell_set_alias("edit", "nano");
    shell_set_alias("cls", "clear");

    vfs_chdir("/root");
}

void shell_run(void) {
    char line[SHELL_LINE_MAX];
    char prompt[VFS_PATH_MAX + 64];

    void *motd;
    uint32_t motd_size;
    if (vfs_load("/etc/motd", &motd, &motd_size) == 0) {
        console_write((const char *)motd);
        console_putchar('\n');
        kfree(motd);
    }

    const char *home = shell_getenv("HOME");
    if (home) {
        char rc_path[VFS_PATH_MAX];
        snprintf(rc_path, sizeof(rc_path), "%s/.vshrc", home);
        script_run_file(rc_path, 0, NULL);
    }

    while (!exit_requested) {
        user_report_children();
        build_prompt(prompt, sizeof(prompt));
        shell_readline(prompt, line);
        if (line[0]) {
            history_add(line);
            shell_run_line(line);
        }
    }

    console_write("Shell session ended. Press any key to restart it.\n");
    input_getkey();
    exit_requested = 0;
}
