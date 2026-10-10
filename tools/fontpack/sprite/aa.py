"""Anti-aliased fills and strokes in cell coordinates (floats, (0, 0) = cell top left), rasterised like z2d's
multisample_4x (the rasterizer Ghostty's sprites use):

    p = Path(c); p.move_to(x, y); p.line_to(x, y); p.curve_to(x1, y1, x2, y2, x3, y3); p.arc(xc, yc, r, a0, a1)
    p.close(); fill(c, p); stroke(c, p, width); inner_stroke(c, p, width); line(c, ...); polygon(c, pts)

Each call rasterises its whole shape once (non-zero union) and composites it with z2d's integer src-over.
"""
import math

from .common import zround

TOLERANCE = 0.1
MITER_LIMIT = 10.0


class Path:
    """Path in device coordinates (cell + padding), as z2d stores it."""

    def __init__(self, c):
        self.ox, self.oy = c.pad_x, c.pad_y
        self.nodes = []
        self.cur = None
        self.start = None

    def _dev(self, x, y):
        return (x + self.ox, y + self.oy)

    def move_to(self, x, y):
        p = self._dev(x, y)
        self.nodes.append(("M", p))
        self.cur = self.start = p

    def line_to(self, x, y):
        if self.cur is None:
            return self.move_to(x, y)
        p = self._dev(x, y)
        self.nodes.append(("L", p))
        self.cur = p

    def curve_to(self, x1, y1, x2, y2, x3, y3):
        if self.cur is None:
            self.move_to(x1, y1)
        p3 = self._dev(x3, y3)
        self.nodes.append(("C", self._dev(x1, y1), self._dev(x2, y2), p3))
        self.cur = p3

    def close(self):
        if self.cur is not None:
            self.nodes.append(("Z",))
            self.cur = self.start

    def arc(self, xc, yc, r, a1, a2):
        while a2 < a1:
            a2 += 2 * math.pi
        self._arc(xc, yc, r, a1, a2)

    def _arc(self, xc, yc, r, amin, amax):
        if amax - amin > math.pi:
            amid = amin + (amax - amin) / 2.0
            self._arc(xc, yc, r, amin, amid)
            self._arc(xc, yc, r, amid, amax)
        elif amax != amin:
            segs = math.ceil(abs(amax - amin) / _arc_max_angle(TOLERANCE / r))
            step = (amax - amin) / segs
            self.line_to(xc + r * math.cos(amin), yc + r * math.sin(amin))
            for _ in range(segs - 1):
                self._arc_segment(xc, yc, r, amin, amin + step)
                amin += step
            self._arc_segment(xc, yc, r, amin, amax)
        else:
            self.line_to(xc + r * math.cos(amin), yc + r * math.sin(amin))

    def _arc_segment(self, xc, yc, r, a, b):
        rsa, rca, rsb, rcb = r * math.sin(a), r * math.cos(a), r * math.sin(b), r * math.cos(b)
        h = 4.0 / 3.0 * math.tan((b - a) / 4.0)
        self.curve_to(xc + rca - h * rsa, yc + rsa + h * rca, xc + rcb + h * rsb, yc + rsb - h * rcb,
                      xc + rcb, yc + rsb)

    def flatten(self, tol=TOLERANCE):
        """[(points, curve_flags, closed)]: curve_flags[i] = segment ending at points[i] came from a curve.
        Consecutive duplicate points are dropped."""
        out, pts, flags, closed = [], None, None, False
        for n in self.nodes:
            k = n[0]
            if k == "M":
                if pts and len(pts) > 1:
                    out.append((pts, flags, closed))
                pts, flags, closed = [n[1]], [False], False
            elif k == "L":
                if n[1] != pts[-1]:
                    pts.append(n[1])
                    flags.append(False)
            elif k == "C":
                seg = []
                _flatten_cubic(pts[-1], n[1], n[2], n[3], tol, seg)
                for p in seg:
                    if p != pts[-1]:
                        pts.append(p)
                        flags.append(True)
            else:
                closed = True
                out.append((pts, flags, closed))
                pts, flags, closed = [pts[0]], [False], False
        if pts and len(pts) > 1:
            out.append((pts, flags, closed))
        return out


