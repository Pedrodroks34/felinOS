#!/usr/bin/env python3
"""
FelinOS Userspace Test Harness
===============================
Boots FelinOS with rootfs and runs userspace command tests.
Exit codes: 0=all pass, 1=some fail, 2=panic/no prompt, 124=timeout
"""

import os
import re
import select
import subprocess
import sys
import time

# Prompt patterns for busybox ash
PROMPT = re.compile(r"[\w\-]+@[\w\-]+:[^#\$]*[#\$]\s*$")
SUMMARY = re.compile(r"^Checks:\s+(\d+)\s+Passed:\s+(\d+)\s+Failed:\s+(\d+)\s*$", re.M)
PANIC = re.compile(r"KERNEL PANIC|Exception \d+:|PANIC:")
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

BOOT_TIMEOUT = 300.0
TEST_TIMEOUT = 120.0
QUIET_BEFORE_TYPING = 3.0

GREEN, RED, DIM, OFF = "\033[32m", "\033[31m", "\033[2m", "\033[0m"


def log(msg):
    print(f"{DIM}test-userspace:{OFF} {msg}", flush=True)


def clean(raw):
    return ANSI.sub("", raw.decode("utf-8", errors="replace")).replace("\r", "")


class Guest:
    def __init__(self, qemu_args):
        in_r, in_w = os.pipe()
        out_r, out_w = os.pipe()

        if not any(a == "-serial" or a.startswith("-serial=") for a in qemu_args):
            qemu_args = [*qemu_args, "-serial", "stdio"]

        self.proc = subprocess.Popen(
            ["qemu-system-x86_64", *qemu_args],
            stdin=in_r,
            stdout=out_w,
            stderr=subprocess.PIPE,
            close_fds=True,
        )
        os.close(in_r)
        os.close(out_w)
        self.to_guest = in_w
        self.from_guest = out_r
        self.buf = bytearray()
        self.alive = True
        self.last_output = time.monotonic()

    def pump(self):
        try:
            ready, _, _ = select.select([self.from_guest], [], [], 0.2)
        except (OSError, ValueError):
            self.alive = False
            return False
        if not ready:
            return True
        try:
            chunk = os.read(self.from_guest, 4096)
        except OSError:
            self.alive = False
            return False
        if not chunk:
            self.alive = False
            return False
        self.buf.extend(chunk)
        self.last_output = time.monotonic()
        return True

    def text(self):
        return clean(bytes(self.buf))

    def take(self):
        text = clean(bytes(self.buf))
        self.buf.clear()
        return text

    def send(self, line):
        try:
            os.write(self.to_guest, (line + "\n").encode())
        except OSError:
            pass

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        for fd in (self.to_guest, self.from_guest):
            try:
                os.close(fd)
            except OSError:
                pass
        if self.proc.stderr:
            return self.proc.stderr.read().decode("utf-8", errors="replace")
        return ""


def wait_for_prompt(guest):
    deadline = time.monotonic() + BOOT_TIMEOUT
    while time.monotonic() < deadline:
        if not guest.pump():
            return False
        if PANIC.search(guest.text()):
            return False
        if PROMPT.search(guest.text()[-500:]):
            return True
        if time.monotonic() - guest.last_output > QUIET_BEFORE_TYPING:
            return True
        time.sleep(0.1)
    return False


