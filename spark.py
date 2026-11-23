"""2.1.5: the logo's golden spark (the user, 2026-11-05: "a golden spark of electricity from the omega sign to the
number 2, a ten second transparent overlay"). MPC skins can't play a GIF, so this is a meter filmstrip of 128 frames
(MPC always slices 128) over the logo: the engine's read-only param logo_fx picks the frame from the clock (frame 0 =
blank for most of the 10 s loop; frames 1..127 = the ~2.5 s spark). Frames are drawn at 3x and scaled to STRIP_W x
STRIP_H; the layout stretches them over AREA (the decoded size stays inside the user's skin budget, ~230 MB since 2026-11-17 (was 150 MB)).

    python3 spark.py out.png [preview.gif]
"""
import math
import random
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageMath

FRAMES = 128
AREA = (456, 4, 380, 48)            # plugin coordinates: x, y, w, h (the logo glass is 440,4 400x48)
STRIP_W, STRIP_H = 214, 27          # one frame in the strip
SS = 3                              # supersampling: drawn at AREA x SS
# the logo (rack.omega_logo at cx 640, cy 28, size 34): the Omega's centre x 494, the 2's 792.7; the glyphs' middle y ~24
OMEGA = (494.1, 24.2)
TWO = (792.7, 24.2)
START = (513.0, 17.0)               # the bolt leaves the Omega's right shoulder ...
END = (782.0, 16.0)                 # ... and strikes the 2's left top
ACTIVE = 2.56                       # seconds of the loop the spark takes (the engine maps them onto frames 1..127)

GOLD_CORE = (255, 252, 228)
GOLD = (255, 205, 70)
EMBER = (255, 120, 20)


def bolt_points(rnd, a, b, rough, depth=7):
    """a jagged bolt from a to b by midpoint displacement, arched up over SAMPLER's letters"""
    pts = [a, b]
    for d in range(depth):
        out = [pts[0]]
        amp = rough * (0.62 ** d)
        for p, q in zip(pts, pts[1:]):
            mx, my = (p[0] + q[0]) / 2, (p[1] + q[1]) / 2
            dx, dy = q[0] - p[0], q[1] - p[1]
            ln = math.hypot(dx, dy) or 1
            off = rnd.uniform(-amp, amp)
            out += [(mx - dy / ln * off, my + dx / ln * off), q]
        pts = out
    ax, bx = a[0], b[0]
    arched = []
    for x, y in pts:
        u = (x - ax) / (bx - ax) if bx != ax else 0
        arched.append((x, y - 7.0 * math.sin(math.pi * max(0, min(1, u)))))
    return arched


def to_px(p):
    return ((p[0] - AREA[0]) * SS, (p[1] - AREA[1]) * SS)


def scaled(im, k):
    return im.point(lambda v: min(255, int(v * k + 0.5)))


class Layer:
    """an 8-bit light canvas (AREA x SS): strokes and dots add up"""
    def __init__(self):
        self.w, self.h = AREA[2] * SS, AREA[3] * SS
        self.a = Image.new("L", (self.w, self.h), 0)

    def _add(self, im, strength):
        self.a = ImageChops.add(self.a, scaled(im, strength))

    def stroke(self, pts, width, strength):
        im = Image.new("L", (self.w, self.h), 0)
        ImageDraw.Draw(im).line([to_px(p) for p in pts], fill=255, width=max(1, int(round(width * SS))), joint="curve")
        self._add(im, strength)

    def dot(self, p, r, strength):
        im = Image.new("L", (self.w, self.h), 0)
        x, y = to_px(p)
        rr = r * SS
        ImageDraw.Draw(im).ellipse((x - rr, y - rr, x + rr, y + rr), fill=255)
        self._add(im, strength)

    def glow(self, radius, strength):
        return scaled(self.a.filter(ImageFilter.GaussianBlur(radius * SS)), strength)


