VERSION = 1.2
PREFIX = /usr
CFLAGS ?= -O2

all: libgreet.so.1 libgreet.pc

libgreet.so.1: greet.c greet.h
	$(CC) $(CFLAGS) -fPIC -shared -Wl,-soname,libgreet.so.1 -o $@ greet.c

libgreet.pc: libgreet.pc.in
	sed -e 's,@PREFIX@,$(PREFIX),' -e 's,@VERSION@,$(VERSION),' libgreet.pc.in > $@

check: libgreet.so.1
	ln -sf libgreet.so.1 libgreet.so
	$(CC) $(CFLAGS) -o test test.c -L. -lgreet
	LD_LIBRARY_PATH=. ./test

install: all
	install -Dm755 libgreet.so.1 $(DESTDIR)$(PREFIX)/lib/libgreet.so.1
	ln -sf libgreet.so.1 $(DESTDIR)$(PREFIX)/lib/libgreet.so
	install -Dm644 greet.h $(DESTDIR)$(PREFIX)/include/greet.h
	install -Dm644 libgreet.pc $(DESTDIR)$(PREFIX)/lib/pkgconfig/libgreet.pc
	install -Dm644 greet.1 $(DESTDIR)$(PREFIX)/share/man/man1/greet.1
