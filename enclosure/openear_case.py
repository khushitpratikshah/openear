"""
OpenEar body-worn enclosure for the MAX32655FTHR + a 20x30 mm LiPo.

Parametric: every opening is a number in CONFIG below. ADI's datasheet does
not publish connector coordinates, so the defaults are best guesses.
MEASURE YOUR BOARD WITH CALIPERS FIRST (see the checklist in README.md),
edit CONFIG, then run:

    pip install manifold3d trimesh numpy
    python3 openear_case.py            # writes STLs + preview.png into ./stl

Prints (PLA/PETG, 0.2 mm layers, no supports):
    openear_tray.stl       body, open side up
    openear_lid.stl        lid, flat face down
    openear_fitcheck.stl   1.2 mm lid template - print this FIRST (10 min),
                           lay it on the board, check every hole lines up.

Stack (bottom -> top): floor | battery bay | clearance for parts under the
board (microSD, line-in jack) | PCB resting on two edge ledges | clearance
for parts on top (headset jack, mic, buttons) | lid. The lid has two ribs
that press the PCB edges onto the ledges, so no screws or holes in the PCB
are needed.

Coordinates: x = along the board (0 = USB end), y = across, z = up.
Licence: MIT.
"""
import os

import numpy as np
from manifold3d import CrossSection, Manifold
import trimesh

# --------------------------------------------------------------------------
# CONFIG - all mm
# --------------------------------------------------------------------------
CONFIG = dict(
    # board (0.9 x 2.6 in Feather footprint, from the ADI datasheet)
    board_l=66.04,
    board_w=22.86,
    board_t=1.6,
    fit=0.4,                 # clearance around the PCB
    # vertical budget
    wall=1.8,
    floor=1.6,
    lid_t=1.6,
    battery_h=6.5,           # bay for a 6 mm LiPo (602030 / 702030 fits)
    under_clear=6.0,         # tallest part on the PCB underside (line-in jack)
    over_clear=6.5,          # tallest part on top (headset jack)
    ledge=1.0,               # how far each edge ledge reaches under the PCB
    corner_r=3.0,
    # USB on the x = 0 end wall: (offset from board centre-line y, z of the
    # connector centre relative to PCB top, width, height)
    usb=dict(y=0.0, z_above_pcb=1.6, w=10.0, h=5.5),
    # 3.5 mm jacks. face: 'end_far' (x = max), 'side_left' (y = min),
    # 'side_right' (y = max), 'end_usb' (x = 0). 'along' = position along that
    # face measured from the board's left/USB corner. 'z' relative to PCB:
    # positive = above the top face, negative = below the underside.
    # Hole is big enough for a plug's moulded body (8 mm).
    jacks=[
        dict(name="headset", face="end_far", along=11.4, z=3.0, d=8.0),
        dict(name="line-in", face="end_far", along=11.4, z=-3.6, d=8.0),
    ],
    # microSD access slot (underside of PCB). Off by default: its position
    # is unknown and it would merge with the jack holes. Measure, then enable.
    sd=dict(enabled=False, face="end_far", along=11.4, w=13.0, h=2.6, z=-1.5),
    # Holes in the lid, (x, y) from the board's USB/left corner.
    mic=dict(x=40.0, y=11.4, holes=7, pitch=1.6, d=1.0),
    buttons=[  # pushbutton access, a pin or finger presses through
        dict(name="SW2 A/B", x=24.0, y=4.0, d=4.0),
        dict(name="SW3 vol", x=32.0, y=4.0, d=4.0),
        dict(name="SW1 power", x=24.0, y=18.8, d=4.0),
        dict(name="SW5 reset", x=50.0, y=4.0, d=1.6),  # paper-clip hole
    ],
    leds=[dict(name="RGB", x=16.0, y=18.8, d=2.2)],
    # wearing it
    clip_slot=dict(enabled=True, w=26.0, gap=3.2, bridge=6.0),  # badge clip / lanyard
    lid_lip=1.6,             # friction lip depth
    lid_lip_gap=0.25,
)


