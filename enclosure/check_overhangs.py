"""Find surfaces that would need support, in each STL's print orientation.

A face needs support if it faces down more steeply than 45 degrees and is not
on the bed. Flat down-facing faces are listed separately as bridges: they are
fine when both ends are anchored (a ceiling across a cavity), not when they
hang free. Each group is printed with its size and position for review.
"""
import os
import sys

import numpy as np
import trimesh

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stl")
LIMIT = np.sin(np.radians(46))   # steeper than 45 deg (+1 deg for rounding): needs support


def groups(mesh, mask):
    idx = np.where(mask)[0]
    if len(idx) == 0:
        return []
    sub = mesh.submesh([idx], append=True)
    parts = sub.split(only_watertight=False)
    out = []
    for p in parts:
        b = p.bounds
        out.append((p.area, b))
    return sorted(out, key=lambda g: -g[0])


def check(name, min_area=0.05):
    m = trimesh.load(os.path.join(OUT, f"openear_pod_{name}.stl"))
    n = m.face_normals
    zc = m.triangles_center[:, 2]
    zmin = m.triangles[:, :, 2].min(1)
    off_bed = zmin > 0.25
    flat = (n[:, 2] < -0.985) & off_bed
    steep = (n[:, 2] < -LIMIT) & (n[:, 2] >= -0.985) & off_bed
    print(f"\n{name}: {m.bounds[1] - m.bounds[0]} mm")
    sg = [g for g in groups(m, steep) if g[0] >= min_area]
    bg = [g for g in groups(m, flat) if g[0] >= min_area]
    print(f"  steeper than 45 deg (needs support): {len(sg)} groups, {sum(g[0] for g in sg):.1f} mm2")
    for a, b in sg[:12]:
        print(f"    {a:6.2f} mm2  x {b[0][0]:5.1f}..{b[1][0]:5.1f}  y {b[0][1]:5.1f}..{b[1][1]:5.1f}  z {b[0][2]:5.2f}..{b[1][2]:5.2f}")
    print(f"  flat down-facing (bridges): {len(bg)} groups, {sum(g[0] for g in bg):.1f} mm2")
    for a, b in bg[:12]:
        print(f"    {a:6.2f} mm2  x {b[0][0]:5.1f}..{b[1][0]:5.1f}  y {b[0][1]:5.1f}..{b[1][1]:5.1f}  z {b[0][2]:5.2f}")
    return sum(g[0] for g in sg)


if __name__ == "__main__":
    total = sum(check(p) for p in ("base", "lid", "clip"))
    print(f"\nTOTAL steep overhang: {total:.1f} mm2")
    sys.exit(0 if total < 1.0 else 1)
