"""Rasterize the Ukraine oblast map (same SVG paths as the Vitalnya TV launcher)
into data for the firmware:
  kUaMap      — per-pixel region index (0 = background, 1..N = region, 255 = border)
  kUaLabelA   — anti-aliased alpha of city labels (white text)
  kUaLabelO   — alpha of the dark outline around the labels
Usage: make_map.py ua.json OUTDIR [font.otf]"""
import json, re, sys
from PIL import Image, ImageDraw, ImageChops, ImageFilter, ImageFont

W, H = 296, 180            # map area: screen 296x240, header 34 px, footer 26 px
HEADER, FOOTER = 34, 26
VB_W, VB_H = 508, 340
S = 4                      # supersampling for clean region edges

d = json.load(open(sys.argv[1]))
outdir = sys.argv[2]
font_path = sys.argv[3] if len(sys.argv) > 3 else "/usr/share/fonts/opentype/inter/Inter-SemiBold.otf"
regions = [r[0] for r in d["REGIONS"]]
scale = min((W - 4) / VB_W, (H - 2) / VB_H)
ox = (W - VB_W * scale) / 2
oy = (H - VB_H * scale) / 2
def tx(x, y): return (x * scale + ox, y * scale + oy)

def subpaths(path):
    out, cur = [], []
    for cmd, args in re.findall(r"([MLZ])([^MLZ]*)", path):
        nums = [float(v) for v in re.findall(r"-?\d+(?:\.\d+)?", args)]
        pts = list(zip(nums[0::2], nums[1::2]))
        if cmd == "M":
            if len(cur) > 2: out.append(cur)
            cur = list(pts)
        elif cmd == "L":
            cur += pts
        elif cmd == "Z":
            if len(cur) > 2: out.append(cur)
            cur = []
    if len(cur) > 2: out.append(cur)
    return out

# ---------- regions ----------
idx = Image.new("L", (W * S, H * S), 0)
for i, rid in enumerate(regions, start=1):
    mask = Image.new("1", idx.size, 0)
    for sp in subpaths(d["UA_PATHS"][rid]):
        m = Image.new("1", idx.size, 0)
        ImageDraw.Draw(m).polygon([(a * S, b * S) for a, b in (tx(x, y) for x, y in sp)], fill=1)
        mask = ImageChops.logical_xor(mask, m)   # even-odd fill
    idx.paste(i, mask=mask)
small = idx.resize((W, H), Image.NEAREST)
px = small.load()
out = bytearray(W * H)
for y in range(H):
    for x in range(W):
        v = px[x, y]
        border = False
        if v:
            for dx, dy in ((1, 0), (0, 1), (-1, 0), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H and px[nx, ny] != v and (px[nx, ny] != 0 or dx > 0 or dy > 0):
                    border = True
        out[y * W + x] = 255 if border else v

# ---------- city labels (обласні центри) ----------
CITY = {
    "vol": "Луцьк", "riv": "Рівне", "zht": "Житомир", "chn": "Чернігів", "sum": "Суми",
    "lvi": "Львів", "ter": "Тернопіль", "khm": "Хмельн.", "vin": "Вінниця", "chk": "Черкаси",
    "pol": "Полтава", "khr": "Харків", "lug": "Луганськ", "zak": "Ужгород", "ifr": "Франків.",
    "chv": "Чернівці", "kir": "Кропивн.", "dnp": "Дніпро", "don": "Донецьк", "ode": "Одеса",
    "myk": "Миколаїв", "zap": "Запоріжжя", "khe": "Херсон", "crimea": "Крим",
}
# невеликі зсуви, щоб підписи не налазили один на одного (у пікселях екрана)
NUDGE = {"vol": (-4, -4), "riv": (-4, 8), "lvi": (-6, -6), "ter": (-4, -2), "khm": (21, 3),
         "ifr": (4, -2), "zak": (2, 7), "chv": (8, 5), "vin": (2, 3), "zht": (-3, -3),
         "chn": (8, 3), "sum": (12, -1), "lug": (-6, 0), "kir": (0, 2), "myk": (-2, -2), "dnp": (-5, 0), "don": (2, 3)}
LS = 4
txt = Image.new("L", (W * LS, H * LS), 0)
big = ImageFont.truetype(font_path, 8 * LS)
dr = ImageDraw.Draw(txt)
for rid, name in CITY.items():
    cx, cy = tx(*d["UA_CENT"][rid])
    nx, ny = NUDGE.get(rid, (0, 0))
    dr.text(((cx + nx) * LS, (cy + ny) * LS), name, font=big, fill=255, anchor="mm")
kx, ky = tx(*d["KYIV_XY"])
dr.text(((kx + 1) * LS, (ky - 8) * LS), "Київ", font=ImageFont.truetype(font_path.replace("SemiBold", "Bold"), 9 * LS), fill=255, anchor="mm")
label = txt.resize((W, H), Image.LANCZOS)
outline = label.point(lambda v: 255 if v > 40 else 0).filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(0.4))
la, lo = label.tobytes(), outline.tobytes()

