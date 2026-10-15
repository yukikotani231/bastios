# BastiOS documentation

BastiOS is a kernel written in [Bastio](https://github.com/yukikotani231/bastiolang), a language whose memory safety is proved at compile time. The [README](../README.md) says what it is and what state it is in. This page is the map of everything else.

## Start here

| If you want to... | Read |
| --- | --- |
| Build it, boot it in QEMU, write a Bastio program, run a Linux program | [Getting started](getting-started.md) |
| Use it as a system: users, network, SSH, Alpine and `apk` | [Using BastiOS](using.md) |
| Develop software on it: editor, compiler, debugger, containers | [Development on BastiOS](development.md) |
| Boot it on a real PC and report what happened | [Real hardware](real-hardware.md) |
| Run it as a cloud guest (virtio, ENA, microvm, PVH) | [Cloud guest](cloud.md) |
| Run it on AArch64 (QEMU `virt`) | [AArch64](aarch64.md) |

## How it works

| Page | What it covers |
| --- | --- |
| [Architecture](architecture.md) | The big picture: boot, memory, processes, the scheduler, system calls, file systems, the network, and what each proof guarantees |
| [Trusted base](trusted-base.md) | What is trusted instead of proved, file by file, and how that list is kept from growing unnoticed |
| [Linux programs](linux.md) | How Linux ELF programs are loaded, which system calls exist, container runtimes, overlay mounts, `ptrace`, network namespaces |
| [Users and permissions](security.md) | The security model, what is enforced, and the known gaps |
| [Performance](performance.md) | Measurements against Linux on the same emulated machine, and where the time goes |

## Design notes

| Page | What it covers |
| --- | --- |
| [The page cache](design/page-cache.md) | Shared pages behind `MAP_SHARED` file mappings |
| [Asynchronous disk I/O](design/async-disk.md) | The disk command queue, block cache and interrupt-driven completion |

## For contributors and maintainers

| Page | What it covers |
| --- | --- |
| [CONTRIBUTING.md](../CONTRIBUTING.md) | The workflow, the checks CI runs, and the rules for commits |
| [Linux compatibility suite](../test/compat/README.md) | What `make compat` runs and how to add a case |
| [Releasing](releasing.md) | How a release is cut |
| [CHANGELOG.md](../CHANGELOG.md) | What changed, by version |
| [SECURITY.md](../SECURITY.md) | How to report a vulnerability |

## The language

The language reference, tutorial and library reference live in bastiolang's [documentation index](https://github.com/yukikotani231/bastiolang/blob/main/docs/README.md) and on the [documentation site](https://yukikotani231.github.io/bastiolang/).
