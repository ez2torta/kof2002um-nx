"""Extracts an STFS package locating every data block by its SHA-1.

The KOF 2002 UM LIVE package in this project is missing one physical 4 KiB
block (data block 19917), so every block after it sits one block earlier than
the standard STFS arithmetic says. Regular extractors therefore read the
wrong block for most files. This extractor hashes all physical blocks, reads
each level-0 hash table and takes each data block from wherever its SHA-1
digest is found, closest to the expected position. Blocks whose data is not in
the package are reported and filled with zeros.

Usage: python -I kof_stfs_extract.py <package> <out_dir> [--report report.txt]
"""
import argparse
import hashlib
import mmap
import os
import struct
import sys

BLOCK = 0x1000
PER_TABLE = 0xAA
LEVEL_BASE = (PER_TABLE, PER_TABLE ** 2, PER_TABLE ** 3)


def u24le(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def u24be(b, o):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


class Stfs:
    def __init__(self, path):
        self.file = open(path, 'rb')
        self.m = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        hdr = self.m[:0xA000]
        if hdr[:4] not in (b'LIVE', b'PIRS', b'CON '):
            raise ValueError(f'not an STFS package (magic {hdr[:4]!r})')
        if hdr[0x3A9] != 0:
            raise ValueError('volume is SVOD, not STFS')
        header_size = struct.unpack_from('>I', hdr, 0x340)[0]
        vd = 0x379
        self.tables_per_level = 1 if hdr[vd + 2] & 1 else 2
        self.ft_blocks = struct.unpack_from('<H', hdr, vd + 3)[0]
        self.ft_start = u24le(hdr, vd + 5)
        self.alloc_blocks = struct.unpack_from('>I', hdr, vd + 0x1C)[0]
        self.data_base = (header_size + BLOCK - 1) & ~(BLOCK - 1)
        self.nphys = (len(self.m) - self.data_base) // BLOCK

        # SHA-1 of every physical block -> physical indices.
        self.by_hash = {}
        for ph in range(self.nphys):
            self.by_hash.setdefault(hashlib.sha1(self.raw(ph)).digest(), []).append(ph)

        self.phys_of = {}   # data block -> physical block (verified)
        self.next_of = {}   # data block -> next data block from its hash entry
        self.missing = []   # data blocks whose data is not in the package
        self._map_blocks()

    def raw(self, ph):
        o = self.data_base + ph * BLOCK
        return self.m[o:o + BLOCK]

    # Standard STFS data block -> physical block (Xenia's BlockToOffsetSTFS).
    def expected(self, block):
        out = block
        for base in LEVEL_BASE:
            out += ((block + base) // base) * self.tables_per_level
            if block < base:
                break
        return out

    def _map_blocks(self):
        for g in range((self.alloc_blocks + PER_TABLE - 1) // PER_TABLE):
            first = g * PER_TABLE
            n = min(PER_TABLE, self.alloc_blocks - first)
            # The level-0 table sits right before the group's first data block;
            # try the standard position and the ones shifted by missing blocks.
            best = None
            for shift in range(0, -4, -1):
                tpos = self.expected(first) - self.tables_per_level + shift
                if tpos < 0:
                    continue
                table = self.raw(tpos)
                hits = sum(1 for i in range(n) if table[i * 0x18:i * 0x18 + 0x14] in self.by_hash)
                if best is None or hits > best[0]:
                    best = (hits, table)
            table = best[1]
            for i in range(n):
                b = first + i
                entry = table[i * 0x18:i * 0x18 + 0x18]
                self.next_of[b] = u24be(entry, 0x15)
                cands = self.by_hash.get(entry[:0x14])
                if not cands:
                    self.missing.append(b)
                    continue
                exp = self.expected(b)
                self.phys_of[b] = min(cands, key=lambda c: abs(c - exp))

    def read_block(self, block):
        ph = self.phys_of.get(block)
        return self.raw(ph) if ph is not None else bytes(BLOCK)

    def chain(self, start, count, consecutive):
        block = start
        for _ in range(count):
            yield block
            block = block + 1 if consecutive else self.next_of[block]

    def entries(self):
        raw = b''.join(self.read_block(b) for b in self.chain(self.ft_start, self.ft_blocks, False))
        out = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            nlen = e[0x28] & 0x3F
            if nlen == 0:
                continue
            out.append({
                'index': i // 0x40,
                'name': e[:nlen].decode('ascii', 'replace'),
                'dir': bool(e[0x28] & 0x80),
                'consecutive': bool(e[0x28] & 0x40),
                'blocks': u24le(e, 0x29),
                'start': u24le(e, 0x2F),
                'parent': struct.unpack_from('>h', e, 0x32)[0],
                'size': struct.unpack_from('>I', e, 0x34)[0],
            })
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('package')
    ap.add_argument('out_dir')
    ap.add_argument('--report')
    a = ap.parse_args()

    p = Stfs(a.package)
    shifted = sum(1 for b, ph in p.phys_of.items() if ph != p.expected(b))
    lines = [f'data blocks {p.alloc_blocks}, physical blocks {p.nphys}',
             f'blocks found away from the standard position: {shifted}',
             f'blocks missing from the package: {p.missing}']

    ents = p.entries()
    by_index = {e['index']: e for e in ents}

    def path_of(e):
        parts = [e['name']]
        while e['parent'] != -1:
            e = by_index[e['parent']]
            parts.append(e['name'])
        return os.path.join(*reversed(parts))

    missing = set(p.missing)
    damaged = []
    total = 0
    for e in ents:
        dst = os.path.join(a.out_dir, path_of(e))
        if e['dir']:
            os.makedirs(dst, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        left = e['size']
        lost = []
        with open(dst, 'wb') as out:
            for b in p.chain(e['start'], e['blocks'], e['consecutive']):
                if left <= 0:
                    break
                if b in missing:
                    lost.append(b)
                data = p.read_block(b)[:min(BLOCK, left)]
                out.write(data)
                left -= len(data)
        if lost:
            damaged.append(f'{path_of(e)}: missing data blocks {lost}')
        total += e['size']

    lines.append(f'{len(ents)} entries, {total / 2**20:.1f} MiB written to {a.out_dir}')
    lines.append(f'files with missing blocks (zero-filled): {len(damaged)}')
    lines += ['  ' + d for d in damaged]
    print('\n'.join(lines))
    if a.report:
        with open(a.report, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
