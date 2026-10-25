#!/usr/bin/env python3
"""Writes the C project the make workloads build: proj/hello.c, FILES sources
of FUNCS arithmetic functions each, a header, a main and a Makefile.

    mkproj.py OUT.tar [FILES [FUNCS]]
"""
import io, sys, tarfile

out = sys.argv[1]
files = int(sys.argv[2]) if len(sys.argv) > 2 else 24
funcs = int(sys.argv[3]) if len(sys.argv) > 3 else 40

def add(t, name, text, mode=0o644):
    b = text.encode()
    i = tarfile.TarInfo("proj/" + name)
    i.size, i.mode, i.mtime = len(b), mode, 0
    t.addfile(i, io.BytesIO(b))

with tarfile.open(out, "w") as t:
    d = tarfile.TarInfo("proj")
    d.type, d.mode = tarfile.DIRTYPE, 0o755
    t.addfile(d)
    add(t, "hello.c", '#include <stdio.h>\nint main(void) { printf("hello\\n"); return 0; }\n')
    add(t, "all.h", "".join(f"int f{i}_{j}(int);\n" for i in range(files) for j in range(funcs)))
    for i in range(files):
        body = ['#include "all.h"', "#include <stdlib.h>", "#include <string.h>"]
        for j in range(funcs):
            body.append(f"int f{i}_{j}(int x) {{ int s = {i * funcs + j}; "
                        f"for (int k = 0; k < x % 7 + {j % 5 + 1}; k++) s = s * 31 + (x ^ k); "
                        f"return s % 1009 + (x > {j} ? f{i}_{(j + 1) % funcs}(x - 1) : 0); }}")
        add(t, f"s{i}.c", "\n".join(body) + "\n")
    add(t, "main.c", '#include "all.h"\nint main(void) { return f0_0(3) & 1; }\n')
    objs = " ".join(f"s{i}.o" for i in range(files)) + " main.o"
    add(t, "Makefile", f"OBJS = {objs}\nprog: $(OBJS)\n\tgcc -o $@ $(OBJS)\n%.o: %.c all.h\n\tgcc -O1 -c -o $@ $<\n")
