"""Erzeugt aus den Messdaten des Prototyps (out/*.json, out/layers*) die C-Tabellen für die Firmware.

Ausgabe: main/ui/gen/sw_gen_assets.{c,h}, main/ui/gen/sw_gen_screens.{c,h}
Aufruf:  python gen.py [bildschirm ...]   (ohne Angabe: alle)
"""
import glob
import json
import math
import os
import re
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
REPO = os.path.normpath(os.path.join(HERE, '../..'))
GEN = os.path.join(REPO, 'main/ui/gen')
FONTDIR = os.path.join(REPO, 'main/ui/fonts')
os.makedirs(GEN, exist_ok=True)

LAYERS = json.load(open(os.path.join(OUT, 'layers.json')))
ASC = 2132 / 2000  # hhea-Ascent von Manrope relativ zur Schriftgröße (Chrome: line-height normal)

# Icon-Namen aus app.js (Pfad -> Name), damit die Logik Icons beim Namen nennen kann
APP = open(os.path.join(REPO, 'design/soft-white/prototype/app.js'), encoding='utf8').read()
ICON_NAMES = {d: n for n, d in re.findall(r"(\w+):'([^']*)'", APP.split('const paths=')[1].split('};')[0])}

# ---------------------------------------------------------------- Hilfen

def rgba(s):
    m = re.match(r'rgba?\(([^)]*)\)', s or '')
    if not m:
        return (0, 0, 0, 0)
    p = [float(x) for x in m.group(1).replace('/', ',').split(',')]
    return (int(p[0]), int(p[1]), int(p[2]), int(round((p[3] if len(p) > 3 else 1) * 255)))


def r565(c):
    """Farbe so wählen, dass LVGLs Abschneiden auf RGB565 die nächstgelegene Displayfarbe trifft."""
    # Mitte der RGB565-Stufe, damit auch interpolierte Verlaufswerte in derselben Stufe bleiben
    return (min(255, (round(c[0] * 31 / 255) << 3) + 4), min(255, (round(c[1] * 63 / 255) << 2) + 2),
            min(255, (round(c[2] * 31 / 255) << 3) + 4))


def cc(c, blend=False):
    """blend=True: Farbe wird nur halbtransparent gemischt (Schatten, Abdunklung) -> unverändert lassen"""
    if not blend:
        c = r565(c)   # auch halbtransparente Farben: LVGL schneidet sonst auf RGB565 ab (Farbstich)
    return f'LV_COLOR_MAKE(0x{c[0]:02x},0x{c[1]:02x},0x{c[2]:02x})'


def jround(v):
    """Rundung wie JavaScript Math.round (x.5 immer aufwärts)"""
    return math.floor(v + 0.5)


def rbox(e):
    x0, y0 = jround(e['x']), jround(e['y'])
    return x0, y0, jround(e['x'] + e['w']) - x0, jround(e['y'] + e['h']) - y0


def parse_shadows(s):
    if not s or s == 'none':
        return []
    parts, depth, cur = [], 0, ''
    for ch in s:
        depth += ch == '('
        depth -= ch == ')'
        if ch == ',' and not depth:
            parts.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    res = []
    for p in parts:
        col = re.search(r'rgba?\([^)]*\)', p).group(0)
        nums = [float(x.replace('px', '')) for x in p.replace(col, '').replace('inset', '').split()]
        nums += [0] * (4 - len(nums))
        res.append(dict(color=rgba(col), dx=nums[0], dy=nums[1], blur=nums[2], spread=nums[3], inset='inset' in p))
    return res


def radius_px(r, w, h):
    v = r.split()[0] if r else '0px'
    rad = min(w, h) * float(v[:-1]) / 100 if v.endswith('%') else float(v[:-2] or 0)
    return int(round(min(rad, w / 2, h / 2)))


def slug(s, n=28):
    s = s.translate(str.maketrans({'ä': 'ae', 'ö': 'oe', 'ü': 'ue', 'ß': 'ss', 'Ä': 'AE', 'Ö': 'OE', 'Ü': 'UE'}))
    s = re.sub(r'[^A-Za-z0-9]+', '_', s).strip('_').upper()
    return (s or 'X')[:n]


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def c_bytes(b):
    return ',\n'.join(','.join(f'0x{x:02x}' for x in b[i:i + 24]) for i in range(0, len(b), 24))


