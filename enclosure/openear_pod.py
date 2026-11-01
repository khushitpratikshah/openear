"""
OpenEar "pod": a display case for the MAX32655FTHR with a pocket clip.

Geometry from the real board (PCB Studio scan of the FTHR, 29 Oct 2026):
board 69.6 x 23.9 mm, mounting holes, buttons, mic, jacks and micro-USB
measured from a calibrated top photo (about +-0.2 mm). Heights of parts
above/below the PCB are estimates (3.5 mm jack ~5 mm, JST ~6 mm).

Parts (print PLA/PETG, 0.2 mm layers):
    openear_pod_base.stl   open side up, no supports
    openear_pod_lid.stl    as exported: rim on the bed, no supports (the
                           25 mm ceiling bridges). For a white logo, pause and
                           swap filament at the first layer above the lid top.
    openear_pod_clip.stl   lying on its side, no supports; glue its two pegs
                           into the holes on the back of the base

Coordinates: x along the board (0 = jack end), y across (0 = button edge),
z up (0 = back of the case). Licence: MIT.

    pip install manifold3d trimesh numpy matplotlib
    python3 openear_pod.py        # STLs + renders into ./stl
"""
import os

import numpy as np
import manifold3d
from manifold3d import CrossSection, FillRule, JoinType, Manifold, OpType
from matplotlib.font_manager import FontProperties
from matplotlib.textpath import TextPath
import trimesh

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stl")

# ---------------------------------------------------------------- board ---
PCB_L, PCB_W, PCB_T = 69.6, 23.9, 1.6
FIT = 0.5                     # clearance all round
HOLES = [(18.8, 2.6), (18.8, 21.1), (66.4, 3.2), (66.4, 21.4)]   # M2.5 pads
BUTTONS = {                   # name: (x, y) centre of the actuator
    "SW3": (6.0, 2.2), "SW2": (12.9, 2.2), "SW5 reset": (26.9, 5.2),
    "SW4 wake": (37.6, 5.2), "SW1 power": (55.9, 4.3), "SW6": (58.3, 17.9),
}
MIC = (7.6, 6.2)              # MK1 sound port
USB = dict(y=12.1, w=11.0, h=6.5, z_above=1.5)      # right end, micro-B
JACK_TOP = dict(y=13.7, d=7.2, z_above=2.6)         # headphone, left end
JACK_BOT = dict(y=5.5, d=7.2, z_below=2.6)          # line-in, under the PCB
JST = dict(x=60.8, w=7.5, h=4.5)                    # battery lead slot, y=0 wall

# ------------------------------------------------------------- case ------
WALL, FLOOR, LID_T = 2.0, 2.0, 2.2
STANDOFF_H = 5.5              # room for the line-in jack and microSD below
TOP_CLEAR = 6.6               # tallest part on top (JST / jack) + margin
R_PLAN = 6.0                  # corner radius seen from above
R_EDGE = 3.0                  # fillet on the top and bottom edges
SEG = 64

IN_L, IN_W = PCB_L + 2 * FIT, PCB_W + 2 * FIT
OUT_L, OUT_W = IN_L + 2 * WALL, IN_W + 2 * WALL
PCB_X0, PCB_Y0 = WALL + FIT, WALL + FIT             # board origin in case coords
PCB_Z0 = FLOOR + STANDOFF_H                          # PCB underside
PCB_Z1 = PCB_Z0 + PCB_T                              # PCB top
CAV_Z1 = PCB_Z1 + TOP_CLEAR                          # lid underside
H = CAV_Z1 + LID_T                                   # case height
SPLIT = PCB_Z1 + JACK_TOP["z_above"]                 # through jack/USB centres


def rrect(l, w, r, x0=0.0, y0=0.0):
    r = min(r, l / 2 - 0.01, w / 2 - 0.01)
    cs = CrossSection.square((l - 2 * r, w - 2 * r)).translate((r, r))
    return cs.offset(r, JoinType.Round, circular_segments=SEG).translate((x0, y0))


def pcb(x, y):
    """board coordinates -> case coordinates"""
    return PCB_X0 + x, PCB_Y0 + y


