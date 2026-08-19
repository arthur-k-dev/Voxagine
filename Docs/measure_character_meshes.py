import struct, glob, sys, collections

def read_vox(path):
    d = open(path,'rb').read()
    frames=[]; pal=None
    def walk(off, end, cur):
        nonlocal pal
        while off < end-12:
            cid = d[off:off+4]; cs, ks = struct.unpack('<ii', d[off+4:off+12]); off += 12
            if cid == b'SIZE':
                cur.append([struct.unpack('<iii', d[off:off+12]), None])
            elif cid == b'XYZI':
                n = struct.unpack('<i', d[off:off+4])[0]
                vox = d[off+4:off+4+n*4]
                cur[-1][1] = vox
            elif cid == b'RGBA':
                pal = d[off:off+1024]
            elif cid == b'MAIN':
                walk(off+cs, end, cur)
            off += cs + ks
    walk(8, len(d), frames)
    return [f for f in frames if f[1] is not None], pal

def greedy(path):
    frames, pal = read_vox(path)
    tot = dict(solid=0, faces=0, quads=0, frames=0)
    for (sx,sy,sz), vox in frames:
        occ = {}
        for i in range(0, len(vox), 4):
            x,y,z,ci = vox[i], vox[i+1], vox[i+2], vox[i+3]
            if ci == 0: continue
            occ[(x,y,z)] = ci
        tot['solid'] += len(occ); tot['frames'] += 1
        dims = (sx,sy,sz)
        for axis in range(3):
            u,v = (axis+1)%3, (axis+2)%3
            for sign in (-1, 1):
                for w in range(dims[axis]):
                    # 2D mask over (u,v)
                    mask = {}
                    for a in range(dims[u]):
                        for b in range(dims[v]):
                            p = [0,0,0]; p[axis]=w; p[u]=a; p[v]=b
                            c = occ.get(tuple(p))
                            if c is None: continue
                            q = list(p); q[axis] = w + sign
                            if tuple(q) in occ: continue
                            mask[(a,b)] = c
                    tot['faces'] += len(mask)
                    # greedy rectangles of equal colour
                    seen = set()
                    for a in range(dims[u]):
                        for b in range(dims[v]):
                            if (a,b) in seen or (a,b) not in mask: continue
                            c = mask[(a,b)]
                            wdt = 1
                            while (a+wdt, b) in mask and (a+wdt,b) not in seen and mask[(a+wdt,b)]==c: wdt += 1
                            hgt = 1
                            while True:
                                ok = all((a+k, b+hgt) in mask and (a+k,b+hgt) not in seen and mask[(a+k,b+hgt)]==c for k in range(wdt))
                                if not ok: break
                                hgt += 1
                            for k in range(wdt):
                                for l in range(hgt): seen.add((a+k, b+l))
                            tot['quads'] += 1
    return tot

files = sorted(glob.glob('Content/Character_Models/**/*.vox', recursive=True))
agg = collections.Counter(); rows=[]
for f in files:
    try: t = greedy(f)
    except Exception as e:
        print("skip", f, e); continue
    agg.update(t); rows.append((t['quads'], t['frames'], t['solid'], t['faces'], f))
rows.sort(reverse=True)
print(f"{len(files)} files, {agg['frames']} frames")
print(f"solid voxels {agg['solid']:,}   exposed faces {agg['faces']:,}   greedy quads {agg['quads']:,}")
print(f"merge ratio  faces/quads = {agg['faces']/max(agg['quads'],1):.2f}x")
print(f"per frame avg: {agg['solid']/agg['frames']:.0f} solid, {agg['faces']/agg['frames']:.0f} faces, {agg['quads']/agg['frames']:.0f} quads")
print()
print("heaviest models (quads, frames, solid, faces):")
for q,fr,s,fa,f in rows[:8]: print(f"  {q:>7,} {fr:>4} {s:>8,} {fa:>8,}  {f}")
