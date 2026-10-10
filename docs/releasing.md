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
   builds the ISO, and creates a **draft** GitHub release.
5. Read the draft, then publish it by hand.

To try the build without making a release, start the workflow from the
Actions tab with a tag that already exists: it uploads the files as a
workflow artifact and stops there. While bastiolang is private, the
workflow needs the same `BASTIOLANG_TOKEN` secret as CI.

## What a release contains

- `bastios-vX.Y.Z.iso`: GRUB and the kernel, a hybrid image that boots from
  CD or USB under BIOS or UEFI (`make iso`).
- `bastios-vX.Y.Z.elf`: the kernel alone, a Multiboot image for
  `qemu-system-x86_64 -kernel`.
- `busybox-1.36.1-source.tar.gz` and `busybox-vX.Y.Z.config`: the kernel
  image embeds a BusyBox binary, which is GPL-2.0-only, so its complete
  source and the configuration it was built with ship alongside it.
- `SHA256SUMS`.