def pod_solid(l, w, h, r_plan, r_edge, chamfer=2.0, steps=10):
    """rounded box: plan corners r_plan, top edges filleted r_edge, bottom
    edges chamfered 45 deg (a fillet on the bed edge would need support)"""
    def slab(inset, z):
        cs = rrect(l - 2 * inset, w - 2 * inset, max(r_plan - inset, 0.5), inset, inset)
        return cs.extrude(0.01).translate((0, 0, min(max(z - 0.005, 0), h - 0.01)))
    slabs = [slab(chamfer, 0.0), slab(0.0, chamfer)]
    for i in range(steps + 1):
        a = (np.pi / 2) * i / steps
        slabs.append(slab(r_edge * (1 - np.sin(a)), h - r_edge * (1 - np.cos(a))))
    return Manifold.batch_hull(slabs)


def text_cs(s, height, weight="bold", family="Inter"):
    fp = FontProperties(family=family, weight=weight)
    tp = TextPath((0, 0), s, size=height, prop=fp)
    polys = [p for p in tp.to_polygons() if len(p) >= 3]
    cs = CrossSection([np.asarray(p, dtype=float) for p in polys], FillRule.EvenOdd)
    b = cs.bounds()
    return cs.translate((-(b[0] + b[2]) / 2, -(b[1] + b[3]) / 2))


def arc_cs(cx, cy, r, width, a0, a1, n=48):
    a = np.radians(np.linspace(a0, a1, n))
    outer = [(cx + (r + width / 2) * np.cos(t), cy + (r + width / 2) * np.sin(t)) for t in a]
    inner = [(cx + (r - width / 2) * np.cos(t), cy + (r - width / 2) * np.sin(t)) for t in a[::-1]]
    arc = CrossSection([np.asarray(outer + inner)])
    caps = [CrossSection.circle(width / 2, 24).translate((cx + r * np.cos(t), cy + r * np.sin(t)))
            for t in (a[0], a[-1])]
    return CrossSection.batch_boolean([arc] + caps, OpType.Add)



def along_x(cs, length, x, y, z):
    """extrude a profile drawn in (a, b) = (-z, y) along +x, centred at (y, z)"""
    return cs.extrude(length).rotate((0, 90, 0)).translate((x, y, z))


def teardrop_x(d, length, x, y, z):
    """horizontal hole with a 45 deg roof, flat-topped just above the circle,
    so it prints without support"""
    r = d / 2
    circ = CrossSection.circle(r, SEG)
    tip = r * np.sqrt(2)
    roof = CrossSection([np.array([(-tip, 0.0), (0.0, -tip), (0.0, tip)])])   # a = -z: tip towards +z
    shape = (circ + roof)
    keep = CrossSection.square((r + 0.3 + r, 2 * tip)).translate((-(r + 0.3), -tip))
    return along_x(shape ^ keep, length, x, y, z)


def octo_x(w, h, c, length, x, y, z):
    """horizontal rectangular hole w (y) x h (z) with 45 deg corner chamfers c"""
    a, b = h / 2, w / 2
    pts = [(-a + c, -b), (a - c, -b), (a, -b + c), (a, b - c), (a - c, b), (-a + c, b), (-a, b - c), (-a, -b + c)]
    return along_x(CrossSection([np.array(pts)]), length, x, y, z)


def cyl_x(d, length, x, y, z):
    return Manifold.cylinder(length, d / 2, d / 2, SEG).rotate((0, 90, 0)).translate((x, y, z))


def box(x0, y0, z0, x1, y1, z1):
    return Manifold.cube((x1 - x0, y1 - y0, z1 - z0)).translate((x0, y0, z0))


def _rbox_x(w, h, r, x0, length, yc, zc):
    # extrude along z, then rotate so the extrusion runs along +x
    cs = rrect(h, w, r, -h / 2, -w / 2)          # (z, y) profile
    m = cs.extrude(length)                          # along +z
    m = m.rotate((0, 90, 0))                        # z -> x (x' = z), x -> -z
    return m.translate((x0, yc, zc))


