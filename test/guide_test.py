#!/usr/bin/env python3
"""Checks the sessions and programs in the docs against the real system.

    test/guide_test.py [FILE.md...]     (default: README.md and
                                         docs/getting-started.md)

A fenced block right after a line `<!-- guide-test: WHAT -->` is checked:

- `greet.bastio`: a user program. It is proved and compiled with BASTIOC
  for the user target, then linked by the Makefile's own rules, as
  `make build/user/greet.elf` would (here as build/user/guide_greet.elf, so
  a reader's own user/greet.bastio is left alone).
- `hi.c`: a C program, built with `musl-gcc -static-pie -O2`.
- `session [FILE...]`: a terminal session. The kernel boots with a disk
  holding the named files (`greet`, `hi`; none: a blank disk), each line
  that starts with a prompt (`bastios> ` or `# `) is typed once that
  prompt appears, and the console output from the first prompt on must
  match the block. A line `...` matches any number of lines.

Environment: KERNEL (build/bastios.elf), BASTIOC, BASTIOLIB, MUSLCC, TIMEOUT
(seconds per prompt, 60).
"""
import os, re, select, subprocess, sys, tempfile, time

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
GUIDES = sys.argv[1:] or ["README.md", "docs/getting-started.md"]
KERNEL = os.environ.get("KERNEL", "build/bastios.elf")
BASTIOC = os.environ.get("BASTIOC", "../bastiolang/stage0/target/release/bastioc0")
BASTIOLIB = os.environ.get("BASTIOLIB", "../bastiolang")
MUSLCC = os.environ.get("MUSLCC", "musl-gcc")
TIMEOUT = float(os.environ.get("TIMEOUT", "60"))
PROMPTS = ("bastios> ", "# ")


def blocks(path):
    """(what, line number, body) for each marked block."""
    lines = open(path).read().split("\n")
    out = []
    for i, line in enumerate(lines):
        m = re.match(r"^<!-- guide-test: (.+?) -->$", line)
        if not m:
            continue
        if i + 1 >= len(lines) or not lines[i + 1].startswith("```"):
            sys.exit(f"{path}:{i + 1}: a guide-test marker must come right before a fenced block")
        j = i + 2
        while not lines[j].startswith("```"):
            j += 1
        out.append((m.group(1), i + 2, lines[i + 2:j]))
    return out


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        print(f"FAIL: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
        sys.exit(1)


def boot(disk, inputs):
    """Boots with `disk`, types each (prompt, line) in turn, returns the output."""
    p = subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", KERNEL, "-cpu", "qemu64,+pdpe1gb", "-m", "256M",
         "-smp", "2", "-display", "none", "-no-reboot",
         "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
         "-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd",
         "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0",
         "-serial", "stdio", "-monitor", "none"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    out = b""
    seen = 0                       # output up to here has been matched to a prompt

    def read_until(test):
        nonlocal out
        end = time.time() + TIMEOUT
        while not test():
            r, _, _ = select.select([p.stdout], [], [], max(0, end - time.time()))
            if not r:
                return False
            d = os.read(p.stdout.fileno(), 4096)
            if not d:
                return False
            out += d
        return True

    for prompt, line in inputs:
        pat = prompt.encode()
        if not read_until(lambda: out.find(pat, seen) >= 0):
            p.kill()
            return out.decode(errors="replace"), f"no prompt {prompt!r} before typing {line!r}"
        seen = out.find(pat, seen) + len(pat)
        p.stdin.write(line.encode() + b"\r")
        p.stdin.flush()
    try:
        rest, _ = p.communicate(timeout=TIMEOUT)
        out += rest
    except subprocess.TimeoutExpired:
        p.kill()
        return out.decode(errors="replace"), "the kernel did not power off"
    return out.decode(errors="replace"), None


def matches(want, got):
    pat = ""
    for line in want:
        pat += r"(?:.*\n)*?" if line == "..." else re.escape(line.rstrip()) + r"\n"
    return re.fullmatch(pat, got) is not None


def main():
    tmp = tempfile.mkdtemp()
    built = {}
    failed = 0
    sessions = []
    for what, line, body, guide in [b + (g,) for g in GUIDES for b in blocks(g)]:
        at = f"{guide}:{line}"
        if what == "greet.bastio":
            src = os.path.join(tmp, "greet.bastio")
            open(src, "w").write("\n".join(body) + "\n")
            run([BASTIOC, "--lib", BASTIOLIB, "--target", "user", "-o", "build/user/guide_greet.c", src])
            run(["make", "-s", f"BASTIOC={BASTIOC}", "build/user/guide_greet.elf"])
            built["greet"] = "build/user/guide_greet.elf"
        elif what == "hi.c":
            src = os.path.join(tmp, "hi.c")
            open(src, "w").write("\n".join(body) + "\n")
            run([MUSLCC, "-static-pie", "-O2", "-o", os.path.join(tmp, "hi"), src])
            built["hi"] = os.path.join(tmp, "hi")
        elif what.split()[0] == "session":
            sessions.append((at, what.split()[1:], body))
        else:
            sys.exit(f"{at}: unknown guide-test block `{what}`")
    for at, files, body in sessions:
        disk = os.path.join(tmp, "disk.img")
        if os.path.exists(disk):
            os.remove(disk)
        missing = [f for f in files if f not in built]
        if missing:
            print(f"FAIL {at}: the session needs {missing}, which no block builds")
            failed += 1
            continue
        if files:
            run(["test/compat/mkdisk.py", disk] + [f"{f}={built[f]}" for f in files])
        else:
            with open(disk, "wb") as f:
                f.truncate(33 << 20)
        inputs = []
        for l in body:
            for pr in PROMPTS:
                if l.startswith(pr):
                    inputs.append((pr, l[len(pr):]))
                    break
        out, err = boot(disk, inputs)
        text = out.replace("\r", "")
        first = text.find(body[0] + "\n")
        got = "\n".join(l.rstrip() for l in text[first:].split("\n")).rstrip("\n") + "\n" if first >= 0 else ""
        if err or not matches(body, got):
            failed += 1
            print(f"FAIL {at}: {err or 'the console differs from the session shown'}")
            print("    console:")
            for l in (got or text).rstrip("\n").split("\n"):
                print("    | " + l)
        else:
            print(f"ok   {at}: session {' '.join(files) or '(blank disk)'}")
    for f in ("build/user/guide_greet.c", "build/user/guide_greet.o", "build/user/guide_greet.elf"):
        if os.path.exists(f):
            os.remove(f)
    if failed:
        sys.exit(f"guide_test: {failed} session(s) differ from the docs")
    print(f"guide_test: every session in {', '.join(GUIDES)} matches")


main()
