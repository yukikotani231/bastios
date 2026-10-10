# BastiOS architecture

How the kernel is put together, and which part of it is proved. Read the [README](../README.md) first for what BastiOS does and how to build it. The language the kernel is written in is described in bastiolang's [language reference](https://github.com/yukikotani231/bastiolang/blob/main/docs/language.md); the capability types below (`Pool`, `Frame`, `PageTable`, `Mmu`, `Cpu`, `Serial`) are explained in its "Kernel capabilities" section.

## The big picture

```text
            proved (Bastio)                          trusted (C, assembly)
  +--------------------------------------+   +----------------------------------+
  | user/*.bastio   sh, hello, spin, ... |   | user/start.S   image header,     |
  |   (--target user, ring 3)            |   |                calls main, exits |
  +------------------|-------------------+   +----------------------------------+
                     | int 0x80 / timer / fault
  +------------------v-------------------+   +----------------------------------+
  | src/main.bastio     scheduler loop   |<->| boot/boot.S    multiboot, long    |
  | src/syscall.bastio  system calls     |   |                mode, trap stubs, |
  | src/proc.bastio     processes        |   |                bst_cpu_enter     |
  | src/fs.bastio       open files       |   |                                  |
  | src/ramfs.bastio    in-memory files  |   |                                  |
  | src/diskfs.bastio   files on disk    |   |                                  |
  | src/blk.bastio      disk: NVMe,      |   |                                  |
  |   virtio, AHCI or ATA (nvme,         |   |                                  |
  |   virtio_blk, ahci, ata.bastio)      |   |                                  |
  | src/net.bastio      IPv4 stack over  |   |                                  |
  |   card.bastio: virtio_net or e1000   |   |                                  |
  | src/virtq.bastio    virtqueues, and  |   |                                  |
  |   pci.bastio                         |   |                                  |
  | src/space.bastio    address spaces   |   | boot/platform.c  TSS, IDT, PIC,  |
  | src/pages.bastio    page allocator   |   |                timer             |
  | src/console.bastio  kernel messages  |   | boot/smp.c, ap.S  start the other|
  |   (--target kernel, ring 0)          |   |                processors        |
  |                                      |   | kernel prelude (in bastiolang):  |
  |                                      |   |   COM1, arena, PTE encoding, CR3 |
  +--------------------------------------+   +----------------------------------+
```

**Devices.** Drivers are Bastio and reach hardware only through capabilities claimed once in `main`: I/O ports (`PortIo`: serial, PIC, PIT, PCI configuration, ATA, virtio), memory-mapped registers (`Mmio`: the HPET, the framebuffer, an xHCI controller, an NVMe drive and an e1000 card, in the device window `boot.S` maps uncached: 2-4 GiB with 2 MiB pages, then up to 2^46 with 1 GiB pages, where firmware puts 64-bit BARs) and pages lent to devices for DMA (`Dma`). Both virtio drivers share `src/virtq.bastio` (the legacy register block and the virtqueue). The disk is a virtio block device when the machine has one: `src/virtio_blk.bastio` lends it four pages (the virtqueue and one request) at boot, programs their physical addresses into the device, and takes them back only after resetting it at power off. While they are lent the prover rejects any attempt to free, map or reuse them, and every access to them is volatile and inside the page. That the device writes only those pages, and stops when reset, is trusted. An NVMe drive comes first when there is one (`src/nvme.bastio`): its BAR, 32- or 64-bit, is claimed as an `Mmio` once it is checked to lie in the device window clear of the framebuffer, and five pages are lent as `Dma` (the admin and I/O submission and completion queues, 64 entries each, and one data page). The driver disables the controller, sets up the admin queue, reads namespace 1's size and block size (only 512-byte blocks are used), and creates one I/O queue pair without interrupts. Each read or write is one command for one sector through the data page, waited for by polling the completion queue's phase bit; a write is followed by a flush, so it is on the medium when it returns. At power off the controller is shut down (which writes back its cache) and disabled before the pages come back. Register, queue and page accesses are proved in bounds; that the controller writes only the lent pages is trusted. Without NVMe or virtio, a SATA disk on an AHCI controller is used (`src/ahci.bastio`): its BAR 5 is claimed as an `Mmio`, and two pages are lent, one holding the command list, the received-FIS area and the command table, the other the data. The driver takes the controller from the firmware (BIOS/OS handoff), sets AHCI mode, picks the first implemented port whose link is up with a disk's signature, stops it, points it at the pages and starts it. IDENTIFY gives the size; each read or write is one READ/WRITE DMA EXT in command slot 0 with one PRD entry, polled until the slot clears, and a write is followed by FLUSH CACHE EXT. At power off the port is stopped before the pages come back. Without any of these the disk is the ATA drive, driven by programmed I/O. Every driver finds its device by walking PCI configuration space through the legacy ports: every bus the firmware numbered, every slot, and the other functions of multi-function devices.

