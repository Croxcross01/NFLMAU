import sys,os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glob, zlib, json, re, time, bgfa
from extract_ast import sniff
M=(1<<64)-1
def fnv(s):
    h=0xcbf29ce484222325
    for c in s.upper().encode('latin-1','replace'): h=((h^c)*0x100000001b3)&M
    return h.to_bytes(8,'little')
root=os.path.expanduser('~/mnt/Madden NFL Arcade (USA)/extracted_assets')
t0=time.time()
import pickle
names={}
cache=os.path.expanduser('~/work/names.pkl')
def add(s):
    if 2<=len(s)<=64: names.setdefault(fnv(s),s)
arcs=sorted(glob.glob(root+'/*/*.bgfa'))
if os.path.exists(cache): names=pickle.load(open(cache,'rb'))
else:
  for p in arcs:
    f,ents,meta=bgfa.parse(p); f.close()
    for e in ents:
        if e.name: add(e.name)
tok=re.compile(rb'[A-Za-z0-9_./\\-]{3,64}')
if not os.path.exists(cache):
  for p in glob.glob(root+'/*/*.txt')+glob.glob(root+'/*/*.xml'):
    for m in set(tok.findall(open(p,'rb').read())): add(m.decode('latin-1'))
  pickle.dump(names,open(cache,'wb'))
print('dictionary',len(names),'names',round(time.time()-t0,1),'s')
# 2) recursive unpack
total=files=named=resolved=0
queue=[os.path.join(d,f) for d,_,fs in os.walk(root) for f in fs if f.endswith('.bgfa') and not os.path.exists(os.path.join(d,f+'.d','_manifest.json'))]
print('pending',len(queue))
while queue:
    if time.time()-t0>140: print('time budget reached, pending',len(queue)); break
    p=queue.pop()
    f,ents,meta=bgfa.parse(p)
    od=p+'.d'; os.makedirs(od,exist_ok=True); man=[]
    for e in ents:
        f.seek(e.offset); data=f.read(e.size)
        if e.compressed: data=zlib.decompress(data)
        label=e.name
        if label: named+=1
        elif e.hash in names: label=names[e.hash]; resolved+=1
        else: label=e.hash.hex()
        label=re.sub(r'[^A-Za-z0-9_.\-]','_',label)[:80]
        ext=sniff(data)
        fn=f"{e.index:05d}_{label}.{ext}"
        fp=os.path.join(od,fn)
        open(fp,'wb').write(data)
        total+=len(data); files+=1
        man.append(dict(index=e.index,hash=e.hash.hex(),name=e.name or names.get(e.hash,''),flags=e.flags,size=len(data),type=ext,file=fn))
        if ext=='bgfa': queue.append(fp)
    f.close()
    json.dump(man,open(os.path.join(od,'_manifest.json'),'w'),indent=0)
print('files',files,'MB',round(total/1e6),'named-in-archive',named,'resolved-by-dictionary',resolved,'unnamed',files-named-resolved,round(time.time()-t0,1),'s')
