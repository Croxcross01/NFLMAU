"""Write a copy of a BGFA1.05 archive with some entries replaced (for mods).

The original archive is never modified. Replaced entries are zlib-compressed (if the original entry was) and appended
at the end of the copy, aligned to the archive's offset granularity, and their table records are updated to point at
the new data. Everything else in the copy is byte-identical to the original.

Usage (library): patch(src_path, dst_path, {entry_index: new_bytes, ...})
"""
import os, shutil, struct, zlib
import bgfa


def _put_le(buf, at, width, value):
    if value >> (8 * width):
        raise ValueError(f'value {value:#x} does not fit in {width} bytes')
    buf[at:at + width] = value.to_bytes(width, 'little')


def patch(src, dst, replacements):
    f, ents, meta = bgfa.parse(src)
    f.close()
    with open(src, 'rb') as fh:
        h = fh.read(0x40)
    toff, tsz = struct.unpack('<QQ', h[0x10:0x20])
    hs, w1, w2, w3, shift = meta['widths']
    fb, namelen, pre = meta['flagbytes'], meta['namelen'], len(meta['prefix'])
    rec = (tsz - pre) // meta['count']
    tmp = dst + '.tmp'
    shutil.copyfile(src, tmp)
    with open(tmp, 'r+b') as out:
        out.seek(toff)
        table = bytearray(out.read(tsz))
        out.seek(0, os.SEEK_END)
        for idx, data in sorted(replacements.items()):
            e = ents[idx]
            stored = zlib.compress(data, 9) if e.compressed else data
            pos = out.tell()
            align = 1 << shift
            pad = (-pos) % align
            out.write(b'\0' * pad)
            pos += pad
            out.write(stored)
            r = pre + idx * rec
            x = r + fb + hs
            _put_le(table, x, w1, pos >> shift)
            _put_le(table, x + w1, w2, len(stored))
            if e.compressed:
                _put_le(table, x + w1 + w2, w3, len(data) - len(stored))
        out.seek(toff)
        out.write(table)
    os.replace(tmp, dst)


def read_entry(src, idx):
    f, ents, meta = bgfa.parse(src)
    e = ents[idx]
    f.seek(e.offset)
    d = f.read(e.size)
    f.close()
    return zlib.decompress(d) if e.compressed else d
