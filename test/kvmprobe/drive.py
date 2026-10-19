# usage: drive.py KERNEL DISK CMD TIMEOUT [SMP]
import sys, io
sys.path.insert(0, "test/compat")
import run as R
smp = sys.argv[5] if len(sys.argv) > 5 else "2"
orig = R.subprocess.Popen
def popen(cmd, **kw):
    if cmd and "qemu-system" in cmd[0]:
        cmd = [("%s" % smp) if (i > 0 and cmd[i-1] == "-smp") else c for i, c in enumerate(cmd)]
    return orig(cmd, **kw)
R.subprocess.Popen = popen
out = io.BytesIO()
g = R.Guest(sys.argv[1], sys.argv[2], out, None)
g.boot()
text, st = g.run(f"echo @@begin; {sys.argv[3]}; echo @@end $?", float(sys.argv[4]))
print(text[-3000:])
print("status", st)
g.stop()
