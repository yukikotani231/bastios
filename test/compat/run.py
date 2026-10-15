#!/usr/bin/env python3
"""The Linux compatibility suite: how much ordinary Linux userland BastiOS runs.

    run.py --kernel build/bastios.elf --disk build/compat/disk.img \\
           --out build/compat PROGRAM...

Boots the kernel in QEMU with a disk holding the suite's programs (static
musl C programs from test/compat/progs, and a BusyBox with a broad set of
applets), starts the kernel's built-in ash, and runs:

  * each PROGRAM as /disk/PROGRAM. A program runs a list of checks and
    prints "@@ NAME", then "ok NAME" or "FAIL NAME: why" for each
    (progs/compat.h);
  * each case in applets.txt, a one-line shell command whose output must
    match exactly;
  * each case in tty.txt: a program driven by keystrokes on the console
    (an editor, a REPL), then a shell command whose output must match.

Commands may name HOSTURL (or HOSTADDR, its address and port, or HOSTPORT,
the port alone): a web server this script runs on the host machine
(WebServer), which the guest reaches through QEMU's user network
(http://10.0.2.2:PORT). HOSTSURL is the same files over HTTPS, with a
certificate made for the run by `openssl` (empty without it).

ssh.txt runs only in the guest. QEMU forwards a port on the host to the
guest's port 22 (SSHPORT); a command starting with "@host " runs on this
machine instead, from the repository, so a client here can log in to the
server the guest started.

The kernel prints "[pid N: Linux system call X not implemented]" for each
call it does not know. Those notes are charged to the check or case that
was running, which gives the list of missing system calls ranked by how
many checks each one shows up in.

A program that hangs is interrupted with Ctrl-C (or the machine rebooted)
and run again with COMPAT_SKIP set past the check that hung, so one hang
costs one check, not the rest of the program.

Writes OUT/report.md (tables), OUT/results.json, and prints the summary.
Exits 1 only if a check listed in test/compat/baseline.txt (checks that
passed when the list was last updated) no longer passes: the suite is a
measurement, so failing checks are expected, but it must not go backwards.
--update-baseline rewrites that list from this run.

--host runs the same programs and cases on this machine instead (from a
scratch directory), to check that the expectations are right for Linux.
"""
import argparse, codecs, concurrent.futures, fcntl, functools, hashlib, http.server, io, json, os, pty, re, select, shutil, signal, socket, ssl, struct, subprocess, sys, tarfile, tempfile, termios, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
PROMPT = b"\n# "
BB = "/disk/busybox"

NOTE = re.compile(r"\[pid \d+: Linux system call (\d+) not implemented\]")
# What the kernel says on the console when a program faults and is killed: it
# is not the output of the command a case runs.
KILLNOTE = re.compile(r"\[pid \d+ \(.*\): .*killed\]$|\[fault diag: ")


def syscall_names():
    names = {}
    for line in open(os.path.join(HERE, "syscalls.txt")):
        if line.startswith("#"):
            continue
        toks = line.split()
        for i in range(0, len(toks), 2):
            names[int(toks[i])] = toks[i + 1]
    return names


def load_applets(path):
    """The cases in applets.txt: (name, command, expected lines)."""
    cases, cur = [], None
    for raw in open(path):
        line = raw.rstrip("\n")
        if line.startswith("== "):
            cur = [line[3:].strip(), None, []]
            cases.append(cur)
        elif cur is None or (cur[1] is None and (line.startswith("#") or not line.strip())):
            continue
        elif cur[1] is None and line.startswith("$ "):
            cur[1] = line[2:]
        elif cur[1] is not None:
            cur[2].append(line)
    out = []
    for name, cmd, exp in cases:
        while exp and not exp[-1].strip():
            exp.pop()
        out.append((name, cmd, exp))
    return out


def load_tty(path):
    """The cases in tty.txt: (name, program, keystrokes, check, expected lines).

        == NAME
        $ PROGRAM        (one line, run by ash in /, in the foreground)
        > KEYS           (typed once the program has drawn something; one
                          line each, with Python's escapes: \\r, \\x1b)
        $ CHECK          (one line, run once the program is back at the shell)
        expected output of CHECK, line by line
    """
    cases, cur = [], None
    for raw in open(path):
        line = raw.rstrip("\n")
        if line.startswith("== "):
            cur = {"name": line[3:].strip(), "prog": None, "keys": [], "check": None, "exp": []}
            cases.append(cur)
        elif cur is None or (cur["check"] is None and (line.startswith("#") or not line.strip())):
            continue
        elif cur["prog"] is None and line.startswith("$ "):
            cur["prog"] = line[2:]
        elif cur["check"] is None and line.startswith("> "):
            cur["keys"].append(codecs.decode(line[2:], "unicode_escape").encode("latin-1"))
        elif cur["check"] is None and line.startswith("$ "):
            cur["check"] = line[2:]
        elif cur["check"] is not None:
            cur["exp"].append(line)
    for c in cases:
        while c["exp"] and not c["exp"][-1].strip():
            c["exp"].pop()
    return [(c["name"], c["prog"], c["keys"], c["check"], c["exp"]) for c in cases]


