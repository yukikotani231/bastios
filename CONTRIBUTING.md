# Contributing to BastiOS

Thanks for your interest. BastiOS is pre-release and moves together with [bastiolang](https://github.com/yukikotani231/bastiolang): a kernel change often needs a compiler change first, and large changes are best discussed in an issue.

## Getting oriented

- [README.md](README.md) says what BastiOS is, how to build and boot it, and where things are.
- [docs/README.md](docs/README.md) indexes the rest; [docs/architecture.md](docs/architecture.md) is the place to start reading the kernel.
- You need the same tools as for a build: Rust, Z3, a C compiler, QEMU and musl-gcc, with bastiolang checked out next to this repository (see [Getting started](docs/getting-started.md#1-build)).
- A change that needs a language feature or a compiler fix goes to [bastiolang](https://github.com/yukikotani231/bastiolang) first; say so in the pull request.

Bugs and ideas go in the issue tracker. For a crash or a hang, include the commit, the QEMU command line (or the hardware), and the console log. A security problem goes through [SECURITY.md](SECURITY.md), not an issue.

## Before you send a pull request

1. Build and test against bastiolang's `main` (see "Build and run" in [README.md](README.md)):
   ```sh
   make test
   ```
   CI runs the same thing (the `test` job), plus `native` (the native backend), `compat` and `compat-aarch64` (the Linux compatibility suite, `make compat`), `compat-ltp` (`make compat-ltp`: the Linux Test Project's system call tests, which must not do worse than `test/compat/ltp_baseline.txt`) and `ena` (the AWS adapter model). A change to a Linux system call should also pass `make compat` locally. On a machine with 4 or more CPUs the tests run in three lanes at once (`test/all.sh`); `JOBS=1 make test` runs them one after another.
2. Kernel logic goes in Bastio (`src/`, `user/*.bastio`), where it is proved. Assembly and C (`boot/`, `user/start.S`) are the trusted base ([docs/trusted-base.md](docs/trusted-base.md)): add to them only what Bastio cannot express yet, say why in the commit, and update `test/trusted_base.txt` with `test/trusted_base.sh --update`.
3. Every new shell command or kernel feature gets a case in `test/shell_test.sh`.
4. Documentation that describes the change is updated in the same pull request: the README, the page under `docs/`, and the sessions in them (`make test` runs the ones marked `guide-test`).
5. A change a user would notice gets a line under `## [Unreleased]` in [CHANGELOG.md](CHANGELOG.md).

Don't copy counts (lines, pages, tests) into prose; they go stale. Give the command that counts them instead.

## Commits

`<area>: <summary>`, where area is `kernel`, `boot`, `user`, `build`, `test`, `docs`, `ci` and so on. The body says honestly what is proved and what is trusted.

## License

Unless you say otherwise, any contribution you submit for inclusion is dual licensed as Apache-2.0 OR MIT, as described in [README.md](README.md#license), without any additional terms or conditions.
