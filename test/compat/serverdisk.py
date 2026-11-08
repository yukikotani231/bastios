#!/usr/bin/env python3
"""Writes the server disk for the compat suite (test/compat/server.txt):
Redis, memcached, nginx, HAProxy and dnsmasq of Ubuntu 24.04, with the glibc and the
libraries they link against, as a tar file (/disk/root.tar) next to a
static BusyBox at /disk/busybox. The checks unpack the root under /disk/s,
chroot into it and run the servers and their clients.

    serverdisk.py OUT.img --cache DIR

Every package is a .deb of Ubuntu's release pocket, pinned below by its
SHA-256 and kept in DIR (checked each time it is used), so the archive is
asked only for what the cache lacks. The Ubuntu snapshot service is the
fallback. The packages are unpacked with dpkg-deb.
"""
import hashlib, os, shutil, subprocess, sys, tarfile, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
MIRRORS = ("https://archive.ubuntu.com/ubuntu/", "https://snapshot.ubuntu.com/ubuntu/20240425T000000Z/")
PACKAGES = (
    ("pool/main/b/busybox/busybox-static_1.36.1-6ubuntu3_amd64.deb",
     "9936a597976a42088f977bfb204b84f02615feb42bb732ff1fdb8bef5b5c13f4"),
    ("pool/main/g/glibc/libc6_2.39-0ubuntu8_amd64.deb",
     "af36c7ac770770fe3d3c10e85d6bc538e76e57570ba7db7d397fb9f654783ef3"),
    ("pool/main/g/gcc-14/libgcc-s1_14-20240412-0ubuntu1_amd64.deb",
     "a39efdcaa2245f026dc3254ce14fcff255fc430a17064632b6ba7c5da974a912"),
    ("pool/main/g/gcc-14/gcc-14-base_14-20240412-0ubuntu1_amd64.deb",
     "bc5a682c694a1ce5af50772b7c737a21e9a74f0f0edaae98e97bc9c4af583fef"),
    ("pool/main/libz/libzstd/libzstd1_1.5.5+dfsg2-2build1_amd64.deb",
     "4dcc3ba9606a5837a55248d08162a5b426dd621f75c0eb24d86409e0d0e71d52"),
    ("pool/main/o/openssl/libssl3t64_3.0.13-0ubuntu3_amd64.deb",
     "6cde53feabd28f26586a4db2ffb9fc84e7b0598aee15b33bee907b6392a92d24"),
    ("pool/main/x/xz-utils/liblzma5_5.6.1+really5.4.5-1_amd64.deb",
     "28851b3997e4ba74bb3197bd58796bd3041c204c21df85fe0ddafdcaced647f0"),
    ("pool/main/libc/libcap2/libcap2_2.66-5ubuntu2_amd64.deb",
     "a6cce0b5f149707a6359e4f03a05fb7417c5e1d3fde9e12c651e804b4c479bbb"),
    ("pool/main/libg/libgcrypt20/libgcrypt20_1.10.3-2build1_amd64.deb",
     "ff32bae487706ba6ca646b5b72d89b935e3b8b2811d02e4ad7360cb97d90e2d0"),
    ("pool/main/libg/libgpg-error/libgpg-error0_1.47-3build2_amd64.deb",
     "81a30b5888e0af43b72bfdbdb44209b82e521eadf8848ff201542e6d258f6a1f"),
    ("pool/main/l/lz4/liblz4-1_1.9.4-1build1_amd64.deb",
     "b9b69dedb126755f063396a6d3a3361066f9661f1c21cf5635cdd5ff6605796a"),
    ("pool/main/z/zlib/zlib1g_1.3.dfsg-3.1ubuntu2_amd64.deb",
     "0b93d16d7498f092fa3070fbbad28cdbc6b3d640f1a7681b96fc37f20d1219f1"),
    ("pool/universe/r/redis/redis-server_7.0.15-1build2_amd64.deb",
     "09a7f28a2f81efcc577f81b1bf546af9bd0e5bf2c2ccdc037f66f26369ce43a4"),
    ("pool/universe/r/redis/redis-tools_7.0.15-1build2_amd64.deb",
     "c02e7a8a11d0b7611bfce1bcb93bc44540769122ebd08fa6dc5493440281f7ee"),
    ("pool/universe/j/jemalloc/libjemalloc2_5.3.0-2build1_amd64.deb",
     "0cccf7701765b5a633b964ed551c2bd1c074cad17daea8fa20a4e9cad0adb5a8"),
    ("pool/universe/libl/liblzf/liblzf1_3.6-4_amd64.deb",
     "253182d4718271565d6c24125f27dd93da56fce34b34d8226615cb3553bbe73f"),
    ("pool/main/g/gcc-14/libatomic1_14-20240412-0ubuntu1_amd64.deb",
     "f496d80626e1512456273965d44f47b281edf53fa26d0d8bd0defc5be7e4601c"),
    ("pool/main/g/gcc-14/libstdc++6_14-20240412-0ubuntu1_amd64.deb",
     "4863e880d6ee538e4734c430c5579c1ac933b88a42c25bc9980d0f148c54b21e"),
    ("pool/main/s/systemd/libsystemd0_255.4-1ubuntu8_amd64.deb",
     "64ba97fddff491b86597e93b078e2fe3256cb80f9a113ae9d8dd1361f01b84ed"),
    ("pool/main/m/memcached/memcached_1.6.24-1build3_amd64.deb",
     "f758daaeb17ff28f1eeb42f6f11c9053c5abe85ba40dc5a66904d5ff0943cd46"),
    ("pool/main/libe/libevent/libevent-2.1-7t64_2.1.12-stable-9ubuntu2_amd64.deb",
     "f95eddfc9e9028567a21b8162a1cb8aae96b2fb168f149c0a5b01d72bfd42471"),
    ("pool/main/c/cyrus-sasl2/libsasl2-2_2.1.28+dfsg1-5ubuntu3_amd64.deb",
     "9a5b0cf88b19de419e585b91b3837c4d7621df229f78a0a58935761866ca1f4d"),
    ("pool/main/libx/libxcrypt/libcrypt1_4.4.36-4build1_amd64.deb",
     "9474785cd6f398512bf8c305c3901dbb111569dccb6f5832002373c0a8ac5832"),
    ("pool/main/p/pcre2/libpcre2-8-0_10.42-4ubuntu2_amd64.deb",
     "bb38c38adb4ef548f2851b5cf7f70bbdbcb27d582c17550aa053edb9e4098d84"),
    ("pool/main/n/nginx/nginx_1.24.0-2ubuntu7_amd64.deb",
     "de56297d8b47be0324cf699be438cb58902a26ed1307d129fc4f32aa5e4a5994"),
    ("pool/main/l/lua5.4/liblua5.4-0_5.4.6-3build2_amd64.deb",
     "6a520259e5b06e86191a25af6d448387cdd2b3d291a0aa956ce272e84d071260"),
    ("pool/main/h/haproxy/haproxy_2.8.5-1ubuntu3_amd64.deb",
     "861bf08a4f55a87663cb56597991ad995e2e4b1a1fff2ce5cd31fd6bf559fbb5"),
    ("pool/main/d/dnsmasq/dnsmasq-base_2.90-2build2_amd64.deb",
     "8d3629fbfddff8eab66d9d912a63876f3927138de6b24622398f9c2a126f51dd"),
    ("pool/main/n/nftables/libnftables1_1.0.9-1build1_amd64.deb",
     "f74276e2b0a6ab97ac1187d38f5b22a5e742e9d17bf67c1d66dacc4ede6ff923"),
    ("pool/main/i/iptables/libxtables12_1.8.10-3ubuntu2_amd64.deb",
     "c94063499d1b0b336f39607107efb7de95d52a5d4e7e6a9368a5f7308f05b714"),
    ("pool/main/libn/libnftnl/libnftnl11_1.2.6-2build1_amd64.deb",
     "e5bde5c9cbb95082700011f0b7ef2c0a41900b0a9fd7434f6e90448e2a7b4e50"),
    ("pool/main/libm/libmnl/libmnl0_1.0.5-2build1_amd64.deb",
     "5170658bf0eaf54a8c7d53061ca6b47a6cf33a83543a75fa521de7046cb7197a"),
    ("pool/main/j/jansson/libjansson4_2.14-2build2_amd64.deb",
     "0cf79113f5d193ce9af2be2ff4b2c3b30dd4e55a0b6c47f7d28f6c849ff3aa60"),
    ("pool/main/g/gmp/libgmp10_6.3.0+dfsg-2ubuntu6_amd64.deb",
     "649a5ee5f74f0aa1e1b9c4f169eabbae56a79aa5aac51ef733eb9e18700bd8fc"),
    ("pool/main/n/nettle/libnettle8t64_3.9.1-2.2build1_amd64.deb",
     "4cac2064e2984c4bc5a31659d4f00c7efe0c4b691559d536dd726c05d3632a57"),
    ("pool/main/libn/libnetfilter-conntrack/libnetfilter-conntrack3_1.0.9-6build1_amd64.deb",
     "6d32a4c4618eb7d6c0d989591d28637a9acdfa86dfe22e42e9ac9cc25d882a97"),
    ("pool/main/libn/libnfnetlink/libnfnetlink0_1.0.2-2build1_amd64.deb",
     "f557a287af2ca00df5dbf632c6b77d77479139ffb51c83e3de9766eb609b5d63"),
    ("pool/main/libi/libidn2/libidn2-0_2.3.7-2build1_amd64.deb",
     "f1be32391a2fabf06c27618a2000ae8f9a26c30fad1b482a007049ae8156d19c"),
    ("pool/main/libu/libunistring/libunistring5_1.1-2build1_amd64.deb",
     "78acc4c141a499e0c1e4cc47ca8e9082996f3972662c4d92337669ecc9b0a519"),
    ("pool/main/n/nettle/libhogweed6t64_3.9.1-2.2build1_amd64.deb",
     "594a82d7b70ecff393753ee0ab6611b0b18226c4f922d27ccb3f66caeae3d5d1"),
    ("pool/main/d/dbus/libdbus-1-3_1.14.10-4ubuntu4_amd64.deb",
     "bbb55111182a44d0fe97a84120407e17bb6e23d41e2670d73aa78f619e29efa2"),
)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def fetch(path, digest, cache):
    dest = os.path.join(cache, digest + ".deb")
    if os.path.exists(dest) and sha256(dest) == digest:
        return dest
    for mirror in MIRRORS:
        try:
            with urllib.request.urlopen(mirror + path, timeout=120) as r, open(dest + ".part", "wb") as f:
                shutil.copyfileobj(r, f)
        except OSError as e:
            print(f"alsadisk: {mirror}{path}: {e}", file=sys.stderr)
            continue
        if sha256(dest + ".part") == digest:
            os.rename(dest + ".part", dest)
            return dest
        print(f"alsadisk: {mirror}{path}: wrong checksum", file=sys.stderr)
    sys.exit(f"alsadisk: cannot get {path}")


