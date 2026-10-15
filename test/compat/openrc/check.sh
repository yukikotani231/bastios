#!/bin/sh
# Runs inside the booted root once the default runlevel is up.
echo "== hostname"
hostname
echo "== services"
/bin/rc-status -s | sed 's/  *\[ *\(.*[a-z]\) *\]/ \1/' | sort
echo "== daemons"
for d in syslogd crond chronyd; do
    ps | grep "[/ ]$d" > /dev/null && echo "$d running"
done
echo "== chronyd user"
ps -o user,args | grep "[c]hronyd -f" | cut -d" " -f1
grep "^Cap" /proc/$(cat /run/chrony/chronyd.pid)/status | sed -n 2,3p
echo "== logger"
logger -t bt hello-from-logger
sleep 1
grep -c hello-from-logger /var/log/messages
echo "== time zone"
ln -sf /usr/share/zoneinfo/Asia/Tokyo /etc/localtime
date -d @1893456000 +%Z
echo "== chrony"
chronyc -h 127.0.0.1 waitsync 40 0.5 0 0.5 > /dev/null 2>&1; echo waitsync $?
date -u +%Y
echo "== done"
poweroff
