"""Generates the Sand Tristram / Desert Wastes tileset: original art, written in Diablo's tile formats.

Usage: python3 gen_desert.py <town.pal> <out_dir> [preview.png]

Outputs (in out_dir):
  levels/desertdata/desert.cel  - micro tile frames
  levels/desertdata/desert.min  - pieces (16 blocks each)
  levels/desertdata/desert.sol  - piece properties
  levels/desertdata/desert.til  - a few megatiles (unused by the generator, needed by the loader)
  txtdata/desert/pieces.tsv     - named pieces and multi-tile structures for the level generators
  towners/desert/packbags.cel   - bags piled on the pack cow (overlay sprite)
  towners/desert/*.trn          - colour translations for the desert NPCs

Only the palette is read from the game data; every pixel of art is generated here.
"""
import colorsys
import math
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from dtiles import TYPE_LTRI, TYPE_RTRI, TYPE_TRANSPARENT, block_index, encode_transparent, encode_triangle, tri_width, write_cel  # noqa: E402
from raytrace import (K, Scene, fbm3, length, pfbm2, pnoise2, render, sd_box, sd_capsule, sd_cone, sd_cyl,  # noqa: E402
                      sd_ellipsoid, sd_plane_slab, sd_ridge, vnoise3)

SOLID = 1
BLOCK_LIGHT = 2
BLOCK_MISSILE = 4

# ---------------------------------------------------------------------------------------------
# Palette


def load_pal(path):
    d = open(path, 'rb').read()
    return np.array([[d[i * 3], d[i * 3 + 1], d[i * 3 + 2]] for i in range(256)], float)


class Quantizer:
    def __init__(self, pal):
        self.pal = pal
        allowed = [i for i in range(1, 255)]
        self.allowed = np.array(allowed)
        cols = pal[self.allowed]
        g = np.arange(0, 256, 4) + 2
        r, gg, b = np.meshgrid(g, g, g, indexing='ij')
        q = np.stack([r.ravel(), gg.ravel(), b.ravel()], 1).astype(float)
        w = np.array([2.0, 4.0, 3.0])
        best = np.zeros(len(q), int)
        bestd = np.full(len(q), 1e18)
        for k, c in enumerate(cols):
            d = (((q - c) ** 2) * w).sum(1)
            m = d < bestd
            bestd[m] = d[m]
            best[m] = k
        self.lut = self.allowed[best].reshape(64, 64, 64)

    def __call__(self, rgb, dither=True, ox=0, oy=0):
        """rgb: HxWx3 in 0..1 -> HxW palette indices."""
        h, w, _ = rgb.shape
        v = np.clip(rgb * 255.0, 0, 255)
        if dither:
            bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], float) / 16.0 - 0.47
            yy, xx = np.mgrid[0:h, 0:w]
            v = np.clip(v + bayer[(yy + oy) & 3, (xx + ox) & 3][..., None] * 10.0, 0, 255)
        idx = (v / 4).astype(int).clip(0, 63)
        return self.lut[idx[..., 0], idx[..., 1], idx[..., 2]]


# ---------------------------------------------------------------------------------------------
# Colours (albedo, 0..1)

def rgb(h):
    return np.array([int(h[i:i + 2], 16) for i in (1, 3, 5)], float) / 255.0


SAND_A = rgb('#c98d56')
SAND_B = rgb('#ab7043')
SAND_C = rgb('#d49e64')
HARDPAN = rgb('#a77f57')
CANVAS = rgb('#d6b88e')
CANVAS_DARK = rgb('#5a3a26')
CANVAS_RED = rgb('#8a2f22')
CANVAS_BLUE = rgb('#34466b')
FELT = rgb('#b59a78')
WOOD = rgb('#6b4a2c')
WOOD_DARK = rgb('#3e2a19')
ADOBE = rgb('#a9784c')
STONE = rgb('#ad7f56')
STONE_DARK = rgb('#6e4c32')
METAL = rgb('#3c3c40')
CLAY = rgb('#9a4f2c')
BURLAP = rgb('#9c7f52')
BONE = rgb('#d8ccb0')
FROND = rgb('#6b6a2e')
FROND_DRY = rgb('#8d7a3a')
BARK = rgb('#5e4b36')
DARK = rgb('#1c130c')
RUG_RED = rgb('#7a1f1a')
RUG_GOLD = rgb('#b88a3a')
RUG_BLUE = rgb('#253a63')


def cvec(c, n):
    return np.repeat(np.asarray(c, float)[:, None], n, 1)


# ---------------------------------------------------------------------------------------------
# Ground textures (tile-periodic, so neighbouring tiles always match)

def sand(x, y, seed=0, tone=0.0):
    u, v = np.mod(x, 1.0), np.mod(y, 1.0)
    n = pfbm2(u, v, 1, base=3, octaves=4)
    base = SAND_A[:, None] * (1 - n) + SAND_B[:, None] * n
    hi = pfbm2(u, v, 5, base=6, octaves=2)
    base = base * (1 - 0.35 * hi) + SAND_C[:, None] * 0.35 * hi
    # Wind ripples (integer frequencies keep them continuous across tiles).
    ph = 2 * math.pi * (3 * u + 2 * v) + 3.0 * pfbm2(u, v, 9, base=2, octaves=2)
    ripple = np.cos(ph)
    grain = pnoise2(u, v, 32, 13 + seed) - 0.5
    speck = pnoise2(u, v, 64, 29 + seed) - 0.5
    shade = 1.0 + 0.035 * ripple + 0.12 * grain + 0.12 * speck + tone
    return base * shade, np.ones_like(u)


def _blob_mask(u, v, cx, cy, rx, ry, rot=0.0):
    du, dv = u - cx, v - cy
    cr, sr = math.cos(rot), math.sin(rot)
    a = (du * cr + dv * sr) / rx
    b = (-du * sr + dv * cr) / ry
    return a * a + b * b


