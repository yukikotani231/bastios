#!/bin/sh
# Runs inside the booted root once the default runlevel is up, as init's
# last job. The first boot checks the services and restarts the machine
# through OpenRC; the second finds what the first left and powers off.
say() { echo "check: $*"; }
n=$(cat /var/boots 2>/dev/null || echo 0)
n=$((n + 1))
echo $n > /var/boots
say "boot $n"
say "hostname $(hostname)"
/bin/rc-status -s | sed 's/  *\[ *\(.*[a-z]\) *\]/ \1/' | grep -E '^ (syslog|crond|chronyd|networking|hostname|seedrng|devfs|localmount) ' | sort | while read l; do say "service $l"; done
for d in syslogd crond chronyd; do
    ps | grep "[/ ]$d" > /dev/null && say "$d running"
done
for p in /proc/[0-9]*; do
    case "$(cat $p/comm 2>/dev/null)" in chronyd) say "chronyd uid $(awk '/^Uid:/{print $2}' $p/status)";; esac
done
if [ $n = 1 ]; then
    logger -t bt hello-from-logger
    sleep 1
    say "logged $(grep -c hello-from-logger /var/log/messages)"
    ln -sf /usr/share/zoneinfo/Asia/Tokyo /etc/localtime
    say "zone $(date -d @1893456000 +%Z)"
    chronyc -h 127.0.0.1 waitsync 60 0.5 0 0.5 > /dev/null 2>&1
    say "waitsync $?"
    say "year $(date -u +%Y)"
    echo '* * * * * touch /var/cron-ran' > /etc/crontabs/root
    i=0
    while [ $i -lt 90 ] && [ ! -e /var/cron-ran ]; do sleep 1; i=$((i + 1)); done
    [ -e /var/cron-ran ] && say "cron ran"
    reboot
else
    say "logged $(grep -c hello-from-logger /var/log/messages)"
    say "cron ran $(ls /var/cron-ran 2>&1)"
    poweroff
fi
