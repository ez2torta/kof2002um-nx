#!/usr/bin/env python3
"""Para un hilo, agrupa las pilas muestreadas de rex_profile.log por (hoja, llamador) y muestra los llamadores
de las funciones hoja pedidas. Uso: hotstack.py <log> <elf> <addr2line> <thread_prefix> <hoja1,hoja2> [top]"""
import re
import subprocess
import sys
from collections import Counter

log, elf, a2l, prefix, leaves_arg = sys.argv[1:6]
top_n = int(sys.argv[6]) if len(sys.argv) > 6 else 12
leaves_wanted = set(leaves_arg.split(","))

thr_re = re.compile(r'^-- thread \d+ "([^"]*)"')
smp_re = re.compile(r"^\s+(\d+) samples,")
stk_re = re.compile(r"^\s+([\d.]+)%\s+stack (.*)$")

raw_stacks = Counter()  # tuple(addrs) -> peso en muestras
cur = None
block_samples = 0
with open(log, "rb") as f:
    for raw in f:
        line = raw.replace(b"\x00", b"").decode("utf-8", "replace").rstrip("\r\n")
        m = thr_re.match(line)
        if m:
            cur, block_samples = m.group(1), 0
            continue
        if line.startswith("===="):
            cur = None
            continue
        if cur is None or not cur.startswith(prefix):
            continue
        m = smp_re.match(line)
        if m:
            block_samples = int(m.group(1))
            continue
        m = stk_re.match(line)
        if m and block_samples:
            addrs = tuple(int(x, 16) for x in re.findall(r"image\+0x([0-9a-f]+)", m.group(2)))
            if addrs:
                raw_stacks[addrs] += float(m.group(1)) * block_samples / 100.0

all_addrs = sorted({a for st in raw_stacks for a in st[:4]})
out = subprocess.run([a2l, "-f", "-C", "-e", elf] + [hex(a) for a in all_addrs],
                     capture_output=True, text=True, check=True).stdout.splitlines()
name = {a: out[2 * i].strip()[:90] for i, a in enumerate(all_addrs)}

total = sum(raw_stacks.values())
if leaves_arg == "*":
    agg = Counter()
    for st, w in raw_stacks.items():
        agg[" <- ".join(name[a] for a in st[:3])] += w
    print(f"{prefix}*: {total:.0f} muestras en pilas (cadenas de 3 frames, hoja primero)")
    for chain, w in agg.most_common(top_n):
        print(f"  {100 * w / total:5.1f}%  {chain}")
    sys.exit(0)
print(f"{prefix}*: {total:.0f} muestras en pilas")
for leaf in leaves_wanted:
    by_chain = Counter()
    leaf_total = 0.0
    for st, w in raw_stacks.items():
        if name[st[0]] == leaf:
            leaf_total += w
            chain = " <- ".join(name[a] for a in st[1:4])
            by_chain[chain] += w
    print(f"\n[{leaf}] {leaf_total:.0f} muestras ({100 * leaf_total / total:.1f}% del hilo)")
    for chain, w in by_chain.most_common(top_n):
        print(f"  {100 * w / total:5.1f}%  {chain}")