def floor_variant(kind, seed):
    rng = np.random.default_rng(seed)

    def fn(x, y):
        u, v = np.mod(x, 1.0), np.mod(y, 1.0)
        col, nz = sand(x, y, seed % 7)
        if kind == 'pebbles':
            for _ in range(rng.integers(5, 10) if False else 7):
                pass
            r2 = np.random.default_rng(seed)
            for _ in range(8):
                cx, cy = r2.uniform(0.18, 0.82, 2)
                rx = r2.uniform(0.025, 0.06)
                m = _blob_mask(u, v, cx, cy, rx, rx * r2.uniform(0.6, 1.0), r2.uniform(0, 3))
                stone = STONE_DARK if r2.random() < 0.5 else STONE
                inside = m < 1
                lit = np.clip(1.15 - 0.6 * (u - cx + v - cy) / rx, 0.6, 1.3)
                col = np.where(inside, stone[:, None] * lit, col)
                ring = (m >= 1) & (m < 1.8)
                col = np.where(ring, col * 0.78, col)  # contact shadow
        elif kind == 'hardpan':
            # Cracked dry mud: Voronoi edges on a tileable lattice.
            pts = []
            r2 = np.random.default_rng(seed)
            for _ in range(9):
                pts.append(r2.uniform(0, 1, 2))
            pts = np.array(pts)
            d1 = np.full_like(u, 9.0)
            d2 = np.full_like(u, 9.0)
            for px_, py_ in pts:
                for ox_ in (-1, 0, 1):
                    for oy_ in (-1, 0, 1):
                        d = np.sqrt((u - px_ - ox_) ** 2 + (v - py_ - oy_) ** 2)
                        d2 = np.where(d < d1, d1, np.minimum(d2, d))
                        d1 = np.minimum(d1, d)
            edge = d2 - d1
            fade = np.clip(1.0 - 2.2 * np.maximum(np.abs(u - 0.5), np.abs(v - 0.5)) + 0.25, 0, 1)
            base = HARDPAN[:, None] * (0.92 + 0.12 * pfbm2(u, v, 3, base=4))
            col = col * (1 - fade) + base * fade
            crack = np.clip(1 - edge / 0.025, 0, 1) * fade
            col = col * (1 - 0.55 * crack)
        elif kind == 'bones':
            r2 = np.random.default_rng(seed)
            cx, cy = 0.5, 0.5
            for k in range(5):
                ang = r2.uniform(0, math.pi)
                m = _blob_mask(u, v, cx + r2.uniform(-0.18, 0.18), cy + r2.uniform(-0.18, 0.18), r2.uniform(0.06, 0.12), 0.018, ang)
                col = np.where(m < 1, BONE[:, None] * (0.85 + 0.2 * (1 - m)), col)
            # a little skull
            m = _blob_mask(u, v, 0.42, 0.55, 0.05, 0.04)
            col = np.where(m < 1, BONE[:, None], col)
            for ex in (0.405, 0.435):
                col = np.where(_blob_mask(u, v, ex, 0.55, 0.011, 0.011) < 1, DARK[:, None], col)
        elif kind == 'scrub':
            r2 = np.random.default_rng(seed)
            for _ in range(3):
                cx, cy = r2.uniform(0.25, 0.75, 2)
                for s in range(14):
                    ang = r2.uniform(0, 2 * math.pi)
                    ln = r2.uniform(0.04, 0.11)
                    ex, ey = cx + math.cos(ang) * ln, cy + math.sin(ang) * ln
                    # distance to segment
                    du, dv = ex - cx, ey - cy
                    t = np.clip(((u - cx) * du + (v - cy) * dv) / (du * du + dv * dv), 0, 1)
                    d = np.sqrt((u - cx - du * t) ** 2 + (v - cy - dv * t) ** 2)
                    c = FROND_DRY if s % 3 else FROND
                    col = np.where(d < 0.006, c[:, None] * (0.8 + 0.4 * t), col)
                col = np.where(_blob_mask(u, v, cx, cy, 0.05, 0.035) < 1, col * 0.8, col)
        elif kind == 'drift':
            m = _blob_mask(u, v, 0.5, 0.5, 0.38, 0.16, seed * 0.7)
            f = np.clip(1 - m, 0, 1)
            col = col * (1 + 0.12 * f)
        elif kind == 'dark':
            col = col * 0.9
        return col, nz
    return fn


def rug_ground(color, border, accent):
    def fn(x, y):
        col, nz = sand(x, y)
        # rug covers [0.2, W-0.2] x [0.2, H-0.2] of its own footprint (set by caller via closure)
        return col, nz
    return fn


# ---------------------------------------------------------------------------------------------
# Materials

def material_fn(scene, p, n, mat):
    N = p.shape[1]
    albedo = np.zeros((3, N))
    emissive = np.zeros((3, N))
    for i, pr in enumerate(scene.prims):
        m = mat == i
        if not m.any():
            continue
        q = p[:, m]
        nn = n[:, m]
        kind = pr.mat
        prm = pr.params
        c = np.asarray(prm.get('color', CANVAS), float)
        cnt = q.shape[1]
        if kind in ('canvas', 'felt'):
            col = cvec(c, cnt)
            stripe = prm.get('stripe')
            if stripe:
                axis, period, c2, width = stripe
                coord = q[axis] / period
                s = (np.mod(coord, 1.0) < width)
                col = np.where(s, cvec(c2, cnt), col)
            band = prm.get('band')
            if band:
                z0, z1, c2, c3 = band
                inb = (q[2] > z0) & (q[2] < z1)
                ang = np.arctan2(q[1] - prm.get('cy', 0), q[0] - prm.get('cx', 0))
                zig = np.abs(np.mod(ang * 6 / math.pi + (q[2] - z0) / (z1 - z0) * 2, 2.0) - 1.0) < 0.35
                col = np.where(inb, np.where(zig, cvec(c3, cnt), cvec(c2, cnt)), col)
            grime = fbm3(q[0] * 3, q[1] * 3, q[2] * 3, 3, 3)
            weave = vnoise3(q[0] * 60, q[1] * 60, q[2] * 60, 5)
            col = col * (0.82 + 0.25 * grime + 0.08 * weave)
            # dirt creeping up from the ground
            col = col * np.clip(0.7 + q[2] * 1.2, 0.7, 1.0)
            patch = prm.get('patches')
            if patch is not None:
                pn = fbm3(q[0] * 2.2 + 9, q[1] * 2.2, q[2] * 2.2, 11, 2)
                col = np.where(pn > 0.62, cvec(patch, cnt) * (0.9 + 0.2 * weave), col)
        elif kind == 'wood':
            axis = prm.get('axis', 2)
            col = cvec(c, cnt)
            other = [a for a in range(3) if a != axis]
            plank = q[other[0]] * 6 + q[other[1]] * 6
            seam = np.abs(np.mod(plank, 1.0) - 0.5) > 0.44
            grain = vnoise3(q[0] * 4 + q[axis] * 0, q[1] * 4, q[2] * 4, 7)
            streak = vnoise3(q[other[0]] * 40, q[other[1]] * 40, q[axis] * 3, 8)
            col = col * (0.75 + 0.3 * grain + 0.15 * streak)
            col = np.where(seam, col * 0.55, col)
            # shade each plank slightly differently
            pid = np.floor(plank)
            col = col * (0.9 + 0.2 * ((pid * 0.618) % 1.0))
        elif kind == 'adobe':
            col = cvec(c, cnt)
            n1 = fbm3(q[0] * 2.5, q[1] * 2.5, q[2] * 2.5, 21, 4)
            col = col * (0.78 + 0.35 * n1)
            # mud bricks peeking through cracked plaster
            bz = q[2] * 6
            bh = (q[0] + q[1]) * 3 + np.floor(bz) * 0.5
            brick = (np.abs(np.mod(bz, 1.0) - 0.5) > 0.42) | (np.abs(np.mod(bh, 1.0) - 0.5) > 0.45)
            bare = fbm3(q[0] * 1.7 + 4, q[1] * 1.7, q[2] * 1.7, 23, 2) > 0.6
            col = np.where(bare & brick, col * 0.62, np.where(bare, col * 0.9, col))
            col = col * np.clip(0.75 + q[2] * 0.6, 0.75, 1.0)
        elif kind == 'stone':
            col = cvec(c, cnt)
            strata = np.sin(q[2] * 18 + fbm3(q[0], q[1], q[2], 31, 2) * 4)
            n1 = fbm3(q[0] * 3, q[1] * 3, q[2] * 3, 33, 4)
            col = col * (0.75 + 0.3 * n1 + 0.07 * strata)
            dark = STONE_DARK[:, None]
            col = np.where(n1 < 0.33, col * 0.6 + dark * 0.4, col)
        elif kind == 'metal':
            col = cvec(c, cnt) * (0.8 + 0.4 * vnoise3(q[0] * 20, q[1] * 20, q[2] * 20, 41))
        elif kind == 'coal':
            col = cvec(rgb('#2a1a10'), cnt)
            heat = vnoise3(q[0] * 14, q[1] * 14, q[2] * 14, 43)
            emissive[:, m] = np.array([1.0, 0.45, 0.08])[:, None] * np.clip(heat * 1.6 - 0.25, 0, 1.2)
        elif kind == 'fire':
            col = cvec(rgb('#000000'), cnt)
            emissive[:, m] = np.array([1.0, 0.62, 0.15])[:, None] * (0.8 + 0.5 * vnoise3(q[0] * 25, q[1] * 25, q[2] * 25, 44))
        elif kind == 'clay':
            col = cvec(c, cnt) * (0.8 + 0.3 * fbm3(q[0] * 4, q[1] * 4, q[2] * 4, 45, 3))
            band = np.abs(np.mod(q[2] * 7, 1.0) - 0.5) < 0.06
            col = np.where(band, col * 0.6, col)
        elif kind == 'burlap':
            w = vnoise3(q[0] * 80, q[1] * 80, q[2] * 80, 47)
            col = cvec(c, cnt) * (0.75 + 0.25 * w + 0.2 * fbm3(q[0] * 3, q[1] * 3, q[2] * 3, 48, 2))
        elif kind == 'rug':
            # Patterned carpet in the rug's own plane coordinates.
            cx, cy, hx, hy = prm['rect']
            du = np.abs(q[0] - cx) / hx
            dv = np.abs(q[1] - cy) / hy
            edge = np.maximum(du, dv)
            col = cvec(prm.get('color', RUG_RED), cnt)
            col = np.where(edge > 0.82, cvec(prm.get('border', RUG_GOLD), cnt), col)
            diamond = (np.abs(np.mod(q[0] * 3, 1.0) - 0.5) + np.abs(np.mod(q[1] * 3, 1.0) - 0.5)) < 0.22
            col = np.where((edge < 0.7) & diamond, cvec(prm.get('accent', RUG_BLUE), cnt), col)
            col = col * (0.85 + 0.2 * vnoise3(q[0] * 50, q[1] * 50, q[2] * 50, 49))
        elif kind == 'bone':
            col = cvec(BONE, cnt) * (0.85 + 0.2 * vnoise3(q[0] * 15, q[1] * 15, q[2] * 15, 51))
        elif kind == 'frond':
            col = cvec(c, cnt) * (0.75 + 0.45 * vnoise3(q[0] * 30, q[1] * 30, q[2] * 30, 53))
        elif kind == 'bark':
            ring = np.abs(np.mod(q[2] * 9, 1.0) - 0.5) > 0.4
            col = cvec(c, cnt) * (0.8 + 0.3 * vnoise3(q[0] * 25, q[1] * 25, q[2] * 6, 55))
            col = np.where(ring, col * 0.7, col)
        elif kind == 'dark':
            col = cvec(DARK, cnt)
        elif kind == 'water':
            col = cvec(rgb('#1b2633'), cnt) * (0.9 + 0.3 * vnoise3(q[0] * 10, q[1] * 10, q[2], 57))
        else:
            col = cvec(c, cnt)
        albedo[:, m] = col
    return albedo, emissive


