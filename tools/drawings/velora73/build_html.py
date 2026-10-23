import os, sheets
svgs = sheets.build()
OUT = sheets.OUT

# ---------- PDF (A3 paisagem, 1:1 em mm) ----------
pages = ''.join('<div class="pg">%s</div>' % s for s in svgs)
open(os.path.join(OUT, 'print.html'), 'w').write(
    '<!doctype html><html><head><meta charset="utf-8"><style>@page{size:420mm 297mm;margin:0}'
    'html,body{margin:0;padding:0;background:#fff}.pg{width:420mm;height:297mm;overflow:hidden;page-break-after:always}'
    '.pg:last-child{page-break-after:auto}.pg svg{display:block;width:420mm;height:297mm}</style></head><body>%s</body></html>' % pages)

# ---------- artefato ----------
tpl = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'viewer_template.html')).read()
inline = ''.join('<div class="sheet-wrap" data-sheet="%d"%s>%s</div>' % (i + 1, '' if i == 0 else ' hidden',
                 s.replace('width="420mm" height="297mm"', '')) for i, s in enumerate(svgs))
open(os.path.join(OUT, 'velora73-desenho-tecnico.html'), 'w').write(tpl.replace('<!--SHEETS-->', inline))
print('html ok')
