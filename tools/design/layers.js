/**
 * Rendert aus den Messdaten (out/*.json) alle Grafik-Bausteine in Chrome:
 *  - Schatten: jeder einzelne box-shadow als Alpha-Maske in exakter Elementgröße
 *  - Icons: SVG-Striche als Alpha-Maske
 *  - Bilder: <img> mit object-fit und Filter als RGBA
 *  - Sonderfälle (Schalter-Knopf, Trennlinie, native Regler, ✓) als RGBA
 * Ergebnis: out/layers/<hash>.png + out/layers.json (Schlüssel -> Datei, Ränder)
 */
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const puppeteer = require('puppeteer-core');

const HERE = __dirname, OUT = path.join(HERE, 'out'), LAY = path.join(OUT, 'layers');
const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
fs.mkdirSync(LAY, { recursive: true });

/* "rgb(198, 197, 193) 7px 7px 14px 0px, rgba(...) -7px ... inset" -> Liste */
function parseShadows(s) {
  if (!s || s === 'none') return [];
  const parts = []; let depth = 0, cur = '';
  for (const ch of s) { if (ch === '(') depth++; if (ch === ')') depth--; if (ch === ',' && !depth) { parts.push(cur.trim()); cur = ''; } else cur += ch; }
  if (cur.trim()) parts.push(cur.trim());
  return parts.map(p => {
    const color = (p.match(/rgba?\([^)]*\)/) || ['rgb(0,0,0)'])[0];
    const nums = p.replace(color, '').replace('inset', '').trim().split(/\s+/).map(parseFloat);
    const [dx, dy, blur = 0, spread = 0] = nums;
    return { color, dx, dy, blur, spread, inset: /inset/.test(p) };
  });
}
const rbox = e => { const x0 = Math.round(e.x), y0 = Math.round(e.y); return { x: x0, y: y0, w: Math.round(e.x + e.w) - x0, h: Math.round(e.y + e.h) - y0 }; };
const key = o => crypto.createHash('sha1').update(JSON.stringify(o)).digest('hex').slice(0, 12);

const jobs = new Map();   // key -> job
function addShadowJobs(w, h, radius, shadowStr) {
  for (const sh of parseShadows(shadowStr)) {
    const M = sh.inset ? 0 : Math.ceil(Math.max(Math.abs(sh.dx), Math.abs(sh.dy)) + sh.blur + Math.max(0, sh.spread)) + 2;
    const job = { kind: 'shadow', w, h, radius, dx: sh.dx, dy: sh.dy, blur: sh.blur, spread: sh.spread, inset: sh.inset, M };
    jobs.set(key(job), job);
  }
}

const screens = fs.readdirSync(OUT).filter(f => f.endsWith('.json') && f !== 'layers.json').map(f => f.slice(0, -5));
for (const name of screens) {
  const d = JSON.parse(fs.readFileSync(path.join(OUT, name + '.json')));
  for (const e of d.els) {
    const b = rbox(e);
    if (!b.w || !b.h || e.st.display === 'none') continue;
    addShadowJobs(b.w, b.h, e.st.radius, e.st.shadow);
    if (e.variants) for (const v of Object.values(e.variants)) if (v && v.shadow) addShadowJobs(b.w, b.h, v.radius || e.st.radius, v.shadow);
    if (e.tag === 'svg') {
      const paths = d.els.filter(p => p.parent === e.i && p.tag === 'path').map(p => p.attrs.d);
      const job = { kind: 'icon', w: b.w, h: b.h, sw: e.st.strokeWidth, paths };
      jobs.set(key(job), job);
    }
    if (e.tag === 'img' && false) {}
    if (e.tag === 'img') {
      const job = { kind: 'img', w: e.w, h: e.h, src: e.attrs.src, filter: e.st.filter, fit: 'contain' };
      jobs.set(key(job), job);
    }
  }
}

// Textzeilen mit Zeichen, die Manrope nicht hat (✓): Chrome nimmt eine Systemschrift -> als Maske übernehmen
for (const name of screens) {
  const d = JSON.parse(fs.readFileSync(path.join(OUT, name + '.json')));
  for (const l of d.lines) if (/[✓⛶]/.test(l.text)) {
    const [size, weight, ls] = l.font.split('|');
    const job = { kind: 'text', text: l.text, size, weight, ls, w: Math.ceil(l.w) + 4, h: Math.ceil(l.h) };
    jobs.set(key(job), job);
  }
}