# ---------------------------------------------------------------------------------------------
# Structures

def poles(s, pts, z1, r=0.045, color=WOOD_DARK):
    for (x, y) in pts:
        s.add(lambda p, x=x, y=y: sd_capsule(p, (x, y, 0), (x, y, z1), r), 'wood', color=color, axis=2)


def tent_big():
    W, H = 4, 3
    s = Scene()
    ridge, eave = 1.72, 1.12

    def roof(x, y):
        slope = ridge - np.abs(y - 1.5) / 1.25 * (ridge - eave)
        sag = 0.10 * np.sin(np.clip((x - 0.28) / 1.72, 0, 2) * math.pi) ** 2
        return slope - sag
    s.add(lambda p: sd_plane_slab(p, roof, 0.025, 0.25, 3.75, 0.22, 2.78), 'canvas', color=CANVAS_DARK,
          stripe=(0, 0.42, CANVAS, 0.28), patches=None)
    # back and right walls: cloth hanging from the eaves; the front (y=2.75) stays open
    s.add(lambda p: sd_box(p, (0.28, 1.5, 0.56), (0.025, 1.24, 0.56)), 'canvas', color=CANVAS_DARK, stripe=(1, 0.42, CANVAS, 0.28))
    s.add(lambda p: sd_box(p, (2.0, 0.25, 0.56), (1.72, 0.025, 0.56)), 'canvas', color=CANVAS_DARK, stripe=(0, 0.42, CANVAS, 0.28))
    s.add(lambda p: sd_box(p, (3.72, 1.0, 0.56), (0.025, 0.75, 0.56)), 'canvas', color=CANVAS_DARK, stripe=(1, 0.42, CANVAS, 0.28))
    # rolled-up flap over the open part of the right wall
    s.add(lambda p: sd_capsule(p, (3.74, 1.78, 1.02), (3.74, 2.72, 1.02), 0.07), 'canvas', color=CANVAS)
    poles(s, [(0.3, 2.74), (2.0, 2.74), (3.7, 2.74), (3.7, 1.75)], eave + 0.05)
    poles(s, [(1.15, 1.5), (2.85, 1.5)], ridge + 0.1, 0.05)
    # inside: a rug, cushions and a chest
    s.add(lambda p: sd_box(p, (1.9, 1.7, 0.012), (1.25, 0.75, 0.012)), 'rug', rect=(1.9, 1.7, 1.25, 0.75), color=RUG_RED)
    for (x, y, c) in [(0.75, 1.0, CANVAS_RED), (1.25, 0.8, CANVAS_BLUE), (3.0, 0.9, CANVAS_RED)]:
        s.add(lambda p, x=x, y=y: sd_ellipsoid(p, (x, y, 0.12), (0.28, 0.2, 0.13)), 'canvas', color=c)
    s.add(lambda p: sd_box(p, (2.2, 0.62, 0.22), (0.32, 0.2, 0.2), 0.02), 'wood', color=WOOD, axis=0)
    # guy ropes to stakes at the front corners
    s.add(lambda p: sd_capsule(p, (0.3, 2.74, eave), (0.25, 2.95, 0.0), 0.012), 'wood', color=BURLAP, axis=2)
    s.add(lambda p: sd_capsule(p, (3.7, 2.74, eave), (3.92, 2.92, 0.0), 0.012), 'wood', color=BURLAP, axis=2)
    return s, W, H


def tent_small(color=CANVAS, flap=CANVAS_RED):
    W, H = 2, 2
    s = Scene()
    s.add(lambda p: sd_ridge(p, 0, 0.3, 1.68, 1.0, 0.72, 1.2), 'canvas', color=color, patches=rgb('#8f7c58'))
    # open front: a dark triangle cut into the +x end
    s.carve(lambda p: sd_ridge(p, 0, 1.45, 1.9, 1.0, 0.36, 0.78))
    s.add(lambda p: sd_box(p, (1.42, 1.0, 0.36), (0.02, 0.4, 0.4)), 'dark')
    # tied-back flaps
    s.add(lambda p: sd_capsule(p, (1.7, 0.55, 0.05), (1.7, 0.78, 0.55), 0.06), 'canvas', color=flap)
    s.add(lambda p: sd_capsule(p, (1.7, 1.45, 0.05), (1.7, 1.22, 0.55), 0.06), 'canvas', color=flap)
    poles(s, [(1.72, 1.0)], 1.32, 0.035)
    poles(s, [(0.27, 1.0)], 1.3, 0.035)
    return s, W, H


def yurt():
    W, H = 3, 3
    s = Scene()
    cx, cy = 1.5, 1.5
    s.add(lambda p: sd_cyl(p, (cx, cy), 1.12, 0.0, 0.95), 'felt', color=FELT, band=(0.55, 0.78, CANVAS_RED, CANVAS_BLUE), cx=cx, cy=cy)
    s.add(lambda p: sd_cone(p, (cx, cy), 1.28, 0.88, 1.85), 'felt', color=rgb('#9c8463'), stripe=(2, 0.18, rgb('#7d6649'), 0.18))
    # door facing the camera
    d = 1.12 * 0.7071
    s.add(lambda p: sd_box(p, (cx + d, cy + d, 0.42), (0.16, 0.16, 0.42)), 'wood', color=rgb('#7a2a1d'), axis=2)
    s.add(lambda p: sd_capsule(p, (cx + 0.36, cy + 1.02, 0.86), (cx + 1.02, cy + 0.36, 0.86), 0.03), 'wood', color=WOOD_DARK, axis=0)
    s.add(lambda p: sd_capsule(p, (cx, cy, 1.8), (cx, cy, 2.05), 0.06), 'wood', color=WOOD_DARK, axis=2)
    return s, W, H


