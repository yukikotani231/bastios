#!/usr/bin/env python3
"""Writes the ALSA disk for the compat suite (test/compat/alsa.txt): the
sound utilities of Ubuntu 24.04 (alsa-utils 1.2.9, alsa-lib 1.2.11), with
the glibc and the libraries they link against, as a tar file
(/disk/root.tar) next to a static BusyBox at /disk/busybox. The checks
unpack the root under /disk/s, chroot into it and play sound on the HD
Audio card the lane's guest has.

    alsadisk.py OUT.img --cache DIR

Every package is a .deb of Ubuntu's release pocket, which the archive keeps,
pinned below by its SHA-256 and kept in DIR (checked each time it is used),
so the archive is asked only for what the cache lacks. The Ubuntu snapshot
service is the fallback. The packages are unpacked with dpkg-deb.
"""
import glob, hashlib, os, shutil, subprocess, sys, tarfile, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
MIRRORS = ("https://archive.ubuntu.com/ubuntu/", "https://snapshot.ubuntu.com/ubuntu/20240425T000000Z/")
PACKAGES = (
    ("pool/main/b/busybox/busybox-static_1.36.1-6ubuntu3_amd64.deb",
     "9936a597976a42088f977bfb204b84f02615feb42bb732ff1fdb8bef5b5c13f4"),
    ("pool/main/a/alsa-utils/alsa-utils_1.2.9-1ubuntu5_amd64.deb",
     "964331366e162595a595be0d4789e30fac939688006e0f0f569b53b0f6568716"),
    ("pool/main/n/ncurses/libtinfo6_6.4+20240113-1ubuntu2_amd64.deb",
     "53e1e1753729d04cf65b05e6e58abe06e2bb76cc07eff0e1b2a638a638ca209b"),
    ("pool/main/g/glibc/libc6_2.39-0ubuntu8_amd64.deb",
     "af36c7ac770770fe3d3c10e85d6bc538e76e57570ba7db7d397fb9f654783ef3"),
    ("pool/main/g/gcc-14/libgcc-s1_14-20240412-0ubuntu1_amd64.deb",
     "a39efdcaa2245f026dc3254ce14fcff255fc430a17064632b6ba7c5da974a912"),
    ("pool/main/g/gcc-14/gcc-14-base_14-20240412-0ubuntu1_amd64.deb",
     "bc5a682c694a1ce5af50772b7c737a21e9a74f0f0edaae98e97bc9c4af583fef"),
    ("pool/main/libs/libsamplerate/libsamplerate0_0.2.2-4build1_amd64.deb",
     "8a389d1cae81860c9fb7cd9641c3f9d38815e80c0379b3181b4317489126f738"),
    ("pool/main/n/ncurses/libncursesw6_6.4+20240113-1ubuntu2_amd64.deb",
     "96d1e64bb1a80f2005f41c4112d10e4756a222a6c245e9383fc8da924bd40bf9"),
    ("pool/main/f/fftw3/libfftw3-single3_3.3.10-1ubuntu3_amd64.deb",
     "07d67e30d98e41e00ce9edcfd885e0ba7f4fe99fcf390f84726a69144b17cf2b"),
    ("pool/main/g/gcc-14/libgomp1_14-20240412-0ubuntu1_amd64.deb",
     "cc2ca1193d2c11d7383bdb03dd333cc621d128237b82dafeb8d7f2bd8302f3dd"),
    ("pool/main/a/alsa-lib/libatopology2t64_1.2.11-1build2_amd64.deb",
     "bc11e5adaafafffd4b35a925678b2cd46286e47fb5e51c641d854f91c5b1ccc6"),
    ("pool/main/a/alsa-lib/libasound2t64_1.2.11-1build2_amd64.deb",
     "c2f0caa30869876791ba349bef4907d5cfec47b8cfd1ae9889a05dce0feb7c39"),
    ("pool/main/a/alsa-lib/libasound2-data_1.2.11-1build2_all.deb",
     "a9e6326591ad5867f0783367f4708f145627ec95edfd8124d70d78b7516db4f1"),
    ("pool/main/k/kmod/kmod_31+20240202-2ubuntu7_amd64.deb",
     "91451843605387ed51c53dcd86816027399a03fc7173d85cab51a0f035877a63"),
    ("pool/main/k/kmod/libkmod2_31+20240202-2ubuntu7_amd64.deb",
     "43858c80a67c3a8a5a10d8e6769e7d92614d9eb19cb14e159b4311a16b9cf54b"),
    ("pool/main/libz/libzstd/libzstd1_1.5.5+dfsg2-2build1_amd64.deb",
     "4dcc3ba9606a5837a55248d08162a5b426dd621f75c0eb24d86409e0d0e71d52"),
    ("pool/main/o/openssl/libssl3t64_3.0.13-0ubuntu3_amd64.deb",
     "6cde53feabd28f26586a4db2ffb9fc84e7b0598aee15b33bee907b6392a92d24"),
    ("pool/main/x/xz-utils/liblzma5_5.6.1+really5.4.5-1_amd64.deb",
     "28851b3997e4ba74bb3197bd58796bd3041c204c21df85fe0ddafdcaced647f0"),
    # SDL 2.30 and the libraries it links against (its ALSA backend plays)
    ("pool/main/a/apparmor/libapparmor1_4.0.0-beta3-0ubuntu3_amd64.deb",
     "2adee8d676a430ca2435461b5d536140f70dbf4268d8bb7bd21db15d4deb51b2"),
    ("pool/main/liba/libasyncns/libasyncns0_0.8-6build4_amd64.deb",
     "f8483917fd216e248143eba1c80dfc9384b94fb4dac402194a98c11505674269"),
    ("pool/main/libb/libbsd/libbsd0_0.12.1-1build1_amd64.deb",
     "8e97b7d541416512a81e7845c5f21910344c9c5c610893cf265d3298e0caad6b"),
    ("pool/main/libc/libcap2/libcap2_2.66-5ubuntu2_amd64.deb",
     "a6cce0b5f149707a6359e4f03a05fb7417c5e1d3fde9e12c651e804b4c479bbb"),
    ("pool/main/d/dbus/libdbus-1-3_1.14.10-4ubuntu4_amd64.deb",
     "bbb55111182a44d0fe97a84120407e17bb6e23d41e2670d73aa78f619e29efa2"),
    ("pool/main/libd/libdecor-0/libdecor-0-0_0.2.2-1build2_amd64.deb",
     "64bf085d16e504e9ed39b91d2a46748fb8881ad7f3603fbacef6c11c04aa7064"),
    ("pool/main/libd/libdrm/libdrm2_2.4.120-2build1_amd64.deb",
     "f5fb4e7ce17921cc466fb7911abf91495ffb181b36772f68e2e82cb621703112"),
    ("pool/main/e/expat/libexpat1_2.6.1-2build1_amd64.deb",
     "730b29497212289cda8a3139e693a3538af0cb57846c24f807d1bb293a591c5e"),
    ("pool/main/libf/libffi/libffi8_3.4.6-1build1_amd64.deb",
     "637e6a7744de08cd331a41f4efd0d24e6ea9064843dea9d1c6ca87bdb5f038a2"),
    ("pool/main/f/flac/libflac12t64_1.4.3+ds-2.1ubuntu2_amd64.deb",
     "f2396692573407b2fbc92cd83148342d4aec717d42d5e24ac64c471204a0b7a9"),
    ("pool/main/m/mesa/libgbm1_24.0.5-1ubuntu1_amd64.deb",
     "bffad21d2383429d6dbec8395ba8d8476a0ed69e7e0b1c3ef2f27da99211b200"),
    ("pool/main/libg/libgcrypt20/libgcrypt20_1.10.3-2build1_amd64.deb",
     "ff32bae487706ba6ca646b5b72d89b935e3b8b2811d02e4ad7360cb97d90e2d0"),
    ("pool/main/libg/libgpg-error/libgpg-error0_1.47-3build2_amd64.deb",
     "81a30b5888e0af43b72bfdbdb44209b82e521eadf8848ff201542e6d258f6a1f"),
    ("pool/main/l/lz4/liblz4-1_1.9.4-1build1_amd64.deb",
     "b9b69dedb126755f063396a6d3a3361066f9661f1c21cf5635cdd5ff6605796a"),
    ("pool/main/libm/libmd/libmd0_1.1.0-2build1_amd64.deb",
     "77f8cdbf03a43656bbf6604e18bd16834fc8bf2c949d4fbdf24ac297869b667a"),
    ("pool/main/l/lame/libmp3lame0_3.100-6build1_amd64.deb",
     "d739e9a3565f98b8b72a9e203a9a537bd1090af6cafb94c98adf8e27f9d3fb94"),
    ("pool/main/m/mpg123/libmpg123-0t64_1.32.5-1ubuntu1_amd64.deb",
     "5c297ac68cecdab93a7bcf694d769e553234999c446e019257d7df0d4be4bbc2"),
    ("pool/main/libo/libogg/libogg0_1.3.5-3build1_amd64.deb",
     "fbdb2fedbafb02c3f07f2c2ddeb4018b5e94e13462f93fbececdcefc06fe2ac1"),
    ("pool/main/o/opus/libopus0_1.4-1build1_amd64.deb",
     "6d8da32a832e655c9603543b47a0bc90c6f60bdee0c00fe7499bd50acad566cf"),
    ("pool/main/p/pulseaudio/libpulse0_16.1+dfsg1-2ubuntu10_amd64.deb",
     "9f19b90d61a4a2e46a422382e4ea1d6a1083bbb2afeac896ea00b7d309a240cf"),
    ("pool/main/libs/libsndfile/libsndfile1_1.2.2-1ubuntu5_amd64.deb",
     "d1c0111d685548ed0ea7d30eca963ef3fcccaad2083843d5bcc9a42e7bf79c49"),
    ("pool/main/s/systemd/libsystemd0_255.4-1ubuntu8_amd64.deb",
     "64ba97fddff491b86597e93b078e2fe3256cb80f9a113ae9d8dd1361f01b84ed"),
    ("pool/main/libv/libvorbis/libvorbis0a_1.3.7-1build3_amd64.deb",
     "d704ac0a5dd7a9797a2f2dc86fc8333c9973023b98b3510dcd2f1cc00da3624d"),
    ("pool/main/libv/libvorbis/libvorbisenc2_1.3.7-1build3_amd64.deb",
     "866c561fe81d4430a48c820f4e62f3cd138f4c8c7664f9203f02c1ced2eb55d4"),
    ("pool/main/w/wayland/libwayland-client0_1.22.0-2.1build1_amd64.deb",
     "6af0aed41d75149bea22fa468f01eb058ffe3e35ef07ff2f13fb88a90387881d"),
    ("pool/main/w/wayland/libwayland-cursor0_1.22.0-2.1build1_amd64.deb",
     "29c20da27e8e37c984cf0518635c11cd0441f4cb6a2919a9f6eee1809171d327"),
    ("pool/main/w/wayland/libwayland-egl1_1.22.0-2.1build1_amd64.deb",
     "76cd17d2776428e0a0551b8f8fd954eeb3d39ca8ed2f88a38a5e4ce3ce3b2f16"),
    ("pool/main/libx/libx11/libx11-6_1.8.7-1build1_amd64.deb",
     "397f84347476a3c5786b39f3ff6f0f82866eb3d8be6d2ad3efeadf019efe5b80"),
    ("pool/main/libx/libx11/libx11-xcb1_1.8.7-1build1_amd64.deb",
     "7d0d357e47cd6e1042be34da1d37cea313420b000035e71e855087c8268ab127"),
    ("pool/main/libx/libxau/libxau6_1.0.9-1build6_amd64.deb",
     "e40d29f1d1a62393bacaedebe0da3d9006084152a9f7e5e029293f08ce1c5c80"),
    ("pool/main/libx/libxcb/libxcb1_1.15-1ubuntu2_amd64.deb",
     "e1c6611d11ad7398326f1bf028afc34c3b14c51d917a3426b966ed4b9687fa58"),
    ("pool/main/libx/libxcursor/libxcursor1_1.2.1-1build1_amd64.deb",
     "1a4beee621454e42fe0d2f4e1633611126e43d891f0aec28e4a06744f6e9f04d"),
    ("pool/main/libx/libxdmcp/libxdmcp6_1.1.3-0ubuntu6_amd64.deb",
     "bcd336fce11ce2a45f34d0f95e6980af22529f22147e8f98c156e5cee8ee42bb"),
    ("pool/main/libx/libxext/libxext6_1.3.4-1build2_amd64.deb",
     "45783969a9ece9d7b7b733b8c60981584c53c6bc5ee3b42d295d2f80d1285679"),
    ("pool/main/libx/libxfixes/libxfixes3_6.0.0-2build1_amd64.deb",
     "0ee1015cccd063249e01c0cd0bf45f513c8ac9a1e5e485070c12e69192455e4a"),
    ("pool/main/libx/libxi/libxi6_1.8.1-1build1_amd64.deb",
     "0ea7acb5e8a8ce4d6653b30e03a319bfe136db7bfb5ee7cad34c2aa1272ea8d9"),
    ("pool/main/libx/libxkbcommon/libxkbcommon0_1.6.0-1build1_amd64.deb",
     "2b9caeb423efb540296a1cb20b872cc630c23908407ecb5c1c787a617622d664"),
    ("pool/main/libx/libxrandr/libxrandr2_1.5.2-2build1_amd64.deb",
     "f2955a5e594f5724b58ad241d9231ea191cb36574a0d5e5ca6b661cd41d6256d"),
    ("pool/main/libx/libxrender/libxrender1_0.9.10-1.1build1_amd64.deb",
     "d70bd831aebe8d4834b5dd2ed98df26dd6bd27f1042c47543bd7f66df1ae22ea"),
    ("pool/main/libx/libxss/libxss1_1.2.3-1build3_amd64.deb",
     "0ac60a2cc034ccc4bf4e2f846f38110469d7ae43b47e808d67aafc538bc3695e"),
    ("pool/main/w/wayland/libwayland-server0_1.22.0-2.1build1_amd64.deb",
     "edbfa4b6857691ae922cc768a753fab230eee9e956aa2ce4f2eaa8d9ad777dca"),
    ("pool/main/libx/libxcb/libxcb-randr0_1.15-1ubuntu2_amd64.deb",
     "b5cb519823a05a617b543dc9b6a9289b7648b20048244abe021caa706309835a"),
    ("pool/main/libs/libsdl2/libsdl2-2.0-0_2.30.0+dfsg-1build3_amd64.deb",
     "843864d304b9084b059c8385a2d11dd769534488b6f0091304f362e3b812d7d7"),
    # sox (play and rec through the ALSA output) and the libraries only it needs
    ("pool/main/b/bzip2/libbz2-1.0_1.0.8-5.1_amd64.deb",
     "695132af8a40fe417f7d4e5ea742d64521c04e069442a4719c2bd0f10ab93d94"),
    ("pool/universe/libg/libgsm/libgsm1_1.0.22-1build1_amd64.deb",
     "8cc765587a3275e861f166a98b4b79dd8d5c673f97e17967945ab4d1eb89c89a"),
    ("pool/main/libt/libtool/libltdl7_2.4.7-7build1_amd64.deb",
     "02d0d2d7e1bf73f838a92f8df7a9b07b698c837e82bd89aee6a581cf793cd966"),
    ("pool/main/f/file/libmagic-mgc_5.45-3build1_amd64.deb",
     "f0ed0e314a63ae1b7e5403d7e43a26c6e0c0ee69e9a305babcf9c6775607377a"),
    ("pool/main/f/file/libmagic1t64_5.45-3build1_amd64.deb",
     "fc2319f342c437e044ccf854e0c95bd472a7eb0b9363e1f76ad1524ab04df68e"),
    ("pool/universe/o/opencore-amr/libopencore-amrnb0_0.1.6-1build1_amd64.deb",
     "b35434b1103f14574985b9533bf1dc202c00d915cfa59431e45138dd96e423ee"),
    ("pool/universe/o/opencore-amr/libopencore-amrwb0_0.1.6-1build1_amd64.deb",
     "ed1d6213b403f538bf64ec0970eae7b557ac1dc13a786961c2626c5342de2159"),
    ("pool/main/libp/libpng1.6/libpng16-16t64_1.6.43-5build1_amd64.deb",
     "128d505038cc19dfc218658562ca5917ec242b79e6e2bbfb65ee27f297a53924"),
    ("pool/universe/s/sox/libsox-fmt-alsa_14.4.2+git20190427-4build4_amd64.deb",
     "294f74493a6a0b11d894a11c39be4289a16307ae4fb40d5e3752b14677c7ec92"),
    ("pool/universe/s/sox/libsox-fmt-base_14.4.2+git20190427-4build4_amd64.deb",
     "76c783610a4618ae996ae64566c65218da784d425d196e83f17a54e73f3e4eac"),
    ("pool/universe/s/sox/libsox3_14.4.2+git20190427-4build4_amd64.deb",
     "758545226e1aaa7a688622e0f857d2bfd20614bb369c5d237d67838fa0070720"),
    ("pool/main/libv/libvorbis/libvorbisfile3_1.3.7-1build3_amd64.deb",
     "fa12c52814bbfddff78186e7300b535bed30db02e4532fafae969aa9e5a79316"),
    ("pool/main/w/wavpack/libwavpack1_5.6.0-1build1_amd64.deb",
     "20b3bf66bdeac236db7255139d8fd2460bca90ddb9155fa30411fb05b6dfd4ec"),
    ("pool/universe/s/sox/sox_14.4.2+git20190427-4build4_amd64.deb",
     "70d26559a960425e5eee551a5c3fe340368c585cbce00f7937f16e7b19677202"),
    ("pool/main/z/zlib/zlib1g_1.3.dfsg-3.1ubuntu2_amd64.deb",
     "0b93d16d7498f092fa3070fbbad28cdbc6b3d640f1a7681b96fc37f20d1219f1"),
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
    # alsadisk.py and serverdisk.py run at once under make -j and share
    # some .debs (libc6 and the like): each writes its own part file, so
    # neither renames the other's away.
    part = f"{dest}.{os.getpid()}.part"
    for mirror in MIRRORS:
        try:
            with urllib.request.urlopen(mirror + path, timeout=120) as r, open(part, "wb") as f:
                shutil.copyfileobj(r, f)
        except OSError as e:
            print(f"alsadisk: {mirror}{path}: {e}", file=sys.stderr)
            continue
        if sha256(part) == digest:
            os.replace(part, dest)
            return dest
        print(f"alsadisk: {mirror}{path}: wrong checksum", file=sys.stderr)
    if os.path.exists(part):
        os.remove(part)
    sys.exit(f"alsadisk: cannot get {path}")


def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    out, = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "alsa")
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
    for d in ("dev", "proc", "tmp", "etc"):
        os.makedirs(f"{work}/root/{d}", exist_ok=True)
    # SDL has no program of its own: sdlbeep.c is built against the unpacked library
    # with the host compiler (the same Ubuntu release as the packages) and plays through SDL's ALSA backend.
    sdl = glob.glob(work + "/root/usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0.*")[0]
    subprocess.check_call(["gcc", "-O1", "-o", work + "/root/usr/bin/sdlbeep", os.path.join(HERE, "progs", "sdlbeep.c"), sdl,
                           "-Wl,--unresolved-symbols=ignore-in-shared-libs", "-Wl,-rpath-link," + os.path.dirname(sdl)])
    root = work + "/root.tar"
    with tarfile.open(root, "w", format=tarfile.PAX_FORMAT) as t:
        t.add(work + "/root", arcname=".")
    env = dict(os.environ, MKDISK_FREE_MB="150")   # room for the unpacked root
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={work}/bb/usr/bin/busybox", f"root.tar={root}"], env)


if __name__ == "__main__":
    main()
