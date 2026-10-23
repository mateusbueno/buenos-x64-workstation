"""Monta as vistas e as folhas A3 do desenho técnico do Velora 73."""
import os
from geom import View, RED, INK, KEYFILL, FONT, fmt
from model import *  # noqa: F401,F403

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')
os.makedirs(OUT, exist_ok=True)

DATE = '23/10/2026'
REV = '00'
LIGHT = '#c8c8c8'


# =====================================================================
# VISTA SUPERIOR (1:5) — mundo: x = X, y = DEPTH - Y
# =====================================================================
def top_view():
    v = View('superior', 0.2, 30, 76, dxf_name='01_vista_superior.dxf')
    D = DEPTH
    Y = lambda y: D - y
    # contorno do gabinete
    v.rrect(0, 0, WIDTH, D, R_CORNER, 'thick')
    v.line(CHEEK, 0, CHEEK, D, 'thick')
    v.line(WIDTH - CHEEK, 0, WIDTH - CHEEK, D, 'thick')
    v.line(CHEEK, Y(LIP), WIDTH - CHEEK, Y(LIP), 'thin')                 # frente das teclas / lábio
    v.line(CHEEK, Y(Y_PANEL), WIDTH - CHEEK, Y(Y_PANEL), 'thick')       # borda frontal do tampo
    v.line(CHEEK + WHEEL_BLOCK, Y(LIP), CHEEK + WHEEL_BLOCK, Y(Y_PANEL), 'thick')  # bloco das rodas
    # teclas (parte visível, à frente do tampo)
    for w in WHITES:
        y0, yh, y1 = Y(LIP), Y(LIP + HEAD), Y(Y_PANEL)
        v.poly([(w['tl'], y1), (w['tl'], yh), (w['xl'], yh), (w['xl'], y0), (w['xr'], y0), (w['xr'], yh), (w['tr'], yh), (w['tr'], y1)],
               'thin', closed=False)
        if w['name'] == 'C':
            mid = w['octave'] == 4
            v.text(w['xl'] + WW / 2, Y(LIP + 9), 'C%d' % w['octave'], 1.6 if not mid else 1.8, bold=mid)
    for b in BLACKS:
        v.rect(b['xl'], Y(Y_PANEL), BW, Y(LIP + BOFF) - Y(Y_PANEL), 'thin', fill=KEYFILL)
    # Dó central
    c4 = c_left(4)
    v.poly([(c4 + WW / 2, Y(LIP) + 1.5), (c4 + WW / 2 - 5, Y(LIP) + 9), (c4 + WW / 2 + 5, Y(LIP) + 9)], 'dim', fill=INK)
    v.text(c4 + WW / 2 + 9, Y(LIP) + 10, 'C4 · DÓ CENTRAL (MIDI 60) · E1–E7', 1.7, 'start', bold=True)
    # rodas
    for nm, x in WHEELS:
        w_, l_ = WHEEL_SIZE
        v.rect(x - w_ / 2, Y(WHEEL_Y + l_ / 2), w_, l_, 'thin', color=RED)
        v.line(x - w_ / 2, Y(WHEEL_Y), x + w_ / 2, Y(WHEEL_Y), 'center')
        v.text(x, Y(WHEEL_Y - l_ / 2 - 6), nm, 1.4)
    v.text(CHEEK + WHEEL_BLOCK / 2, Y(WHEEL_Y + WHEEL_SIZE[1] / 2 + 12), 'Ø/LARG. A DEFINIR', 1.3, color=RED)
    # display
    gw, gh = DISP_GLASS
    aw, ah = DISP_ACTIVE
    cx, cy = DISP_C
    v.rect(cx - gw / 2, Y(cy + gh / 2), gw, gh, 'hidden', color=RED)
    v.rect(cx - aw / 2, Y(cy + ah / 2), aw, ah, 'thick')
    v.text(cx, Y(cy) - 3, 'DISPLAY 7" 1024×600', 1.8)
    v.text(cx, Y(cy) + 9, 'ÁREA ATIVA 153,6 × 90,0', 1.6)
    v.text(cx, Y(cy - gh / 2) - 3, 'VIDRO/RECORTE A DEFINIR', 1.3, color=RED)
    # faders
    for i, x in enumerate(FADER_X):
        v.rect(x - 1.2, Y(FADER_Y + FADER_TRAVEL / 2 + 1), 2.4, FADER_TRAVEL + 2, 'thin')
        v.rect(x - 6, Y(FADER_Y + 8), 12, 16, 'thin')
        v.text(x, Y(FADER_Y - FADER_TRAVEL / 2 - 7), 'REV' if i == 8 else str(i + 1), 1.3)
    v.text((FADER_X[0] + FADER_X[-1]) / 2, Y(FADER_Y + FADER_TRAVEL / 2 + 6), 'MIXER · DRAWBARS', 1.3)
    # botões
    bw, bh = BTN
    for bid, name, x, y in BUTTONS:
        v.rect(x - bw / 2, Y(y + bh / 2), bw, bh, 'thin')
        if bid in ('14.%d' % i for i in range(1, 9)):
            v.circle(x, Y(y + LED_DY), LED_D / 2, 'thin')
    lbl = {'14.9': '◀', '14.10': '▶', '14.11': '◀', '14.12': '▶', '14.13': 'EXIT', '14.14': 'MENU', '14.15': 'WRITE',
           '14.16': 'STOP', '14.17': 'S/F'}
    for bid, name, x, y in BUTTONS:
        if bid in lbl:
            v.text(x, Y(y - bh / 2 - 6), lbl[bid], 1.2)
    v.text(725, Y(262 + 10), 'LIVE SET', 1.2)
    v.text(725, Y(240 + 10), 'SCENE', 1.2)
    v.text(122.5, Y(215 + 10), 'ROTARY', 1.2)
    # toggles
    for tid, name, x, y, n in TOGGLES:
        v.rect(x - bw / 2, Y(y + bh / 2), bw, bh, 'thin')
        for j in range(n):
            v.circle(x + bw / 2 + 4, Y(y - (n - 1) * 2.5 + j * 5), LED_D / 2, 'thin')
        v.text(x, Y(y - bh / 2 - 6), name, 1.1)
    # encoders
    for r, yy in enumerate(ENC_Y):
        for c, x in enumerate(ENC_X):
            v.circle(x, Y(yy), 14, 'thin')
            v.circle(x, Y(yy), 10, 'thick')
            v.line(x, Y(yy) - 10, x, Y(yy) - 5, 'thin')
            v.text(x, Y(yy - 14) + 6, ENC_NAMES[r][c], 1.1)
    # VALUE e MASTER
    _, _, x, y, dia = VALUE
    v.circle(x, Y(y), dia / 2, 'thick')
    v.circle(x, Y(y), dia / 2 - 3, 'thin')
    v.text(x, Y(y - dia / 2) + 6, 'VALUE', 1.2)
    _, _, x, y, dia = MASTER
    v.circle(x, Y(y), dia / 2, 'thick')
    v.line(x, Y(y) - 10, x, Y(y) - 5, 'thin')
    v.text(x, Y(y - dia / 2) + 6, 'MASTER', 1.2)
    v.text(105, Y(272), 'LADIANI', 2.0, bold=True)
    v.text(105, Y(262), 'VELORA 73', 1.4)
    # internos (ocultos)
    kb0 = CHEEK + WHEEL_BLOCK
    v.poly([(kb0, Y(Y_PANEL)), (kb0, Y(LIP + KEYBED_DEPTH)), (kb0 + KEYBED_W, Y(LIP + KEYBED_DEPTH)), (kb0 + KEYBED_W, Y(Y_PANEL))],
           'hidden', closed=False)
    for key, (name, x0, y0, x1, y1, z0, z1) in INTERNAL.items():
        v.rect(x0, Y(y1), x1 - x0, y1 - y0, 'hidden', color=None if key == '28' else RED)
    for x, y in FEET:
        v.circle(x, Y(y), FOOT_D / 2, 'hidden')
        v.circle(x, Y(y), 2.0, 'hidden')
        v.centermark(x, Y(y), FOOT_D / 2)
    # corte A-A
    yA0, yA1 = -16, D + 16
    v.line(X_CUT, yA0, X_CUT, yA1, 'center')
    for yy, sg in ((yA0, 1), (yA1, -1)):
        v.line(X_CUT, yy, X_CUT, yy + sg * 10, 'thick')
        v.line(X_CUT, yy, X_CUT - 20, yy, 'thin')
        v.arrow((X_CUT - 22, yy), (-1, 0))
        v.text(X_CUT - 30, yy + (4 if sg > 0 else 10), 'A', 3.0, bold=True)
    # detalhe B
    bx0, bx1 = c_left(4) - 6, c_left(5) + 8
    v.rect(bx0, Y(LIP + 147), bx1 - bx0, 147 + 13, 'center')
    v.text(bx1 + 3, D - 4, 'B', 3.0, 'start', bold=True)
    # origem
    v.circle(0, D, 1.8, 'thin', fill=INK)
    v.line(0, D, 45, D, 'thin'); v.arrow((50, D), (1, 0))
    v.line(0, D, 0, D - 45, 'thin'); v.arrow((0, D - 50), (0, -1))
    v.text(-10, D + 16, 'O', 2.2, bold=True)
    v.text(52, D + 12, 'X', 1.8)
    v.text(-9, D - 45, 'Y', 1.8)
    # cotas: larguras (frente)
    xs = [0, CHEEK, CHEEK + WHEEL_BLOCK, XK0, XK1, WIDTH - CHEEK, WIDTH]
    pts = [(x, D) for x in xs]
    v.dim(pts[0], pts[1], D + 30, text='25')
    v.dim(pts[1], pts[2], D + 30, text='80')
    v.dim(pts[3], pts[4], D + 30, text='1013,4', note='43 × 23,6 − 1,4')
    v.dim(pts[5], pts[6], D + 30, text='25')
    v.dim(pts[2], pts[3], D + 46, text='10,4', tbd=True, tside='out_r')
    v.dim(pts[4], pts[5], D + 46, text='10,4', tbd=True, tside='out_l')
    v.dim((CHEEK + WHEEL_BLOCK, D), (WIDTH - CHEEK, D), D + 62, text='1034,2', tbd=True, note='MOLDURA FATAR 10,4/LADO: A CONFIRMAR (73SW)')
    v.dim((0, D), (WIDTH, D), D + 100, text='1164,2', note='LARGURA TOTAL')
    # cotas: profundidade (direita)
    ys = [0, LIP, Y_PANEL, Y_PANEL + PANEL_ZONE, DEPTH]
    v.chain([(WIDTH, Y(y)) for y in ys], WIDTH + 30, 'v', texts=['15', '139,4', '130', '20'],
            tbd=[False, False, True, False], sides=['out_l', None, None, None])
    v.dim((WIDTH, Y(0)), (WIDTH, Y(DEPTH)), WIDTH + 62, 'v', text='304,4', tbd=True, note='PROF. TOTAL')
    return v


