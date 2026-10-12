# BastiOS as a cloud guest

A virtual machine in a cloud is the setting where a small kernel with a proved
memory-safety story is easiest to use: the hardware is a fixed, short list of
virtual devices, there is no screen or keyboard, and the console is a serial
line. This page says what BastiOS needs from such a machine, which clouds it
fits today, and what is not done.

## What a guest needs

| Need | Driver | Status |
|---|---|---|
| Boot | Multiboot (`-kernel`), PVH (Firecracker, Cloud Hypervisor, QEMU `microvm`), or Limine from an ISO or a USB/disk image, under BIOS or UEFI | works; PVH is tested on QEMU only, not on Firecracker; the Linux boot protocol (bzImage) is not done |
| Console | COM1 (115200 baud) | works; EC2's serial console and most clouds' "serial output" are this |
| Disk | virtio-blk (PCI), NVMe, AHCI, ATA | works; virtio over MMIO (microVMs) is found at QEMU microvm's slots only, see below |
| Network | virtio-net (PCI), Intel e1000 families, **AWS ENA** | works; ENA only against a model, see below; virtio-net over MMIO as for disks |
| Power | ACPI S5 power off, reset through the FADT | works |
| Clock | HPET, else the ACPI PM timer; TSC calibrated against it | works |

NVMe is what EC2 shows an EBS volume as, virtio-blk and virtio-net are what
KVM-based clouds (Google Cloud, most OpenStack and libvirt hosts) show, and
ENA is EC2's network card. Hyper-V's synthetic devices (Azure) and
VMware's vmxnet3 have no driver.

## The AWS ENA driver

`src/ena.bastio` drives the Elastic Network Adapter (PCI `1d0f:ec20`, its LLQ
twin `ec21`, and the physical-function ids `0ec2`, `1ec2`, `0051`). It is
written from the interface definition Linux's `drivers/net/ethernet/amazon/ena`
publishes (`ena_regs_defs.h`, `ena_admin_defs.h`, `ena_eth_io_defs.h`;
GPL-2.0 OR Linux-OpenIB), reading the driver's own sequence for what order
things happen in. No code is copied.

What it does:

- Claims BAR 0 (anywhere in the 32-bit part of the device window; EC2's
  firmware places it below the 3 GiB mark) and lends the device eleven pages.
- Resets the device and waits for the reset to finish. On PCI revisions where
  registers cannot be read directly (revision bit 0 clear) every register read
  goes through the response buffer the device writes, as Linux does.
- Sets up the admin queue (32 entries) and the asynchronous event queue, reads
  the MAC address and the queue limits, asks for no asynchronous events, and
  sets the MTU to 1500 so no frame outgrows a 2 KiB receive buffer.
- Creates one transmit and one receive queue pair of 256 descriptors in host
  memory, offers eight receive buffers, and polls the completion queues by
  their phase bits. The stack computes its own checksums, so no offloads.

What it proves: every ring access stays inside the page lent for it, every
register access is in bounds and aligned (doorbell offsets, which the device
names, are checked before they are used), the pages go back to the pool after
the device is reset at power off, and, as for every driver here, nothing else
can reach a page while it is lent. What it trusts: that the device does what the
interface says.

What it does not do:

- **Low latency queues (LLQ).** Descriptors and packet headers go through host
  memory. Some instance types may accept only LLQ queues; then creating the
  queue fails and the boot log says `net: ena found but not started (reason 5,
  see docs/cloud.md)`. The reasons are `ENA_ERR_*` in `src/ena.bastio`: 1 a
  page above 4 GiB or no usable BAR, 2 the device would not reset, 3 an admin
  command failed or timed out, 4 the device cannot give 256-entry queues, 5
  creating an IO queue failed.
- Interrupts (the card is polled like the e1000 without its interrupt), jumbo
  frames, more than one queue pair, host information for the device, ENI
  statistics, and the 100 Gbit receive-side scaling.

### It has not met a real ENA

QEMU has no ENA, and no AWS account was used. The driver is tested against a
model of the device, `tools/qemu-ena/ena.c`, a QEMU device written from the same
Linux headers. The model is deliberately strict: a wrong phase bit, a command it
does not know, a doorbell for a queue that does not exist or a receive descriptor
with wrong control bits is logged (`-d guest_errors`) and the command or frame
dropped, so it catches bugs in the driver's handling of the rings and the
sequence. It cannot catch a misreading of the interface that the driver and the
model share, or any behaviour of the real device the header files do not
state. Treat the driver as untested on EC2 until someone has booted it there. If
you do, the boot log and `diag` output of a failed start are the useful thing to
send.

### Running the tests against the model

```sh
tools/qemu-ena/build.sh                     # builds QEMU 8.2 with the model: ~20 min, build/qemu-ena/
NIC=ena test/dhcp_test.sh                   # DHCP through the ENA queues
NIC=ena test/tcp_test.sh                    # TCP, with loss and reordering
NIC=ena test/ipv6_test.sh
test/shell_test.sh                          # includes three ENA boots, skipped when the build is missing
```

`QEMU_ENA` names another QEMU binary. The model has two switches:
`-device ena,readless=off` (PCI revision 1: registers read directly) and
`-device ena,queues-ext=off` (only the older maximum-queues feature).

## Other gaps in a cloud guest (issue #228)

- **virtio over MMIO** (`src/virtio_mmio.bastio`, and the `Vt` transport in
  `src/virtq.bastio` that the block and network drivers go through): what
  Firecracker and QEMU's `microvm` machine expose. Both layouts work:
  version 1 (legacy registers, the queue given by page number) and version 2
  (modern: VERSION_1 negotiated, the queue given by three addresses, a
  12-byte network header). Devices are looked for only on a machine with no
  PCI host bridge, at QEMU microvm's slots (24 blocks of 512 bytes from
  `0xFEB00000`); the interrupt line is not used, the queues are polled.
  Firecracker puts its devices elsewhere and names them on the kernel
  command line (`virtio_mmio.device=4K@0xd0000000:5`); reading that is not
  done yet. Tested in `test/shell_test.sh` on `-machine microvm` (disk, DHCP,
  ping) for both versions.
- **Firecracker itself.** The PVH entry (an ELF note naming a 32-bit entry
  point; `boot/boot.S` tags the `hvm_start_info` the loader passes and
  `boot/multiboot.c` converts its command line, memory map and RSDP into the
  Multiboot 1 form) is tested by booting the image with its Multiboot header
  blanked on QEMU's `pc` and `microvm` machines, since QEMU prefers
  Multiboot when both are present. Firecracker has not been run.
- **Boot time and memory under KVM** have not been measured: the machine
  these numbers were taken on has no `/dev/kvm`. Under TCG (QEMU 8, one CPU,
  `-kernel`) the 2.9 MB image reaches the `bastios>` prompt in 0.2 to 0.4 s
  including QEMU's start-up. The smallest memory that boots is 36 MiB
  (`-m 36M`: 1863 pages of 4 KiB free at the prompt); `-m 33M` and `-m 34M`
  print `bastio: out of memory` (the kernel's own heap does not fit), and
  `-m 24M` and below fail the same way. **`-m 16M` prints nothing** because
  the image is linked at 16 MiB (`boot/linker.ld`), which is the end of 16 MiB
  of RAM: the loader has nowhere to put it and QEMU does not complain.