def hut():
    W, H = 3, 3
    s = Scene()
    s.add(lambda p: sd_box(p, (1.5, 1.5, 0.62), (1.18, 1.18, 0.62), 0.05), 'adobe', color=ADOBE)
    # doorway on the +y face, window on the +x face
    s.carve(lambda p: sd_box(p, (1.1, 2.7, 0.5), (0.27, 0.2, 0.5)))
    s.add(lambda p: sd_box(p, (1.1, 2.5, 0.5), (0.27, 0.02, 0.5)), 'canvas', color=CANVAS_BLUE, patches=None)
    s.carve(lambda p: sd_box(p, (2.7, 1.6, 0.8), (0.2, 0.18, 0.14)))
    s.add(lambda p: sd_box(p, (2.56, 1.6, 0.8), (0.02, 0.18, 0.14)), 'dark')
    # roof beams poking out and a patched tarp on top
    for k in range(5):
        y = 0.55 + k * 0.48
        s.add(lambda p, y=y: sd_capsule(p, (0.18, y, 1.27), (2.86, y, 1.27), 0.045), 'wood', color=WOOD_DARK, axis=0)
    s.add(lambda p: sd_plane_slab(p, lambda x, y: 1.33 - 0.05 * np.sin((x - 0.3) * 2.4) ** 2, 0.03, 0.28, 2.72, 0.28, 2.72), 'canvas',
          color=rgb('#8e7a55'), patches=rgb('#7d6446'))
    # plank patch nailed over a crack, and a pot by the door
    s.add(lambda p: sd_box(p, (2.69, 0.75, 0.45), (0.02, 0.28, 0.32)), 'wood', color=WOOD, axis=2)
    s.add(lambda p: sd_cyl(p, (1.75, 2.8), 0.13, 0.0, 0.32), 'clay', color=CLAY)
    return s, W, H


def shack():
    W, H = 3, 2
    s = Scene()
    s.add(lambda p: sd_box(p, (1.5, 1.0, 0.38), (1.2, 0.72, 0.38), 0.02), 'adobe', color=rgb('#9a6c45'))
    s.add(lambda p: sd_box(p, (1.5, 1.0, 1.0), (1.18, 0.7, 0.26), 0.01), 'wood', color=WOOD, axis=2)
    s.carve(lambda p: sd_box(p, (2.0, 1.75, 0.55), (0.25, 0.1, 0.55)))
    s.add(lambda p: sd_box(p, (2.0, 1.66, 0.55), (0.25, 0.02, 0.55)), 'dark')
    s.add(lambda p: sd_plane_slab(p, lambda x, y: 1.55 - (y - 0.2) * 0.32, 0.03, 0.2, 2.8, 0.18, 1.86), 'wood', color=rgb('#5c4027'), axis=1)
    s.add(lambda p: sd_box(p, (0.9, 1.74, 0.85), (0.35, 0.02, 0.2)), 'canvas', color=CANVAS_RED)
    return s, W, H


def leanto():
    W, H = 2, 2
    s = Scene()
    s.add(lambda p: sd_plane_slab(p, lambda x, y: 0.55 + (y - 0.25) * 0.5, 0.025, 0.22, 1.78, 0.22, 1.78), 'canvas', color=rgb('#a48c62'),
          patches=rgb('#6a5236'))
    poles(s, [(0.28, 1.72), (1.72, 1.72)], 1.32)
    s.add(lambda p: sd_box(p, (0.6, 0.6, 0.2), (0.22, 0.2, 0.2), 0.02), 'wood', color=WOOD, axis=0)
    s.add(lambda p: sd_box(p, (0.62, 0.62, 0.52), (0.16, 0.15, 0.12), 0.02), 'wood', color=rgb('#7b5835'), axis=1)
    for (x, y) in [(1.15, 0.6), (1.35, 0.95), (1.05, 1.0)]:
        s.add(lambda p, x=x, y=y: sd_ellipsoid(p, (x, y, 0.15), (0.2, 0.17, 0.16)), 'burlap', color=BURLAP)
    return s, W, H


def forge():
    W, H = 2, 2
    s = Scene()
    s.add(lambda p: sd_box(p, (0.72, 0.72, 0.36), (0.48, 0.48, 0.36), 0.06), 'stone', color=rgb('#8b6b4c'))
    s.add(lambda p: sd_cyl(p, (0.48, 0.48), 0.22, 0.6, 1.9), 'stone', color=rgb('#7a5c40'))
    s.carve(lambda p: sd_cyl(p, (0.85, 0.85), 0.24, 0.62, 1.0))
    s.add(lambda p: sd_cyl(p, (0.85, 0.85), 0.24, 0.55, 0.64), 'coal')
    s.add(lambda p: sd_box(p, (1.22, 0.72, 0.18), (0.02, 0.24, 0.12)), 'coal')  # glow from the stoke hole
    # anvil on a stump
    s.add(lambda p: sd_cyl(p, (1.42, 1.38), 0.2, 0.0, 0.42), 'bark', color=BARK)
    s.add(lambda p: sd_box(p, (1.42, 1.38, 0.5), (0.2, 0.09, 0.07), 0.01), 'metal', color=METAL)
    s.add(lambda p: sd_box(p, (1.42, 1.38, 0.44), (0.08, 0.06, 0.05)), 'metal', color=METAL)
    s.add(lambda p: sd_cyl(p, (1.55, 0.6), 0.14, 0.0, 0.3), 'wood', color=WOOD, axis=2)  # water bucket
    return s, W, H


def stall(color=CANVAS_RED):
    W, H = 3, 2
    s = Scene()
    s.add(lambda p: sd_plane_slab(p, lambda x, y: 1.35 - 0.08 * np.sin((x - 0.25) / 2.5 * math.pi * 2) ** 2 - (y - 0.2) * 0.12,
                                  0.025, 0.2, 2.8, 0.2, 1.8), 'canvas', color=color, stripe=(0, 0.36, CANVAS, 0.5))
    poles(s, [(0.25, 0.25), (2.75, 0.25), (0.25, 1.75), (2.75, 1.75)], 1.36)
    s.add(lambda p: sd_box(p, (1.5, 1.3, 0.3), (1.0, 0.25, 0.3), 0.02), 'wood', color=WOOD, axis=0)
    s.add(lambda p: sd_box(p, (1.5, 1.52, 0.42), (1.0, 0.02, 0.18)), 'canvas', color=CANVAS_BLUE)
    for k, x in enumerate([0.75, 1.15, 1.55, 1.95, 2.3]):
        h = 0.14 + 0.06 * (k % 2)
        s.add(lambda p, x=x, h=h: sd_cyl(p, (x, 1.3), 0.09, 0.6, 0.6 + h), 'clay', color=CLAY if k % 2 else rgb('#7a6a4a'))
    return s, W, H


def well():
    W, H = 2, 2
    s = Scene()
    s.add(lambda p: sd_cyl(p, (1.0, 1.0), 0.62, 0.0, 0.5), 'stone', color=STONE)
    s.carve(lambda p: sd_cyl(p, (1.0, 1.0), 0.44, 0.15, 0.7))
    s.add(lambda p: sd_cyl(p, (1.0, 1.0), 0.44, 0.0, 0.17), 'water')
    poles(s, [(0.42, 1.0), (1.58, 1.0)], 1.35, 0.05)
    s.add(lambda p: sd_capsule(p, (0.36, 1.0, 1.28), (1.64, 1.0, 1.28), 0.05), 'wood', color=WOOD, axis=0)
    s.add(lambda p: sd_capsule(p, (1.0, 1.0, 1.27), (1.0, 1.0, 0.8), 0.012), 'burlap', color=BURLAP)
    s.add(lambda p: sd_cyl(p, (1.0, 1.0), 0.11, 0.62, 0.8), 'wood', color=WOOD, axis=2)
    return s, W, H