# =====================================================================
# VISTA FRONTAL (1:5) — mundo: x = X, y = -Z
# =====================================================================
def front_view():
    v = View('frontal', 0.2, 30, 42, dxf_name='02_vista_frontal.dxf')
    Z = lambda z: -z
    for x0 in (0, WIDTH - CHEEK):
        v.rrect(x0, Z(HEIGHT), CHEEK, HEIGHT, R_CORNER, 'thick')
    v.line(CHEEK, Z(0), WIDTH - CHEEK, Z(0), 'thick')
    v.rect(CHEEK, Z(LIP_H), WIDTH - 2 * CHEEK, LIP_H, 'thick')                         # lábio
    v.rect(CHEEK, Z(Z_WHITE), WHEEL_BLOCK, Z_WHITE - LIP_H, 'thick')                   # bloco das rodas
    v.rect(CHEEK, Z(HEIGHT), WIDTH - 2 * CHEEK, FASCIA, 'thick')                       # aba do tampo
    for x, y in FEET[:2]:
        v.rect(x - FOOT_D / 2, Z(0), FOOT_D, FOOT_H, 'thick')
    for w in WHITES:
        v.rect(w['xl'], Z(Z_WHITE), WW, WATERFALL, 'thin')
    for b in BLACKS:
        v.rect(b['xl'], Z(Z_BLACK), BW, H_BLACK, 'thin', fill=KEYFILL)
    for nm, x in WHEELS:
        v.rect(x - WHEEL_SIZE[0] / 2, Z(Z_WHITE + 20), WHEEL_SIZE[0], 20, 'thin', color=RED)
    # knobs, caps e botões acima do tampo: alturas provisórias
    for x in FADER_X:
        v.rect(x - 6, Z(HEIGHT + 10), 12, 10, 'thin', color=RED)
    for x in ENC_X:
        v.rect(x - 10, Z(HEIGHT + 15), 20, 15, 'thin', color=RED)
    v.rect(VALUE[2] - 17.5, Z(HEIGHT + 20), 35, 20, 'thin', color=RED)
    v.rect(MASTER[2] - 10, Z(HEIGHT + 15), 20, 15, 'thin', color=RED)
    # fone
    _, _, x, _, z = PHONES
    v.circle(x, Z(z), 6.35 / 2, 'thin')
    v.circle(x, Z(z), 6.0, 'thin', color=RED)
    # cotas de altura
    xa = WIDTH + 30
    v.chain([(WIDTH, Z(-FOOT_H)), (WIDTH, Z(0)), (WIDTH, Z(HEIGHT))], xa, 'v', texts=['10', '68,7'], tbd=[False, True])
    v.dim((ENC_X[-1] + 10, Z(HEIGHT)), (ENC_X[-1] + 10, Z(HEIGHT + 15)), xa, 'v', tbd=True, text='A DEF.', tside='out_l')
    v.dim((WIDTH, Z(-FOOT_H)), (WIDTH, Z(HEIGHT)), WIDTH + 62, 'v', text='78,7', tbd=True, note='ALT. S/ KNOBS')
    v.text(ENC_X[0] - 30, Z(HEIGHT + 24), 'ALTURA DE KNOBS/CAPS ACIMA DO TAMPO: A DEFINIR', 1.5, 'end', color=RED)
    return v


