"""Delete stale ppc_recomp.N.cpp files left over from earlier XenonRecomp runs (every function they define is
also defined in a newer file). usage: prune_stale.py <ppc dir> [obj dir]"""
import re, sys, pathlib, os
ppc = pathlib.Path(sys.argv[1]); obj = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else None
files = sorted(ppc.glob('ppc_recomp.*.cpp'), key=lambda p: p.stat().st_mtime, reverse=True)   # newest first
seen = set(); removed = 0
for f in files:
    fns = set(re.findall(r'PPC_FUNC_IMPL\(__imp__(\w+)\)', f.read_text()))
    if fns and fns <= seen:
        f.unlink(); removed += 1
        if obj and (obj / (f.stem + '.o')).exists(): (obj / (f.stem + '.o')).unlink()
        print('removed stale', f.name)
    seen |= fns
print(removed, 'stale files removed')
