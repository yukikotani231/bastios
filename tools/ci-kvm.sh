#!/bin/sh
# Runs the x86-64 guests of a CI job under KVM when the runner offers it.
#
# GitHub's hosted Linux runners have /dev/kvm, but only root may open it.
# This opens it to the runner's user and puts a qemu-system-x86_64 first on
# the PATH that adds `-accel kvm` to every command line, so the tests, which
# call qemu-system-x86_64 by name, need no change. Without /dev/kvm nothing
# changes and QEMU emulates the processor (TCG), as it does on a machine of
# our own without KVM.
#
# Only the accelerator changes: the machine, the CPU model (`-cpu qemu64` and
# the features the tests name), the memory and the devices are the same, and
# the kernel, the disks and the checks are the same files.
#
# CI_KVM_PCID=1 (the pcid-kvm job) also adds +pcid to every `-cpu` and
# `-append pcid` to the command line, so the kernel runs with process-context
# identifiers (boot/platform.c) and the tests pass through them.
#
# Only the compat job uses it. The test and native jobs do not yet: under
# KVM the stack guard test (it moves the guest's stack pointer from
# outside) saw no fault, and the e1000 TCP test, which times the guest's
# DHCP and resends, failed three of its checks. Nor does compat-ltp: it
# took no less time (its tests mostly wait), and LTP inotify11 failed twice
# under KVM, where the two processors really run at once. Those boots stay
# emulated until the tests hold under both.
set -eu
if [ ! -e /dev/kvm ]; then
    echo "ci-kvm: no /dev/kvm, the guests run under TCG"
    exit 0
fi
echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' |
    sudo tee /etc/udev/rules.d/99-kvm-ci.rules >/dev/null
sudo udevadm control --reload-rules
sudo udevadm trigger --name-match=kvm
dir="${RUNNER_TEMP:-/tmp}/kvm-bin"
mkdir -p "$dir"
# Ask for QEMU without our own directory on the PATH: a second run in the same
# job (the pcid-kvm job runs this twice) would otherwise find the first wrapper
# and make the new one exec itself.
qemu=$(PATH=$(printf %s "$PATH" | tr : '\n' | grep -vxF "$dir" | paste -sd:) command -v qemu-system-x86_64)
if [ -n "${CI_KVM_PCID:-}" ]; then
    # Every argument after -cpu gets +pcid; -append pcid goes last.
    cat >"$dir/qemu-system-x86_64" <<EOF
#!/bin/sh
n=\$#
prev=
while [ "\$n" -gt 0 ]; do
    a=\$1
    shift
    n=\$((n - 1))
    o=\$a
    if [ "\$prev" = -cpu ]; then a="\$a,+pcid"; fi
    set -- "\$@" "\$a"
    prev=\$o
done
exec $qemu -accel kvm "\$@" -append pcid
EOF
else
    printf '#!/bin/sh\nexec %s -accel kvm "$@"\n' "$qemu" >"$dir/qemu-system-x86_64"
fi
chmod +x "$dir/qemu-system-x86_64"
echo "$dir" >>"$GITHUB_PATH"
# Probe QEMU itself: the PCID wrapper adds -append, which -machine none refuses.
"$qemu" -accel kvm -machine none -display none -S -monitor none -serial none &
pid=$!
sleep 1
if kill -0 "$pid" 2>/dev/null; then
    kill "$pid"
    echo "ci-kvm: the guests run under KVM ($qemu -accel kvm)"
else
    wait "$pid" || true
    rm "$dir/qemu-system-x86_64"
    echo "ci-kvm: KVM did not start, the guests run under TCG"
fi