# =====================================================================
# VISTA POSTERIOR (1:5) — mundo: x = u = WIDTH - X, y = -Z
# =====================================================================
def rear_view():
    v = View('posterior', 0.2, 30, 188, dxf_name='03_vista_posterior.dxf')
    Z = lambda z: -z
    for x0 in (0, WIDTH - CHEEK):
        v.rrect(x0, Z(HEIGHT), CHEEK, HEIGHT, R_CORNER, 'thick')
    v.rect(CHEEK, Z(HEIGHT), WIDTH - 2 * CHEEK, HEIGHT, 'thick')
    v.line(CHEEK, Z(T_BOT), WIDTH - CHEEK, Z(T_BOT), 'thin')
    for x, y in FEET[2:]:
        u = WIDTH - x
        v.rect(u - FOOT_D / 2, Z(0), FOOT_D, FOOT_H, 'thick')
    z = Z(REAR_Z)
    for cid, name, u, kind in REAR:
        if kind == 'jack':
            v.circle(u, z, 6.35 / 2, 'thin')
            v.circle(u, z, 6.0, 'thin', color=RED)
        elif kind == 'mini':
            v.circle(u, z, 3.5 / 2, 'thin')
            v.circle(u, z, 4.0, 'thin', color=RED)
        elif kind == 'din':
            v.circle(u, z, 9.5, 'thin', color=RED)
            v.circle(u, z, 7.5, 'thin')
            import math
            for a in (-90, -45, 0, 45, 90):
                r = math.radians(a + 90)
                v.circle(u + 4.5 * math.cos(r) * -1, z + 4.5 * math.sin(r), 0.6, 'thin', fill=INK)
        elif kind == 'usbc':
            v.rrect(u - 8.94 / 2, z - 3.26 / 2, 8.94, 3.26, 1.6, 'thin', n=4)
        elif kind == 'usba':
            v.rect(u - 12.5 / 2, z - 5.12 / 2, 12.5, 5.12, 'thin')
        elif kind == 'dc':
            v.circle(u, z, 6.0, 'thin', color=RED)
            v.circle(u, z, 2.75, 'thin')
        else:
            v.circle(u, z, 6.0, 'thin', color=RED)
            v.circle(u, z, 4.0, 'thick')
        v.text(u, z - 14, REAR_LABEL[cid], 1.15)
    groups = [('ENERGIA', 80, 105), ('USB', 160, 185), ('MIDI', 240, 270), ('PEDAIS', 330, 380), ('LINE IN', 440, 490),
              ('AUX OUT', 550, 575), ('MAIN OUT', 635, 660)]
    for name, a, b in groups:
        v.line(a - 8, Z(HEIGHT - 9), b + 8, Z(HEIGHT - 9), 'dim')
        v.text((a + b) / 2, Z(HEIGHT - 6), name, 1.3)
    v.dim((CHEEK, Z(0)), (CHEEK, Z(REAR_Z)), CHEEK + 15, 'v', text='35')
    return v