def crates():
    s = Scene()
    s.add(lambda p: sd_box(p, (0.45, 0.48, 0.2), (0.24, 0.24, 0.2), 0.015), 'wood', color=WOOD, axis=0)
    s.add(lambda p: sd_box(p, (0.62, 0.62, 0.17), (0.17, 0.17, 0.17), 0.015), 'wood', color=rgb('#7d5a36'), axis=1)
    s.add(lambda p: sd_box(p, (0.47, 0.5, 0.52), (0.16, 0.16, 0.12), 0.015), 'wood', color=rgb('#5d4126'), axis=0)
    return s, 1, 1


def barrels():
    s = Scene()
    for (x, y, h) in [(0.4, 0.45, 0.48), (0.68, 0.62, 0.42)]:
        s.add(lambda p, x=x, y=y, h=h: sd_cyl(p, (x, y), 0.19 + 0.02 * np.sin(np.clip(p[2] / h, 0, 1) * math.pi), 0, h), 'wood', color=WOOD, axis=2)
    return s, 1, 1


def sacks():
    s = Scene()
    for (x, y, z, r) in [(0.35, 0.45, 0.15, 0.2), (0.65, 0.4, 0.14, 0.19), (0.5, 0.68, 0.14, 0.2), (0.5, 0.5, 0.36, 0.17)]:
        s.add(lambda p, x=x, y=y, z=z, r=r: sd_ellipsoid(p, (x, y, z), (r, r * 0.85, r * 0.8)), 'burlap', color=BURLAP)
    return s, 1, 1


def urns():
    s = Scene()
    for (x, y, h, c) in [(0.4, 0.4, 0.45, CLAY), (0.66, 0.55, 0.32, rgb('#8a6a40')), (0.42, 0.7, 0.26, CLAY)]:
        s.add(lambda p, x=x, y=y, h=h: sd_cyl(p, (x, y), 0.13 + 0.06 * np.sin(np.clip(p[2] / h, 0, 1) * math.pi * 0.9), 0, h), 'clay', color=c)
    return s, 1, 1


def campfire():
    s = Scene()
    for k in range(7):
        a = k / 7 * 2 * math.pi
        s.add(lambda p, a=a: sd_ellipsoid(p, (0.5 + 0.28 * math.cos(a), 0.5 + 0.28 * math.sin(a), 0.05), (0.08, 0.07, 0.06)), 'stone', color=STONE)
    s.add(lambda p: sd_capsule(p, (0.32, 0.4, 0.06), (0.68, 0.62, 0.1), 0.04), 'bark', color=BARK)
    s.add(lambda p: sd_capsule(p, (0.65, 0.35, 0.06), (0.38, 0.65, 0.11), 0.04), 'bark', color=BARK)
    s.add(lambda p: sd_cyl(p, (0.5, 0.5), 0.16, 0.0, 0.06), 'coal')
    s.add(lambda p: sd_cone(p, (0.5, 0.5), 0.12, 0.05, 0.42), 'fire')
    return s, 1, 1


def totem():
    s = Scene()
    s.add(lambda p: sd_capsule(p, (0.5, 0.5, 0), (0.5, 0.5, 1.35), 0.035), 'wood', color=WOOD_DARK, axis=2)
    s.add(lambda p: sd_ellipsoid(p, (0.52, 0.52, 1.25), (0.13, 0.11, 0.11)), 'bone')
    s.add(lambda p: sd_capsule(p, (0.45, 0.4, 1.3), (0.3, 0.25, 1.45), 0.025), 'bone')
    s.add(lambda p: sd_capsule(p, (0.59, 0.6, 1.3), (0.74, 0.75, 1.45), 0.025), 'bone')
    s.add(lambda p: sd_box(p, (0.5, 0.5, 0.9), (0.1, 0.02, 0.2)), 'canvas', color=CANVAS_RED)
    return s, 1, 1


def rock(seed, tall=False, size=1):
    s = Scene()
    rng = np.random.default_rng(seed)
    c = size * 0.5
    blobs = []
    if tall:
        blobs = [(c, c, 0.0, 0.0, 0.0, -1.0)]
    else:
        for _ in range(rng.integers(2, 4)):
            blobs.append((c + rng.uniform(-0.12, 0.12) * size, c + rng.uniform(-0.12, 0.12) * size, rng.uniform(0.12, 0.28) * size ** 0.6,
                          rng.uniform(0.22, 0.34) * size, rng.uniform(0.2, 0.32) * size, rng.uniform(0.25, 0.45) * size ** 0.7))

    height = rng.uniform(1.1, 2.3)

    def sdf(p, blobs=blobs, seed=seed, tall=tall, height=height):
        d = np.full(p.shape[1], 1e9)
        if tall:
            # a weathered sandstone column, wider at the foot, with ledges
            t = np.clip(p[2] / height, 0, 1)
            r = 0.38 - 0.16 * t + 0.05 * np.sin(p[2] * 6 + seed) - 0.04 * np.sin(p[2] * 13 + seed * 2)
            d = sd_cyl(p, (c, c), r, 0.0, height - 0.15) * 0.8
            cap = sd_ellipsoid(p, (c + 0.03, c - 0.02, height - 0.15), (0.24, 0.22, 0.24))
            k = 0.12  # smooth union rounds the top off
            h = np.clip(0.5 + 0.5 * (cap - d) / k, 0, 1)
            d = cap * h + d * (1 - h) - k * h * (1 - h)
            # a broad, eroded foot
            foot = sd_ellipsoid(p, (c, c, 0.0), (0.46, 0.44, 0.28))
            h = np.clip(0.5 + 0.5 * (foot - d) / k, 0, 1)
            d = foot * h + d * (1 - h) - k * h * (1 - h)
            return d + (fbm3(p[0] * 3, p[1] * 3, p[2] * 1.5, seed, 3) - 0.5) * 0.16
        for (x, y, z, rx, ry, rz) in blobs:
            d = np.minimum(d, sd_ellipsoid(p, (x, y, z), (rx, ry, rz)))
        return d + (fbm3(p[0] * 4, p[1] * 4, p[2] * 4, seed, 3) - 0.5) * 0.09
    tone = STONE * rng.uniform(0.85, 1.1)
    s.add(sdf, 'stone', color=tone)
    return s, size, size


def palm(seed):
    s = Scene()
    rng = np.random.default_rng(seed)
    lean = rng.uniform(-0.12, 0.12, 2)
    top = (0.5 + lean[0], 0.5 + lean[1], 2.0)
    for k in range(6):
        t0, t1 = k / 6, (k + 1) / 6
        a = (0.5 + lean[0] * t0 * t0, 0.5 + lean[1] * t0 * t0, 2.0 * t0)
        b = (0.5 + lean[0] * t1 * t1, 0.5 + lean[1] * t1 * t1, 2.0 * t1)
        s.add(lambda p, a=a, b=b, r=0.07 - 0.015 * t0: sd_capsule(p, a, b, r), 'bark', color=BARK)
    for k in range(9):
        ang = k / 9 * 2 * math.pi + rng.uniform(-0.2, 0.2)
        ln = rng.uniform(0.38, 0.48)
        dx, dy = math.cos(ang), math.sin(ang)
        mid = (top[0] + dx * ln * 0.55, top[1] + dy * ln * 0.55, top[2] + 0.05)
        tip = (top[0] + dx * ln, top[1] + dy * ln, top[2] - 0.32)
        col = FROND if k % 3 else FROND_DRY
        s.add(lambda p, a=top, b=mid: sd_capsule(p, a, b, 0.055), 'frond', color=col)
        s.add(lambda p, a=mid, b=tip: sd_capsule(p, a, b, 0.045), 'frond', color=col)
    return s, 1, 1


