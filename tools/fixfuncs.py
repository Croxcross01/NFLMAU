"""Compute explicit function boundaries for functions whose jump-table cases lie outside the auto-detected extent."""
import re, struct, bisect, sys, tomllib, collections
LOG, SW, MAP, IMG, OUTF = sys.argv[1:6]
BASE = 0x88000000
img = open(IMG, 'rb').read()
def W(a): return struct.unpack('>I', img[a-BASE:a-BASE+4])[0]
starts = sorted(int(m, 16) for m in re.findall(r'\{\s*0x([0-9A-Fa-f]{8})\s*,', open(MAP).read()))
tables = tomllib.loads(open(SW).read())['switch']
bad = collections.defaultdict(set)
for m in re.finditer(r'Switch case at ([0-9A-F]+) is trying to jump outside function: ([0-9A-F]+)', open(LOG).read()):
    bad[int(m.group(1), 16)].add(int(m.group(2), 16))
def terminator_end(a, limit=0x800):
    p = a
    for _ in range(limit // 4):
        w = W(p)
        if w in (0x4E800020, 0x4E800420) or ((w >> 26) == 18 and (w & 1) == 0): return p + 4
        p += 4
    return a + 4
funcs = {}
for bctr, labels in bad.items():
    t = next((t for t in tables if t['base'] <= bctr <= t['base'] + 12), None)
    if t is None: print('no table for bctr', hex(bctr)); continue
    lo = min(bctr, min(t['labels']))
    i = bisect.bisect_right(starts, lo) - 1
    fs = starts[i]
    end = max(terminator_end(max(t['labels'])), bctr + 4)
    prev = funcs.get(fs)
    funcs[fs] = max(prev or 0, end - fs)
with open(OUTF, 'w') as f:
    for a in sorted(funcs): f.write(f"    {{ address = 0x{a:X}, size = 0x{funcs[a]:X} }},\n")
print(len(bad), 'failing switches ->', len(funcs), 'explicit functions written to', OUTF)
