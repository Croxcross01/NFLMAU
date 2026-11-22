"""List the variable (data) imports of a XEX: thunk slot address, library, ordinal, name.
usage: xeximports.py <xex> <decrypted image> <xenia src dir>"""
import struct, sys, re
x = open(sys.argv[1], 'rb').read(); img = open(sys.argv[2], 'rb').read(); xsrc = sys.argv[3]
B = 0x88000000
names = {}
for lib, f in (('xboxkrnl', 'kernel/xboxkrnl/xboxkrnl_table.inc'), ('xam', 'kernel/xam/xam_table.inc')):
    for m in re.finditer(r'XE_EXPORT\(\w+,\s*0x([0-9A-Fa-f]+),\s*(\w+),\s*(\w+)\)', open(f'{xsrc}/{f}').read()):
        names[(lib, int(m.group(1), 16))] = (m.group(2), m.group(3))
cnt = struct.unpack_from('>I', x, 0x14)[0]
hdr = {struct.unpack_from('>I', x, 0x18 + 8*i)[0]: struct.unpack_from('>I', x, 0x1C + 8*i)[0] for i in range(cnt)}
o = hdr[0x000103FF]
size, strsz, nlibs = struct.unpack_from('>III', x, o)
strs = x[o + 12:o + 12 + strsz].split(b'\0'); libnames = [s.decode() for s in strs if s]
p = o + 12 + strsz
for li in range(nlibs):
    lsize = struct.unpack_from('>I', x, p)[0]
    nameidx, count = struct.unpack_from('>HH', x, p + 0x24)
    lib = libnames[nameidx].split('.')[0]
    for k in range(count):
        a = struct.unpack_from('>I', x, p + 0x28 + 4*k)[0]
        v = struct.unpack_from('>I', img, a - B)[0]
        typ, ordv = v >> 24, v & 0xFFFF
        if typ == 0:
            n, kind = names.get((lib, ordv), ('?', '?'))
            print(f'{a:08X} {lib} {ordv:#05x} {n} ({kind})')
    p += lsize
