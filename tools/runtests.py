#!/usr/bin/env python3
"""Drive FelinOS under QEMU, run the in-kernel test suite, report the result.

'qemu -kernel gato.bin' boots straight into the shell, so the suite has to be
triggered by typing it on the console. A shell pipe is not enough: the suite
prints a summary and then leaves the machine at a prompt, so there is nothing
to wait for and no way to learn whether it passed. This script talks to the
guest over a pipe pair, waits for it to settle at a prompt, runs 'selftest', reads the
summary back, and then powers the machine off so QEMU exits on its own.

Exit status is the test result: 0 if every test passed, 1 if any failed,
2 if the guest panicked or never reached the shell, 124 on timeout.
"""

import os
import re
import select
import subprocess
import sys
import time

PROMPT = re.compile(r"root@felinos:\S*\s*\$\s")
SUMMARY = re.compile(r"^Checks:\s+(\d+)\s+Passed:\s+(\d+)\s+Failed:\s+(\d+)\s*$", re.M)
PANIC = re.compile(r"KERNEL PANIC|Exception \d+:")
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

# Booting with the four attached disks means formatting a 4 GB sparse GatoFS
# volume, so the wait for the prompt has to be generous.
BOOT_TIMEOUT = 300.0
TEST_TIMEOUT = 300.0
POWEROFF_TIMEOUT = 30.0
QUIET_BEFORE_TYPING = 3.0

GREEN, RED, DIM, OFF = "\033[32m", "\033[31m", "\033[2m", "\033[0m"


def log(msg):
    print(f"{DIM}test:{OFF} {msg}", flush=True)


def clean(raw):
    return ANSI.sub("", raw.decode("utf-8", errors="replace")).replace("\r", "")


class Guest:
    def __init__(self, qemu_args):
        # Plain pipes, not a pty. A pty makes the guest's console talk to a
        # terminal, which brings in line discipline, window size and flow
        # control; the guest's UART busy-wait on the transmit register then
        # interacts with all of it, and the boot stops mid-banner. Two pipes
        # give the same full-duplex channel with none of that.
        in_r, in_w = os.pipe()
        out_r, out_w = os.pipe()

        # The guest's only console is COM1. Without this QEMU gives the serial
        # port nowhere to go and the guest looks mute, so the flag is added
        # here rather than left to the caller to remember.
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
        """Drain whatever the guest has written. False once it closes stdout."""
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
            os.write(self.to_guest, line.encode())
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
    """Block until the guest is sitting at an idle shell prompt.

    The prompt itself is the signal, but the fallback is a quiet period: the
    console goes silent once the shell is up, and that holds even if the prompt
    string is ever reformatted.
    """
    deadline = time.monotonic() + BOOT_TIMEOUT
    while time.monotonic() < deadline:
        if not guest.pump():
            return False
        if PANIC.search(guest.text()):
            return False
        if PROMPT.search(guest.text()[-300:]):
            return True
        # The guest can also go quiet at the prompt without the exact string
        # showing up, so a silent console counts as ready too.
        if time.monotonic() - guest.last_output > QUIET_BEFORE_TYPING:
            return True
        time.sleep(0.1)
    return False


def main():
    if len(sys.argv) < 2:
        print("usage: runtests.py <qemu-args...>", file=sys.stderr)
        return 2

    guest = Guest(sys.argv[1:])
    summary = None
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
        log("shell reached, running selftest")
        guest.send("selftest\n")

        deadline = time.monotonic() + TEST_TIMEOUT
        while time.monotonic() < deadline:
            if not guest.pump():
                break
            match = SUMMARY.search(guest.text())
            if match:
                summary = tuple(int(g) for g in match.groups())
                break
            if PANIC.search(guest.text()):
                print(guest.take(), end="", flush=True)
                log("guest panicked during the suite")
                return 2
            time.sleep(0.1)

        if summary is None:
            text = guest.take()
            if text.strip():
                print(text, end="", flush=True)
            log(f"no test summary after {TEST_TIMEOUT:.0f}s")
            return 124

        print(guest.take(), end="", flush=True)
        log("suite finished, powering off")
        guest.send("poweroff\n")

        deadline = time.monotonic() + POWEROFF_TIMEOUT
        while time.monotonic() < deadline and guest.proc.poll() is None:
            guest.pump()
            time.sleep(0.1)
        if guest.proc.poll() is None:
            log("guest did not power off; terminating QEMU")
    finally:
        stderr = guest.close()

    if stderr and stderr.strip():
        print(stderr, file=sys.stderr, end="")

    run, passed, failed = summary
    print()
    if failed == 0 and run > 0:
        print(f"{GREEN}make test: {passed}/{run} passed{OFF}")
        return 0
    print(f"{RED}make test: {failed} of {run} failed{OFF}")
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        sys.exit(130)