class WebServer:
    """A web server on the host, for HOSTURL: /hello.txt, a line,
    /mib.bin, 1 MiB whose md5 is 85bfb9943e6f0cb09f0f52679e9eac2b, and
    /repo.git, a git repository for the dumb HTTP protocol, and
    /lsp-server.lua, /lsp-client.lua and /lsp-real-client.lua, the files of the Neovim LSP cases."""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="compat-www-")
        with open(os.path.join(self.dir, "hello.txt"), "w") as f:
            f.write("hello from the host\n")
        with open(os.path.join(self.dir, "mib.bin"), "wb") as f:
            f.write(bytes((i * 7 + i // 256) % 256 for i in range(1 << 20)))
        # A repository for git's dumb HTTP protocol: /repo.git, two commits
        # on main, "two" and "one" in `git log`; it is also a Neovim plugin
        # (plugin/hello.lua sets vim.g.hello_plugin to the file's content).
        if shutil.which("git"):
            work = os.path.join(self.dir, "work")
            git = ["git", "-c", "user.name=t", "-c", "user.email=t@t", "-c", "init.defaultBranch=main"]
            os.makedirs(work)
            for n, msg in (("one", "one"), ("two", "two")):
                with open(os.path.join(work, "f"), "w") as f:
                    f.write(n + "\n")
                if n == "one":
                    subprocess.run(git + ["init", "-q"], cwd=work, capture_output=True)
                os.makedirs(os.path.join(work, "plugin"), exist_ok=True)
                with open(os.path.join(work, "plugin", "hello.lua"), "w") as f:
                    f.write('vim.g.hello_plugin = "%s"\n' % n)
                subprocess.run(git + ["add", "f", "plugin"], cwd=work, capture_output=True)
                subprocess.run(git + ["commit", "-q", "-m", msg], cwd=work, capture_output=True)
            bare = os.path.join(self.dir, "repo.git")
            subprocess.run(git + ["clone", "-q", "--bare", work, bare], capture_output=True)
            subprocess.run(git + ["update-server-info"], cwd=bare, capture_output=True)
            shutil.rmtree(work)
        for name in ("server.lua", "client.lua", "real-client.lua"):
            shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), "lsp", name),
                        os.path.join(self.dir, "lsp-" + name))
        handler = functools.partial(Quiet, directory=self.dir)
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        # The same files over TLS, with a throwaway self-signed certificate.
        self.https, self.sport = None, 0
        cert, key = os.path.join(self.dir, "cert.pem"), os.path.join(self.dir, "key.pem")
        if shutil.which("openssl") and subprocess.run(
                ["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                 "-nodes", "-days", "1", "-subj", "/CN=compat.test", "-keyout", key, "-out", cert],
                capture_output=True).returncode == 0:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(cert, key)
            os.unlink(key)
            self.https = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
            self.https.socket = ctx.wrap_socket(self.https.socket, server_side=True)
            self.sport = self.https.server_address[1]
            threading.Thread(target=self.https.serve_forever, daemon=True).start()

    def add_registry(self, layer_list):
        """A registry for tools/ocipull.sh: /v2/library/alpine, an image index
        whose image for the guest's processor (linux/amd64, or linux/arm64 on AArch64) is the layers named by the JSON file layer_list
        (files sha256-HEX.tar.gz beside it, the cache alpinedisk.py keeps) and a
        layer of ours on top that hides /etc/motd and adds /etc/ocilayer; its
        config gives the process and the environment. /v2/secure/alpine is the
        same behind a token (/token)."""
        cache = os.path.dirname(os.path.abspath(layer_list))
        base = os.path.join(self.dir, "v2", "library", "alpine")
        os.makedirs(os.path.join(base, "blobs"))
        os.makedirs(os.path.join(base, "manifests"))

        def blob(data=None, link=None):
            if link:
                d = link.split("sha256-")[1].split(".")[0]
                os.symlink(os.path.join(cache, link), os.path.join(base, "blobs", "sha256:" + d))
                return "sha256:" + d, os.path.getsize(os.path.join(cache, link))
            d = hashlib.sha256(data).hexdigest()
            with open(os.path.join(base, "blobs", "sha256:" + d), "wb") as f:
                f.write(data)
            return "sha256:" + d, len(data)

        layers = []
        for name in json.load(open(layer_list)):
            d, n = blob(link=name)
            layers.append({"mediaType": "application/vnd.oci.image.layer.v1.tar+gzip", "digest": d, "size": n})
        raw = io.BytesIO()
        with tarfile.open(fileobj=raw, mode="w:gz") as t:
            for name, body in (("etc/.wh.motd", b""), ("etc/ocilayer", b"two\n")):
                ti = tarfile.TarInfo(name)
                ti.size = len(body)
                ti.mode = 0o644
                t.addfile(ti, io.BytesIO(body))
        d, n = blob(raw.getvalue())
        layers.append({"mediaType": "application/vnd.oci.image.layer.v1.tar+gzip", "digest": d, "size": n})
        goarch, other = ("arm64", "amd64") if ARCH == "aarch64" else ("amd64", "arm64")
        config = {"architecture": goarch, "os": "linux",
                  "config": {"Env": ["FOO=bar", "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"],
                             "Entrypoint": ["/bin/sh", "-c"],
                             "Cmd": ["echo from-image $FOO; id -u; hostname; test -e /etc/motd || cat /etc/ocilayer"],
                             "WorkingDir": "/etc"},
                  "rootfs": {"type": "layers", "diff_ids": []}}
        cd, cn = blob(json.dumps(config).encode())
        manifest = json.dumps({"schemaVersion": 2, "mediaType": "application/vnd.oci.image.manifest.v1+json",
                               "config": {"mediaType": "application/vnd.oci.image.config.v1+json", "digest": cd, "size": cn},
                               "layers": layers}).encode()
        md = "sha256:" + hashlib.sha256(manifest).hexdigest()
        with open(os.path.join(base, "manifests", md), "wb") as f:
            f.write(manifest)
        index = {"schemaVersion": 2, "mediaType": "application/vnd.oci.image.index.v1+json", "manifests": [
            {"mediaType": "application/vnd.oci.image.manifest.v1+json", "digest": "sha256:" + "0" * 64, "size": 1,
             "platform": {"architecture": other, "os": "linux"}},
            {"mediaType": "application/vnd.oci.image.manifest.v1+json", "digest": md, "size": len(manifest),
             "platform": {"architecture": goarch, "os": "linux"}}]}
        for ref in ("latest", "3.24"):
            with open(os.path.join(base, "manifests", ref), "w") as f:
                json.dump(index, f)

    def shutdown(self):
        self.httpd.shutdown()
        if self.https:
            self.https.shutdown()
        shutil.rmtree(self.dir, ignore_errors=True)


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        # The registry's token (WebServer.add_registry): /v2/secure/ wants one.
        if self.path.startswith("/token"):
            body = b'{"token":"compat-token"}'
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith("/v2/secure/"):
            if self.headers.get("Authorization") != "Bearer compat-token":
                self.send_response(401)
                self.send_header("WWW-Authenticate", f'Bearer realm="http://{self.headers["Host"]}/token",service="compat"')
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.path = "/v2/library/" + self.path[len("/v2/secure/"):]
        super().do_GET()


def with_bb(cmd, bb):
    return re.sub(r"\bbb\b", bb, cmd)


# ---------------------------------------------------------------- the guest

# The programs of apps.txt the AArch64 suite builds (arch/aarch64/Makefile).
AARCH64_APPS = ("lua", "qjs", "sqlite3")


class Guest:
    def __init__(self, kernel, disk, log, web=None, idle=False, arch="x86_64", mem="256M"):
        # idle: QEMU runs under SCHED_IDLE, on CPU time no other process wants.
        self.kernel, self.disk, self.log, self.idle, self.arch, self.mem = kernel, disk, log, idle, arch, mem
        self.q = None
        self.hosturl = f"http://10.0.2.2:{web.port}" if web else ""
        self.hostaddr = f"10.0.2.2 {web.port}" if web else ""
        self.hostport = f"{web.port}" if web else ""
        self.hostsurl = f"https://10.0.2.2:{web.sport}" if web and web.sport else ""
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        self.sshport = str(s.getsockname()[1])
        s.close()

    def boot(self):
        self.stop()
        self.out = b""
        if self.arch == "aarch64":
            # The kernel powers off through PSCI (no debug-exit device).
            cmd = ["qemu-system-aarch64", "-M", "virt,gic-version=3", "-cpu", "cortex-a72", "-kernel", self.kernel,
                   "-m", self.mem, "-smp", "2", "-nographic", "-no-reboot", "-serial", "stdio", "-monitor", "none",
                   "-drive", f"file={self.disk},format=raw,if=none,id=vd", "-device", "virtio-blk-device,drive=vd",
                   "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{self.sshport}-:22", "-device", "virtio-net-device,netdev=n0"]
        else:
            # qemu64 plus the x86-64-v2 instruction set (SSSE3, SSE4, POPCNT): what
            # most distributions build their programs for (the Alpaquita gdb, say).
            # Nothing past SSE: the kernel keeps a process's FXSAVE state only.
            cmd = ["qemu-system-x86_64", "-cpu", "qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt",
                   "-kernel", self.kernel, "-m", self.mem, "-smp", "2",
                   "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
                   "-serial", "stdio", "-monitor", "none",
                   "-drive", f"file={self.disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd",
                   "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{self.sshport}-:22", "-device", "virtio-net-pci,netdev=n0"]
        self.q = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            preexec_fn=(lambda: os.sched_setscheduler(0, os.SCHED_IDLE, os.sched_param(0))) if self.idle else None)
        if not self.read_until(b"bastios> ", 0, 60):
            raise SystemExit("compat: the kernel did not reach its shell prompt")
        mark = len(self.out)
        self.send(b"ash\r")
        if not self.read_until(PROMPT, mark, 30):
            raise SystemExit("compat: ash did not start")

    def stop(self):
        if self.q:
            self.q.kill()
            self.q.wait()
            self.log.write(self.out)
            self.q = None

    def send(self, data):
        self.q.stdin.write(data)
        self.q.stdin.flush()

    def read_until(self, marker, start, timeout):
        deadline = time.time() + timeout
        while marker not in self.out[start:]:
            left = deadline - time.time()
            if left <= 0:
                return False
            r, _, _ = select.select([self.q.stdout], [], [], left)
            if not r:
                return False
            chunk = self.q.stdout.read1(65536)
            if not chunk:
                return False
            self.out += chunk
        return True

    def settle(self, quiet, limit):
        """Waits until the console has been silent for `quiet` seconds (at most
        `limit`), so that a program busy with one line of keys (a :make, a
        slow start on an emulated processor) has finished before the next."""
        end = time.time() + limit
        seen, since = len(self.out), time.time()
        while time.time() < end and time.time() - since < quiet:
            self.read_until(b"\x00never", len(self.out), 0.1)
            if len(self.out) != seen:
                seen, since = len(self.out), time.time()

    def keys(self, prog, keys, timeout):
        """Starts prog and types keys at it once it has drawn something.
        Returns whether it got back to the shell prompt."""
        self.hang_tail = b""
        self.hang_disk = 0
        mark = len(self.out)
        # The marker shows the shell is back: a full-screen program's last
        # output can run into the prompt.
        cmd = prog.encode() + b"; echo @@back\r"
        self.send(cmd)
        # Its command line echoes; then wait for the program's own output, so
        # the keys reach it after it has set up the terminal.
        if not self.read_until(b"\n", mark, 10):
            return False
        echoed = len(self.out)
        deadline = time.time() + 30
        while len(self.out) == echoed and time.time() < deadline:
            self.read_until(b"\x00never", echoed, 0.2)
        time.sleep(0.5)
        typed = len(self.out)
        for k in keys:
            self.send(k)
            typed = len(self.out)
            time.sleep(0.4)
            self.settle(1.0, 15)
        # Back at the shell: the marker and a prompt, or a prompt on a line
        # of its own if a Ctrl-C the program took also cut the shell's list
        # short (as ash does on Linux).
        end = time.time() + timeout
        done = False
        while not done and time.time() < end:
            at = self.out.find(b"@@back\r\n", echoed)
            done = (at >= 0 and PROMPT in self.out[at:]) or self.out[typed:].endswith(b"\r\n# ")
            if not done:
                self.read_until(b"\x00never", len(self.out), 0.2)
        if not done:
            # What the screen showed last, for the report of a hang.
            self.hang_tail = bytes(self.out[-600:])
            self.hang_disk = self.out.count(b"[disk:")
            self.send(b"\x03")
            if not self.read_until(PROMPT, len(self.out), 10):
                self.boot()
        return done

    def run(self, line, timeout):
        """Runs a shell line ending in `echo @@end $?`. Returns (text, status),
        status None if it never finished (the guest is then made usable
        again, by Ctrl-C or a reboot)."""
        mark = len(self.out)
        self.send(line.encode() + b"\r")
        done = self.read_until(b"\n@@end ", mark, timeout) and self.read_until(PROMPT, mark, 10)
        text = self.out[mark:].decode(errors="replace").replace("\r", "")
        if done:
            m = re.search(r"\n@@end (\d+)", text)
            return text, int(m.group(1)) if m else None
        mark2 = len(self.out)
        self.send(b"\x03")
        if not self.read_until(PROMPT, mark2, 10):
            self.boot()
        else:
            # The Ctrl-C ended the program; let the trailing echo run, so the
            # next line starts on a clean prompt.
            self.read_until(PROMPT, len(self.out), 2)
        return text, None


