# The Docker Hub images the Alpine, Debian and Node disks of the compat suite
# are made from, pinned by digest (the digests name multi-platform images:
# alpinedisk.py takes the layers for the guest's processor). Included by the
# Makefile and by arch/aarch64/Makefile.
ALPINE_BASE ?= library/alpine@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6
ALPINE_PKGS ?= alpine/git@sha256:a4bb51f1a3553df194ce679fc1db721d8bfba2046fa1a88fe9d4ac551ffbce25 \
               alpine/curl@sha256:e17bf3bdfef2efda277af9b73149ece806d30354b3eebd002555f66a8cabb357 \
               alpinelinux/alpine-gitlab-ci@sha256:4bce54b0b246967f6ab51476d5511245ba35a49f30b2e63102ca294ab488add9 \
               alpine/ansible@sha256:72ed4cb27b533dc2bc608a657b9ed630c2ee53a86ba9111d0b710c5424de0fe2 \
               alpine/xml@sha256:836f531a7e38bbdaab8f1c1ce76bf868412f253671625ebf54cf14fa28544de6 \
               heywoodlh/vim@sha256:c8c4f0de47f20de92af6d3b0544898e857f6c2eecd7ab397a1e7daf0b5fc6292 \
               serh/nvim@sha256:e62acc0ba679451c43a0197088af546bd8ab0e23a1ce27c010748a347aa3a837 \
               odovzhan/tmux@sha256:b439b1fc9a4391884ef4c1266980639a10ac629f684f4551bfbe3a77ca005cd6 \
               lspcontainers/lua-language-server@sha256:5edb5674fb555bec3df35e77a45cd2a98f829fd3602ea3fa68394c80f186f072
# One more image, for the x86-64 Alpine disk only (the devtools cases of
# alpine.txt): cmake and ninja. The image has no arm64 build.
ALPINE_X86_PKGS ?= dxloc/alpine-cmake@sha256:fe609ae835a3b195c302adb966464b378d57b2882c0d915008f8781634dab485
ALPINE_DISK_PKGS ?= git,curl,less,openssh-keygen,bash,make,jq,rsync,openssl,file,libxml2-utils
DEBIAN_IMAGE ?= library/debian@sha256:eb593cf2c358cacef45ca0a424bbc7d30cfa3466265fc2662b9466a0ca6ba1c5
JDK_IMAGE ?= library/eclipse-temurin@sha256:9ececb44042a7cc91e4e6bca525c678e811c1602330a80b5f2d2fcfa0aa97c4e
RUST_IMAGE ?= library/rust@sha256:ddcfc394ea1f26b4363936ff59ee69c8eedb7b72146477c844d73b85031880ea
GO_IMAGE ?= library/golang@sha256:8bee1901f1e530bfb4a7850aa7a479d17ae3a18beb6e09064ed54cfd245b7191
PYTHON_IMAGE := library/python@sha256:f6a589d43c42b9e7f7dc67a12d37132491f362859a5d750607710cc56da3bc72
PODMAN_IMAGE ?= mgoltzsche/podman@sha256:86e6ecde27e86140a20fa16dcc03cc1e67150536bd8335f6effbba067e352b48
DOCKER_IMAGE ?= library/docker@sha256:9e79b879b177fbd72c34376a3dd3d91747da5db49e1f560f76aea59561471977
NODE_IMAGE ?= library/node@sha256:0a7108bf6c7bf5de370ffb1a3ed6be93d405b43ff159f681a8d18c0e2bc2e402
# gopls, the Go language server, built from this release (Makefile: build/compat/gopls).
GOPLS_VER := v0.20.0
# rust-analyzer, Rust's language server, from the Rust release the rust image holds
# (the musl build of the rust-analyzer component, pinned by the SHA-256 of its
# tar.gz in that release's manifest, https://static.rust-lang.org/dist/channel-rust-1.99.0.toml).
RUST_ANALYZER_URL    := https://static.rust-lang.org/dist/2026-10-01/rust-analyzer-1.99.0-x86_64-unknown-linux-musl.tar.gz
RUST_ANALYZER_SHA256 := 9c135328ac82c90b6e2b801f679f75cf6e9fa4ff77a4486106b839e529e67f51
# The standard library's source (rust-src), which rust-analyzer reads, from the same release.
RUST_SRC_URL    := https://static.rust-lang.org/dist/2026-10-01/rust-src-1.99.0.tar.gz
RUST_SRC_SHA256 := 82b978093b33c71bcbe69005b92181bfb76d3d0e434ef303e637812e21badcd7
# The tree-sitter C parser for Neovim (npm package, pinned by version and SHA-256).
TREESITTER_C_VER    := 0.24.1
TREESITTER_C_SHA256 := 8e9ecd7687665d2fa1b3c07fbd1f4380b7d285850d6954629e2156e1b757451e

# strace for AArch64, built from source (arch/aarch64/Makefile).
STRACE_VER := 6.11
STRACE_SHA256 := 83262583a3529f02c3501aa8b8ac772b4cbc03dc934e98bab6e4883626e283a5
# Eclipse JDT LS (jdtls), the Java language server: a milestone, the last one
# that runs on the JDK 21 of the Java disk (1.62 wants Java 25).
JDTLS_URL    := https://download.eclipse.org/jdtls/milestones/1.61.0/jdt-language-server-1.61.0-202609031315.tar.gz
JDTLS_SHA256 := 338e7e73d61836651ba2453919a0d34fa763eb4e7c03342092309bffb8934c64
# TypeScript 7's compiler, a static Go program that also speaks LSP
# (`tsc --lsp --stdio`), with the standard library's declaration files: the
# linux-x64 package from npm, pinned by the SHA-256 of its tarball.
TSC_VER    := 7.0.2
TSC_SHA256 := 7ecad6f67377e831856367ab062ef394f21506a611405bf8ac0ff039348637d3