# =====================================================================
# CORTE A-A (1:2) — mundo: x = Y (profundidade), y = -Z
# =====================================================================
def section_view():
    v = View('corte_AA', 0.5, 42, 252, dxf_name='04_corte_AA.dxf')
    Z = lambda z: -z
    D = DEPTH
    # lateral esquerda, além do plano (aresta visível)
    v.rrect(0, Z(HEIGHT), D, HEIGHT, R_CORNER, 'thin')
    # chapas cortadas (seções finas enegrecidas)
    v.rect(0, Z(T_BOT), D, T_BOT, 'thin', fill=INK)                                    # fundo
    v.rect(0, Z(LIP_H), T_BOT, LIP_H - T_BOT, 'thin', fill=INK)                          # lábio: parede
    v.rect(0, Z(LIP_H), LIP, T_BOT, 'thin', fill=INK)                                    # lábio: topo
    v.rect(D - T_TOP, Z(HEIGHT - T_TOP), T_TOP, HEIGHT - T_TOP - T_BOT, 'thin', fill=INK)  # traseira
    win0, win1 = DISP_C[1] - DISP_ACTIVE[1] / 2, DISP_C[1] + DISP_ACTIVE[1] / 2
    for a, b in ((Y_PANEL, win0), (win1, D)):
        v.rect(a, Z(HEIGHT), b - a, T_TOP, 'thin', fill=INK)                            # tampo
    v.rect(Y_PANEL, Z(HEIGHT - T_TOP), T_TOP, FASCIA - T_TOP, 'thin', fill=INK)          # aba frontal
    # pés (além do plano)
    for y in (FOOT_IN, D - FOOT_IN):
        v.rect(y - FOOT_D / 2, Z(0), FOOT_D, FOOT_H, 'thick')
    # teclado: envoltória + tecla branca cortada (C4) + preta além do plano
    v.rect(LIP + 2, Z(38.7), KEYBED_DEPTH - 2, 38.7 - Z_KB, 'thin')
    v.text(LIP + 80, Z(14), 'TECLADO FATAR (ENVOLTÓRIA)', 2.0)
    v.text(LIP + 80, Z(8), 'perfil interno: desenho Fatar', 1.6)
    t = 2.5
    kf, kb = LIP, LIP + LW_TOT
    v.poly([(kf, Z(Z_WHITE - WATERFALL)), (kf, Z(Z_WHITE)), (kb, Z(Z_WHITE)), (kb, Z(Z_WHITE - 8)), (kb - t, Z(Z_WHITE - 8)),
            (kb - t, Z(Z_WHITE - t)), (kf + t, Z(Z_WHITE - t)), (kf + t, Z(Z_WHITE - WATERFALL)),
            ], 'thick', fill='#9a9a9a')
    bf = LIP + BOFF
    v.poly([(bf, Z(Z_WHITE)), (LIP + 56.25, Z(Z_BLACK)), (LIP + 96.65, Z(Z_BLACK)), (LIP + 133.7, Z(Z_BLACK - 3.3)),
            (LIP + BTOP_END, Z(Z_BLACK - 3.3)), (LIP + BTOP_END, Z(Z_WHITE))], 'thick', fill=KEYFILL)
    # curso: tecla pressionada
    v.poly([(kf, Z(Z_WHITE - WATERFALL - TRAVEL)), (kf, Z(Z_WHITE - TRAVEL)), (kb, Z(Z_WHITE))], 'phantom', closed=False)
    # display e placas (A DEFINIR)
    g0, g1 = DISP_C[1] - DISP_GLASS[1] / 2, DISP_C[1] + DISP_GLASS[1] / 2
    v.rect(g0, Z(HEIGHT - T_TOP), g1 - g0, 12, 'hidden', color=RED)
    for key in ('27', '29a'):
        name, x0, y0, x1, y1, z0, z1 = INTERNAL[key]
        v.rect(y0, Z(z1), y1 - y0, z1 - z0, 'hidden', color=RED)
    name, x0, y0, x1, y1, z0, z1 = INTERNAL['27']
    for yy in (y0 + 6, y1 - 6):
        v.line(yy, Z(T_BOT), yy, Z(z0), 'hidden', color=RED)
    # cotas de profundidade (abaixo)
    ys = [0, LIP, Y_PANEL, Y_PANEL + PANEL_ZONE, D]
    v.chain([(y, Z(-FOOT_H)) for y in ys], 26, texts=['15', '139,4', '130', '20'], tbd=[False, False, True, False],
            sides=['out_l', None, None, None])
    v.dim((0, Z(-FOOT_H)), (D, Z(-FOOT_H)), 40, text='304,4', tbd=True, note='PROFUNDIDADE TOTAL (PROVISÓRIA)')
    # cotas das teclas (acima)
    v.chain([(LIP, Z(Z_WHITE)), (bf, Z(Z_WHITE)), (LIP + 110 + BOFF, Z(Z_WHITE))], Z(80), texts=['50', '110'])
    v.dim((LIP, Z(Z_WHITE)), (LIP + LW_TOT, Z(Z_WHITE)), Z(90), text='160')
    # cotas de altura (direita)
    xa = D + 14
    v.dim((D, Z(-FOOT_H)), (D, Z(0)), xa, 'v', text='10')
    v.dim((D, Z(0)), (D, Z(T_BOT)), xa + 10, 'v', text='1,2', tside='out_r')
    zs = [T_BOT, Z_WHITE, Z_BLACK, HEIGHT - T_TOP, HEIGHT]
    v.chain([(D, Z(z)) for z in zs], xa, 'v', texts=['47,5', '10', '8', '2'],
            tbd=[False, False, True, False], sides=[None, None, None, 'out_l'])
    v.dim((D, Z(0)), (D, Z(HEIGHT)), D + 34, 'v', text='68,7', tbd=True, note='DEPENDE DA FOLGA')
    # cotas da frente (esquerda)
    v.dim((0, Z(0)), (0, Z(LIP_H)), -8, 'v', tbd=True, text='A DEF.', tside='out_l')
    v.dim((kf, Z(Z_WHITE - WATERFALL)), (kf, Z(Z_WHITE)), -16, 'v', tbd=True, text='A DEF.')
    v.dim((kf, Z(Z_WHITE - TRAVEL)), (kf, Z(Z_WHITE)), -24, 'v', text='10', tside='out_l')
    v.text(-8, Z(-6), 'LÁBIO', 1.4, color=RED)
    v.text(-16, Z(Z_WHITE + 12), 'WATERFALL', 1.4, color=RED)
    v.text(-25, Z(Z_WHITE + 17), 'CURSO', 1.4)
    return v


