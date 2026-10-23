"""Velora 73: geometria do instrumento em mm reais.

Origem O no canto frontal esquerdo do gabinete, vista de cima:
X para a direita, Y (profundidade) para trás, Z para cima a partir da face
inferior do fundo (sem pés). Fontes de cada valor: ver decisoes.md.
"""

# ---------------- teclado (desenho Fatar 61 TP/9 C+, cód. 94163200) ----------------
P = 23.6          # passo da tecla branca
WW = 22.2         # largura da tecla branca
GAP = 1.4         # vão entre brancas
OCT = 7 * P       # 165,2
LW_TOT = 160.0    # comprimento total da branca
HEAD = 48.0       # parte larga (cabeça) da branca
VIS = 139.4       # linha de referência do desenho Fatar (borda do tampo)
BW = 12.2         # largura da preta (base)
BTOP = 8.2        # largura do topo da preta
BOFF = 50.0       # frente da preta a partir da frente da branca
BCH = 3.5         # chanfro frontal da preta
BTOP_END = 136.0  # fim do topo da preta
BTOP_TAPER = 101.67
BCLR = 1.35       # folga lateral preta/branca
BLACK_OFF = {'C#': 14.55, 'D#': 42.65, 'F#': 84.27, 'G#': 111.21, 'A#': 138.15}
H_WHITE = 47.5    # base do keybed -> topo da branca
H_BLACK = 10.0    # preta acima da branca
TRAVEL = 10.0     # curso na frente da branca
FRAME_SIDE = 10.4  # moldura do keybed por lado (61 TP/9; A CONFIRMAR no 73SW)
KEYBED_DEPTH = 161.5

N_WHITE = 43
N_BLACK = 30

# ---------------- gabinete (decisões do Mateus) ----------------
CHEEK = 25.0
WHEEL_BLOCK = 80.0
LIP = 15.0
REAR_BORDER = 20.0
PANEL_ZONE = 130.0   # proposta: vidro do display (A DEFINIR) + margens
T_TOP = 2.0
T_BOT = 1.2
CLEAR = 8.0          # folga topo da preta -> face inferior do tampo (A DEFINIR)
FASCIA = 5.0         # aba frontal do tampo (proposta)
FOOT_D, FOOT_H, FOOT_IN = 30.0, 10.0, 50.0
R_CORNER = 3.0
LIP_H = 15.0         # altura do lábio frontal (A DEFINIR)
WATERFALL = 20.0     # altura da face waterfall (A DEFINIR)

KEYBED_W = N_WHITE * P - GAP + 2 * FRAME_SIDE           # 1034,2
WIDTH = 2 * CHEEK + WHEEL_BLOCK + KEYBED_W               # 1164,2
XK0 = CHEEK + WHEEL_BLOCK + FRAME_SIDE                   # borda esquerda do E1
XK1 = XK0 + N_WHITE * P - GAP                            # borda direita do E7
DEPTH = LIP + VIS + PANEL_ZONE + REAR_BORDER             # 304,4
Y_PANEL = LIP + VIS                                      # 154,4 borda frontal do tampo
Z_KB = T_BOT                                             # face inferior do keybed
Z_WHITE = Z_KB + H_WHITE                                 # 48,7
Z_BLACK = Z_WHITE + H_BLACK                              # 58,7
HEIGHT = Z_BLACK + CLEAR + T_TOP                         # 68,7
X_MID = WIDTH / 2

WHITE_NAMES = ['C', 'D', 'E', 'F', 'G', 'A', 'B']
LEFT_BLACK = {'D': 'C#', 'E': 'D#', 'G': 'F#', 'A': 'G#', 'B': 'A#'}
RIGHT_BLACK = {'C': 'C#', 'D': 'D#', 'F': 'F#', 'G': 'G#', 'A': 'A#'}


def c_left(octave):
    """Borda esquerda do Dó da oitava (C4 = MIDI 60). E1 é a tecla 0."""
    # E1 está 2 brancas acima de C1
    return XK0 - 2 * P + (octave - 1) * OCT


