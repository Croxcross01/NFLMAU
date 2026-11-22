"""Find PowerPC jump tables (lwzx/mtctr/bctr idiom) in a decoded Xbox 360 image and emit XenonRecomp switch-table TOML."""
import struct, sys, collections
IMG, BASE = sys.argv[1], int(sys.argv[2], 16)
CODE_LO, CODE_HI = int(sys.argv[3], 16), int(sys.argv[4], 16)
OUT = sys.argv[5]
img = open(IMG, 'rb').read()
def W(a): return struct.unpack('>I', img[a-BASE:a-BASE+4])[0]
def sx16(v): return v - 0x10000 if v & 0x8000 else v
BCTR, STATS = 0x4E800420, collections.Counter()

def resolve_const(a, reg, limit=40):
    """Walk backwards from a to find `lis reg,hi` + (`addi`/`ori` reg,reg,lo) that builds an address into reg."""
    lo = None; p = a - 4
    for _ in range(limit):
        w = W(p); op = w >> 26; rd = (w >> 21) & 31; ra = (w >> 16) & 31; imm = w & 0xFFFF
        if rd == reg and op in (14, 24) and ra == reg and lo is None:      # addi / ori reg,reg,lo
            lo = sx16(imm) if op == 14 else imm
        elif rd == reg and op == 15 and ra == 0:                            # lis reg,hi
            hi = sx16(imm) << 16
            return ((hi + (lo or 0)) & 0xFFFFFFFF) if True else None
        elif op == 14 and rd == reg and ra != reg and lo is None:           # addi reg,rA,.. (not a plain const)
            return None
        elif op == 15 and rd == reg and ra != 0:                            # addis reg,rA,hi (not plain)
            return None
        p -= 4
    return None

def find_back(a, pred, limit=24):
    p = a - 4
    for _ in range(limit):
        w = W(p)
        if pred(w): return p, w
        if w in (0x4E800020, BCTR): break    # stop at blr / earlier bctr
        p -= 4
    return None, None

def offset_table(a, pm, rX):
    """Offset-table switch: lbzx/lhzx rO,rT,rI ; [slwi rO,rO,k] ; lis/addi rB=code base ; add rX,rB,rO ; mtctr rX ; bctr"""
    pa, wa = find_back(pm, lambda w: (w >> 26) == 31 and ((w >> 1) & 0x1FF) == 266 and ((w >> 21) & 31) == rX, 4)   # add
    if pa is None: return None
    r1, r2 = (wa >> 16) & 31, (wa >> 11) & 31
    for rB, rO in ((r1, r2), (r2, r1)):
        base = resolve_const(pa, rB)
        if base is None or not (CODE_LO <= base < CODE_HI): continue
        # walk back from the add: either the lbzx/lhzx loads rO directly, or rO = loaded value << k (scaling after the load)
        isload = lambda w, r: (w >> 26) == 31 and ((w >> 1) & 0x3FF) in (87, 279) and ((w >> 21) & 31) == r
        shift = 0; rLoad = rO; pl = wl = None; p = pa - 4
        for _ in range(12):
            w = W(p)
            if isload(w, rLoad): pl, wl = p, w; break
            if shift == 0 and rLoad == rO and (w >> 26) == 21 and ((w >> 16) & 31) == rO and ((w >> 6) & 31) == 0 \
               and ((w >> 1) & 31) == 31 - ((w >> 11) & 31):
                shift = (w >> 11) & 31; rLoad = (w >> 21) & 31
            p -= 4
        if pl is None: continue
        width = 1 if ((wl >> 1) & 0x3FF) == 87 else 2
        ra, rb = (wl >> 16) & 31, (wl >> 11) & 31
        for rT, rI in ((ra, rb), (rb, ra)):
            tbl = resolve_const(pl, rT)
            if tbl is None or not (BASE <= tbl < BASE + len(img)): continue
            idx = rI
            if width == 2:   # halfword table index is pre-scaled (slwi rI,rIdx,1)
                q = pl - 4
                for _ in range(8):
                    w = W(q)
                    if (w >> 26) == 21 and ((w >> 16) & 31) == rI and ((w >> 11) & 31) == 1 and ((w >> 1) & 31) == 30:
                        idx = (w >> 21) & 31; break
                    q -= 4
            pc, wc = find_back(pl, lambda w: (w >> 26) == 10 and ((w >> 16) & 31) == idx, 64)
            if pc is None:   # bound checked on a register that was copied into idx (mr idx,rS)
                pm2, wm2 = find_back(pl, lambda w: (w >> 26) == 31 and ((w >> 1) & 0x3FF) == 444 and ((w >> 16) & 31) == idx
                                     and ((w >> 21) & 31) == ((w >> 11) & 31), 64)
                if pm2 is not None:
                    src = (wm2 >> 21) & 31
                    pc, wc = find_back(pl, lambda w: (w >> 26) == 10 and ((w >> 16) & 31) in (idx, src), 64)
            if pc is None: return None
            n = (wc & 0xFFFF) + 1
            offs = [img[tbl - BASE + i] if width == 1 else struct.unpack('>H', img[tbl - BASE + 2*i: tbl - BASE + 2*i + 2])[0] for i in range(n)]
            labels = [base + (o << shift) for o in offs]
            if not all(CODE_LO <= l < CODE_HI and l % 4 == 0 for l in labels): return None
            return dict(bctr=a, key=pm, r=idx, table=tbl, n=n, labels=labels)
    return None

