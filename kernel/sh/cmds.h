#ifndef FELINOS_CMDS_H
#define FELINOS_CMDS_H

#include "sh/shell.h"
#include "sh/stream.h"
#include "fs/vfs.h"

void cmd_error(struct stream *out, const char *cmd, const char *target, const char *msg);
void cmd_vfs_error(struct stream *out, const char *cmd, const char *target, int err);
char *cmd_collect_input(int argc, char **argv, int start, struct stream *in,
                        struct stream *out, const char *cmdname, uint32_t *len_out);
int cmd_split_lines(char *text, char ***lines_out);
void cmd_format_size(uint32_t bytes, char *buf, uint32_t size);
void cmd_format_size64(uint64_t bytes, char *buf, uint32_t size);
void cmd_format_time(uint32_t stamp, char *buf, uint32_t size);
void cmd_mode_string(uint8_t type, uint16_t mode, char *buf);
int cmd_has_flag(int argc, char **argv, const char *flag);
int cmd_is_flag(const char *arg);

int cmd_ls(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_cd(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_pwd(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_mkdir(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_rmdir(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_touch(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_rm(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_cp(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_mv(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_cat(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_tree(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_find(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_stat(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_du(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_df(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_mount(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_umount(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_chmod(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_chown(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_login(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_su(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_passwd(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_useradd(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_id(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_source(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_file(int argc, char **argv, struct stream *in, struct stream *out);

int cmd_ifconfig(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_ping(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_dhcp(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_dns(int argc, char **argv, struct stream *in, struct stream *out);

int cmd_echo(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_head(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_tail(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_wc(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_grep(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_sort(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_uniq(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_cut(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_tr(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_rev(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_tee(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_nl(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_tac(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_more(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_hexdump(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_strings(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_diff(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_printf(int argc, char **argv, struct stream *in, struct stream *out);

int cmd_exec(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_help(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_man(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_uname(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_uptime(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_date(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_cal(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_free(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_ps(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_kill(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_renice(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_sched(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_top(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_fg(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_dmesg(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_whoami(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_credits(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_hostname(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_acpi(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_reboot(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_poweroff(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_halt(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_sleep(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_beep(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_tune(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_lscpu(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_smp(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_lsmem(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_clear(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_color(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_history(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_alias(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_unalias(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_env(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_export(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_unset(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_which(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_exit(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_true(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_false(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_test(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_expr(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_seq(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_yes(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_basename(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_dirname(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_time(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_sync(int argc, char **argv, struct stream *in, struct stream *out);

int cmd_lsblk(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_lspci(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_blkid(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_bcache(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_fdisk(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_gatofs(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_fat32(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_dd(int argc, char **argv, struct stream *in, struct stream *out);

int cmd_vmm(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_pmap(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_vmstat(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_mkswap(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_swapon(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_swapoff(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_swapinfo(int argc, char **argv, struct stream *in, struct stream *out);
int cmd_memtest(int argc, char **argv, struct stream *in, struct stream *out);

int cmd_nano(int argc, char **argv, struct stream *in, struct stream *out);

int cmd_simplecc(int argc, char **argv, struct stream *in, struct stream *out);

int cmd_selftest(int argc, char **argv, struct stream *in, struct stream *out);

#endif
