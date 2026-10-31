"""Exact relocation with correct PE mapping: known-build bytes at a VA -> target build."""
import json, os, re, sys, glob
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE

KNOWN = r"D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag\AC4BFSP.exe"
SRC = r"C:\Users\Administrator\Documents\Default Project\projects\assassins-creed\bf-coop\ref\AC4Coop\src"
WINDOWS = [40, 32, 24, 20, 16, 12]

def addrs():
    pats=[re.compile(r'base\(\)\s*\+\s*0x([0-9A-Fa-f]+)'), re.compile(r'\b0x([0-9A-Fa-f]{6,8})\b')]
    out={}
    for p in glob.glob(os.path.join(SRC,'*.cpp'))+glob.glob(os.path.join(SRC,'*.h')):
        t=open(p,encoding='utf-8',errors='replace').read()
        for pat in pats:
            for m in pat.finditer(t):
                v=int(m.group(1),16)
                if v < 0x400000: v += 0x400000
                if 0x404000 <= v < 0x3000000: out.setdefault(v, os.path.basename(p))
    return out

def main(argv):
    target=None
    for i,a in enumerate(argv):
        if a=='--target' and i+1<len(argv): target=argv[i+1]
    kp, tp = PE(KNOWN), PE(target)
    rows=[]
    for va in sorted(addrs()):
        off=kp.va_to_off(va)
        if off is None: rows.append({'known':va,'target':None,'n':0,'w':0}); continue
        hit=None
        for w in WINDOWS:
            needle=kp.data[off:off+w]
            if len(needle)<w: break
            pos=[]; s=0
            while True:
                i=tp.data.find(needle,s)
                if i<0: break
                pos.append(i); s=i+1
                if len(pos)>6: break
            if len(pos)==1: hit=(w,tp.off_to_va(pos[0]),1); break
            if len(pos)>1:  hit=(w,tp.off_to_va(pos[0]),len(pos)); break
        rows.append({'known':va,'target':hit[1] if hit else None,'n':hit[2] if hit else 0,'w':hit[0] if hit else 0})
    uniq=[r for r in rows if r['target'] and r['n']==1]
    print(f"located {len([r for r in rows if r['target']])}/{len(rows)}  unique {len(uniq)}")
    for r in rows:
        t=f"{r['target']:#x}" if r['target'] else 'MISSING'
        print(f"  {r['known']:#9x} -> {t:>10}  n={r['n']} w={r['w']}")
    json.dump({'known':KNOWN,'target':target,'rows':rows}, open(r'..\..\ops\evidence\sig_map_v3.json','w'), indent=1)
    return 0

if __name__=='__main__': sys.exit(main(sys.argv))