kyiv = (round(kx), round(ky))

# ---------- full-screen preview (як на пристрої) ----------
alert = {"khr", "sum", "don", "kyiv", "chn", "pol", "dnp", "zap", "lug", "khe", "myk", "crimea"}
scr = Image.new("RGB", (W, HEADER + H + FOOTER), (15, 17, 21))
pp = scr.load()
for y in range(H):
    for x in range(W):
        v = out[y * W + x]
        c = (15, 17, 21) if v == 0 else (15, 17, 21) if v == 255 else ((255, 90, 78) if regions[v - 1] == "kyiv" else (229, 56, 59) if regions[v - 1] in alert else (58, 63, 75))
        a_o, a_t = lo[y * W + x] / 255 * 0.85, la[y * W + x] / 255
        c = tuple(int(c[i] * (1 - a_o) + 10 * a_o) for i in range(3))
        c = tuple(int(c[i] * (1 - a_t) + 255 * a_t) for i in range(3))
        pp[x, y + HEADER] = c
dd = ImageDraw.Draw(scr)
dd.rectangle([0, 0, W, HEADER - 1], fill=(198, 40, 40))
dd.text((W / 2, HEADER / 2), "ПОВІТРЯНА ТРИВОГА", font=ImageFont.truetype(font_path.replace("SemiBold", "Bold"), 18), fill="white", anchor="mm")
dd.ellipse([kyiv[0] - 4, kyiv[1] + HEADER - 4, kyiv[0] + 4, kyiv[1] + HEADER + 4], fill=(255, 214, 10), outline=(15, 17, 21), width=2)
f15 = ImageFont.truetype(font_path.replace("SemiBold", "Medium"), 13)
dd.text((16, HEADER + H + FOOTER / 2), "Київ · 12 хв", font=f15, fill=(232, 234, 237), anchor="lm")
dd.text((W - 16, HEADER + H + FOOTER / 2), "12 обл. · 09:34", font=f15, fill=(154, 160, 166), anchor="rm")
scr.resize((W * 3, (HEADER + H + FOOTER) * 3), Image.NEAREST).save(outdir + "/map_preview.png")

# ---------- C header ----------
cent = {k: tuple(round(c) for c in tx(*v)) for k, v in d["UA_CENT"].items()}
def arr(name, data):
    s = [f"static const uint8_t {name}[UA_MAP_W * UA_MAP_H] = {{"]
    for i in range(0, len(data), 48):
        s.append("  " + ",".join(str(b) for b in data[i:i + 48]) + ",")
    s.append("};\n")
    return "\n".join(s)
with open(outdir + "/ua_map.h", "w") as f:
    f.write("// generated by tools/make_map.py — Ukraine oblasts + city labels\n#pragma once\n#include <cstdint>\n\n")
    f.write(f"#define UA_MAP_W {W}\n#define UA_MAP_H {H}\n#define UA_MAP_Y {HEADER}\n#define UA_FOOTER_H {FOOTER}\n#define UA_REGION_COUNT {len(regions)}\n")
    f.write(f"#define UA_KYIV_X {kyiv[0]}\n#define UA_KYIV_Y {kyiv[1]}\n\n")
    f.write("static const char* const kUaRegionIds[UA_REGION_COUNT] = {" + ",".join(f'"{r}"' for r in regions) + "};\n\n")
    f.write(arr("kUaMap", out))
    f.write(arr("kUaLabelA", la))
    f.write(arr("kUaLabelO", lo))
print("ok", W, H, "kyiv", kyiv)
