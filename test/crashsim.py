#!/usr/bin/env python3
"""Power cuts the way a disk with a write cache sees them.

    crashsim.py [--kernel K] [--runs N] [--boots N] [--every E] [--seed S] [--jobs J]

test/shell_test.sh cuts power by killing QEMU. That keeps every sector
write that completed, in order, so it only ever tests the states between
two writes. A real disk with a volatile write cache can lose any of the
writes that came after the last cache flush, in any combination, and can
tear a write of several sectors (NVMe promises one logical block is
atomic, not eight). Where src/diskfs.bastio puts its flushes is what keeps
the file system whole across those, and killing QEMU cannot test that.

So this records instead. It boots the kernel once with the disk on NVMe
(a drive whose FLUSH the kernel uses) behind QEMU's blklogwrites driver,
which logs every write and every flush in the order the drive saw them
(the dm-log-writes format), and runs user/crashme.bastio RUNS times from a
blank disk. Then, without QEMU, it rebuilds the disk as a power cut could
have left it at every point of the log:

  - everything up to some flush, plus every subset of the writes after
    it (applied in log order: those writes were in the cache together);
  - the same with one of those writes torn: only some of its sectors
    reach the disk (the first half, the second half, and a random mix).

test/fsck.py checks every one of those images. A sample of them (BOOTS,
with the reordered and torn ones first) is then booted in QEMU on virtio,
and crashme checks what it finds there: the records, the in-place
rewrites, the renames and links (see the top of user/crashme.bastio).
Images with no BSTDISK magic are not errors: the cut came before the
format finished, and the next mount formats the disk again.

Prints one line per finding and a summary; exits 1 if any image failed.
"""
import argparse, hashlib, multiprocessing, os, random, select, struct, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fsck  # noqa: E402

SECTOR = 512
DISK_BYTES = 33 << 20
LOG_BYTES = 1 << 30                     # sparse: only what is logged takes space
FLUSH = 1
MAGIC = b"rhswfsj\0"                    # dm-log-writes' superblock magic


def qemu(kernel, drive):
    return subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", kernel, "-m", "64M", "-display", "none", "-no-reboot",
         "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-serial", "stdio",
         "-monitor", "none", "-nic", "none"] + drive,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


class Guest:
    def __init__(self, kernel, drive, timeout):
        self.q = qemu(kernel, drive)
        self.out = b""
        self.deadline = time.time() + timeout

    def until(self, *marks, start=0):
        while not any(m in self.out[start:] for m in marks):
            left = self.deadline - time.time()
            if left <= 0:
                return False
            r, _, _ = select.select([self.q.stdout], [], [], left)
            if not r:
                return False
            chunk = self.q.stdout.read1(4096)
            if not chunk:
                return False
            self.out += chunk
        return True

    def send(self, line):
        mark = len(self.out)
        self.q.stdin.write(line.encode() + b"\r")
        self.q.stdin.flush()
        return mark

    def kill(self):
        self.q.kill()
        self.q.wait()


def record(kernel, runs, dirname, timeout):
    """Runs crashme RUNS times on a blank disk; returns (base, log path)."""
    disk = os.path.join(dirname, "rec.img")
    log = os.path.join(dirname, "log.img")
    for path, size in ((disk, DISK_BYTES), (log, LOG_BYTES)):
        with open(path, "wb") as f:
            f.truncate(size)
    g = Guest(kernel, [
        "-machine", "q35", "-device", "pcie-root-port,id=rp1,chassis=1",
        "-blockdev", f"driver=file,filename={disk},node-name=d",
        "-blockdev", f"driver=file,filename={log},node-name=l",
        "-blockdev", "driver=blklogwrites,file=d,log=l,log-sector-size=512,node-name=w",
        "-device", "nvme,serial=bastios,drive=w,bus=rp1"], timeout)
    ok = g.until(b"bastios> ")
    for n in range(runs):
        if not ok:
            break
        mark = g.send("crashme")
        ok = g.until(b"crashme: done", b"crashme: bad", start=mark) and b"crashme: done" in g.out[mark:]
        ok = ok and g.until(b"bastios> ", start=mark)
    g.kill()
    if not ok:
        sys.stdout.write(g.out[-2000:].decode(errors="replace"))
        sys.exit("crashsim: the recording run did not finish")
    return bytes(DISK_BYTES), log