# ---------------------------------------------------------------- Schriften

FONTS = {}
for f in glob.glob(os.path.join(FONTDIR, 'sw_font_*.c')):
    name = os.path.basename(f)[:-2]
    t = open(f).read()
    lh = int(re.search(r'\.line_height = (\d+)', t).group(1))
    bl = int(re.search(r'\.base_line = (-?\d+)', t).group(1))
    m = re.match(r'sw_font_(\d+)_(\d+)(_tnum)?', name)
    FONTS[(int(m.group(1)), int(m.group(2)), bool(m.group(3)))] = (name, lh, bl)


def font_for(fs):
    size, weight, ls, lh, num = fs.split('|')
    sz = int(round(float(size[:-2])))
    key = (sz, int(weight), 'tabular' in num)
    if key not in FONTS:
        raise SystemExit(f'Schrift fehlt: {key} ({fs})')
    name, lineh, base = FONTS[key]
    lsv = 0 if ls == 'normal' else float(ls[:-2]) * 16
    # Chrome: Oberkante = Grundlinie - ASC*size; LVGL: Oberkante = Grundlinie - (line_height - base_line)
    yoff = ASC * float(size[:-2]) - (lineh - base)
    return name, int(round(lsv)), yoff


# ---------------------------------------------------------------- Bausteine (Masken, Icons, Bilder)

ASSETS = {}   # key -> (cname, kind)
asset_c, asset_h = [], []


def collapse(a, axis):
    n = a.shape[axis]
    c = n // 2
    take = lambda i: a.take(i, axis=axis)
    lo = c
    while lo > 0 and np.array_equal(take(lo - 1), take(c)):
        lo -= 1
    hi = c
    while hi < n - 1 and np.array_equal(take(hi + 1), take(c)):
        hi += 1
    return lo, hi


def mask_asset(lkey):
    if lkey in ASSETS:
        return ASSETS[lkey]
    j = LAYERS[lkey]
    a = np.array(Image.open(os.path.join(OUT, 'layers', j['file'])))[:, :, 3]
    # Mittelband (identische Spalten/Zeilen) auf höchstens BAND Pixel kürzen: verlustfrei, und breit genug,
    # damit das Kacheln auf dem Gerät nur wenige Zeichenaufrufe braucht
    BAND = 32
    x0, x1 = collapse(a, 1)
    y0, y1 = collapse(a, 0)
    bw = min(BAND, x1 - x0 + 1)
    bh = min(BAND, y1 - y0 + 1)
    a2 = np.concatenate([a[:, :x0 + bw], a[:, x1 + 1:]], axis=1)
    a2 = np.concatenate([a2[:y0 + bh, :], a2[y1 + 1:, :]], axis=0)
    cname = f'sw_m_{lkey}'
    h, w = a2.shape
    M = -j['ox']
    asset_c.append(f'static const uint8_t {cname}_d[] = {{\n{c_bytes(a2.tobytes())}\n}};\n'
                   f'const sw_mask_t {cname} = {{ {cname}_d, {w}, {h}, {x0}, {bw}, {y0}, {bh}, {j["ox"]}, {j["oy"]}, {2 * M}, {2 * M} }};\n')
    asset_h.append(f'extern const sw_mask_t {cname};')
    ASSETS[lkey] = cname
    return cname


def a8_image(cname, a):
    h, w = a.shape
    asset_c.append(f'static const uint8_t {cname}_d[] = {{\n{c_bytes(a.tobytes())}\n}};\n'
                   f'const lv_image_dsc_t {cname} = {{ .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, '
                   f'.w = {w}, .h = {h}, .stride = {w} }}, .data_size = sizeof({cname}_d), .data = {cname}_d }};\n')
    asset_h.append(f'extern const lv_image_dsc_t {cname};')