def dead_tree(seed):
    s = Scene()
    rng = np.random.default_rng(seed)
    s.add(lambda p: sd_capsule(p, (0.5, 0.5, 0), (0.52, 0.48, 1.45), 0.09), 'bark', color=rgb('#6c5c4a'))
    for k in range(5):
        ang = rng.uniform(0, 2 * math.pi)
        z0 = rng.uniform(0.8, 1.4)
        ln = rng.uniform(0.3, 0.45)
        a = (0.52, 0.48, z0)
        b = (0.52 + math.cos(ang) * ln, 0.48 + math.sin(ang) * ln, z0 + rng.uniform(0.2, 0.45))
        s.add(lambda p, a=a, b=b: sd_capsule(p, a, b, 0.04), 'bark', color=rgb('#6c5c4a'))
    return s, 1, 1


# ---------------------------------------------------------------------------------------------
# Tileset assembly

class Tileset:
    def __init__(self, quant):
        self.q = quant
        self.frames = []
        self.frame_ids = {}
        self.pieces = []  # list of (16 entries, sol)
        self.named = []   # (name, category, w, h, [piece ids row-major])
        self.dropped = 0
        # piece 0: the void outside the map (no art, solid)
        self.pieces.append(([0] * 16, SOLID | BLOCK_MISSILE | BLOCK_LIGHT))

    def frame(self, data, ftype):
        key = (ftype, data)
        if key not in self.frame_ids:
            self.frames.append(data)
            self.frame_ids[key] = len(self.frames)  # 1-based
            if len(self.frames) > 4095:
                raise RuntimeError('too many frames')
        return self.frame_ids[key] | (ftype << 12)

    def add_piece(self, blocks, sol):
        """blocks: dict b -> (ftype, data)."""
        entries = [0] * 16
        for b, (ftype, data) in blocks.items():
            entries[block_index(b)] = self.frame(data, ftype)
        self.pieces.append((entries, sol))
        return len(self.pieces) - 1

    def flat(self, name, category, fn, W=1, H=1):
        """Floor pieces (triangles) for a flat area rendered with a ground function."""
        rgbimg, mask, ox, oy = render(Scene(), W, H, fn, material_fn, ztop=0.5, clip=False)
        idx = self.q(rgbimg, True)
        ids = []
        for j in range(H):
            for i in range(W):
                tcx = (i - j) * 32 + ox      # tile centre x in the image
                tcy = (i + j + 1) * 16 + oy  # tile centre y in the image
                left = [[None] * 32 for _ in range(32)]
                right = [[None] * 32 for _ in range(32)]
                for t in range(31):
                    w = tri_width(t)
                    yb = 31 - t
                    yy = tcy - 16 + yb
                    for x in range(w):
                        xl = 32 - w + x
                        left[yb][xl] = int(idx[yy, tcx - 32 + xl])
                        right[yb][x] = int(idx[yy, tcx + x])
                pid = self.add_piece({0: (TYPE_LTRI, encode_triangle(left, True)), 1: (TYPE_RTRI, encode_triangle(right, False))}, 0)
                ids.append(pid)
        self.named.append((name, category, W, H, ids))
        return ids

    def structure(self, name, category, scene, W, H, sol=SOLID | BLOCK_MISSILE, ground=None, ztop=5.0):
        ground = ground or (lambda x, y: sand(x, y))
        rgbimg, mask, ox, oy = render(scene, W, H, ground, material_fn, ztop=ztop, clip=False)
        surface = render.surface
        idx = self.q(rgbimg, True)
        h, w = mask.shape
        # Ground pixels only where they fall exactly inside a footprint tile's diamond (the same
        # pixels the floor triangles cover), so the piece meets its neighbours seamlessly.
        diamond = np.zeros((h, w), bool)
        for j in range(H):
            for i in range(W):
                tcx = (i - j) * 32 + ox
                tcy = (i + j + 1) * 16 + oy
                for t in range(31):
                    wdt = tri_width(t)
                    yy = tcy - 16 + 31 - t
                    if 0 <= yy < h:
                        diamond[yy, max(tcx - wdt, 0):min(tcx + wdt, w)] = True
        mask = surface | (mask & diamond)
        # assign every pixel to the frontmost footprint tile whose piece column holds it
        owner = np.full((h, w), -1)
        local = {}
        py, px = np.mgrid[0:h, 0:w]
        best = np.full((h, w), -1)
        for j in range(H):
            for i in range(W):
                tcx = (i - j) * 32 + ox
                tcy = (i + j + 1) * 16 + oy
                xl = px - tcx + 32
                yl = py - tcy + 240
                ok = mask & (xl >= 0) & (xl < 64) & (yl >= 0) & (yl < 256)
                depth = i + j
                take = ok & (depth > best)
                best = np.where(take, depth, best)
                owner = np.where(take, j * W + i, owner)
                local[(i, j)] = (tcx, tcy)
        self.dropped += int((mask & (owner < 0)).sum())
        ids = []
        for j in range(H):
            for i in range(W):
                tcx, tcy = local[(i, j)]
                mine = owner == (j * W + i)
                blocks = {}
                for b in range(16):
                    r, s_ = b >> 1, b & 1
                    g = [[None] * 32 for _ in range(32)]
                    any_px = False
                    y0 = tcy - 240 + 256 - 32 * (r + 1)
                    x0 = tcx - 32 + 32 * s_
                    for yb in range(32):
                        yy = y0 + yb
                        if yy < 0 or yy >= h:
                            continue
                        for xb in range(32):
                            xx = x0 + xb
                            if 0 <= xx < w and mine[yy, xx]:
                                g[yb][xb] = int(idx[yy, xx])
                                any_px = True
                    if any_px:
                        blocks[b] = (TYPE_TRANSPARENT, encode_transparent(g))
                ids.append(self.add_piece(blocks, sol))
        self.named.append((name, category, W, H, ids))
        return ids

    def write(self, out_dir):
        d = os.path.join(out_dir, 'levels', 'desertdata')
        os.makedirs(d, exist_ok=True)
        write_cel(os.path.join(d, 'desert.cel'), self.frames)
        with open(os.path.join(d, 'desert.min'), 'wb') as fh:
            for entries, _ in self.pieces:
                fh.write(struct.pack('<16H', *entries))
        with open(os.path.join(d, 'desert.sol'), 'wb') as fh:
            fh.write(bytes(sol for _, sol in self.pieces))
        with open(os.path.join(d, 'desert.til'), 'wb') as fh:
            floors = [n for n in self.named if n[1] == 'floor']
            for name, cat, w, h, ids in floors[:8]:
                fh.write(struct.pack('<4H', ids[0], ids[0], ids[0], ids[0]))
        t = os.path.join(out_dir, 'txtdata', 'desert')
        os.makedirs(t, exist_ok=True)
        with open(os.path.join(t, 'pieces.tsv'), 'w', newline='') as fh:
            fh.write('name\tcategory\twidth\theight\tpieces\r\n')
            for name, cat, w, h, ids in self.named:
                fh.write('%s\t%s\t%d\t%d\t%s\r\n' % (name, cat, w, h, ','.join(str(i) for i in ids)))


