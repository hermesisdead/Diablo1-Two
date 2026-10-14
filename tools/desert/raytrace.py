"""Tiny isometric SDF raytracer for Diablo-style tile art.

World units: one tile = 1 unit on the ground; heights in the same units.
Screen: sx = (x - y) * 32, sy = (x + y) * 16 - z * K.
"""
import numpy as np

K = 39.0  # screen pixels per unit of height (2:1 dimetric, 30 degree elevation)
CAM = np.array([K / 32.0, K / 32.0, 1.0])
CAM = CAM / np.linalg.norm(CAM)  # points toward the camera
SUN = np.array([-0.55, -0.1, 1.25])
SUN = SUN / np.linalg.norm(SUN)
SUN_COLOR = np.array([1.06, 0.98, 0.86])
SKY_COLOR = np.array([0.80, 0.86, 1.00])


# --------------------------------------------------------------------------------------------
# Noise

def _hash3(ix, iy, iz, seed):
    h = (ix.astype(np.int64) * 73856093) ^ (iy.astype(np.int64) * 19349663) ^ (iz.astype(np.int64) * 83492791) ^ (seed * 2654435761)
    h = (h ^ (h >> 13)) * 1274126177
    h = h ^ (h >> 16)
    return (h & 0xFFFF).astype(np.float64) / 65535.0


def vnoise3(x, y, z, seed=0):
    ix, iy, iz = np.floor(x), np.floor(y), np.floor(z)
    fx, fy, fz = x - ix, y - iy, z - iz
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)
    fz = fz * fz * (3 - 2 * fz)
    ix, iy, iz = ix.astype(np.int64), iy.astype(np.int64), iz.astype(np.int64)
    out = 0
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                w = (fx if dx else 1 - fx) * (fy if dy else 1 - fy) * (fz if dz else 1 - fz)
                out = out + w * _hash3(ix + dx, iy + dy, iz + dz, seed)
    return out


def fbm3(x, y, z, seed=0, octaves=4):
    a, f, s, norm = 1.0, 1.0, 0.0, 0.0
    for o in range(octaves):
        s = s + a * vnoise3(x * f, y * f, z * f, seed + o * 17)
        norm += a
        a *= 0.5
        f *= 2.03
    return s / norm


def pnoise2(u, v, period, seed=0):
    """Value noise on a lattice that wraps every `period` cells (tileable)."""
    x, y = u * period, v * period
    ix, iy = np.floor(x), np.floor(y)
    fx, fy = x - ix, y - iy
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)
    ix, iy = ix.astype(np.int64) % period, iy.astype(np.int64) % period
    z0 = np.zeros_like(ix)
    out = 0
    for dx in (0, 1):
        for dy in (0, 1):
            w = (fx if dx else 1 - fx) * (fy if dy else 1 - fy)
            out = out + w * _hash3((ix + dx) % period, (iy + dy) % period, z0, seed)
    return out


def pfbm2(u, v, seed=0, base=4, octaves=4):
    a, s, norm = 1.0, 0.0, 0.0
    p = base
    for o in range(octaves):
        s = s + a * pnoise2(u, v, p, seed + o * 31)
        norm += a
        a *= 0.5
        p *= 2
    return s / norm


# --------------------------------------------------------------------------------------------
# SDF primitives (p is a (3, N) array)

def length(*c):
    return np.sqrt(sum(ci * ci for ci in c))


def sd_box(p, c, h, r=0.0):
    q = [np.abs(p[i] - c[i]) - h[i] for i in range(3)]
    out = length(*(np.maximum(qi, 0) for qi in q)) + np.minimum(np.maximum(np.maximum(q[0], q[1]), q[2]), 0)
    return out - r


def sd_cyl(p, c, r, z0, z1):
    d2 = length(p[0] - c[0], p[1] - c[1]) - r
    dz = np.maximum(z0 - p[2], p[2] - z1)
    return np.minimum(np.maximum(d2, dz), 0) + length(np.maximum(d2, 0), np.maximum(dz, 0))


