#!/usr/bin/env python3
"""Agrega muestras de pc de un hilo en rex_profile.log y las agrupa por funcion con addr2line.
Uso: hot.py <profile.log> <elf> <addr2line> <thread_prefix> [top_n]"""
import re
import subprocess
import sys
from collections import Counter

log, elf, a2l, prefix = sys.argv[1:5]
top_n = int(sys.argv[5]) if len(sys.argv) > 5 else 40

pc_re = re.compile(r"^\s+[\d.]+%\s+pc image\+0x([0-9a-f]+)\s+x(\d+)")
thr_re = re.compile(r'^-- thread \d+ "([^"]*)"')
samples = Counter()
total = 0
cur = None
with open(log, "rb") as f:
    for raw in f:
        line = raw.replace(b"\x00", b"").decode("utf-8", "replace").rstrip("\r\n")
        m = thr_re.match(line)
        if m:
            cur = m.group(1)
            continue
        if line.startswith("===="):
            cur = None
            continue
        if cur is not None and cur.startswith(prefix):
            m = pc_re.match(line)
            if m:
                n = int(m.group(2))
                samples[int(m.group(1), 16)] += n
                total += n

addrs = sorted(samples)
out = subprocess.run([a2l, "-f", "-C", "-e", elf] + [hex(a) for a in addrs],
                     capture_output=True, text=True, check=True).stdout.splitlines()
# addr2line -f emite 2 lineas por direccion: funcion y archivo:linea
funcs = {}
for i, a in enumerate(addrs):
    funcs[a] = out[2 * i].strip()

by_func = Counter()
for a, n in samples.items():
    by_func[funcs[a]] += n

print(f"hilo '{prefix}*': {total} muestras, {len(by_func)} funciones")
cum = 0
for name, n in by_func.most_common(top_n):
    cum += n
    print(f"{100.0 * n / total:5.1f}%  (acum {100.0 * cum / total:5.1f}%)  {name[:110]}")
