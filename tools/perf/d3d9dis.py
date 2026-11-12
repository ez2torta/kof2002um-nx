#!/usr/bin/env python3
"""Escanea un exe en busca de shaders D3D9 compilados (SM1.x/2.x/3.0) y los desensambla a texto.

Uso: d3d9dis.py <exe> <carpeta_salida>

Encuentra bloques que empiezan con un token de version (0xFFFF03xx = ps, 0xFFFE03xx = vs, ...) y terminan con el token
END (0x0000FFFF), recorriendo instrucciones y comentarios. Decodifica la tabla de constantes (CTAB) para poner nombre a
los registros. Escribe un .txt por shader y imprime un resumen. Solo lee; no ejecuta nada del exe.
"""
import os
import re
import struct
import sys

VERSIONS = {
    0xFFFF0200: "ps_2_0", 0xFFFE0200: "vs_2_0", 0xFFFF0300: "ps_3_0", 0xFFFE0300: "vs_3_0",
    0xFFFF0101: "ps_1_1", 0xFFFE0101: "vs_1_1", 0xFFFF0104: "ps_1_4", 0xFFFF0201: "ps_2_x",
    0xFFFE0201: "vs_2_x", 0xFFFF0100: "ps_1_0",
}
OPCODES = {
    0: "nop", 1: "mov", 2: "add", 3: "sub", 4: "mad", 5: "mul", 6: "rcp", 7: "rsq", 8: "dp3", 9: "dp4", 10: "min",
    11: "max", 12: "slt", 13: "sge", 14: "exp", 15: "log", 16: "lit", 17: "dst", 18: "lrp", 19: "frc", 20: "m4x4",
    21: "m4x3", 22: "m3x4", 23: "m3x3", 24: "m3x2", 25: "call", 26: "callnz", 27: "loop", 28: "ret", 29: "endloop",
    30: "label", 31: "dcl", 32: "pow", 33: "crs", 34: "sgn", 35: "abs", 36: "nrm", 37: "sincos", 38: "rep",
    39: "endrep", 40: "if", 41: "ifc", 42: "else", 43: "endif", 44: "break", 45: "breakc", 46: "mova", 47: "defb",
    48: "defi", 64: "texcoord", 65: "texkill", 66: "texld", 78: "expp", 79: "logp", 80: "cnd", 81: "def",
    88: "cmp", 89: "bem", 90: "dp2add", 91: "dsx", 92: "dsy", 93: "texldd", 94: "setp", 95: "texldl", 96: "breakp",
}
REG_PREFIX = {0: "r", 1: "v", 2: "c", 3: "a", 4: "oPos", 5: "oD", 6: "oT", 7: "i", 8: "oC", 9: "oDepth", 10: "s",
              11: "c", 12: "c", 13: "c", 14: "b", 15: "aL", 16: "r", 17: "vMisc", 18: "l", 19: "p"}
USAGES = {0: "position", 1: "blendweight", 2: "blendindices", 3: "normal", 4: "psize", 5: "texcoord", 6: "tangent",
          7: "binormal", 8: "tessfactor", 9: "positiont", 10: "color", 11: "fog", 12: "depth", 13: "sample"}
SRC_MOD = {0: "{}", 1: "-{}", 2: "{}_bias", 3: "-{}_bias", 4: "{}_bx2", 5: "-{}_bx2", 6: "1-{}", 7: "{}_x2", 8: "-{}_x2",
           9: "{}_dz", 10: "{}_dw", 11: "abs({})", 12: "-abs({})", 13: "!{}"}
CMP = {1: "gt", 2: "eq", 3: "ge", 4: "lt", 5: "ne", 6: "le"}


def reg_type(tok):
    return ((tok >> 28) & 7) | (((tok >> 11) & 3) << 3)


