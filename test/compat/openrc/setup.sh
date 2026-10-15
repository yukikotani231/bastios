#!/bin/sh
# Turns the Alpine root at $1 (openrc, chrony, tzdata and ifupdown-ng
# installed; $2 is the port of the NTP server on the host) into a system
# that boots the way setup-alpine leaves one: BusyBox init runs the OpenRC
# runlevels, with a logger, cron and an NTP client in them.
R=$1
NTPPORT=$2
S=/disk/openrc
B=/disk/busybox
for n in syslog crond ntpd chronyd; do
    $B cp $S/$n $R/etc/init.d/$n
    $B chmod 755 $R/etc/init.d/$n
done
$B mkdir -p $R/etc/crontabs $R/dev/pts
for d in null:1:3 zero:1:5 random:1:8 urandom:1:9 tty:5:0 console:5:1; do
    set -- $(echo $d | $B tr : ' ')
    $B mknod $R/dev/$1 c $2 $3 2>/dev/null
done
# The root was made by a container image: its rc.conf names a container.
$B sed -i 's/^rc_sys=.*/#rc_sys=""/' $R/etc/rc.conf
echo bastio-box > $R/etc/hostname
printf 'auto lo\niface lo inet loopback\n' > $R/etc/network/interfaces
printf 'server 10.0.2.2 port '"$NTPPORT"' iburst\nmakestep 1 3\nrtcsync\n' > $R/etc/chrony/chrony.conf
$B cp $S/check.sh $R/check.sh
$B chmod 755 $R/check.sh
cat > $R/etc/inittab <<'I'
::sysinit:/sbin/openrc sysinit
::sysinit:/sbin/openrc boot
::wait:/sbin/openrc default
::wait:/check.sh
::ctrlaltdel:/sbin/reboot
::shutdown:/sbin/openrc shutdown
I
for s in devfs:sysinit dmesg:sysinit sysctl:boot hostname:boot bootmisc:boot swclock:boot seedrng:boot syslog:boot networking:boot crond:default chronyd:default killprocs:shutdown savecache:shutdown; do
    $B chroot $R /sbin/rc-update add ${s%:*} ${s#*:} >/dev/null 2>&1
done