**Console.** The console (`Con`, `src/console.bastio`) is COM1 plus, when there is one, a screen and a keyboard. Output goes to both; input is taken from the serial port first, then the keyboard. The screen (`src/fb.bastio`) is the linear framebuffer the boot loader set up: the kernel's Multiboot header asks for 1024x768 at 32 bits per pixel, GRUB sets it through VBE under BIOS or GOP under UEFI, and the `boot_fb` builtin hands the kernel what GRUB reported. If it is 32-bit RGB and lies in the device window below the I/O APIC (under QEMU it is at 0xFD000000 under BIOS and at 2 GiB under UEFI, which is why the window starts there), the kernel claims it as an `Mmio` and draws text in 8x16 cells: an 8x8 public-domain font doubled vertically. Every pixel write is proved inside the claimed block. The text is kept in a byte buffer with a copy of what each cell shows, so scrolling redraws only the cells that change; ANSI escape sequences are skipped. Booted with `-kernel`, QEMU sets no video mode and the console is serial only. The keyboard (`src/kbd.bastio`) is a PS/2 keyboard on the i8042, polled through its two ports: scan code set 1 becomes the bytes a terminal would send (Enter is CR, Backspace DEL, Ctrl-C 3), with Shift and Ctrl tracked; mouse bytes and other extended keys are dropped. A USB keyboard works too (`src/xhci.bastio`): the first xHCI controller on PCI gets its registers claimed as an `Mmio` (the BAR, sized the usual way, in the device window and clear of the framebuffer) and eleven pages lent as `Dma` (the slot array, the command ring, the event ring and its segment table, the device and input contexts, two transfer rings, and four scratchpad pages). The driver takes the controller from the firmware (USB legacy support), resets and starts it, and attaches the first device on a root port that offers a HID boot keyboard interface: a slot, an address, the configuration, the boot protocol, and its interrupt IN endpoint, on which one 8-byte transfer is always queued. Each finished one is a boot report, and keys newly down become the same bytes as on PS/2. It is polled with the other inputs; at power off the controller is reset and the pages come back. Every register and page access is proved in bounds; what the controller writes into the lent pages is read defensively (descriptor lengths are bounded, unknown events skipped) but trusted to stay in them. There is no hub support. That GRUB reports the framebuffer truthfully is trusted.

