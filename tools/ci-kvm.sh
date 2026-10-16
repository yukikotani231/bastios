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
qemu=$(command -v qemu-system-x86_64)
printf '#!/bin/sh\nexec %s -accel kvm "$@"\n' "$qemu" >"$dir/qemu-system-x86_64"
chmod +x "$dir/qemu-system-x86_64"
echo "$dir" >>"$GITHUB_PATH"
"$dir/qemu-system-x86_64" -machine none -display none -S -monitor none -serial none &
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