def argb_image(cname, im):
    a = np.array(im.convert('RGBA')).astype(np.float64)
    a[:, :, 0] = np.minimum(255, np.round(a[:, :, 0] * 31 / 255) * 8 + 4)
    a[:, :, 1] = np.minimum(255, np.round(a[:, :, 1] * 63 / 255) * 4 + 2)
    a[:, :, 2] = np.minimum(255, np.round(a[:, :, 2] * 31 / 255) * 8 + 4)
    a = a.astype(np.uint8)
    h, w = a.shape[:2]
    bgra = a[:, :, [2, 1, 0, 3]].tobytes()
    asset_c.append(f'static const uint8_t {cname}_d[] = {{\n{c_bytes(bgra)}\n}};\n'
                   f'const lv_image_dsc_t {cname} = {{ .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, '
                   f'.w = {w}, .h = {h}, .stride = {w * 4} }}, .data_size = sizeof({cname}_d), .data = {cname}_d }};\n')
    asset_h.append(f'extern const lv_image_dsc_t {cname};')


def find_layer(**kw):
    for k, j in LAYERS.items():
        if all(j.get(a) == b for a, b in kw.items()):
            return k
    return None


def icon_asset(w, h, sw, paths):
    k = find_layer(kind='icon', w=w, h=h, sw=sw, paths=paths)
    if k in ASSETS:
        return ASSETS[k]
    nm = ICON_NAMES.get(paths[0], 'x') if paths else 'x'
    cname = f'sw_icon_{nm}_{w}_{slug(sw).lower()}'
    a = np.array(Image.open(os.path.join(OUT, 'layers', LAYERS[k]['file'])))[:, :, 3]
    a8_image(cname, a)
    ASSETS[k] = cname
    return cname


def img_asset(e):
    k = find_layer(kind='img', w=e['w'], h=e['h'], src=e['attrs']['src'], filter=e['st']['filter'])
    if k in ASSETS:
        return ASSETS[k], LAYERS[k]['ox']
    cname = f'sw_img_{slug(os.path.basename(e["attrs"]["src"])).lower()}_{int(e["w"])}'
    argb_image(cname, Image.open(os.path.join(OUT, 'layers', LAYERS[k]['file'])))
    ASSETS[k] = cname
    return cname, LAYERS[k]['ox']


# ---------------------------------------------------------------- Stile

STYLES = {}
style_c = []


def style_for(st, w, h):
    """st: dict mit bg, bgImage, shadow, radius, border, color, transform, opacity"""
    rad = radius_px(st.get('radius', '0px'), w, h)
    bg = rgba(st.get('bg'))
    gi = st.get('bgImage') or 'none'
    kind, c1, c2, angle = 'SW_BG_NONE', (0, 0, 0, 0), (0, 0, 0, 0), 180
    m = re.match(r'linear-gradient\((?:(-?[\d.]+)deg, )?(rgba?\([^)]*\)), (rgba?\([^)]*\))\)', gi)
    if m:
        kind, angle, c1, c2 = 'SW_BG_LINEAR', int(float(m.group(1) or 180)), rgba(m.group(2)), rgba(m.group(3))
    elif bg[3]:
        kind, c1, c2 = 'SW_BG_COLOR', bg, bg
    bw, bc, bbot = 0, (0, 0, 0, 0), 0
    bm = re.match(r'([\d.]+)px \w+ (rgba?\([^)]*\))', st.get('border') or '')
    if bm and float(bm.group(1)) > 0 and rgba(bm.group(2))[3]:
        bw, bc = int(round(float(bm.group(1)))), rgba(bm.group(2))
    bb = re.match(r'([\d.]+)px \w+ (rgba?\([^)]*\))', st.get('borderBottom') or '')
    if not bw and bb and float(bb.group(1)) > 0 and rgba(bb.group(2))[3]:
        bw, bc, bbot = int(round(float(bb.group(1)))), rgba(bb.group(2)), 1
    shadows = []
    for sh in parse_shadows(st.get('shadow')):
        M = 0 if sh['inset'] else math.ceil(max(abs(sh['dx']), abs(sh['dy'])) + sh['blur'] + max(0, sh['spread'])) + 2
        lk = find_layer(kind='shadow', w=w, h=h, radius=st.get('radius'), dx=sh['dx'], dy=sh['dy'],
                        blur=sh['blur'], spread=sh['spread'], inset=sh['inset'], M=M)
        if not lk:
            print('  Maske fehlt', w, h, st.get('radius'), sh, file=sys.stderr)
            continue
        shadows.append((mask_asset(lk), sh['color'], sh['inset']))
    tc = rgba(st.get('color'))
    ty = 0
    tm = re.match(r'matrix\(1, 0, 0, 1, ([-\d.]+), ([-\d.]+)\)', st.get('transform') or '')
    if tm:
        ty = int(round(float(tm.group(2))))
    op = int(round(float(st.get('opacity') or 1) * 255))
    sig = json.dumps([kind, c1, c2, angle, rad, bw, bbot, bc, shadows, tc, ty, op])
    if sig in STYLES:
        return STYLES[sig]
    n = f'sw_st_{len(STYLES)}'
    sh_name = 'NULL'
    if shadows:
        sh_name = f'{n}_sh'
        style_c.append(f'static const sw_shadow_t {sh_name}[] = {{ ' + ', '.join(
            f'{{ &{m_}, {cc(c)}, {c[3]}, {int(i)} }}' for m_, c, i in shadows) + ' };')
    # Verläufe werden mit Dithering gezeichnet -> Originalfarben (nicht auf RGB565-Stufen gesetzt)
    # halbtransparente Flächen (Abdunklung des Dialogs) ebenfalls unverändert: dort ist das genauer
    raw = kind == 'SW_BG_LINEAR' or c1[3] < 255
    gc1, gc2 = (cc(c1, True), cc(c2, True)) if raw else (cc(c1), cc(c2))
    style_c.append(f'static const sw_style_t {n} = {{ {kind}, {gc1}, {gc2}, {c1[3]}, {c2[3]}, {angle}, {rad}, '
                   f'{bw}, {bbot}, {cc(bc)}, {bc[3]}, {len(shadows)}, {sh_name}, {cc(tc)}, {ty}, {op} }};')
    STYLES[sig] = n
    return n


