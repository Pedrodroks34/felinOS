#ifndef FELINOS_SHELL_H
#define FELINOS_SHELL_H

#include <stdint.h>
#include "sh/stream.h"
#include "fs/vfs.h"

#define SHELL_MAX_ARGS 32
#define SHELL_LINE_MAX 512

typedef int (*command_fn)(int argc, char **argv, struct stream *in, struct stream *out);

struct command {
    const char *name;
    const char *summary;
    const char *usage;
    command_fn fn;
};

struct shell_params {
    int argc;
    char *argv[10];
};

const struct shell_params *shell_set_params(const struct shell_params *p);

void shell_init(void);
void shell_run(void);
int shell_run_line(const char *line);
void shell_readline(const char *prompt, char *buf);

const struct command *shell_command_table(void);
int shell_command_count(void);
const struct command *shell_find_command(const char *name);

const char *shell_getenv(const char *name);
int shell_setenv(const char *name, const char *value);
int shell_unsetenv(const char *name);
int shell_env_count(void);
const char *shell_env_name(int index);
const char *shell_env_value(int index);

int shell_set_alias(const char *name, const char *value);
int shell_remove_alias(const char *name);
const char *shell_get_alias(const char *name);
int shell_alias_count(void);
const char *shell_alias_name(int index);
const char *shell_alias_value(int index);

int shell_history_count(void);
const char *shell_history_get(int index);
void shell_history_clear(void);

int shell_last_status(void);
void shell_request_exit(void);
int shell_take_background(void);

int script_run_file(const char *path, int argc, char **argv);
int script_run_command(const char *path, int argc, char **argv, int background);
int script_resolve_path(const char *name, char *out, size_t size);

#endif
