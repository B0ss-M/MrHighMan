"""Omni Sampler's rack-mount look (1.8.13): a 19-inch hardware faceplate after the user's reference picture and design
guide (2026-11-02): matte black rack ears with oval holes and silver Phillips screws, a brushed gunmetal plate, the
browser as a recessed navy glass screen with a diagonal glare, raised dark plastic keys, machined DATA dial, glowing
LEDs, a bottom I/O strip; light from the top left (shadows fall down and right).

Drawn as SVG and rendered by FigCraft (_toolcache/figcraft, `figcraft-cli convert` + `export --scale 2`), so the art
is vector-sharp; design.py calls it for the pages that use the rack look (BROWSE first). Sizes are plugin pixels
(1280 x 628). Needs the FIGCRAFT binary only when the art is regenerated; builds use the PNGs in art/.
"""
import os
import subprocess
import tempfile

from PIL import Image

FIGCRAFT = os.environ.get("FIGCRAFT", "/workspaces/MPC_Mod/_toolcache/figcraft/target/release/figcraft-cli")
SS = 2
W, H = 1280, 628
EAR = 0                                    # 1.8.13: no rack ears (the user: use the full width); the plate runs edge to edge

INK, INK_DIM, CYAN, EDGE = (222, 230, 238), (150, 162, 176), (88, 222, 246), (62, 71, 84)
NAVY = (10, 20, 38)
LED = {"green": (90, 255, 140), "amber": (255, 190, 70), "cyan": CYAN, "red": (255, 90, 90)}
# 1.8.13: the theme. "light" (default, the user 2026-11-04: "look like an actual hardware sampler"): a light silver
# plate and modules, dark printed lettering, white knobs; screens and readout wells stay dark backlit glass.
# OMNI_THEME=dark: the gunmetal look.
THEME = os.environ.get("OMNI_THEME", "light")
# OMNI_THEME=studio (1.8.14, the user 2026-11-05: "look like a real hardware VST instrument", chose "Black studio"):
# black anodised aluminium with a real brushed grain, dark sub-panels with a thin accent stripe in the section's colour,
# machined aluminium knobs on knurled black skirts with LED rings, graphite keys with a small LED, white silkscreen
STUDIO = THEME == "studio"
LIGHT = THEME not in ("dark", "studio")
PRINT, PRINT_DIM = ((38, 44, 54), (78, 86, 98)) if LIGHT else (INK, INK_DIM)   # lettering on the plate / modules


def hexc(c):
    return "#%02x%02x%02x" % tuple(c[:3])


class Svg:
    def __init__(self, w, h):
        self.w, self.h, self.defs, self.body, self.n = w, h, [], [], 0

    def uid(self, s):
        self.n += 1
        return "%s%d" % (s, self.n)

    def _stops(self, stops):
        return "".join('<stop offset="%g" stop-color="%s" stop-opacity="%g"/>' % (o, hexc(c), a) for o, c, a in stops)

    def lin(self, stops, x1=0, y1=0, x2=0, y2=1):
        i = self.uid("g")
        self.defs.append('<linearGradient id="%s" x1="%g" y1="%g" x2="%g" y2="%g">%s</linearGradient>' % (i, x1, y1, x2, y2, self._stops(stops)))
        return "url(#%s)" % i

    def rad(self, stops, cx=0.5, cy=0.5, r=0.5):
        i = self.uid("r")
        self.defs.append('<radialGradient id="%s" cx="%g" cy="%g" r="%g">%s</radialGradient>' % (i, cx, cy, r, self._stops(stops)))
        return "url(#%s)" % i

    def blur(self, sd):
        i = self.uid("b")
        self.defs.append('<filter id="%s" x="-50%%" y="-50%%" width="200%%" height="200%%"><feGaussianBlur stdDeviation="%g"/></filter>' % (i, sd))
        return "url(#%s)" % i

    def add(self, s):
        self.body.append(s)

    def rect(self, x, y, w, h, r=0, fill="none", stroke=None, sw=1, extra=""):
        st = ' stroke="%s" stroke-width="%g"' % (stroke, sw) if stroke else ""
        self.add('<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="%s"%s %s/>' % (x, y, w, h, r, fill, st, extra))

    def circle(self, cx, cy, r, fill="none", stroke=None, sw=1, extra=""):
        st = ' stroke="%s" stroke-width="%g"' % (stroke, sw) if stroke else ""
        self.add('<circle cx="%g" cy="%g" r="%g" fill="%s"%s %s/>' % (cx, cy, r, fill, st, extra))

    def text(self, x, y, s, size, fill, anchor="middle", weight=600, spacing=0, family="Titillium Web", extra=""):
        s = s.replace("&", "&amp;").replace("<", "&lt;")
        self.add('<text x="%g" y="%g" font-family="%s" font-weight="%d" font-size="%g" fill="%s" text-anchor="%s" '
                 'letter-spacing="%g" %s>%s</text>' % (x, y + size * 0.35, family, weight, size, fill, anchor, spacing, extra, s))

    def doc(self):
        return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d"><defs>%s</defs>%s</svg>'
                % (self.w, self.h, self.w, self.h, "".join(self.defs), "".join(self.body)))


_HERE = os.path.dirname(os.path.abspath(__file__))
# 2.4.9: FigCraft lays text out with the INSTALLED fonts and silently falls back when one is missing. After the container
# rebuild (2026-11-14) Michroma and Titillium Web were gone, so the 2.4.7/2.4.8 art had a narrow fallback "SAMPLER" and a
# wide gap before the logo's "2" (the user: "the 2 is spaced out"). Install them per user before the first render, and
# stop if fontconfig still cannot see them.
_ART_FONTS = {"Michroma": [os.path.join(_HERE, "fonts", "Michroma-Regular.ttf")],
              "Titillium Web": [os.path.join(os.environ.get("MPC_VST") or os.path.join(os.path.dirname(_HERE), "mpc-vst-plugins"),
                                             "tools", "html_art", "fonts", "TitilliumWeb-%s.ttf" % w)
                                for w in ("Regular", "SemiBold", "Bold")]}
_fonts_ok = False


def _font_families():
    out = subprocess.run(["fc-list", ":", "family"], check=True, capture_output=True, text=True).stdout
    return {f.strip() for line in out.splitlines() for f in line.split(",")}


def ensure_fonts():
    global _fonts_ok
    if _fonts_ok:
        return
    missing = [fam for fam in _ART_FONTS if fam not in _font_families()]
    if missing:
        import shutil
        dst = os.path.expanduser("~/.local/share/fonts")
        os.makedirs(dst, exist_ok=True)
        for fam in missing:
            for p in _ART_FONTS[fam]:
                shutil.copy(p, dst)
        subprocess.run(["fc-cache", "-f", dst], check=True, stdout=subprocess.DEVNULL)
        still = [fam for fam in missing if fam not in _font_families()]
        if still:
            raise SystemExit("rack.py: fonts not visible to fontconfig after install: %s" % ", ".join(still))
    _fonts_ok = True