# =====================================================================
# DETALHE B (2:1): uma oitava, C4 a B4, vista de cima com interrupção
# =====================================================================
BRK_F, BRK_R = 55.0, 115.0


def dy(d):
    """profundidade a partir da frente da tecla -> y do detalhe (com interrupção)."""
    return (VIS + 3 - BRK_R + BRK_F) - d if d <= BRK_F else VIS - d


def detail_view():
    v = View('detalhe_B', 2.0, 30, 64, dxf_name='05_detalhe_B_oitava.dxf')
    c0 = c_left(4)
    oct_w = [w for w in WHITES if w['octave'] == 4]
    oct_b = [b for b in BLACKS if b['octave'] == 4]
    X = lambda x: x - c0
    for w in oct_w:
        xl, xr, tl, tr = X(w['xl']), X(w['xr']), X(w['tl']), X(w['tr'])
        v.poly([(tl, dy(BRK_F)), (tl, dy(HEAD)), (xl, dy(HEAD)), (xl, dy(0)), (xr, dy(0)), (xr, dy(HEAD)), (tr, dy(HEAD)), (tr, dy(BRK_F))],
               'thick', closed=False)
        v.line(tl, dy(BRK_R), tl, dy(VIS), 'thick')
        v.line(tr, dy(BRK_R), tr, dy(VIS), 'thick')
        mid = w['name'] == 'C'
        v.text((xl + xr) / 2, dy(14), '%s%d' % (w['name'], w['octave']), 3.2, bold=mid)
    v.text(X(c0) + WW / 2, dy(6), 'DÓ CENTRAL', 1.8, bold=True)
    for b in oct_b:
        x = X(b['xl'])
        v.rect(x, dy(BRK_F), BW, BRK_F - BOFF, 'thick', fill=KEYFILL)
        v.rect(x, dy(VIS), BW, VIS - BRK_R, 'thick', fill=KEYFILL)
        v.line(x, dy(BOFF), x + 2, dy(BOFF + BCH), 'thin', color=LIGHT)
        v.line(x + BW, dy(BOFF), x + BW - 2, dy(BOFF + BCH), 'thin', color=LIGHT)
        v.line(x + 2, dy(BOFF + BCH), x + BW - 2, dy(BOFF + BCH), 'thin', color=LIGHT)
        v.line(x + 2, dy(BOFF + BCH), x + 2, dy(BRK_F), 'thin', color=LIGHT)
        v.line(x + BW - 2, dy(BOFF + BCH), x + BW - 2, dy(BRK_F), 'thin', color=LIGHT)
        k = (BRK_R - BTOP_TAPER) / (BTOP_END - BTOP_TAPER)
        v.line(x + 2 - k, dy(BRK_R), x + 1, dy(BTOP_END), 'thin', color=LIGHT)
        v.line(x + BW - 2 + k, dy(BRK_R), x + BW - 1, dy(BTOP_END), 'thin', color=LIGHT)
        v.line(x, dy(BTOP_END), x + BW, dy(BTOP_END), 'thin', color=LIGHT)
        v.text(x + BW / 2, dy(122), b['name'] + str(b['octave']), 2.2, color='#ffffff', bold=True)
    # interrupções e borda do tampo
    for d in (BRK_F, BRK_R):
        v.break_line(-6, OCT + 4, dy(d))
    v.line(-4, dy(VIS), OCT + 6, dy(VIS), 'thick')
    v.text(151.2, dy(VIS) - 1.2, 'BORDA DO TAMPO', 1.6, 'start')
    # C5 (início da oitava seguinte), fantasma
    v.poly([(OCT, dy(BRK_F)), (OCT, dy(0)), (OCT + 6, dy(0))], 'phantom', closed=False)
    v.text(OCT + 1, dy(14), 'C5', 2.2)
    # cotas: posição das pretas (coordenadas a partir da borda do C4)
    xs = [X(b['xl']) for b in oct_b]
    v.ordinate(0, [0] + xs, [dy(VIS)] * (len(xs) + 1), -15, ['0'] + [fmt(x) for x in xs])
    a = X(oct_b[-1]['xl'])
    v.dim((a, dy(VIS)), (a + BW, dy(VIS)), -8, text='12,2')
    f = X(oct_b[2]['xl'])
    v.dim((f - BCLR, dy(VIS)), (f + BW + BCLR, dy(VIS)), -8, text='14,9', tside='out_l')
    # cotas: brancas (frente)
    yf = dy(0)
    v.dim((0, yf), (WW, yf), yf + 4, text='22,2')
    v.dim((WW, yf), (P, yf), yf + 4, text='1,4', tside='out_r')
    v.dim((3 * P, yf), (4 * P, yf), yf + 4, text='23,6', note='PASSO')
    v.dim((0, yf), (OCT, yf), yf + 10, text='165,2', note='OITAVA (7 × 23,6)')
    e = [w for w in oct_w if w['name'] == 'E'][0]
    v.dim((X(e['tl']), dy(BRK_F)), (X(e['tr']), dy(BRK_F)), dy(BRK_F) + 4, text='13,2')
    # cotas: comprimentos (direita)
    xr = OCT + 9
    v.dim((OCT, dy(0)), (OCT, dy(HEAD)), xr, 'v', text='48')
    v.dim((OCT, dy(0)), (OCT, dy(BOFF)), xr + 4, 'v', text='50')
    v.dim((OCT, dy(0)), (OCT, dy(VIS)), xr + 8, 'v', text='139,4')
    v.dim((OCT, dy(BOFF)), (OCT, dy(VIS)), xr + 12, 'v', text='89,4', note='PRETA VISÍVEL')
    v.dim((X(oct_b[0]['xl']) + 2, dy(BOFF)), (X(oct_b[0]['xl']) + 2, dy(BOFF + BCH)), X(oct_b[0]['xl']) - 3, 'v', text='3,5', tside='out_r')
    return v