def has_box(st):
    if rgba(st['bg'])[3] or st['bgImage'] != 'none' or st['shadow'] != 'none':
        return True
    for b in (st['border'], st['borderBottom']):
        m = re.match(r'([\d.]+)px \w+ (rgba?\([^)]*\))', b or '')
        if m and float(m.group(1)) > 0 and rgba(m.group(2))[3]:
            return True
    return False


# ---------------------------------------------------------------- Bildschirme

screen_c, screen_h, screen_list = [], [], []


def gen_screen(name):
    d = json.load(open(os.path.join(OUT, name + '.json')))
    els, lines = d['els'], d['lines']
    by_i = {e['i']: e for e in els}
    root_i = next(e['i'] for e in els if e['tag'] == 'div' and e['attrs'].get('style') is None and e['cls'] == '' and e['parent'] == -1)

    def hidden(e):
        while e:
            if e['st']['display'] == 'none' or e['st']['visibility'] == 'hidden':
                return True
            e = by_i.get(e['parent'])
        return False

    def is_container(e):
        c = e['cls'].split()
        return bool(e['action'] or e['tag'] == 'button' or (e.get('scrollBox') and e['scrollBox']['sh'] > e['scrollBox']['ch'] + 1)
                    or float(e['st']['opacity']) < 1 or 'modal-shade' in c or 'modal-box' in c or 'sleep' in c or e['attrs'].get('style') is None and e['tag'] == 'div' and e['parent'] == -1 and False
                    or e.get('id') in ('toast', 'modal-root'))

    nodes, node_of, names = [], {}, {}
    keys_used = {}

    def keyname(e, prefix):
        parts = []
        cur = e
        while cur is not None and cur['i'] != root_i:
            c = cur['cls'].split()
            if c and c[0] not in ('raised', 'btn', 'sunken'):
                parts.append(c[0])
                break
            parts.append(cur['tag'] if not c else cur['tag'] + '.' + c[0])
            cur = by_i.get(cur['parent'])
        k = prefix + '/'.join(reversed(parts))
        n = keys_used.get(k, 0) + 1
        keys_used[k] = n
        return k if n == 1 else f'{k}#{n}'

    def cont_parent(e):
        p = by_i.get(e['parent'])
        while p is not None:
            if p['i'] in node_of and nodes[node_of[p['i']]]['container']:
                return p
            p = by_i.get(p['parent'])
        return None

    def rel(x, y, parent_el):
        if parent_el is None:
            return x, y
        px, py, _, _ = rbox(parent_el)
        return x - px, y - py

    def add(n, nm=None):
        nodes.append(n)
        if nm:
            base = f'SW_{slug(name)}_{nm}'
            k, i = base, 2
            while k in names:
                k, i = f'{base}_{i}', i + 1
            names[k] = len(nodes) - 1
        return len(nodes) - 1

    for e in els:
        if e['i'] == root_i or hidden(e):
            continue
        x, y, w, h = rbox(e)
        if w <= 0 or h <= 0:
            continue
        pe = cont_parent(e)
        pidx = node_of[pe['i']] if pe else -1
        cont = is_container(e)
        if e['tag'] in ('path',):
            continue
        if e['tag'] == 'svg':
            ic = icon_asset(w, h, e['st']['strokeWidth'], [p['attrs']['d'] for p in els if p['parent'] == e['i'] and p['tag'] == 'path'])
            col = rgba(e['st']['stroke'] if e['st']['stroke'].startswith('rgb') else e['st']['color'])
            rx, ry = rel(x, y, pe)
            owner = by_i.get(e['parent'])
            inh = int(pe is not None and rgba(pe['st']['color'])[:3] == col[:3])
            nm = ICON_NAMES.get((els[e['i'] + 1]['attrs'] or {}).get('d', ''), 'ICON') if e['i'] + 1 < len(els) else 'ICON'
            add(dict(kind='SW_N_ICON', parent=pidx, x=rx, y=ry, w=w, h=h, color=col, img=ic, inherit=inh,
                     key=keyname(by_i[e['parent']], 'icon:')), f'I_{slug(nm)}')
            continue
        if e['tag'] == 'img':
            im, ox = img_asset(e)
            rx, ry = rel(jround(e['x']) + ox, jround(e['y']) + ox, pe)
            add(dict(kind='SW_N_IMG', parent=pidx, x=rx, y=ry, w=w, h=h, img=im, key=keyname(e, 'img:')), 'IMG')
            continue
        if e['tag'] == 'option':
            continue
        if e['tag'] == 'input' and e['attrs'].get('type') == 'range':
            rx, ry = rel(x, y, pe)
            parts = {}
            for v in ('min', 'max', 'thumb'):
                lk = find_layer(kind='range', w=w, h=h, v=v)
                parts[v] = np.array(Image.open(os.path.join(OUT, 'layers', LAYERS[lk]['file'])).convert('RGBA'))
            alpha = sum(p_[:, :, 3].astype(int) for p_ in parts.values())
            rows = np.where(alpha.max(1) > 0)[0]
            r0, r1 = int(rows[0]), int(rows[-1]) + 1
            cx = 8 + (w - 16) * 0.5
            names_ = {}
            for v in ('min', 'max'):
                cn = f'sw_range_{v}_{w}'
                if cn not in ASSETS:
                    argb_image(cn, Image.fromarray(parts[v][r0:r1]))
                    ASSETS[cn] = cn
                names_[v] = cn
            cn = f'sw_range_thumb_{w}'
            tx0 = int(math.floor(cx - 9))
            if cn not in ASSETS:
                argb_image(cn, Image.fromarray(parts['thumb'][r0:r1, tx0:tx0 + 18]))
                ASSETS[cn] = cn
            names_['thumb'] = cn
            a = e['attrs']
            add(dict(kind='SW_N_RANGE', parent=pidx, x=rx, y=ry, w=w, h=h, img=names_['min'], img2=names_['max'],
                     img3=names_['thumb'], img_y=r0, vmin=int(a['min'] or 0), vmax=int(a['max'] or 100),
                     vstep=int(a['step'] or 1), value=int(float(a['value'])), field=e['field'],
                     key='field:' + (e['field'] or 'range')), 'F_' + slug(e['field'] or 'range'))
            continue
        if e['tag'] == 'input' and e['attrs'].get('type') == 'color':
            rx, ry = rel(x, y, pe)
            lk = find_layer(kind='color', w=w, h=h, value=e['attrs']['value'])
            cn = f'sw_color_{lk}'
            if cn not in ASSETS:
                argb_image(cn, Image.open(os.path.join(OUT, 'layers', LAYERS[lk]['file'])))
                ASSETS[cn] = cn
            add(dict(kind='SW_N_IMG', parent=pidx, x=rx, y=ry, w=w, h=h, img=cn, field=e['field'], key='field:' + (e['field'] or 'color')))
            continue
        if e['tag'] in ('input', 'select'):
            # Feld: eingelassene Fläche + Wert/Platzhalter (+ Pfeil der Auswahlliste)
            rx, ry = rel(x, y, pe)
            a = e['attrs']
            sel = e['tag'] == 'select'
            n = dict(kind='SW_N_BOX', parent=pidx, x=rx, y=ry, w=w, h=h, container=1, st=style_for(dict(e['st']), w, h),
                     action=('select:' + (e['field'] or '')) if sel else None, field=e['field'],
                     key='field:' + (e['field'] or e['tag']))
            if sel:
                n['options'] = '\n'.join(a['options'] or [])
                n['sel_index'] = a['selectedIndex'] or 0
            bi = add(n, 'F_' + slug(e['field'] or e['tag']))
            val = a['value'] if not sel else (a['options'] or [''])[a['selectedIndex'] or 0]
            ph = not val and a.get('placeholder')
            txt = ('•' * len(val) if a.get('type') == 'password' else val) if val else (a.get('placeholder') or '')
            if txt:
                fname, ls, yoff = font_for(e['st']['font'])
                col = rgba(a['phColor'] if ph else e['st']['color'])
                fsz = float(e['st']['font'].split('|')[0][:-2])
                top = (h - ASC * fsz - 600 / 2000 * fsz) / 2 + yoff
                # Chrome: Text im select 4 px weiter innen als im input (gemessen)
                add(dict(kind='SW_N_LABEL', parent=bi, x=jround((22 if sel else 18) * 16), y=jround(top), w=0, h=0, font=fname, color=col,
                         opa=col[3], ls=ls, key='text:field:' + (e['field'] or e['tag']), text=txt, align='SW_AL_LEFT'))
            if sel:
                lk = find_layer(kind='arrow', w=w, h=h, radius=e['st']['radius'])
                ar = np.array(Image.open(os.path.join(OUT, 'layers', LAYERS[lk]['file'])))[:, :, 3]
                ys, xs = np.where(ar > 0)
                cn = f'sw_arrow_{w}_{h}'
                if cn not in ASSETS:
                    a8_image(cn, ar[ys.min():ys.max() + 1, xs.min():xs.max() + 1])
                    ASSETS[cn] = cn
                add(dict(kind='SW_N_ICON', parent=bi, x=int(xs.min()), y=int(ys.min()), w=0, h=0, color=(51, 51, 51, 255),
                         img=cn, key='icon:field:' + (e['field'] or 'select')))
            node_of[e['i']] = bi
            continue
        box = has_box(e['st'])
        sw = 'switch' in e['cls'].split()
        if not (box or cont):
            continue
        rx, ry = rel(x, y, pe)
        st_dict = dict(e['st'])
        opv = float(e['st']['opacity'])
        n = dict(kind='SW_N_SWITCH' if sw else 'SW_N_BOX', parent=pidx, hidden=int(opv == 0), opa=int(round(opv * 255)), x=rx, y=ry, w=w, h=h, container=int(cont),
                 st=style_for(st_dict, w, h) if box else None, action=e['action'], disabled=int(bool(e['attrs'].get('disabled'))),
                 selected=int('selected' in e['cls'].split() or e['attrs'].get('checked') == 'true'))
        v = e.get('variants') or {}
        if box and v.get('pressed'):
            n['st_pressed'] = style_for({**st_dict, **v['pressed']}, w, h)
        sel = v.get('selected') or v.get('unselected') or v.get('toggled')
        if box and sel:
            n['st_sel'] = style_for({**st_dict, **sel}, w, h)
            sp = v.get('selectedPressed') or v.get('unselectedPressed')
            if sp:
                n['st_sel_pressed'] = style_for({**st_dict, **sp}, w, h)
            if n['selected']:   # Messung war "selected": Normal/Sel tauschen
                n['st'], n['st_sel'] = n['st_sel'], n['st']
                if n.get('st_pressed') and n.get('st_sel_pressed'):
                    n['st_pressed'], n['st_sel_pressed'] = n['st_sel_pressed'], n['st_pressed']
        sc = e.get('scrollBox')
        if sc and sc['sh'] > sc['ch'] + 1:
            n['scroll'], n['pad_bottom'] = 1, int(round(sc['pad'][2]))
        n['key'] = e['action'] or keyname(e, 'box:')
        cls = e['cls'].split()
        node_of[e['i']] = add(n, ('A_' + slug(e['action'])) if e['action'] else ('E_' + slug(cls[0])) if cls else None)
        # Pseudo-Trenner (.home-temps button+button:before): 1px Linie + 1px weißer Versatz
        if e.get('before') and e['before']['display'] != 'none' and e['before']['w'] == '1px':
            b = e['before']
            add(dict(kind='SW_N_DIVIDER', parent=node_of[e['i']], x=int(float(b['left'][:-2])), y=int(float(b['top'][:-2])),
                     w=1, h=int(float(b['h'][:-2])), color=rgba(b['bg'])))

    for l in lines:
        e = by_i[l['el']]
        if hidden(e):
            continue
        pe = e if (e['i'] in node_of and nodes[node_of[e['i']]].get('container')) else cont_parent(e)
        pidx = node_of[pe['i']] if pe else -1
        if any(ch in '✓⛶' for ch in l['text']):
            fs_ = l['font'].split('|')
            lk = find_layer(kind='text', text=l['text'], size=fs_[0], weight=fs_[1], ls=fs_[2])
            if lk not in ASSETS:
                a8_image(f'sw_txt_{lk}', np.array(Image.open(os.path.join(OUT, 'layers', LAYERS[lk]['file'])))[:, :, 3])
                ASSETS[lk] = f'sw_txt_{lk}'
            col = rgba(l['color'])
            px0, py0 = (rbox(pe)[0], rbox(pe)[1]) if pe else (0, 0)
            add(dict(kind='SW_N_ICON', parent=pidx, x=jround(l['x']) - px0, y=jround(l['y']) - py0, w=0, h=0, color=col,
                     img=ASSETS[lk], inherit=int(pe is not None and rgba(pe['st']['color'])[:3] == col[:3])), 'T_CHECK')
            continue
        fname, ls, yoff = font_for(l['font'])
        col = rgba(l['color'])
        al = l.get('align', 'left')
        anchor = l['x'] + (l['w'] / 2 if al == 'center' else l['w'] if al == 'right' else 0)
        px0, py0 = (rbox(pe)[0], rbox(pe)[1]) if pe else (0, 0)
        rx, ry, W = jround((anchor - px0) * 16), jround(l['y'] + yoff - py0), 0
        inh = int(pe is not None and rgba(pe['st']['color'])[:3] == col[:3])
        add(dict(kind='SW_N_LABEL', parent=pidx, x=rx, y=ry, w=W, h=0, font=fname, color=col, opa=col[3], ls=ls, key=keyname(e, 'text:'),
                 align={'left': 'SW_AL_LEFT', 'center': 'SW_AL_CENTER', 'right': 'SW_AL_RIGHT'}[al], text=l['text'], inherit=inh),
            'T_' + slug(l['text'], 20))

    rows = []
    for n in nodes:
        def st(k):
            return f'&{n[k]}' if n.get(k) else 'NULL'
        rows.append('    { ' + ', '.join([
            f'.kind = {n["kind"]}', f'.container = {n.get("container", 0)}', f'.scroll = {n.get("scroll", 0)}',
            f'.inherit_color = {n.get("inherit", 0)}', f'.parent = {n["parent"]}',
            f'.x = {n["x"]}', f'.y = {n["y"]}', f'.w = {n["w"]}', f'.h = {n["h"]}', f'.pad_bottom = {n.get("pad_bottom", 0)}',
            f'.st = {st("st")}', f'.st_pressed = {st("st_pressed")}', f'.st_sel = {st("st_sel")}', f'.st_sel_pressed = {st("st_sel_pressed")}',
            f'.selected = {n.get("selected", 0)}', f'.disabled = {n.get("disabled", 0)}',
            f'.action = {cstr(n["action"]) if n.get("action") else "NULL"}',
            f'.font = {("&" + n["font"]) if n.get("font") else "NULL"}',
            f'.color = {cc(n["color"]) if n.get("color") else "LV_COLOR_MAKE(0,0,0)"}',
            f'.opa = {n.get("opa", 255)}', f'.ls_q4 = {n.get("ls", 0)}', f'.hidden = {n.get("hidden", 0)}', f'.align = {n.get("align", "SW_AL_LEFT")}',
            f'.text = {cstr(n["text"]) if n.get("text") else "NULL"}',
            f'.img = {("&" + n["img"]) if n.get("img") else "NULL"}',
            f'.key = {cstr(n["key"]) if n.get("key") else "NULL"}',
            f'.field = {cstr(n["field"]) if n.get("field") else "NULL"}',
            f'.img2 = {("&" + n["img2"]) if n.get("img2") else "NULL"}',
            f'.img3 = {("&" + n["img3"]) if n.get("img3") else "NULL"}',
            f'.img_y = {n.get("img_y", 0)}', f'.vmin = {n.get("vmin", 0)}', f'.vmax = {n.get("vmax", 0)}',
            f'.vstep = {n.get("vstep", 0)}', f'.value = {n.get("value", 0)}',
            f'.options = {cstr(n["options"]) if n.get("options") else "NULL"}', f'.sel_index = {n.get("sel_index", 0)}',
        ]) + ' },')
    cn = f'sw_scr_{name}'
    screen_c.append(f'static const sw_node_t {cn}_nodes[] = {{\n' + '\n'.join(rows) + '\n};\n'
                    f'const sw_screen_def_t {cn} = {{ "{name}", {cn}_nodes, {len(nodes)} }};\n')
    screen_h.append(f'extern const sw_screen_def_t {cn};')
    for k, v in names.items():
        screen_h.append(f'#define {k} {v}')
    screen_list.append(cn)
    print(f'{name:18} Knoten {len(nodes)}')


