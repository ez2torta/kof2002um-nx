import os, struct, subprocess, sys, collections
root = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
stats = collections.Counter(); bad = []; flags_seen = collections.Counter(); low_bits = collections.Counter()
for dp, _, fs in os.walk(root):
    for fn in fs:
        p = os.path.join(dp, fn)
        d = open(p, "rb").read()
        if d[:4] != b"\x0f\xf5\x12\xed":
            continue
        flags, = struct.unpack(">I", d[12:16]); f10, = struct.unpack(">I", d[16:20])
        flags_seen[hex(flags)] += 1; low_bits[hex(f10 & 0xFFF)] += 1
        wb = 15 + ((flags >> 4) & 3); usize = f10 >> 12
        q = 0x14; raw = bytearray(); ok = True
        while q < len(d):
            if d[q] == 0xFF:
                u, c = struct.unpack(">HH", d[q+1:q+5]); q += 5
            else:
                c, = struct.unpack(">H", d[q:q+2]); q += 2
            raw += d[q:q+c]; q += c
        if q != len(d):
            stats["frame_mismatch"] += 1; bad.append((p, "frames")); continue
        open("/tmp/x_in.bin", "wb").write(bytes(raw) + b"\0" * 64)
        r = subprocess.run([os.path.join(here, "xctdtest"), "/tmp/x_in.bin", "/tmp/x_out.bin", "0", str(wb), str(usize), "0"],
                           capture_output=True, text=True).stdout
        if "result=0 " in r and os.path.getsize("/tmp/x_out.bin") == usize:
            stats["ok"] += 1
        else:
            stats["fail"] += 1; bad.append((p, r.strip()))
print(dict(stats)); print("flags", dict(flags_seen)); print("low12", dict(low_bits))
for b in bad[:15]: print(b)