# =====================================================================
# Papel: moldura, carimbo, tabelas
# =====================================================================
def paper(name):
    return View(name, 1.0, 0, 0)


def frame(pv):
    pv.rect(20, 10, 390, 277, 'thick')
    # marcas de centro
    for x, y0, y1 in ((210, 0, 10), (210, 287, 297)):
        pv.line(x, y0, x, y1, 'thin')
    for y, x0, x1 in ((148.5, 10, 20), (148.5, 410, 420)):
        pv.line(x0, y, x1, y, 'thin')


def title_block(pv, titulo, escala, folha):
    x0, y0, w, h = 240, 251, 170, 36
    pv.rect(x0, y0, w, h, 'thick')
    rows = [y0, y0 + 9, y0 + 18, y0 + 27, y0 + h]
    for y in rows[1:-1]:
        pv.line(x0, y, x0 + w, y, 'thin')
    def cell(x, y, label, value, wv=3.0, bold=False, color=None):
        pv.text(x + 1.2, y + 2.6, label, 1.6, 'start')
        pv.text(x + 1.2, y + 7.4, value, wv, 'start', bold=bold, color=color)
    pv.line(x0 + 110, y0, x0 + 110, y0 + 9, 'thin')
    cell(x0, y0, 'PROJETO', 'LADIANI VELORA 73 · buenos-x64-workstation', 2.6, True)
    cell(x0 + 110, y0, 'FORMATO / DIEDRO', 'A3 · 1º DIEDRO', 2.6)
    cell(x0, y0 + 9, 'TÍTULO', titulo, 2.8, True)
    for xs in (40, 75, 110, 140):
        pv.line(x0 + xs, y0 + 18, x0 + xs, y0 + 36, 'thin')
    cell(x0, y0 + 18, 'ESCALA', escala, 2.6, True)
    cell(x0 + 40, y0 + 18, 'UNIDADE', 'mm', 2.6)
    cell(x0 + 75, y0 + 18, 'DATA', DATE, 2.6)
    cell(x0 + 110, y0 + 18, 'REVISÃO', REV, 2.6)
    cell(x0 + 140, y0 + 18, 'FOLHA', folha, 2.6)
    cell(x0, y0 + 27, 'AUTOR', 'Mateus', 2.6, True)
    cell(x0 + 40, y0 + 27, 'TOLERÂNCIA GERAL', 'ISO 2768-mK', 2.4)
    cell(x0 + 75, y0 + 27, 'TECLAS', 'Fatar 61 TP/9 (ref.)', 2.2)
    cell(x0 + 110, y0 + 27, 'PENDENTE', 'em vermelho', 2.4, color=RED)
    # símbolo do 1º diedro
    sx, sy = x0 + 154, y0 + 31.5
    pv.circle(sx + 9, sy, 3.2, 'thin'); pv.circle(sx + 9, sy, 1.6, 'thin')
    pv.poly([(sx - 4, sy - 1.6), (sx + 3, sy - 3.2), (sx + 3, sy + 3.2), (sx - 4, sy + 1.6)], 'thin')


def table(pv, x, y, cols, rows, rh, size=2.0, header_size=2.0, title=None, red_tokens=('A DEFINIR', 'A DEF.', 'A CONFIRMAR')):
    if title:
        pv.text(x, y - 2, title, 2.6, 'start', bold=True)
    W = sum(c[1] for c in cols)
    pv.rect(x, y, W, rh * (len(rows) + 1), 'thick')
    pv.line(x, y + rh, x + W, y + rh, 'thin')
    cx = x
    for i, (name, w, al) in enumerate(cols):
        if i:
            pv.line(cx, y, cx, y + rh * (len(rows) + 1), 'thin')
        pv.text(cx + 1.2, y + rh - 1.1, name, header_size, 'start', bold=True)
        cx += w
    for r, row in enumerate(rows):
        yy = y + rh * (r + 2) - 1.0
        if r % 2 == 1:
            pass
        pv.line(x, y + rh * (r + 2), x + W, y + rh * (r + 2), 'dim')
        cx = x
        for (name, w, al), val in zip(cols, row):
            red = any(t in str(val) for t in red_tokens)
            if al == 'r':
                pv.text(cx + w - 1.2, yy, str(val), size, 'end', color=RED if red else None)
            else:
                pv.text(cx + 1.2, yy, str(val), size, 'start', color=RED if red else None)
            cx += w


def view_title(pv, x, y, text, sub=None):
    pv.text(x, y, text, 3.2, 'start', bold=True)
    if sub:
        pv.text(x, y + 3.6, sub, 1.8, 'start')