names = sys.argv[1:] or sorted(os.path.basename(p)[:-5] for p in glob.glob(os.path.join(OUT, '*.json')) if not p.endswith('layers.json'))
for nm in names:
    gen_screen(nm)

# Schalter-Knopf-Schatten für sw_render.c
_k = find_layer(kind='shadow', w=40, h=40, radius='50%', dx=3, dy=3, blur=5, spread=0, inset=False, M=10)
asset_c.append('const sw_mask_t *sw_switch_knob_mask(void) { return ' + (f'&{mask_asset(_k)}' if _k else 'NULL') + '; }')

fonts_used = sorted({v[0] for v in FONTS.values()})
hdr = '/* Erzeugt von tools/design/gen.py – nicht von Hand ändern. */\n#pragma once\n#include "../sw_types.h"\n'
with open(os.path.join(GEN, 'sw_gen_assets.h'), 'w') as f:
    f.write(hdr + '\n'.join(asset_h) + '\n' + '\n'.join(f'LV_FONT_DECLARE({x})' for x in fonts_used) + '\n')
with open(os.path.join(GEN, 'sw_gen_assets.c'), 'w') as f:
    f.write('/* Erzeugt von tools/design/gen.py – nicht von Hand ändern. */\n#include "sw_gen_assets.h"\n\n' + '\n'.join(asset_c))
with open(os.path.join(GEN, 'sw_gen_screens.h'), 'w') as f:
    f.write(hdr + '#include "sw_gen_assets.h"\n' + '\n'.join(screen_h) + '\n'
            + 'extern const sw_screen_def_t *const sw_all_screens[];\nextern const int sw_all_screens_count;\n')
with open(os.path.join(GEN, 'sw_gen_screens.c'), 'w') as f:
    f.write('/* Erzeugt von tools/design/gen.py – nicht von Hand ändern. */\n#include "sw_gen_screens.h"\n\n'
            + '\n'.join(style_c) + '\n\n' + '\n'.join(screen_c)
            + '\nconst sw_screen_def_t *const sw_all_screens[] = { ' + ', '.join('&' + s for s in screen_list) + ' };\n'
            + f'const int sw_all_screens_count = {len(screen_list)};\n')
print('Stile', len(STYLES), 'Bausteine', len(ASSETS))
