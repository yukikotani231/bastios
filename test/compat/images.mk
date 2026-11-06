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
# Xvfb (slieb/xvfb-run, Alpine 3.5), xterm (jbonds/xterm, Alpine 3.6) and PuTTY, a GTK 3
# program (jlesage/putty, Alpine 3.20) MediaInfo, a Qt 5 program (jlesage/mediainfo), and KeePassXC, a larger
# Qt 5 one (jlesage/keepassxc) and the installer of JDownloader, a Java 8 Swing
# program (jlesage/jdownloader-2, Alpine's OpenJDK) and Audacity, a wxWidgets (GTK 3)
# program (jlesage/audacity) and dupeGuru, a Python program with PyQt 5
# (jlesage/dupeguru) and puddletag, a tag editor in Python with PyQt 5
# (jlesage/puddletag) and QDirStat, a Qt 5 disk usage viewer
# (jlesage/qdirstat) and FileZilla, a GTK 3 FTP client (jlesage/filezilla) and Avidemux, a Qt 5 video editor
# (jlesage/avidemux) and HandBrake, a GTK 3 video transcoder
# (jlesage/handbrake) and MKVToolNix, a Qt 5 Matroska tool
# (jlesage/mkvtoolnix) and Czkawka, a GTK 4 duplicate finder in Rust
# (jlesage/czkawka) and tsMuxeR, a Qt 5 transport stream muxer
# (jlesage/tsmuxer) and FileBot, a Java media renamer
# (jlesage/filebot) and FreeFileSync, a GTK folder synchroniser
# (jlesage/freefilesync), for x11.txt.
X11_SERVER_IMAGE := slieb/xvfb-run@sha256:295c246aa1da131a977d23eb20d6bfe58fed6728edc3a8c0cee13c6570547982
X11_CLIENT_IMAGE := jbonds/xterm@sha256:bac17f91159dffccecd9d3c73bea2c16c6b2de0f98cb81803e157dabb401c2ec
X11_GTK_IMAGE := jlesage/putty@sha256:ecd5868c45a512fef25a6219f6559a3966136ca3f1652998e30584a91f901034
X11_QT_IMAGE := jlesage/mediainfo@sha256:b9d0388107f44cbe9c3d36f1d43371cb7bfb261ff8b3ffa2ba3d1f3b6f3ae9bf
X11_QT2_IMAGE := jlesage/keepassxc@sha256:f48315f5c8452a6cdc4838262111a9639d8d9b09480a33263e00dfd2e71f9241
X11_JAVA_IMAGE := jlesage/jdownloader-2@sha256:b5e40d7e313f43db2a02124949703c45c4e28d45db601236010f3db71ac2d39f
X11_WX_IMAGE := jlesage/audacity@sha256:5ff22fadf705ddc828ae480671f61056288394ad7629eead4a628e16a29196a9
X11_PY_IMAGE := jlesage/dupeguru@sha256:b88814675c5bd5d7202c8bbef2d17f783388d0c1990c41a4395275436804b4d3
X11_TAG_IMAGE := jlesage/puddletag@sha256:19fc365f82a62da588c682ee724bc6f3f2c98aedc98a8bc2a710e9b56872bf37
X11_DIRSTAT_IMAGE := jlesage/qdirstat@sha256:e7daf42e334e9bb29d516a468b2280055f4657cdd74d9dc7410cd941397e5c3a
X11_FILEZILLA_IMAGE := jlesage/filezilla@sha256:123fc4d7aac55b482b9936ece2c8208e56530875e75624c53cd674739ecba121
X11_AVIDEMUX_IMAGE := jlesage/avidemux@sha256:1031e94ad4b27f9dc9e50a4c5c48608574248a2045b772ecb5417ac5d70464f0
X11_HANDBRAKE_IMAGE := jlesage/handbrake@sha256:6f3a121e235d34e7299080e8cbac5284060e942fb2b137f777e2521fb7af65ae
X11_MKV_IMAGE := jlesage/mkvtoolnix@sha256:768768cd651071d4a64a8e51583268aaac57f24a0483ae04022b3cc23d356036
X11_CZKAWKA_IMAGE := jlesage/czkawka@sha256:228d47eb7edac003bb54082eaa96cb41613b08f9fbee310c775714c90dd79628
X11_TSMUXER_IMAGE := jlesage/tsmuxer@sha256:44c691ee32244ac8b68da5185efa034dbbd68122c0177c0d7efd6e968f83fb56
X11_FILEBOT_IMAGE := jlesage/filebot@sha256:dd542a03218fe84b99876eae47ed1bffc1deab756a599b9f0fd87fabbeb76b0b
X11_FFS_IMAGE := jlesage/freefilesync@sha256:64df12be899a29de4d4eba46e2648857a8fc1e5878fbce9eaae8696d004d0330
X11_GIMP_IMAGE := jess/gimp@sha256:0bd926306454cdf9aaa40ae5494b196f6b8b14ddb4046cb88cd77ff70be8cf84
X11_LIBREOFFICE_IMAGE := jess/libreoffice@sha256:e652411be692cb9576aa01437dfec2c518d8730ee4de0326d2e5e3717ae5eef6
X11_INKSCAPE_IMAGE := jess/inkscape@sha256:9b5cf1bc5d90376844ab94e7c379689ce0803294d07fe0509840e52eaaf3326a
X11_WIRESHARK_IMAGE := jess/wireshark@sha256:d6feef972a189fd5498ed0aab4d529e218b6ffd3a44adf7b2de2cbcb385379c1
X11_VLC_IMAGE := jess/vlc@sha256:cc3895c0199313bbd6bb2ba492fdf5fc33e30214cca1d6dadba0e23f388d7dfa
X11_EVINCE_IMAGE := jess/evince@sha256:27d54eeef7640850d87a35c6cb413591bacae978c63521b28a6c29fe7e1b1b4b
X11_KRUSADER_IMAGE := jlesage/krusader@sha256:ecb77bb73b1d3c66720eefb07805f70289349b449d1ddaa15d120c399bd89cc4
RUBY_IMAGE ?= library/ruby@sha256:730b265fc537e93f4ad335031ba95f8a9df88dc75bd9462968b92f368a115f6d
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