def _arc_max_angle(tol):
    for i in range(1, 1000):
        angle = math.pi / i
        err = 2.0 / 27.0 * math.sin(angle / 4) ** 6 / math.cos(angle / 4) ** 2
        if err < tol:
            return angle
    return math.pi / 999


def _half(a, b):
    return (a[0] + (b[0] - a[0]) / 2, a[1] + (b[1] - a[1]) / 2)


def _err_sq(a, b, c, d):
    """Squared distance of the control points from the chord (the subdivision test cairo/z2d use)."""
    bx, by, cx, cy = b[0] - a[0], b[1] - a[1], c[0] - a[0], c[1] - a[1]
    if a != d:
        dx, dy = d[0] - a[0], d[1] - a[1]
        dd = dx * dx + dy * dy
        bd = bx * dx + by * dy
        if bd >= dd:
            bx, by = bx - dx, by - dy
        else:
            bx, by = bx - bd / dd * dx, by - bd / dd * dy
        cd = cx * dx + cy * dy
        if cd >= dd:
            cx, cy = cx - dx, cy - dy
        else:
            cx, cy = cx - cd / dd * dx, cy - cd / dd * dy
    return max(bx * bx + by * by, cx * cx + cy * cy)


def _flatten_cubic(a, b, c, d, tol, out):
    """Append the polyline points after a (subdivide at t=1/2 until the control points are within tol)."""
    if a == b and c == d:
        out.append(d)
        return
    tol2 = tol * tol
    stack = [(a, b, c, d)]
    while stack:
        k = stack.pop()
        if _err_sq(*k) < tol2:
            if k[0] != a:
                out.append(k[0])
            continue
        p0, p1, p2, p3 = k
        ab, bc, cd = _half(p0, p1), _half(p1, p2), _half(p2, p3)
        abbc, bccd = _half(ab, bc), _half(bc, cd)
        mid = _half(abbc, bccd)
        stack.append((mid, bccd, cd, p3))
        stack.append((p0, ab, abbc, mid))
    out.append(d)


def _unit(dx, dy):
    if dx == 0.0:
        return 0.0, (1.0 if dy > 0 else -1.0)
    if dy == 0.0:
        return (1.0 if dx > 0 else -1.0), 0.0
    m = math.hypot(dx, dy)
    return dx / m, dy / m


def _area2(poly):
    s = 0.0
    n = len(poly)
    for i in range(n):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % n]
        s += x0 * y1 - x1 * y0
    return s


def _oriented(poly):
    a = _area2(poly)
    if a == 0:
        return None
    return poly if a > 0 else poly[::-1]


def _pen(hw, tol=TOLERANCE):
    if tol >= hw * 4:
        n = 1
    elif tol >= hw:
        n = 4
    else:
        delta = math.acos(1 - tol / hw)
        n = math.ceil(2 * math.pi / delta) if delta else 4
        n = 4 if n < 4 else n + (n % 2)
    return [(hw * math.cos(2 * math.pi * i / n), hw * math.sin(2 * math.pi * i / n)) for i in range(n)]


