"""Diablo level tile formats: CEL frames (TransparentSquare / triangles), MIN pieces, SOL, TIL."""
import struct

TYPE_SQUARE, TYPE_TRANSPARENT, TYPE_LTRI, TYPE_RTRI, TYPE_LTRAP, TYPE_RTRAP = range(6)


def tri_width(i):
    return 2 * (i + 1) if i < 16 else 2 * (31 - i)


def decode_frame(data, ftype):
    """Returns 32x32 grid (rows top->bottom) of palette index or None."""
    g = [[None] * 32 for _ in range(32)]
    p = 0
    if ftype == TYPE_TRANSPARENT:
        y = 31
        x = 0
        while y >= 0 and p < len(data):
            c = data[p]; p += 1
            if c >= 0x80:
                x += 256 - c
            else:
                for k in range(c):
                    g[y][x] = data[p]; p += 1; x += 1
            if x >= 32:
                x = 0; y -= 1
        return g
    if ftype == TYPE_SQUARE:
        for i in range(32):
            for x in range(32):
                g[31 - i][x] = data[p]; p += 1
        return g
    if ftype in (TYPE_LTRI, TYPE_RTRI):
        for i in range(31):
            w = tri_width(i)
            if ftype == TYPE_LTRI and i % 2 == 0:
                p += 2
            x0 = 32 - w if ftype == TYPE_LTRI else 0
            for x in range(w):
                g[31 - i][x0 + x] = data[p]; p += 1
            if ftype == TYPE_RTRI and i % 2 == 0:
                p += 2
        return g
    raise ValueError(ftype)


def encode_transparent(g):
    """g: 32 rows top->bottom of index/None. RLE, bottom row first, runs don't cross rows."""
    out = bytearray()
    for y in range(31, -1, -1):
        row = g[y]
        x = 0
        while x < 32:
            if row[x] is None:
                n = 0
                while x < 32 and row[x] is None and n < 127:
                    n += 1; x += 1
                out.append(256 - n)
            else:
                run = []
                while x < 32 and row[x] is not None and len(run) < 127:
                    run.append(row[x]); x += 1
                out.append(len(run)); out.extend(run)
    return bytes(out)


def encode_triangle(g, left):
    out = bytearray()
    for i in range(31):
        w = tri_width(i)
        y = 31 - i
        x0 = 32 - w if left else 0
        if left and i % 2 == 0:
            out += b'\0\0'
        for x in range(w):
            v = g[y][x0 + x]
            out.append(0 if v is None else v)
        if not left and i % 2 == 0:
            out += b'\0\0'
    return bytes(out)


def write_cel(path, frames):
    n = len(frames)
    header = 4 * (n + 2)
    offs = [header]
    for f in frames:
        offs.append(offs[-1] + len(f))
    with open(path, 'wb') as fh:
        fh.write(struct.pack('<I', n))
        fh.write(struct.pack('<%dI' % (n + 1), *offs))
        for f in frames:
            fh.write(f)


def read_cel(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 0)[0]
    offs = [struct.unpack_from('<I', d, 4 * (i + 1))[0] for i in range(n + 1)]
    return [d[offs[i]:offs[i + 1]] for i in range(n)]


def block_index(b):
    """MIN entry index for block b (b=0 bottom-left, b=1 bottom-right, b=2 next row left...)."""
    return 14 + (b & 1) - (b & 0xE)