def keys():
    whites, blacks = [], []
    idx = WHITE_NAMES.index('E')
    octv = 1
    for i in range(N_WHITE):
        name = WHITE_NAMES[idx]
        whites.append(dict(name=name, octave=octv, xl=XK0 + i * P))
        idx += 1
        if idx == 7:
            idx, octv = 0, octv + 1
    lo, hi = XK0, XK1
    for o in range(1, 8):
        for nm, off in BLACK_OFF.items():
            x = c_left(o) + off
            if x > lo and x + BW < hi:
                blacks.append(dict(name=nm, octave=o, xl=x))
    # contorno de cada branca: cabeça e cauda
    bx = {(b['name'], b['octave']): b['xl'] for b in blacks}
    for w in whites:
        xl, xr = w['xl'], w['xl'] + WW
        lb = LEFT_BLACK.get(w['name'])
        rb = RIGHT_BLACK.get(w['name'])
        tl = bx[(lb, w['octave'])] + BW + BCLR if lb and (lb, w['octave']) in bx else xl
        tr = bx[(rb, w['octave'])] - BCLR if rb and (rb, w['octave']) in bx else xr
        w.update(xr=xr, tl=tl, tr=tr)
    return whites, blacks


WHITES, BLACKS = keys()
assert len(WHITES) == N_WHITE and len(BLACKS) == N_BLACK, (len(WHITES), len(BLACKS))
assert abs(KEYBED_W - 1034.2) < 1e-6 and abs(WIDTH - 1164.2) < 1e-6

# ---------------- painel (posições PROPOSTAS) ----------------
FADER_X = [255 + 20 * i for i in range(9)]
FADER_Y = 245.0
FADER_TRAVEL = 45.0
PART_Y = 180.0
LED_DY = 11.5
ENC_X = [840, 872, 904, 936]
ENC_Y = [241.0, 209.0]
ENC_NAMES = [['DELAY', 'BRIGHTNESS', 'ATTACK', 'RELEASE'], ['LOW', 'MID', 'MID FREQ', 'HIGH']]
DISP_C = (X_MID, Y_PANEL + PANEL_ZONE / 2)
DISP_ACTIVE = (153.6, 90.0)
DISP_GLASS = (175.0, 110.0)   # A DEFINIR (provisório)
WHEELS = [('PITCH', 50.0), ('MOD', 80.0)]
WHEEL_Y = 85.0
WHEEL_SIZE = (15.0, 60.0)     # A DEFINIR (provisório)
BTN = (14.0, 9.0)
LED_D = 3.0

BUTTONS = [  # id, rótulo, X, Y
    *[('14.%d' % (i + 1), 'PARTE/CENA %d' % (i + 1), FADER_X[i], PART_Y) for i in range(8)],
    ('14.9', 'LIVE SET ◀', 715, 262), ('14.10', 'LIVE SET ▶', 735, 262),
    ('14.11', 'SCENE ◀', 715, 240), ('14.12', 'SCENE ▶', 735, 240),
    ('14.13', 'EXIT', 705, 172), ('14.14', 'MENU', 725, 172), ('14.15', 'WRITE', 745, 172),
    ('14.16', 'ROTARY STOP', 110, 215), ('14.17', 'ROTARY S/F', 135, 215),
]
TOGGLES = [('15.1', 'DRAWBARS / MIXER', 215, FADER_Y, 2), ('15.2', 'PARTS / SCENES', 215, PART_Y, 2),
           ('15.3', 'PART / ALL / ASSIGN', 990, 225, 3)]
VALUE = ('12', 'VALUE (push)', 725, 205, 35.0)
MASTER = ('13', 'MASTER', 55, 215, 20.0)
PHONES = ('19', 'PHONES ¼" TRS', 65.0, 0.0, 7.5)

