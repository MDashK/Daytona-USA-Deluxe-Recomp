"""Compare polygons drawn per frame between two runs of our build (attract
demo, DAYTONA_CARDUMP=1 DAYTONA_D3DSTATS=1), frames aligned on the player car
record so the same moment of the race is compared."""
import sys
def load(path):
    out = {}
    for l in open(path, errors='replace'):
        if ' cars draws=' not in l: continue
        p = l.split()
        d = int(p[2].split('=')[1]); v = int(p[3].split('=')[1])
        key = p[-1]
        if v and key not in out: out[key] = (d, v)
    return out
A, B = load(sys.argv[1]), load(sys.argv[2])
common = [k for k in A if k in B]
if not common: print('no aligned frames'); sys.exit()
da = sum(A[k][1] for k in common) / len(common); db = sum(B[k][1] for k in common) / len(common)
more = sum(1 for k in common if B[k][1] > A[k][1]); less = sum(1 for k in common if B[k][1] < A[k][1])
print('aligned frames %d: verts/frame %.0f -> %.0f (%+.1f%%), frames with more %d, fewer %d'
      % (len(common), da, db, (db - da) * 100 / da, more, less))
