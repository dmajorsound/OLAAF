#!/usr/bin/env python3
# Builds OLAAF's two UI fonts from the unmodified sources in third_party (needs fontTools):
#   "Mulish OLAAF"       Regular = Mulish Regular (400), Bold = Mulish SemiBold (600)
#   "Red Hat Mono OLAAF" Regular = Red Hat Mono Medium (500), Bold = Red Hat Mono Bold (700)
# Changes: the family names (so they never clash with a Mulish or Red Hat Mono the user has installed),
# the lighter weights filed as Regular / Bold (so every system picks them with a plain "bold"), and in the
# Mulish faces four arrows it lacks (<- ^ -> v), drawn to match its minus sign.
# Both fonts: SIL Open Font License 1.1 without Reserved Font Names, so modified versions may be made and
# named like this; they stay under the OFL (third_party/mulish/OFL.txt, third_party/redhat-mono/OFL.txt).
# Writes third_party/olaaf-fonts/*.ttf; tools/embed_fonts.py then puts them into src/ext/fonts_data.cpp.
import math, os
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.boundsPen import BoundsPen

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
out_dir = os.path.join(root, 'third_party/olaaf-fonts')
os.makedirs(out_dir, exist_ok=True)


def rename(f, family, style, ps):
    name = f['name']
    for nid in (1, 2, 3, 4, 6, 16, 17, 21, 22):
        name.removeNames(nameID=nid)
    full = family + ' ' + style
    for nid, s in ((1, family), (2, style), (3, ps + ';OLAAF'), (4, full), (6, ps)):
        name.setName(s, nid, 3, 1, 0x409)
        name.setName(s, nid, 1, 0, 0)


def weight(f, bold):
    os2, head = f['OS/2'], f['head']
    os2.usWeightClass = 700 if bold else 400
    sel = os2.fsSelection & ~((1 << 0) | (1 << 5) | (1 << 6))
    os2.fsSelection = sel | ((1 << 5) if bold else (1 << 6))
    head.macStyle = (head.macStyle & ~1) | (1 if bold else 0)


def add_arrows(f):
    gs = f.getGlyphSet()
    cmap = f.getBestCmap()
    minus = cmap[0x2212]
    bp = BoundsPen(gs)
    gs[minus].draw(bp)
    x0, y0, x1, y1 = bp.bounds
    t = (y1 - y0) / 2.0  # half the stroke, as the minus sign
    cy = (y0 + y1) / 2.0  # its height
    cx, half, head = 380.0, 320.0, 230.0

    def seg(ax, ay, bx, by):  # a stroke from a to b as a clockwise quad
        dx, dy = bx - ax, by - ay
        l = math.hypot(dx, dy)
        nx, ny = -dy / l * t, dx / l * t
        pts = [(ax + nx, ay + ny), (bx + nx, by + ny), (bx - nx, by - ny), (ax - nx, ay - ny)]
        area = sum(pts[i][0] * pts[(i + 1) % 4][1] - pts[(i + 1) % 4][0] * pts[i][1] for i in range(4))
        return pts if area < 0 else pts[::-1]

    def arrow(angle):  # pointing at angle (0 = right), around (cx, cy)
        ca, sa = math.cos(angle), math.sin(angle)
        tip = (cx + half * ca, cy + half * sa)
        tail = (cx - half * ca, cy - half * sa)
        # the shaft stops a little short of the tip so the point stays sharp
        end = (tip[0] - t * 1.2 * ca, tip[1] - t * 1.2 * sa)
        contours = [seg(*tail, *end)]
        for s in (1, -1):
            a = angle + math.pi - s * math.pi / 4
            back = (tip[0] + head * math.cos(a), tip[1] + head * math.sin(a))
            contours.append(seg(tip[0] + t * 0.7 * math.cos(a), tip[1] + t * 0.7 * math.sin(a), *back))
        pen = TTGlyphPen(None)
        for c in contours:
            pen.moveTo(tuple(round(v) for v in c[0]))
            for p in c[1:]:
                pen.lineTo(tuple(round(v) for v in p))
            pen.closePath()
        return pen.glyph()

    order = list(f.getGlyphOrder())
    glyf = f['glyf']
    for code, name, ang in ((0x2190, 'arrowleft', math.pi), (0x2191, 'arrowup', math.pi / 2),
                            (0x2192, 'arrowright', 0.0), (0x2193, 'arrowdown', -math.pi / 2)):
        if code in cmap:
            continue
        g = arrow(ang)
        order.append(name)
        glyf.glyphOrder = order
        glyf.glyphs[name] = g
        g.recalcBounds(glyf)
        f["hmtx"][name] = (760, g.xMin)
        for sub in f['cmap'].tables:
            if sub.isUnicode():
                sub.cmap[code] = name
    f.setGlyphOrder(order)
    glyf.glyphOrder = order
    if 'post' in f and f['post'].formatType == 2.0:
        f['post'].extraNames = list(getattr(f['post'], 'extraNames', []))
        f['post'].glyphOrder = order


jobs = [
    ('mulish/Mulish-Regular.ttf', 'Mulish OLAAF', 'Regular', 'MulishOLAAF-Regular', False, True),
    ('mulish/Mulish-SemiBold.ttf', 'Mulish OLAAF', 'Bold', 'MulishOLAAF-Bold', True, True),
    ('redhat-mono/RedHatMono-Medium.ttf', 'Red Hat Mono OLAAF', 'Regular', 'RedHatMonoOLAAF-Regular', False, False),
    ('redhat-mono/RedHatMono-Bold.ttf', 'Red Hat Mono OLAAF', 'Bold', 'RedHatMonoOLAAF-Bold', True, False),
]
for src, family, style, ps, bold, arrows in jobs:
    f = TTFont(os.path.join(root, 'third_party', src))
    for tag in ('STAT', 'DSIG'):
        if tag in f:
            del f[tag]
    rename(f, family, style, ps)
    weight(f, bold)
    if arrows:
        add_arrows(f)
    f.save(os.path.join(out_dir, ps + '.ttf'))
    print('wrote', ps + '.ttf')