def reg_name(tok, names):
    t, n = reg_type(tok), tok & 0x7FF
    base = f"{REG_PREFIX.get(t, f'reg{t}_')}{n}" if t not in (4, 8, 9) or t == 8 else REG_PREFIX[t]
    if t == 4:
        base = ["oPos", "oFog", "oPts"][n] if n < 3 else f"oPos{n}"
    elif t == 9:
        base = "oDepth"
    elif t == 8:
        base = f"oC{n}"
    elif t == 17:
        base = ["vPos", "vFace"][n] if n < 2 else f"vMisc{n}"
    note = names.get((t, n))
    return base, note


def decode_dst(tok, names):
    base, note = reg_name(tok, names)
    mask = (tok >> 16) & 0xF
    m = "".join(c for i, c in enumerate("xyzw") if mask & (1 << i))
    suffix = "" if mask == 0xF else "." + m
    mods = (tok >> 20) & 0xF
    pre = ("_sat" if mods & 1 else "") + ("_pp" if mods & 2 else "") + ("_centroid" if mods & 4 else "")
    return base + suffix, pre, note


def decode_src(tok, names):
    base, note = reg_name(tok, names)
    sw = (tok >> 16) & 0xFF
    comps = "".join("xyzw"[(sw >> (2 * i)) & 3] for i in range(4))
    if comps == "xyzw":
        sw_s = ""
    else:
        while len(comps) > 1 and comps[-1] == comps[-2]:
            comps = comps[:-1]
        sw_s = "." + comps
    s = SRC_MOD.get((tok >> 24) & 0xF, "{}").format(base + sw_s)
    return s, note


def parse_ctab(words, start, length):
    """Devuelve {(tipo_registro, indice): nombre} desde un bloque CTAB (tras el fourcc)."""
    names = {}
    data = struct.pack("<%dI" % length, *words[start:start + length])
    if len(data) < 28 or data[:4] != b"CTAB":
        return names, []
    blob = data[4:]
    if len(blob) < 28:
        return names, []
    size, creator, version, count, cinfo, flags, target = struct.unpack_from("<7I", blob, 0)
    consts = []
    for i in range(count):
        if cinfo + 20 * (i + 1) > len(blob):
            break
        name_off, reg_set, reg_index, reg_count, _res, type_off, def_off = struct.unpack_from("<IHHHHII", blob, cinfo + 20 * i)
        end = blob.find(b"\0", name_off)
        nm = blob[name_off:end].decode("latin1") if end > name_off else "?"
        # RegisterSet: 0 bool, 1 int4, 2 float4, 3 sampler
        t = {0: 14, 1: 7, 2: 2, 3: 10}.get(reg_set, 2)
        for k in range(max(reg_count, 1)):
            names[(t, reg_index + k)] = nm if reg_count <= 1 else f"{nm}[{k}]"
        consts.append((nm, reg_set, reg_index, reg_count))
    return names, consts


