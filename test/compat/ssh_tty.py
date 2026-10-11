#!/usr/bin/env python3
"""Runs a command (ssh.txt's SSH client) on a terminal of its own, types
the given lines into it one by one, and prints what came out, without the
carriage returns. The terminal is 24 rows of 80 columns. The client needs a terminal to ask the server for one
(-t); ssh.txt's other cases run it on a plain pipe.

    ssh_tty.py [--wait S] 'line 1' 'line 2' ... -- command args...

Each line is typed once the output has been quiet for a moment (or after
S seconds at most), as a person would. Exits with the command's status."""
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import termios
import time


def main():
    args = sys.argv[1:]
    wait = 10.0
    if args[:1] == ["--wait"]:
        wait = float(args[1])
        args = args[2:]
    cut = args.index("--")
    lines, cmd = args[:cut], args[cut + 1:]
    pid, fd = pty.fork()
    if pid == 0:
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        os.execvp(cmd[0], cmd)
    out = b""

    def read(secs, quiet):
        """Reads for at most secs, or until nothing came for `quiet`
        seconds after something did. False at end of file."""
        nonlocal out
        end = time.time() + secs
        got = False
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], quiet if got else 0.2)
            if not r:
                if got:
                    return True
                continue
            try:
                b = os.read(fd, 4096)
            except OSError:
                return False
            if not b:
                return False
            out += b
            got = True
        return True

    alive = read(wait, 1.0)
    for line in lines:
        if not alive:
            break
        os.write(fd, line.encode() + b"\r")
        alive = read(wait, 1.0)
    end = time.time() + wait
    while alive and time.time() < end:
        alive = read(end - time.time(), wait)
    if alive:
        os.kill(pid, signal.SIGKILL)        # it never finished
    _, status = os.waitpid(pid, 0)
    sys.stdout.write(out.decode(errors="replace").replace("\r", ""))
    sys.exit(os.waitstatus_to_exitcode(status))


main()