**Network.** A virtio network card, when present, gets nine pages the same way (two virtqueues, four 2 KiB receive buffers, one transmit page). Without one, an Intel e1000 (82540EM, `src/e1000.bastio`, the card most hypervisors and many older PCs offer) is driven instead. Its 128 KiB register block is claimed from the device window as an `Mmio` once its BAR is checked to lie in the window below the I/O APIC. It gets six pages: both descriptor rings, eight 2 KiB receive buffers and one transmit page. It is polled, like virtio. `src/card.bastio` puts the two drivers behind one `Card` type. `src/net.bastio` is a minimal IPv4 stack on it. Its address, netmask and gateway come from a DHCP client that starts when the card does (DISCOVER, OFFER, REQUEST, ACK; retried on the timer, no lease renewal), falling back to QEMU's user-mode defaults (10.0.2.15/24, gateway 10.0.2.2) if no server answers four tries; the shell's `ifconfig` shows them (system call 47). The stack speaks Ethernet, ARP (it answers requests for its address and keeps a four-entry cache), IPv4 without options or fragments (header checksum checked on the way in), ICMP echo both ways, and UDP with checksums sent and checked. Every field is read out of a byte buffer, and the proof shows no length read off the wire can take a read or write outside it: a header claiming more bytes than arrived is dropped. The card (`Nic`) lives in the `Kernel` struct, inside the shared `Sys` state, and each processor's scheduler polls it under the lock on every pass. Nothing in the stack waits, so no network wait holds the lock: a frame whose next hop has no cached MAC is held (one at a time) and sent when the ARP answer arrives, and a ping (system call 40) puts its process in the `NETWAIT` state until `net_wake`, run on every scheduler pass, finds the reply or the two-second deadline. On top sits a table of eight UDP sockets (`sock_new`, `sock_bind`, `sock_sendto`, `sock_recvfrom`, `sock_ready`, `sock_peer_ip`/`sock_peer_port`, `sock_port`, `sock_close`), each queueing up to four datagrams (later arrivals are dropped, which UDP allows); unbound sockets get an ephemeral port (49152 and up) on their first send, and `sock_recvfrom` never waits: blocking belongs to the caller. A port no socket has falls back to the echo service on 7. The shell reaches sockets through native system calls 41-46 (`udpecho`, `udpsend`). `src/tcp.bastio` adds TCP: a table of eight connections, each with a 4 KiB receive and a 4 KiB send buffer, opened by listening (`tcp_listen`, `tcp_accept`) or connecting (`tcp_connect`, then `tcp_status`), with `tcp_send`, `tcp_recv` (0 at the end, `TCP_AGAIN` when nothing has arrived) and `tcp_close`, none of which waits. Data that arrives early is held in the receive buffer at its place (one stretch of it per connection) and joins once the gap before it is filled. Unacknowledged data is resent go-back-N; the timeout starts at one second and doubles on each resend, up to 64 seconds, and the connection is reset after eight unanswered resends. Segments keep to the MSS the peer asked for, or 536 bytes if it didn't say. A zero window is probed with one byte for as long as the peer keeps answering. There is no congestion control, window scaling or selective acknowledgement. `test/tcp_test.sh` checks these paths, which QEMU's user-mode network never takes. It joins the card to the script through QEMU's `dgram` netdev, and the script plays the network: ARP, DHCP, and a TCP peer that drops, delays, reorders and duplicates segments, by rule or on a seed (`SEED=n`). The shell reaches TCP through native system calls 48-55 (`tcpecho`, `tcpget`). Sending to the machine's own address does not loop back.

The Bastio compiler turns each Bastio file into freestanding C, after proving it. The C is compiled and linked with the trusted parts into one kernel image, and each user program into a flat image that the kernel embeds. The boundary between the two columns is the set of capability builtins: Bastio code never sees an address it could dereference, only linear values (`Frame`, `PageTable`, ...) that the builtins turn into memory accesses.

## Booting

