#!/usr/bin/env python3
"""Process-context identifiers (boot/boot.S, boot/platform.c): the kernel
turns them on only when the boot command line says `pcid` and the processor
has them. Boots BastiOS twice, with and without the word, runs a system call
benchmark in each, and reads CR4 from QEMU's monitor afterwards:

    test/pcid_test.py [--kernel build/bastios.elf]

Without the word CR4.PCIDE (bit 17) must be clear, and with it, set on every
processor that has run user code. QEMU's TCG does not offer PCID, so the
check needs KVM: without /dev/kvm the test says so and passes. The two
system call times go to stdout (and to the job summary in CI).
"""
import argparse, os, re, socket, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "bench"))
import run as R  # noqa: E402

PCIDE = 1 << 17


def boot_and_read(kernel, append, smp, cpu="host"):
    sock = os.path.join(tempfile.gettempdir(), f"pcid-mon-{os.getpid()}-{int(bool(append))}.sock")
    R.EXTRA_QEMU = ["-monitor", f"unix:{sock},server,nowait"]
    args = argparse.Namespace(accel="kvm", cpu=cpu, mem="512M", smp=smp, kernel=kernel, append=append)
    tmp = tempfile.mkdtemp()
    g = R.boot("bastios", args, R.build_osbench(), tmp)
    try:
        at = len(g.out)
        # (the quotes keep the shell's echo of this line from ending the wait)
        g.send("/disk/osbench syscall 2>&1; echo @@do''ne")
        m = g.until(rb"@@done", at, 120)
        if not m:
            raise SystemExit("pcid_test: the system call benchmark did not finish")
        text = g.out[at:].decode(errors="replace")
        e = re.search(r"@end syscall (\d+) (\d+)", text)
        t = f"syscall {int(e.group(2)) / int(e.group(1)):.0f} ns" if e and int(e.group(1)) else ""
        mon = socket.socket(socket.AF_UNIX)
        mon.connect(sock)
        time.sleep(0.5)
        mon.recv(65536)
        mon.send(b"info registers -a\n")
        time.sleep(1.5)
        regs = mon.recv(1 << 20).decode(errors="replace")
        cr4s = [int(x, 16) for x in re.findall(r"CR4=([0-9a-f]+)", regs)]
        return cr4s, t or text.strip()[-200:]
    finally:
        g.stop()
        try:
            os.unlink(sock)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(os.path.dirname(HERE), "build", "bastios.elf"))
    ap.add_argument("--smp", type=int, default=2)
    args = ap.parse_args()
    if R.accel() != "kvm":
        print("pcid_test: no /dev/kvm; QEMU's TCG has no PCID, nothing to check")
        return 0
    with open("/proc/cpuinfo") as f:
        if " pcid" not in f.read():
            print("pcid_test: this processor has no PCID; nothing to check")
            return 0
    bad = 0
    qemu64 = "qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+pcid"
    for append, want, cpu in (("", False, "host"), ("pcid", True, "host"), ("pcid", True, qemu64)):
        try:
            cr4s, timing = boot_and_read(args.kernel, append, args.smp, cpu)
        except BaseException as e:  # a hang or a crash is a failure of this case, not of the script
            print(f"pcid_test: command line {append!r}, -cpu {cpu}: FAIL; {e}")
            bad += 1
            continue
        on = [bool(c & PCIDE) for c in cr4s]
        # A processor that never ran user code (an idle application processor)
        # has not turned PCIDE on yet; the one that ran the benchmark has.
        ok = bool(cr4s) and (any(on) if want else not any(on))
        print(f"pcid_test: command line {append!r}, -cpu {cpu}: CR4.PCIDE per processor {on}: {'ok' if ok else 'FAIL'}; {timing}")
        bad += not ok
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