results = []; unresolved = []
for a in range(CODE_LO, CODE_HI, 4):
    if W(a) != BCTR: continue
    STATS['bctr'] += 1
    # mtctr rX
    pm, wm = find_back(a, lambda w: (w & 0xFC1FFFFF) == 0x7C0903A6, 3)
    if pm is None: STATS['no_mtctr'] += 1; continue
    rX = (wm >> 21) & 31
    # lwzx rX, rA, rB   (op31, xo 23)
    pl, wl = find_back(pm, lambda w: (w >> 26) == 31 and ((w >> 1) & 0x3FF) == 23 and ((w >> 21) & 31) == rX, 6)
    if pl is None:
        r = offset_table(a, pm, rX)
        if r: STATS['offset_table'] += 1; results.append(r); continue
        STATS['no_lwzx'] += 1; unresolved.append((a, 'no lwzx')); continue
    rA, rB = (wl >> 16) & 31, (wl >> 11) & 31
    tbl = None; idxreg_scaled = None
    for cand, other in ((rA, rB), (rB, rA)):
        v = resolve_const(pl, cand)
        if v is not None and BASE <= v < BASE + len(img): tbl, idxreg_scaled = v, other; break
    if tbl is None: STATS['no_table_base'] += 1; unresolved.append((a, 'no table base')); continue
    # unscaled index register: rlwinm rS_scaled, rI, 2, 0, 29  (slwi 2)  or mulli/addi-like; else same reg
    ps, ws = find_back(pl, lambda w: (w >> 26) == 21 and ((w >> 16) & 31) == 2 and (w >> 21) & 31 != 99 and ((w >> 11) & 31) == 2 or
                       ((w >> 26) == 21 and ((w >> 11) & 31) == 2 and ((w >> 6) & 31) == 0 and ((w >> 1) & 31) == 29 and ((w >> 16) & 31) == idxreg_scaled), 10)
    # rlwinm encoding: op21 | rS<<21 | rA<<16 | SH<<11 | MB<<6 | ME<<1 ; scaled result is rA(dest)
    ps = None
    p = pl - 4
    for _ in range(10):
        w = W(p)
        if (w >> 26) == 21 and ((w >> 16) & 31) == idxreg_scaled and ((w >> 11) & 31) == 2 and ((w >> 6) & 31) == 0 and ((w >> 1) & 31) == 29:
            ps, ws = p, w; break
        p -= 4
    rI = ((ws >> 21) & 31) if ps else idxreg_scaled
    # bound: cmplwi crN, rI, N  (op 10) searched before the lwzx
    pc, wc = find_back(pl, lambda w: (w >> 26) == 10 and ((w >> 16) & 31) == rI, 64)
    if pc is None:
        pm2, wm2 = find_back(pl, lambda w: (w >> 26) == 31 and ((w >> 1) & 0x3FF) == 444 and ((w >> 16) & 31) == rI
                             and ((w >> 21) & 31) == ((w >> 11) & 31), 64)
        if pm2 is not None:
            src = (wm2 >> 21) & 31
            pc, wc = find_back(pl, lambda w: (w >> 26) == 10 and ((w >> 16) & 31) in (rI, src), 64)
    if pc is not None:
        n = (wc & 0xFFFF) + 1
    else:
        # no bound check found: count consecutive entries that look like code addresses near the bctr
        n = 0
        while n < 512:
            l = W(tbl + 4*n)
            if not (CODE_LO <= l < CODE_HI and l % 4 == 0 and abs(l - a) < 0x4000): break
            n += 1
        if n < 2: STATS['no_bound'] += 1; unresolved.append((a, f'no cmplwi bound (table {tbl:#x}, idx r{rI})')); continue
        STATS['bound_by_scan'] += 1
    labels = [W(tbl + 4*i) for i in range(n)]
    ok = all(CODE_LO <= l < CODE_HI and l % 4 == 0 and abs(l - a) < 0x20000 for l in labels)
    if not ok: STATS['bad_labels'] += 1; unresolved.append((a, f'implausible labels (table {tbl:#x}, n={n})')); continue
    STATS['ok'] += 1
    results.append(dict(bctr=a, key=pm, r=rI, table=tbl, n=n, labels=labels))

print(dict(STATS))
with open(OUT, 'w') as f:
    f.write('# Generated by findswitch.py (custom jump-table detector)\n')
    for r in results:
        f.write(f"[[switch]]\nbase = 0x{r['key']:08X}\nr = {r['r']}\ndefault = 0x{r['labels'][-1]:08X}\nlabels = [\n")
        for l in r['labels']: f.write(f"    0x{l:08X},\n")
        f.write("]\n\n")
with open(OUT + '.unresolved.txt', 'w') as f:
    for a, why in unresolved: f.write(f'{a:#x}: {why}\n')
print('tables written:', len(results), '| unresolved bctr:', len(unresolved))