def sd_cone(p, c, r0, z0, z1):
    h = z1 - z0
    rz = r0 * np.clip((z1 - p[2]) / h, 0, None)
    d = (length(p[0] - c[0], p[1] - c[1]) - rz) * (h / np.sqrt(h * h + r0 * r0))
    return np.maximum(d, np.maximum(z0 - p[2], p[2] - z1))


def sd_capsule(p, a, b, r):
    a = np.asarray(a, float)[:, None]
    b = np.asarray(b, float)[:, None]
    pa = p - a
    ba = b - a
    t = np.clip((pa * ba).sum(0) / (ba * ba).sum(), 0, 1)
    d = pa - ba * t
    return length(*d) - r


def sd_ellipsoid(p, c, r):
    q = [(p[i] - c[i]) / r[i] for i in range(3)]
    k = length(*q)
    return (k - 1.0) * min(r)


def sd_plane_slab(p, f, th, x0, x1, y0, y1):
    """A thin sheet z = f(x, y) over a rectangle (cloth roofs and awnings)."""
    d = np.abs(p[2] - f(p[0], p[1])) - th
    dx = np.maximum(x0 - p[0], p[0] - x1)
    dy = np.maximum(y0 - p[1], p[1] - y1)
    return np.maximum(d * 0.7, np.maximum(dx, dy))


def sd_ridge(p, axis, a0, a1, c, w, h, z0=0.0):
    """Triangular prism (ridge tent) along x (axis=0) or y (axis=1): base half-width w, ridge height h."""
    along = p[axis]
    across = p[1 - axis] - c
    z = p[2] - z0
    n = np.sqrt(h * h + w * w)
    d1 = (np.abs(across) * h + z * w - h * w) / n
    return np.maximum(np.maximum(d1, -z), np.maximum(a0 - along, along - a1))


# --------------------------------------------------------------------------------------------
# Scene

class Prim:
    def __init__(self, sdf, mat, **params):
        self.sdf = sdf
        self.mat = mat
        self.params = params


class Scene:
    def __init__(self):
        self.prims = []
        self.subtract = []  # (sdf) carved out of everything

    def add(self, sdf, mat, **params):
        self.prims.append(Prim(sdf, mat, **params))
        return self

    def carve(self, sdf):
        self.subtract.append(sdf)
        return self

    def dist(self, p, want_mat=False):
        best = np.full(p.shape[1], 1e9)
        mat = np.full(p.shape[1], -1)
        for i, pr in enumerate(self.prims):
            d = pr.sdf(p)
            m = d < best
            best = np.where(m, d, best)
            mat = np.where(m, i, mat)
        for s in self.subtract:
            best = np.maximum(best, -s(p))
        return (best, mat) if want_mat else best


def normal(scene, p, eps=0.004):
    n = []
    for i in range(3):
        e = np.zeros((3, 1))
        e[i] = eps
        n.append(scene.dist(p + e) - scene.dist(p - e))
    n = np.array(n)
    return n / (np.linalg.norm(n, axis=0) + 1e-9)


def soft_shadow(scene, p, steps=48, maxt=4.0):
    res = np.ones(p.shape[1])
    t = np.full(p.shape[1], 0.02)
    L = SUN[:, None]
    for _ in range(steps):
        q = p + L * t
        d = scene.dist(q)
        res = np.minimum(res, np.clip(9.0 * d / t, 0, 1))
        t = t + np.clip(d, 0.01, 0.25)
        if (t > maxt).all():
            break
    return np.clip(res, 0, 1)