def stroke_polys(pts, flags, closed, width):
    """Polygons whose non-zero union is the stroke of one flattened subpath (butt caps).
    Joins at vertex i: round if the outgoing segment came from a curve, else miter (z2d's rule)."""
    hw = width / 2
    if closed and len(pts) > 2 and pts[0] == pts[-1]:
        pts, flags = pts[:-1], flags[:-1]
    n = len(pts)
    if n < 2:
        return []
    segs = [(pts[i], pts[i + 1], flags[i + 1]) for i in range(n - 1)]
    if closed and n > 2:
        segs.append((pts[-1], pts[0], False))
    out, units = [], []
    for p0, p1, _ in segs:
        ux, uy = _unit(p1[0] - p0[0], p1[1] - p0[1])
        units.append((ux, uy))
        ox, oy = -uy * hw, ux * hw
        q = _oriented([(p0[0] + ox, p0[1] + oy), (p1[0] + ox, p1[1] + oy),
                       (p1[0] - ox, p1[1] - oy), (p0[0] - ox, p0[1] - oy)])
        if q:
            out.append(q)
    pen = None
    m = len(segs)
    joins = range(m) if closed and n > 2 else range(1, m)
    for j in joins:
        ia, ib = (j - 1) % m, j
        (ax, ay), (bx, by) = units[ia], units[ib]
        v = segs[ib][0]
        cross = ax * by - ay * bx
        if cross == 0:
            continue
        s = -1.0 if cross > 0 else 1.0
        oa = (v[0] + s * -ay * hw, v[1] + s * ax * hw)
        ob = (v[0] + s * -by * hw, v[1] + s * bx * hw)
        if segs[ib][2]:
            if pen is None:
                pen = _pen(hw)
            a0 = math.atan2(oa[1] - v[1], oa[0] - v[0])
            a1 = math.atan2(ob[1] - v[1], ob[0] - v[0])
            sweep = (a1 - a0) % (2 * math.pi) if cross > 0 else -((a0 - a1) % (2 * math.pi))
            # Pen vertices on the outer arc, as z2d picks them: clockwise turns use the vertices at least half a pen
            # step inside the sweep; counter-clockwise turns run from the vertex nearest the incoming normal to the one
            # nearest the outgoing normal, both included, and none when that is the same vertex.
            n = len(pen)
            step = 2 * math.pi / n
            half = step / 2
            mids = []
            if cross > 0:
                for px, py in pen:
                    t = math.atan2(py, px)
                    d = (t - a0) % (2 * math.pi)
                    if half <= d <= sweep - half:
                        mids.append((d, (v[0] + px, v[1] + py)))
            else:
                def nearest(phi):
                    k = (phi - half) / step
                    return (math.ceil(k) if k != math.ceil(k) else int(k) + 1) % n
                i, e = nearest(a0), nearest(a1)
                if i != e:
                    while True:
                        mids.append((len(mids), (v[0] + pen[i][0], v[1] + pen[i][1])))
                        if i == e:
                            break
                        i = (i - 1) % n
            mids.sort()
            poly = [v, oa] + [p for _, p in mids] + [ob]
        elif 2 <= MITER_LIMIT * MITER_LIMIT * (1 + ax * bx + ay * by):
            t = ((ob[0] - oa[0]) * by - (ob[1] - oa[1]) * bx) / (ax * by - ay * bx)
            poly = [v, oa, (oa[0] + t * ax, oa[1] + t * ay), ob]
        else:
            poly = [v, oa, ob]
        q = _oriented(poly)
        if q:
            out.append(q)
    return out


def _offset_polyline(pts, closed, d):
    """Offset toward the inside of the turn (z2d Path.offset with a negative value = inset), miter vertices."""
    if closed and len(pts) > 2 and pts[0] == pts[-1]:
        pts = pts[:-1]
    n = len(pts)
    segs = [(pts[i], pts[i + 1]) for i in range(n - 1)] + ([(pts[-1], pts[0])] if closed else [])
    units = [_unit(b[0] - a[0], b[1] - a[1]) for a, b in segs]
    turn = sum(units[i][0] * units[(i + 1) % len(units)][1] - units[i][1] * units[(i + 1) % len(units)][0]
               for i in range(len(units) if closed else len(units) - 1))
    s = 1.0 if turn > 0 else -1.0  # +normal (-uy, ux) points to the right of travel = inside of a clockwise turn

    def off(p, u):
        return (p[0] + s * -u[1] * d, p[1] + s * u[0] * d)

    def meet(i, j):
        (ux, uy), (vx, vy) = units[i], units[j]
        p, q = off(segs[i][1], units[i]), off(segs[j][0], units[j])
        div = ux * vy - uy * vx
        if div == 0:
            return p
        t = ((q[0] - p[0]) * vy - (q[1] - p[1]) * vx) / div
        return (p[0] + t * ux, p[1] + t * uy)

    if closed:
        return [meet(i - 1 if i else len(segs) - 1, i) for i in range(len(segs))]
    return [off(segs[0][0], units[0])] + [meet(i, i + 1) for i in range(len(segs) - 1)] + [off(segs[-1][1], units[-1])]


