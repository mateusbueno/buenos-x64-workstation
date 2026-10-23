# Velora 73 technical drawing

Generator for the instrument's technical drawing (rev. 00, 2026-10-23, author Mateus). The geometry is hand-built in real millimetres (1 unit = 1 mm) and written to three A3 landscape sheets:

1. Front, top and rear views at 1:5, and section A–A through C4 at 1:2.
2. Detail B: one octave (C4–B4) at 2:1.
3. Balloon legend, proposed coordinates, and general notes.

Dimensions and their sources live in `_bmad-output/specs/spec-arm64-stage-workstation/mechanical.md`. Keep `model.py` and that file in step. Founder decisions behind the drawing are in `decisoes.md`. Red on the sheets means a value that is TBD or still to confirm.

## Files

| File | Role |
|---|---|
| `model.py` | All dimensions, key layout, control positions, legend and coordinate rows |
| `geom.py` | Drawing primitives: ISO 129 dimensions, balloons, break lines, SVG and DXF output |
| `sheets.py` | Builds the views and the three sheets |
| `build_html.py` | Writes the SVG sheets, the DXF views, `print.html` and the zoom/pan viewer |
| `viewer_template.html` | Viewer page; the sheets are inlined at `<!--SHEETS-->` |
| `render.js` | Playwright: A3 PDF from `print.html`, PNG previews of the sheets |
| `parse_fatar.py` | Reads line segments from a 1:1 Fatar drawing, to measure keys |
| `out/` | Generated drawing: PDF, one DXF per view, SVG sheets, viewer HTML |

## Regenerate

```sh
pip install ezdxf                     # DXF output
python3 build_html.py                 # out/*.svg, out/*.dxf, out/print.html, viewer
node render.js pdf                    # out/velora73_desenho_tecnico_A3.pdf (needs playwright)
node render.js png 1,2,3              # out/folha*.png previews
```

Run `render.js` with `NODE_PATH` pointing at a global Playwright install if it is not in a local `node_modules`. Print the PDF at 100% (no fit to page) so the stated scales hold.

The Fatar reference drawing (61 TP/9 C+, drawing code 94163200) is marked proprietary by Fatar. It is not in the repository. Doepfer hosts it at <https://doepfer.de/service/61TP9_drawing.pdf>.