def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    out, = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "server")
    cache = cache or work
    os.makedirs(cache, exist_ok=True)
    shutil.rmtree(work + "/root", ignore_errors=True)
    shutil.rmtree(work + "/bb", ignore_errors=True)
    os.makedirs(work + "/root")
    os.makedirs(work + "/bb")
    for path, digest in PACKAGES:
        deb = fetch(path, digest, cache)
        subprocess.check_call(["dpkg-deb", "-x", deb, work + ("/bb" if "busybox" in path else "/root")])
    # Ubuntu merges /usr: /lib and /lib64 are links into it, which the debs do not carry.
    for link, to in (("lib", "usr/lib"), ("lib64", "usr/lib64"), ("bin", "usr/bin"), ("sbin", "usr/sbin")):
        if not os.path.lexists(f"{work}/root/{link}"):
            os.symlink(to, f"{work}/root/{link}")
    for d in ("dev", "proc", "tmp", "etc", "var/log/nginx", "var/lib/nginx", "run"):
        os.makedirs(f"{work}/root/{d}", exist_ok=True)
    root = work + "/root.tar"
    with tarfile.open(root, "w", format=tarfile.PAX_FORMAT) as t:
        t.add(work + "/root", arcname=".")
    env = dict(os.environ, MKDISK_FREE_MB="100")   # room for the unpacked root
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={work}/bb/usr/bin/busybox", f"root.tar={root}"], env)


if __name__ == "__main__":
    main()