def render(svg):
    """the SVG through FigCraft at 2x, then down to its size"""
    ensure_fonts()   # 2.4.9
    with tempfile.TemporaryDirectory() as tmp:
        src, doc, png = (os.path.join(tmp, n) for n in ("a.svg", "a.figcraft", "a.png"))
        open(src, "w").write(svg.doc())
        for args in (["convert", src, doc], ["export", doc, "-o", png, "--scale", str(SS)]):
            subprocess.run([FIGCRAFT] + args, check=True, stdout=subprocess.DEVNULL)
        im = Image.open(png).convert("RGBA")
        im.load()
    return im.resize((svg.w, svg.h), Image.LANCZOS)


_GRAIN = {}


def grain(s, x, y, w, h, opacity=0.22, seed=1, r=0):
    """brushed-metal grain (studio): random noise stretched along x into fine streaks, light and dark, as an embedded
    PNG at render scale (light streaks and dark ones in one RGBA image; its alpha carries the strength)"""
    import base64, io, random
    gw, gh = max(1, int(w * SS)), max(1, int(h * SS))
    k = (gw, gh, seed)
    if k not in _GRAIN:
        rnd = random.Random(seed * 7919 + gw * 31 + gh)
        cols = max(1, gw // 12)
        small = Image.new("L", (cols, gh))
        small.putdata([rnd.randint(0, 255) for _ in range(cols * gh)])
        streak = small.resize((gw, gh), Image.BICUBIC)                       # long horizontal streaks
        fine = Image.new("L", (gw, gh))
        fine.putdata([rnd.randint(0, 255) for _ in range(gw * gh)])          # and a fine speckle in the anodising
        g = Image.blend(streak, fine, 0.3)
        lum = g.point(lambda v: 255 if v > 128 else 0)
        alpha = g.point(lambda v: min(255, abs(v - 128) * 2))
        im = Image.merge("RGBA", (lum, lum, lum, alpha))
        b = io.BytesIO()
        im.save(b, "PNG", optimize=True)
        _GRAIN[k] = base64.b64encode(b.getvalue()).decode()
    img = ('<image x="%g" y="%g" width="%g" height="%g" preserveAspectRatio="none" opacity="%g" href="data:image/png;base64,%s"/>'
           % (x, y, w, h, opacity, _GRAIN[k]))
    if r:
        cid = s.uid("c")
        s.defs.append('<clipPath id="%s"><rect x="%g" y="%g" width="%g" height="%g" rx="%g"/></clipPath>' % (cid, x, y, w, h, r))
        img = '<g clip-path="url(#%s)">%s</g>' % (cid, img)
    s.add(img)


# ---- parts -----------------------------------------------------------------------------------------------------------

def screw(s, cx, cy, r=6.0, silver=True):
    """a Phillips-head screw in a countersink; silver on the ears, dark on the plate"""
    s.circle(cx + 0.8, cy + 1.6, r + 1.3, "#000", extra='opacity="0.65" filter="%s"' % s.blur(1.3))
    s.circle(cx, cy, r + 1.1, "#090a0c")
    hi, mid, lo = ((232, 236, 242), (156, 164, 176), (70, 76, 86)) if silver else ((150, 156, 166), (78, 84, 94), (34, 38, 44))
    s.circle(cx, cy, r, s.rad([(0, hi, 1), (0.55, mid, 1), (1, lo, 1)], 0.36, 0.3, 0.8))
    d = r * 0.64
    s.add('<path d="M%g %g L%g %g M%g %g L%g %g" stroke="#1a1d22" stroke-width="%g" stroke-linecap="round"/>'
          % (cx - d * .93, cy - d * .37, cx + d * .93, cy + d * .37, cx + d * .37, cy - d * .93, cx - d * .37, cy + d * .93, r * 0.27))


def faceplate(s):
    """the brushed gunmetal plate between two matte black rack ears"""
    s.rect(0, 0, W, H, 0, "#0b0c0e")
    x0, x1 = EAR, W - EAR
    if STUDIO:   # black anodised aluminium: a deep charcoal, brushed grain, light falling from the top
        s.rect(x0, 0, x1 - x0, H, 0, s.lin([(0, (36, 38, 42), 1), (0.5, (25, 26, 30), 1), (1, (17, 18, 21), 1)]))
        grain(s, x0, 0, x1 - x0, H, 0.07, 3)
        s.rect(x0, 0, x1 - x0, H, 0, s.rad([(0, (255, 255, 255), 0.05), (1, (255, 255, 255), 0)], 0.3, 0.0, 0.9))
        return
    plate = [(0, (226, 229, 233), 1), (0.4, (212, 216, 221), 1), (1, (188, 193, 200), 1)] if LIGHT else \
            [(0, (44, 49, 57), 1), (0.4, (33, 37, 44), 1), (1, (22, 25, 31), 1)]
    s.rect(x0, 0, x1 - x0, H, 0, s.lin(plate))
    for i, yy in enumerate(range(0, H, 2)):   # brushed: uneven horizontal streaks
        k = (i * 53) % 11
        s.rect(x0, yy, x1 - x0, 1, 0, "#ffffff" if k % 3 else "#000000", extra='opacity="%.3f"' % (0.012 + 0.028 * k / 10))
    s.rect(x0, 0, x1 - x0, H, 0, s.lin([(0, (255, 255, 255), 0.06), (0.35, (255, 255, 255), 0.0), (1, (0, 0, 0), 0.12)], 0, 0, 1, 1))
    for ex in ((0, x1) if EAR else ()):   # the ears (none since EAR = 0): matte black, oval holes, a silver screw in each
        s.rect(ex, 0, EAR, H, 0, s.lin([(0, (24, 25, 28), 1), (0.5, (12, 13, 15), 1), (1, (8, 9, 10), 1)], 0, 0, 1, 0))
        s.rect(ex + (EAR - 1 if ex == 0 else 0), 0, 1, H, 0, "#000", extra='opacity="0.9"')
        s.rect(ex + (EAR if ex == 0 else -6), 0, 6, H, 0, s.lin([(0, (0, 0, 0), 0.5 if ex == 0 else 0), (1, (0, 0, 0), 0 if ex == 0 else 0.5)], 0, 0, 1, 0))
        for cy in (64, 228, 400, 564):   # the mounting slots: the light of the rack shows through
            cx = ex + EAR / 2
            s.rect(cx - 12, cy - 8, 24, 16, 8, "#000", extra='opacity="0.8"')
            s.rect(cx - 11, cy - 7, 22, 14, 7, s.lin([(0, (170, 174, 180), 1), (1, (232, 235, 239), 1)]))
            s.rect(cx - 11, cy - 7, 22, 4, 3, "#000", extra='opacity="0.45"')
        for cy in (146, 482):              # and the screws holding the ears to the plate
            screw(s, ex + EAR / 2, cy, 5.4)
    for cx, cy in (((EAR + 14, H - 12), (W - EAR - 14, H - 12)) if EAR else ()):
        screw(s, cx, cy, 5.4, silver=False)


def key(s, x, y, w, h, r=6):
    """a raised plastic key (its art is the button; this is a plate behind one): light grey or dark by the theme"""
    s.rect(x + 1, y + 3, w, h, r, "#000", extra='opacity="%g" filter="%s"' % (0.3 if LIGHT else 0.6, s.blur(1.8)))
    if LIGHT:
        s.rect(x, y, w, h, r, s.lin([(0, (250, 251, 252), 1), (1, (206, 210, 216), 1)]), "#6d747e", 1)
        s.rect(x + 2, y + 1.2, w - 4, 1, 0, "#ffffff", extra='opacity="0.85"')
    else:
        s.rect(x, y, w, h, r, s.lin([(0, (58, 64, 74), 1), (1, (28, 31, 37), 1)]), "#0b0d10", 1)
        s.rect(x + 2, y + 1.2, w - 4, 1, 0, "#ffffff", extra='opacity="0.16"')


def fire_glyph(s, x, y, ch, size, k):
    """2.0: a glyph in fiery gold (the user, 2026-11-05: "the omega sign and 2 should be a fiery gold"): a red-orange
    flame halo, a hot yellow inner glow, then a molten gradient (white-hot top, gold, orange, ember-red foot), a dark
    bevel stroke and a glossy shine"""
    f = "Liberation Serif"
    fire = s.lin([(0, (255, 253, 226), 1), (0.25, (255, 230, 110), 1), (0.55, (250, 196, 50), 1),
                  (0.82, (230, 128, 18), 1), (1, (170, 52, 6), 1)])
    shine = s.lin([(0, (255, 255, 255), 0.7), (0.4, (255, 255, 255), 0), (1, (255, 255, 255), 0)])
    s.text(x, y, ch, size, "#ff3a00", "middle", 700, 0, f, 'opacity="0.7" filter="%s"' % s.blur(9 * k))
    s.text(x, y, ch, size, "#ffc832", "middle", 700, 0, f, 'opacity="0.8" filter="%s"' % s.blur(3.5 * k))
    s.text(x + 0.6, y + 1.4, ch, size, "#2a0c00", "middle", 700, 0, f, 'opacity="0.9"')
    s.text(x, y, ch, size, fire, "middle", 700, 0, f, 'stroke="#6a2404" stroke-width="%g"' % (0.9 * k))
    s.text(x, y, ch, size, shine, "middle", 700, 0, f)


def omega_logo(s, cx, cy, size=34):
    """2.0: the logo "Ω SAMPLER 2" (the user, 2026-11-05); 1.8.13's was "ΩMNI SAMPLER". The Omega and the 2 are heavy
    serif capitals (Liberation Serif Bold, 45 px at size 34) in fiery gold; SAMPLER is glowing cyan Michroma.
    Widths measured with PIL at size 34: Omega 36.0, SAMPLER 243.4, the serif 2 22.5; gaps 13; centred on cx."""
    k = size / 34.0
    w_om, w_sa, w_2, gap = 36.03 * k, 243.375 * k, 22.5 * k, 13 * k
    x0 = cx - (w_om + gap + w_sa + gap + w_2) / 2
    base = cy + size * 0.35                          # Michroma's baseline as Svg.text sets it
    om = 45 * k
    gy = base - om * 0.35                            # the serif glyphs on the same baseline
    fire_glyph(s, x0 + w_om / 2, gy, "\u03a9", om, k)
    sx = x0 + w_om + gap
    s.text(sx, cy, "SAMPLER", size, hexc(CYAN), "start", 400, 0, "Michroma", 'opacity="0.65" filter="%s"' % s.blur(4 * k))
    s.text(sx, cy, "SAMPLER", size, hexc(CYAN), "start", 400, 0, "Michroma")
    fire_glyph(s, sx + w_sa + gap + w_2 / 2, gy, "2", om, k)


def qled(s, x, y, w, h, r=6, glare=False):
    """1.8.13 (the user: "all the screens ... like a 4k QLED screen"): a premium flat panel: a thin glossy black
    bezel, true-black glass with a faint backlight lift in the middle, the quantum-dot anti-glare sheen (violet into
    teal), a soft top reflection, dark edge vignetting; big screens add a faint diagonal glare"""
    b = 2.2 if min(w, h) < 60 else 4
    s.rect(x - b - 0.8, y - b - 0.8, w + 2 * b + 1.6, h + 2 * b + 1.6, r + b, "#000", extra='opacity="%g"' % (0.35 if LIGHT else 0.85))
    s.rect(x - b, y - b, w + 2 * b, h + 2 * b, r + b, s.lin([(0, (70, 73, 80), 1), (0.12, (22, 23, 26), 1), (1, (6, 6, 8), 1)]))
    s.rect(x - b + 1, y - b + 0.6, w + 2 * b - 2, 0.9, 0, "#ffffff", extra='opacity="0.28"')
    s.rect(x, y, w, h, r, s.lin([(0, (4, 5, 9), 1), (1, (1, 1, 3), 1)]), "#000", 0.8)
    s.rect(x, y, w, h, r, s.rad([(0, (18, 30, 58), 0.55), (0.7, (8, 12, 26), 0.2), (1, (0, 0, 0), 0)], 0.5, 0.55, 0.7))
    s.rect(x, y, w, h, r, s.lin([(0, (120, 60, 255), 0.07), (0.5, (0, 0, 0), 0), (1, (0, 200, 220), 0.06)], 0, 0, 1, 1))
    s.rect(x, y, w, h, r, s.lin([(0, (255, 255, 255), 0.09), (0.5, (255, 255, 255), 0.015), (1, (255, 255, 255), 0)]))
    s.rect(x + 0.5, y + 0.5, w - 1, h - 1, r, "none", "#000", 2.2, 'opacity="0.55"')
    if glare:
        s.add('<path d="M%g %g L%g %g L%g %g L%g %g Z" fill="#ffffff" opacity="0.035"/>'
              % (x + w * 0.22, y, x + w * 0.46, y, x + w * 0.16, y + h, x + 2, y + h))


def glass(s, x, y, w, h, r=6, deep=False):
    """a readout window / display: a QLED panel (1.8.13)"""
    qled(s, x, y, w, h, min(r, 6), glare=deep and w > 300)


def screen(s, x, y, w, h):
    """the browser display: a thick black gloss frame cut into the plate, a QLED panel inside"""
    s.rect(x - 4, y - 4, w + 8, h + 8, 14, "#000", extra='opacity="%g" filter="%s"' % (0.35 if LIGHT else 0.55, s.blur(3)))
    s.rect(x, y, w, h, 12, s.lin([(0, (44, 46, 52), 1), (0.06, (16, 17, 20), 1), (1, (8, 8, 10), 1)]), "#3a3e45", 1.2)
    s.rect(x + 2, y + 1.2, w - 4, 1, 0, "#ffffff", extra='opacity="0.3"')
    qled(s, x + 9, y + 9, w - 18, h - 18, 6, glare=True)


def module(s, x, y, w, h, title=None, colour=None):
    """a raised, screwed-down control module; colour: a built-in hardware module in that paint (1.8.13)"""
    if STUDIO:   # a dark anodised sub-panel set into the chassis: machined chamfer, grain, the section's accent stripe
        s.rect(x - 1.5, y - 1.5, w + 3, h + 3, 9.5, "#000", extra='opacity="0.7"')
        s.rect(x, y, w, h, 8, s.lin([(0, (44, 46, 51), 1), (1, (28, 29, 33), 1)]), "#0a0b0d", 1)
        grain(s, x, y, w, h, 0.08, int(x * 7 + y), 8)
        s.rect(x + 3, y + 1, w - 6, 1, 0, "#ffffff", extra='opacity="0.16"')                  # the lit top chamfer
        s.rect(x + 3, y + h - 1.6, w - 6, 1, 0, "#000", extra='opacity="0.6"')                # the shaded bottom one
        if colour:
            s.rect(x + 18, y + 5, w - 36, 2.4, 1.2, hexc(colour), extra='opacity="0.85"')    # anodised accent stripe
        for cx, cy in ((x + 9, y + 9), (x + w - 9, y + 9), (x + 9, y + h - 9), (x + w - 9, y + h - 9)):
            screw(s, cx, cy, 3.4, silver=False)
        if title:
            s.text(x + w / 2, y + 20, title, 18, hexc(INK), "middle", 600, 2.2)
        return
    if colour:   # painted steel, set into the chassis: a dark seam, a lit top lip, an engraved border, silver screws
        top = tuple(int(v * 0.40 + 22) for v in colour)
        bot = tuple(int(v * 0.22 + 10) for v in colour)
        s.rect(x - 1.5, y - 1.5, w + 3, h + 3, 11.5, "#000", extra='opacity="0.55"')
        s.rect(x, y, w, h, 10, s.lin([(0, top, 1), (1, bot, 1)]), hexc(tuple(int(v * 0.12) for v in colour)), 1)
        for i, yy in enumerate(range(int(y) + 3, int(y + h) - 3, 3)):   # a faint brushed grain in the paint
            if i % 2:
                s.rect(x + 4, yy, w - 8, 1, 0, "#ffffff", extra='opacity="0.025"')
        s.rect(x + 3, y + 1.2, w - 6, 1.2, 0, "#ffffff", extra='opacity="0.32"')
        s.rect(x + 5, y + 5, w - 10, h - 10, 7, "none", hexc(tuple(min(255, int(v * 0.55 + 40)) for v in colour)), 0.8, 'opacity="0.45"')
        for cx, cy in ((x + 11, y + 11), (x + w - 11, y + 11), (x + 11, y + h - 11), (x + w - 11, y + h - 11)):
            screw(s, cx, cy, 3.8, silver=True)
        if title:
            s.text(x + w / 2, y + 20, title, 18, hexc(INK), "middle", 600, 2.2)
        return
    if LIGHT:   # a light panel, its edge a fine dark seam with a lit top lip, set slightly proud of the plate
        s.rect(x + 1, y + 2.5, w, h, 10, "#000", extra='opacity="0.28" filter="%s"' % s.blur(3))
        s.rect(x, y, w, h, 10, s.lin([(0, (246, 247, 249), 1), (1, (222, 226, 231), 1)]), "#8d949e", 1.1)
        s.rect(x + 3, y + 1.4, w - 6, 1.2, 0, "#ffffff", extra='opacity="0.9"')
    else:
        s.rect(x + 2, y + 4, w, h, 10, "#000", extra='opacity="0.6" filter="%s"' % s.blur(3.5))
        s.rect(x, y, w, h, 10, s.lin([(0, (46, 52, 61), 1), (1, (29, 33, 40), 1)]), "#0c0e12", 1.2)
        s.rect(x + 3, y + 1.4, w - 6, 1, 0, "#ffffff", extra='opacity="0.14"')
    for cx, cy in ((x + 11, y + 11), (x + w - 11, y + 11), (x + 11, y + h - 11), (x + w - 11, y + h - 11)):
        screw(s, cx, cy, 3.8, silver=LIGHT)
    if title:
        s.text(x + w / 2, y + 20, title, 18, hexc(PRINT), "middle", 600, 2.2)
        tw = len(title) * 12 + 30
        for a, b in ((x + 28, x + w / 2 - tw / 2), (x + w / 2 + tw / 2, x + w - 28)):
            s.rect(a, y + 20, b - a, 1, 0, "#a7aeb8" if LIGHT else hexc(EDGE))


def label(s, x, y, txt, size=12, anchor="middle", fill=None, spacing=1.4):
    """silkscreened panel lettering: capitals, letter-spaced, flat (no glow)"""
    s.text(x, y, txt.upper(), size, hexc(fill or PRINT_DIM), anchor, 600, spacing)


def data_dial(s, cx, cy, r):
    """the DATA encoder: knurled black grip ring, machined aluminium cap with radial (anisotropic) shine, a cyan dot"""
    s.circle(cx + 2, cy + 5, r + 4, "#000", extra='opacity="0.7" filter="%s"' % s.blur(4))
    s.circle(cx, cy, r + 3, "#060708")
    s.circle(cx, cy, r, s.rad([(0, (40, 42, 46), 1), (1, (14, 15, 17), 1)], 0.4, 0.35, 0.8))
    import math
    for k in range(60):   # knurling
        a = 2 * math.pi * k / 60
        s.add('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="1.1" opacity="0.8"/>'
              % (cx + math.cos(a) * (r - 6), cy + math.sin(a) * (r - 6), cx + math.cos(a) * (r - 0.5), cy + math.sin(a) * (r - 0.5),
                 "#3a3d43" if k % 2 else "#0a0b0d"))
    rc = r * 0.74
    s.circle(cx, cy, rc, s.rad([(0, (196, 202, 210), 1), (0.6, (120, 126, 136), 1), (1, (62, 66, 74), 1)], 0.42, 0.36, 0.85),
             "#0d0e10", 1)
    for k in range(16):   # brushed-aluminium spokes: alternating light / dark wedges round the centre
        a0, a1 = 2 * math.pi * k / 16, 2 * math.pi * (k + 0.5) / 16
        s.add('<path d="M%g %g L%g %g A%g %g 0 0 1 %g %g Z" fill="%s" opacity="%.2f"/>'
              % (cx, cy, cx + math.cos(a0) * rc, cy + math.sin(a0) * rc, rc, rc, cx + math.cos(a1) * rc, cy + math.sin(a1) * rc,
                 "#ffffff" if k % 2 else "#000000", 0.10 if k % 2 else 0.12))
    s.circle(cx, cy, rc * 0.28, s.rad([(0, (90, 94, 102), 1), (1, (40, 43, 48), 1)]))
    s.circle(cx - rc * 0.46, cy - rc * 0.46, 3.2, hexc(CYAN), extra='filter="%s"' % s.blur(1.2))
    s.circle(cx - rc * 0.46, cy - rc * 0.46, 2.2, "#c8f6ff")


def jack(s, cx, cy, r=11):
    """a 1/4-inch jack: chrome nut, black hole (decoration)"""
    s.circle(cx + 1, cy + 2, r + 2, "#000", extra='opacity="0.6" filter="%s"' % s.blur(1.5))
    s.circle(cx, cy, r, s.rad([(0, (220, 224, 230), 1), (0.7, (120, 126, 136), 1), (1, (52, 56, 62), 1)], 0.38, 0.32, 0.8))
    s.circle(cx, cy, r * 0.62, "#050506", "#2d3036", 1)
    s.circle(cx, cy, r * 0.36, "#000")


def usb_port(s, cx, cy):
    """a USB type-B socket, recessed (decoration)"""
    s.rect(cx - 15, cy - 14, 30, 28, 4, s.lin([(0, (90, 96, 106), 1), (1, (40, 44, 50), 1)]), "#0c0d0f", 1)
    s.add('<path d="M%g %g h20 v12 l-4 6 h-12 l-4 -6 Z" fill="#050506"/>' % (cx - 10, cy - 9))
    s.rect(cx - 5, cy - 5, 10, 7, 1, "#d8dadd", extra='opacity="0.85"')


def card_slot(s, cx, cy, w=118):
    """an SD card slot cut into the plate (decoration)"""
    s.rect(cx - w / 2 - 3, cy - 8, w + 6, 16, 4, s.lin([(0, (12, 13, 15), 1), (1, (40, 44, 50), 1)]), "#000", 1)
    s.rect(cx - w / 2, cy - 3, w, 6, 2, "#000")
    s.rect(cx - w / 2 + 2, cy - 3, w - 4, 2, 1, "#000", extra='opacity="0.9" filter="%s"' % s.blur(1))


def led_socket(s, cx, cy, r=5):
    """the bezel round an LED (the LED itself is a meter strip over it)"""
    s.circle(cx, cy + 0.8, r + 2.2, "#000", extra='opacity="0.7"')
    s.circle(cx, cy, r + 1.4, s.lin([(0, (20, 22, 25), 1), (1, (60, 64, 70), 1)]))


def led_strip(path, colour, size=14):
    """an LED as a 2-state meter filmstrip (frame 0 off, frame 1 lit with glow), 128 frames: lit above half"""
    frames = 128   # MPC always cuts a FilmStrip into 128 frames
    out = Image.new("RGBA", (size, size * frames), (0, 0, 0, 0))
    looks = []
    for on in (0, 1):
        s = Svg(size, size)
        c = size / 2
        rr = size * 0.30
        if on:
            s.circle(c, c, size * 0.48, hexc(colour), extra='opacity="0.55" filter="%s"' % s.blur(size * 0.12))
            s.circle(c, c, rr, s.rad([(0, (255, 255, 255), 1), (0.45, colour, 1), (1, tuple(int(v * 0.6) for v in colour), 1)], 0.4, 0.35, 0.7))
        else:
            dim = tuple(int(v * 0.22) + 8 for v in colour)
            s.circle(c, c, rr, s.rad([(0, tuple(min(255, v + 30) for v in dim), 1), (1, dim, 1)], 0.4, 0.35, 0.7))
        looks.append(render(s))
    for f in range(frames):
        out.paste(looks[1 if f >= frames // 2 else 0], (0, f * size))
    out.save(path)


def _key(w, h, lit, accent=CYAN):
    """a raised dark plastic key with an inner bevel; lit: lit from inside in the accent, its edge glowing"""
    s = Svg(w, h)
    r = min(6, h / 3)
    s.rect(1, 2.5, w - 2, h - 3, r, "#000", extra='opacity="0.55" filter="%s"' % s.blur(1.2))
    if STUDIO:   # a graphite rubber key; its LED (a bar on top, a dot on short keys) lights in the accent
        s.rect(1, 1, w - 2, h - 3.5, r, s.lin([(0, (62, 65, 71), 1), (0.5, (44, 46, 51), 1), (1, (30, 31, 35), 1)]), "#060708", 1)
        s.rect(3, 2.1, w - 6, 1, 0, "#ffffff", extra='opacity="0.14"')
        s.rect(3, h - 5, w - 6, 1, 0, "#000", extra='opacity="0.4"')
        bar = h >= 26 and w >= 34
        lx, ly, lw, lh = ((w / 2 - min(14, w * 0.2), 4, 2 * min(14, w * 0.2), 2.4) if bar else (5, h / 2 - 3.2, 2.6, 4.4))
        if lit:
            s.rect(lx - 1.5, ly - 1.5, lw + 3, lh + 3, 2, hexc(accent), extra='opacity="0.75" filter="%s"' % s.blur(1.6))
            s.rect(lx, ly, lw, lh, 1.2, hexc(tuple(min(255, v + 70) for v in accent)))
        else:
            s.rect(lx, ly, lw, lh, 1.2, hexc(tuple(int(v * 0.22) + 12 for v in accent)))
        s.bar = bar
        return s
    if lit and LIGHT:   # backlit in the accent
        top = tuple(min(255, int(v * 0.85 + 40)) for v in accent)
        bot = tuple(int(v * 0.62) for v in accent)
    elif lit:
        top = tuple(int(v * 0.42 + 18) for v in accent)
        bot = tuple(int(v * 0.20 + 10) for v in accent)
    elif LIGHT:   # light grey plastic
        top, bot = (250, 251, 252), (206, 210, 216)
    else:
        top, bot = (66, 74, 86), (34, 38, 46)
    edge = hexc(accent) if lit and not LIGHT else ("#6d747e" if LIGHT else "#0a0c0f")
    s.rect(1, 1, w - 2, h - 3.5, r, s.lin([(0, top, 1), (1, bot, 1)]), edge, 1.3 if lit else 1)
    s.rect(3, 2.2, w - 6, 1, 0, "#ffffff", extra='opacity="%.2f"' % (0.85 if LIGHT else 0.22 if not lit else 0.3))
    s.rect(3, h - 5, w - 6, 1, 0, "#000", extra='opacity="0.35"')
    return s


def key_image(path, w, h, label_txt, lit, accent=CYAN, size=15):
    """a key with a legend; lit: the legend glows"""
    s = _key(w, h, lit, accent)
    if label_txt:
        while size > 8 and len(label_txt) * size * 0.62 > w - 10:   # the legend fits the key
            size -= 1
        if STUDIO:   # silkscreen legend, below the LED bar (beside the dot on short keys)
            ty = h / 2 + (1.2 if getattr(s, "bar", False) else -1)
            tx = w / 2 + (0 if getattr(s, "bar", False) else 2)
            s.text(tx, ty, label_txt, size, "#ffffff" if lit else hexc((196, 201, 209)), "middle", 600, 1)
            render(s).save(path)
            return
        if lit and not LIGHT:
            s.text(w / 2, h / 2 - 1, label_txt, size, hexc(accent), "middle", 600, 1, extra='opacity="0.7" filter="%s"' % s.blur(2))
        ink = "#ffffff" if lit else hexc(PRINT if LIGHT else INK)
        s.text(w / 2, h / 2 - 1, label_txt, size, ink, "middle", 600, 1)
    render(s).save(path)


def arrow_key(path, w, h, direction, lit, accent=CYAN):
    """a key with an arrow (prev / next, up / down): the arrow in the accent, glowing when lit"""
    s = _key(w, h, lit, accent)
    cx, cy, a = w / 2, h / 2 + (1.5 if STUDIO and getattr(s, "bar", False) else -1), min(w, h) * 0.22
    pts = {"left": [(cx + a * .6, cy - a), (cx - a * .7, cy), (cx + a * .6, cy + a)],
           "right": [(cx - a * .6, cy - a), (cx + a * .7, cy), (cx - a * .6, cy + a)],
           "up": [(cx - a, cy + a * .6), (cx, cy - a * .7), (cx + a, cy + a * .6)],
           "down": [(cx - a, cy - a * .6), (cx, cy + a * .7), (cx + a, cy - a * .6)]}[direction]
    d = "M%g %g L%g %g L%g %g Z" % tuple(v for p in pts for v in p)
    if lit and not LIGHT and not STUDIO:
        s.add('<path d="%s" fill="%s" opacity="0.8" filter="%s"/>' % (d, hexc(accent), s.blur(2)))
    dark_acc = hexc(tuple(int(v * 0.7) for v in accent))
    s.add('<path d="%s" fill="%s"/>' % (d, "#ffffff" if lit else dark_acc if LIGHT else "#c4c9d1" if STUDIO else hexc(accent)))
    render(s).save(path)


def _arc(cx, cy, r, a0, a1):
    """an SVG arc path from angle a0 to a1 (degrees, clockwise on screen, 0 = right)"""
    import math
    p0 = (cx + r * math.cos(math.radians(a0)), cy + r * math.sin(math.radians(a0)))
    p1 = (cx + r * math.cos(math.radians(a1)), cy + r * math.sin(math.radians(a1)))
    return "M%g %g A%g %g 0 %d 1 %g %g" % (p0[0], p0[1], r, r, 1 if a1 - a0 > 180 else 0, p1[0], p1[1])


def encoder_strip(path, colour, size=96, frames=65):
    """a knob filmstrip (minimum first), after the user's reference picture (2026-11-04): a recessed dark well, a thick
    value arc glowing in the knob's colour over a dim track, a matte charcoal dome lit from the top with a soft concave
    face, a short glowing pointer tick on the face; 270 degrees from 135 (lower left) clockwise"""
    import math
    if STUDIO:
        return studio_knob_strip(path, colour, size, frames)
    out = Image.new("RGBA", (size, size * frames), (0, 0, 0, 0))
    c = size / 2
    rr, tw = size * 0.405, size * 0.085        # arc radius and width
    kr = size * 0.315                          # the dome
    dim = tuple(int(v * 0.16 + 14) for v in colour) if not LIGHT else (176, 182, 190)
    for f in range(frames):
        t = f / (frames - 1)
        s = Svg(size, size)
        # the well cut into the plate round the knob
        well = [(0, (186, 191, 198), 1), (0.85, (198, 203, 209), 1), (1, (170, 176, 184), 1)] if LIGHT else \
               [(0, (6, 7, 9), 1), (0.85, (12, 14, 17), 1), (1, (26, 29, 34), 1)]
        s.circle(c, c, size * 0.47, s.rad(well, 0.5, 0.45, 0.55))
        s.circle(c, c + 0.8, size * 0.47, "none", "#ffffff", 0.8, extra='opacity="0.06"')
        s.add('<path d="%s" fill="none" stroke="%s" stroke-width="%g" stroke-linecap="round"/>' % (_arc(c, c, rr, 135, 405), hexc(dim), tw))
        if t > 0.004:
            a1 = 135 + 270 * t
            arc = _arc(c, c, rr, 135, a1)
            s.add('<path d="%s" fill="none" stroke="%s" stroke-width="%g" stroke-linecap="round" opacity="0.9" filter="%s"/>'
                  % (arc, hexc(colour), tw * 1.6, s.blur(size * 0.035)))
            s.add('<path d="%s" fill="none" stroke="%s" stroke-width="%g" stroke-linecap="round"/>' % (arc, hexc(colour), tw))
            s.add('<path d="%s" fill="none" stroke="#ffffff" stroke-width="%g" stroke-linecap="round" opacity="0.35"/>' % (arc, tw * 0.25))
        # the dome: shadow, body lit from the top, a dark rim, a soft concave face
        s.circle(c + 0.6, c + size * 0.03, kr + 1.5, "#000", extra='opacity="%g" filter="%s"' % (0.5 if LIGHT else 0.85, s.blur(size * 0.03)))
        fr = kr * 0.80
        if LIGHT:   # white plastic: a bright dome, a grey skirt edge, a soft dished face
            s.circle(c, c, kr, s.lin([(0, (255, 255, 255), 1), (0.5, (232, 234, 238), 1), (1, (176, 181, 189), 1)]), "#7c838d", 1)
            s.circle(c, c, fr, s.lin([(0, (226, 229, 233), 1), (1, (252, 252, 253), 1)]))
        else:
            s.circle(c, c, kr, s.lin([(0, (78, 82, 90), 1), (0.45, (40, 43, 49), 1), (1, (16, 17, 20), 1)]), "#08090b", 1)
            s.circle(c, c, fr, s.lin([(0, (24, 26, 30), 1), (1, (52, 55, 62), 1)]))
            s.circle(c, c, fr, s.rad([(0, (70, 74, 82), 0.35), (1, (0, 0, 0), 0)], 0.45, 0.3, 0.6))
        s.add('<path d="%s" fill="none" stroke="#ffffff" stroke-width="%g" stroke-linecap="round" opacity="0.18"/>'
              % (_arc(c, c, kr - 0.8, 205, 335), size * 0.012))
        # the pointer tick, glowing in the knob's colour
        a = math.radians(135 + 270 * t)
        p0 = (c + math.cos(a) * fr * 0.42, c + math.sin(a) * fr * 0.42)
        p1 = (c + math.cos(a) * fr * 0.80, c + math.sin(a) * fr * 0.80)
        ln = '<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="%g" stroke-linecap="round" %s/>'
        if LIGHT:   # a dark printed pointer line from the centre
            q0 = (c + math.cos(a) * fr * 0.15, c + math.sin(a) * fr * 0.15)
            s.add(ln % (q0[0], q0[1], p1[0], p1[1], "#2a2f37", size * 0.04, ""))
        else:
            s.add(ln % (p0[0], p0[1], p1[0], p1[1], hexc(colour), size * 0.06, 'opacity="0.8" filter="%s"' % s.blur(size * 0.02)))
            s.add(ln % (p0[0], p0[1], p1[0], p1[1], hexc(tuple(min(255, v + 50) for v in colour)), size * 0.035, ""))
        out.paste(render(s), (0, f * size))
    out.save(path)


def fader_strip(path, w, h, frames=65, colour=(80, 150, 255)):
    """a vertical fader filmstrip (minimum first): a recessed slot with scale ticks, the fill glowing up to the cap,
    a machined metal cap with grip grooves and a cyan index line"""
    if STUDIO:
        return studio_fader_strip(path, w, h, frames, colour)
    out = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    top, bot, sx = 10, h - 10, w / 2
    ch = max(10, min(18, h * 0.09))             # cap height
    for f in range(frames):
        t = f / (frames - 1)
        s = Svg(w, h)
        for k in range(11):                    # scale ticks either side
            ty = bot - (bot - top) * k / 10
            ln = w * (0.16 if k % 5 == 0 else 0.09)
            for x0 in (sx - w * 0.22 - ln, sx + w * 0.22):
                s.rect(x0, ty - 0.5, ln, 1, 0, "#3c434d" if LIGHT else "#5b6470", extra='opacity="0.8"')
        s.rect(sx - 4.5, top - 4, 9, bot - top + 8, 4.5, "#000", extra='opacity="0.8"')
        s.rect(sx - 3.5, top - 3, 7, bot - top + 6, 3.5, s.lin([(0, (6, 8, 11), 1), (1, (16, 19, 24), 1)]))
        s.rect(sx - 3.5, bot + 3.6, 7, 1, 0, "#ffffff", extra='opacity="0.08"')
        ty = bot - (bot - top) * t
        if t > 0.004:
            s.rect(sx - 3, ty, 6, bot - ty + 3, 3, hexc(colour), extra='opacity="0.7" filter="%s"' % s.blur(2.2))
            s.rect(sx - 2, ty, 4, bot - ty + 3, 2, s.lin([(0, CYAN, 1), (1, colour, 1)]))
        cw = w * 0.84
        s.rect(sx - cw / 2 + 1, ty - ch / 2 + 2.5, cw, ch, 3, "#000", extra='opacity="0.7" filter="%s"' % s.blur(1.5))
        s.rect(sx - cw / 2, ty - ch / 2, cw, ch, 3, s.lin([(0, (214, 220, 228), 1), (0.48, (140, 148, 160), 1),
                                                          (0.52, (98, 106, 118), 1), (1, (62, 68, 78), 1)]), "#14171b", 1)
        for k in (-0.3, 0.3):
            s.rect(sx - cw / 2 + 3, ty + k * ch - 0.5, cw - 6, 1, 0, "#2a2f36", extra='opacity="0.7"')
        s.rect(sx - cw * 0.36, ty - 0.8, cw * 0.72, 1.6, 0.8, hexc(CYAN))
        out.paste(render(s), (0, f * h))
    out.save(path)


def pad_image(path, w, h, lit, tint, margin=6):
    """an MPC-style rubber pad (w x h inside a margin): tinted, soft top light; lit: glowing from inside"""
    s = Svg(w + 2 * margin, h + 2 * margin)
    x, y = margin, margin
    s.rect(x + 1, y + 2.5, w, h, 7, "#000", extra='opacity="0.6" filter="%s"' % s.blur(2))
    if STUDIO:   # grey-black silicone pads (unlit, only a hint of the bank tint); lit: backlit through the rubber
        if lit:
            s.rect(x - 2, y - 2, w + 4, h + 4, 9, hexc(tint), extra='opacity="0.55" filter="%s"' % s.blur(3.5))
            hi = tuple(min(255, int(v * 0.5 + 128)) for v in tint)
            s.rect(x, y, w, h, 7, s.rad([(0, hi, 1), (0.7, tint, 1), (1, tuple(int(v * 0.6) for v in tint), 1)], 0.5, 0.4, 0.75),
                   hexc(hi), 1.2)
        else:
            top = tuple(int(a * 0.12 + b * 0.88) for a, b in zip(tint, (58, 60, 65)))
            s.rect(x, y, w, h, 7, s.lin([(0, top, 1), (1, (30, 31, 35), 1)]), "#050506", 1.2)
            s.rect(x + 4, y + 2, w - 8, h * 0.35, 5, "#ffffff", extra='opacity="0.04"')
            s.rect(x + 5, y + 1.6, w - 10, 1, 0, "#ffffff", extra='opacity="0.16"')
        render(s).save(path)
        return
    if lit:
        s.rect(x - 2, y - 2, w + 4, h + 4, 9, hexc(CYAN), extra='opacity="0.6" filter="%s"' % s.blur(3.5))
        s.rect(x, y, w, h, 7, s.rad([(0, (220, 252, 255), 1), (0.6, (110, 215, 245), 1), (1, (50, 150, 210), 1)], 0.5, 0.4, 0.75),
               "#c8f6ff", 1.4)
    else:
        top = tuple(int(a * 0.55 + b * 0.45) for a, b in zip(tint, (30, 36, 46)))
        s.rect(x, y, w, h, 7, s.lin([(0, top, 1), (1, (20, 24, 31), 1)]), "#0a0c10", 1.2)
        s.rect(x + 4, y + 2, w - 8, h * 0.35, 5, "#ffffff", extra='opacity="0.06"')
        s.rect(x + 5, y + 1.6, w - 10, 1, 0, "#ffffff", extra='opacity="0.22"')
    render(s).save(path)


def knob_strip(path, size=56, frames=65):
    """a rubberised black pot (VOLUME, PAN): grippy skirt, a white pointer line, 270 degrees of travel from lower
    left, minimum first; a faint cyan scale arc on the plate under it"""
    import math
    out = Image.new("RGBA", (size, size * frames), (0, 0, 0, 0))
    for f in range(frames):
        t = f / (frames - 1)
        s = Svg(size, size)
        c, r = size / 2, size * 0.36
        for k in range(11):   # the scale dots
            a = math.radians(135 + 27 * k)
            s.circle(c + math.cos(a) * size * 0.46, c + math.sin(a) * size * 0.46, 1.1, hexc(CYAN) if k <= t * 10 + 0.01 else "#59606a")
        s.circle(c + 1, c + 2.5, r + 1, "#000", extra='opacity="0.7" filter="%s"' % s.blur(2))
        s.circle(c, c, r, s.rad([(0, (60, 63, 68), 1), (0.7, (24, 25, 28), 1), (1, (10, 10, 12), 1)], 0.4, 0.35, 0.8), "#050506", 1)
        for k in range(24):   # rubber ribs
            a = 2 * math.pi * k / 24
            s.add('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="#000" stroke-width="1" opacity="0.6"/>'
                  % (c + math.cos(a) * r * 0.82, c + math.sin(a) * r * 0.82, c + math.cos(a) * r, c + math.sin(a) * r))
        s.circle(c, c, r * 0.66, s.rad([(0, (48, 50, 55), 1), (1, (22, 23, 26), 1)], 0.4, 0.35, 0.8))
        a = math.radians(135 + 270 * t)
        s.add('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="#f2f4f6" stroke-width="2.4" stroke-linecap="round"/>'
              % (c + math.cos(a) * r * 0.25, c + math.sin(a) * r * 0.25, c + math.cos(a) * r * 0.9, c + math.sin(a) * r * 0.9))
        out.paste(render(s), (0, f * size))
    out.save(path)


def studio_knob(s, size, t, colour):
    """one frame of the studio knob: an LED ring (21 LEDs over 270 degrees, lit up to the value in the section's
    colour), a knurled black skirt with a white index line, a machined aluminium cap (static anisotropic sheen, as
    real light does not turn with the knob) with an engraved line"""
    import math
    c = size / 2
    n, lr, dr = 21, size * 0.445, max(1.1, size * 0.024)
    hot = tuple(min(255, int(v * 0.6 + 110)) for v in colour)
    for k in range(n):
        a = math.radians(135 + 270 * k / (n - 1))
        x, y = c + math.cos(a) * lr, c + math.sin(a) * lr
        if t > 0.004 and k / (n - 1) <= t + 1e-6:
            s.circle(x, y, dr * 2.0, hexc(colour), extra='opacity="0.6" filter="%s"' % s.blur(dr * 0.9))
            s.circle(x, y, dr, hexc(hot))
        else:
            s.circle(x, y, dr, "#17181b", "#000", 0.5)
    r = size * 0.355                                   # the skirt
    s.circle(c + 0.5, c + size * 0.035, r + 1.2, "#000", extra='opacity="0.85" filter="%s"' % s.blur(size * 0.03))
    s.circle(c, c, r, s.lin([(0, (46, 48, 53), 1), (0.5, (22, 23, 26), 1), (1, (8, 8, 10), 1)]), "#000", 0.8)
    for k in range(40):                                # knurling
        a = 2 * math.pi * k / 40
        s.add('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="%g" opacity="0.75"/>'
              % (c + math.cos(a) * r * 0.80, c + math.sin(a) * r * 0.80, c + math.cos(a) * (r - 0.6), c + math.sin(a) * (r - 0.6),
                 "#3b3e44" if k % 2 else "#000000", max(0.6, size * 0.012)))
    rc = r * 0.74                                      # the aluminium cap
    s.circle(c, c, rc + 0.8, "#000", extra='opacity="0.8"')
    s.circle(c, c, rc, s.rad([(0, (226, 230, 236), 1), (0.55, (150, 156, 166), 1), (1, (84, 89, 98), 1)], 0.4, 0.32, 0.85))
    for k in range(24):                                # turned-aluminium sheen: alternating wedges round the centre
        a0, a1 = 2 * math.pi * k / 24, 2 * math.pi * (k + 0.5) / 24
        s.add('<path d="M%g %g L%g %g A%g %g 0 0 1 %g %g Z" fill="%s" opacity="%.2f"/>'
              % (c, c, c + math.cos(a0) * rc, c + math.sin(a0) * rc, rc, rc, c + math.cos(a1) * rc, c + math.sin(a1) * rc,
                 "#ffffff" if k % 2 else "#000000", 0.08 if k % 2 else 0.07))
    s.circle(c, c, rc, "none", "#ffffff", max(0.5, size * 0.008), 'opacity="0.35"')
    a = math.radians(135 + 270 * t)
    ln = '<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="%g" stroke-linecap="round"/>'
    s.add(ln % (c + math.cos(a) * rc * 0.25, c + math.sin(a) * rc * 0.25, c + math.cos(a) * rc * 0.92, c + math.sin(a) * rc * 0.92,
                "#16181c", max(1.0, size * 0.03)))                                    # engraved in the cap
    s.add(ln % (c + math.cos(a) * (rc + 1.5), c + math.sin(a) * (rc + 1.5), c + math.cos(a) * (r - 1), c + math.sin(a) * (r - 1),
                "#f4f6f8", max(1.0, size * 0.035)))                                   # printed on the skirt


def studio_knob_strip(path, colour, size, frames):
    out = Image.new("RGBA", (size, size * frames), (0, 0, 0, 0))
    for f in range(frames):
        s = Svg(size, size)
        studio_knob(s, size, f / (frames - 1), colour)
        out.paste(render(s), (0, f * size))
    out.save(path)


def studio_fader_strip(path, w, h, frames, colour):
    """the studio fader: a black slot with silkscreen ticks, a thin LED line in the section colour up to the cap,
    a black plastic cap with grip ridges and a white index line"""
    out = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    top, bot, sx = 10, h - 10, w / 2
    ch = max(12, min(20, h * 0.1))
    for f in range(frames):
        t = f / (frames - 1)
        s = Svg(w, h)
        for k in range(11):
            ty = bot - (bot - top) * k / 10
            ln = w * (0.16 if k % 5 == 0 else 0.09)
            for x0 in (sx - w * 0.2 - ln, sx + w * 0.2):
                s.rect(x0, ty - 0.5, ln, 1, 0, "#aab0b8", extra='opacity="%g"' % (0.75 if k % 5 == 0 else 0.45))
        s.rect(sx - 4, top - 4, 8, bot - top + 8, 4, "#000", extra='opacity="0.9"')
        s.rect(sx - 3, top - 3, 6, bot - top + 6, 3, s.lin([(0, (4, 4, 6), 1), (1, (14, 15, 18), 1)], 0, 0, 1, 0))
        s.rect(sx - 3, bot + 3.4, 6, 1, 0, "#ffffff", extra='opacity="0.1"')
        ty = bot - (bot - top) * t
        if t > 0.004:
            s.rect(sx - 1, ty, 2, bot - ty + 2, 1, hexc(colour), extra='opacity="0.9"')
            s.rect(sx - 2, ty, 4, bot - ty + 2, 2, hexc(colour), extra='opacity="0.35" filter="%s"' % s.blur(1.5))
        cw = w * 0.80
        s.rect(sx - cw / 2 + 1, ty - ch / 2 + 3, cw, ch, 2.5, "#000", extra='opacity="0.8" filter="%s"' % s.blur(1.6))
        s.rect(sx - cw / 2, ty - ch / 2, cw, ch, 2.5, s.lin([(0, (70, 73, 80), 1), (0.45, (34, 36, 40), 1), (1, (12, 13, 15), 1)]), "#000", 0.8)
        for k in (-0.32, -0.18, 0.18, 0.32):
            s.rect(sx - cw / 2 + 2, ty + k * ch - 0.4, cw - 4, 0.9, 0, "#000", extra='opacity="0.7"')
        s.rect(sx - cw / 2 + 1, ty - ch / 2 + 0.8, cw - 2, 0.8, 0, "#ffffff", extra='opacity="0.25"')
        s.rect(sx - cw / 2 + 1.5, ty - 0.9, cw - 3, 1.8, 0.9, "#f4f6f8")
        out.paste(render(s), (0, f * h))
    out.save(path)