// Pseudo-Elemente: Schalter-Knopf (.switch::after) – 40×40, rund, 3px 3px 5px #7e847a70
addShadowJobs(40, 40, '50%', 'rgba(126, 132, 122, 0.44) 3px 3px 5px 0px');

(async () => {
  const browser = await puppeteer.launch({ executablePath: CHROME, headless: true,
    args: ['--allow-file-access-from-files', '--force-device-scale-factor=1', '--hide-scrollbars', '--font-render-hinting=none'] });
  const page = await browser.newPage();
  await page.setViewport({ width: 1600, height: 1200, deviceScaleFactor: 1 });
  const base = 'file://' + HERE + '/';
  await page.goto(base + 'harness.html', { waitUntil: 'load' });
  await page.evaluate(() => { document.body.innerHTML = '<div id="stage" style="position:absolute;left:0;top:0"></div>';
    document.querySelectorAll('style').forEach(s => { if (s.textContent.includes('#eeede9!important')) s.remove(); });
    document.documentElement.style.setProperty('background', 'transparent', 'important'); document.body.style.setProperty('background', 'transparent', 'important'); });
  const index = {};
  for (const [k, j] of jobs) {
    const file = path.join(LAY, k + '.png');
    let clip;
    if (j.kind === 'shadow') {
      const M = j.M;
      await page.evaluate((j, M) => {
        document.getElementById('stage').innerHTML = `<div style="position:absolute;left:${M}px;top:${M}px;width:${j.w}px;height:${j.h}px;box-sizing:border-box;border-radius:${j.radius};box-shadow:${j.dx}px ${j.dy}px ${j.blur}px ${j.spread}px #000${j.inset ? ' inset' : ''};background:transparent"></div>`;
      }, j, M);
      clip = { x: 0, y: 0, width: j.w + 2 * M, height: j.h + 2 * M };
      index[k] = { ...j, file: k + '.png', ox: -M, oy: -M };
    } else if (j.kind === 'icon') {
      await page.evaluate(j => {
        document.getElementById('stage').innerHTML = `<svg width="${j.w}" height="${j.h}" viewBox="0 0 24 24" style="position:absolute;left:0;top:0;fill:none;stroke:#000;stroke-width:${j.sw};stroke-linecap:round;stroke-linejoin:round">${j.paths.map(d => `<path d="${d}"/>`).join('')}</svg>`;
      }, j);
      clip = { x: 0, y: 0, width: j.w, height: j.h };
      index[k] = { ...j, file: k + '.png', ox: 0, oy: 0 };
    } else if (j.kind === 'text') {
      await page.evaluate(j => {
        document.getElementById('stage').innerHTML = `<span style="position:absolute;left:0;top:0;white-space:nowrap;font-family:Manrope;font-size:${j.size};font-weight:${j.weight};letter-spacing:${j.ls};color:#000;line-height:normal">${j.text}</span>`;
      }, j);
      clip = { x: 0, y: 0, width: j.w, height: j.h };
      index[k] = { ...j, file: k + '.png', ox: 0, oy: 0 };
    } else if (j.kind === 'img') {
      const M = 12;
      await page.evaluate((j, M) => {
        document.getElementById('stage').innerHTML = `<img src="${j.src}" style="position:absolute;left:${M}px;top:${M}px;width:${j.w}px;height:${j.h}px;object-fit:${j.fit};filter:${j.filter}">`;
      }, j, M);
      await page.evaluate(() => Promise.all([...document.images].map(i => i.complete ? 0 : new Promise(r => i.onload = r))));
      clip = { x: 0, y: 0, width: Math.ceil(j.w) + 2 * M, height: Math.ceil(j.h) + 2 * M };
      index[k] = { ...j, file: k + '.png', ox: -M, oy: -M };
    }
    await page.screenshot({ path: file, clip, omitBackground: true });
  }
  fs.writeFileSync(path.join(OUT, 'layers.json'), JSON.stringify(index));
  const c = {}; for (const j of jobs.values()) c[j.kind] = (c[j.kind] || 0) + 1;
  console.log('Bausteine:', c);
  await browser.close();
})();
