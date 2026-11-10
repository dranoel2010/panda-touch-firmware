/**
 * Vermisst den Soft-White-Prototyp in Chrome (800×480, Faktor 1).
 * Für jeden Bildschirm/Zustand: Referenz-Screenshot, alle Elemente (Box, Stil, Aktion)
 * und alle Textzeilen (Position, Schrift, Farbe). Ergebnis: out/<name>.json + out/<name>.png
 *
 * Aufruf: NODE_PATH=<pfad zu node_modules> node extract.js [nur-diese-namen...]
 */
const fs = require('fs');
const path = require('path');
const puppeteer = require('puppeteer-core');

const HERE = __dirname;
const PROTO = path.join(HERE, '../../design/soft-white/prototype');
const OUT = path.join(HERE, 'out');
const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';

fs.mkdirSync(OUT, { recursive: true });
// Prototyp-CSS ohne Google-Fonts-Import (Schrift kommt lokal), Bild daneben verlinken
fs.writeFileSync(path.join(HERE, 'style-local.css'),
  fs.readFileSync(path.join(PROTO, 'style.css'), 'utf8').replace(/@import url\([^)]*\);?/g, ''));
const img = path.join(HERE, 'organizer.png');
if (!fs.existsSync(img)) fs.copyFileSync(path.join(PROTO, 'organizer.png'), img);

