# Releasing

How a version of BastiOS is cut. Nothing is released yet; this is the
procedure the release workflow is written for.

## Versions

BastiOS uses [semantic versioning](https://semver.org/). Before 1.0, a minor
version may change the system call ABI, the disk format or the build, and a
patch version only fixes bugs. The kernel prints its major and minor version
at boot (`BastiOS 0.2` in `src/main.bastio`, and in `user/sh.bastio`); the
release workflow refuses a tag whose version the banner does not show.

Each release is built with one version of bastiolang, named in the release
notes. Once bastiolang has releases, set `BASTIOLANG_REF` in
`.github/workflows/release.yml` to the matching bastiolang tag.

## Before tagging

- The README's "Status" section is true: the release notes copy it, so it is
  what readers learn about the known limits of this version (the single
  kernel lock, table sizes, what has only run in QEMU). Update it if the
  release changes them.
- `README.md`'s "Tested with" table and `docs/real-hardware.md` say what has
  and has not been tried on real hardware.
- `tests/trusted_base.txt` matches `bastioc0 --trusted-base`, and
  `docs/trusted-base.md` says what grew.
- `CHANGELOG.md` names the changes that break the system call ABI or the
  disk format, since a 0.x minor may do either.
- The bastiolang commit or tag the release is built with is the one named in
  the release notes (`BASTIOLANG_REF`).

## Cutting a release

1. On `main`, move the entries under `## [Unreleased]` in `CHANGELOG.md`
   into a new `## [X.Y.Z] - YYYY-MM-DD` section, update the comparison links
   at the bottom, and update the banner if the minor version changed.
2. Run `make test`, open a pull request, and merge it once CI is green.
3. Tag the merge commit and push the tag:
   ```sh
   git tag -a vX.Y.Z -m "BastiOS X.Y.Z"
   git push origin vX.Y.Z
   ```
4. `.github/workflows/release.yml` runs: it checks that the tag, the banner
   and the changelog agree, builds bastiolang's stage0, runs `make test`,
   builds the ISO and the AArch64 kernel (boot-testing it in QEMU as part of
   `make test`), records a build provenance attestation for the files, and
   creates a **draft** GitHub release.
5. Read the draft, then publish it by hand.

To try the build without making a release, start the workflow from the
Actions tab with a tag that already exists: it uploads the files as a
workflow artifact and stops there.

## What a release contains

- `bastios-vX.Y.Z.iso`: Limine and the kernel, a hybrid image that boots from
  CD or USB under BIOS or UEFI (`make iso`). Limine is BSD-2-Clause; its
  license is copied into the image as `boot/limine/LICENSE`.
- `bastios-usb-vX.Y.Z.img`: the same files for a USB stick (`make usb`), with
  a 64 MiB partition after them where BastiOS keeps its files.
- `bastios-vX.Y.Z.elf`: the kernel alone, a Multiboot image for
  `qemu-system-x86_64 -kernel`.
- `bastios-aarch64-vX.Y.Z.bin` and `.elf`: the AArch64 kernel for QEMU's
  `virt` machine (`make ARCH=aarch64`; a Linux arm64 `Image`, so
  `qemu-system-aarch64 -M virt -cpu max -kernel` boots it; see
  [aarch64.md](aarch64.md)).
- `busybox-1.36.1-source.tar.gz` and `busybox-vX.Y.Z.config`: the kernel
  image embeds a BusyBox binary, which is GPL-2.0-only, so its complete
  source and the configuration it was built with ship alongside it.
- `bench-vX.Y.Z.json`: the result file of `make bench` for BastiOS on the
  release runner (QEMU TCG, no KVM), whose table and change from
  `bench/baseline.json` are also at the end of the release notes. Read them
  against the baseline, not as absolute figures; update the baseline when a
  release's numbers are meant to become the new reference.
- `SHA256SUMS`.

## Checking a download

```sh
sha256sum -c --ignore-missing SHA256SUMS
gh attestation verify bastios-vX.Y.Z.iso --repo yukikotani231/bastios
```

The attestation says which workflow run, on which commit, built the file.

## Rebuilding a release

What decides the bytes of a release:

- **BastiOS**: the tag.
- **bastiolang**: the commit named in the release notes ("Built with
  bastiolang ..."), and the Rust release pinned in its
  `rust-toolchain.toml`. Build `bastioc0` from it with `cargo build --release
  --locked`.
- **The C compiler**: the kernel is Bastio translated to C, so its bytes
  follow the C compiler and its version. The release runner is Ubuntu 24.04
  (`cc`, and `aarch64-linux-gnu-gcc` for the AArch64 kernel). Compare a rebuild
  with a release only on the same Ubuntu release.
- **Fetched sources** are pinned: BusyBox by tag (`BUSYBOX_TAG`) and Limine
  by tag (`LIMINE_TAG`) in the `Makefile`.
- **Time**: BusyBox records its build time in its version string unless
  `KCONFIG_NOTIMESTAMP` is set, which the `Makefile` does. The images use the
  time of the tagged commit when `SOURCE_DATE_EPOCH` is set: file dates in the
  ISO, and the identifiers in its partition tables (the MBR signature, and
  for the USB image the GPT disk and partition GUIDs, derived from it).

With those fixed, `make build/bastios.elf` and `make ARCH=aarch64` give
byte-identical kernels from two different checkout directories, and with
`SOURCE_DATE_EPOCH` set to the same value `make iso usb` gives byte-identical
`bastios.iso` and `bastios-usb.img` (tried on an x86-64 host with Ubuntu
24.04's compilers; the ISO also boots under BIOS and UEFI in `test/iso_test.sh`).
Without `SOURCE_DATE_EPOCH` the images carry the build time and random
identifiers. To rebuild a release's images, set it to the commit's time (the
release workflow does):

```sh
SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) make iso usb
```
