# BastiOS

A kernel written in Bastio (github.com/yukikotani231/bastiolang). Open source: write all code, comments, docs and commit messages in English.

- `make test` boots the kernel in QEMU and drives the shell over the serial line. It needs `../bastiolang` built, or `BASTIOC=...`.
- Kernel logic goes in Bastio (`src/`). Assembly and C (`boot/`) are the trusted base: add to them only what Bastio cannot express yet, and say why in the commit.
- Every new shell command or kernel feature gets a case in `test/shell_test.sh`.
- Commits: `<area>: <summary>`. The body says honestly what is proved and what is trusted.
