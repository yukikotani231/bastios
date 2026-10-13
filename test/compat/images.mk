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
               serh/nvim@sha256:e62acc0ba679451c43a0197088af546bd8ab0e23a1ce27c010748a347aa3a837
ALPINE_DISK_PKGS ?= git,curl,less,openssh-keygen,bash,make,jq,rsync,openssl,file,libxml2-utils
DEBIAN_IMAGE ?= library/debian@sha256:eb593cf2c358cacef45ca0a424bbc7d30cfa3466265fc2662b9466a0ca6ba1c5
NODE_IMAGE ?= library/node@sha256:0a7108bf6c7bf5de370ffb1a3ed6be93d405b43ff159f681a8d18c0e2bc2e402
