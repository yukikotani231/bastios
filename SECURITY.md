# Security policy

BastiOS is a research kernel. It has not been audited for hostile
multi-user use, and [docs/security.md](docs/security.md) lists what it
does not enforce yet. Within that, we would like to hear about security
bugs privately first.

## What counts

- A way for an unprivileged process to gain root, read or write another
  process's memory or files it has no permission for, or crash or take
  over the kernel.
- Network input that crashes the kernel or corrupts its memory.
- A gap between what [docs/security.md](docs/security.md) or
  [docs/trusted-base.md](docs/trusted-base.md) says is enforced and what
  the code does.

Gaps that docs/security.md already lists as open are known; an issue is
fine for those.

If the bug is that the Bastio compiler accepted unsafe kernel code, it is
a soundness bug: report it to [bastiolang](https://github.com/yukikotani231/bastiolang/blob/main/SECURITY.md)
instead.

## How to report

Use GitHub's private vulnerability reporting:
[open a draft advisory](https://github.com/yukikotani231/bastios/security/advisories/new).
Please include the commit, how you ran the kernel (QEMU command line or
hardware), and the steps or program that show the problem.

We will answer within a week, agree on a fix and a date with you, and
credit you in the advisory and the changelog unless you would rather not
be named.

## Supported versions

BastiOS is pre-1.0. Fixes go to `main` and the next release only.
