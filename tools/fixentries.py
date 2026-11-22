"""Collect '// ERROR <addr>' branch targets (branches into the middle of another function) from XenonRecomp
output and add them to the config's entry_points list. usage: fixentries.py <ppc dir> <config.toml>
prints the number of new entry points added (0 = done)."""
import re, sys, pathlib
ppc, cfg = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
found = set()
for f in ppc.glob('ppc_recomp.*.cpp'):
    found |= {int(a, 16) for a in re.findall(r'// ERROR ([0-9A-F]{8})\b', f.read_text())}
t = cfg.read_text()
m = re.search(r'entry_points = \[(.*?)\]', t, re.S)
have = {int(a, 16) for a in re.findall(r'0x([0-9A-Fa-f]+)', m.group(1))} if m else set()
new = found - have
allp = sorted(have | found)
block = 'entry_points = [\n' + ''.join(f'    0x{a:08X},\n' for a in allp) + ']\n'
if m: t = t[:m.start()] + block.rstrip('\n') + t[m.end():]
else: t = t.replace('functions = [', block + 'functions = [', 1)
cfg.write_text(t)
print(len(new))