# =====================================================================
# Folhas
# =====================================================================
def sheet1():
    top, front, rear, sec = top_view(), front_view(), rear_view(), section_view()
    pv = paper('papel1')
    frame(pv)
    view_title(pv, 24, 16, 'VISTA FRONTAL — ESC. 1:5')
    view_title(pv, 24, 62, 'VISTA SUPERIOR — ESC. 1:5', 'Posições dos controles: tabela de coordenadas, folha 3/3 (origem O)')
    view_title(pv, 24, 165, 'VISTA POSTERIOR — ESC. 1:5', 'Vista de trás: a esquerda do desenho é o lado dos agudos')
    view_title(pv, 222, 202, 'CORTE A–A', 'ESC. 1:2 · pelo centro do C4')
    # balões
    T = lambda v, x, y: (v.ox + x * v.s, v.oy + y * v.s)
    Ftop = lambda X, Yd: (top.ox + X * top.s, top.oy + (DEPTH - Yd) * top.s)
    targets = [
        ('3', (12, 290)), ('13', (MASTER[2], MASTER[3] + 10)), ('30', (190, 228)),
        ('15', (215, FADER_Y + 4)), ('10', (FADER_X[3], FADER_Y + 20)), ('16', (FADER_X[5], PART_Y + LED_DY)),
        ('14', (FADER_X[7], PART_Y)), ('29', (450, 293)), ('1', (480, 300)), ('17', (DISP_C[0] + 60, DISP_C[1] + 40)),
        ('27', (690, 296)), ('12', (VALUE[2], VALUE[3] + 17)), ('11', (ENC_X[3], ENC_Y[0] + 10)),
        ('28', (1000, 272)), ('31', (1112, 280)), ('32', (1114.2, 254.4 + 15))]
    targets.sort(key=lambda t: Ftop(*t[1])[0])
    xs = [Ftop(*t[1])[0] for t in targets]
    for i in range(1, len(xs)):
        xs[i] = max(xs[i], xs[i - 1] + 8.5)
    over = xs[-1] - 262
    if over > 0:
        xs = [x - over for x in xs]
        for i in range(len(xs) - 2, -1, -1):
            xs[i] = min(xs[i], xs[i + 1] - 8.5)
    for (n, (X, Yd)), bx in zip(targets, xs):
        pv.balloon(n, Ftop(X, Yd), (bx, 70.5))
    pv.balloon('9', Ftop(WHEELS[0][1], WHEEL_Y - 20), (24, 118))
    pv.balloon('6', Ftop(110.4, 165), (24, 100))
    # frontal
    Ff = lambda X, Z: (front.ox + X * front.s, front.oy - Z * front.s)
    for n, (X, Z), (bx, byy) in [('19', (PHONES[2], PHONES[4]), (45, 51)), ('4', (300, 7), (75, 51)), ('7', (420, 40), (95, 51)),
                                 ('8', (BLACKS[10]['xl'] + 5, 54), (115, 51)), ('32', (FEET[0][0], -5), (35, 51)),
                                 ('3', (1150, 30), (250, 51)), ('9', (WHEELS[0][1], 60), (24, 30))]:
        pv.balloon(n, Ff(X, Z), (bx, byy))
    # posterior
    Fr = lambda u, Z: (rear.ox + u * rear.s, rear.oy - Z * rear.s)
    for n, u, bx in [('25', 80, 40), ('26', 105, 50), ('24', 160, 62), ('23', 185, 72), ('22', 255, 86), ('21', 355, 102),
                     ('20', 465, 120), ('18', 635, 150), ('5', 900, 210)]:
        pv.balloon(n, Fr(u, REAR_Z if n != '5' else 55), (bx, 197.5))
    # corte
    Fs = lambda Yd, Z: (sec.ox + Yd * sec.s, sec.oy - Z * sec.s)
    for n, (Yd, Z), (bx, byy) in [('1', (290, HEIGHT - 1), (186, 206)), ('2', (150, 0.6), (150, 251)), ('7', (60, Z_WHITE - 1), (52, 232)),
                                  ('8', (100, Z_BLACK - 2), (66, 232)), ('17', (200, HEIGHT - 8), (150, 206)),
                                  ('29', (200, 46), (158, 236)), ('27', (240, 11), (172, 240)), ('4', (8, 14.4), (49, 248)),
                                  ('5', (303.4, 30), (184, 230)), ('6', (40, 25), (110, 240))]:
        pv.balloon(n, Fs(Yd, Z), (bx, byy))
    # inconsistências do escopo
    x, y = 286, 198
    inc = ['INCONSISTÊNCIAS DO ESCOPO',
           '• MASTER no layout A, ausente da MCU do painel (HWI §4).',
           '• Botão power (HWI §12) ausente do layout A.',
           '• Waterfall E–E "assumido" (HWP:31); Story 11.6 aberta.',
           '• Mockup panel-v2: 2 LEDs no toggle; aprovado: 3 estados.',
           '• Alvo de toque "≈ 8 mm" vs 56 px × 0,150 = 8,4 mm.',
           '• Desenho Fatar recebido: 61 TP/9 (não o 73SW).']
    for i, s in enumerate(inc):
        pv.text(x, y + i * 4.4, s, 2.4 if i == 0 else 1.95, 'start', bold=(i == 0))
    # quadro de verificação e pendências
    x, y = 286, 150
    lines = [
        ('VERIFICAÇÃO DAS SOMAS', None, True),
        ('Teclas E1–E7: 43 brancas + 30 pretas = 73 ✓', None, False),
        ('43 × 23,6 − 1,4 = 1013,4 (teclas)', None, False),
        ('1013,4 + 2 × 10,4 = 1034,2 (moldura)', RED, False),
        ('25 + 80 + 1034,2 + 25 = 1164,2 = largura ✓', None, False),
        ('15 + 139,4 + 130 + 20 = 304,4 (prof.)', RED, False),
        ('1,2 + 47,5 + 10 + 8 + 2 = 68,7 (+10 pés)', RED, False),
        ('Em vermelho: depende de valor A DEFINIR', RED, False),
        ('ou A CONFIRMAR.', RED, False),
    ]
    for i, (s, c, b) in enumerate(lines):
        pv.text(x, y + i * 4.6, s, 2.4 if b else 2.1, 'start', color=c, bold=b)
    title_block(pv, 'VISTAS GERAIS E CORTE A–A', '1:5 · CORTE 1:2', '1/3')
    return [front, top, rear, sec, pv]


