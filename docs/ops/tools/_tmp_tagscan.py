import re, glob, os, collections

ROOT = 'C:/Users/Administrator/Documents/Default Project/projects/assassins-creed/bf-coop'
files = sorted(glob.glob(ROOT + '/**/phase1_log_*.txt', recursive=True))
files = [f for f in files if '/archive/' not in f]
for f in files:
    txt = open(f, 'r', errors='replace').read()
    lines = txt.splitlines()
    tags = collections.Counter()
    crashes = []
    net_lines = []
    for ln in lines:
        m = re.match(r'^\[([A-Za-z0-9_]+)\]', ln)
        if m:
            tags[m.group(1)] += 1
        if any(k in ln for k in ('0xC0000005', 'EXCEPTION', '0x0067B2EA', '0x67B2EA')):
            crashes.append(ln.strip())
        if ln.startswith('[Net]') or ln.startswith('[Build]') or ln.startswith('[GKT]') or ln.startswith('[KT]'):
            net_lines.append(ln.strip())
    print(f, 'lines=%d tags=%d crashes=%d' % (len(lines), len(tags), len(crashes)))
    print('   top:', ', '.join('%s=%d' % (k, v) for k, v in tags.most_common(25)))
    for c in crashes[:8]:
        print('   CRASH:', c[:180])
    for l in net_lines[:60]:
        print('   ', l[:180])
    print()
