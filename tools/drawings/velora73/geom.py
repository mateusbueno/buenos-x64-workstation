"""Primitivas de desenho técnico em mm reais (1 unidade = 1 mm).

Cada View guarda geometria em coordenadas do mundo (mm reais, y para baixo)
e uma escala de folha. Espessuras, setas e textos são definidos em mm de papel
e convertidos para o mundo dividindo pela escala, como num CAD.
"""
import math
from xml.sax.saxutils import escape

RED = '#d40000'
INK = '#000000'
KEYFILL = '#3a3a3a'
FONT = "'DejaVu Sans', 'Liberation Sans', Arial, Helvetica, sans-serif"

# espessuras de linha em mm de papel (ISO 128: grossa 0,5 / fina 0,25 / cota 0,18)
LW = dict(thick=0.5, med=0.35, thin=0.25, dim=0.18, hidden=0.35, center=0.18, phantom=0.18)
DASH = dict(hidden=(2.5, 1.2), center=(8, 1.5, 1, 1.5), phantom=(8, 1.5, 1, 1.5, 1, 1.5))

ARROW_L = 2.5   # comprimento da seta (papel)
ARROW_W = 0.45  # meia-largura da seta (papel)
GAP = 1.0       # folga entre a peça e a linha de chamada (papel)
OVER = 2.0      # ultrapassagem da linha de chamada além da linha de cota (papel)
TXT = 2.5       # altura do texto de cota (papel)


def fmt(v, nd=2):
    s = ('%.*f' % (nd, v)).rstrip('0').rstrip('.')
    return s.replace('.', ',')