# conectores traseiros: u = distância da borda esquerda vista DE TRÁS
REAR_Z = 35.0
REAR = [
    ('25', 'DC IN (P4 c/ trava)', 80, 'dc'), ('26', 'POWER', 105, 'btn'),
    ('24', 'USB-A (host)', 160, 'usba'), ('23', 'USB-C (device)', 185, 'usbc'),
    ('22.1', 'MIDI OUT', 240, 'din'), ('22.2', 'MIDI IN', 270, 'din'),
    ('21.1', 'PEDAL SUSTAIN', 330, 'jack'), ('21.2', 'PEDAL EXPRESSION', 355, 'jack'), ('21.3', 'PEDAL SWITCH', 380, 'jack'),
    ('20.1', 'LINE IN L ¼"', 440, 'jack'), ('20.2', 'LINE IN R ¼"', 465, 'jack'), ('20.3', 'LINE IN 3,5 mm', 490, 'mini'),
    ('18.3', 'AUX OUT R', 550, 'jack'), ('18.4', 'AUX OUT L', 575, 'jack'),
    ('18.1', 'MAIN OUT R', 635, 'jack'), ('18.2', 'MAIN OUT L', 660, 'jack'),
]
REAR_LABEL = {'25': 'DC IN', '26': 'POWER', '24': 'USB-A', '23': 'USB-C', '22.1': 'MIDI OUT', '22.2': 'MIDI IN',
              '21.1': 'SUST', '21.2': 'EXP', '21.3': 'SW', '20.1': 'L', '20.2': 'R', '20.3': '3,5',
              '18.3': 'R', '18.4': 'L', '18.1': 'R', '18.2': 'L'}

FEET = [(FOOT_IN, FOOT_IN), (WIDTH - FOOT_IN, FOOT_IN), (FOOT_IN, DEPTH - FOOT_IN), (WIDTH - FOOT_IN, DEPTH - FOOT_IN)]

# componentes internos: X0, Y0, X1, Y1, Z0, Z1 (PROPOSTA; dimensões A DEFINIR, exceto NVMe)
INTERNAL = {
    '27': ('Placa principal (SoM RK3588 + PCM3168A)', 500, 186, 1090, 298, 10.0, 11.6),
    '28': ('SSD NVMe M.2 2280', 960, 250, 1040, 272, 13.0, 15.4),
    '29a': ('Placa do painel (esq.)', 30, 160, 470, 296, 45.0, 46.6),
    '29b': ('Placa do painel (dir.)', 680, 160, 1060, 296, 45.0, 46.6),
    '30': ('Placa MCU do teclado', 110, 182, 200, 230, 10.0, 11.6),
    '31': ('Supercapacitores (hold-up)', 1095, 200, 1130, 280, 3.0, 30.0),
}

X_CUT = c_left(4) + WW / 2   # corte A-A pelo centro do C4

LEGEND = [  # nº, componente, material/acabamento, qtd
    ('1', 'Tampo (painel superior)', 'Alumínio 2,0 escovado (não mostra desgaste)', '1'),
    ('2', 'Fundo', 'Aço 1,2 pintado', '1'),
    ('3', 'Lateral (end cheek)', 'Plástico resistente (resina A DEFINIR)', '2'),
    ('4', 'Lábio frontal', 'A DEFINIR', '1'),
    ('5', 'Painel traseiro', 'A DEFINIR', '1'),
    ('6', 'Teclado Fatar 73SW (E1–E7, waterfall*)', 'Conjunto Fatar (comprado)', '1'),
    ('7', 'Tecla branca', 'Fatar', '43'),
    ('8', 'Tecla preta', 'Fatar', '30'),
    ('9', 'Roda PITCH / MOD', 'A DEFINIR', '2'),
    ('10', 'Fader 45 mm, cap (REV diferenciado)', 'Cap: alumínio usinado anodizado', '9'),
    ('11', 'Encoder c/ anel de LED, knob Ø20', 'Knob: alumínio usinado anodizado', '8'),
    ('12', 'Encoder VALUE Ø35 recartilhado, push', 'Knob: alumínio usinado anodizado', '1'),
    ('13', 'Potenciômetro MASTER, knob Ø20', 'Knob: alumínio usinado anodizado', '1'),
    ('14', 'Botão 14×9 (WRITE côncavo, MENU c/ ponto)', 'A DEFINIR', '17'),
    ('15', 'Botão toggle c/ LED por estado', 'A DEFINIR', '3'),
    ('16', 'LED difuso Ø3 (≥ 10:1 a 50 klx)', 'Comprado', '15'),
    ('17', 'Display 7" 1024×600 + vidro AG/AR colado', 'Comprado', '1'),
    ('18', 'Jack ¼" TRS saída balanceada (MAIN/AUX)', 'Comprado', '4'),
    ('19', 'Jack ¼" TRS fone (frontal)', 'Comprado', '1'),
    ('20', 'Entrada de linha: ¼" L/R + 3,5 mm', 'Comprado', '3'),
    ('21', 'Jack ¼" TRS pedal (sust./expr./switch)', 'Comprado', '3'),
    ('22', 'MIDI DIN 5 pinos (IN/OUT)', 'Comprado', '2'),
    ('23', 'USB-C (device)', 'Comprado', '1'),
    ('24', 'USB-A (host)', 'Comprado', '1'),
    ('25', 'Conector DC P4 com trava (fonte externa)', 'Comprado', '1'),
    ('26', 'Botão power (soft)', 'Comprado', '1'),
    ('27', 'Placa principal (SoM RK3588 + codec PCM3168A)', 'PCB (parceiro)', '1'),
    ('28', 'SSD NVMe M.2 2280 (22×80)', 'Comprado', '1'),
    ('29', 'Placa(s) do painel com MCU', 'PCB (parceiro)', 'A DEF.'),
    ('30', 'Placa MCU do teclado', 'PCB (parceiro)', '1'),
    ('31', 'Supercapacitores (hold-up ≥ 150 ms)', 'Comprado', 'A DEF.'),
    ('32', 'Pé Ø30×10 + parafuso M4', 'Borracha / aço zincado', '4'),
]