1. QEMU loads `build/bastios.elf` with its Multiboot 1 loader. (The kernel is linked as 64-bit code and repackaged as ELF32, because that loader accepts only 32-bit images.) From `build/bastios.iso` (`make iso`), GRUB loads the same file with Multiboot 2, under BIOS or UEFI (`boot/grub.cfg`): the kernel carries both headers. Multiboot 2 also passes a copy of the ACPI RSDP, which UEFI firmware gives only to EFI programs. Once in long mode, `boot/multiboot.c` copies the Multiboot 2 fields the kernel uses (memory map, framebuffer, RSDP) into the kernel image, in the Multiboot 1 form, so everything after it reads one format.
2. `boot/boot.S` starts in 32-bit protected mode, identity-maps the first 1 GiB with 2 MiB pages (kernel-only: no user bit), turns on long mode with write protection, and loads a GDT with kernel and user segments.
3. `bst_platform_init` (`boot/platform.c`) fills in the TSS (the stack a trap from ring 3 lands on), the interrupt table (vector `0x80` is raisable from ring 3; #DB, NMI, #DF and #MC switch to a 4 KiB IST stack in the processor's block, because the first instructions of the `syscall` entry run in ring 0 on the user's stack), the two 8259 interrupt controllers, and a 100 Hz timer.
4. `bst_smp_init` (`boot/smp.c`) starts the other processors. It finds them in the ACPI MADT (every firmware read checked against the table's length and the identity map), reached from the RSDP: the copy Multiboot 2 passed, else where BIOS firmware puts it, finds the local APIC (inside the device window boot.S maps uncached), copies the trampoline in `boot/ap.S` to physical `0x8000`, and sends each application processor INIT and startup IPIs, one at a time. The processor climbs from real mode to long mode on the kernel's own page tables, loads the kernel's GDT and IDT, counts itself in, and parks with interrupts off, waking on its local APIC timer to look for work. `cpu_count` tells the kernel how many processors are up, and the shell's `cpus` command shows it. The kernel hands every parked processor the scheduler with `smp_run` (`bst_platform_smp_start` / `bst_platform_smp_join` in `boot/smp.c`), see "The scheduler loop".
5. `bst_kmain` (the kernel prelude, generated by bastiolang) initialises COM1, builds the `Pool` from the Multiboot memory map (every page from the end of the kernel image to the end of the run of available RAM it lies in, capped at the identity-mapped 1 GiB; without a map, from the memory size) and calls the Bastio `main` with the four capabilities it is entitled to: `&mut Serial`, `&mut Pool`, `&mut Mmu`, `&mut Cpu`.
6. When `main` returns, `boot.S` writes to QEMU's debug-exit port and the machine powers off.

## Memory

**Physical pages.** `src/pages.bastio` keeps a `Mem`: the `Pool` capability together with a bitmap of free pages, and a hint `low` below which no page is free. The spec `mem_ok` says the bitmap agrees with the pool (`get(bm, i) == 1` exactly when `pool_has(pool, i)`), and every function that touches `Mem` must preserve it. Since `pool_take` demands `pool_has`, and `take_page` demands the bitmap bit, a page is never handed out twice. That is proved, not tested. `count_free` at power off is how the test checks that nothing leaked.

**Virtual memory.** Every top-level (level-4) table has two parts:

| Slot | Maps | Owner |
| --- | --- | --- |
| 0 | The kernel: the boot identity map of the first 1 GiB, without the user bit | Not owned; `pt_new` points it at the kernel's own map in every new level-4 table, and the prover rejects any `pt_put` or `pt_take` on slot 0 |
| 1 | The process's user window: `WPAGES` = 16384 pages (64 MiB) from `USER_BASE` = `0x8000000000` | The process, through one table at each of levels 3 and 2 and up to `WIN_L1S` = 32 level-1 tables, each made when a page under it is first mapped |

The program image starts at page 0 of the window and the stack takes the top `STACK_PAGES` pages. Nothing else is mapped at the start: a Linux program's heap (`brk`) grows up from the image and its `mmap` pages come down from the stack, each page mapped when asked for. `new_space` (`src/space.bastio`) builds this from the bottom up, taking every page from `Mem`; if memory runs out half way, it frees what it built and returns `Space::Empty`, so a failed spawn leaks nothing. `free_space` tears it down level by level; `pt_free` and `pt_free_root` require an empty table, so the proof shows every page is returned, and `pt_free_root` also requires that the table is not the one in CR3.

**Kernel heap.** `Buf`, `Vec` and `Box` values in the kernel come from the prelude's fixed 8 MiB arena, not from the page pool. Running out of it halts the machine (see "Limits").

**Reading user memory.** The kernel cannot dereference a user pointer. To copy a user buffer (`write`, `spawn`'s program name), it takes the level-3 and level-2 tables out of their parents, then for each page takes its level-1 table out of the level-2 one and the page out of that (`w_read`, `w_write` and `w_byte` in `src/space.bastio`), reads or writes the bytes it needs on that page, and puts everything back. Ownership is moved, never aliased, so the proof covers these copies too. `user_range` checks first that the buffer lies inside the window; a page that is not mapped ends the copy early.

## Processes

`src/proc.bastio` holds the kernel's state in one struct:

```text
struct Kernel {
  mem: Mem,             // the page allocator
  spaces: Vec[Space],   // element pid is that process's address space
  procs: Buf,           // the process table: a row of STRIDE words per pid
  q: Buf,               // console input waiting for a read
  ticks: u64,           // timer ticks since boot
}
```

The spec `kok(k)` (allocator consistent, one `Space` per pid with every mapped space a level-4 table, table and queue the right sizes) is a precondition and postcondition of everything that takes the kernel. `Mmu` and `Cpu` stay with `main` and are lent where needed.

A process is a row of the table, indexed by pid (1 to `NPROC - 1`; pid 0 means "none"). The row holds the state, the parent, the program, a tick count, a wait target or exit code, and the 18 saved user registers in `cpu_get` order. Address spaces live in `spaces`, outside the table, because a `PageTable` is linear and a `Buf` row can only hold numbers. Code that needs a process's space takes it out with `take_space` (which leaves `Space::Empty` behind) and puts it back with `put_space`. While it is out, nothing else can reach it.

States: `FREE`, `READY`, `READING` (blocked on console input), `WAITING` (blocked on a child), `ZOMBIE` (exited; its code is held for the parent), `RUNNING` (on a processor, with its space and FPU page taken out), `KILLED` (killed while `RUNNING`: it ends when its processor brings it back), `NETWAIT` (waiting for a ping's reply), `CONWAIT` (writing to the console while its output ring is full; the call restarts once the ring is half empty). `spawn` builds the address space first and claims a row only if that succeeded. `terminate` frees the space, hands the exit code to a waiting parent, and orphans the children.

Programs are not loaded from disk: `prog_image` returns each program's flat image with `include_bytes`, and `spawn` checks its header (magic `BSTUSER\n`, memory size, entry point inside the image) before mapping it. Only the image bytes are known at compile time, so the header is checked at run time like any untrusted input.

## The scheduler loop

Every processor runs the same loop, `sched` (`src/main.bastio`), started on all of them at once by `smp_run(sched, ..)`. Each passes its own `Cpu` and `Mmu`; what they share is one `Lock[Sys]` holding the kernel's state (`Kernel`), the console's output ring (`Con`), the interrupt controller and the HPET clock, and a separate `Lock[Term]` for the console's devices (serial port, screen, keyboard). The lock's invariant, `kok(k) && con_ok(con) && pic_ok(pic) && hpet_ok(clock)`, is what a processor may assume when it takes the lock and must restore before it lets go; the prover checks both (bastiolang `docs/language.md`, "Locks and struct invariants"). Kernel code runs with interrupts off, so a processor never takes an interrupt while it holds the lock.

1. With the lock (`next_job`): after an interrupt or an idle wait (not after a system call), poll the network card and hand typed input to processes blocked in `READ` (`wake_readers`); then pick the next `READY` process after this processor's last one, round robin (`pick`). If there is none: stop if no process is alive, else wait for an interrupt without the lock (`cpu_idle`). Otherwise take its address space and FPU page out, load its registers and FPU state into this processor's `Cpu`, and mark it `RUNNING`, so no other processor picks it.
2. Without the lock: load its table into CR3 (`mmu_switch`), enter user mode until the next trap (`cpu_run`), and switch back to the kernel's map (`mmu_kernel`).
3. With the lock (`finish`): save the FPU state, put the space and page back, save the registers, error code and fault address into its row, and act on the trap vector:
   - `32` (the PIT, on the bootstrap processor) or `48` (each other processor's local APIC timer): count the tick and move on to the next process. This is the preemption.
   - `0x80` (system call): handle it (`syscall`); it says whether to keep running the same process, switch, or power off.
   - below `32` (an exception): print the fault and kill the process with exit code `128 + vector`. The kernel itself is unaffected.

   Step 3 and the next step 1 are one critical section (`turn`), so a system call costs one round trip of the lock.
4. Without `Lock[Sys]`, if the console has output waiting (an `Atomic` count every critical section publishes) or an interrupt just came in: pump the console (`try_pump`). Kernel code never drives the console's devices; it appends to the 64 KiB output ring in `Sys`. One processor at a time pumps: it takes an `Atomic[bool]` with compare-and-swap, and the others skip instead of waiting. The pumper takes `Lock[Term]` and moves up to 16 chunks of 256 bytes. For each chunk it reads what was typed, takes `Lock[Sys]` just long enough to queue that input and take the next chunk of output (`exchange`), then writes the chunk to the serial port and the screen with `Lock[Sys]` free. The two locks only ever nest in the order `Term`, then `Sys`, which bastiolang's lock-order check (`E-LOCK-ORDER`) confirms. A `write` that does not fit in the ring blocks in `CONWAIT` and restarts once `exchange` has freed half the ring, so no byte is dropped. An idle processor pumps instead of sleeping while output is waiting and no other processor is pumping.

`kill` of a process that is `RUNNING` on another processor only marks it `KILLED`; that processor ends it in step 3. Power off sets a flag that every processor sees the next time it takes the lock, and `smp_run` returns once all of them have left the loop, so every space is back in the table. Then `main` takes the state out of the lock (`lock_end`), pops every remaining space and frees it, and prints the free page count, which `test/shell_test.sh` compares with the count at boot.

### How it scales

`make smpbench` (`test/smpbench.sh`) runs N copies of `bench` at once, on one processor and on four. The numbers below are wall seconds, measured 2026-10-10 under QEMU 8.2 with multi-threaded TCG (no KVM) on a 4-core host:

| | `-smp 1`, 4 copies | `-smp 4`, 4 copies | `-smp 4`, 1 copy |
| --- | --- | --- | --- |
| console devices written under `Lock[Sys]` (before) | 8.7 | 48.0 | 5.2 |
| console ring, pumped outside the lock (now) | 8.1 | 17.0 | 3.1 |

One copy on one processor takes about 2 s either way. I found where the time went by changing one thing at a time:

- **Almost all of it was the serial console.** `bench` writes 1 MiB to the console per copy, and each byte is two port accesses. With those writes made no-ops, the old kernel ran the four copies in 3.4 s on one processor and 4.4 s on four. Under single-threaded TCG (`-accel tcg,thread=single`, which emulates every processor on one host thread) it took 7.8 s and 8.9 s. So the kernel's own work scaled, and QEMU's cost of the UART did not.
- **QEMU serializes port I/O, and `pause` contends with it.** In multi-threaded TCG, every port access takes QEMU's global lock. So does every `pause` a processor executes: the instruction leaves the translated code, and QEMU takes that lock again before it re-enters. The old kernel held `Lock[Sys]` across its UART writes, so the other processors spun on that lock with `pause` and fought the writer for QEMU's lock. Even one copy on four processors ran at about a third of the UART speed of one processor. With the other three processors never started, it ran at full speed.
- **Every fix is also right on real hardware.** On a real 115200-baud line, holding the kernel lock for a 256-byte write stops every processor for 22 ms. The changes:
  - The devices are now written outside `Lock[Sys]`, by one processor at a time, and the others never wait for them.
  - A full ring blocks the writer instead of having it retry.
  - An idle processor sleeps instead of spinning while another one pumps.
  - Devices are no longer polled on every system call.
  - A system call takes the lock once instead of twice.

What is left is mostly emulation:
- `pause` still costs. As an experiment (not a change, because `pause` is the right instruction on real hardware), removing it from the spin loop cuts getpid on four copies from about 190 ticks to about 85, against about 130 on one processor.
- QEMU's four processor threads and its I/O thread share the host's four cores.
- A writer woken when the ring drains waits up to one 10 ms tick for an idle processor to pick it up: BastiOS sends no wake-up interrupts between processors. Measured and rejected (2026-10-10): sending one to each idle processor (claimed as an `Mmio` block of the local APIC) left 4 x `bench` unchanged within noise and made a single `bench`'s spawn+exit half as fast (49 to 94 ticks), because the child of a spawning parent then starts on another processor instead of on the parent's when it blocks in wait4. Waking only for writers, or with affinity, may still pay off; it needs a benchmark that shows it.

These numbers should be taken again on KVM or real hardware before `Lock[Sys]` is split further.

## System calls

A user program raises `int 0x80` with the call number in `rax` and arguments in `rdi`, `rsi`, `rdx`; the result comes back in `rax`. The numbers are the ones in bastiolang's user prelude, which provides the `sys_*` builtins:

| Number | Call | Effect |
| --- | --- | --- |
| 0 | exit(code) | Terminate the caller |
| 1 | write(va, n) | Write user bytes to the console; returns the count |
| 2 | read() | One byte of console input, blocking |
| 3 | spawn(va, n) | Start the program whose name is the user bytes; returns the pid or 0 |
| 4 | wait(pid) | Wait for a child and return its exit code |
| 5 | yield() | Give up the CPU |
| 6 | getpid() | The caller's pid |
| 7 | info(k) | 0: free pages; 1: ticks since boot; 2: processors running |
| 8 | ps() | Print the process table |
| 9 | kill(pid) | Terminate a process with code 137 |
| 10 | poweroff() | Stop the machine |
| 11 | open(flags, va, n) | Open the file named by the user bytes; flags `O_READ` 1, `O_WRITE` 2, `O_CREATE` 4, `O_TRUNC` 8, `O_APPEND` 16; returns a descriptor (0 to 7) |
| 12 | close(fd) | Close a descriptor |
| 13 | fread(fd, va, n) | Read up to n bytes (at most 4096) at the descriptor's offset into user memory; returns the count, 0 at the end |
| 14 | fwrite(fd, va, n) | Write up to n user bytes (at most 4096); returns the count |
| 15 | unlink(_, va, n) | Remove a file that no descriptor has open |
| 16 | dirent(i, va, n) | Copy the name of directory entry i; returns its length, 0 for an unused entry |
| 17 | seek(fd, off) | Move a descriptor's offset |
| 18 | stat(_, va, n) | The size of a file |

Calls 0 to 10 have named builtins in bastiolang's user prelude. The file calls (11 to 18) do not: user programs make them with the generic `sys_call(n, a, b)`, `sys_send(n, a, buf, len)` (lends the kernel bytes of a buffer to read) and `sys_recv(n, a, buf)` (lends a buffer to overwrite), wrapped in `user/lib/file.bastio`. A failed call returns `u64::MAX`.

Every argument is an untrusted number. The handlers check ranges at run time (`user_range`, pid bounds, states), and the proof makes sure no check is missing: a handler that indexed the process table with an unchecked pid would not compile.

`test/fuzz_test.sh` (`make fuzz`, and a short run in `make test`) checks this from the outside. `user/sysfuzz.c`, a freestanding program, makes random calls through both entry points (`syscall` with Linux numbers and `int 0x80` with native ones) with hostile arguments: kernel, device and non-canonical addresses, pointers at the edges of the user window and running off mapped pages, huge and negative lengths, bad descriptors. Several run at once on several processors. The kernel must not fault or stop answering, and every page must be back at power off. Calls that would only end or stall the fuzzer itself (exit, execve, signals to other processes, a read with nothing to read) are left out or tamed, and a seed replays the same calls.

## Files

The file system is in memory (`src/ramfs.bastio`). Its directory is a flat table of 32 nodes, each free or one file: a name, a size, and a vector of pages, where page i holds bytes 4096 i to 4096 (i + 1). A page the file has never written is a hole and reads as zeros. The pages are `Frame`s taken from the page allocator, so a file owns its pages the way a page table owns the pages it maps: the proof shows that truncating or removing a file gives every page back once, and power off frees every file before counting the free pages.

`src/fs.bastio` sits between the system calls and the file system. It owns the open-file table (8 rows per process; a descriptor is a row number, so a process can only name its own files) and a 4096-byte transfer buffer. Each transfer is two copies: user memory to the buffer and the buffer to the file, or back (`src/usermem.bastio` walks the user window as the console `write` does). The file system below only ever sees the buffer. Each open row records which file system its file is on, and the few functions that touch a file dispatch on it; the system calls above do not know there are two.

Names that start with `disk/` are on the disk (`src/diskfs.bastio`), which keeps files across power off. The disk is cut into 4096-byte blocks: block 0 holds the magic `BSTDISK2`, blocks 1 to 4 a directory of 64 entries of 256 bytes, and the rest holds file data and the maps that find it. An entry holds its kind, its parent entry, one path component of up to 200 bytes, a size, a mode, a modification time, and its root map: a block of 512 block numbers, each naming a first-level map of 512 data blocks, so a file can reach 1 GiB as far as the disk allows. Block number 0 is a hole and reads as zeros. Which blocks are free is not stored: mounting walks every file's maps and rebuilds the table in memory. The directory is kept in memory and written through. A disk without the magic (blank, or in the first format, which had fixed 1 MiB slots) is formatted when it is mounted at boot. Without a disk, names under `disk/` cannot be opened and everything else works.

Names are paths, and both file systems hold directories as well as files: a directory is a node with no data, and what is in it is every node whose name starts with its name and a `/`. A file or directory can only be created while its parent directory exists, and a directory can only be removed while nothing is in it, so a file's parent always exists. A file removed while it is open loses its name at once and its data at the last close (`vfs_close`); on the disk it becomes an orphan entry (kind 3) in the meantime, and mounting frees any orphan a power cut left. `disk` itself is the disk's top directory and not a node. In memory a node's name is its whole path; on the disk an entry names only its last component and points at its parent, so a path is walked from the top. The kernel side is `vfs_mkdir`, `vfs_rmdir`, `vfs_dir_lookup` and `vfs_readdir(handle, index)` in `src/fs.bastio`; user programs reach them through calls 19 to 21, and the shell has `mkdir`, `rmdir` and `ls DIR`. A path can be up to 255 bytes. Each file and directory keeps permission bits and a modification time (`vfs_meta`, `vfs_set_meta`); writes, creation and truncation set the time from the kernel's clock, which counts from boot, as Linux `CLOCK_REALTIME` does here. `vfs_rename` (call 22, the shell's `mv`) renames a file or directory within one file system, never onto a name that exists or into itself; descriptors stay open on the file. On the disk it rewrites one entry, one sector, so a power cut leaves the old name or the new one; in memory a directory's contents are renamed with it. `vfs_truncate` (call 23) sets an open file's length: bytes past a file's end are always zeros and blocks wholly past it are holes, so growing only moves the end, and shrinking zeroes the tail of the last block kept and drops the rest (on the disk by copying that block, its map and the root, then switching the entry, as a write does).

A power cut cannot leave the disk file system inconsistent, and it cannot tear a write. Data is never written over in place: a write builds the blocks it changes in memory, writes them and new copies of the maps above them to free blocks, and then switches the file to the new root and size by writing its directory entry, a single sector (an entry never straddles one). The old blocks are freed only after that; removing and truncating write only the entry. The drivers return only once the drive has the sector: ATA, AHCI and NVMe flush the drive's cache after every write, and the virtio disk is write-through because the driver does not accept the cache feature. So a mount after a cut sees each write call entirely or not at all, even one that overwrites the middle of a file across a block boundary. Work that takes several calls (`O_TRUNC` and then writes) can still be cut between them. There is no journal because nothing else needs one. `test/shell_test.sh` cuts power eight times during `user/crashme.bastio`, twice each on ATA, virtio, NVMe and AHCI, and checks the files after each boot: a log must hold whole records, a snapshot file must be empty or whole, and a region rewritten in place by single 4000-byte writes must never mix two of them.

The disk driver (`src/ata.bastio`) talks to the first drive of the legacy ATA controller in PIO mode with 28-bit sector numbers, polling with interrupts off. It owns the controller's ports 0x1F0 to 0x1F7 and 0x3F6 as `PortIo` capabilities claimed in `main`, so the proof checks that it touches no other port and that no other driver touches these. Every wait for the drive is bounded, so a missing or stuck drive makes an operation fail instead of hanging the kernel.

At boot the kernel installs the files in `files/` (built into the image with `include_bytes`). A process's descriptors are closed when it ends, and a file cannot be removed while a descriptor has it open.

## User programs

A user program is ordinary Bastio built with `--target user`. `user/start.S` provides the image header and `_start`, which calls `main` and exits with its result. `user/user.ld` links it at `USER_BASE`, and the Makefile flattens it to a raw image in `build/user/*.bin` for the kernel to embed. The shell (`user/sh.bastio`) reads lines with `serial_read` (the `read` call), runs built-in commands, and starts the other programs with `sys_spawn` and `sys_wait`. Two programs are assembly on purpose: `fault.S` touches kernel memory to test fault isolation, and `straddle.S` passes buffers that cross page boundaries to test the kernel's copies.

Entering user mode is the one place where a value from the proved side controls the CPU directly. `bst_cpu_enter` therefore takes only `rip`, `rsp`, the general registers and the arithmetic flags from the saved context; the segments, IOPL 0 and the interrupt flag are forced, and non-canonical addresses are replaced. Whatever the kernel stores, user code runs in ring 3 with interrupts on.

## What is proved and what is trusted

Proved, for every input, by the Bastio compiler before any C is emitted:
- every page is owned by exactly one place at a time (the allocator, or one slot of one table), and the allocator's bitmap agrees with the pool;
- a table is empty when it is freed, so tearing down a process returns every page;
- table levels line up, frames go only into level-1 tables, slot 0 of a top-level table (the kernel's map) is never touched, and the table in CR3 is never freed;
- every buffer, string and vector index is in bounds, no arithmetic overflows, and the kernel's own `Buf`s, `Vec`s and tables are never leaked or used after being freed.

Trusted, and listed in the README's "What is trusted" table and in [trusted-base.md](trusted-base.md): `boot/`, `user/start.S` and the linker scripts, the bastiolang preludes, the compilers and Z3, and QEMU. What the proof does not cover: page contents (the kernel copies bytes, but the proof does not know what they mean), the hardware's behaviour, and termination (a kernel loop that never ends is not detected).

## Limits

These are current choices, not design limits; the README's "Status" lists them with the plan:
- one lock for the whole kernel state (the console's devices have their own); the kernel runs with interrupts off and polls the UART and the keyboard, console and PIT interrupts reach only the bootstrap processor, and an idle processor notices new work only at its next timer tick (no wake-up interrupts between processors);
- a 64 MiB user window per process, at most `NPROC - 1` processes;
- files are simple (32 in memory and 64 on disk, paths up to 255 bytes, files up to 64 MiB in memory and 1 GiB on disk, 8 descriptors per process; every write is copy-on-write, so a block written costs four block writes), and programs cannot be run from files yet;
- user pages are mapped writable and executable;
- the kernel's arena halts the machine when it runs out (the page pool does not: spawns fail cleanly).