/* Bildschirme: Name -> JS, das im Prototyp den Zustand herstellt (Variablen aus app.js) */
const SCREENS = {
  home: `s=defaults();route='home';render();`,
  home_paused: `s=defaults();s.status='paused';route='home';render();`,
  home_finished: `s=defaults();stateChange('finished');`,
  home_idle: `s=defaults();stateChange('idle');`,
  home_offline: `s=defaults();stateChange('offline');`,
  home_error: `s=defaults();stateChange('error');`,
  home_heating: `s=defaults();stateChange('heating');`,
  home_levelling: `s=defaults();stateChange('levelling');`,
  home_loading: `s=defaults();stateChange('loading');`,
  home_lightoff: `s=defaults();s.light=false;route='home';render();`,
  menu: `s=defaults();route='menu';render();`,
  control: `s=defaults();route='control';render();`,
  control_paused: `s=defaults();s.status='paused';route='control';render();`,
  temperatures: `s=defaults();route='temperatures';render();`,
  filament: `s=defaults();route='filament';render();`,
  slot: `s=defaults();slotIndex=0;route='slot';render();`,
  slot_ext: `s=defaults();slotIndex=4;route='slot';render();`,
  wizard: `s=defaults();s.status='idle';wizardMode='laden';wizardStep=0;route='wizard';render();`,
  files: `s=defaults();fileSource='SD-Karte';route='files';render();`,
  files_usb_empty: `s=defaults();s.usb=false;fileSource='USB-Stick';route='files';render();`,
  file: `s=defaults();fileIndex=0;route='file';render();`,
  movement: `s=defaults();route='movement';render();`,
  fans: `s=defaults();route='fans';render();`,
  history: `s=defaults();route='history';render();`,
  messages: `s=defaults();route='messages';render();`,
  messages_error: `s=defaults();stateChange('error');route='messages';render();`,
  connection: `s=defaults();route='connection';render();`,
  setup: `s=defaults();route='setup';render();`,
  power: `s=defaults();route='power';render();`,
  system: `s=defaults();route='system';render();`,
  demo: `s=defaults();route='demo';render();`,
  modal_cancel: `s=defaults();route='control';render();dispatch('cancel');`,
  modal_replace: `s=defaults();fileIndex=1;route='file';render();dispatch('start-print');`,
  modal_move: `s=defaults();s.homed=true;route='movement';render();dispatch('move:x:1');`,
  modal_homeaxes: `s=defaults();route='movement';render();dispatch('home-axes');`,
  modal_extrude: `s=defaults();route='movement';render();dispatch('extrude:1');`,
  modal_tempoff: `s=defaults();route='temperatures';render();dispatch('temp-off');`,
  modal_poweroff: `s=defaults();route='power';render();dispatch('power-switch');`,
  modal_poweroff_idle: `s=defaults();s.status='idle';route='power';render();dispatch('power-switch');`,
  modal_factory: `s=defaults();route='system';render();dispatch('factory');`,
  modal_wizard: `s=defaults();slotIndex=0;route='slot';render();dispatch('wizard:laden');`,
  modal_restart: `s=defaults();route='system';render();dispatch('restart');`,
  busy_homeaxes: `s=defaults();s.status='idle';route='movement';render();dispatch('home-axes');`,
  busy_filament: `s=defaults();s.status='idle';wizardMode='laden';wizardStep=1;route='wizard';render();dispatch('wizard-next');`,
  busy_reconnect: `s=defaults();route='connection';render();dispatch('reconnect');`,
  busy_wifi: `s=defaults();route='setup';render();dispatch('setup-connect');`,
  busy_restart: `s=defaults();route='system';render();busy('Neustart','Panda Touch startet …',()=>{});`,
  busy_update: `s=defaults();route='system';render();dispatch('update');`,
  wizard_1: `s=defaults();s.status='idle';wizardMode='laden';wizardStep=1;route='wizard';render();`,
  wizard_2: `s=defaults();s.status='idle';wizardMode='laden';wizardStep=2;route='wizard';render();`,
  wizard_3: `s=defaults();s.status='idle';wizardMode='laden';wizardStep=3;route='wizard';render();`,
  wizard_out0: `s=defaults();s.status='idle';wizardMode='entladen';wizardStep=0;route='wizard';render();`,
  wizard_out1: `s=defaults();s.status='idle';wizardMode='entladen';wizardStep=1;route='wizard';render();`,
  wizard_out2: `s=defaults();s.status='idle';wizardMode='entladen';wizardStep=2;route='wizard';render();`,
  wizard_out3: `s=defaults();s.status='idle';wizardMode='entladen';wizardStep=3;route='wizard';render();`,
  filament_noams: `s=defaults();s.ams=false;route='filament';render();`,
  files_usb: `s=defaults();fileSource='USB-Stick';route='files';render();`,
  files_empty: `s=defaults();s.empty=true;route='files';render();`,
  files_offline: `s=defaults();stateChange('offline');fileSource='SD-Karte';route='files';render();`,
  file_1: `s=defaults();fileIndex=1;route='file';render();`,
  file_2: `s=defaults();fileIndex=2;route='file';render();`,
  history_empty: `s=defaults();s.history=[];route='history';render();`,
  messages_dev: `s=defaults();s.developer=false;route='connection';render();dispatch('reconnect');asyncToken++;closeModal();s.connected=false;s.messages=[{title:'Developer Mode ist aus',text:'Aktiviere den Developer Mode am Drucker und teste die Verbindung erneut.'}];s.status='offline';route='messages';render();`,
  connection_offline: `s=defaults();stateChange('offline');route='connection';render();`,
  toast: `s=defaults();route='home';render();toast('Licht eingeschaltet');`,
  sleep: `s=defaults();route='home';render();sleep();`,
};