def parse(log):
    """The log as epochs: the writes (sector, data) between two flushes."""
    epochs = [[]]
    with open(log, "rb") as f:
        sb = f.read(SECTOR)
        if sb[:8] != MAGIC:
            sys.exit("crashsim: the write log has no dm-log-writes superblock")
        sectsize = struct.unpack_from("<I", sb, 24)[0]
        assert sectsize == SECTOR, sectsize
        while True:
            head = f.read(SECTOR)
            sector, nr, flags, _ = struct.unpack_from("<QQQQ", head)
            if not (sector or nr or flags):
                break                   # past the last entry (the superblock's count lags)
            if nr:
                epochs[-1].append((sector, f.read(nr * SECTOR)))
            if flags & FLUSH:
                epochs.append([])
    return [e for e in epochs if e]


def cuts(epoch, rng):
    """The states a cut during this epoch can leave: lists of (sector,
    data) to apply on top of the epochs before it. The first is the empty
    one (nothing of this epoch reached the disk)."""
    m = len(epoch)
    subsets = range(1 << m) if m <= 6 else [(1 << k) - 1 for k in range(m + 1)] + [rng.getrandbits(m) for _ in range(64)]
    for bits in subsets:
        kind = "prefix" if bits & (bits + 1) == 0 else "reordered"
        yield kind, [epoch[i] for i in range(m) if bits >> i & 1]
    for i, (sector, data) in enumerate(epoch):
        n = len(data) // SECTOR
        if n < 2:
            continue
        mixed = [s for s in range(n) if rng.random() < 0.5]
        if 0 < len(mixed) < n:
            mixed = [mixed]
        else:
            mixed = []
        for keep in [list(range(n // 2)), list(range(n // 2, n))] + mixed:
            torn = [(sector + s, data[s * SECTOR:(s + 1) * SECTOR]) for s in keep]
            yield "torn", epoch[:i] + torn


def apply(img, writes, undo=None):
    for sector, data in writes:
        at = sector * SECTOR
        if undo is not None:
            undo.append((sector, bytes(img[at:at + len(data)])))
        img[at:at + len(data)] = data


# user/crashme.bastio's checks, on the image itself: what its files would
# read as after a mount. The boots below run the real ones on a sample.
REC, SNAP, MAX_RECS = 16, 2000, 65536
OW, OW_A, OW_AT, OW_N = 6000, 4096, 1000, 4000
TR_SMALL, TR_BIG, PAY, GMAX = 3000, 9000, 100, 600


def files(img):
    """{path: bytes} of every file the mount would show, by name from the
    disk's top ("log", "g/000", ...). A hard link reads as its file; the
    links the mount drops (fsck's two notes) and orphans are not there."""
    _, ents, _, _ = fsck.entries(img)
    B = fsck.BLOCK

    def data(e):
        out = bytearray(e["size"])
        if e["root"]:
            for m, l1 in fsck.used(img, e["root"]):
                for k, b in fsck.used(img, l1):
                    at = (m * fsck.PTRS + k) * B
                    if at < len(out):
                        out[at:at + B] = img[b * B:(b + 1) * B][:len(out) - at]
        return bytes(out)

    def path(i):
        parts, p = [], i
        while p != fsck.DTOP:
            parts.append(ents[p]["name"].decode(errors="replace"))
            p = ents[p]["parent"]
        return "/".join(reversed(parts))

    out = {}
    for i, e in enumerate(ents):
        if e["kind"] == fsck.FILE:
            out[path(i)] = data(e)
        elif e["kind"] == fsck.DIR:
            out.setdefault(path(i) + "/", b"")
    for i, e in enumerate(ents):
        t = e["link"]
        if e["kind"] == fsck.LINK and t < fsck.DSLOTS and ents[t]["kind"] == fsck.FILE \
                and (ents[t]["parent"], ents[t]["name"]) != (e["parent"], e["name"]):
            out[path(i)] = data(ents[t])
    return out


def rec(i):
    return b"r" + b"%014d" % i + b"\n"


def crashme_check(img):
    """The first thing crashme would find wrong ("log", "ra/rb", ...), or None."""
    f = files(img)
    log = f.get("log", b"")
    n = len(log) // REC
    if len(log) % REC or n > MAX_RECS or log != b"".join(rec(i) for i in range(n)):
        return "log"
    snap = f.get("snap", b"")
    if snap and (len(snap) != SNAP or snap[0] not in b"0123456789" or snap != snap[:1] * SNAP):
        return "snap"
    if f.get("t"):
        return "t"
    ow = f.get("ow", b"")
    if ow:
        if len(ow) == OW_A:
            good = ow == b"x" * OW_A
        else:
            mid = ow[OW_AT:OW_AT + OW_N]
            good = len(ow) == OW and ow[:OW_AT] + ow[OW_AT + OW_N:] == b"x" * (OW - OW_N) \
                and mid[0] in b"x0123456789" and mid == mid[:1] * OW_N
        if not good:
            return "ow"
    tr = f.get("tr", b"")
    if tr and (len(tr) not in (TR_SMALL, TR_BIG) or tr[:TR_SMALL] != b"a" * TR_SMALL or any(tr[TR_SMALL:])):
        return "tr"

    def pay(name, c):                   # None absent, 0 empty, PAY whole, -1 wrong
        if name not in f:
            return None
        d = f[name]
        return len(d) if d in (b"", c * PAY) else -1
    a, b = pay("ra", b"R"), pay("rb", b"R")
    if -1 in (a, b) or (a is not None and b is not None) or (n > 6 and a is None and b is None):
        return "ra/rb"
    a, b = pay("la", b"L"), pay("lb", b"L")
    if -1 in (a, b) or (n > 12 and a is None and b is None):
        return "la/lb"
    gap = empty = False
    for k in range(GMAX):
        d = f.get("g/%03d" % k)
        if d is None:
            gap = True
        elif gap or empty:
            return "g"
        elif d == b"":
            empty = True
        elif d != b"%03d" % k:
            return "g"
    return None


def check_range(args):
    """fsck on every cut state of epochs [lo, hi). Returns (states, the
    failures as (epoch, kind, writes, errors), the boot candidates)."""
    base, epochs, lo, hi, seed, every = args
    rng = random.Random(seed * 100003 + lo)
    img = bytearray(base)
    for e in epochs[:lo]:
        apply(img, e)
    states, fails, seen, picks = 0, [], set(), []
    for k in range(lo, hi):
        if (k + seed) % every:
            apply(img, epochs[k])
            continue
        for idx, (kind, writes) in enumerate(cuts(epochs[k], rng)):
            undo = []
            apply(img, writes, undo)
            # The same image can come from two subsets (a sector written
            # twice, or a write that changed nothing): check it once.
            key = hashlib.blake2b(b"".join(struct.pack("<Q", s) + d for s, d in undo) + b"|" +
                                  b"".join(struct.pack("<Q", s) + d for s, d in writes), digest_size=16).digest()
            if key not in seen:
                seen.add(key)
                states += 1
                if bytes(img[0:8]) in (b"BSTDISK2", b"BSTDISK3", b"BSTDISK4"):
                    errors, _, _ = fsck.check(img)
                    if not errors:
                        bad = crashme_check(img)
                        errors = [f"crashme would find {bad} wrong"] if bad else []
                    if errors:
                        fails.append((k, kind, writes, errors))
                    elif kind != "prefix":
                        picks.append((k, idx, kind, len(writes)))
            for sector, data in reversed(undo):
                img[sector * SECTOR:sector * SECTOR + len(data)] = data
        apply(img, epochs[k])
    return states, fails, picks


def rebuild(base, epochs, k, writes):
    img = bytearray(base)
    for e in epochs[:k]:
        apply(img, e)
    apply(img, writes)
    return img


def boot_check(kernel, path, timeout):
    g = Guest(kernel, ["-drive", f"file={path},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd"], timeout)
    ok = g.until(b"bastios> ")
    line = "(no prompt)"
    if ok:
        mark = g.send("crashme")
        ok = g.until(b" records\r\n", b"crashme: bad", start=mark)
        lines = [l for l in g.out[mark:].split(b"\n") if b"crashme:" in l]
        line = lines[0].strip().decode(errors="replace") if lines else "(nothing)"
        ok = ok and b"crashme: clean" in g.out[mark:]
    g.kill()
    return ok, line


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(here, "build", "bastios.elf"))
    ap.add_argument("--runs", type=int, default=2, help="crashme runs to record")
    ap.add_argument("--boots", type=int, default=40, help="states to boot and check with crashme")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--every", type=int, default=1, help="cut only every EVERY-th epoch (make test: a quick pass)")
    ap.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    ap.add_argument("--timeout", type=int, default=300)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        t0 = time.time()
        base, log = record(a.kernel, a.runs, tmp, a.timeout)
        epochs = parse(log)
        writes = sum(len(e) for e in epochs)
        print(f"crashsim: recorded {a.runs} crashme run(s): {writes} writes in {len(epochs)} flush epochs "
              f"(largest {max(len(e) for e in epochs)} writes), {time.time() - t0:.0f} s")
        t0 = time.time()
        step = max(1, (len(epochs) + 4 * a.jobs - 1) // (4 * a.jobs))
        ranges = [(base, epochs, lo, min(lo + step, len(epochs)), a.seed, a.every) for lo in range(0, len(epochs), step)]
        with multiprocessing.Pool(a.jobs) as pool:
            results = pool.map(check_range, ranges)
        states = sum(r[0] for r in results)
        bad = [f for r in results for f in r[1]]
        picks = [p for r in results for p in r[2]]
        for k, kind, w, errors in bad[:20]:
            print(f"FAIL cut in epoch {k}, {kind} ({len(w)} writes: sectors {[s for s, _ in w]}):")
            for e in errors[:5]:
                print("       ", e)
        print(("ok  " if not bad else "FAIL"),
              f"fsck and the crashme checks on {states} cut states: {len(bad)} failed ({time.time() - t0:.0f} s)")
        fails += len(bad)
        # Boot a sample: the reordered and torn states, spread over the log.
        t0 = time.time()
        rng.shuffle(picks)
        picks = sorted(picks[:a.boots])
        booted = 0
        for k, idx, kind, n in picks:
            # The same state again: replay the generator from where its
            # range started, so the random choices come out the same.
            lo = next(r[2] for r in ranges if r[2] <= k < r[3])
            erng = random.Random(a.seed * 100003 + lo)
            for j in range(lo, k):
                if (j + a.seed) % a.every:
                    continue
                for _ in cuts(epochs[j], erng):
                    pass
            want = next(w for i, (_, w) in enumerate(cuts(epochs[k], erng)) if i == idx)
            path = os.path.join(tmp, "cut.img")
            with open(path, "wb") as f:
                f.write(rebuild(base, epochs, k, want))
            ok, line = boot_check(a.kernel, path, a.timeout)
            booted += 1
            print(("ok  " if ok else "FAIL"), f"boot after a {kind} cut in epoch {k} ({n} writes):", line)
            fails += not ok
        # And a few of the failures, to show the kernel sees them too
        # (crashme's own checks; an fsck error may not show in a file).
        for k, kind, w, errors in bad[:3]:
            path = os.path.join(tmp, "cut.img")
            with open(path, "wb") as f:
                f.write(rebuild(base, epochs, k, w))
            _, line = boot_check(a.kernel, path, a.timeout)
            booted += 1
            print(f"     boot after the failing {kind} cut in epoch {k}:", line)
        print(f"crashsim: booted {booted} cut states ({time.time() - t0:.0f} s)")
    print(f"---- {fails} failed ----")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
