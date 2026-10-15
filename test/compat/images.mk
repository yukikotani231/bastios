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
RUST_IMAGE ?= library/rust@sha256:ddcfc394ea1f26b4363936ff59ee69c8eedb7b72146477c844d73b85031880ea
GO_IMAGE ?= library/golang@sha256:8bee1901f1e530bfb4a7850aa7a479d17ae3a18beb6e09064ed54cfd245b7191
NODE_IMAGE ?= library/node@sha256:0a7108bf6c7bf5de370ffb1a3ed6be93d405b43ff159f681a8d18c0e2bc2e402
# The tree-sitter C parser for Neovim (npm package, pinned by version and SHA-256).
TREESITTER_C_VER    := 0.24.1
TREESITTER_C_SHA256 := 8e9ecd7687665d2fa1b3c07fbd1f4380b7d285850d6954629e2156e1b757451e

# strace for AArch64, built from source (arch/aarch64/Makefile).
STRACE_VER := 6.11
STRACE_SHA256 := 83262583a3529f02c3501aa8b8ac772b4cbc03dc934e98bab6e4883626e283a5