def rounded_box(lx, ly, lz, r):
    """Box with rounded vertical edges, corner at origin."""
    r = min(r, lx / 2 - 0.01, ly / 2 - 0.01)
    if r <= 0:
        return Manifold.cube((lx, ly, lz))
    return _round_rect(lx, ly, r).extrude(lz)


def _round_rect(lx, ly, r, segs=12):
    pts = []
    for cx, cy, a0 in [(lx - r, r, -90), (lx - r, ly - r, 0), (r, ly - r, 90), (r, r, 180)]:
        for k in range(segs + 1):
            a = np.radians(a0 + 90 * k / segs)
            pts.append((cx + r * np.cos(a), cy + r * np.sin(a)))
    return CrossSection([pts])


def cyl_x(d, length):
    return Manifold.cylinder(length, d / 2, d / 2, 48).rotate((0, 90, 0))


def cyl_y(d, length):
    return Manifold.cylinder(length, d / 2, d / 2, 48).rotate((-90, 0, 0))


def build(c):
    w, fl = c["wall"], c["floor"]
    in_l = c["board_l"] + 2 * c["fit"]
    in_w = c["board_w"] + 2 * c["fit"]
    pcb_z = fl + c["battery_h"] + c["under_clear"]          # PCB underside
    pcb_top = pcb_z + c["board_t"]
    tray_h = pcb_top + c["over_clear"]
    out_l, out_w = in_l + 2 * w, in_w + 2 * w
    # board origin (USB/left corner of the PCB) in case coordinates
    bx, by = w + c["fit"], w + c["fit"]

    tray = rounded_box(out_l, out_w, tray_h, c["corner_r"])
    tray = tray - Manifold.cube((in_l, in_w, tray_h)).translate((w, w, fl))

    # PCB edge ledges along both long walls
    for y in (w, w + in_w - c["ledge"] - c["fit"]):
        tray = tray + Manifold.cube((in_l, c["ledge"] + c["fit"], 1.6)).translate(
            (w, y, pcb_z - 1.6))

    big = 20.0
    # USB on the x=0 end
    u = c["usb"]
    tray = tray - Manifold.cube((big, u["w"], u["h"])).translate(
        (-big / 2, w + in_w / 2 + u["y"] - u["w"] / 2, pcb_top + u["z_above_pcb"] - u["h"] / 2))

    def face_cut(tray, face, along, zc, solid_x, solid_y):
        if face in ("end_far", "end_usb"):
            x0 = out_l - w - 1 if face == "end_far" else -1
            m = solid_x.translate((x0, by + along, zc))
        else:
            y0 = -1 if face == "side_left" else out_w - w - 1
            m = solid_y.translate((bx + along, y0, zc))
        return tray - m

    for j in c["jacks"]:
        zc = pcb_top + j["z"] if j["z"] >= 0 else pcb_z + j["z"]
        tray = face_cut(tray, j["face"], j["along"], zc,
                        cyl_x(j["d"], w + 2), cyl_y(j["d"], w + 2))
    s = c["sd"]
    if s["enabled"]:
        zc = pcb_z + s["z"]
        tray = face_cut(tray, s["face"], s["along"], zc,
                        Manifold.cube((w + 2, s["w"], s["h"])).translate((0, -s["w"] / 2, -s["h"] / 2)),
                        Manifold.cube((s["w"], w + 2, s["h"])).translate((-s["w"] / 2, 0, -s["h"] / 2)))

    # badge-clip / lanyard loop under the floor: two bridges over a channel
    cs = c["clip_slot"]
    if cs["enabled"]:
        loop = Manifold.cube((cs["bridge"], out_w - 2 * c["corner_r"], cs["gap"] + 1.6))
        hole = Manifold.cube((cs["bridge"] + 2, out_w - 2 * c["corner_r"] - 4, cs["gap"]))
        for x in (out_l * 0.3, out_l * 0.7 - cs["bridge"]):
            part = loop.translate((x, c["corner_r"], -(cs["gap"] + 1.6)))
            part = part - hole.translate((x - 1, c["corner_r"] + 2, -(cs["gap"] + 1.6)))
            tray = tray + part

    # ---------------- lid ----------------
    lid = rounded_box(out_l, out_w, c["lid_t"], c["corner_r"])
    g = c["lid_lip_gap"]
    lip = Manifold.cube((in_l - 2 * g, in_w - 2 * g, c["lid_lip"]))
    lip = lip - Manifold.cube((in_l - 2 * g - 2.4, in_w - 2 * g - 2.4, c["lid_lip"] + 1)).translate((1.2, 1.2, -0.5))
    lid = lid + lip.translate((w + g, w + g, -c["lid_lip"]))
    # pressing ribs over the PCB edges (lid sits on the tray rim)
    rib_h = c["over_clear"] - 0.2
    for y in (w + c["fit"], w + in_w - c["fit"] - 0.8):
        lid = lid + Manifold.cube((in_l - 16, 0.8, rib_h)).translate((w + 8, y, -rib_h))

    holes = []
    m = c["mic"]
    for k in range(m["holes"]):
        a = 2 * np.pi * k / max(1, m["holes"] - 1)
        r = 0 if k == 0 else m["pitch"]
        holes.append((m["x"] + r * np.cos(a), m["y"] + r * np.sin(a), m["d"]))
    for b in c["buttons"] + c["leds"]:
        holes.append((b["x"], b["y"], b["d"]))

    def drill(part, t):
        for x, y, d in holes:
            part = part - Manifold.cylinder(t + 40, d / 2, d / 2, 32).translate((bx + x, by + y, -20))
        return part

    lid = drill(lid, c["lid_t"])

    fit = rounded_box(out_l, out_w, 1.2, c["corner_r"])
    fit = drill(fit, 1.2)
    # outline of the PCB as a raised 0.6 mm frame so you can align it
    frame = Manifold.cube((c["board_l"] + 1.2, c["board_w"] + 1.2, 0.6)) - \
        Manifold.cube((c["board_l"], c["board_w"], 2)).translate((0.6, 0.6, -0.5))
    fit = fit + frame.translate((bx - 0.6, by - 0.6, 1.2))

    info = dict(outer=(out_l, out_w, tray_h + c["lid_t"]), pcb_z=pcb_z)
    return tray, lid, fit, info


