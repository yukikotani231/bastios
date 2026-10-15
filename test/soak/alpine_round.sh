# Run by test/soak.py in BastiOS's ash with the Alpine root unpacked in
# /disk/a: rounds of builds and file work, in the format msoak prints.
# $1: the number of rounds, $2: the host and port of the web server.
BB=/disk/busybox
R=/disk/a
fail() { echo "soak: FAIL $*"; exit 1; }
ch() { $BB chroot $R /bin/sh -c "$1"; }
sample() {
  free=$($BB sed -n 's/^MemFree: *\([0-9]*\).*/\1/p' /proc/meminfo)
  cached=$($BB sed -n 's/^Cached: *\([0-9]*\).*/\1/p' /proc/meminfo)
  fds=$($BB ls /proc/self/fd | $BB wc -l)
  procs=$($BB ls /proc | $BB grep -c '^[0-9]')
}
# the project: 14 files of generated functions, a Makefile, a main
ch '
mkdir -p /root/w && cd /root/w || exit 1
i=1
while [ $i -le 14 ]; do
  {
    echo "#include <stdio.h>"
    echo "#include <string.h>"
    echo "static unsigned long mix(unsigned long x) { return x * 6364136223846793005UL + 1442695040888963407UL; }"
    j=1
    while [ $j -le 40 ]; do
      echo "unsigned long f${i}_$j(unsigned long x) { char b[64]; memset(b, (int)x, sizeof b); for (int k = 0; k < 64; k++) x = mix(x ^ b[k]); return x + $j; }"
      j=$((j + 1))
    done
    echo "unsigned long g$i(unsigned long x) {"
    j=1
    while [ $j -le 40 ]; do echo "  x = f${i}_$j(x);"; j=$((j + 1)); done
    echo "  return x; }"
  } > m$i.c
  i=$((i + 1))
done
{
  echo "#include <stdio.h>"
  i=1; while [ $i -le 14 ]; do echo "unsigned long g$i(unsigned long);"; i=$((i + 1)); done
  echo "int main(void) { unsigned long x = 1;"
  i=1; while [ $i -le 14 ]; do echo "  x = g$i(x);"; i=$((i + 1)); done
  echo "  printf(\"app %lu\\n\", x); return 0; }"
} > main.c
{
  echo "OBJ = main.o m1.o m2.o m3.o m4.o m5.o m6.o m7.o m8.o m9.o m10.o m11.o m12.o m13.o m14.o"
  echo "app: \$(OBJ)"
  printf "\t\$(CC) -o app \$(OBJ)\n"
  echo "%.o: %.c"
  printf "\t\$(CC) -O2 -c -o \$@ \$<\n"
  echo "clean:"
  printf "\trm -f app *.o\n"
} > Makefile
' || fail "writing the project"
want=""
n=1
while [ $n -le $1 ]; do
  out=$(ch 'cd /root/w && make -j2 -s app && ./app') || fail "build (round $n)"
  [ -n "$want" ] || want=$out
  [ "$out" = "$want" ] || fail "the program printed '$out', not '$want' (round $n)"
  ch 'cd /root/w && make -s clean && test ! -e app' || fail "clean (round $n)"
  # a copy of the tree through a pipe, compressed and unpacked, and compared
  ch 'rm -rf /root/cp && mkdir /root/cp && cd /root/w && tar cf - . | gzip -1 | gunzip | tar xf - -C /root/cp && diff -r /root/w /root/cp && rm -rf /root/cp' || fail "tar/gzip/diff (round $n)"
  # a download, checked
  if [ -n "$2" ]; then
    sum=$($BB wget -q -O - http://$2/blob | $BB md5sum | $BB cut -d' ' -f1)
    [ "$sum" = "$3" ] || fail "download checksum '$sum' (round $n)"
  fi
  sample
  echo "soak: round $n ok ops=$((n * 4)) free=$free cached=$cached fds=$fds procs=$procs"
  n=$((n + 1))
done