class Host:
    """The same interface, on this machine, for checking expectations."""

    def __init__(self, bindir, web=None):
        self.bindir = bindir
        self.cwd = tempfile.mkdtemp(prefix="compat-host-")
        self.hosturl = f"http://127.0.0.1:{web.port}" if web else ""
        self.hostaddr = f"127.0.0.1 {web.port}" if web else ""
        self.hostport = f"{web.port}" if web else ""
        self.hostsurl = f"https://127.0.0.1:{web.sport}" if web and web.sport else ""

    def boot(self):
        pass

    def stop(self):
        shutil.rmtree(self.cwd, ignore_errors=True)

    def keys(self, prog, keys, timeout):
        """As Guest.keys, with the program on a pseudo-terminal here."""
        prog = prog.replace("/disk/", self.bindir + "/")
        pid, fd = pty.fork()
        if pid == 0:
            # 24 rows of 80 columns, as the guest's console reports.
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
            os.chdir(self.cwd)
            os.execv("/bin/sh", ["sh", "-c", prog])
        def drain(t):
            got, end = b"", time.time() + t
            while time.time() < end:
                r, _, _ = select.select([fd], [], [], 0.05)
                if r:
                    try:
                        got += os.read(fd, 65536)
                    except OSError:
                        break
            return got
        drain(1.0)
        for k in keys:
            os.write(fd, k)
            drain(0.4)
        end = time.time() + timeout
        while time.time() < end:
            drain(0.2)
            done, _ = os.waitpid(pid, os.WNOHANG)
            if done:
                os.close(fd)
                return True
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        os.close(fd)
        return False

    def run(self, line, timeout):
        line = line.replace("/disk/", self.bindir + "/")
        try:
            # Under `script`, so the programs have a terminal as in the guest.
            p = subprocess.run(["script", "-qec", line, "/dev/null"], cwd=self.cwd, capture_output=True,
                               timeout=timeout, stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired as e:
            return (e.stdout or b"").decode(errors="replace").replace("\r", ""), None
        text = p.stdout.decode(errors="replace").replace("\r", "")
        m = re.search(r"(?:^|\n)@@end (\d+)", text)
        return text, int(m.group(1)) if m else None


# ---------------------------------------------------------------- running

def run_program(env, prog, timeout):
    """Runs /disk/PROG, rerunning past hangs. Returns its checks, in order:
    dicts with name, result (pass / fail / hang / crash), detail, syscalls."""
    checks, skip, rounds = [], 0, 0
    while rounds < 20:
        rounds += 1
        pre = f"COMPAT_SKIP={skip} " if skip else ""
        text, status = env.run(f"{pre}/disk/{prog}; echo @@end $?", timeout)
        cur, idx = None, skip
        for line in text.split("\n"):
            m = NOTE.search(line)
            if m and cur is not None:
                cur["syscalls"].append(int(m.group(1)))
            if line.startswith("@@ "):
                cur = {"name": line[3:], "result": None, "detail": "", "syscalls": [], "index": idx}
                checks.append(cur)
                idx += 1
            elif cur is not None and line.startswith("ok ") and line[3:] == cur["name"]:
                cur["result"] = "pass"
            elif cur is not None and line.startswith("FAIL " + cur["name"] + ": "):
                cur["result"] = "fail"
                cur["detail"] = line[len("FAIL " + cur["name"] + ": "):]
            elif cur is not None and cur["result"] is None and line.startswith("[") and "not implemented" not in line:
                cur["detail"] = (cur["detail"] + " " + line).strip()    # a kernel note: a fault, a kill
        if cur is not None and cur["result"] is None:
            # The program stopped inside this check: hung, or died.
            cur["result"] = "hang" if status is None else "crash"
            if status is not None and not cur["detail"]:
                cur["detail"] = f"program exited with status {status}"
            skip = cur["index"] + 1
            continue
        if status is None and cur is None:
            checks.append({"name": "(start-up)", "result": "hang", "detail": "no output", "syscalls": [], "index": skip})
        elif status is not None and cur is None and skip == 0:
            checks.append({"name": "(start-up)", "result": "crash", "detail": f"exited with {status} before any check",
                           "syscalls": [int(n) for n in NOTE.findall(text)], "index": 0})
        break
    # A check that failed once may only have lost a race with the emulator's
    # timing. Run the failures again, once, and keep a pass.
    failed = [c for c in checks if c["result"] == "fail"]
    if failed:
        text, _ = env.run(f"COMPAT_ONLY={','.join(str(c['index']) for c in failed)} /disk/{prog}; echo @@end $?", timeout)
        for c in failed:
            if re.search(r"(?:^|\n)@@ " + re.escape(c["name"]) + r"\nok " + re.escape(c["name"]) + r"(?:\n|$)", text):
                c["result"], c["detail"], c["retried"] = "pass", "", True
    for c in checks:
        if c["result"] is None:
            # Its result line never came, though the program went on (say,
            # the shell running it was killed).
            c["result"] = "crash"
            c["detail"] = (c["detail"] + " no result line").strip()
        c["syscalls"] = sorted(set(c["syscalls"]))
        c.pop("index", None)
    return checks


# What differs between the guests' processors in the cases (debian.txt's).
ARCH_WORDS = {"x86_64": {"GLIBCLD": "/lib64/ld-linux-x86-64.so.2", "MULTIARCH": "x86_64-linux-gnu", "MACHINE": "x86_64", "SCMPARCH": "SCMP_ARCH_X86_64"},
              "aarch64": {"GLIBCLD": "/lib/ld-linux-aarch64.so.1", "MULTIARCH": "aarch64-linux-gnu", "MACHINE": "aarch64", "SCMPARCH": "SCMP_ARCH_AARCH64"}}
ARCH = "x86_64"

def arch_words(text):
    for k, v in ARCH_WORDS[ARCH].items():
        text = text.replace("%" + k + "%", v)
    return text

def run_applet(env, name, cmd, expected, timeout, bb):
    cmd = arch_words(cmd)
    expected = [arch_words(l) for l in expected]
    cmd = cmd.replace("ALPINEURL", env.hosturl + "/alpine")
    cmd = (cmd.replace("HOSTSURL", env.hostsurl).replace("HOSTURL", env.hosturl)
           .replace("HOSTADDR", env.hostaddr).replace("HOSTPORT", env.hostport)
           .replace("SSHPORT", getattr(env, "sshport", "")))
    if cmd.startswith("@host "):
        return run_on_host(name, cmd[6:], expected, timeout)
    text, status = env.run(f"echo @@begin; {with_bb(cmd, bb)}; echo @@end $?", timeout)
    lines = text.split("\n")
    try:
        start = lines.index("@@begin") + 1
    except ValueError:
        start = len(lines)
    body, calls, notes = [], [], []
    for line in lines[start:]:
        if line.startswith("@@end"):
            break
        m = NOTE.search(line)
        if m:
            calls.append(int(m.group(1)))
            line = NOTE.sub("", line)
            if not line:
                continue
        if KILLNOTE.match(line):
            if body and body[-1] == "":
                body.pop()                  # the blank line that comes before the note
            continue
        body.append(line)
    if status is None:
        return {"name": name, "result": "hang", "detail": "did not finish", "syscalls": sorted(set(calls))}
    ok = body == expected
    detail = "" if ok else "got " + json.dumps("\n".join(body)[:200]) + ", expected " + json.dumps("\n".join(expected))
    return {"name": name, "result": "pass" if ok else "fail", "detail": detail, "syscalls": sorted(set(calls))}


def run_on_host(name, cmd, expected, timeout):
    """A case's command run on this machine (from the repository), with
    its output (stdout and stderr) checked as a guest command's is."""
    try:
        p = subprocess.run(cmd, shell=True, cwd=os.path.dirname(os.path.dirname(HERE)), stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"name": name, "result": "hang", "detail": "did not finish (on the host)", "syscalls": []}
    body = p.stdout.decode(errors="replace").replace("\r", "").split("\n")
    while body and not body[-1]:
        body.pop()
    ok = body == expected
    detail = "" if ok else "got " + json.dumps("\n".join(body)[:200]) + ", expected " + json.dumps("\n".join(expected))
    return {"name": name, "result": "pass" if ok else "fail", "detail": detail, "syscalls": []}


def run_tty_once(env, name, prog, keys, check, expected, timeout, bb):
    if not env.keys(with_bb(prog, bb), keys, timeout):
        tail = getattr(env, "hang_tail", b"")
        detail = "the program did not return to the shell"
        if getattr(env, "hang_disk", 0):
            detail += "; the kernel reported a dead disk %d time(s)" % env.hang_disk
        if tail:
            detail += "; the console ended with " + json.dumps(tail.decode("latin-1")[-300:])
        return {"name": name, "result": "hang", "detail": detail,
                "syscalls": []}
    return run_applet(env, name, check, expected, timeout, bb)


def run_tty(env, name, prog, keys, check, expected, timeout, bb):
    """A tty.txt case: prog driven by keys, then check as an applet case.
    Keys typed at a full-screen program race the emulator's timing, and a
    loaded host can lose that race (a hang, or keys that arrive early). As
    for the cases of the other groups, a failure runs again once, and a pass
    is kept; the first try is printed so it does not go unseen."""
    first = run_tty_once(env, name, prog, keys, check, expected, timeout, bb)
    if first["result"] not in ("hang", "fail"):
        return first
    second = run_tty_once(env, name, prog, keys, check, expected, timeout, bb)
    if second["result"] == "pass":
        print(f"compat: {name}: passed on the second try (first: {first['result']}, {first['detail'][:120]})", flush=True)
        second["retried"] = True
    return second


# ---------------------------------------------------------------- reporting

def report(results, names, out_dir, elapsed):
    rows = [(g, c) for g, cs in results.items() for c in cs]
    total = len(rows)
    passed = sum(1 for _, c in rows if c["result"] == "pass")
    rank = {}
    for g, c in rows:
        if c["result"] == "pass":
            continue
        for n in c["syscalls"]:
            rank.setdefault(n, []).append(f"{g}: {c['name']}")
    ranked = sorted(rank.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    md = ["# BastiOS Linux compatibility report", "",
          f"{passed} of {total} checks pass ({elapsed:.0f} s). Generated by `make compat` (test/compat/run.py).", "",
          "| Group | Checks | Pass | Fail | Hang | Crash |", "|---|---:|---:|---:|---:|---:|"]
    for g, cs in results.items():
        cnt = {k: sum(1 for c in cs if c["result"] == k) for k in ("pass", "fail", "hang", "crash")}
        md.append(f"| {g} | {len(cs)} | {cnt['pass']} | {cnt['fail']} | {cnt['hang']} | {cnt['crash']} |")
    md += ["", "## Missing system calls, by checks blocked", "",
           "A system call counts against a check when the kernel reported it as not implemented while that "
           "check was running and the check did not pass.", "",
           "| Rank | System call | Nr | Checks | Examples |", "|---:|---|---:|---:|---|"]
    for i, (n, who) in enumerate(ranked, 1):
        ex = "; ".join(who[:3]) + (" ..." if len(who) > 3 else "")
        md.append(f"| {i} | {names.get(n, '?')} | {n} | {len(who)} | {ex} |")
    nosys = [(g, c) for g, c in rows if c["result"] != "pass" and not c["syscalls"]]
    md += ["", "## Failures with no missing system call", "",
           "These checks fail although every call they made exists: wrong results, missing files "
           "(/dev, /proc, /etc), unsupported flags, hangs, or crashes.", "",
           "| Group | Check | Result | Detail |", "|---|---|---|---|"]
    for g, c in nosys:
        md.append(f"| {g} | {c['name']} | {c['result']} | {esc(c['detail'])} |")
    md += ["", "## Every check", "", "| Group | Check | Result | Missing calls | Detail |", "|---|---|---|---|---|"]
    for g, c in rows:
        sc = ", ".join(names.get(n, str(n)) for n in c["syscalls"])
        md.append(f"| {g} | {c['name']} | {c['result']} | {sc} | {esc(c['detail'])} |")
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "report.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    with open(os.path.join(out_dir, "results.json"), "w") as f:
        json.dump({"passed": passed, "total": total, "groups": results,
                   "missing": [{"nr": n, "name": names.get(n, "?"), "checks": who} for n, who in ranked]}, f, indent=1)
    print(f"compat: {passed} of {total} checks pass")
    for g, cs in results.items():
        print(f"  {g:10} {sum(1 for c in cs if c['result'] == 'pass'):3} / {len(cs)}")
    print("compat: missing system calls, by checks blocked:")
    for n, who in ranked[:15]:
        print(f"  {names.get(n, '?'):20} {n:4} {len(who)}")
    print(f"compat: full report in {os.path.join(out_dir, 'report.md')}")


def esc(s):
    return s.replace("|", "\\|").replace("\n", " ")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default="build/bastios.elf")
    ap.add_argument("--disk", default="build/compat/disk.img")
    ap.add_argument("--out", default="build/compat")
    ap.add_argument("--dev-disk", help="the development disk (tcc and make) for dev.txt")
    ap.add_argument("--bastio-disk", help="the Bastio disk (bastioc1, z3, tcc) for bastio.txt")
    ap.add_argument("--alpine-disk", help="the Alpine disk (root file system, apk repository) for alpine.txt")
    ap.add_argument("--alpine-repo", help="the same repository's directory, served at ALPINEURL")
    ap.add_argument("--registry-image", help="the layer list (JSON) of an image in the cache, which the web server serves as a registry for ocipull.sh")
    ap.add_argument("--debian-disk", help="the Debian disk (a glibc root file system) for debian.txt")
    ap.add_argument("--node-disk", help="the Node disk (the root of the node:alpine image) for node.txt")
    ap.add_argument("--go-disk", help="the Go disk (the Go toolchain of the golang image) for go.txt")
    ap.add_argument("--jdk-disk", help="the JDK disk (the JDK of the eclipse-temurin image) for java.txt")
    ap.add_argument("--rust-disk", help="the Rust disk (the toolchain of the rust image) for rust.txt")
    ap.add_argument("--host", metavar="BINDIR", help="run on this machine, programs from BINDIR")
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--arch", default="x86_64", choices=["x86_64", "aarch64"],
                    help="the guest's processor; aarch64 runs the groups named by --groups, against baseline-aarch64.txt")
    ap.add_argument("--groups", help="comma-separated groups of the first disk to run (programs and busybox, by default all)")
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("programs", nargs="*")
    args = ap.parse_args()

    global ARCH
    ARCH = args.arch
    names = syscall_names()
    applets = load_applets(os.path.join(HERE, "applets.txt"))
    apps = load_applets(os.path.join(HERE, "apps.txt"))
    sshc = load_applets(os.path.join(HERE, "ssh.txt"))
    dev = load_applets(os.path.join(HERE, "dev.txt"))
    bastio = load_applets(os.path.join(HERE, "bastio.txt"))
    alpine = load_applets(os.path.join(HERE, "alpine.txt"))
    debian = load_applets(os.path.join(HERE, "debian.txt"))
    node = load_applets(os.path.join(HERE, "node.txt"))
    go = load_applets(os.path.join(HERE, "go.txt"))
    java = load_applets(os.path.join(HERE, "java.txt"))
    rust = load_applets(os.path.join(HERE, "rust.txt"))
    tty = load_tty(os.path.join(HERE, "tty.txt"))
    alpine_tty = load_tty(os.path.join(HERE, "alpine_tty.txt"))
    os.makedirs(args.out, exist_ok=True)
    log = open(os.path.join(args.out, "serial.log"), "wb")
    web = WebServer()
    if args.registry_image:
        web.add_registry(args.registry_image)
    env = Host(os.path.abspath(args.host), web) if args.host else None
    bb = os.path.join(os.path.abspath(args.host), "busybox") if args.host else BB
    t0 = time.time()
    results = {}

    def on_main_disk():
        """The groups the first disk holds, as (name, what, run) with run
        taking a guest."""
        g = [(prog, prog, lambda e, p=prog: run_program(e, p, args.timeout)) for prog in args.programs]
        g.append(("busybox", "busybox applets", lambda e: [run_applet(e, n, c, x, 30, bb) for n, c, x in applets]))
        g.append(("apps", "real programs", lambda e: [run_applet(e, n, c, x, 60, bb) for n, c, x in apps
                                                       if ARCH == "x86_64" or n.split(":")[0] in AARCH64_APPS]))
        if not args.host:
            g.append(("ssh", "logging in over SSH", lambda e: [run_applet(e, n, c, x, 60, bb) for n, c, x in sshc]))
        g.append(("tty", "programs on the terminal", lambda e: [run_tty(e, n, p, k, c, x, 30, bb) for n, p, k, c, x in tty]))
        if args.groups:
            keep = args.groups.split(",")
            g = [x for x in g if x[0] in keep or (x[0] in args.programs and "progs" in keep)]
        return g

    def lane(disk, groups, mem="256M"):
        """Boots a guest from disk and runs groups on it, one after another.
        Returns their results and the guest's serial output."""
        out = io.BytesIO()
        g = Guest(args.kernel, disk, out, web, arch=args.arch, mem=mem)
        try:
            g.boot()
            res = {}
            for k, what, run in groups:
                print(f"compat: {what}", flush=True)
                res[k] = run(g)
            return res, out.getvalue()
        finally:
            g.stop()

    def applets_lane(cases, timeout):
        return lambda e: [run_applet(e, n, c, x, timeout, bb) for n, c, x in cases]

    main_groups = on_main_disk()
    order = [k for k, _, _ in main_groups]
    if args.host:
        env.boot()
        try:
            for k, what, run in main_groups:
                print(f"compat: {what}", flush=True)
                results[k] = run(env)
        finally:
            env.stop()
            log.close()
            web.shutdown()
        report(results, names, args.out, time.time() - t0)
        return 0

    # Every lane is a guest booted from a disk of its own, running its
    # groups one after another. tcc and make are on a disk of their own (the
    # C runtime needs most of a directory), the Bastio compiler, Z3 and tcc
    # fill a third, and Alpine a fourth. With JOBS above 1 (the default is the number of
    # CPUs) the lanes run at the same time, and the first disk's groups are
    # split over two guests, the second on a copy of that disk: the guests
    # share nothing but the host's web server. Each lane's checks run in the
    # same order as before, and the serial logs are written in lane order.
    jobs = max(1, int(os.environ.get("JOBS") or os.cpu_count() or 1))
    scratch = tempfile.mkdtemp(prefix="compat-")
    if jobs > 1:
        cut = order.index("apps") + 1 if "ssh" in order else len(order)
        copy = os.path.join(scratch, "disk.img")
        shutil.copyfile(args.disk, copy)
        lanes = [(args.disk, main_groups[:cut]), (copy, main_groups[cut:])]
    else:
        lanes = [(args.disk, main_groups)]
    if args.dev_disk:
        lanes.append((args.dev_disk, [("dev", "building C (tcc, make)", applets_lane(dev, 60))]))
    if args.bastio_disk:
        lanes.append((args.bastio_disk, [("bastio", "the Bastio compiler (bastioc1, z3, tcc)", applets_lane(bastio, 900))]))
    if args.alpine_disk:
        # Alpine's root and its packages. The cases change the disk (they
        # unpack and install), so they get a copy and every run starts from
        # the same disk.
        disk = os.path.join(args.out, "alpine-run.img")
        shutil.copyfile(args.alpine_disk, disk)
        if args.alpine_repo:
            os.makedirs(os.path.join(web.dir, "alpine"), exist_ok=True)
            os.symlink(os.path.abspath(args.alpine_repo), os.path.join(web.dir, "alpine", args.arch))
        lanes.append((disk, [("alpine", "Alpine Linux (apk)", applets_lane(alpine, 300)),
                             ("alpine-tty", "programs on the terminal in Alpine",
                              lambda e: [run_tty(e, n, p, k, c, x, 60, bb) for n, p, k, c, x in alpine_tty])]))
    if args.debian_disk:
        # Debian's root, glibc's programs: unpacked on a copy, as above.
        disk = os.path.join(args.out, "debian-run.img")
        shutil.copyfile(args.debian_disk, disk)
        lanes.append((disk, [("debian", "Debian (glibc)", applets_lane(debian, 300))]))
    if args.node_disk:
        # V8 reserves a large range of address space and wants more memory
        # than the other lanes; the cases write the disk, so on a copy.
        disk = os.path.join(args.out, "node-run.img")
        shutil.copyfile(args.node_disk, disk)
        lanes.append((disk, [("node", "Node.js (V8)", applets_lane(node, 300))], "1G"))
    if args.go_disk:
        # The compiler and the linker are large static programs and the
        # runtime maps address space freely; the cases write the disk.
        disk = os.path.join(args.out, "go-run.img")
        shutil.copyfile(args.go_disk, disk)
        lanes.append((disk, [("go", "Go toolchain", applets_lane(go, 600))], "1G"))
    if args.jdk_disk:
        # The cases unpack a root of about 400 MB onto the disk: a copy.
        disk = os.path.join(args.out, "jdk-run.img")
        shutil.copyfile(args.jdk_disk, disk)
        lanes.append((disk, [("java", "JDK", applets_lane(java, 900))], "2G"))
    if args.rust_disk:
        # rustc is a large dynamic program, and the cases unpack a root
        # of about a gigabyte onto the disk: a copy.
        disk = os.path.join(args.out, "rust-run.img")
        shutil.copyfile(args.rust_disk, disk)
        lanes.append((disk, [("rust", "Rust toolchain", applets_lane(rust, 900))], "2G"))
    lanes = [l for l in lanes if l[1]]
    # Write out the copies just made before any guest reads a disk: a host
    # still flushing hundreds of MiB can hold up a virtio request for longer
    # than the kernel waits (DQ_TICKS), which it takes for a dead disk, and
    # tar then stops at the first block that did not come.
    os.sync()
    try:
        with concurrent.futures.ThreadPoolExecutor(len(lanes) if jobs > 1 else 1) as pool:
            done = list(pool.map(lambda l: lane(*l), lanes))
        for res, out in done:
            results.update(res)
            log.write(out)
    finally:
        log.close()
        web.shutdown()
        shutil.rmtree(scratch, ignore_errors=True)
    results = {k: results[k] for k in order + ["dev", "bastio", "alpine", "alpine-tty", "debian", "node", "go", "java", "rust"] if k in results}
    report(results, names, args.out, time.time() - t0)

    ids = [f"{g}: {c['name']}" for g, cs in results.items() for c in cs if c["result"] == "pass"]
    base = os.path.join(HERE, "baseline.txt" if args.arch == "x86_64" else f"baseline-{args.arch}.txt")
    if args.host:
        return 0
    if args.update_baseline:
        with open(base, "w") as f:
            f.write("# Checks that pass on BastiOS (make compat-baseline rewrites this).\n"
                    "# make compat fails if one of them stops passing.\n")
            f.write("".join(i + "\n" for i in sorted(ids)))
        print(f"compat: baseline rewritten, {len(ids)} checks")
        return 0
    if os.path.exists(base):
        want = [l.strip() for l in open(base) if l.strip() and not l.startswith("#")]
        lost = sorted(set(want) - set(ids))
        if lost:
            print("compat: REGRESSION, these checks passed before:")
            for i in lost:
                print("  " + i)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