def to_trimesh(m):
    mesh = m.to_mesh()
    return trimesh.Trimesh(vertices=np.asarray(mesh.vert_properties)[:, :3],
                           faces=np.asarray(mesh.tri_verts), process=True)


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stl")
    os.makedirs(out, exist_ok=True)
    tray, lid, fit, info = build(CONFIG)
    # print orientation: lid flat face down
    lid_print = lid.rotate((180, 0, 0))
    meshes = {}
    for name, part in [("openear_tray", tray), ("openear_lid", lid_print), ("openear_fitcheck", fit)]:
        tm = to_trimesh(part)
        b = tm.bounds
        tm.apply_translation((-b[0][0], -b[0][1], -b[0][2]))
        tm.export(os.path.join(out, name + ".stl"))
        meshes[name] = tm
        print(f"{name}.stl  {tm.extents.round(1)} mm  watertight={tm.is_watertight}  "
              f"volume={tm.volume / 1000:.1f} cm3")
    print("outer size L x W x H: %.1f x %.1f x %.1f mm" % info["outer"])
    try:
        preview(tray, lid, os.path.join(out, "preview.png"), info)
    except Exception as e:  # preview is optional
        print("preview skipped:", e)


def preview(tray, lid, path, info):
    """2D dimensioned drawing: lid holes (top view), far-end wall, stack-up."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Circle, FancyBboxPatch, Rectangle

    c = CONFIG
    ink, ink2, grid = "#0b0b0b", "#52514e", "#c9c8c3"
    blue, orange = "#2a78d6", "#eb6834"
    w = c["wall"]
    bx = by = w + c["fit"]
    L, W, H = info["outer"]
    pcb_z = info["pcb_z"]
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.6),
                             gridspec_kw=dict(width_ratios=[2.6, 1.1, 1.1]), facecolor="#fcfcfb")

    # (a) lid, top view
    ax = axes[0]
    ax.add_patch(FancyBboxPatch((0, 0), L, W, boxstyle="round,pad=0,rounding_size=3",
                                fill=False, ec=ink, lw=1.5))
    ax.add_patch(Rectangle((bx, by), c["board_l"], c["board_w"], fill=False, ec=grid, ls="--"))
    ax.text(bx + 1, by + c["board_w"] - 2.2, "PCB outline", color=ink2, fontsize=8)
    m = c["mic"]
    ax.add_patch(Circle((bx + m["x"], by + m["y"]), m["pitch"] + m["d"], fill=False, ec=blue, lw=1.5))
    ax.annotate("mic port", (bx + m["x"], by + m["y"]), (bx + m["x"] + 6, by + m["y"] + 3),
                fontsize=8, color=ink, arrowprops=dict(arrowstyle="-", color=ink2))
    for b in c["buttons"] + c["leds"]:
        ax.add_patch(Circle((bx + b["x"], by + b["y"]), b["d"] / 2, fill=False, ec=orange, lw=1.5))
        ax.text(bx + b["x"], by + b["y"] + b["d"] / 2 + 0.8, b["name"], ha="center",
                fontsize=7, color=ink)
    ax.text(-1, W / 2, "USB\nend", ha="right", va="center", fontsize=8, color=ink2)
    ax.set_xlim(-8, L + 2)
    ax.set_ylim(-3, W + 3)
    ax.set_title("Lid, top view (hole positions from PCB corner in CONFIG)", loc="left",
                 fontsize=10, fontweight="bold")

    # (b) far end wall, looking at it
    ax = axes[1]
    ax.add_patch(Rectangle((0, 0), W, H, fill=False, ec=ink, lw=1.5))
    ax.add_patch(Rectangle((by, pcb_z), c["board_w"], c["board_t"], color=grid))
    ax.text(by + 0.5, pcb_z + c["board_t"] + 0.5, "PCB", fontsize=7, color=ink2)
    for j in c["jacks"]:
        if j["face"] != "end_far":
            continue
        zc = pcb_z + c["board_t"] + j["z"] if j["z"] >= 0 else pcb_z + j["z"]
        ax.add_patch(Circle((by + j["along"], zc), j["d"] / 2, fill=False, ec=blue, lw=1.5))
        ax.text(by + j["along"] + j["d"] / 2 + 0.6, zc, j["name"], fontsize=7, va="center")
    ax.set_xlim(-2, W + 9)
    ax.set_ylim(-2, H + 2)
    ax.set_title("Far end wall", loc="left", fontsize=10, fontweight="bold")

    # (c) stack-up
    ax = axes[2]
    layers = [("floor", c["floor"]), ("battery bay", c["battery_h"]),
              ("under-PCB parts", c["under_clear"]), ("PCB", c["board_t"]),
              ("top parts", c["over_clear"]), ("lid", c["lid_t"])]
    z = 0
    for name, h in layers:
        ax.add_patch(Rectangle((0, z), 10, h, fill=name in ("floor", "PCB", "lid"),
                               color=grid if name in ("floor", "PCB", "lid") else None,
                               ec=ink2, lw=0.8))
        ax.text(11, z + h / 2, "%s  %.1f" % (name, h), va="center", fontsize=8)
        z += h
    ax.set_xlim(-1, 30)
    ax.set_ylim(-1, z + 1)
    ax.set_title("Stack-up (mm), total %.1f" % z, loc="left", fontsize=10, fontweight="bold")

    for ax in axes:
        ax.set_aspect("equal")
        ax.set_xticks([])
        ax.set_yticks([])
        for sp in ax.spines.values():
            sp.set_visible(False)
    fig.suptitle("OpenEar case  %.1f x %.1f x %.1f mm  -  defaults are GUESSES: measure your board,"
                 " edit CONFIG, re-run" % (L, W, H), x=0.01, ha="left", fontsize=11, color=ink)
    fig.tight_layout()
    fig.savefig(path, dpi=140)
    print("wrote preview.png")


if __name__ == "__main__":
    main()
