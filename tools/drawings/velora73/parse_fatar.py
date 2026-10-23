"""Extrai os segmentos de reta de um desenho Fatar em escala 1:1 para medir as teclas.

Uso:
    pdftocairo -svg <desenho_fatar.pdf> fatar.svg
    python3 parse_fatar.py fatar.svg segs.pkl

Grava uma lista de (cor, x1, y1, x2, y2) em mm (1 pt = 25,4/72 mm), com y para
cima como no PDF. Cores: blk, blu, red, grn. As medidas de model.py vieram
daqui, conferidas contra as cotas 848,2 e 869,00 impressas no desenho 61 TP/9.
"""
import pickle
import re
import sys

PT = 25.4 / 72
COLORS = {'13.7': 'blk', '18.0': 'blu', '92.9': 'red', '0%,': 'grn'}


def color_name(c):
    for k, v in COLORS.items():
        if k in c:
            return v
    return c


def segments(svg_text):
    body = svg_text.split('</defs>', 1)[1]
    out = []
    for m in re.finditer(r'<path[^>]*?stroke="([^"]*)"[^>]*?d="([^"]*)"', body):
        col = color_name(m.group(1))
        toks = re.findall(r'[MLCZ]|-?[\d.]+', m.group(2))
        i, cur = 0, None
        while i < len(toks):
            t = toks[i]
            if t == 'M':
                cur = (float(toks[i + 1]), float(toks[i + 2])); i += 3
            elif t == 'L':
                p = (float(toks[i + 1]), float(toks[i + 2]))
                out.append((col, cur[0] * PT, cur[1] * PT, p[0] * PT, p[1] * PT))
                cur = p; i += 3
            elif t == 'C':
                cur = (float(toks[i + 5]), float(toks[i + 6])); i += 7
            else:
                i += 1
    return out


if __name__ == '__main__':
    segs = segments(open(sys.argv[1]).read())
    pickle.dump(segs, open(sys.argv[2], 'wb'))
    print(len(segs), 'segmentos')