def frame(i):
    """frame i of 128 as RGBA (AREA x SS); 0 = blank"""
    w, h = AREA[2] * SS, AREA[3] * SS
    if i == 0:
        return Image.new("RGBA", (w, h), (0, 0, 0, 0))
    t = (i - 1) / (FRAMES - 2)                       # 0..1 over the spark
    rnd = random.Random(1000 + i)
    core, halo = Layer(), Layer()
    # phases: charge 0..0.12, travel 0.12..0.48, strike 0.48..0.58, fade 0.58..1
    charge = min(1, t / 0.12)
    if t < 0.58:
        omega_glow = 0.35 + 0.65 * charge * (0.8 + 0.2 * rnd.random())
    else:
        omega_glow = max(0, 1 - (t - 0.58) / 0.25)
    # the Omega charging: crackles around its shoulder
    if t < 0.5:
        for _ in range(int(2 + 5 * charge)):
            a = rnd.uniform(-math.pi * 1.1, math.pi * 0.1)           # around the Omega's arch
            b = a + rnd.uniform(0.5, 1.2)
            r0 = 15.5
            p = (OMEGA[0] + math.cos(a) * r0, OMEGA[1] - 2 + math.sin(a) * r0)
            q = (OMEGA[0] + math.cos(b) * r0, OMEGA[1] - 2 + math.sin(b) * r0)
            core.stroke(bolt_points(rnd, p, q, 4.0, 4), 0.6, 0.7 * charge)
    halo.dot(OMEGA, 17, 0.55 * omega_glow)
    # the leader travelling to the 2, then the full bolt
    if 0.12 <= t < 0.58:
        u = min(1.0, (t - 0.12) / 0.36)
        u = 1 - (1 - u) ** 2                          # fast out of the Omega, easing in to the 2
        full = bolt_points(rnd, START, END, 16.0)
        n = max(2, int(len(full) * u))
        pts = full[:n]
        flash = 1.0 if t >= 0.48 else 0.75
        core.stroke(pts, 1.1 if t < 0.48 else 1.6, flash)
        # a thinner second strand and forks
        alt = bolt_points(rnd, START, END, 20.0)[:n]
        core.stroke(alt, 0.6, 0.45 * flash)
        for _ in range(3 if t < 0.48 else 6):
            k = rnd.randrange(1, n)
            p = pts[k]
            ang = rnd.choice((-1, 1)) * rnd.uniform(0.35, 1.0)          # forks branch forward, up or down
            ln = rnd.uniform(8, 18)
            q = (p[0] + math.cos(ang) * ln, p[1] + math.sin(ang) * ln * 0.7)
            fork = [(fx, fy) for fx, fy in bolt_points(rnd, p, q, 3.5, 4)]
            core.stroke(fork, 0.5, 0.55 * flash)
        head = pts[-1]
        halo.dot(head, 5 if t < 0.48 else 3, 1.0)
        core.dot(head, 1.6, 1.0)
    # the strike: the 2 bursts, sparks fly
    if t >= 0.46:
        s = (t - 0.46) / 0.54
        burst = math.exp(-s * 5.5)
        halo.dot(TWO, 12 + 8 * (1 - burst), 0.8 * burst)            # the 2 lights up, it doesn't vanish in a blob
        core.dot(END, 3.5 * burst, 0.9 * burst)
        rs = random.Random(77)
        for _ in range(26):                          # sparks: fixed paths, moving out with gravity
            ang = rs.uniform(-math.pi * 0.95, math.pi * 0.15)
            v = rs.uniform(30, 75)
            life = rs.uniform(0.35, 1.0)
            if s > life:
                continue
            tt = s * 0.9
            px = TWO[0] + math.cos(ang) * v * tt
            py = TWO[1] + math.sin(ang) * v * tt + 60 * tt * tt
            fade = 1 - s / life
            core.dot((px, py), 0.9, 0.9 * fade)
            halo.dot((px, py), 2.2, 0.35 * fade)
    # the bolt's after-image flickering out
    if 0.58 <= t < 0.78:
        f = 1 - (t - 0.58) / 0.2
        if rnd.random() < 0.75:
            core.stroke(bolt_points(rnd, START, END, 16.0), 0.8, 0.6 * f)
    # light: white-hot core, gold glow, ember fringe (premultiplied, then divided by alpha)
    c = core.a
    g1 = ImageChops.add(core.glow(1.4, 1.6), halo.glow(2.5, 1.0))
    g2 = scaled(ImageChops.add(core.glow(4.5, 1.2), halo.glow(6, 0.9)), 0.75)
    alpha = ImageChops.add(ImageChops.add(c, scaled(g1, 0.9)), scaled(g2, 0.6))
    chans = []
    for k in range(3):
        pre = ImageChops.add(ImageChops.add(scaled(c, GOLD_CORE[k] / 255), scaled(g1, 0.9 * GOLD[k] / 255)),
                             scaled(g2, 0.6 * EMBER[k] / 255))
        if hasattr(ImageMath, "eval"):
            chans.append(ImageMath.eval("convert(min((p * 255) / max(a, 1), 255), 'L')", p=pre, a=alpha))
        else:   # 2.4.15: Pillow 13 has no ImageMath.eval: the same integer arithmetic (output pixel-identical)
            import numpy as np
            p_ = np.asarray(pre.convert("L"), dtype=np.int64)
            a_ = np.asarray(alpha.convert("L"), dtype=np.int64)
            chans.append(Image.fromarray(np.minimum((p_ * 255) // np.maximum(a_, 1), 255).astype(np.uint8), "L"))
    return Image.merge("RGBA", chans + [alpha])


def build(out_png, gif=None, bg=None):
    strip = Image.new("RGBA", (STRIP_W, STRIP_H * FRAMES), (0, 0, 0, 0))
    frames = []
    for i in range(FRAMES):
        f = frame(i)
        strip.paste(f.resize((STRIP_W, STRIP_H), Image.LANCZOS), (0, i * STRIP_H))
        frames.append(f)
    strip.save(out_png, optimize=True)
    if gif:   # a preview over the logo, the way MPC shows it (the strip scaled back up)
        base = Image.open(bg).convert("RGBA").crop((AREA[0] - 20, 0, AREA[0] + AREA[2] + 20, 56)) if bg else None
        seq = []
        for i in list(range(1, FRAMES)) + [0] * 30:
            f = strip.crop((0, i * STRIP_H, STRIP_W, (i + 1) * STRIP_H)).resize((AREA[2], AREA[3]), Image.BICUBIC)
            if base is not None:
                im = base.copy()
                im.alpha_composite(f, (20, AREA[1]))
            else:
                im = f
            seq.append(im.resize((im.width * 2, im.height * 2), Image.BICUBIC).convert("RGB"))
        seq[0].save(gif, save_all=True, append_images=seq[1:], duration=20, loop=0)
    return strip


if __name__ == "__main__":
    build(sys.argv[1], *(sys.argv[2:4] if len(sys.argv) > 2 else []))
