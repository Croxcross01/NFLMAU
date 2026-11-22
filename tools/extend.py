import re,sys,struct,bisect,collections
LOG,MAP,IMG,CFG,OUT=sys.argv[1:6]
B=0x88000000
img=open(IMG,'rb').read()
W=lambda a:struct.unpack('>I',img[a-B:a-B+4])[0]
cfg=open(CFG).read()
ents={int(a,16):int(s,16) for a,s in re.findall(r'address\s*=\s*0x([0-9A-Fa-f]+),\s*size\s*=\s*0x([0-9A-Fa-f]+)',cfg)}
starts=sorted(int(m,16) for m in re.findall(r'\{\s*0x([0-9A-Fa-f]{8})\s*,',open(MAP).read()))
mx=collections.defaultdict(int)
for m in re.finditer(r'Switch case at ([0-9A-F]+) is trying to jump outside function: ([0-9A-F]+)',open(LOG).read()):
    b=int(m.group(1),16);l=int(m.group(2),16)
    fs=starts[bisect.bisect_right(starts,b)-1]
    mx[fs]=max(mx[fs],l)
for fs,l in mx.items():
    p=l
    while W(p)!=0x4E800020: p+=4
    ents[fs]=max(ents.get(fs,0),p+4-fs)
    print(hex(fs),'->',hex(ents[fs]))
def sub(m):
    return m.group(0)
# rewrite config: drop all function entries, re-add
head=cfg[:cfg.index('functions')]
tail=cfg[cfg.rindex(']')+1:]
body='functions = [\n'+',\n'.join(f'    {{ address = 0x{a:X}, size = 0x{s:X} }}' for a,s in sorted(ents.items()))+'\n]'
open(OUT,'w').write(head+body+tail)