def build(pal_path, out_dir):
    pal = load_pal(pal_path)
    q = Quantizer(pal)
    ts = Tileset(q)
    # floors
    for k in range(6):
        ts.flat('sand%d' % k, 'floor', floor_variant('sand', 100 + k))
    for k in range(2):
        ts.flat('pebbles%d' % k, 'floordetail', floor_variant('pebbles', 200 + k))
        ts.flat('hardpan%d' % k, 'floordetail', floor_variant('hardpan', 300 + k))
        ts.flat('scrub%d' % k, 'floordetail', floor_variant('scrub', 400 + k))
    ts.flat('bones0', 'floordetail', floor_variant('bones', 500))
    ts.flat('drift0', 'floordetail', floor_variant('drift', 600))
    ts.flat('drift1', 'floordetail', floor_variant('drift', 601))
    print('floors done, frames', len(ts.frames))
    # rugs (flat, walkable) laid out in front of the tents
    for name, col, acc in [('rug_red', RUG_RED, RUG_BLUE), ('rug_blue', RUG_BLUE, RUG_RED)]:
        def rugfn(x, y, col=col, acc=acc):
            base, nz = sand(x, y)
            du = np.abs(x - 1.0) / 0.78
            dv = np.abs(y - 1.0) / 0.78
            edge = np.maximum(du, dv)
            c = np.repeat(col[:, None], x.size, 1)
            c = np.where(edge > 0.8, RUG_GOLD[:, None], c)
            diamond = (np.abs(np.mod(x * 3, 1.0) - 0.5) + np.abs(np.mod(y * 3, 1.0) - 0.5)) < 0.22
            c = np.where((edge < 0.68) & diamond, acc[:, None], c)
            c = c * (0.82 + 0.25 * vnoise3(x * 50, y * 50, np.zeros_like(x), 49))
            return np.where(edge < 1.0, c, base), nz
        ts.flat(name, 'rug', rugfn, 2, 2)
    # structures
    builders = [
        ('tent_big', 'structure', tent_big),
        ('tent_small', 'structure', lambda: tent_small()),
        ('tent_small_b', 'structure', lambda: tent_small(rgb('#b9a27a'), CANVAS_BLUE)),
        ('yurt', 'structure', yurt),
        ('hut', 'structure', hut),
        ('shack', 'structure', shack),
        ('leanto', 'structure', leanto),
        ('forge', 'forge', forge),
        ('stall', 'stall', lambda: stall()),
        ('stall_b', 'stall', lambda: stall(CANVAS_BLUE)),
        ('well', 'well', well),
        ('crates', 'clutter', crates),
        ('barrels', 'clutter', barrels),
        ('sacks', 'clutter', sacks),
        ('urns', 'clutter', urns),
        ('campfire', 'clutter', campfire),
        ('totem', 'clutter', totem),
    ]
    for name, cat, fn in builders:
        s, W, H = fn()
        ts.structure(name, cat, s, W, H)
        print(name, 'frames', len(ts.frames), 'pieces', len(ts.pieces))
    for k in range(5):
        s, W, H = rock(700 + k)
        ts.structure('rock%d' % k, 'rock', s, W, H, sol=SOLID | BLOCK_MISSILE | BLOCK_LIGHT)
    for k in range(4):
        s, W, H = rock(800 + k, tall=True)
        ts.structure('pillar%d' % k, 'pillar', s, W, H, sol=SOLID | BLOCK_MISSILE | BLOCK_LIGHT)
    for k in range(2):
        s, W, H = rock(900 + k, size=2)
        ts.structure('boulder%d' % k, 'boulder', s, W, H, sol=SOLID | BLOCK_MISSILE | BLOCK_LIGHT)
    for k in range(2):
        s, W, H = palm(1000 + k)
        ts.structure('palm%d' % k, 'tree', s, W, H, sol=SOLID | BLOCK_MISSILE)
        s, W, H = dead_tree(1100 + k)
        ts.structure('deadtree%d' % k, 'tree', s, W, H, sol=SOLID | BLOCK_MISSILE)
    print('total frames', len(ts.frames), 'pieces', len(ts.pieces), 'dropped px', ts.dropped)
    ts.write(out_dir)
    return ts, pal


# ---------------------------------------------------------------------------------------------
# Preview: composite pieces the way the engine draws them

def decode_piece_images(ts, pal):
    from dtiles import decode_frame
    imgs = []
    frames = ts.frames
    for entries, _ in ts.pieces:
        im = np.zeros((256, 64, 4), np.uint8)
        for b in range(16):
            v = entries[block_index(b)]
            if not v:
                continue
            g = decode_frame(frames[(v & 0xFFF) - 1], (v >> 12) & 7)
            r, s_ = b >> 1, b & 1
            for y in range(32):
                for x in range(32):
                    if g[y][x] is not None:
                        im[256 - 32 * (r + 1) + y, 32 * s_ + x, :3] = pal[g[y][x]]
                        im[256 - 32 * (r + 1) + y, 32 * s_ + x, 3] = 255
        imgs.append(im)
    return imgs


def preview(ts, pal, path):
    from PIL import Image
    imgs = decode_piece_images(ts, pal)
    named = {n[0]: n for n in ts.named}
    N = 16
    grid = [[None] * N for _ in range(N)]
    rng = np.random.default_rng(3)
    sands = [n for n in ts.named if n[1] == 'floor']
    details = [n for n in ts.named if n[1] == 'floordetail']
    for y in range(N):
        for x in range(N):
            src = sands[rng.integers(len(sands))] if rng.random() > 0.15 else details[rng.integers(len(details))]
            grid[y][x] = src[4][0]

    def place(name, x, y):
        n = named[name]
        for j in range(n[3]):
            for i in range(n[2]):
                grid[y + j][x + i] = n[4][j * n[2] + i]
    place('tent_big', 1, 1)
    place('yurt', 7, 1)
    place('hut', 11, 1)
    place('forge', 1, 6)
    place('well', 6, 6)
    place('stall', 9, 6)
    place('tent_small', 13, 6)
    place('shack', 1, 10)
    place('leanto', 5, 10)
    place('rug_red', 8, 10)
    place('crates', 11, 10)
    place('barrels', 12, 10)
    place('sacks', 13, 10)
    place('urns', 14, 10)
    place('campfire', 11, 12)
    place('totem', 13, 12)
    place('palm0', 1, 14)
    place('palm1', 3, 14)
    place('deadtree0', 5, 14)
    place('rock0', 7, 14)
    place('rock1', 8, 14)
    place('pillar0', 10, 14)
    place('pillar1', 11, 14)
    place('boulder0', 13, 13)
    Wd, Hd = 64 * N + 64, 32 * N + 300
    canvas = Image.new('RGBA', (Wd, Hd), (20, 16, 12, 255))
    ox, oy = 32 * N, 280

    def blit(pid, x, y, floor_only=False, upper=False):
        im = imgs[pid]
        sx = (x - y) * 32 + ox - 32
        sy = (x + y + 1) * 16 + oy - 240
        part = im.copy()
        if floor_only:
            part[:224] = 0
        tile = Image.fromarray(part, 'RGBA')
        canvas.alpha_composite(tile, (sx, sy))
    for s in range(2 * N):
        for y in range(N):
            x = s - y
            if 0 <= x < N and ts.pieces[grid[y][x]][1] == 0:
                blit(grid[y][x], x, y, floor_only=True)
    for s in range(2 * N):
        for y in range(N):
            x = s - y
            if 0 <= x < N and ts.pieces[grid[y][x]][1] != 0:
                blit(grid[y][x], x, y)
    canvas.save(path)




# ---------------------------------------------------------------------------------------------
# Sprites: pack cow bags overlay, NPC colour translations

