#!/bin/sh
# The BastiOS installer (docs/install.md): partitions the disk, puts the
# boot loader, the kernel and an Alpine Linux userland on it, and restarts
# into the new system.

INST=/install
BB=/install/busybox

stop() {
  echo
  echo "The installation stopped: $1"
  exit 1
}

echo
echo "==== BastiOS installer ===="
echo
$INST/binstall info > /tmp/disk.txt || stop "there is no disk to install to"
echo "The disk: $(cat /tmp/disk.txt)"
echo
echo "This erases the whole disk and installs BastiOS on it."
echo "Type erase and Enter to go on; anything else stops, and changes nothing."
printf "> "
read answer
if [ "$answer" != erase ]; then
  stop "nothing was changed"
fi

echo
echo "-- partitioning, boot loader, kernel"
$INST/binstall write $INST/pay || stop "the disk is not usable yet; start the installer again"
$INST/binstall mount || stop "the new partition cannot be used"

echo
echo "-- Alpine Linux"
mkdir /disk/a || stop "cannot make /disk/a"
$BB gunzip -c $INST/root.tgz | $BB tar -x -C /disk/a || stop "cannot unpack Alpine Linux"
for d in null:1:3 zero:1:5 random:1:8 urandom:1:9 tty:5:0 console:5:1; do
  set -- $(echo $d | $BB tr : ' ')
  /disk/a/bin/busybox mknod /disk/a/dev/$1 c $2 $3 || stop "cannot make /dev/$1"
done
cp $BB /disk/busybox || stop "cannot copy BusyBox"
chmod 755 /disk/busybox
cat > /disk/alpine <<'EOS'
#!/disk/busybox sh
# Enters the Alpine Linux userland installed in /disk/a.
cp /etc/resolv.conf /disk/a/etc/resolv.conf 2>/dev/null
exec /disk/a/bin/busybox chroot /disk/a /bin/sh -l
EOS
chmod 755 /disk/alpine
$BB sync

echo
echo "BastiOS is installed. Take out the installation media, then press Enter to restart."
echo "In the new system, start ash and run /disk/alpine for the Alpine Linux shell."
read answer
$BB sync
reboot -f
