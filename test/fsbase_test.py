#!/usr/bin/env python3
"""The user's FS base across traps (boot/boot.S, boot/platform.c): on a
processor with RDFSBASE and WRFSBASE the kernel turns CR4.FSGSBASE on and
the trap path uses them instead of RDMSR and WRMSR; on one without, it uses
the MSR. Boots BastiOS with and without the feature (`-cpu qemu64` with
and without +fsgsbase, under TCG or KVM), runs test/fsbase.c, which checks
that FS comes back from system calls, page faults and process switches as the
program left it, and reads CR4 from QEMU's monitor afterwards:

    test/fsbase_test.py [--kernel build/bastios.elf]

With the feature CR4.FSGSBASE (bit 16) must be set on every processor that
has run user code, without it clear.
"""
import argparse, os, re, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "bench"))
import run as R  # noqa: E402

FSGSBASE = 1 << 16


def build():
    out = os.path.join(R.B, "fsbase")
    os.makedirs(R.B, exist_ok=True)
    subprocess.run([os.environ.get("MUSLCC", "musl-gcc"), "-static-pie", "-O1", "-fno-stack-protector", "-o", out,
                    os.path.join(HERE, "fsbase.c")], check=True)
    return out


def boot_and_read(kernel, cpu, smp, prog):
    sock = os.path.join(tempfile.gettempdir(), f"fsbase-mon-{os.getpid()}.sock")
    R.EXTRA_QEMU = ["-monitor", f"unix:{sock},server,nowait"]
    args = argparse.Namespace(accel=R.accel(), cpu=cpu, mem="256M", smp=smp, kernel=kernel, append="")
    g = R.boot("bastios", args, prog, tempfile.mkdtemp())
    try:
        at = len(g.out)
        g.send("/disk/fsbase 2>&1; echo @@d")
        if not g.until(rb"[\r\n]@@d[\r\n]", at, 120):
            raise SystemExit("fsbase_test: the program did not finish")
        text = g.out[at:].decode(errors="replace")
        said = re.findall(r"fsbase: [^\r\n]*", text)
        mon = socket.socket(socket.AF_UNIX)
        mon.connect(sock)
        time.sleep(0.5)
        mon.recv(65536)
        mon.send(b"info registers -a\n")
        time.sleep(1.5)
        regs = mon.recv(1 << 20).decode(errors="replace")
        return [int(x, 16) for x in re.findall(r"CR4=([0-9a-f]+)", regs)], said[-1] if said else text.strip()[-200:]
    finally:
        g.stop()
        try:
            os.unlink(sock)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build", "bastios.elf"))
    ap.add_argument("--smp", type=int, default=2)
    args = ap.parse_args()
    prog = build()
    bad = 0
    for cpu, want in (("qemu64,+pdpe1gb", False), ("qemu64,+pdpe1gb,+fsgsbase", True)):
        try:
            cr4s, said = boot_and_read(args.kernel, cpu, args.smp, prog)
        except BaseException as e:  # a hang or a crash is a failure of this case, not of the script
            print(f"fsbase_test: -cpu {cpu}: FAIL; {e}")
            bad += 1
            continue
        on = [bool(c & FSGSBASE) for c in cr4s]
        ok = bool(cr4s) and said.startswith("fsbase: ok") and (any(on) if want else not any(on))
        print(f"fsbase_test: -cpu {cpu}: CR4.FSGSBASE per processor {on}: {'ok' if ok else 'FAIL'}; {said}")
        bad += not ok
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