def encode_sprite_frame(idx, mask):
    """Standard CEL sprite frame: rows bottom-up, runs of transparent (0x80+) / opaque pixels."""
    h, w = mask.shape
    out = bytearray()
    for y in range(h - 1, -1, -1):
        x = 0
        while x < w:
            if not mask[y, x]:
                n = 0
                while x < w and not mask[y, x] and n < 127:
                    n += 1
                    x += 1
                out.append(256 - n)
            else:
                run = []
                while x < w and mask[y, x] and len(run) < 127:
                    run.append(int(idx[y, x]))
                    x += 1
                out.append(len(run))
                out.extend(run)
    return bytes(out)


def packbags(quant, out_dir, anchor=(66, 56)):
    """Bags piled on a cow facing south-west (its body runs along world y)."""
    s = Scene()
    # saddle blanket draped over the back
    s.add(lambda p: sd_box(p, (0.0, 0.0, 0.0), (0.24, 0.3, 0.025), 0.02), 'rug', rect=(0.0, 0.0, 0.24, 0.3), color=RUG_RED)
    # big sack hanging on the near (+x) flank, a smaller one peeking over the far side
    s.add(lambda p: sd_ellipsoid(p, (0.26, 0.02, -0.12), (0.12, 0.24, 0.2)), 'burlap', color=BURLAP)
    s.add(lambda p: sd_ellipsoid(p, (0.27, -0.2, -0.05), (0.1, 0.12, 0.13)), 'burlap', color=rgb('#86683f'))
    s.add(lambda p: sd_ellipsoid(p, (-0.2, 0.05, 0.06), (0.1, 0.2, 0.12)), 'burlap', color=rgb('#7d6240'))
    # rolled rug and a bundle tied on top, a pot hanging off the side
    s.add(lambda p: sd_capsule(p, (-0.12, -0.18, 0.11), (0.12, -0.18, 0.11), 0.06), 'canvas', color=CANVAS_BLUE, stripe=(0, 0.06, CANVAS, 0.3))
    s.add(lambda p: sd_box(p, (0.0, 0.1, 0.12), (0.12, 0.12, 0.08), 0.03), 'canvas', color=CANVAS, patches=None)
    s.add(lambda p: sd_cyl(p, (0.33, 0.22), 0.06, -0.3, -0.16), 'clay', color=CLAY)
    s.add(lambda p: sd_capsule(p, (0.0, 0.1, 0.2), (0.33, 0.22, -0.16), 0.008), 'burlap', color=rgb('#5a4630'))
    # render in a local frame centred on the back
    width = height = 128
    zt = 1.0
    py, px = np.mgrid[0:height, 0:width]
    sx = (px - anchor[0] + 0.5).ravel().astype(float)
    sy = (py - anchor[1] + 0.5).ravel().astype(float)
    from raytrace import CAM, normal, soft_shadow, SUN, SUN_COLOR, SKY_COLOR
    a = sx / 32.0
    b0 = (sy + K * zt) / 16.0
    p0 = np.array([(a + b0) / 2, (b0 - a) / 2, np.full_like(a, zt)])
    t = np.zeros_like(a)
    hit = np.zeros_like(a, bool)
    active = np.ones_like(a, bool)
    for _ in range(200):
        idxs = np.nonzero(active)[0]
        if idxs.size == 0:
            break
        p = p0[:, idxs] - CAM[:, None] * t[idxs]
        d = s.dist(p)
        h_ = d < 0.0015
        hit[idxs[h_]] = True
        t[idxs] += np.where(h_, 0, np.maximum(d * 0.8, 0.002))
        done = h_ | (t[idxs] > 3.0)
        active[idxs[done]] = False
    hi = np.nonzero(hit)[0]
    rgbimg = np.zeros((a.size, 3))
    ph = p0[:, hi] - CAM[:, None] * t[hi]
    _, mat = s.dist(ph, want_mat=True)
    n = normal(s, ph)
    albedo, emissive = material_fn(s, ph, n, mat)
    ndl = np.clip((n * SUN[:, None]).sum(0), 0, 1)
    light = SKY_COLOR[:, None] * (0.36 + 0.12 * n[2]) + SUN_COLOR[:, None] * 0.62 * ndl * soft_shadow(s, ph + n * 0.01)
    rgbimg[hi] = (albedo * light + emissive).T
    mask = hit.reshape(height, width)
    idx = quant(rgbimg.reshape(height, width, 3), True)
    frame = encode_sprite_frame(idx, mask)
    d = os.path.join(out_dir, 'towners', 'desert')
    os.makedirs(d, exist_ok=True)
    write_cel(os.path.join(d, 'packbags.cel'), [frame] * 12)
    return idx, mask


def hue_trn(pal, rules):
    """256-entry colour translation: rules are functions (h, s, v) -> target rgb or None."""
    trn = list(range(256))
    pal_hsv = [colorsys.rgb_to_hsv(*(pal[i] / 255.0)) for i in range(256)]
    for i in range(256):
        h, s_, v = pal_hsv[i]
        for rule in rules:
            target = rule(h, s_, v)
            if target is not None:
                tr, tg, tb = target
                best, bd = i, 1e18
                for j in range(1, 255):
                    r, g, b = pal[j] / 255.0
                    d = 2 * (r - tr) ** 2 + 4 * (g - tg) ** 2 + 3 * (b - tb) ** 2
                    if d < bd:
                        bd, best = d, j
                trn[i] = best
                break
    return bytes(trn)


def npc_trns(pal, out_dir):
    d = os.path.join(out_dir, 'towners', 'desert')
    os.makedirs(d, exist_ok=True)

    def hsv(h, s_, v):
        return colorsys.hsv_to_rgb(h % 1.0, max(0, min(1, s_)), max(0, min(1, v)))

    # Desert smith: Griswold's red tunic becomes indigo, his leather a sun-bleached tan.
    smith = hue_trn(pal, [
        lambda h, s_, v: hsv(0.64, s_ * 0.75, v * 0.95) if (h < 0.04 or h > 0.93) and s_ > 0.45 and v > 0.12 else None,
        lambda h, s_, v: hsv(0.10, s_ * 0.6, min(1, v * 1.15)) if 0.04 <= h < 0.12 and s_ > 0.5 and v > 0.15 else None,
    ])
    # Priest: the black habit becomes white linen with a golden sash (greys -> ivory).
    # Priest: explicit ramp moves (the habit is almost black, hue rules can't see it).
    # Dark greys and the near-black browns become a tan linen robe; the light rope belt turns gold.
    priest = bytearray(range(256))
    # The habit's core is palette black (shared with the shadow), so the folds carry the colour:
    # greys become a deep crimson ramp and the robe reads as dark red cloth.
    for k, i in enumerate(range(254, 247, -1)):
        priest[i] = 171 - k  # 254 -> 171 ... 248 -> 165
    for i, t in [(175, 173), (174, 172), (173, 170), (172, 169), (159, 172), (223, 172), (222, 171)]:
        priest[i] = t
    for k, i in enumerate(range(247, 239, -1)):
        priest[i] = 150 - min(k, 6)  # rope belt: golden yellows
    priest = bytes(priest)
    # Caravan master: olive tunic -> burnt orange, red hood -> indigo wrap.
    caravan = bytearray(range(256))
    for k in range(4, 15):
        caravan[192 + k] = 208 + k
    for i, t in [(173, 184), (174, 186), (175, 188), (220, 183), (221, 185), (222, 187)]:
        caravan[i] = t
    caravan = bytes(caravan)
    for name, trn in [('smith', smith), ('priest', priest), ('caravan', caravan)]:
        open(os.path.join(d, name + '.trn'), 'wb').write(trn)
    return {'smith': smith, 'priest': priest, 'caravan': caravan}


if __name__ == '__main__':
    ts, pal = build(sys.argv[1], sys.argv[2])
    packbags(ts.q, sys.argv[2])
    npc_trns(pal, sys.argv[2])
    if len(sys.argv) > 3:
        preview(ts, pal, sys.argv[3])
