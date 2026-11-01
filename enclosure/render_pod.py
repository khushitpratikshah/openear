"""Shaded renders of the OpenEar pod (numpy z-buffer, no GPU). Writes PNGs into ./stl."""
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import openear_pod as P  # noqa: E402

BODY = np.array([0.15, 0.17, 0.20])     # graphite PLA
ACCENT = np.array([0.93, 0.94, 0.95])   # white (filament swap at the logo layer)
CLIP = np.array([0.16, 0.18, 0.21])
PCB = np.array([0.13, 0.42, 0.55])
METAL = np.array([0.72, 0.72, 0.70])


def tris(m, color, logo_z=None):
    mg = m.to_mesh()
    v = np.asarray(mg.vert_properties)[:, :3].astype(float)
    f = np.asarray(mg.tri_verts)
    t = v[f]
    c = np.tile(color, (len(f), 1))
    if logo_z is not None:
        zmin, zmax = t[:, :, 2].min(1), t[:, :, 2].max(1)
        c[(zmin >= logo_z - 1e-3) & (zmax > logo_z + 0.01)] = ACCENT
    return t, c


def look_at(eye, target, up=(0, 0, 1)):
    f = np.asarray(target, float) - eye
    f /= np.linalg.norm(f)
    r = np.cross(f, up)
    r /= np.linalg.norm(r)
    u = np.cross(r, f)
    return r, u, f


def render(scene, eye, target, size=(1600, 1050), fov=24, ss=2, name="render.png", bg=((246, 247, 249), (222, 226, 232)), lights=None):
    W, H = size[0] * ss, size[1] * ss
    eye = np.asarray(eye, float)
    r, u, f = look_at(eye, target)
    tan = np.tan(np.radians(fov) / 2)
    T = np.concatenate([s[0] for s in scene])
    C = np.concatenate([s[1] for s in scene])
    n = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
    rel = T - eye
    cx, cy, cz = rel @ r, rel @ u, rel @ f
    sx = (cx / (cz * tan) * (H / 2)) + W / 2
    sy = H / 2 - (cy / (cz * tan) * (H / 2))
    view = (T.mean(1) - eye)
    view /= np.linalg.norm(view, axis=1, keepdims=True)
    facing = (n * view).sum(1) < 0
    # lighting (Lambert key + fill + rim, Blinn highlight)
    lights = lights or [(np.array([-0.4, -0.7, 0.85]), 0.95), (np.array([0.8, 0.3, 0.5]), 0.35), (np.array([0.2, 0.9, -0.1]), 0.25)]
    shade = np.full(len(T), 0.20)
    spec = np.zeros(len(T))
    for d, k in lights:
        d = d / np.linalg.norm(d)
        lam = np.clip(n @ d, 0, None)
        shade += k * lam
        h = d - view
        h /= np.linalg.norm(h, axis=1, keepdims=True)
        spec += 0.22 * k * np.clip((n * h).sum(1), 0, None) ** 40
    col = np.clip(C * shade[:, None] + spec[:, None], 0, 1)

    img = np.zeros((H, W, 3))
    top, bot = np.array(bg[0]) / 255, np.array(bg[1]) / 255
    img[:] = (top[None, :] * (1 - np.linspace(0, 1, H))[:, None] + bot[None, :] * np.linspace(0, 1, H)[:, None])[:, None, :]
    zbuf = np.full((H, W), np.inf)
    idx = np.where(facing & (cz.min(1) > 0))[0]
    for i in idx:
        x, y, z = sx[i], sy[i], cz[i]
        x0, x1 = int(max(np.floor(x.min()), 0)), int(min(np.ceil(x.max()), W - 1))
        y0, y1 = int(max(np.floor(y.min()), 0)), int(min(np.ceil(y.max()), H - 1))
        if x1 < x0 or y1 < y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        d = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
        if abs(d) < 1e-9:
            continue
        a = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / d
        b = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / d
        c = 1 - a - b
        m = (a >= -1e-6) & (b >= -1e-6) & (c >= -1e-6)
        if not m.any():
            continue
        zz = 1 / (a / z[0] + b / z[1] + c / z[2])
        sub = zbuf[y0:y1 + 1, x0:x1 + 1]
        w = m & (zz < sub)
        sub[w] = zz[w]
        img[y0:y1 + 1, x0:x1 + 1][w] = col[i]
    # soft edge darkening where depth jumps (outline feel)
    dz = np.zeros_like(zbuf)
    zf = np.where(np.isinf(zbuf), 1e4, zbuf)
    dz[1:, :] += np.abs(np.diff(zf, axis=0))
    dz[:, 1:] += np.abs(np.diff(zf, axis=1))
    edge = np.clip(dz / 3.0, 0, 1)[..., None] * 0.35
    img = img * (1 - edge)
    out = Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).resize(size, Image.LANCZOS)
    out.save(os.path.join(P.OUT, name))
    return out


def board_proxy():
    """simple stand-in for the FTHR: PCB, jacks, USB, buttons (exploded view only)"""
    B = P.box
    x0, y0, z0 = P.PCB_X0, P.PCB_Y0, P.PCB_Z0
    parts = [(B(x0, y0, z0, x0 + P.PCB_L, y0 + P.PCB_W, z0 + P.PCB_T), PCB)]
    z1 = z0 + P.PCB_T
    metal = [B(x0, y0 + 8.5, z1, x0 + 17, y0 + 18.7, z1 + 5.0),          # headphone jack
             B(x0 + 64.4, y0 + 7.7, z1, x0 + 69.6, y0 + 16.4, z1 + 3.0),   # micro-USB
             B(x0 + 58, y0 + 0.3, z1, x0 + 63.5, y0 + 7.5, z1 + 6.0)]      # JST
    for bx, by in P.BUTTONS.values():
        metal.append(B(x0 + bx - 2, y0 + by - 2, z1, x0 + bx + 2, y0 + by + 2, z1 + 1.6))
    parts += [(m, METAL) for m in metal]
    return parts


if __name__ == "__main__":
    base, lid, clip, _ = P.build()
    Hz = P.H
    c = np.array([P.OUT_L / 2, P.OUT_W / 2, Hz / 2])
    s_base = tris(base, BODY)
    s_lid = tris(lid, BODY, logo_z=Hz)
    s_clip = tris(clip, CLIP)
    # hero: assembled, 3/4 from the front-left, looking at the logo
    render([s_base, s_lid, s_clip], eye=c + np.array([-62, -150, 120]), target=c + np.array([3, 0, -2]),
           fov=21, name="pod_hero.png")
    # top view
    render([s_base, s_lid, s_clip], eye=c + np.array([0, -1, 230]), target=c, fov=22, name="pod_top.png")
    # back: the pocket clip
    under = [(np.array([0.3, 0.5, -0.85]), 1.0), (np.array([-0.7, 0.4, -0.3]), 0.45), (np.array([0.0, -0.3, 0.9]), 0.2)]
    render([s_base, s_lid, s_clip], eye=c + np.array([75, 120, -105]), target=c + np.array([0, 0, -3]), fov=24,
           name="pod_back.png", lights=under)
    # exploded: base, board, lid lifted
    lifted = lid.translate((0, 0, 26))
    s_l2 = tris(lifted, BODY, logo_z=Hz + 26)
    scene = [s_base, s_l2, s_clip] + [tris(m, col) for m, col in board_proxy()]
    render(scene, eye=c + np.array([-70, -165, 150]), target=c + np.array([0, 0, 10]), fov=28,
           size=(1600, 1200), name="pod_exploded.png")
    print("renders written")
