#ifndef FELINOS_USER_H
#define FELINOS_USER_H

#include <stdint.h>
#include "idt.h"
#include "fs/vfs.h"
#include "sched.h"

#define USER_NOTFOUND (-1000)

void user_init(int install_missing);
int user_is_elf(const char *path);
struct task *user_spawn(const char *path, int argc, char **argv);
struct task *user_spawn_io(const char *path, int argc, char **argv,
                            struct vfs_file *fin, struct vfs_file *fout);
struct task *user_spawn_piped(int argc, char **argv, struct vfs_file *fin, struct vfs_file *fout);
int user_program_exists(const char *name);
int user_pipe_create(struct vfs_file **out_r, struct vfs_file **out_w);
int user_run_command(int argc, char **argv, int background);
int user_wait_foreground(int pid);
void user_report_children(void);
void syscall_dispatch(struct regs *r);
void user_fault(struct regs *r, const char *name, uint32_t cr2) __attribute__((noreturn));

#endif
