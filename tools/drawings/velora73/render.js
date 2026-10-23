const { chromium } = require('playwright');
const fs = require('fs'); const path = require('path');
(async () => {
  const out = path.join(__dirname, 'out');
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 2100, height: 1485 } });
  const mode = process.argv[2] || 'png';
  if (mode === 'png') {
    for (const n of (process.argv[3] || '1,2,3').split(',')) {
      const svg = fs.readFileSync(path.join(out, `folha${n}.svg`), 'utf8').replace('width="420mm" height="297mm"', 'width="2100" height="1485"');
      await page.setContent(`<html><body style="margin:0">${svg}</body></html>`);
      await page.screenshot({ path: path.join(out, `folha${n}.png`) });
      if (process.argv[4]) {
        const [x, y, w, h, z] = process.argv[4].split(',').map(Number);
        const svg2 = fs.readFileSync(path.join(out, `folha${n}.svg`), 'utf8').replace('width="420mm" height="297mm"', `width="${420*z}" height="${297*z}"`);
        await page.setViewportSize({ width: Math.round(w*z), height: Math.round(h*z) });
        await page.setContent(`<html><body style="margin:0"><div style="margin-left:${-x*z}px;margin-top:${-y*z}px">${svg2}</div></body></html>`);
        await page.screenshot({ path: path.join(out, `zoom${n}.png`) });
        await page.setViewportSize({ width: 2100, height: 1485 });
      }
    }
  } else {
    const html = fs.readFileSync(path.join(out, 'print.html'), 'utf8');
    await page.setContent(html, { waitUntil: 'load' });
    await page.pdf({ path: path.join(out, 'velora73_desenho_tecnico_A3.pdf'), width: '420mm', height: '297mm', printBackground: true, margin: { top: 0, right: 0, bottom: 0, left: 0 } });
  }
  await browser.close();
})();