class View:
    def __init__(self, name, scale, ox, oy, title=None, dxf_name=None):
        self.name = name
        self.s = scale
        self.ox, self.oy = ox, oy
        self.title = title
        self.dxf_name = dxf_name
        self.items = []

    # conversões papel -> mundo
    def p(self, paper):
        return paper / self.s

    # ---------- primitivas ----------
    def line(self, x1, y1, x2, y2, k='thick', color=None):
        self.items.append(dict(t='poly', pts=[(x1, y1), (x2, y2)], closed=False, k=k, color=color, fill=None))

    def poly(self, pts, k='thick', closed=True, fill=None, color=None):
        self.items.append(dict(t='poly', pts=list(pts), closed=closed, k=k, color=color, fill=fill))

    def rect(self, x, y, w, h, k='thick', fill=None, color=None):
        self.poly([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], k=k, fill=fill, color=color)

    def rrect(self, x, y, w, h, r, k='thick', color=None, n=6):
        pts = []
        for cx, cy, a0 in ((x + w - r, y + r, -90), (x + w - r, y + h - r, 0), (x + r, y + h - r, 90), (x + r, y + r, 180)):
            for i in range(n + 1):
                a = math.radians(a0 + 90 * i / n)
                pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        self.poly(pts, k=k, color=color)

    def circle(self, cx, cy, r, k='thick', fill=None, color=None):
        self.items.append(dict(t='circle', c=(cx, cy), r=r, k=k, fill=fill, color=color))

    def text(self, x, y, s, size=2.5, anchor='middle', color=None, rot=0, bold=False):
        self.items.append(dict(t='text', x=x, y=y, s=s, size=size, anchor=anchor, color=color, rot=rot, bold=bold))

    def arrow(self, tip, direction, color=None):
        """Seta cheia com a ponta em `tip` apontando na direção (dx, dy)."""
        dx, dy = direction
        n = math.hypot(dx, dy)
        dx, dy = dx / n, dy / n
        L, W = self.p(ARROW_L), self.p(ARROW_W)
        bx, by = tip[0] - dx * L, tip[1] - dy * L
        pts = [tip, (bx - dy * W, by + dx * W), (bx + dy * W, by - dx * W)]
        self.items.append(dict(t='poly', pts=pts, closed=True, k='dim', color=color, fill=color or INK))

    def centermark(self, cx, cy, r):
        e = r + self.p(2)
        self.line(cx - e, cy, cx + e, cy, 'center')
        self.line(cx, cy - e, cx, cy + e, 'center')

    # ---------- cotas ISO 129 ----------
    def dim(self, a, b, at, orient='h', text=None, tbd=False, tside=None, ext_a=True, ext_b=True, note=None):
        """Cota linear entre os pontos a e b.

        orient 'h': mede |bx-ax|, linha de cota em y=at.
        orient 'v': mede |by-ay|, linha de cota em x=at.
        tbd: cota pendente, desenhada em vermelho.
        tside: 'out_r'/'out_l' força o texto para fora (cotas curtas).
        """
        color = RED if tbd else None
        if orient == 'h':
            (x1, y1), (x2, y2) = sorted([a, b])
            val = x2 - x1
            for (x, y), on in (((x1, y1), ext_a), ((x2, y2), ext_b)):
                if not on:
                    continue
                sgn = 1 if at > y else -1
                if abs(at - y) > self.p(GAP):
                    self.line(x, y + sgn * self.p(GAP), x, at + sgn * self.p(OVER), 'dim', color)
            p1, p2 = (x1, at), (x2, at)
            u = (1, 0)
        else:
            (x1, y1), (x2, y2) = sorted([a, b], key=lambda q: q[1])
            val = y2 - y1
            for (x, y), on in (((x1, y1), ext_a), ((x2, y2), ext_b)):
                if not on:
                    continue
                sgn = 1 if at > x else -1
                if abs(at - x) > self.p(GAP):
                    self.line(x + sgn * self.p(GAP), y, at + sgn * self.p(OVER), y, 'dim', color)
            p1, p2 = (at, y1), (at, y2)
            u = (0, 1)
        label = text if text is not None else fmt(val)
        if tbd and text is None:
            label = 'A DEFINIR'
        tw = self.p(0.62 * TXT * len(label))
        length = val
        inside = length > self.p(2 * ARROW_L + 1.5)
        if inside:
            self.line(p1[0], p1[1], p2[0], p2[1], 'dim', color)
            self.arrow(p1, (-u[0], -u[1]), color)
            self.arrow(p2, u, color)
        else:
            ext = self.p(ARROW_L + 2)
            self.line(p1[0] - u[0] * ext, p1[1] - u[1] * ext, p2[0] + u[0] * ext, p2[1] + u[1] * ext, 'dim', color)
            self.arrow(p1, u, color)
            self.arrow(p2, (-u[0], -u[1]), color)
        fits = length > tw + self.p(1)
        side = tside or ('mid' if fits else 'out_r')
        off = self.p(0.9)
        if orient == 'h':
            if side == 'mid':
                tx, anchor = (p1[0] + p2[0]) / 2, 'middle'
            elif side == 'out_r':
                tx, anchor = p2[0] + self.p(ARROW_L + 2.5), 'start'
            else:
                tx, anchor = p1[0] - self.p(ARROW_L + 2.5), 'end'
            self.text(tx, at - off, label, TXT, anchor, color)
            if note:
                self.text(tx, at + self.p(TXT + 0.6), note, 1.8, anchor, color)
        else:
            if side == 'mid':
                ty, anchor = (p1[1] + p2[1]) / 2, 'middle'
            elif side == 'out_r':
                ty, anchor = p2[1] + self.p(ARROW_L + 2.5), 'end'
            else:
                ty, anchor = p1[1] - self.p(ARROW_L + 2.5), 'start'
            self.text(at - off, ty, label, TXT, anchor, color, rot=-90)
            if note:
                self.text(at + self.p(TXT + 0.6), ty, note, 1.8, anchor, color, rot=-90)

    def chain(self, pts, at, orient='h', texts=None, tbd=None, sides=None):
        texts = texts or [None] * (len(pts) - 1)
        tbd = tbd or [False] * (len(pts) - 1)
        sides = sides or [None] * (len(pts) - 1)
        for i in range(len(pts) - 1):
            self.dim(pts[i], pts[i + 1], at, orient, texts[i], tbd[i], sides[i])

    def ordinate(self, x0, xs, y_feat, y_txt, labels=None):
        """Cotagem por coordenadas (ISO 129-1): origem 0 e valores ao longo das linhas de chamada."""
        labels = labels or [fmt(x - x0) for x in xs]
        for x, yf, lab in zip(xs, y_feat, labels):
            self.line(x, yf - self.p(GAP), x, y_txt, 'dim')
            self.text(x - self.p(0.6), y_txt + self.p(0.2), lab, TXT, 'start', rot=-90)
        self.circle(x0, y_txt + self.p(3.2), self.p(0.8), 'dim')

    def balloon(self, n, target, pos, r=3.4):
        R = self.p(r)
        tx, ty = target
        bx, by = pos
        d = math.hypot(tx - bx, ty - by)
        if d > R:
            ex, ey = bx + (tx - bx) * R / d, by + (ty - by) * R / d
            self.line(ex, ey, tx, ty, 'dim')
        self.circle(tx, ty, self.p(0.45), 'dim', fill=INK)
        self.circle(bx, by, R, 'thin', fill='#ffffff')
        self.text(bx, by + self.p(1.1), str(n), 3.0 if len(str(n)) < 3 else 2.4, 'middle', bold=True)

    def break_line(self, x1, x2, y, k='thin'):
        """Linha de interrupção em zigue-zague (ISO 128)."""
        m = (x1 + x2) / 2
        z = self.p(2.0)
        self.poly([(x1, y), (m - z, y), (m - z / 2, y - z), (m + z / 2, y + z), (m + z, y), (x2, y)], k=k, closed=False)

    # ---------- saída SVG ----------
    def svg(self):
        s = self.s
        out = ['<g class="view" data-view="%s" transform="translate(%.3f %.3f) scale(%.5f)">' % (self.name, self.ox, self.oy, s)]
        for it in self.items:
            col = it.get('color') or INK
            if it['t'] in ('poly', 'circle'):
                k = it['k']
                sw = LW[k] / s
                dash = ''
                if k in DASH:
                    dash = ' stroke-dasharray="%s"' % ' '.join('%.3f' % (d / s) for d in DASH[k])
                fill = it.get('fill') or 'none'
                if it['t'] == 'poly':
                    d = ' '.join('%.3f,%.3f' % p for p in it['pts'])
                    tag = 'polygon' if it['closed'] else 'polyline'
                    out.append('<%s points="%s" fill="%s" stroke="%s" stroke-width="%.4f"%s stroke-linejoin="round"/>' % (tag, d, fill, col, sw, dash))
                else:
                    out.append('<circle cx="%.3f" cy="%.3f" r="%.3f" fill="%s" stroke="%s" stroke-width="%.4f"%s/>' % (it['c'][0], it['c'][1], it['r'], fill, col, sw, dash))
            else:
                fs = it['size'] / s
                tr = ''
                if it['rot']:
                    tr = ' transform="rotate(%d %.3f %.3f)"' % (it['rot'], it['x'], it['y'])
                w = ' font-weight="bold"' if it['bold'] else ''
                out.append('<text x="%.3f" y="%.3f" font-size="%.3f" text-anchor="%s" fill="%s"%s%s>%s</text>' % (
                    it['x'], it['y'], fs, it['anchor'], col, w, tr, escape(it['s'])))
        out.append('</g>')
        return '\n'.join(out)

    # ---------- saída DXF (1:1, y invertido) ----------
    def dxf(self, path):
        import ezdxf
        from ezdxf.enums import TextEntityAlignment
        doc = ezdxf.new('R2018', setup=True)
        doc.units = ezdxf.units.MM
        doc.header['$INSUNITS'] = 4
        doc.header['$MEASUREMENT'] = 1
        lay = dict(thick=('CONTORNO', 7, 50, 'Continuous'), med=('CONTORNO_MEDIO', 7, 35, 'Continuous'),
                   thin=('FINA', 7, 25, 'Continuous'), dim=('COTAS', 7, 18, 'Continuous'),
                   hidden=('OCULTA', 7, 35, 'DASHED'), center=('EIXO', 7, 18, 'CENTER'),
                   phantom=('FANTASMA', 7, 18, 'PHANTOM'))
        for name, color, lw, lt in lay.values():
            if name not in doc.layers:
                doc.layers.add(name, color=color, linetype=lt, lineweight=lw)
        doc.layers.add('PENDENTE_A_DEFINIR', color=1, lineweight=18)
        doc.layers.add('TEXTO', color=7, lineweight=18)
        doc.layers.add('PREENCHIMENTO', color=8)
        msp = doc.modelspace()
        F = lambda p: (p[0], -p[1])
        ltscale = 1 / self.s
        for it in self.items:
            red = it.get('color') == RED
            if it['t'] in ('poly', 'circle'):
                L = 'PENDENTE_A_DEFINIR' if red else lay[it['k']][0]
                attrs = dict(layer=L)
                if red and it['k'] in DASH:
                    attrs['linetype'] = lay[it['k']][3]
                if it['k'] in DASH:
                    attrs['ltscale'] = ltscale
                if it['t'] == 'poly':
                    pts = [F(p) for p in it['pts']]
                    fill = it.get('fill')
                    if fill and fill not in ('none', '#ffffff'):
                        h = msp.add_hatch(color=1 if red else (8 if fill == KEYFILL else 7), dxfattribs=dict(layer='PREENCHIMENTO'))
                        h.paths.add_polyline_path(pts, is_closed=True)
                    if len(pts) == 2:
                        msp.add_line(pts[0], pts[1], dxfattribs=attrs)
                    else:
                        msp.add_lwpolyline(pts, close=it['closed'], dxfattribs=attrs)
                else:
                    msp.add_circle(F(it['c']), it['r'], dxfattribs=attrs)
            else:
                h = it['size'] / self.s
                al = {'start': TextEntityAlignment.BOTTOM_LEFT, 'middle': TextEntityAlignment.BOTTOM_CENTER,
                      'end': TextEntityAlignment.BOTTOM_RIGHT}[it['anchor']]
                t = msp.add_text(it['s'], height=h, rotation=-it['rot'],
                                 dxfattribs=dict(layer='PENDENTE_A_DEFINIR' if red else 'TEXTO'))
                t.set_placement(F((it['x'], it['y'])), align=al)
        doc.saveas(path)