def render(scene, W, H, ground_fn, material_fn, ztop=5.0, footprint_ground=True, aa=1, clip=True):
    """Renders a structure whose footprint is W x H tiles at origin.

    Returns (rgb float HxWx3, mask bool, ox, oy) where screen (sx, sy) = (px - ox, py - oy).
    """
    ox = 32 * H
    oy = int(np.ceil(K * ztop)) + 8
    width = 32 * (W + H)
    height = 16 * (W + H) + oy + 2
    py, px = np.mgrid[0:height, 0:width]
    sx = (px - ox + 0.5).ravel().astype(float)
    sy = (py - oy + 0.5).ravel().astype(float)
    a = sx / 32.0
    b0 = (sy + K * ztop) / 16.0
    p0 = np.array([(a + b0) / 2, (b0 - a) / 2, np.full_like(a, ztop)])
    C = CAM[:, None]
    tg = ztop / CAM[2]
    t = np.zeros_like(a)
    active = np.ones_like(a, bool)
    hit = np.zeros_like(a, bool)
    for _ in range(220):
        if not active.any():
            break
        idx = np.nonzero(active)[0]
        p = p0[:, idx] - C * t[idx]
        d = scene.dist(p) if scene.prims else np.full(idx.size, 1e9)
        h = d < 0.0015
        hit[idx[h]] = True
        t[idx] += np.where(h, 0, np.maximum(d * 0.8, 0.002))
        done = h | (t[idx] >= tg)
        active[idx[done]] = False
    t = np.where(hit, t, tg)
    p = p0 - C * t
    rgb = np.zeros((3, a.size))
    mask = np.zeros(a.size, bool)

    # Surfaces.
    hi = np.nonzero(hit & (t < tg))[0]
    if hi.size:
        ph = p[:, hi]
        _, mat = scene.dist(ph, want_mat=True)
        n = normal(scene, ph)
        sh = soft_shadow(scene, ph + n * 0.01)
        ao = np.clip(0.45 + 0.55 * scene.dist(ph + n * 0.12) / 0.12, 0.35, 1.0)
        albedo, emissive = material_fn(scene, ph, n, mat)
        ndl = np.clip((n * SUN[:, None]).sum(0), 0, 1)
        light = SKY_COLOR[:, None] * (0.36 + 0.12 * n[2]) * ao + SUN_COLOR[:, None] * 0.62 * ndl * sh
        rgb[:, hi] = albedo * light + emissive
        mask[hi] = True

    # Ground inside the footprint.
    gi = np.nonzero(~(hit & (t < tg)))[0]
    if gi.size and footprint_ground:
        pg = p[:, gi]
        inside = (pg[0] >= 0) & (pg[0] < W) & (pg[1] >= 0) & (pg[1] < H) if clip else np.ones(pg.shape[1], bool)
        gi = gi[inside]
        pg = pg[:, inside]
        if gi.size:
            n = np.array([np.zeros(gi.size), np.zeros(gi.size), np.ones(gi.size)])
            if scene.prims:
                sh = soft_shadow(scene, pg + n * 0.01)
                ao = np.clip(0.55 + 0.45 * scene.dist(pg + n * 0.15) / 0.15, 0.45, 1.0)
                # Fade shadows out toward the footprint edge: the neighbouring tiles cannot show them.
                edge = np.minimum(np.minimum(pg[0], W - pg[0]), np.minimum(pg[1], H - pg[1]))
                fadeout = np.clip(edge / 0.3, 0, 1)
                sh = 1 - (1 - np.maximum(sh, 0.35)) * fadeout
                ao = 1 - (1 - ao) * fadeout
            else:
                sh = np.ones(gi.size)
                ao = np.ones(gi.size)
            albedo, nz = ground_fn(pg[0], pg[1])
            ndl = np.clip(nz * SUN[2], 0, 1)
            light = SKY_COLOR[:, None] * 0.36 * ao + SUN_COLOR[:, None] * 0.62 * ndl * sh
            rgb[:, gi] = albedo * light
            mask[gi] = True
    surface = np.zeros(a.size, bool)
    surface[hi] = True
    render.surface = surface.reshape(height, width)
    return rgb.T.reshape(height, width, 3), mask.reshape(height, width), ox, oy