# ------------------------------------------------------------- build -----
def build():
    body = pod_solid(OUT_L, OUT_W, H, R_PLAN, R_EDGE)
    cav = rrect(IN_L, IN_W, R_PLAN - WALL, WALL, WALL).extrude(CAV_Z1 - FLOOR).translate((0, 0, FLOOR))
    body = body - cav

    cuts = []
    # left end: headphone (above PCB) and line-in (below PCB)
    for jk, zc in ((JACK_TOP, PCB_Z1 + JACK_TOP["z_above"]), (JACK_BOT, PCB_Z0 - JACK_BOT["z_below"])):
        cuts.append(teardrop_x(jk["d"], WALL + 2, -1, PCB_Y0 + jk["y"], zc))
    # right end: micro-USB plug
    cuts.append(octo_x(USB["w"], USB["h"], 1.5, WALL + 2, OUT_L - WALL - 1, PCB_Y0 + USB["y"],
                       PCB_Z1 + USB["z_above"]))
    # battery lead slot in the y = 0 wall, at PCB-top level
    jx = PCB_X0 + JST["x"]
    cuts.append(box(jx - JST["w"] / 2, -1, PCB_Z1, jx + JST["w"] / 2, WALL + 1, PCB_Z1 + JST["h"]))
    # lid: buttons (chamfered), mic grille
    for (bx, by) in BUTTONS.values():
        x, y = pcb(bx, by)
        cuts.append(Manifold.cylinder(LID_T + 2, 2.1, 2.1, SEG).translate((x, y, CAV_Z1 - 1)))
        cuts.append(Manifold.cylinder(0.8, 2.1, 2.9, SEG).translate((x, y, H - 0.8)))
    mx, my = pcb(*MIC)
    grille = [(0, 0)] + [(1.25 * np.cos(a), 1.25 * np.sin(a)) for a in np.radians(np.arange(0, 360, 60))]
    for gx, gy in grille:
        cuts.append(Manifold.cylinder(LID_T + 2, 0.42, 0.42, 6).translate((mx + gx, my + gy, CAV_Z1 - 1)))
    # accent grooves along the top
    for gy in (OUT_W - 6.2, OUT_W - 4.8):
        g = rrect(30.0, 0.55, 0.27, 24.0, gy - 0.27).extrude(0.45).translate((0, 0, H - 0.45))
        cuts.append(g)
    openings = Manifold.batch_boolean(cuts[:4], OpType.Add)   # jacks, USB, JST
    body = body - Manifold.batch_boolean(cuts, OpType.Add)

    # raised wordmark and sound-wave arcs on the lid
    logo = text_cs("OpenEar", 7.4).translate((PCB_X0 + 39.5, PCB_Y0 + 12.4))
    waves = [arc_cs(mx, my, r, 0.75, 22, 78) for r in (3.6, 5.2, 6.8)]
    emb = CrossSection.batch_boolean([logo] + waves, OpType.Add).extrude(0.8).translate((0, 0, H))
    body = body + emb

    # standoffs with locating pins
    posts = []
    for hx, hy in HOLES:
        x, y = pcb(hx, hy)
        posts.append(Manifold.cylinder(STANDOFF_H, 2.6, 2.6, SEG).translate((x, y, FLOOR - 0.01)))
        posts.append(Manifold.cylinder(PCB_T + 0.6, 0.8, 0.7, SEG).translate((x, y, PCB_Z0)))
    body = body + Manifold.batch_boolean(posts, OpType.Add)

    # peg holes for the clip (jack end of the back) and a debossed label
    clip_root_x = 6.0
    pegs = [(clip_root_x + 4.5, OUT_W / 2 - 3.5), (clip_root_x + 4.5, OUT_W / 2 + 3.5)]
    holes = [Manifold.cylinder(1.8, 0.98, 0.98, 32).translate((px, py, -0.1)) for px, py in pegs]  # 1.75 mm dowel, 1.7 deep (floor is 2.0)
    # label on the free end of the back (mirrored: it is read from below)
    for txt, size, dy in (("OpenEar", 4.2, 3.0), ("MAX32655FTHR  2026", 2.0, -3.2)):
        lab = text_cs(txt, size, weight="bold" if size > 3 else "semibold").mirror((1, 0))
        holes.append(lab.translate((61.0, OUT_W / 2 + dy)).extrude(0.5).translate((0, 0, -0.01)))
    body = body - Manifold.batch_boolean(holes, OpType.Add)

    # split into base and lid. The inner half of the base wall rises 2.4 mm
    # as a lip; the lid has the matching rabbet, so the lid prints right way
    # up: rim on the bed, ceiling bridged, logo last (swap filament there).
    big = box(-5, -5, -5, OUT_L + 5, OUT_W + 5, SPLIT)
    base = body ^ big
    lid = body - big
    half = WALL / 2
    ring = lambda a, b: (rrect(OUT_L - 2 * a, OUT_W - 2 * a, R_PLAN - a, a, a)
                         - rrect(OUT_L - 2 * b, OUT_W - 2 * b, R_PLAN - b, b, b))
    lip = ring(half + 0.15, WALL - 0.02).extrude(2.4).translate((0, 0, SPLIT - 0.01))  # flush with the inner wall
    base = base + (lip - openings)
    a0 = half - 0.05
    rab = rrect(OUT_L - 2 * a0, OUT_W - 2 * a0, R_PLAN - a0, a0, a0)
    roof = Manifold.batch_hull([
        rab.extrude(0.01).translate((0, 0, SPLIT + 2.5)),
        rrect(OUT_L - 2 * WALL, OUT_W - 2 * WALL, R_PLAN - WALL, WALL, WALL).extrude(0.01).translate((0, 0, SPLIT + 2.5 + 1.25)),
    ])
    lid = lid - (rab.extrude(2.6).translate((0, 0, SPLIT - 0.1)) + roof)

    # reveal chamfer round the outside at the seam (hides the split line);
    # 0.5 mm in over 0.7 mm up, so the base prints it without support
    frust = Manifold.batch_hull([
        rrect(OUT_L, OUT_W, R_PLAN, 0, 0).extrude(0.01).translate((0, 0, SPLIT - 0.7)),
        rrect(OUT_L - 1.0, OUT_W - 1.0, R_PLAN - 0.5, 0.5, 0.5).extrude(0.01).translate((0, 0, SPLIT - 0.01)),
    ])
    band = box(-2, -2, SPLIT - 0.7, OUT_L + 2, OUT_W + 2, SPLIT)
    base = base - (band - frust)

    clip = build_clip().translate((clip_root_x, OUT_W / 2, 0))   # in place on the back
    return base, lid, clip, pegs