/* Läuft im Browser: sammelt Elemente und Textzeilen */
function collect() {
  // :active-Regeln des Prototyps als [data-fa]-Regeln duplizieren, damit "gedrückt" messbar wird
  if (!document.getElementById('fa-rules')) {
    const css = [];
    for (const sh of document.styleSheets) { let rules; try { rules = sh.cssRules; } catch (e) { continue; }
      for (const r of rules) if (r.cssText.includes(':active')) css.push(r.cssText.replaceAll(':active', '[data-fa]')); }
    const st = document.createElement('style'); st.id = 'fa-rules'; st.textContent = css.join('\n'); document.head.appendChild(st);
  }
  const look = el => { const c = getComputedStyle(el); return { bg: c.backgroundColor, bgImage: c.backgroundImage,
    shadow: c.boxShadow, transform: c.transform, color: c.color, opacity: c.opacity, radius: c.borderRadius, border: c.border }; };
  const variants = el => {
    const v = {};
    el.setAttribute('data-fa', ''); v.pressed = look(el); el.removeAttribute('data-fa');
    if (el.tagName === 'BUTTON' && !el.classList.contains('switch')) {
      const had = el.classList.contains('selected'); el.classList.toggle('selected');
      v[had ? 'unselected' : 'selected'] = look(el);
      el.setAttribute('data-fa', ''); v[had ? 'unselectedPressed' : 'selectedPressed'] = look(el); el.removeAttribute('data-fa');
      el.classList.toggle('selected');
    }
    if (el.classList.contains('switch')) {
      const was = el.getAttribute('aria-checked'); el.setAttribute('aria-checked', was === 'true' ? 'false' : 'true');
      v.toggled = look(el); const a = getComputedStyle(el, '::after'); v.toggledKnob = { transform: a.transform };
      el.setAttribute('aria-checked', was);
    }
    return v;
  };
  const dev = document.querySelector('#device');
  const all = [...dev.querySelectorAll('*')];
  const idx = new Map(all.map((e, i) => [e, i]));
  const px = v => Math.round(v * 100) / 100;
  const els = all.map((el, i) => {
    const r = el.getBoundingClientRect(), cs = getComputedStyle(el);
    const act = el.closest('[data-action]');
    const pick = (pseudo) => { const p = getComputedStyle(el, pseudo); return p.content && p.content !== 'none' ? {
      bg: p.backgroundColor, bgImage: p.backgroundImage, w: p.width, h: p.height, radius: p.borderRadius,
      shadow: p.boxShadow, transform: p.transform, left: p.left, top: p.top, display: p.display } : null; };
    return {
      i, id: el.id || null, tag: el.tagName.toLowerCase(), cls: (el.getAttribute('class') || '').toString(),
      parent: idx.has(el.parentElement) ? idx.get(el.parentElement) : -1,
      action: el.dataset.action || null, field: el.dataset.field || null,
      actionOwner: act ? idx.get(act) : -1,
      attrs: { disabled: el.disabled || false, checked: el.getAttribute('aria-checked'), pressed: el.getAttribute('aria-pressed'),
               type: el.getAttribute('type'), value: el.value, min: el.getAttribute('min'), max: el.getAttribute('max'),
               style: el.getAttribute('style'), src: el.getAttribute('src'), d: el.tagName === 'path' ? el.getAttribute('d') : null },
      x: px(r.left), y: px(r.top), w: px(r.width), h: px(r.height),
      st: { bg: cs.backgroundColor, bgImage: cs.backgroundImage, radius: cs.borderRadius, shadow: cs.boxShadow,
            border: cs.border, borderBottom: cs.borderBottom, opacity: cs.opacity, color: cs.color, visibility: cs.visibility,
            display: cs.display, overflowY: cs.overflowY, transform: cs.transform, filter: cs.filter,
            font: `${cs.fontSize}|${cs.fontWeight}|${cs.letterSpacing}|${cs.lineHeight}|${cs.fontVariantNumeric}`,
            stroke: cs.stroke, strokeWidth: cs.strokeWidth, textAlign: cs.textAlign },
      before: pick('::before'), after: pick('::after'),
      scroll: el.scrollHeight > el.clientHeight + 1 && /auto|scroll/.test(cs.overflowY) ? { sh: el.scrollHeight, ch: el.clientHeight } : null,
      scrollBox: /auto|scroll/.test(cs.overflowY) ? { sh: el.scrollHeight, ch: el.clientHeight,
        pad: [cs.paddingTop, cs.paddingRight, cs.paddingBottom, cs.paddingLeft].map(parseFloat) } : null,
      variants: (el.dataset.action || el.tagName === 'BUTTON') ? variants(el) : null,
    };
  });
  // Textzeilen: jede Textknoten-Zeile einzeln (Umbruch exakt wie im Browser)
  const lines = [];
  const walker = document.createTreeWalker(dev, NodeFilter.SHOW_TEXT);
  let n;
  while ((n = walker.nextNode())) {
    const t = n.textContent; if (!t.trim()) continue;
    const el = n.parentElement, cs = getComputedStyle(el);
    if (cs.visibility === 'hidden' || cs.display === 'none') continue;
    const groups = []; let cur = null;
    for (let k = 0; k < t.length; k++) {
      const rg = document.createRange(); rg.setStart(n, k); rg.setEnd(n, k + 1);
      const rr = rg.getClientRects()[0]; if (!rr) continue;
      if (!cur || Math.abs(rr.top - cur.top) > 2) { cur = { top: rr.top, from: k, to: k + 1 }; groups.push(cur); } else cur.to = k + 1;
    }
    for (const g of groups) {
      const rg = document.createRange(); rg.setStart(n, g.from); rg.setEnd(n, g.to);
      const text = t.slice(g.from, g.to).replace(/\s+$/,'').replace(/^\s+/,'');
      if (!text) continue;
      // Rechteck nur über die sichtbaren Zeichen (ohne führende/folgende Leerzeichen)
      const lead = t.slice(g.from, g.to).length - t.slice(g.from, g.to).replace(/^\s+/,'').length;
      const rg2 = document.createRange(); rg2.setStart(n, g.from + lead); rg2.setEnd(n, g.from + lead + text.length);
      const r = rg2.getBoundingClientRect();
      lines.push({ text, el: idx.get(el), node: n, from: g.from + lead, len: text.length, single: groups.length === 1,
        x: px(r.left), y: px(r.top), w: px(r.width), h: px(r.height),
        font: `${cs.fontSize}|${cs.fontWeight}|${cs.letterSpacing}|${cs.lineHeight}|${cs.fontVariantNumeric}`,
        color: cs.color, opacity: cs.opacity });
    }
  }
  // Ausrichtung messen: einzeilige Texte probehalber verlängern und schauen, welche Kante stehen bleibt
  for (const l of lines) {
    if (!l.single) { l.align = 'left'; continue; }
    const n = l.node, orig = n.textContent;
    n.textContent = orig.slice(0, l.from + l.len) + 'M' + orig.slice(l.from + l.len);
    const rg = document.createRange(); rg.setStart(n, l.from); rg.setEnd(n, l.from + l.len + 1);
    const r = rg.getBoundingClientRect(); n.textContent = orig;
    const dl = Math.abs(r.left - l.x), dr = Math.abs(r.right - (l.x + l.w)), dc = Math.abs((r.left + r.right) / 2 - (l.x + l.w / 2));
    l.align = dl < 0.3 ? 'left' : dr < 0.3 ? 'right' : dc < 0.5 ? 'center' : 'left';
    l.box = (() => { const p = el2box(l.el); return p; })();
  }
  function el2box(i) { const e = all[i]; const c = getComputedStyle(e); const r = e.getBoundingClientRect();
    return { x: px(r.left + parseFloat(c.paddingLeft) + parseFloat(c.borderLeftWidth)),
             w: px(r.width - parseFloat(c.paddingLeft) - parseFloat(c.paddingRight) - parseFloat(c.borderLeftWidth) - parseFloat(c.borderRightWidth)) }; }
  lines.forEach(l => { delete l.node; });
  return { els, lines };
}

