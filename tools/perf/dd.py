#!/usr/bin/env python3
"""Resume el log [dd] por frame: draws por (tipo de prim, ps, RT) y copias, para frames dados.
Uso: dd.py <frame1,frame2,...> <log1> [log2 ...]"""
import re
import sys
from collections import Counter, defaultdict

frames = {int(x) for x in sys.argv[1].split(",")}
line_re = re.compile(r"\[dd\] f(\d+) (ok|copy) p(\d+) n(\d+) h(\d+) vs([0-9A-F]+) ps([0-9A-F]+) em(\d+) cf(\d+) cb([0-9A-F]+) pitch(\d+) msaa(\d+) cm([0-9A-F]+) z([0-9A-F]+) blend([0-9A-F]+)(.*)$")
tex_re = re.compile(r" t(\d):f(\d+) (\d+)x(\d+) @([0-9A-F]+)([TL]) d(\d)")
wr_re = re.compile(r"wtl([0-9A-F]+) wbr([0-9A-F]+)")

by_frame = defaultdict(list)
last = None
for path in sys.argv[2:]:
    with open(path, "rb") as f:
        for raw in f:
            line = raw.replace(b"\x00", b"").decode("utf-8", "replace").rstrip("\r\n")
            i = line.find("[dd]")
            if i < 0:
                continue
            body = line[i:]
            m = line_re.match(body)
            if m:
                fr = int(m.group(1))
                if fr in frames:
                    last = {"kind": m.group(2), "prim": int(m.group(3)), "n": int(m.group(4)),
                            "ps": m.group(7), "vs": m.group(6), "cb": m.group(10), "pitch": int(m.group(11)),
                            "msaa": int(m.group(12)), "blend": m.group(15), "tex": tex_re.findall(m.group(16)),
                            "wbr": None}
                    by_frame[fr].append(last)
                else:
                    last = None
            elif last is not None and body.startswith("[dd]   "):
                w = wr_re.search(body)
                if w and last["wbr"] is None:
                    last["wbr"] = w.group(2)

for fr in sorted(by_frame):
    ds = by_frame[fr]
    ok = [d for d in ds if d["kind"] == "ok"]
    cp = [d for d in ds if d["kind"] == "copy"]
    print(f"\n=== frame {fr}: {len(ok)} draws, {len(cp)} copies")
    groups = Counter()
    for d in ok:
        texs = ",".join(f"{t[1]}:{t[2]}x{t[3]}" for t in d["tex"])
        groups[(d["prim"], d["n"], d["ps"][:8], f"cb{d['cb']}", f"p{d['pitch']}", f"msaa{d['msaa']}", f"scissor_br={d['wbr']}", texs)] += 1
    for k, c in groups.most_common(14):
        print(f"  x{c:<3} prim{k[0]} n{k[1]} ps{k[2]} {k[3]} {k[4]} {k[5]} {k[6]} tex[{k[7]}]")