def build_clip():
    """pen-style clip, profile in x-z, extruded along y; z = 0 is the case back"""
    gap, t, length, width = 1.8, 2.0, 38.0, 12.0
    root = 9.0
    prof = [
        (0, 0), (root, 0), (root, gap), (length, gap - 0.6),       # arm preloaded 0.6 mm
        (length + 1.2, gap - 0.3 + t / 2), (length, gap - 0.6 + t),
        (root + 1.5, gap + t), (0, gap + t),
    ]
    cs = CrossSection([np.asarray(prof, dtype=float)])
    m = cs.extrude(width).rotate((90, 0, 0)).translate((0, width / 2, 0))
    m = m.mirror((0, 0, 1))                                          # hang below z = 0
    bump = Manifold.cylinder(width, 1.1, 1.1, 32).rotate((90, 0, 0)).translate((length - 2.0, width / 2, -1.5))  # 0.4 mm short of the case
    m = m + bump
    # 45 deg corner chamfers seen from above (they stand vertical in print,
    # where rounds would overhang)
    L, hw, c = length + 2.5, width / 2, 3.0
    plan = CrossSection([np.array([(-0.5, -hw + 1.0), (0.5, -hw), (L - c, -hw), (L, -hw + c),
                                   (L, hw - c), (L - c, hw), (0.5, hw), (-0.5, hw - 1.0)])])
    m = m ^ plan.extrude(12).translate((0, 0, -10))
    # dowel holes (1.75 mm filament offcuts glue clip and case together)
    # teardrop pointing +y: that is "up" when the clip lies on its side to print
    r = 0.98
    tip = r * np.sqrt(2)
    tear = (CrossSection.circle(r, 32) + CrossSection([np.array([(-tip, 0.0), (tip, 0.0), (0.0, tip)])])) \
        ^ CrossSection.square((2 * tip, 2 * r + 0.25)).translate((-tip, -r))
    dow = [tear.extrude(3.2).translate((4.5, yy, -3.2)) for yy in (-3.5, 3.5)]
    return m - Manifold.batch_boolean(dow, OpType.Add)


def to_mesh(m):
    mg = m.to_mesh()
    return trimesh.Trimesh(vertices=np.asarray(mg.vert_properties)[:, :3], faces=np.asarray(mg.tri_verts))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    base, lid, clip, pegs = build()
    clip_print = clip.rotate((90, 0, 0))                 # profile flat on the bed
    clip_print = clip_print.translate((0, 0, -clip_print.bounding_box()[2]))
    for name, m in (("base", base), ("lid", lid.translate((0, 0, -SPLIT))), ("clip", clip_print)):
        tm = to_mesh(m)
        assert tm.is_watertight, name
        tm.export(os.path.join(OUT, f"openear_pod_{name}.stl"))
        print(f"{name}: {tm.bounds[1] - tm.bounds[0]} mm, {len(tm.faces)} faces, watertight")
    print(f"case {OUT_L:.1f} x {OUT_W:.1f} x {H:.1f} mm (+0.8 logo), split at z = {SPLIT:.1f}")