(async () => {
  const only = process.argv.slice(2);
  const browser = await puppeteer.launch({ executablePath: CHROME, headless: true,
    args: ['--allow-file-access-from-files', '--force-device-scale-factor=1', '--hide-scrollbars', '--font-render-hinting=none'] });
  const page = await browser.newPage();
  await page.setViewport({ width: 800, height: 480, deviceScaleFactor: 1 });
  await page.goto('file://' + path.join(HERE, 'harness.html'), { waitUntil: 'load' });
  await page.evaluate(() => document.fonts.ready);
  for (const [name, setup] of Object.entries(SCREENS)) {
    if (only.length && !only.includes(name)) continue;
    await page.evaluate(`(()=>{try{closeModal()}catch(e){};document.querySelector('.sleep')?.remove();
      const t=document.querySelector('#toast');t.classList.remove('show');${setup}})()`);
    await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
    const data = await page.evaluate(collect);
    await page.screenshot({ path: path.join(OUT, name + '.png'), clip: { x: 0, y: 0, width: 800, height: 480 } });
    fs.writeFileSync(path.join(OUT, name + '.json'), JSON.stringify(data));
    console.log(name.padEnd(18), 'Elemente', String(data.els.length).padStart(4), 'Textzeilen', data.lines.length,
      data.els.filter(e => e.scroll).map(e => `scroll ${e.scroll.sh}/${e.scroll.ch}`).join(' '));
  }
  await browser.close();
})();