def disassemble(words, i, j):
    """words[i..j] inclusive: version token ... END."""
    ver = VERSIONS[words[i]]
    names, consts = {}, []
    # primer pase: CTAB
    k = i + 1
    while k <= j:
        t = words[k]
        if (t & 0xFFFF) == 0xFFFE:
            ln = (t >> 16) & 0x7FFF
            if ln >= 2 and words[k + 1] == 0x42415443:
                names, consts = parse_ctab(words, k + 1, ln)
            k += 1 + ln
        else:
            k += 1 + ((t >> 24) & 0xF)
    out = [ver]
    if consts:
        out.append("// constantes: " + ", ".join(f"{n}(set{s} {'c' if s == 2 else 's' if s == 3 else 'r'}{r}+{c})" for n, s, r, c in consts))
    k = i + 1
    ninstr = ntex = 0
    while k <= j:
        t = words[k]
        op = t & 0xFFFF
        if op == 0xFFFE:
            k += 1 + ((t >> 16) & 0x7FFF)
            continue
        if t == 0x0000FFFF:
            out.append("// end")
            break
        ln = (t >> 24) & 0xF
        args = words[k + 1:k + 1 + ln]
        name = OPCODES.get(op, f"op{op}")
        ctrl = (t >> 16) & 0xFF
        text = None
        if op == 31 and len(args) >= 2:  # dcl
            d = args[0]
            dst, pre, _ = decode_dst(args[1], names)
            if reg_type(args[1]) == 10:
                kind = {2: "2d", 3: "cube", 4: "volume"}.get((d >> 27) & 0xF, "?")
                text = f"dcl_{kind} {dst}"
            else:
                us = USAGES.get(d & 0x1F, f"usage{d & 0x1F}")
                text = f"dcl_{us}{(d >> 16) & 0xF} {dst}{pre}"
        elif op == 81 and len(args) >= 5:  # def
            dst, _, _ = decode_dst(args[0], names)
            fl = ", ".join(("%g" % struct.unpack("<f", struct.pack("<I", a))[0]) for a in args[1:5])
            text = f"def {dst}, {fl}"
        elif op == 48 and len(args) >= 5:
            dst, _, _ = decode_dst(args[0], names)
            text = f"defi {dst}, " + ", ".join(str(struct.unpack("<i", struct.pack("<I", a))[0]) for a in args[1:5])
        elif op == 47 and len(args) >= 2:
            dst, _, _ = decode_dst(args[0], names)
            text = f"defb {dst}, {args[1]}"
        else:
            parts = []
            notes = []
            has_dst = op not in (25, 26, 27, 28, 29, 30, 38, 39, 40, 41, 42, 43, 44, 45, 96) and ln >= 1
            idx = 0
            pre = ""
            if has_dst and op != 0 and args:
                d, pre, note = decode_dst(args[0], names)
                parts.append(d)
                idx = 1
            for a in args[idx:]:
                if a & 0x80000000:
                    s, note = decode_src(a, names)
                    parts.append(s)
                    if note:
                        notes.append(note)
                else:
                    parts.append(f"{a:#x}")
            mn = name
            if op == 66:
                mn = {0: "texld", 1: "texldp", 2: "texldb"}.get(ctrl, "texld")
                ntex += 1
            if op in (41, 45):
                mn += "_" + CMP.get(ctrl & 7, "?")
            text = f"{mn}{pre} " + ", ".join(parts)
            if notes:
                text += "    // " + ", ".join(sorted(set(notes)))
        out.append(text.rstrip())
        ninstr += 1
        k += 1 + ln
    out.append(f"// {ninstr} instrucciones, {ntex} texld")
    return ver, ninstr, ntex, consts, "\n".join(out)


def scan(path):
    d = open(path, "rb").read()
    n = len(d) // 4
    words = struct.unpack("<%dI" % n, d[:n * 4])
    found = []
    i = 0
    while i < n:
        if words[i] in VERSIONS:
            j, count, ok = i + 1, 0, False
            while j < n and count < 8000:
                t = words[j]
                op = t & 0xFFFF
                if t == 0x0000FFFF:
                    ok = True
                    break
                if op == 0xFFFE:
                    ln = (t >> 16) & 0x7FFF
                    if ln > 4000 or j + 1 + ln > n:
                        break
                    j += 1 + ln
                else:
                    if 0x60 < op < 0xFFFE:
                        break
                    j += 1 + ((t >> 24) & 0xF)
                count += 1
            if ok and j - i > 3:
                found.append((i, j))
                i = j + 1
                continue
        i += 1
    return words, found


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    exe, outdir = sys.argv[1:3]
    os.makedirs(outdir, exist_ok=True)
    words, found = scan(exe)
    print(f"{len(found)} shaders")
    print(f"{'offset':>10} {'ver':6} {'bytes':>6} {'instr':>5} {'texld':>5}  constantes")
    for i, j in found:
        ver, ninstr, ntex, consts, text = disassemble(words, i, j)
        fn = os.path.join(outdir, f"{ver}_{i * 4:08x}.txt")
        open(fn, "w").write(text + "\n")
        print(f"{i * 4:#10x} {ver:6} {(j - i + 1) * 4:6d} {ninstr:5d} {ntex:5d}  " + ", ".join(c[0] for c in consts))


if __name__ == "__main__":
    main()
