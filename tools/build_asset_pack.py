#!/usr/bin/env python3
"""Writes the asset pack of a game data folder on a PC: <folder>.nxpack + <folder>.nxidx.

Same bytes as PackDevice::Build in tools/rexglue-sdk/src/filesystem/devices/pack_device.cpp (the console does it on
first boot when there is no pack): magic SSNXPAK1 / SSNXIDX1, version 2, 16-byte aligned files, a sorted path index,
and 64-bit FNV-1a checksums. The file is read back and verified before the script exits.

Usage: python3 tools/build_asset_pack.py <game_folder> <out_dir>
  -> <out_dir>/<game_folder name>.nxpack and .nxidx (copy both next to the NRO, as the runtime expects).
"""
import os
import struct
import sys
import time

KFNV_BASIS = 1469598103934665603
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1
PACK_VERSION = 2
ALIGN = 16
CHUNK = 1 << 20
MAX_ENTRIES = 100000
MAX_PATHS = 64 << 20
FAT32_LIMIT = 4 << 30

PACK_HDR = struct.Struct("<8sIIQQQ")      # 40 bytes
INDEX_HDR = struct.Struct("<8sIIQQQQQ")   # 56 bytes
ENTRY = struct.Struct("<QQII")            # 24 bytes


def fnv(h, data):
    # FNV-1a 64 sobre bytes; pure python es lento, así que va por bloques con la misma recurrencia.
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def fnv_fast(h, data):
    # Igual que fnv(), pero con memoryview y sin atributos en el bucle (archivos de cientos de MB).
    mv = memoryview(data)
    for b in mv:
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def round_up(x, a):
    return (x + a - 1) // a * a


def collect(folder):
    items = []
    for root, _dirs, files in os.walk(folder):
        for name in files:
            full = os.path.join(root, name)
            if not os.path.isfile(full):
                continue
            rel = os.path.relpath(full, folder).replace(os.sep, "/")
            if (not rel or rel.startswith("/") or rel.endswith("/") or "\\" in rel or ":" in rel
                    or any(p in ("", ".", "..") for p in rel.split("/"))):
                raise SystemExit(f"ruta no válida para el pack: {rel!r}")
            items.append({"path": rel, "source": full, "size": os.path.getsize(full)})
    if not items or len(items) > MAX_ENTRIES:
        raise SystemExit("la carpeta no tiene archivos o tiene demasiados")
    # std::string::operator< compara bytes; en ASCII/UTF-8 coincide con bytes.
    items.sort(key=lambda it: it["path"].encode("utf-8"))
    return items


def build(folder, out_dir):
    folder = os.path.abspath(folder)
    name = os.path.basename(folder.rstrip("/"))
    items = collect(folder)

    paths_size = 0
    total = 0
    pack_size = PACK_HDR.size
    pack_id = KFNV_BASIS
    for it in items:
        pack_size = round_up(pack_size, ALIGN)
        it["offset"] = pack_size
        pack_size += it["size"]
        pb = it["path"].encode("utf-8")
        it["path_bytes"] = pb
        paths_size += len(pb) + 1
        total += it["size"]
        pack_id = fnv(pack_id, pb)
        pack_id = fnv(pack_id, struct.pack("<Q", it["size"]))
    if paths_size > MAX_PATHS:
        raise SystemExit("las rutas son demasiado largas")
    if pack_size >= FAT32_LIMIT:
        raise SystemExit(f"el pack pesa {pack_size >> 20} MB: pasa el límite de FAT32 (4 GB)")
    nonce = ((int(time.time()) << 32) ^ time.monotonic_ns()) & MASK64
    pack_id = fnv(pack_id, struct.pack("<Q", nonce)) or 1

    os.makedirs(out_dir, exist_ok=True)
    pack_path = os.path.join(out_dir, name + ".nxpack")
    idx_path = os.path.join(out_dir, name + ".nxidx")

    data_checksum = KFNV_BASIS
    written = 0
    with open(pack_path, "wb") as out:
        out.write(PACK_HDR.pack(b"SSNXPAK1", PACK_VERSION, 0, pack_id, pack_size, 0))
        position = PACK_HDR.size
        for it in items:
            if it["offset"] > position:
                out.write(b"\0" * (it["offset"] - position))
            position = it["offset"]
            with open(it["source"], "rb") as f:
                remaining = it["size"]
                while remaining:
                    piece = f.read(min(remaining, CHUNK))
                    if not piece:
                        raise SystemExit(f"el archivo cambió mientras se empaquetaba: {it['path']}")
                    out.write(piece)
                    data_checksum = fnv_fast(data_checksum, piece)
                    remaining -= len(piece)
                    position += len(piece)
                    written += len(piece)
                    print(f"\rempaquetando {written * 100 // max(total, 1):3d} %", end="", flush=True)
        # El tamaño final es el del último archivo (no se redondea al final), igual que el builder del SDK.
        assert position == pack_size, (position, pack_size)
        out.seek(0)
        out.write(PACK_HDR.pack(b"SSNXPAK1", PACK_VERSION, 0, pack_id, pack_size, data_checksum))
    print()

    entries = bytearray()
    paths = bytearray()
    for it in items:
        entries += ENTRY.pack(it["offset"], it["size"], len(paths), len(it["path_bytes"]))
        paths += it["path_bytes"] + b"\0"
    checksum = fnv(fnv(KFNV_BASIS, bytes(entries)), bytes(paths))
    with open(idx_path, "wb") as out:
        out.write(INDEX_HDR.pack(b"SSNXIDX1", PACK_VERSION, len(items), pack_id, pack_size, len(paths),
                                 checksum, data_checksum))
        out.write(entries)
        out.write(paths)

    verify(pack_path, idx_path, items, data_checksum)
    print(f"listo: {pack_path} ({pack_size} bytes), {idx_path}; {len(items)} archivos")


def verify(pack_path, idx_path, items, data_checksum):
    """Lee el índice y todos los bytes de vuelta, igual que verify_pack_data del SDK."""
    with open(idx_path, "rb") as f:
        raw = f.read()
    magic, version, count, pid, psize, psz, chk, dchk = INDEX_HDR.unpack_from(raw, 0)
    assert magic == b"SSNXIDX1" and version == PACK_VERSION and count == len(items), "índice inválido"
    body = raw[INDEX_HDR.size:]
    ent = body[:count * ENTRY.size]
    pth = body[count * ENTRY.size:]
    assert fnv(fnv(KFNV_BASIS, ent), pth) == chk, "checksum del índice no coincide"
    h = KFNV_BASIS
    with open(pack_path, "rb") as f:
        for i in range(count):
            off, size, po, pl = ENTRY.unpack_from(ent, i * ENTRY.size)
            f.seek(off)
            left = size
            while left:
                piece = f.read(min(left, CHUNK))
                assert piece, "el pack terminó antes de tiempo"
                h = fnv_fast(h, piece)
                left -= len(piece)
    assert h == data_checksum == dchk, "checksum de datos no coincide"
    print("verificado: índice y datos coinciden")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    build(sys.argv[1], sys.argv[2])