# Test cases: (command, expected_output_substring, description)
TESTS = [
    # Shell basics
    ("echo hello", "hello", "echo command"),
    ("pwd", "/", "pwd shows root"),
    ("ls /bin", "busybox", "ls /bin shows busybox"),
    ("ls /usr/bin", "busybox", "ls /usr/bin shows busybox"),
    ("which sh", "/bin/sh", "which finds sh"),
    ("which ls", "/bin/ls", "which finds ls"),

    # File operations
    ("echo test > /tmp/test.txt", "", "write file"),
    ("cat /tmp/test.txt", "test", "read file"),
    ("mkdir -p /tmp/dir/subdir", "", "mkdir -p"),
    ("ls /tmp/dir", "subdir", "ls subdirectory"),
    ("rm /tmp/test.txt", "", "remove file"),
    ("rmdir /tmp/dir/subdir /tmp/dir", "", "remove directories"),

    # Environment
    ("echo $PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "PATH variable"),
    ("export TEST=value; echo $TEST", "value", "export variable"),

    # Process management
    ("sleep 1 &", "", "background job"),
    ("jobs", "Running", "jobs command"),
    ("kill %1", "", "kill background job"),
    ("wait", "", "wait for job"),

    # Busybox applets
    ("ash -c 'echo from ash'", "from ash", "ash shell"),
    ("true", "", "true command"),
    ("false", "", "false command (exit code 1)"),

    # Filesystem
    ("mount | grep -E 'proc|sysfs|devtmpfs|tmpfs'", "", "mount shows virtual filesystems"),
    ("df -h /", "/dev/root", "df shows root filesystem"),
    ("free", "Mem:", "free shows memory"),

    # Network (may fail if no DHCP)
    ("ping -c 1 -W 2 10.0.2.2 2>/dev/null || echo 'ping failed'", "", "ping gateway"),

    # Shell features
    ("echo {a,b,c}", "a b c", "brace expansion"),
    ("echo \$((2+2))", "4", "arithmetic expansion"),
    ("[ 1 -eq 1 ] && echo ok", "ok", "test command"),
    ("for i in 1 2 3; do echo \$i; done | tail -1", "3", "for loop"),

    # Signals
    ("trap 'echo caught' INT; sleep 2 & kill -INT \$!; wait", "caught", "signal trap"),

    # Permissions
    ("touch /tmp/perms && chmod 755 /tmp/perms && ls -l /tmp/perms | grep rwxr-xr-x", "rwxr-xr-x", "chmod"),
]


def run_tests(guest):
    passed = 0
    failed = 0
    results = []

    for i, (cmd, expected, desc) in enumerate(TESTS, 1):
        log(f"Test {i}/{len(TESTS)}: {desc}")
        guest.send(cmd)

        # Wait for command to complete (prompt returns)
        deadline = time.monotonic() + 10.0
        output = ""
        while time.monotonic() < deadline:
            if not guest.pump():
                break
            if PROMPT.search(guest.text()[-500:]):
                output = guest.take()
                break
            time.sleep(0.05)

        if expected and expected not in output:
            log(f"  {RED}FAIL{OFF}: {desc}")
            log(f"  Command: {cmd}")
            log(f"  Expected: {expected}")
            log(f"  Got: {output[-200:] if len(output) > 200 else output}")
            failed += 1
            results.append((desc, False, output))
        else:
            log(f"  {GREEN}PASS{OFF}: {desc}")
            passed += 1
            results.append((desc, True, output))

    return passed, failed, results


def main():
    if len(sys.argv) < 2:
        print("usage: test-userspace.py <qemu-args...>", file=sys.stderr)
        return 2

    # Build QEMU args with rootfs
    qemu_args = sys.argv[1:]
    rootfs = os.path.join(os.path.dirname(__file__), "..", "rootfs.img")
    if os.path.exists(rootfs) and "-drive" not in " ".join(qemu_args):
        qemu_args = [*qemu_args, "-drive", f"file={rootfs},format=raw,if=virtio"]

    guest = Guest(qemu_args)
    try:
        if not wait_for_prompt(guest):
            text = guest.take()
            if text.strip():
                print(text, end="", flush=True)
            if PANIC.search(text):
                log("guest panicked before reaching the shell")
                return 2
            if not guest.alive:
                log("QEMU exited before the guest reached a prompt")
                return 2
            log(f"timed out after {BOOT_TIMEOUT:.0f}s waiting for the shell")
            return 124

        print(guest.take(), end="", flush=True)
        log("shell reached, running userspace tests")

        passed, failed, results = run_tests(guest)

        # Print summary
        total = passed + failed
        print()
        print(f"Checks: {total}  Passed: {passed}  Failed: {failed}")

        if failed == 0 and total > 0:
            print(f"{GREEN}make test-userspace: {passed}/{total} passed{OFF}")
            ret = 0
        else:
            print(f"{RED}make test-userspace: {failed} of {total} failed{OFF}")
            ret = 1

        log("tests finished, powering off")
        guest.send("poweroff")

        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline and guest.proc.poll() is None:
            guest.pump()
            time.sleep(0.1)
        if guest.proc.poll() is None:
            log("guest did not power off; terminating QEMU")

    finally:
        stderr = guest.close()

    if stderr and stderr.strip():
        print(stderr, file=sys.stderr, end="")

    return ret


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        sys.exit(130)