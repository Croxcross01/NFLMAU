"""Minimal STFS (Xbox 360 LIVE/PIRS/CON) reader/extractor."""
import struct, os, sys
BLK = 0x1000
class STFS:
    def __init__(self, path):
        self.f = open(path, 'rb'); h = self.f.read(0x400)
        self.magic = h[:4]; assert self.magic in (b'LIVE', b'PIRS', b'CON '), self.magic
        hs = struct.unpack('>I', h[0x340:0x344])[0]
        self.base = (hs + 0xFFF) & 0xFFFFF000
        self.title_id = struct.unpack('>I', h[0x360:0x364])[0]
        self.ctype = struct.unpack('>I', h[0x344:0x348])[0]
        self.sep = 2 if (h[0x37B] & 1) == 0 else 1   # bit0 set -> read-only/"female": single hash table
        self.ft_count = struct.unpack('<H', h[0x37C:0x37E])[0]
        self.ft_start = int.from_bytes(h[0x37E:0x381], 'little')
        self.total_alloc = struct.unpack('>I', h[0x395:0x399])[0]
        self.title = h[0x411:0x411+0x80].decode('utf-16-be', 'ignore').split('\0')[0]
    def fb(self, n):  # data block number -> file block index
        s = self.sep
        r = n + (n // 0xAA + 1) * s
        if n >= 0xAA: r += (n // 0x70E4 + 1) * s
        if n >= 0x70E4: r += s
        return r
    def read_block(self, n):
        self.f.seek(self.base + self.fb(n) * BLK); return self.f.read(BLK)
    def next_block(self, n):
        g = n // 0xAA
        tbl_fb = self.fb(g * 0xAA) - self.sep
        self.f.seek(self.base + tbl_fb * BLK + (n % 0xAA) * 0x18 + 0x15)
        return int.from_bytes(self.f.read(3), 'big')
    def entries(self):
        out = []
        for i in range(self.ft_count):
            blk = self.read_block(self.ft_start + i)
            for j in range(64):
                e = blk[j*0x40:(j+1)*0x40]
                if e[0] == 0: return out
                fl = e[0x28]; nl = fl & 0x3f
                out.append(dict(idx=len(out), name=e[:nl].decode('latin-1'), consecutive=bool(fl & 0x40), isdir=bool(fl & 0x80),
                    blocks=int.from_bytes(e[0x29:0x2c], 'little'), start=int.from_bytes(e[0x2f:0x32], 'little'),
                    parent=struct.unpack('>h', e[0x32:0x34])[0], size=struct.unpack('>I', e[0x34:0x38])[0]))
        return out
    def read_file(self, e):
        rem = e['size']; n = e['start']; out = bytearray(); k = 0
        while rem > 0 and k < e['blocks'] + 1:
            b = self.read_block(n); out += b[:min(rem, BLK)]; rem -= BLK; k += 1
            n = n + 1 if e['consecutive'] else self.next_block(n)
        return bytes(out[:e['size']])
    def path(self, ents, e):
        p = [e['name']]; q = e['parent']
        while q != -1 and q != 0xFFFF and q < len(ents) and len(p) < 32: p.append(ents[q]['name']); q = ents[q]['parent']
        return '/'.join(reversed(p))
if __name__ == '__main__':
    s = STFS(sys.argv[1])
    print(s.magic, hex(s.title_id), hex(s.ctype), repr(s.title), 'sep', s.sep, 'ft', s.ft_count, s.ft_start, 'alloc', s.total_alloc)
    ents = s.entries(); print(len(ents), 'entries')
    for e in ents[:15]: print(s.path(ents, e), e['size'], 'dir' if e['isdir'] else '', 'consec' if e['consecutive'] else 'chain')
