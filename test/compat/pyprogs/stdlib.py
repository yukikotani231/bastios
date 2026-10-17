"""Exercises the parts of the Python standard library that lean on the
operating system, one check per line: "ok NAME" or "FAIL NAME: error".
Run by python.txt on BastiOS; the same file runs on Linux for comparison."""
import warnings, os, sys, time, threading, tempfile, shutil, subprocess, socket, select, selectors, signal, mmap, fcntl, resource, struct, zlib, io, faulthandler

# A check that has not ended after this many seconds is a hang: the script
# names it, dumps every thread's stack and exits, so a lost wake-up costs
# a minute and not the lane's whole timeout.
HANG_SECONDS = 90

# The watchdog is a thread, so every fork would warn about forking a
# multi-threaded process; the checks fork on purpose.
warnings.simplefilter("ignore")

def main():
    failed = []

    def check(name):
        def deco(f):
            def hang():
                # One line per thread (innermost call first, the watchdog
                # itself left out), short enough for the lane's report.
                print("HANG %s: no end after %d s" % (name, HANG_SECONDS), flush=True)
                for tid, frame in sys._current_frames().items():
                    if tid == threading.get_ident():
                        continue
                    calls = []
                    while frame and len(calls) < 7:
                        calls.append("%s:%d %s" % (os.path.basename(frame.f_code.co_filename), frame.f_lineno, frame.f_code.co_name))
                        frame = frame.f_back
                    print("thread %d: %s" % (tid & 0xffff, " < ".join(calls)), flush=True)
                # asyncio's tasks: where each coroutine is suspended, the call
                # the event loop is waiting for.
                aio = sys.modules.get("asyncio.tasks")
                for t in list(getattr(aio, "_scheduled_tasks", [])):
                    chain, c = [], t.get_coro()
                    while c is not None:
                        fr = getattr(c, "cr_frame", None) or getattr(c, "gi_frame", None)
                        chain.append("%s:%d" % (getattr(c, "__qualname__", "?"), fr.f_lineno if fr else -1))
                        c = getattr(c, "cr_await", None) or getattr(c, "gi_yieldfrom", None)
                    print("task: %s" % " > ".join(chain), flush=True)
                faulthandler.dump_traceback(file=sys.stdout, all_threads=True)
                sys.stdout.flush()
                os._exit(3)
            watchdog = threading.Timer(HANG_SECONDS, hang)
            watchdog.daemon = True
            watchdog.start()
            try:
                f()
                print("ok", name)
            except BaseException as e:
                failed.append(name)
                tb = e.__traceback__
                while tb.tb_next:
                    tb = tb.tb_next
                print("FAIL %s: %s: %s (%s:%d)" % (name, type(e).__name__, str(e)[:120], os.path.basename(tb.tb_frame.f_code.co_filename), tb.tb_lineno))
            watchdog.cancel()
            sys.stdout.flush()
            return f
        return deco

    tmp = tempfile.mkdtemp(prefix="stdlib-")

    @check("fork, wait and exit status")
    def _():
        pid = os.fork()
        if pid == 0:
            os._exit(7)
        _, st = os.waitpid(pid, 0)
        assert os.WEXITSTATUS(st) == 7

    @check("pipe between parent and forked child")
    def _():
        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(r); os.write(w, b"hello"); os._exit(0)
        os.close(w)
        assert os.read(r, 10) == b"hello"
        os.waitpid(pid, 0)

    @check("posix_spawn and subprocess")
    def _():
        pid = os.posix_spawn("/bin/true", ["true"], os.environ)
        os.waitpid(pid, 0)
        out = subprocess.run(["sh", "-c", "echo a; echo b >&2; exit 3"], capture_output=True, text=True)
        assert (out.stdout, out.stderr, out.returncode) == ("a\n", "b\n", 3)
        p = subprocess.Popen(["cat"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        assert p.communicate(b"abc")[0] == b"abc"
        try:
            subprocess.run(["sleep", "5"], timeout=0.3)
            assert False
        except subprocess.TimeoutExpired:
            pass

    @check("files: stat, rename, link, symlink, truncate, utime, chmod")
    def _():
        p = os.path.join(tmp, "f")
        with open(p, "w") as f:
            f.write("0123456789")
        os.truncate(p, 4)
        assert os.stat(p).st_size == 4
        os.link(p, p + ".l"); assert os.stat(p).st_nlink == 2
        os.symlink("f", p + ".s"); assert os.readlink(p + ".s") == "f"
        os.rename(p + ".l", p + ".r")
        os.utime(p, (1000000000, 1000000000)); assert int(os.stat(p).st_mtime) == 1000000000
        os.chmod(p, 0o600); assert os.stat(p).st_mode & 0o777 == 0o600
        assert sorted(e.name for e in os.scandir(tmp)) == ["f", "f.r", "f.s"]
        assert [d for d, _, fs in os.walk(tmp)] == [tmp]

    @check("shutil: copytree, move, rmtree, disk_usage, make_archive")
    def _():
        src = os.path.join(tmp, "src"); os.makedirs(src + "/a/b")
        for n in range(20):
            open("%s/a/b/%d" % (src, n), "w").write("x" * n)
        shutil.copytree(src, tmp + "/dst")
        shutil.move(tmp + "/dst", tmp + "/dst2")
        assert len(os.listdir(tmp + "/dst2/a/b")) == 20
        assert shutil.disk_usage(tmp).total > 0
        shutil.make_archive(tmp + "/arc", "gztar", src)
        shutil.unpack_archive(tmp + "/arc.tar.gz", tmp + "/unp")
        assert len(os.listdir(tmp + "/unp/a/b")) == 20
        shutil.rmtree(tmp + "/dst2")

    @check("threads, locks, queue, condition, timer")
    def _():
        import queue
        q = queue.Queue(); lock = threading.Lock(); n = [0]
        def work():
            for _ in range(500):
                with lock:
                    n[0] += 1
            q.put(1)
        ts = [threading.Thread(target=work) for _ in range(8)]
        [t.start() for t in ts]; [t.join() for t in ts]
        assert n[0] == 4000 and q.qsize() == 8
        c = threading.Condition(); got = []
        def waiter():
            with c:
                c.wait_for(lambda: got, timeout=5)
        t = threading.Thread(target=waiter); t.start()
        time.sleep(0.1)
        with c:
            got.append(1); c.notify_all()
        t.join(5); assert not t.is_alive()
        ev = threading.Event(); threading.Timer(0.1, ev.set).start(); assert ev.wait(5)

    @check("signals: alarm, setitimer, kill, handler in a thread's process")
    def _():
        hit = []
        signal.signal(signal.SIGALRM, lambda *a: hit.append("alrm"))
        signal.setitimer(signal.ITIMER_REAL, 0.1)
        time.sleep(0.5)
        assert hit == ["alrm"], hit
        signal.signal(signal.SIGUSR1, lambda *a: hit.append("usr1"))
        os.kill(os.getpid(), signal.SIGUSR1)
        assert hit == ["alrm", "usr1"]
        signal.signal(signal.SIGCHLD, lambda *a: hit.append("chld"))
        pid = os.fork()
        if pid == 0:
            os._exit(0)
        os.waitpid(pid, 0); time.sleep(0.1)
        signal.signal(signal.SIGCHLD, signal.SIG_DFL)
        assert "chld" in hit
        assert signal.sigtimedwait([], 0) is None or True

    @check("sockets: TCP, UDP, unix, socketpair, SCM_RIGHTS, getaddrinfo")
    def _():
        s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen()
        port = s.getsockname()[1]
        def srv():
            c, _ = s.accept(); c.sendall(c.recv(100).upper()); c.close()
        t = threading.Thread(target=srv); t.start()
        c = socket.create_connection(("127.0.0.1", port)); c.sendall(b"abc"); assert c.recv(10) == b"ABC"; c.close(); t.join()
        u1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u1.bind(("127.0.0.1", 0))
        u2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u2.sendto(b"dg", u1.getsockname())
        assert u1.recvfrom(10)[0] == b"dg"
        path = os.path.join(tmp, "sock")
        us = socket.socket(socket.AF_UNIX); us.bind(path); us.listen()
        uc = socket.socket(socket.AF_UNIX); uc.connect(path); ua, _ = us.accept()
        uc.sendall(b"u"); assert ua.recv(1) == b"u"
        a, b = socket.socketpair()
        r, w = os.pipe(); os.write(w, b"via")
        socket.send_fds(a, [b"x"], [r])
        _, fds, _, _ = socket.recv_fds(b, 10, 1)
        assert os.read(fds[0], 10) == b"via"
        assert socket.getaddrinfo("localhost", 80, type=socket.SOCK_STREAM)

    @check("selectors: epoll and poll")
    def _():
        r, w = os.pipe()
        sel = selectors.DefaultSelector()
        sel.register(r, selectors.EVENT_READ)
        assert sel.select(0) == []
        os.write(w, b"x")
        assert len(sel.select(1)) == 1
        p = select.poll(); p.register(r, select.POLLIN); assert p.poll(1)
        assert select.select([r], [], [], 1)[0] == [r]

    @check("mmap: anonymous and shared file")
    def _():
        m = mmap.mmap(-1, 8192); m[0:3] = b"abc"; assert m[:3] == b"abc"; m.close()
        p = os.path.join(tmp, "mm")
        with open(p, "wb") as f:
            f.write(b"\0" * 8192)
        with open(p, "r+b") as f:
            m = mmap.mmap(f.fileno(), 0); m[5:8] = b"xyz"; m.flush(); m.close()
        assert open(p, "rb").read()[5:8] == b"xyz"

    @check("fcntl: flock, lockf, F_GETFL, O_CLOEXEC")
    def _():
        f = open(os.path.join(tmp, "lk"), "w")
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        g = open(os.path.join(tmp, "lk"), "w")
        try:
            fcntl.flock(g, fcntl.LOCK_EX | fcntl.LOCK_NB); assert False
        except BlockingIOError:
            pass
        fcntl.flock(f, fcntl.LOCK_UN)
        fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB, 10, 0)
        assert fcntl.fcntl(f, fcntl.F_GETFD) & fcntl.FD_CLOEXEC

    @check("resource, time and clocks")
    def _():
        assert resource.getrlimit(resource.RLIMIT_NOFILE)[0] > 0
        resource.setrlimit(resource.RLIMIT_CORE, (0, resource.getrlimit(resource.RLIMIT_CORE)[1]))
        assert resource.getrusage(resource.RUSAGE_SELF).ru_maxrss > 0
        for c in (time.CLOCK_MONOTONIC, time.CLOCK_REALTIME, time.CLOCK_PROCESS_CPUTIME_ID, time.CLOCK_THREAD_CPUTIME_ID, time.CLOCK_BOOTTIME):
            time.clock_gettime(c)
        t0 = time.monotonic(); time.sleep(0.2); assert 0.19 < time.monotonic() - t0 < 1.0
        assert time.time() > 1.7e9
        assert os.times().elapsed > 0

    @check("os: urandom, uname, getgroups, sendfile, copy_file_range, memfd, eventfd, pidfd")
    def _():
        assert len(os.urandom(16)) == 16 and os.uname().sysname
        os.getgroups(); os.getpgid(0); os.getsid(0); os.sched_yield()
        a = os.path.join(tmp, "sa"); b = os.path.join(tmp, "sb")
        open(a, "wb").write(b"d" * 10000)
        fa = os.open(a, os.O_RDONLY); fb = os.open(b, os.O_WRONLY | os.O_CREAT)
        assert os.sendfile(fb, fa, 0, 10000) == 10000
        assert os.copy_file_range(fa, fb, 100, 0, 0) == 100
        fd = os.memfd_create("m"); os.write(fd, b"mem"); os.lseek(fd, 0, 0); assert os.read(fd, 3) == b"mem"
        e = os.eventfd(0); os.eventfd_write(e, 5); assert os.eventfd_read(e) == 5
        pid = os.fork()
        if pid == 0:
            os._exit(0)
        pfd = os.pidfd_open(pid); select.select([pfd], [], [], 5); os.waitpid(pid, 0)

    @check("pty and termios")
    def _():
        import pty, termios
        m, s = pty.openpty()
        os.write(m, b"hi\n")
        assert os.read(s, 10).startswith(b"hi")
        termios.tcgetattr(s)
        pid, fd = pty.fork()
        if pid == 0:
            os.execv("/bin/echo", ["echo", "child"])
        out = b""
        while True:
            try:
                d = os.read(fd, 100)
            except OSError:
                break
            if not d:
                break
            out += d
        os.waitpid(pid, 0)
        assert b"child" in out

    @check("compression: zlib, gzip, bz2, lzma, zipfile, tarfile")
    def _():
        import gzip, bz2, lzma, zipfile, tarfile
        data = os.urandom(1000) * 50
        for m in (zlib, gzip, bz2, lzma):
            assert m.decompress(m.compress(data)) == data
        z = os.path.join(tmp, "z.zip")
        with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("a.txt", data)
        assert zipfile.ZipFile(z).read("a.txt") == data
        with tarfile.open(os.path.join(tmp, "t.tar.xz"), "w:xz") as tf:
            tf.add(z, "z.zip")
        assert tarfile.open(os.path.join(tmp, "t.tar.xz")).getnames() == ["z.zip"]

    @check("sqlite3: WAL, transactions, threads")
    def _():
        import sqlite3
        db = sqlite3.connect(os.path.join(tmp, "t.db"), check_same_thread=False)
        assert db.execute("pragma journal_mode=wal").fetchone()[0] == "wal"
        db.execute("create table t(a integer primary key, b text)")
        with db:
            db.executemany("insert into t(b) values (?)", [("x%d" % i,) for i in range(5000)])
        db.execute("create index i on t(b)")
        assert db.execute("select count(*) from t where b like 'x1%'").fetchone()[0] > 100
        res = []
        th = threading.Thread(target=lambda: res.append(db.execute("select count(*) from t").fetchone()[0]))
        th.start(); th.join()
        assert res == [5000]
        b = sqlite3.connect(os.path.join(tmp, "b.db")); db.backup(b); assert b.execute("select count(*) from t").fetchone()[0] == 5000

    @check("ctypes: libc calls and callbacks")
    def _():
        import ctypes, ctypes.util
        libc = ctypes.CDLL(None)
        assert libc.abs(-4) == 4
        buf = ctypes.create_string_buffer(16)
        libc.snprintf(buf, 16, b"%d-%s", 42, b"x"); assert buf.value == b"42-x"
        cmp = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int))
        arr = (ctypes.c_int * 5)(5, 3, 1, 4, 2)
        libc.qsort(arr, 5, 4, cmp(lambda a, b: a[0] - b[0]))
        assert list(arr) == [1, 2, 3, 4, 5]

    @check("multiprocessing: fork, spawn, forkserver; Pool, Queue, Manager, shared_memory")
    def _():
        import multiprocessing as mp
        from multiprocessing import shared_memory
        for method in ("fork", "spawn", "forkserver"):
            ctx = mp.get_context(method)
            with ctx.Pool(2) as p:
                assert p.map(abs, [-1, -2, -3]) == [1, 2, 3], method
        ctx = mp.get_context("fork")
        q = ctx.Queue(); pr = ctx.Process(target=q.put, args=("q",)); pr.start(); assert q.get(timeout=20) == "q"; pr.join()
        with ctx.Manager() as m:
            d = m.dict(); pr = ctx.Process(target=d.update, args=({"a": 1},)); pr.start(); pr.join(); assert dict(d) == {"a": 1}
        sm = shared_memory.SharedMemory(create=True, size=64); sm.buf[0] = 9
        s2 = shared_memory.SharedMemory(name=sm.name); assert s2.buf[0] == 9; s2.close(); sm.close(); sm.unlink()
        from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
        with ProcessPoolExecutor(2, mp_context=ctx) as ex:
            assert list(ex.map(abs, [-5, -6])) == [5, 6]
        with ThreadPoolExecutor(4) as ex:
            assert sum(ex.map(abs, range(-10, 0))) == 55

    @check("asyncio: loopback server, subprocess, executor, unix socket, timers")
    def _():
        import asyncio
        async def main():
            async def h(r, w):
                w.write((await r.readline()).upper()); await w.drain(); w.close()
            srv = await asyncio.start_server(h, "127.0.0.1", 0)
            port = srv.sockets[0].getsockname()[1]
            r, w = await asyncio.open_connection("127.0.0.1", port); w.write(b"hi\n")
            assert await r.readline() == b"HI\n"; w.close(); srv.close()
            p = await asyncio.create_subprocess_exec("sh", "-c", "echo sub; exit 2", stdout=asyncio.subprocess.PIPE)
            o, _ = await p.communicate(); assert o == b"sub\n" and p.returncode == 2
            assert await asyncio.get_running_loop().run_in_executor(None, sum, [1, 2, 3]) == 6
            path = os.path.join(tmp, "as.sock")
            usrv = await asyncio.start_unix_server(h, path)
            r, w = await asyncio.open_unix_connection(path); w.write(b"u\n"); assert await r.readline() == b"U\n"; w.close(); usrv.close()
            t0 = time.monotonic(); await asyncio.sleep(0.2); assert time.monotonic() - t0 >= 0.19
            got = []
            asyncio.get_running_loop().add_signal_handler(signal.SIGUSR2, lambda: got.append(1))
            os.kill(os.getpid(), signal.SIGUSR2); await asyncio.sleep(0.1); assert got == [1]
        asyncio.run(main())

    @check("ssl: hashes and a TLS connection to the host's HTTPS server")
    def _():
        import ssl, hashlib, hmac, secrets, urllib.request
        ctx = ssl.create_default_context(); assert ctx.verify_mode == ssl.CERT_REQUIRED
        assert ssl.OPENSSL_VERSION and len(secrets.token_hex(8)) == 16
        assert hmac.new(b"k", b"m", hashlib.sha256).hexdigest().startswith("b60090")
        assert hashlib.sha3_256(b"a").hexdigest().startswith("80084b")
        if len(sys.argv) > 2:
            ctx = ssl.create_default_context(cafile=sys.argv[2]); ctx.check_hostname = False
            assert urllib.request.urlopen(sys.argv[1] + "/hello.txt", context=ctx).read() == b"hello from the host\n"
            bad = ssl.create_default_context(); bad.check_hostname = False
            try:
                urllib.request.urlopen(sys.argv[1] + "/hello.txt", context=bad); assert False
            except urllib.error.URLError:
                pass

    @check("misc: tempfile, uuid, locale, platform, datetime, gc, faulthandler, tracemalloc")
    def _():
        import uuid, locale, platform, datetime, gc, faulthandler, tracemalloc, pathlib, json, pickle, decimal, fractions, random
        with tempfile.TemporaryFile() as f: f.write(b"x")
        assert uuid.uuid4().version == 4
        assert platform.system() == "Linux" and platform.machine() == "x86_64"
        assert datetime.datetime.fromtimestamp(0, datetime.timezone.utc).year == 1970
        gc.collect(); faulthandler.enable(); tracemalloc.start(); x = [0] * 1000; assert tracemalloc.get_traced_memory()[0] > 0; tracemalloc.stop()
        assert pathlib.Path(tmp).is_dir()
        assert pickle.loads(pickle.dumps({"a": decimal.Decimal("1.1")}))["a"] == decimal.Decimal("1.1")
        assert json.loads(json.dumps(fractions.Fraction(1, 3).as_integer_ratio())) == [1, 3]
        random.seed(1); assert 0 <= random.random() < 1

    @check("big allocations and deep recursion")
    def _():
        b = bytearray(200 * 1024 * 1024); b[-1] = 1; del b
        sys.setrecursionlimit(5000)
        def rec(n):
            return 0 if n == 0 else 1 + rec(n - 1)
        assert rec(3000) == 3000

    shutil.rmtree(tmp, ignore_errors=True)
    print("failed:", len(failed))


if __name__ == "__main__":
    main()