def _edges(polys, scale):
    out = []
    for poly in polys:
        n = len(poly)
        for i in range(n):
            x0, y0 = poly[i][0] * scale, poly[i][1] * scale
            x1, y1 = poly[(i + 1) % n][0] * scale, poly[(i + 1) % n][1] * scale
            if y0 < y1:
                out.append((y0, y1, x0, (x1 - x0) / (y1 - y0), -1))
            elif y0 > y1:
                out.append((y1, y0, x1, (x0 - x1) / (y0 - y1), 1))
    return out


def _nonzero_spans(xs):
    xs.sort(key=lambda e: e[0])
    w, spans, start = 0, [], None
    for x, d in xs:
        if w == 0:
            start = x
        w += d
        if w == 0:
            spans.append((start, x))
    return spans


def _cover(polys, sw, sh):
    """{index: alpha} with z2d multisample_4x rules."""
    edges = _edges(polys, 4)
    if not edges:
        return {}
    top = min(e[0] for e in edges)
    bot = max(e[1] for e in edges)
    y0 = max(0, min(sh - 1, math.floor(top / 4)))
    y1 = max(y0, min(sh - 1, math.ceil(bot / 4)))
    lim = sw * 4
    out = {}
    for y in range(y0, y1 + 1):
        row = {}
        for sub in range(4):
            ym = y * 4 + sub + 0.5
            xs = [(zround(e[2] + e[3] * (ym - e[0])), e[4]) for e in edges if e[0] < ym <= e[1]]
            for a, b in _nonzero_spans(xs):
                a, b = max(a, 0), min(b, lim)
                while a < b:
                    px = a >> 2
                    nxt = min(b, (px + 1) << 2)
                    row[px] = row.get(px, 0) + nxt - a
                    a = nxt
        for px, n in row.items():
            if n:
                out[y * sw + px] = 255 if n >= 16 else max(0, min(255, n * 16 - 1))
    return out


def _composite(c, cov, shade=255):
    """z2d src-over of a solid source with alpha `shade` through the coverage mask (integer pipeline)."""
    buf = c.buf
    for i, m in cov.items():
        s = shade if m >= 255 else (shade * m) // 255
        if s >= 255:
            buf[i] = 255
        else:
            d = buf[i]
            buf[i] = s + d - (s * d) // 255


def _fill_polys(c, polys, shade=255):
    polys = [p for p in polys if len(p) >= 3]
    if polys:
        _composite(c, _cover(polys, c.stride, c.rows), shade)


def fill(c, path, shade=255):
    _fill_polys(c, [pts for pts, _, _ in path.flatten() if len(pts) >= 3], shade)


def stroke(c, path, width):
    polys = []
    for pts, flags, closed in path.flatten():
        polys += stroke_polys(pts, flags, closed, width)
    _fill_polys(c, polys)


def inner_stroke(c, path, width):
    polys = []
    for pts, _, closed in path.flatten():
        off = _offset_polyline(pts, closed, width / 2)
        polys += stroke_polys(off, [False] * len(off), closed, width)
    _fill_polys(c, polys)


def line(c, x0, y0, x1, y1, width):
    p = Path(c)
    p.move_to(x0, y0)
    p.line_to(x1, y1)
    stroke(c, p, width)


def polygon(c, pts, shade=255):
    p = Path(c)
    for x, y in pts:
        p.line_to(x, y)
    p.close()
    fill(c, p, shade)