def coord_rows():
    rows = []
    for nm, x in WHEELS:
        rows.append(('9.%d' % (1 if nm == 'PITCH' else 2), 'Roda ' + nm, x, WHEEL_Y, None, 'tamanho A DEFINIR'))
    rows.append((MASTER[0], MASTER[1], MASTER[2], MASTER[3], None, 'knob Ø20'))
    for b in BUTTONS[15:]:
        rows.append((b[0], b[1], b[2], b[3], None, 'botão 14×9'))
    rows.append(('15.1', TOGGLES[0][1], TOGGLES[0][2], TOGGLES[0][3], None, '2 LEDs'))
    for i, x in enumerate(FADER_X):
        rows.append(('10.%d' % (i + 1), 'Fader ' + ('REV' if i == 8 else str(i + 1)), x, FADER_Y, None, 'centro do curso 45'))
    rows.append(('15.2', TOGGLES[1][1], TOGGLES[1][2], TOGGLES[1][3], None, '2 LEDs'))
    for b in BUTTONS[:8]:
        rows.append((b[0], b[1], b[2], b[3], None, 'LED em Y=%s' % fmtc(b[3] + LED_DY)))
    rows.append(('17', 'Display (centro da área ativa)', DISP_C[0], DISP_C[1], None, 'área 153,6×90,0'))
    for b in BUTTONS[8:15]:
        rows.append((b[0], b[1], b[2], b[3], None, 'botão 14×9'))
    rows.append((VALUE[0], VALUE[1], VALUE[2], VALUE[3], None, 'knob Ø35'))
    k = 1
    for r, yy in enumerate(ENC_Y):
        for c, x in enumerate(ENC_X):
            rows.append(('11.%d' % k, 'Encoder ' + ENC_NAMES[r][c], x, yy, None, 'knob Ø20, anel Ø28'))
            k += 1
    rows.append(('15.3', TOGGLES[2][1], TOGGLES[2][2], TOGGLES[2][3], None, '3 LEDs'))
    rows.append((PHONES[0], PHONES[1], PHONES[2], PHONES[3], PHONES[4], 'face frontal'))
    for cid, name, u, kind in REAR:
        rows.append((cid, name, WIDTH - u, DEPTH, REAR_Z, 'face traseira'))
    for i, (x, y) in enumerate(FEET):
        rows.append(('32.%d' % (i + 1), 'Pé / furo M4', x, y, 0.0, 'face inferior'))
    return rows


def fmtc(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return s.replace('.', ',')
