"""BGFA1.05 archive reader (reverse-engineered from Madden NFL Arcade PS3 .ast files).

Header (little-endian):
  0x00 'BGFA1.05' | 0x08 u32 checksum? | 0x0C u32 entry count | 0x10 u64 table offset | 0x18 u64 table size
  0x22 u8 hash width | 0x23..0x25 u8 widths of offset/size/delta fields | 0x26 u8 offset shift (bytes = value << shift)
  0x28 u32 N (table starts with N*4 bytes of unknown bucket data) | 0x2C u32 name field length (0 = no names)
Record: flags(1-2 bytes) | hash | offset | stored size | delta (unpacked = size + delta, only if compressed) | name (fixed width, NUL padded)
flags low bit = zlib-compressed.
"""
import struct
from dataclasses import dataclass

@dataclass
class Entry:
    index: int
    flags: int
    hash: bytes
    offset: int
    size: int
    delta: int
    name: str = ''
    @property
    def compressed(self): return bool(self.flags & 1)
    @property
    def usize(self): return self.size + self.delta if self.compressed else self.size

def le(b): return int.from_bytes(b, 'little')

def parse(path):
    f = open(path, 'rb')
    h = f.read(0x40)
    assert h[:8] == b'BGFA1.05', 'not a BGFA1.05 archive'
    cnt = struct.unpack('<I', h[0xC:0x10])[0]
    toff, tsz = struct.unpack('<QQ', h[0x10:0x20])
    hs = h[0x22]; w1, w2, w3, w4 = h[0x23], h[0x24], h[0x25], h[0x26]
    pre = 4 * struct.unpack('<I', h[0x28:0x2C])[0]
    namelen = struct.unpack('<I', h[0x2C:0x30])[0]
    if cnt == 0: return f, [], dict(count=0, widths=(hs, w1, w2, w3, w4), flagbytes=0, namelen=namelen, prefix=b"")
    f.seek(toff); t = f.read(tsz)
    assert cnt == 0 or (tsz - pre) % cnt == 0, ('table size', pre, cnt, tsz)
    rec = (tsz - pre) // cnt if cnt else 0
    fb = rec - hs - w1 - w2 - w3 - namelen
    assert fb in (1, 2), ('record layout', pre, cnt, rec, fb)
    ents = []
    for i in range(cnt):
        r = t[pre + i*rec: pre + (i+1)*rec]
        x = fb + hs
        o = le(r[x:x+w1]); x += w1
        s = le(r[x:x+w2]); x += w2
        d = le(r[x:x+w3]); x += w3
        name = r[x:x+namelen].split(b'\0')[0].decode('latin-1') if namelen else ''
        ents.append(Entry(i, le(r[:fb]), r[fb:fb+hs], o << w4, s, d, name))
    return f, ents, dict(count=cnt, widths=(hs, w1, w2, w3, w4), flagbytes=fb, namelen=namelen, prefix=t[:pre])