def sheet2():
    det = detail_view()
    pv = paper('papel2')
    frame(pv)
    view_title(pv, 24, 16, 'DETALHE B — UMA OITAVA (C4–B4) — ESC. 2:1', 'Vista de cima, interrompida entre 55 e 115 mm da frente da tecla. Cotas superiores: borda esquerda de cada preta a partir da borda esquerda do C4.')
    rows = [('Passo da branca', '23,6'), ('Largura da branca / vão', '22,2 / 1,4'), ('Oitava', '165,2'),
            ('Branca: total / até o tampo', '160,0 / 139,4'), ('Branca: cabeça (parte larga)', '48,0'),
            ('Preta: base / topo', '12,2 / 8,2'), ('Preta: total / visível', '110,0 / 89,4'),
            ('Preta: frente a partir da branca', '50,0'), ('Folga lateral preta/branca', '1,35'),
            ('Altura topo da branca (base keybed)', '47,5'), ('Preta acima da branca', '10,0'), ('Curso (frente da branca)', '10,0')]
    half = [rows[:6], rows[6:]]
    for i, part in enumerate(half):
        table(pv, 24 + i * 104, 257, [('MEDIDA', 70, 'l'), ('mm', 30, 'r')], part, 3.75, 1.85, 1.85,
              title='MEDIDAS DA TECLA (FATAR)' if i == 0 else None)
    pv.text(24, 285.2, 'Fonte: Fatar "Insieme tastiera 61 TP/9 C+ after touch", cód. dis. 94163200, rev. 11/06/02, esc. 1:1. Aplicação ao 73SW: A CONFIRMAR (Story 11.6).',
            1.6, 'start', color=RED)
    title_block(pv, 'DETALHE B — OITAVA C4–B4', '2:1', '2/3')
    return [det, pv]


NOTES = [
    'NOTAS GERAIS',
    '1. Dimensões em mm. Tolerâncias gerais ISO 2768-mK, salvo indicação.',
    '2. Desenho em 1º diedro (ABNT). Origem O: canto frontal esquerdo; X para a direita,',
    '    Y para trás, Z para cima a partir da face inferior do fundo (sem pés).',
    '3. Geometria das teclas medida no desenho Fatar 61 TP/9 C+ (cód. 94163200, esc. 1:1).',
    '    Aplicação ao 73SW e perfil waterfall E–E: A CONFIRMAR (Story 11.6).',
    '4. Nomenclatura: C4 = MIDI 60 (Dó central). Extensão E1–E7, 43 brancas + 30 pretas.',
    '5. Tampo em alumínio 2,0 escovado, acabamento que não mostra desgaste; fundo em aço',
    '    1,2 pintado; laterais em plástico resistente. Painel plano; cantos R3.',
    '6. Legendas em inglês, altura de maiúscula ≥ 2,5 mm, sobre faixa fosca ou jateada.',
    '    LEDs difusos Ø3, contraste ligado/desligado ≥ 10:1 a 50 klx.',
    '7. Knobs e caps em alumínio usinado anodizado; cap do REV diferenciado; VALUE',
    '    recartilhado com push; WRITE côncavo ou com anel; MENU com ponto central.',
    '8. Posições de controles, conectores e componentes internos são PROPOSTAS:',
    '    validar com o parceiro de hardware. Recortes dependem dos part numbers.',
    '9. Em vermelho: valor A DEFINIR ou A CONFIRMAR. Geometria em vermelho é provisória.',
    '10. Fonte externa homologada Inmetro (P4 com trava). Peso de referência: ~20 kg.',
]


def sheet3():
    pv = paper('papel3')
    frame(pv)
    table(pv, 24, 22, [('Nº', 9, 'l'), ('COMPONENTE', 80, 'l'), ('MATERIAL / ACABAMENTO', 74, 'l'), ('QTD', 15, 'r')],
          LEGEND, 4.6, 1.9, 2.0, title='LEGENDA DOS BALÕES')
    ny = 22 + 4.6 * (len(LEGEND) + 1) + 9
    for i, s in enumerate(NOTES):
        pv.text(24, ny + i * 4.15, s, 2.4 if i == 0 else 1.95, 'start', bold=(i == 0))
    rows = []
    for cid, name, x, y, z, obs in coord_rows():
        rows.append((cid, name, fmtc(x), fmtc(y), '—' if z is None else fmtc(z), obs))
    table(pv, 210, 22, [('ID', 12, 'l'), ('ELEMENTO', 52, 'l'), ('X', 17, 'r'), ('Y', 17, 'r'), ('Z', 13, 'r'), ('OBS.', 87, 'l')],
          rows, 3.47, 1.75, 1.9, title='COORDENADAS (PROPOSTA) — ORIGEM O, mm')
    title_block(pv, 'LEGENDA, COORDENADAS E NOTAS', 'SEM ESCALA', '3/3')
    return [pv]


def sheet_svg(views, sid):
    body = '\n'.join(v.svg() for v in views)
    return ('<svg xmlns="http://www.w3.org/2000/svg" id="%s" class="sheet" viewBox="0 0 420 297" width="420mm" height="297mm" '
            'font-family="%s" stroke-linecap="butt">\n<rect x="0" y="0" width="420" height="297" fill="#ffffff"/>\n%s\n</svg>') % (sid, FONT.replace("'", '&apos;').replace('"', '&quot;'), body)


def build():
    s1, s2, s3 = sheet1(), sheet2(), sheet3()
    svgs = [sheet_svg(s1, 'folha1'), sheet_svg(s2, 'folha2'), sheet_svg(s3, 'folha3')]
    for i, s in enumerate(svgs, 1):
        open(os.path.join(OUT, 'folha%d.svg' % i), 'w').write(s)
    for v in s1[:4] + s2[:1]:
        v.dxf(os.path.join(OUT, v.dxf_name))
    return svgs


if __name__ == '__main__':
    build()
    print('ok', WIDTH, DEPTH, HEIGHT, KEYBED_W, XK0, XK1, X_CUT)
