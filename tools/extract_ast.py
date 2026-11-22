import sys,os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zlib, json, time, bgfa

def sniff(b):
    h = b[:16]
    for m, ext in ((b'MVhd','mvhd'),(b'RIFF','riff'),(b'OggS','ogg'),(b'\x89PNG','png'),(b'DDS ','dds'),(b'GIF8','gif'),
                   (b'BIGF','big'),(b'BGFA','bgfa'),(b'SCE\0','self'),(b'\x7fELF','elf'),(b'PK\x03\x04','zip'),
                   (b'<?xml','xml'),(b'\xff\xd8\xff','jpg'),(b'EAAC','eaac'),(b'SHPS','shps'),(b'p3R\x02','p3r')):
        if h.startswith(m): return ext
    if len(b) and sum(32 <= c < 127 or c in (9,10,13) for c in b[:256]) >= 0.97 * min(len(b),256):
        return 'txt'
    return 'bin'

def main(src, outroot):
    f, ents, meta = bgfa.parse(src)
    name = os.path.splitext(os.path.basename(src))[0]
    od = os.path.join(outroot, name); os.makedirs(od, exist_ok=True)
    man = []; t0 = time.time(); total = 0
    for e in ents:
        f.seek(e.offset); data = f.read(e.size)
        comp = bool(e.flags & 1)
        if comp:
            data = zlib.decompress(data)
            assert len(data) == e.size + e.usize
        ext = sniff(data)
        fn = f"{e.index:05d}_{e.hash.hex()}.{ext}"
        with open(os.path.join(od, fn), 'wb') as w: w.write(data)
        total += len(data)
        man.append(dict(index=e.index, hash=e.hash.hex(), flags=e.flags, category=e.flags >> 8,
                        compressed=comp, stored_size=e.size, size=len(data), type=ext, file=fn))
    json.dump(dict(archive=os.path.basename(src), count=len(man), entries=man), open(os.path.join(od, '_manifest.json'), 'w'), indent=1)
    print(f"{name}: {len(man)} files, {total/1e6:.1f} MB unpacked in {time.time()-t0:.1f}s")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
