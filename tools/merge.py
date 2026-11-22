import re,sys
cfg=open(sys.argv[1]).read()
e=sorted((int(a,16),int(s,16)) for a,s in re.findall(r'address\s*=\s*0x([0-9A-Fa-f]+),\s*size\s*=\s*0x([0-9A-Fa-f]+)',cfg))
m=[]
for a,s in e:
    if m and a<m[-1][0]+m[-1][1]:
        m[-1][1]=max(m[-1][1],a+s-m[-1][0])
    else: m.append([a,s])
head=cfg[:cfg.index('functions')];tail=cfg[cfg.rindex(']')+1:]
open(sys.argv[1],'w').write(head+'functions = [\n'+',\n'.join(f'    {{ address = 0x{a:X}, size = 0x{s:X} }}' for a,s in m)+'\n]'+tail)
print(len(e),'->',len(m))
