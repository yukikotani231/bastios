# Development on BastiOS

BastiOS runs the programs people develop with: an editor, a C compiler, build tools, version control, debuggers, language runtimes and container runtimes, all as unmodified Linux binaries. This page lists what is exercised, how, and where the limits are. It is a map of `make compat`, not a promise: nothing here has been run on a real PC, only in QEMU.

Every item below is a case in the compatibility suite ([test/compat/README.md](../test/compat/README.md)). Where the suite is the only evidence, the case file is named so you can read exactly what was run and what output was expected.

## How the tools get onto BastiOS

BastiOS has no package repository of its own. The suite builds disk images from the official Docker Hub images, pinned by digest in `test/compat/images.mk`: it takes the image layers apart and writes the files onto a BastiOS disk (`test/compat/alpinedisk.py` and its siblings), so the binaries are the distribution's own. The same route works by hand: [Using BastiOS](using.md#6-alpine-linux-and-apk) unpacks Alpine's root file system onto the disk and enters it with `chroot`, and `apk` then installs packages. `make compat` builds the images (it needs network access and a lot of disk space the first time; set `ALPINE_CACHE` to keep downloads between runs).

Packages are installed from a repository served over HTTP from the host, because the test machines have no route to Alpine's mirrors.

## What is exercised

| Area | What runs | Where it is checked |
| --- | --- | --- |
| Editing | Neovim 0.11 over a serial or SSH terminal and inside `tmux`: editing and saving, undo files, `:terminal`, `:make` with the quickfix list, `:grep` with ripgrep, the LSP client against `lua-language-server`, `python-lsp-server` (on Alpine's python3) and `gopls` (with the Go toolchain unpacked under the Alpine root), tree-sitter highlighting for C, `vim.system`, file watchers, a small plugin manager that clones with git | `test/compat/alpine.txt`, `alpine_tty.txt` |
| C | gcc (with `cc1`, `as`, `ld`), tcc, GNU make, cmake; the edit, build and run loop from inside Neovim | `alpine.txt`, `alpine_tty.txt`, `dev.txt` |
| Bastio | bastioc1 with Z3 and tcc on the machine: edit a program, prove it, build it, run it; an edit that breaks a proof is rejected | `bastio.txt`, `alpine_tty.txt` |
| Debugging | `strace` (following children and attaching to a running process), `gdb` (breakpoints in PIE programs, attach, threads, core files) over `ptrace`, and Neovim's Termdebug plugin driving gdb over MI | `alpine.txt` |
| Version control and search | git (commit, clone over HTTP, gc, fsck), ripgrep, fzf, jq, rsync, openssl | `alpine.txt`, `apps.txt` |
| Language runtimes | Python 3 (asyncio, sqlite3, multiprocessing), Node.js 22 (V8's JIT), Go (compile and run, and `gopls` answering definition, diagnostics and format queries), Rust (rustc and cargo: build, test and run, and `rust-analyzer` finding a type error), Java (JDK 21: javac, java, jar, and Eclipse JDT LS finding a type error and answering a hover), Lua, QuickJS, MicroPython, Perl, sqlite | `alpine.txt`, `node.txt`, `go.txt`, `rust.txt`, `java.txt`, `apps.txt` |
| Containers | `runc` and `crun`: own hostname, PID 1, cgroup limits (memory, pids, cpu), seccomp filters, read-only roots, overlay roots, rootless containers in a user namespace, a bridge to the host, images pulled from a registry with `tools/ocipull.sh` | `alpine.txt`; [Linux programs](linux.md#container-runtimes) |
| Remote access | Dropbear SSH server with a pseudo-terminal, Neovim over SSH | `ssh.txt`, `alpine.txt` |

## Limits to know about

- There is no clangd: the only suitable image is larger than the Alpine disk has room for. Neovim's LSP client is exercised against `lua-language-server`, `python-lsp-server` and `gopls`.
- Everything is measured under QEMU's software emulation in CI, so timing-sensitive checks are flaky under load; [performance.md](performance.md) has the numbers that exist.
- The suite runs cmake, the Go, Rust and Java toolchains on x86-64 only; their images have no AArch64 build in it.
- Kernel limits that matter to development (processes, open files, address space, files per disk) are in the README's Status section.
- The user database and SSH host keys are kept in memory; the working tree, packages and `/root` live on the disk.
- The checks use a serial console or an SSH terminal. There is no graphical desktop.

To see what failed or is skipped on your machine, run `make compat` and read `build/compat/report.md`.
