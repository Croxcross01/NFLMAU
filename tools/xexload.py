"""Decrypt/decompress an Xbox 360 XEX2 (retail key, basic/none compression) to a raw PE image. Port of XenonRecomp's Xex2LoadImage."""
import struct, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
RETAIL = bytes([0x20,0xB1,0x85,0xA5,0x9D,0x28,0xFD,0xC3,0x40,0x58,0x3F,0xBB,0x08,0x96,0xBF,0x91])
def cbc(key, data):
    return Cipher(algorithms.AES(key), modes.CBC(bytes(16))).decryptor().update(data)
def opt(d, key):
    cnt = struct.unpack('>I', d[0x14:0x18])[0]
    for i in range(cnt):
        k, v = struct.unpack('>II', d[0x18+i*8:0x20+i*8])
        if k == key: return v
    return None
def load(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'XEX2'
    hdr = struct.unpack('>I', d[8:12])[0]; sec = struct.unpack('>I', d[0x10:0x14])[0]
    imgsize = struct.unpack('>I', d[sec+4:sec+8])[0]
    aes = d[sec+0x150:sec+0x160]
    ff = opt(d, 0x3FF); info, enc, comp = struct.unpack('>IHH', d[ff:ff+8])
    body = d[hdr:]
    if enc == 1:
        k = cbc(RETAIL, aes); n = len(body) & ~15
        body = cbc(k, body[:n])
    if comp == 0: img = body[:imgsize]
    elif comp == 1:
        nb = info // 8 - 1; p = 0; img = bytearray()
        for i in range(nb):
            ds, zs = struct.unpack('>II', d[ff+8+i*8:ff+16+i*8])
            img += body[p:p+ds]; p += ds; img += bytes(zs)
        img = bytes(img)
    else: raise SystemExit('LZX compression not handled in this script')
    img = bytes(img).ljust(imgsize, b'\0')[:imgsize]   # trailing zero pages are not stored in the file
    base = opt(d, 0x10201); entry = opt(d, 0x10100)
    return img, base, entry
if __name__ == '__main__':
    img, base, entry = load(sys.argv[1]); open(sys.argv[2], 'wb').write(img)
    print('image', len(img), 'base', hex(base), 'entry', hex(entry), 'MZ' if img[:2]==b'MZ' else 'no MZ')
