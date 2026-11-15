"""Compare the 40 car render records per frame between the original game
(tools/oracle/log_cars40.txt, from probe.dll) and our build (DAYTONA_CARDUMP=1
-> game/daytona_log.txt).  Frames are aligned on the player car state."""
import struct, sys
def load(path):
    fr=[]
    for l in open(path, errors='replace'):
        if ' cars' not in l: continue
        hexes=[p.split('=')[-1] for p in l.split() if len(p.split('=')[-1])==32]
        if len(hexes)==40: fr.append([struct.unpack('<3i2h',bytes.fromhex(h)) for h in hexes])
    return fr
O=load(sys.argv[1] if len(sys.argv) > 1 else 'tools/oracle/log_cars40.txt')
R=load(sys.argv[2] if len(sys.argv) > 2 else 'game/daytona_log.txt')
print('frames', len(O), len(R))
key=lambda f: f[39]
Ridx={}
for i,f in enumerate(R):
    if f[39][0]: Ridx.setdefault(key(f), i)
first={}
matched=0
for i,f in enumerate(O):
    if not f[39][0]: continue
    j=Ridx.get(key(f))
    if j is None: continue
    matched+=1
    g=R[j]
    for n in range(39):
        if f[n]!=g[n] and n not in first:
            first[n]=(i,j,f[n],g[n])
print('matched frames', matched)
for n in sorted(first, key=lambda n: first[n][0]):
    i,j,a,b=first[n]
    print('car %2d first diff at O#%d R#%d\n   O %s\n   R %s' % (n,i,j,a,b))
