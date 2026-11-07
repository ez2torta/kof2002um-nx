# Copia un árbol de datos descomprimiendo los XCTD (0FF512ED) con ./xctd; el resto se copia tal cual.
# uso: python3 unpack_all.py <src> <dst>
import os, shutil, subprocess, sys, collections
src, dst = sys.argv[1], sys.argv[2]
xctd = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xctd")
st = collections.Counter(); bad = []
for dp, _, fs in os.walk(src):
    for fn in fs:
        p = os.path.join(dp, fn); o = os.path.join(dst, os.path.relpath(p, src))
        os.makedirs(os.path.dirname(o), exist_ok=True)
        with open(p, "rb") as f: magic = f.read(4)
        if magic != b"\x0f\xf5\x12\xed":
            shutil.copyfile(p, o); st["plain"] += 1; continue
        r = subprocess.run([xctd, p, o, "-k"], capture_output=True, text=True).stdout.strip()
        last = r.splitlines()[-1] if r else ""
        if last.startswith("ok"):
            st["ok"] += 1
            if "ZEROED" in last or "zero" in last: bad.append((os.path.relpath(p, src), r.replace("\n", " | ")))
        else: st["fail"] += 1; bad.append((os.path.relpath(p, src), r)); shutil.copyfile(p, o)
print(dict(st))
for b in bad: print(*b)
