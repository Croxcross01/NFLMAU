import struct, sys, re
img = open(sys.argv[1], 'rb').read(); base = int(sys.argv[2], 16)
e = struct.unpack('<I', img[0x3c:0x40])[0]
# Xbox 360 PE is big-endian-flavored in header? detect
nsec_le = struct.unpack('<H', img[e+6:e+8])[0]; nsec_be = struct.unpack('>H', img[e+6:e+8])[0]
be = nsec_be < nsec_le if (nsec_be and nsec_le > 64) else False
print('PE hdr at', hex(e), 'endianness', 'BE' if be else 'LE')
E = '>' if be else '<'
if be: e = struct.unpack('>I', img[0x3c:0x40])[0]
nsec = struct.unpack(E+'H', img[e+6:e+8])[0]; opt = struct.unpack(E+'H', img[e+20:e+22])[0]
so = e + 24 + opt; secs = []
for i in range(nsec):
    s = img[so+i*40:so+(i+1)*40]
    name = s[:8].split(b'\0')[0].decode(); vs, va, rs, ro = struct.unpack(E+'IIII', s[8:24]); ch = struct.unpack(E+'I', s[36:40])[0]
    secs.append((name, va, vs, ch)); print(f'  {name:8} va={base+va:#010x} size={vs:#09x} flags={ch:#010x}{"  CODE" if ch & 0x20 else ""}')
pats = {'restgprlr_14':'e9c1ff68','savegprlr_14':'f9c1ff68','restfpr_14':'c9ccff70','savefpr_14':'d9ccff70',
        'restvmx_14':'3960fee07dcb60ce','savevmx_14':'3960fee07dcb61ce','restvmx_64':'3960fc00100b60cb','savevmx_64':'3960fc00100b61cb'}
code = [(va, va+vs) for n, va, vs, ch in secs if ch & 0x20]
for k, hx in pats.items():
    b = bytes.fromhex(hx); hits = [m.start() for m in re.finditer(re.escape(b), img) if any(a <= m.start() < z for a, z in code) and m.start() % 4 == 0]
    print(f'{k:14} {[hex(base+h) for h in hits[:4]]}')
