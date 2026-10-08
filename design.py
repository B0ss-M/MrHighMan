#!/usr/bin/env python3
"""Omni Sampler's design source: writes params.json, the skin artwork (art/*.png), skin.css and layout.conf.

    python3 design.py

The look follows a supplied panel concept (dark brushed navy panel, glowing cyan title,
ring-arc knobs, a wave display in the middle, ADSR faders and a 16-pad row). Every control is a real sampler
function; the image's placeholder labels (oscillators, LFO, sequencer) map to:
  PRESETS box     -> instrument stepper (prev / name / next)      gear       -> format readout
  OSC 1           -> PITCH: transpose, bend range, fine tune      OSC 2      -> VOICE: glide, voices, drive
  MODULATION      -> REVERB: mix, size, damping                   top pills  -> filter type, voice mode, interpolation
  display         -> the active layer (name, root, keys, velocity, hit) and instrument info
  LFO stepper     -> pad bank (which 16 notes the pads play)       FILTER     -> cutoff, resonance
  ENVELOPE        -> attack / decay / sustain / release faders     bottom row -> velocity sensitivity, filter velocity
  SEQUENCER       -> 16 playable pads (light while their note plays), the small fader = pad velocity
MPC's plugin area is 1280 x 628. BROWSE, SETUP and INFO use the same look. The envelope and filter controls adjust
the instrument's own settings (attack/release add time, decay/sustain scale); the labels say so.

The parameter list is the VST index order: append only (saved projects store values by index).
"""
import json
import math
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("MPC_VST") or os.path.join(os.path.dirname(HERE), "mpc-vst-plugins")   # sd88me/mpc-vst-plugins checkout
ART = os.path.join(HERE, "art")
TITILLIUM = os.path.join(REPO, "tools", "html_art", "fonts")
MICHROMA = os.path.join(HERE, "fonts", "Michroma-Regular.ttf")   # OFL, fonts/OFL-Michroma.txt
W, H, Y_OFF = 1280, 628, 86
SS = 3

# the panel image's palette
BG = (16, 19, 24)             # outside the body
BODY_TOP, BODY_BOT = (44, 51, 61), (30, 35, 43)
PANEL = (33, 38, 46)          # inner boxes (pages other than PLAY)
EDGE = (62, 71, 84)           # borders, dividers
DARK = (19, 23, 29)           # wells, readout boxes
CHIP = (40, 47, 57)
KNOB_IN = (24, 27, 32)
INK = (222, 230, 238)
INK_DIM = (150, 162, 176)
CYAN = (88, 222, 246)
BLUE = (70, 132, 255)
VIOLET = (160, 112, 255)
ORANGE = (255, 150, 80)
GREEN = (120, 220, 150)
YELLOW = (255, 196, 92)
RED = (255, 110, 110)
SLOT_COLOURS = [CYAN, VIOLET, ORANGE, GREEN]

BROWSER_ROWS = 10
WAVE_BARS = 48
SOUND_KEYS = {"volume", "pan", "transpose", "tune", "cutoff", "resonance", "filter_type", "filter_vel", "attack", "decay",
              "sustain", "release", "vel_sens", "bend", "polyphony", "voice_mode", "glide", "interp", "rev_mix", "rev_size",
              "rev_damp", "drive"} | {"lfo%d_%s" % (i, k) for i in (1, 2) for k in ("wave", "rate", "sync", "retrig")} | \
             {"mod%d_%s" % (i, k) for i in range(1, 9) for k in ("src", "dst", "amt")}
LFO_WAVES = ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H"]
LFO_SYNC = ["Free", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4 T", "1/8 T", "1/16 T", "1/4 .", "1/8 .",
            "1/16 ."]
# the engine's ModSrc / ModDst enums (src/engine/sampler.hpp), in order
MOD_SRC = ["Off", "LFO 1", "LFO 2", "Mod Wheel", "Aftertouch", "Pitch Bend", "Velocity", "Key", "Random", "Amp Env"]
MOD_DST = ["Off", "Pitch", "Cutoff", "Resonance", "Volume", "Pan", "Sample Start", "Drive", "Reverb Mix", "LFO 1 Rate", "LFO 2 Rate"]
# plugin.cpp NUM_PARAMS defaults: ready-made routings, all at amount 0
MOD_DEFAULTS = [(1, 1), (2, 2), (3, 2), (6, 2), (8, 5), (7, 2), (4, 4), (1, 4)]


# ---------------------------------------------------------------------------------------------------------------
# parameters

def params():
    P = []

    def add(key, name, **kw):
        d = {"key": key, "name": name}
        d.update(kw)
        P.append(d)

    def num(key, name, lo, hi, default, unit="", **kw):
        add(key, name, min=lo, max=hi, default=default, unit=unit, display="int", **kw)

    def text(key, name, live=True):
        d = dict(min=0, max=0, default=0, display="string", type="readout")
        if live:
            d["live"] = True
        add(key, name, **d)

    def trigger(key, name, **kw):
        add(key, name, min=0, max=1, default=0, momentary=True, hold_ms=250, **kw)

    # 0-23: the engine's numeric settings (plugin.cpp NUM_PARAMS, same units)
    num("volume", "Volume", -48, 12, 0, "dB")
    num("pan", "Pan", -100, 100, 0)
    num("transpose", "Transpose", -24, 24, 0, "st")
    num("tune", "Fine Tune", -100, 100, 0, "ct")
    add("cutoff", "Cutoff", min=0, max=100, default=100, unit="", dynamic_display=True)
    num("resonance", "Resonance", 0, 100, 0, "%")
    add("filter_type", "Filter Type", options=["LP", "HP", "BP"], default=0)
    num("filter_vel", "Filter Velocity", 0, 100, 0, "%")
    num("attack", "Attack (+ms)", 0, 2000, 0, "ms")
    num("decay", "Decay (scale)", 0, 200, 100, "%")
    num("sustain", "Sustain (scale)", 0, 100, 100, "%")
    num("release", "Release (+ms)", 0, 5000, 0, "ms")
    num("vel_sens", "Velocity Sensitivity", 0, 100, 100, "%")
    add("bend", "Bend Range", min=0, max=24, default=0, display="int", dynamic_display=True)
    num("polyphony", "Polyphony", 1, 64, 48)
    add("voice_mode", "Voice Mode", options=["Poly", "Mono", "Legato"], default=0)
    num("glide", "Glide", 0, 2000, 0, "ms")
    add("interp", "Interpolation", options=["None", "Linear", "Cubic"], default=2)
    num("rev_mix", "Reverb Mix", 0, 100, 0, "%")
    num("rev_size", "Reverb Size", 0, 100, 60, "%")
    num("rev_damp", "Reverb Damping", 0, 100, 40, "%")
    num("drive", "Drive", 0, 100, 0, "%")
    num("mem_limit", "Memory Limit", 64, 1536, 512, "MB")
    add("prog_change", "Program Change", options=["Off", "On"], default=1)
    # programs and status
    trigger("prog_prev", "Previous Instrument")
    trigger("prog_next", "Next Instrument")
    text("prog_name", "Instrument")
    text("prog_info", "Instrument Info")
    text("prog_format", "Format")
    text("status", "Status")
    for k, n in (("name", "Active Layer"), ("root", "Layer Root"), ("keys", "Layer Keys"), ("vel", "Layer Velocity"),
                 ("hit", "Last Velocity")):
        text("layer_" + k, n)
    # the browser
    for r in range(1, BROWSER_ROWS + 1):
        add("br_%d" % r, "Browser Row %d" % r, min=0, max=1, default=0, display="string", live=True)
    for k, n in (("prev", "Browser Page Up"), ("next", "Browser Page Down"), ("up", "Browser Up"),
                 ("drives", "Browser Drives"), ("library", "Browser Library"), ("refresh", "Browser Refresh")):
        trigger("br_" + k, n)
    for k, n in (("loc", "Browser Folder"), ("page", "Browser Page"), ("info", "Browser Info")):
        text("br_" + k, n)
    # 1.2: the PLAY page's pads (each plays one note; lit while that note is down), their velocity and bank
    for p in range(1, 17):
        trigger("pad_%d" % p, "Pad %d" % p, live=True)
    num("pad_vel", "Pad Velocity", 1, 127, 100)
    num("pad_base", "Pad Bank Note", 0, 112, 36)
    trigger("pad_down", "Pad Bank Down")
    trigger("pad_up", "Pad Bank Up")
    text("pad_info", "Pad Bank")
    # 1.3: extraction, LFOs and the modulation matrix
    add("auto_extract", "Auto Extract", options=["Off", "Disk Images"], default=1)
    trigger("br_extract", "Extract To Library")
    for i, (wave, rate) in enumerate(((0, 50), (1, 35))):
        n = i + 1
        add("lfo%d_wave" % n, "LFO %d Wave" % n, options=LFO_WAVES, default=wave)
        add("lfo%d_rate" % n, "LFO %d Rate" % n, min=0, max=100, default=rate, dynamic_display=True)
        add("lfo%d_sync" % n, "LFO %d Sync" % n, options=LFO_SYNC, default=0)
        add("lfo%d_retrig" % n, "LFO %d Retrigger" % n, options=["Free", "Note"], default=0)
    for i in range(8):
        n = i + 1
        add("mod%d_src" % n, "Mod %d Source" % n, options=MOD_SRC, default=MOD_DEFAULTS[i][0])
        add("mod%d_dst" % n, "Mod %d Destination" % n, options=MOD_DST, default=MOD_DEFAULTS[i][1])
        num("mod%d_amt" % n, "Mod %d Amount" % n, -100, 100, 0, "%")
    # 1.4: instrument slots and layering, auto loop, displays, library location
    add("target_slot", "Target Slot", options=["A", "B", "C", "D"], default=0)
    add("layer_mode", "Layer Mode", options=["Layer", "Keyswitch"], default=0)
    add("ks_base", "Keyswitch Base", min=0, max=124, default=24, display="int", dynamic_display=True)
    add("auto_loop", "Auto Loop", options=["Off", "On"], default=0)
    for i in range(1, 5):
        L_ = "ABCD"[i - 1]
        add("slot%d_lo" % i, "Slot %s Low Key" % L_, min=0, max=127, default=0, display="int", dynamic_display=True, live=True)
        add("slot%d_hi" % i, "Slot %s High Key" % L_, min=0, max=127, default=127, display="int", dynamic_display=True, live=True)
        add("slot%d_vol" % i, "Slot %s Volume" % L_, min=-48, max=12, default=0, display="int", dynamic_display=True)
        add("slot%d_tune" % i, "Slot %s Transpose" % L_, min=-24, max=24, default=0, display="int", dynamic_display=True)
        add("slot%d_mute" % i, "Slot %s Mute" % L_, options=["Play", "Mute"], default=0)
        text("slot%d_name" % i, "Slot %s Instrument" % L_)
    trigger("slot_clear", "Clear Slot")
    trigger("auto_split", "Auto Split")
    trigger("br_setlib", "Set Library Here")
    text("lib_path", "Library Location")
    for b in range(1, WAVE_BARS + 1):
        add("wave_%d" % b, "Wave %d" % b, min=0, max=127, default=0, display="int", live=True)
    for t in range(1, 33):
        add("ptile_%d" % t, "Zone Map %d" % t, min=0, max=2, default=0, display="int", live=True)
    for t in range(1, 33):
        add("ltile_%d" % t, "Layer Map %d" % t, min=0, max=15, default=0, display="int", live=True)
    # 1.5: patches (every loaded slot, with the slot and sound settings, saved as one .omnipatch); appended last so the
    # earlier parameters keep their VST indices
    trigger("patch_save", "Save Patch")
    # 1.5.1: PANIC (header, every page): silences every voice and clears held notes, pedal and pads
    trigger("panic", "Panic")
    # settings an extracted instrument restores reach MPC's controls through the live polling
    for p in P:
        if p["key"] in SOUND_KEYS:
            p["live"] = True
    return {"name": "Omni Sampler", "params": P}


# ---------------------------------------------------------------------------------------------------------------
# drawing

def font(size, weight="Regular"):
    if weight == "Title":
        return ImageFont.truetype(MICHROMA, int(size * SS))
    return ImageFont.truetype(os.path.join(TITILLIUM, "TitilliumWeb-%s.ttf" % weight), int(size * SS))


def rgba(c, a=255):
    return tuple(c[:3]) + (a,)


class Canvas:
    def __init__(self, w, h, fill=(0, 0, 0, 0)):
        self.w, self.h = w, h
        self.im = Image.new("RGBA", (w * SS, h * SS), fill)
        self.d = ImageDraw.Draw(self.im)

    def S(self, *v):
        return [round(x * SS) for x in v]

    def rrect(self, x, y, w, h, r, fill=None, outline=None, width=1):
        self.d.rounded_rectangle(self.S(x, y, x + w, y + h), radius=r * SS, fill=fill, outline=outline,
                                 width=max(1, round(width * SS)) if outline else 0)

    def grad(self, x, y, w, h, r, top, bottom, outline=None, width=1):
        g = Image.new("RGBA", (max(1, round(w * SS)), max(1, round(h * SS))))
        gd = ImageDraw.Draw(g)
        for j in range(g.height):
            t = j / max(1, g.height - 1)
            gd.line([(0, j), (g.width, j)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top[:3], bottom[:3])) + (255,))
        m = Image.new("L", g.size, 0)
        ImageDraw.Draw(m).rounded_rectangle([0, 0, g.width - 1, g.height - 1], radius=r * SS, fill=255)
        self.im.paste(g, (round(x * SS), round(y * SS)), m)
        if outline:
            self.rrect(x, y, w, h, r, outline=outline, width=width)

    def circle(self, cx, cy, r, fill=None, outline=None, width=1):
        self.d.ellipse(self.S(cx - r, cy - r, cx + r, cy + r), fill=fill, outline=outline,
                       width=max(1, round(width * SS)) if outline else 0)

    def arc(self, cx, cy, r, a0, a1, fill, width):
        self.d.arc(self.S(cx - r, cy - r, cx + r, cy + r), a0, a1, fill=fill, width=max(1, round(width * SS)))

    def line(self, pts, fill, width=1):
        self.d.line([round(v * SS) for p in pts for v in p], fill=fill, width=max(1, round(width * SS)), joint="curve")

    def text(self, x, y, s, size, fill=INK, weight="Regular", anchor="mm", spacing=0):
        if not spacing:
            self.d.text((x * SS, y * SS), s, font=font(size, weight), fill=fill, anchor=anchor)
            return
        f = font(size, weight)
        widths = [f.getlength(ch) + spacing * SS for ch in s]
        total = sum(widths) - spacing * SS
        sx = x * SS - (total / 2 if anchor[0] == "m" else total if anchor[0] == "r" else 0)
        for ch, wd in zip(s, widths):
            self.d.text((sx, y * SS), ch, font=f, fill=fill, anchor="l" + anchor[1])
            sx += wd

    def glow(self, draw, blur, strength=1.0):
        """Draw something (draw(canvas)) on a layer, blur it into a glow under a sharp copy."""
        layer = Canvas(self.w, self.h)
        draw(layer)
        g = layer.im.filter(ImageFilter.GaussianBlur(blur * SS))
        if strength != 1.0:
            a = g.getchannel("A").point(lambda v: min(255, int(v * strength)))
            g.putalpha(a)
        self.im.alpha_composite(g)
        self.im.alpha_composite(layer.im)

    def save(self, name):
        self.im.resize((self.w, self.h), Image.LANCZOS).save(os.path.join(ART, name + ".png"))


def body(c, x, y, w, h, r=14):
    """The brushed panel: a vertical gradient, faint horizontal grain, a light top edge and a dark border."""
    c.grad(x, y, w, h, r, BODY_TOP, BODY_BOT)
    grain = Image.new("RGBA", c.im.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(grain)
    for j in range(0, round(h * SS), 3):
        a = 6 + (j * 7919 % 9)
        gd.line([(round(x * SS) + 8, round(y * SS) + j), (round((x + w) * SS) - 8, round(y * SS) + j)], fill=(255, 255, 255, a))
    m = Image.new("L", c.im.size, 0)
    ImageDraw.Draw(m).rounded_rectangle(c.S(x, y, x + w, y + h), radius=r * SS, fill=255)
    c.im.paste(Image.composite(grain, Image.new("RGBA", c.im.size, (0, 0, 0, 0)), m), (0, 0), grain)
    c.rrect(x, y, w, h, r, outline=(70, 80, 95), width=1.5)
    c.line([(x + r, y + 1.5), (x + w - r, y + 1.5)], (120, 132, 150, 120), 1)


def chip(c, cx, cy, label, colour=CYAN):
    f = font(12, "SemiBold")
    tw = f.getlength(label) / SS
    c.rrect(cx - tw / 2 - 9, cy - 11, tw + 18, 22, 5, fill=(18, 40, 58), outline=rgba(colour, 200), width=1.2)
    c.text(cx, cy + 1, label, 12, colour, "SemiBold")


def section_title(c, cx, cy, title, tag=None, tag_x=None):
    c.text(cx, cy, title, 20, INK, "SemiBold")
    if tag:
        chip(c, tag_x, cy, tag)


def title_glow(c, cx, cy, text, size):
    c.glow(lambda l: l.text(cx, cy, text, size, CYAN, "Title", spacing=3), 6, 1.6)


def topbar(c, left_label=None):
    c.grad(0, 0, W, 56, 0, (26, 31, 38), (17, 20, 25))
    c.line([(0, 56), (W, 56)], (58, 66, 78), 1)
    title_glow(c, W / 2, 29, "OMNI SAMPLER", 30)
    # format readout box (where the image has its gear), then PANIC (1.5.1)
    c.rrect(946, 10, 220, 36, 7, fill=DARK, outline=(64, 74, 88), width=1.2)
    if left_label:
        c.rrect(16, 10, 300, 36, 7, fill=(34, 40, 49), outline=(64, 74, 88), width=1.2)
        for k in range(3):
            c.line([(30, 21 + k * 7), (46, 21 + k * 7)], INK_DIM, 2)
        c.text(60, 28, left_label, 16, INK, "SemiBold", "lm")


def panel(c, x, y, w, h, title=None, title_y=None):
    body(c, x, y, w, h, 12)
    if title:
        c.text(x + w / 2, title_y or y + 26, title, 19, INK, "SemiBold")


def knob_label(c, cx, cy, label):
    c.text(cx, cy, label, 13, INK_DIM, "SemiBold")


WAVE_X0, WAVE_STEP, WAVE_CY, WAVE_W, WAVE_H = 376, 11, 228, 8, 120   # bars: x of the first, spacing, centre line, size
PTILE_X0, PTILE_STEP, PTILE_CY, PTILE_W = 368, 17.5, 330, 16


def display_panel(c, x, y, w, h):
    """The centre display: dark glass, a centre line and grid for the live waveform bars, a keyboard under the zone map."""
    c.grad(x, y, w, h, 10, (30, 40, 52), (18, 24, 32), outline=(70, 84, 102), width=1.5)
    for k in range(1, 8):
        c.line([(x + 14 + k * (w - 28) / 8, WAVE_CY - WAVE_H / 2), (x + 14 + k * (w - 28) / 8, WAVE_CY + WAVE_H / 2)], (60, 80, 100, 70), 1)
    c.line([(x + 14, WAVE_CY), (x + w - 14, WAVE_CY)], (120, 200, 240, 90), 1)
    c.text(x + 22, WAVE_CY - WAVE_H / 2 - 6, "WAVE", 11, (110, 150, 180), "SemiBold", "lm")
    # a keyboard under the zone tiles: 32 tiles x 4 keys
    kx0, ky = PTILE_X0, PTILE_CY + 11
    kw = PTILE_STEP * 32
    c.rrect(kx0 - 1, ky, kw + 2, 14, 2, fill=(200, 206, 214))
    for key in range(128):
        px = kx0 + key * kw / 128
        if key % 12 in (1, 3, 6, 8, 10):
            c.rrect(px, ky, kw / 128, 8, 0, fill=(20, 24, 30))
        if key % 12 == 0:
            c.line([(px, ky), (px, ky + 14)], (120, 128, 140), 1)
    c.text(x + 22, PTILE_CY, "KEYS", 11, (110, 150, 180), "SemiBold", "lm")


def wave_display(c, x, y, w, h):
    """The centre display: a dark glass panel with layered, glowing sample-like waves (orange -> cyan -> violet)."""
    c.grad(x, y, w, h, 10, (34, 46, 60), (20, 27, 36), outline=(70, 84, 102), width=1.5)
    mid = y + h * 0.58
    stops = [(0.0, ORANGE), (0.35, (120, 200, 255)), (0.7, CYAN), (1.0, VIOLET)]

    def colour_at(t):
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t <= t1:
                k = (t - t0) / (t1 - t0)
                return tuple(round(a + (b - a) * k) for a, b in zip(c0, c1))
        return stops[-1][1]

    def wave(l, phase, amp, alpha, width):
        n = 220
        pts = []
        for i in range(n + 1):
            t = i / n
            env = math.sin(math.pi * t) ** 0.8
            v = (math.sin(t * 14 + phase) * 0.55 + math.sin(t * 31 + phase * 1.7) * 0.25 + math.sin(t * 6 - phase) * 0.35)
            pts.append((x + 14 + t * (w - 28), mid - v * amp * env))
        for i in range(n):
            col = colour_at(i / n)
            l.line([pts[i], pts[i + 1]], rgba(col, alpha), width)

    def draw(l):
        for k in range(12):
            wave(l, k * 0.23, h * 0.30 * (0.55 + 0.45 * math.cos(k * 0.5)), 70, 1)
        wave(l, 0.4, h * 0.34, 235, 2.6)
        wave(l, 2.2, h * 0.24, 200, 2)
    c.glow(draw, 5, 1.3)
    c.line([(x + 14, mid), (x + w - 14, mid)], (140, 220, 255, 90), 1)


def arrow_glyph(c, cx, cy, direction, colour, a=7):
    pts = {"left": [(cx + a * .6, cy - a), (cx - a * .6, cy), (cx + a * .6, cy + a)],
           "right": [(cx - a * .6, cy - a), (cx + a * .6, cy), (cx - a * .6, cy + a)],
           "up": [(cx - a, cy + a * .6), (cx, cy - a * .6), (cx + a, cy + a * .6)],
           "down": [(cx - a, cy - a * .6), (cx, cy + a * .6), (cx + a, cy - a * .6)]}[direction]
    c.d.polygon([(round(px * SS), round(py * SS)) for px, py in pts], fill=colour)


# ---------------------------------------------------------------------------------------------------------------
# PLAY page geometry (plugin coordinates)

BODY = (8, 62, 1264, 490)
LEFT_X, CENTER_X, RIGHT_X = (20, 336), (344, 936), (944, 1260)
# knobs: cx, cy, r, key, label, colour
PLAY_KNOBS = [
    (92, 150, 28, "transpose", "TRANSPOSE", BLUE), (178, 150, 20, "bend", "BEND", CYAN),
    (264, 150, 28, "tune", "FINE TUNE", VIOLET),
    (92, 312, 26, "glide", "GLIDE", BLUE), (178, 312, 26, "polyphony", "VOICES", CYAN),
    (264, 312, 26, "drive", "DRIVE", ORANGE),
    (92, 470, 26, "rev_mix", "MIX", VIOLET), (178, 470, 26, "rev_size", "SIZE", VIOLET),
    (264, 470, 26, "rev_damp", "DAMP", VIOLET),
    (1030, 150, 30, "cutoff", "CUTOFF", CYAN), (1182, 150, 30, "resonance", "RESONANCE", ORANGE),
    (1030, 500, 22, "vel_sens", "VELOCITY", BLUE), (1182, 500, 22, "filter_vel", "FILTER VEL", CYAN),
    (384, 500, 22, "volume", "VOLUME", CYAN), (896, 500, 22, "pan", "PAN", CYAN),
]
FADERS = [(996, "attack", "ATTACK +"), (1068, "decay", "DECAY %"), (1140, "sustain", "SUSTAIN %"), (1212, "release", "RELEASE +")]
FADER_CY, FADER_W, FADER_H = 346, 34, 124
SEG_GROUPS = [("filter_type", ["LP", "HP", "BP"], 360), ("voice_mode", ["Poly", "Mono", "Legato"], 570),
              ("interp", ["None", "Linear", "Cubic"], 780)]
SEG_W, SEG_H, SEG_Y = 58, 30, 84
DISPLAY = (352, 106, 576, 334)
CHIPS = [("layer_root", 372, 112), ("layer_keys", 492, 176), ("layer_vel", 676, 126), ("layer_hit", 810, 100)]
PADS_Y, PAD_W, PAD_H = 590, 70, 50
PAD_X0, PAD_STEP = 86, 74.5


def strip_knob(name, colour, frames=65, size=96):
    """A knob filmstrip, minimum first: dark ring track, the value arc in colour (with glow), black cap, white
    pointer. 270 degrees of travel from 135 (lower left) clockwise."""
    im = Image.new("RGBA", (size, size * frames), (0, 0, 0, 0))
    for f in range(frames):
        t = f / (frames - 1)
        c = Canvas(size, size)
        cx = cy = size / 2
        ring_r, ring_w = size * 0.42, size * 0.07
        c.arc(cx, cy, ring_r, 135, 405, (12, 15, 20, 255), ring_w + 2)
        c.arc(cx, cy, ring_r, 135, 405, (40, 47, 58, 255), ring_w * 0.6)
        if t > 0.004:
            end = 135 + 270 * t
            c.glow(lambda l: l.arc(cx, cy, ring_r, 135, end, rgba(colour), ring_w), 2.2, 1.3)
        c.circle(cx, cy, size * 0.33, fill=(10, 12, 15, 255))
        c.circle(cx, cy, size * 0.31, fill=(26, 29, 35, 255))
        c.circle(cx, cy - size * 0.05, size * 0.22, fill=(36, 40, 47, 120))
        a = math.radians(135 + 270 * t)
        p0 = (cx + math.cos(a) * size * 0.10, cy + math.sin(a) * size * 0.10)
        p1 = (cx + math.cos(a) * size * 0.27, cy + math.sin(a) * size * 0.27)
        c.line([p0, p1], (235, 240, 245, 255), size * 0.035)
        frame = c.im.resize((size, size), Image.LANCZOS)
        im.paste(frame, (0, f * size))
    im.save(os.path.join(ART, name + ".png"))


def strip_fader(name, w, h, frames=65):
    """A vertical fader filmstrip (minimum first): a dark slot, a glowing blue fill up to the thumb, a metal thumb."""
    im = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    top, bot = 10, h - 10
    for f in range(frames):
        t = f / (frames - 1)
        c = Canvas(w, h)
        sx = w / 2
        c.rrect(sx - 4, top - 4, 8, bot - top + 8, 4, fill=(10, 12, 16, 255))
        ty = bot - (bot - top) * t
        if t > 0.004:
            c.glow(lambda l: l.rrect(sx - 3, ty, 6, bot - ty + 3, 3, fill=rgba((80, 150, 255))), 2.5, 1.2)
        c.grad(sx - w * 0.42, ty - 8, w * 0.84, 16, 3, (150, 165, 185), (70, 80, 96), outline=(20, 24, 30), width=1)
        c.line([(sx - w * 0.3, ty), (sx + w * 0.3, ty)], CYAN, 1.6)
        im.paste(c.im.resize((w, h), Image.LANCZOS), (0, f * h))
    im.save(os.path.join(ART, name + ".png"))


def strip_wave(name, w, h, frames=128):
    """A waveform bar, symmetric about the centre: height = value (minimum first)."""
    im = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for f in range(frames):
        t = f / (frames - 1)
        c = Canvas(w, h)
        bh = max(1.0, t * (h - 4))
        top = (h - bh) / 2
        col = tuple(round(a + (b - a) * t) for a, b in zip((60, 140, 255), CYAN))
        c.rrect(1, top, w - 2, bh, 2, fill=rgba(col, 235))
        im.paste(c.im.resize((w, h), Image.LANCZOS), (0, f * h))
    im.save(os.path.join(ART, name + ".png"))


def strip_states(name, w, h, states, draw, frames=128):
    """A tile with `states` looks for a 0..states-1 parameter: frame f shows state round(f * (states-1) / 127)."""
    im = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    cache = {}
    for f in range(frames):
        st = round(f * (states - 1) / (frames - 1))
        if st not in cache:
            c = Canvas(w, h)
            draw(c, st)
            cache[st] = c.im.resize((w, h), Image.LANCZOS)
        im.paste(cache[st], (0, f * h))
    im.save(os.path.join(ART, name + ".png"))


def draw_ptile(c, st):   # 0 no zone, 1 zones here, 2 the zone just played
    w, h = c.w, c.h
    if st == 0:
        c.rrect(1, 1, w - 2, h - 2, 3, outline=(50, 64, 80), width=1)
    elif st == 1:
        c.rrect(1, 1, w - 2, h - 2, 3, fill=(40, 90, 170), outline=(80, 140, 220), width=1)
    else:
        c.glow(lambda l: l.rrect(1, 1, w - 2, h - 2, 3, fill=rgba(CYAN)), 1.5, 1.2)


def draw_ltile(c, st):   # bit s = slot s covers these keys: one coloured lane per slot
    w, h = c.w, c.h
    lane = (h - 2) / 4
    for s_ in range(4):
        y0 = 1 + s_ * lane
        if st >> s_ & 1:
            c.rrect(1, y0 + 1, w - 2, lane - 2, 2, fill=rgba(SLOT_COLOURS[s_]))
        else:
            c.rrect(1, y0 + 1, w - 2, lane - 2, 2, fill=(26, 30, 37), outline=(40, 46, 56), width=1)


def bg_play():
    c = Canvas(W, H, BG + (255,))
    topbar(c)
    # presets box: the stepper buttons and name sit on it
    c.rrect(16, 10, 376, 36, 7, fill=(34, 40, 49), outline=(64, 74, 88), width=1.2)
    for k in range(3):
        c.line([(28, 21 + k * 7), (42, 21 + k * 7)], INK_DIM, 2)
    body(c, *BODY)
    bx, by, bw, bh = BODY
    for xv in (LEFT_X[1] + 4, CENTER_X[1] + 4):
        c.line([(xv, by + 14), (xv, by + bh - 14)], (12, 15, 19, 200), 1.5)
        c.line([(xv + 1.5, by + 14), (xv + 1.5, by + bh - 14)], (80, 90, 104, 90), 1)
    for (x0, x1), yv in (((LEFT_X[0], LEFT_X[1]), 228), ((LEFT_X[0], LEFT_X[1]), 390), ((RIGHT_X[0], RIGHT_X[1]), 228),
                         ((RIGHT_X[0], RIGHT_X[1]), 446), ((CENTER_X[0] + 8, CENTER_X[1] - 8), 450)):
        c.line([(x0 + 6, yv), (x1 - 6, yv)], (12, 15, 19, 200), 1.5)
        c.line([(x0 + 6, yv + 1.5), (x1 - 6, yv + 1.5)], (80, 90, 104, 80), 1)
    # section titles with their tags (the image's OSC 1 / OSC 2 / LFO 3 chips)
    section_title(c, 150, 84, "PITCH", "TUNE", 290)
    section_title(c, 150, 248, "VOICE", "POLY", 290)
    section_title(c, 150, 408, "REVERB", "FX", 290)
    section_title(c, 1102, 84, "FILTER")
    section_title(c, 1102, 248, "ENVELOPE")
    for cx, cy, r, key, label, col in PLAY_KNOBS:
        knob_label(c, cx, cy - r - 16, label)
    for fx, key, label in FADERS:
        knob_label(c, fx, FADER_CY - FADER_H / 2 - 14, label)
    # segment groups' wells
    for key, opts, x0 in SEG_GROUPS:
        gw = len(opts) * SEG_W + 6
        c.rrect(x0 - 3, SEG_Y - SEG_H / 2 - 3, gw, SEG_H + 6, 9, fill=(14, 17, 22), outline=(66, 76, 90), width=1.2)
    display_panel(c, *DISPLAY)
    dx, dy, dw, dh = DISPLAY
    for _, x0, cw in CHIPS:
        c.rrect(x0, dy + dh - 42, cw, 28, 6, fill=(14, 20, 28, 210), outline=(70, 110, 140, 160), width=1)
    # below the display: status box, pad bank stepper, pad velocity box
    c.rrect(422, 484, 140, 32, 7, fill=DARK, outline=(64, 74, 88), width=1.2)
    c.text(492, 470, "STATUS", 12, INK_DIM, "SemiBold")
    c.rrect(574, 484, 196, 32, 7, fill=DARK, outline=(64, 74, 88), width=1.2)
    c.text(672, 470, "PAD BANK", 12, INK_DIM, "SemiBold")
    c.rrect(780, 484, 76, 32, 7, fill=DARK, outline=(64, 74, 88), width=1.2)
    c.text(818, 470, "PAD VEL", 12, INK_DIM, "SemiBold")
    # pad row
    c.grad(8, 560, 1264, 66, 12, (28, 33, 40), (18, 21, 26), outline=(58, 66, 78), width=1.2)
    c.rrect(22, 566, 40, 54, 6, fill=(14, 17, 22), outline=(58, 66, 78), width=1)
    c.save("bg_play")


def pad_image(name, lit, tint):
    w, h = PAD_W, PAD_H
    c = Canvas(w + 12, h + 12)
    if lit:
        c.glow(lambda l: l.rrect(6, 6, w, h, 7, fill=rgba(CYAN, 200)), 4, 1.4)
        c.grad(6, 6, w, h, 7, (150, 240, 255), (60, 170, 230), outline=(200, 250, 255), width=1.5)
    else:
        top = tuple(round(a * 0.55 + b * 0.45) for a, b in zip(tint, (30, 36, 46)))
        c.grad(6, 6, w, h, 7, top, (22, 26, 34), outline=(84, 100, 124), width=1.2)
        c.line([(14, 8), (w - 2, 8)], (255, 255, 255, 40), 1)
    c.save(name + ("_on" if lit else "_off"))


def seg_image(name, w, h, lit, label, accent=CYAN):
    c = Canvas(w, h)
    if lit:
        c.grad(1, 1, w - 2, h - 2, 7, (44, 110, 160), (22, 60, 98), outline=rgba(accent), width=1.4)
        c.text(w / 2, h / 2 + 1, label, 13, (210, 246, 255), "Bold")
    else:
        c.grad(1, 1, w - 2, h - 2, 7, (44, 51, 61), (30, 35, 43))
        c.text(w / 2, h / 2 + 1, label, 13, INK_DIM, "SemiBold")
    c.save(name + ("_on" if lit else "_off"))


def opt_name(key, opt):
    return "opt_%s_%s" % (key, "".join(ch if ch.isalnum() else "_" for ch in opt.lower()))


def arrow_image(name, w, h, direction, lit, colour=BLUE):
    c = Canvas(w, h)
    if lit:
        c.grad(1, 1, w - 2, h - 2, 7, tuple(min(255, v + 40) for v in colour), colour, outline=(220, 240, 255), width=1.4)
    else:
        c.grad(1, 1, w - 2, h - 2, 7, tuple(round(v * 0.75) for v in colour), tuple(round(v * 0.45) for v in colour),
               outline=(20, 24, 30), width=1)
    arrow_glyph(c, w / 2, h / 2, direction, (240, 248, 255))
    c.save(name + ("_on" if lit else "_off"))


def button_image(name, w, h, label, lit, accent=CYAN, size=15):
    c = Canvas(w, h)
    if lit:
        c.grad(1, 1, w - 2, h - 2, 8, (44, 110, 160), (22, 60, 98), outline=rgba(accent), width=1.5)
    else:
        c.grad(1, 1, w - 2, h - 2, 8, (48, 56, 68), (32, 38, 46), outline=(70, 80, 95), width=1.2)
    if label:
        c.text(w / 2, h / 2 + 1, label, size, (220, 246, 255) if lit else INK, "SemiBold")
    c.save(name + ("_on" if lit else "_off"))


# ---------------------------------------------------------------------------------------------------------------
# BROWSE / SETUP / INFO

BR_LIST = (16, 66, 740, 554)
BR_SIDE = (768, 66, 496, 554)


def bg_browse():
    c = Canvas(W, H, BG + (255,))
    topbar(c, "BROWSE")
    x, y, w, h = BR_LIST
    panel(c, x, y, w, h)
    c.rrect(x + 14, y + 12, w - 28, 34, 7, fill=DARK, outline=(64, 74, 88), width=1)
    c.rrect(x + 14, y + h - 46, w - 28, 34, 7, fill=DARK)
    x, y, w, h = BR_SIDE
    panel(c, x, y, w, h, "LOCATIONS", y + 24)
    c.rrect(x + 16, y + 182, w - 32, 26, 6, fill=DARK)
    c.text(x + w / 2, y + 226, "SELECTED", 13, INK_DIM, "SemiBold")
    c.rrect(x + 16, y + 238, w - 32, 62, 8, fill=DARK, outline=(64, 74, 88), width=1)
    c.text(x + 24, y + 324, "LOAD INTO SLOT", 13, INK_DIM, "SemiBold", "lm")
    c.rrect(x + 16, y + 342, w - 32, 106, 8, fill=DARK, outline=(64, 74, 88), width=1)
    c.text(x + 368, y + 472, "AUTO EXTRACT", 12, INK_DIM, "SemiBold")
    c.text(x + w / 2, y + 538, "Presets from disk images are saved to the Plugin Library", 12, INK_DIM, "Regular")
    c.save("bg_browse")


SETUP_PANELS = [("VOICE", (16, 66, 616, 300)), ("OUTPUT", (648, 66, 616, 300)), ("PLAYBACK", (16, 380, 616, 240)),
                ("EFFECTS & SYSTEM", (648, 380, 616, 240))]
SETUP_KNOBS = [(150, 190, 34, "polyphony", "VOICES", CYAN), (330, 190, 34, "glide", "GLIDE (MS)", BLUE),
               (510, 190, 34, "bend", "BEND RANGE", CYAN),
               (730, 210, 34, "volume", "VOLUME (DB)", CYAN), (875, 210, 34, "pan", "PAN", CYAN),
               (1020, 210, 34, "transpose", "TRANSPOSE (ST)", BLUE), (1165, 210, 34, "tune", "FINE TUNE (CT)", VIOLET),
               (110, 500, 34, "vel_sens", "VELOCITY SENS %", BLUE), (250, 500, 34, "filter_vel", "FILTER VEL %", CYAN),
               (740, 500, 34, "rev_damp", "REVERB DAMP %", VIOLET), (880, 500, 34, "mem_limit", "MEMORY (MB)", ORANGE),
               (1180, 500, 34, "pad_base", "PAD BANK NOTE", CYAN)]
SETUP_OPTIONS = [("voice_mode", ["Poly", "Mono", "Legato"], 184, 310, "VOICE MODE"),
                 ("interp", ["None", "Linear", "Cubic"], 340, 480, "INTERPOLATION"),
                 ("auto_loop", ["Off", "On"], 384, 574, "AUTO LOOP"),
                 ("prog_change", ["Off", "On"], 960, 500, "PROGRAM CHANGE")]
OPT_W, OPT_H = 84, 34


def bg_setup():
    c = Canvas(W, H, BG + (255,))
    topbar(c, "SETUP")
    for title, box in SETUP_PANELS:
        panel(c, *box, title=title)
    for cx, cy, r, _, label, _ in SETUP_KNOBS:
        knob_label(c, cx, cy - r - 16, label)
    for key, opts, x0, cy, label in SETUP_OPTIONS:
        wtot = len(opts) * OPT_W + (len(opts) - 1) * 6
        knob_label(c, x0 + wtot / 2, cy - 34, label)
    c.save("bg_setup")


LFO_BOX = [(16, 66), (16, 350)]
LFO_BW, LFO_BH = 400, 270
MX = (432, 66, 832, 554)
MX_ROW0, MX_STEP = 96, 58


def bg_mod():
    c = Canvas(W, H, BG + (255,))
    topbar(c, "MODULATION")
    for i, (x, yy) in enumerate(LFO_BOX):
        panel(c, x, yy, LFO_BW, LFO_BH, "LFO %d" % (i + 1), yy + 26)
        knob_label(c, x + 200, yy + 52, "WAVE")
        knob_label(c, x + 80, yy + 124, "RATE")
        knob_label(c, x + 270, yy + 124, "TEMPO SYNC")
        knob_label(c, x + 270, yy + 200, "RETRIGGER")
    x, yy, w, h = MX
    panel(c, x, yy, w, h, "MOD MATRIX", yy + 26)
    for label, cx in (("SOURCE", x + 200), ("DESTINATION", x + 480), ("AMOUNT", x + 700)):
        knob_label(c, cx, yy + 58, label)
    for i in range(8):
        ry = yy + MX_ROW0 + i * MX_STEP
        c.rrect(x + 14, ry - 24, w - 28, 48, 8, fill=(26, 31, 38, 255), outline=(52, 60, 72), width=1)
        c.text(x + 40, ry, str(i + 1), 18, CYAN, "Bold")
        arrow_glyph(c, x + 340, ry, "right", (120, 200, 240), 9)
    c.save("bg_mod")


SLOT_X0, SLOT_STEP, SLOT_Y, SLOT_W, SLOT_H = 16, 314, 66, 300, 336
LMAP = (16, 412, 1248, 208)
LT_X0, LT_STEP, LT_CY, LT_W, LT_H = 96, 34, 470, 32, 44


def bg_layers():
    c = Canvas(W, H, BG + (255,))
    topbar(c, "LAYERS")
    for i in range(4):
        x = SLOT_X0 + i * SLOT_STEP
        panel(c, x, SLOT_Y, SLOT_W, SLOT_H)
        col = SLOT_COLOURS[i]
        c.glow(lambda l, x=x, col=col: l.rrect(x + 14, SLOT_Y + 12, 40, 36, 8, fill=rgba(col)), 3, 1.1)
        c.text(x + 34, SLOT_Y + 31, "ABCD"[i], 22, BG, "Bold")
        c.rrect(x + 62, SLOT_Y + 14, SLOT_W - 76, 32, 7, fill=DARK, outline=(64, 74, 88), width=1)
        for lx, lab in ((x + 80, "LOW KEY"), (x + 220, "HIGH KEY")):
            knob_label(c, lx, SLOT_Y + 108, lab)
        for lx, lab in ((x + 80, "VOLUME"), (x + 220, "TRANSPOSE")):
            knob_label(c, lx, SLOT_Y + 208, lab)
    x, yy, w, h = LMAP
    panel(c, x, yy, w, h, "LAYER MAP", yy + 22)
    # the keyboard under the 32 lane tiles
    kx0, ky, kw = LT_X0, LT_CY + LT_H / 2 + 6, LT_STEP * 32
    c.rrect(kx0 - 1, ky, kw + 2, 46, 4, fill=(214, 218, 226))
    for key in range(128):
        px = kx0 + key * kw / 128
        if key % 12 in (1, 3, 6, 8, 10):
            c.rrect(px, ky, kw / 128, 28, 1, fill=(18, 22, 28))
        if key % 12 == 0:
            c.line([(px, ky), (px, ky + 46)], (110, 118, 130), 1)
            c.text(px + 2, ky + 40, "C%d" % (key // 12 - 2), 10, (60, 66, 76), "SemiBold", "lm")
    for s_ in range(4):
        c.text(kx0 - 46, LT_CY - LT_H / 2 + 6 + s_ * (LT_H - 2) / 4, "ABCD"[s_], 11, SLOT_COLOURS[s_], "Bold", "lm")
    knob_label(c, 210, yy + 168, "MODE")
    knob_label(c, 512, yy + 168, "KEYSWITCH BASE")
    c.save("bg_layers")


# INFO page: every format, with its file extensions. True = checked against test files, False = not yet.
INFO_PANELS = [
    ("AKAI", (16, 66), [
        ("S1000 / S3000", ".p .p1 .p3 .s .s1 .s3", True),
        ("S900 / S950", ".p9 .s9 .s9c", False),
        ("MESA", ".s3p", False),
        ("S5000 / S6000 / Z4 / Z8", ".akp .akm", False),
        ("MPC keygroup / drum", ".xpm", True),
        ("MPC 2 / 3 JSON, projects", ".xpm .xpj .xty", False),
        ("MPC1000 / 2000 / 3000 / 60", ".pgm .snd .set", False)]),
    ("E-MU", (440, 66), [
        ("EOS / E4 / E-Synth", ".e4b .e4a .eos", True),
        ("Emulator III / IIIX / ESI", ".e3b .e3x .esi .eiii", True),
        ("Emax / Emax II", ".em1 .eb1 .em2 .eb2 .emx", True),
        ("Emulator I", ".ei (or a floppy image)", True),
        ("Emulator II", ".eii (or a floppy image)", True),
        ("Emulator X", ".exb .ebl", True)]),
    ("NATIVE INSTRUMENTS", (864, 66), [
        ("Kontakt 1", ".nki", True),
        ("Kontakt 2 - 4.1", ".nki .nkm", False),
        ("Kontakt 4.2", ".nki .nkm", False),
        ("Kontakt 5 - 8", ".nki .nkm", True),
        ("Kontakt 5+ monoliths", ".nki (samples inside)", True),
        ("Kontakt samples", ".ncw", False),
        ("Maschine 1 / 2 / 3", ".msnd .mxsnd", True)]),
    ("OPEN FORMATS", (16, 350), [
        ("SFZ", ".sfz", True),
        ("SoundFont 2", ".sf2 .sbk", True),
        ("WAV (PCM)", ".wav .wave", True),
        ("WAV float / ADPCM / RF64", ".wav", False),
        ("AIFF / AIFC", ".aif .aiff .aifc", False),
        ("FLAC / Ogg Vorbis", ".flac .ogg", False)]),
    ("DISK IMAGES", (440, 350), [
        ("CD / DVD: ISO 9660, raw BIN", ".iso .bin .img", True),
        ("FAT12/16/32: floppy, ZIP, CF", ".img .ima .dsk .raw", True),
        ("Akai S1000 / S3000 CD + HD", ".iso .img .hda .hds", True),
        ("Akai floppies", ".img .dsk", False),
        ("E-mu EOS / EIII CD + disk", ".iso .img .e4 .e3", True),
        ("Floppy: HFE (E-mu FM, IBM MFM/FM)", ".hfe", True),
        ("Floppy: IMD", ".imd", False)]),
    ("ROLAND", (864, 350), [
        ("S-700 CD / HD: S-750/760/770", ".iso .img .bin", False),
        ("S-700 floppy, SP-700, DJ-70", ".img", False),
        ("S-50/S-550/S-330/W-30 floppy", ".img .out", False),
        ("S-500 CD-ROM (LAND)", ".iso .bin", True)]),
]
INFO_W, INFO_H = 400, 272


def bg_info():
    c = Canvas(W, H, BG + (255,))
    topbar(c, "SUPPORTED FORMATS")
    for title, (x, y), rows in INFO_PANELS:
        panel(c, x, y, INFO_W, INFO_H, title, y + 26)
        for i, (name, exts, ok) in enumerate(rows):
            ry = y + 64 + i * 29
            col = GREEN if ok else YELLOW
            c.glow(lambda l, ry=ry, col=col: l.circle(x + 22, ry, 4.5, fill=rgba(col)), 2, 1.0)
            c.text(x + 36, ry, name, 14, INK, "SemiBold", "lm")
            c.text(x + INFO_W - 16, ry, exts, 13, INK_DIM, "Regular", "rm")
    x, y = 864, 350
    for i, (col, t) in enumerate(((GREEN, "checked against test files"), (YELLOW, "supported, not yet tested on real files"))):
        c.glow(lambda l, i=i, col=col: l.circle(x + 22, y + 192 + i * 22, 4, fill=rgba(col)), 2, 1.0)
        c.text(x + 34, y + 192 + i * 22, t, 12, INK_DIM, "SemiBold", "lm")
    c.text(x + 22, y + 244, "Disk images open like folders; presets played from them", 12, INK_DIM, "Regular", "lm")
    c.text(x + 22, y + 260 - 2, "are saved to the Plugin Library (Extracted).", 12, INK_DIM, "Regular", "lm")
    c.save("bg_info")


def clear_image():
    Image.new("RGBA", (4, 4), (0, 0, 0, 0)).save(os.path.join(ART, "clear.png"))


KNOB_STRIPS = {BLUE: "knob_blue", CYAN: "knob_cyan", VIOLET: "knob_violet", ORANGE: "knob_orange", GREEN: "knob_green"}
PAD_TINTS = [(70, 150, 255), (70, 150, 255), (70, 150, 255), (150, 90, 200)] * 4


def images():
    os.makedirs(ART, exist_ok=True)
    for f in os.listdir(ART):
        if f.endswith(".png"):
            os.remove(os.path.join(ART, f))
    bg_play()
    bg_browse()
    bg_setup()
    bg_info()
    bg_mod()
    bg_layers()
    strip_wave("wave_bar", WAVE_W, WAVE_H)
    strip_states("ptile", PTILE_W, 16, 3, draw_ptile)
    strip_states("ltile", LT_W, LT_H, 16, draw_ltile)
    for col, n in KNOB_STRIPS.items():
        strip_knob(n, col)
    strip_fader("fader", FADER_W, FADER_H)
    strip_fader("fader_small", 30, 50)
    for lit in (False, True):
        for key, opts, _ in SEG_GROUPS:
            for opt in opts:
                seg_image(opt_name(key, opt), SEG_W, SEG_H, lit, opt.upper())
        for key, opts, _, _, _ in SETUP_OPTIONS:
            for opt in opts:
                seg_image("set_" + opt_name(key, opt), OPT_W, OPT_H, lit, opt.upper())
        arrow_image("prog_prev", 38, 30, "left", lit, BLUE)
        arrow_image("prog_next", 38, 30, "right", lit, VIOLET)
        arrow_image("pad_down", 38, 30, "left", lit, BLUE)
        arrow_image("pad_up", 38, 30, "right", lit, VIOLET)
        arrow_image("pg_prev", 70, 34, "up", lit, BLUE)
        arrow_image("pg_next", 70, 34, "down", lit, BLUE)
        for p in range(16):
            pad_image("pad%d" % (p + 1), lit, PAD_TINTS[p])
        for opt in ("Off", "Disk Images"):
            seg_image("set_" + opt_name("auto_extract", opt), 98, 34, lit, opt.upper())
        for i in range(2):
            for opt in LFO_WAVES:
                short = {"Triangle": "TRI", "Saw Up": "SAW UP", "Saw Down": "SAW DN"}.get(opt, opt.upper())
                seg_image(opt_name("lfo%d_wave" % (i + 1), opt), 58, 30, lit, short)
            for opt in ("Free", "Note"):
                seg_image(opt_name("lfo%d_retrig" % (i + 1), opt), 92, 32, lit, opt.upper())
        button_image("b_extract", 220, 40, "SAVE TO LIBRARY", lit)
        button_image("b_setlib", 220, 40, "SET LIBRARY HERE", lit)
        button_image("b_split", 160, 36, "AUTO SPLIT", lit)
        button_image("b_clear", 160, 36, "CLEAR SLOT", lit)
        button_image("b_patch", 160, 36, "SAVE PATCH", lit)
        button_image("b_panic", 92, 36, "PANIC", lit, RED)
        for i in range(4):
            seg_image("slot_target_%d" % i, 50, 30, lit, "ABCD"[i], SLOT_COLOURS[i])
            seg_image("slot_load_%d" % i, 200, 30, lit, "LOADS HERE" if lit else "LOAD HERE", SLOT_COLOURS[i])
            for opt in ("Play", "Mute"):
                seg_image("slot%d_%s" % (i, opt.lower()), 90, 30, lit, opt.upper(), SLOT_COLOURS[i] if opt == "Play" else RED)
        for opt in ("Layer", "Keyswitch"):
            seg_image(opt_name("layer_mode", opt), 120, 32, lit, opt.upper())
        for opt in ("Off", "On"):
            seg_image("set_" + opt_name("auto_loop", opt), OPT_W, OPT_H, lit, opt.upper())
        for key, label in (("drives", "DRIVES"), ("library", "PLUGIN LIBRARY"), ("up", "FOLDER UP"), ("refresh", "REFRESH")):
            button_image("b_" + key, 220, 44, label, lit)
    clear_image()


# ---------------------------------------------------------------------------------------------------------------
# layout

def y(v):
    return v + Y_OFF


def knob_line(cx, cy, r, key, colour, width=92):
    # width: the component (and its value text) box; neighbours' boxes must not overlap, or one takes the other's touch
    return ('knob cx=%d cy=%d r=%d key=%s label="" name=hide strip=art/%s.png width=%d'
            % (cx, y(cy), r, key, KNOB_STRIPS[colour], width))


def header_lines(L):
    L.append('readout cx=1056 cy=%d w=200 h=34 label="" key=prog_format text_size=14' % y(28))
    L.append('button cx=1218 cy=%d label="" key=panic img=art/b_panic_off.png img_on=art/b_panic_on.png w=92 h=36' % y(28))


def layout():
    L = ["# Omni Sampler skin layout (generated by design.py). Plugin-area art is in art/; y = plugin y + 86.",
         "live_font=Roboto", "art_css=skin.css",
         "theme_bg=101318", "theme_ink=dee6ee", "theme_ink_dim=96a2b0", "theme_accent=8fe8ff", "theme_accent_hi=58def6",
         "theme_line=3e4754", "theme_lcd=21262e", "theme_display_ink=8fe8ff", "theme_box=13171d",
         "theme_seg_active=58def6", "theme_seg_inactive=2c333d", "theme_seg_active_tx=101318",
         "theme_knob_face=1a1d23", "theme_knob_ring=0c0f14", "theme_knob_dot=58def6",
         "qlinks_track = attack,decay,sustain,release,cutoff,resonance,filter_vel,vel_sens,transpose,bend,tune,glide,drive,rev_mix,volume,pan", ""]

    # PLAY
    L.append("[tab PLAY]")
    L.append("art file=art/bg_play.png x=0 y=%d w=1280 h=628 fit=stretch" % Y_OFF)
    header_lines(L)
    L.append('button cx=74 cy=%d label="" key=prog_prev img=art/prog_prev_off.png img_on=art/prog_prev_on.png w=38 h=30' % y(28))
    L.append('readout cx=222 cy=%d w=240 h=32 label="" key=prog_name text_size=16' % y(28))
    L.append('button cx=368 cy=%d label="" key=prog_next img=art/prog_next_off.png img_on=art/prog_next_on.png w=38 h=30' % y(28))
    for cx, cy, r, key, _, col in PLAY_KNOBS:
        L.append(knob_line(cx, cy, r, key, col, 82 if cx < 340 else 92))
    for fx, key, _ in FADERS:
        L.append('slider_v cx=%d cy=%d w=%d h=%d label="" key=%s name=hide strip=art/fader.png width=68'
                 % (fx, y(FADER_CY), FADER_W, FADER_H, key))
    for key, opts, x0 in SEG_GROUPS:
        for i, opt in enumerate(opts):
            n = opt_name(key, opt)
            L.append('option cx=%d cy=%d w=%d h=%d key=%s option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                     % (x0 + SEG_W // 2 + i * SEG_W, y(SEG_Y), SEG_W, SEG_H, key, opt, n, n))
    dx, dy, dw, dh = DISPLAY
    for b in range(WAVE_BARS):
        L.append('meter cx=%d cy=%d w=%d h=%d key=wave_%d strip=art/wave_bar.png frames=128' % (WAVE_X0 + b * WAVE_STEP, y(WAVE_CY), WAVE_W, WAVE_H, b + 1))
    for t in range(32):
        L.append('meter cx=%d cy=%d w=%d h=16 key=ptile_%d strip=art/ptile.png frames=128' % (round(PTILE_X0 + t * PTILE_STEP + PTILE_W / 2), y(PTILE_CY), PTILE_W, t + 1))
    L.append('readout cx=%d cy=%d w=%d h=34 label="" key=layer_name text_size=19' % (dx + dw // 2, y(dy + 26), dw - 40))
    L.append('readout cx=%d cy=%d w=%d h=26 label="" key=prog_info text_size=14' % (dx + dw // 2, y(dy + 54), dw - 40))
    for key, x0, cw in CHIPS:
        L.append('readout cx=%d cy=%d w=%d h=28 label="" key=%s text_size=14' % (x0 + cw // 2, y(dy + dh - 28), cw, key))
    L.append('readout cx=492 cy=%d w=136 h=30 label="" key=status text_size=14' % y(500))
    L.append('button cx=596 cy=%d label="" key=pad_down img=art/pad_down_off.png img_on=art/pad_down_on.png w=38 h=30' % y(500))
    L.append('readout cx=672 cy=%d w=110 h=30 label="" key=pad_info text_size=13' % y(500))
    L.append('button cx=748 cy=%d label="" key=pad_up img=art/pad_up_off.png img_on=art/pad_up_on.png w=38 h=30' % y(500))
    L.append('readout cx=818 cy=%d w=72 h=30 label="" key=pad_vel text_size=15' % y(500))
    L.append('slider_v cx=42 cy=%d w=30 h=50 label="" key=pad_vel name=hide value=hide strip=art/fader_small.png' % y(593))
    for p in range(16):
        cx = PAD_X0 + PAD_STEP * p + PAD_W / 2
        L.append('button cx=%d cy=%d label="%02d" key=pad_%d img=art/pad%d_off.png img_on=art/pad%d_on.png w=%d h=%d'
                 % (cx, y(PADS_Y + 3), p + 1, p + 1, p + 1, p + 1, PAD_W + 12, PAD_H + 12))
    L.append('qlinks "PLAY" = attack,decay,sustain,release,cutoff,resonance,filter_vel,vel_sens,transpose,bend,tune,glide,drive,rev_mix,volume,pan')
    L.append("")

    # LAYERS
    L.append("[tab LAYERS]")
    L.append("art file=art/bg_layers.png x=0 y=%d w=1280 h=628 fit=stretch" % Y_OFF)
    header_lines(L)
    for i in range(4):
        x = SLOT_X0 + i * SLOT_STEP
        n = i + 1
        col = SLOT_COLOURS[i]
        L.append('readout cx=%d cy=%d w=%d h=30 label="" key=slot%d_name text_size=16' % (x + 62 + (SLOT_W - 76) // 2, y(SLOT_Y + 30), SLOT_W - 84, n))
        L.append('option cx=%d cy=%d w=200 h=30 key=target_slot option=%s label="" img=art/slot_load_%d_off.png img_on=art/slot_load_%d_on.png'
                 % (x + SLOT_W // 2, y(SLOT_Y + 70), "ABCD"[i], i, i))
        L.append(knob_line(x + 80, SLOT_Y + 148, 26, "slot%d_lo" % n, col, 120))
        L.append(knob_line(x + 220, SLOT_Y + 148, 26, "slot%d_hi" % n, col, 120))
        L.append(knob_line(x + 80, SLOT_Y + 248, 26, "slot%d_vol" % n, col, 120))
        L.append(knob_line(x + 220, SLOT_Y + 248, 26, "slot%d_tune" % n, col, 120))
        for k, opt in enumerate(("Play", "Mute")):
            m = "slot%d_%s" % (i, opt.lower())
            L.append('option cx=%d cy=%d w=90 h=30 key=slot%d_mute option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                     % (x + SLOT_W // 2 - 48 + k * 96, y(SLOT_Y + 312), n, opt, m, m))
    for t in range(32):
        L.append('meter cx=%d cy=%d w=%d h=%d key=ltile_%d strip=art/ltile.png frames=128' % (LT_X0 + t * LT_STEP + LT_W // 2, y(LT_CY), LT_W, LT_H, t + 1))
    lx, ly, lw, lh = LMAP
    for k, opt in enumerate(("Layer", "Keyswitch")):
        m = opt_name("layer_mode", opt)
        L.append('option cx=%d cy=%d w=120 h=32 key=layer_mode option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                 % (150 + k * 126, y(ly + 188), opt, m, m))
    L.append(knob_line(440, ly + 186, 16, "ks_base", ORANGE, 44).replace(' name=hide', ' name=hide value=hide'))
    L.append('readout cx=%d cy=%d w=80 h=28 label="" key=ks_base text_size=15' % (520, y(ly + 188)))
    L.append('button cx=%d cy=%d label="" key=auto_split img=art/b_split_off.png img_on=art/b_split_on.png w=160 h=36' % (980, y(ly + 188)))
    L.append('button cx=%d cy=%d label="" key=slot_clear img=art/b_clear_off.png img_on=art/b_clear_on.png w=160 h=36' % (1160, y(ly + 188)))
    L.append('button cx=%d cy=%d label="" key=patch_save img=art/b_patch_off.png img_on=art/b_patch_on.png w=160 h=36' % (800, y(ly + 188)))
    L.append('qlinks "LAYERS" = slot1_lo,slot1_hi,slot1_vol,slot1_tune,slot2_lo,slot2_hi,slot2_vol,slot2_tune,'
             'slot3_lo,slot3_hi,slot3_vol,slot3_tune,slot4_lo,slot4_hi,slot4_vol,slot4_tune')
    L.append("")

    # BROWSE
    L.append("[tab BROWSE]")
    L.append("art file=art/bg_browse.png x=0 y=%d w=1280 h=628 fit=stretch" % Y_OFF)
    header_lines(L)
    x, yy, w, h = BR_LIST
    L.append('readout cx=%d cy=%d w=%d h=34 label="" key=br_loc text_size=17' % (x + w // 2, y(yy + 29), w - 40))
    L.append('list x=%d y=%d w=%d h=%d cols=1 rows=%d th=40 gap=4 key=br' % (x + 14, y(yy + 56), w - 28, BROWSER_ROWS * 44 - 4, BROWSER_ROWS))
    by = yy + h - 29
    L.append('button cx=%d cy=%d label="" key=br_prev img=art/pg_prev_off.png img_on=art/pg_prev_on.png w=70 h=34' % (x + 60, y(by)))
    L.append('readout cx=%d cy=%d w=160 h=30 label="" key=br_page text_size=16' % (x + w // 2, y(by)))
    L.append('button cx=%d cy=%d label="" key=br_next img=art/pg_next_off.png img_on=art/pg_next_on.png w=70 h=34' % (x + w - 60, y(by)))
    x, yy, w, h = BR_SIDE
    for i, key in enumerate(("drives", "library", "up", "refresh")):
        cx = x + 128 + (i % 2) * 240
        cy = yy + 64 + (i // 2) * 46
        L.append('button cx=%d cy=%d label="" key=br_%s img=art/b_%s_off.png img_on=art/b_%s_on.png w=220 h=40' % (cx, y(cy), key, key, key))
    L.append('button cx=%d cy=%d label="" key=br_setlib img=art/b_setlib_off.png img_on=art/b_setlib_on.png w=220 h=40' % (x + 128, y(yy + 156)))
    L.append('readout cx=%d cy=%d w=%d h=24 label="" key=lib_path text_size=12' % (x + w // 2, y(yy + 195), w - 44))
    L.append('readout cx=%d cy=%d w=%d h=58 label="" key=br_info text_size=15' % (x + w // 2, y(yy + 269), w - 52))
    for i in range(4):
        n = "slot_target_%d" % i
        L.append('option cx=%d cy=%d w=50 h=30 key=target_slot option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                 % (x + 220 + i * 60, y(yy + 324), "ABCD"[i], n, n))
    L.append('button cx=%d cy=%d label="" key=prog_prev img=art/prog_prev_off.png img_on=art/prog_prev_on.png w=38 h=30' % (x + 48, y(yy + 368)))
    L.append('readout cx=%d cy=%d w=340 h=32 label="" key=prog_name text_size=17' % (x + w // 2, y(yy + 368)))
    L.append('button cx=%d cy=%d label="" key=prog_next img=art/prog_next_off.png img_on=art/prog_next_on.png w=38 h=30' % (x + w - 48, y(yy + 368)))
    L.append('readout cx=%d cy=%d w=%d h=24 label="" key=prog_info text_size=13' % (x + w // 2, y(yy + 402), w - 52))
    L.append('readout cx=%d cy=%d w=%d h=24 label="" key=status text_size=13' % (x + w // 2, y(yy + 428), w - 52))
    L.append('button cx=%d cy=%d label="" key=br_extract img=art/b_extract_off.png img_on=art/b_extract_on.png w=220 h=40' % (x + 128, y(yy + 500)))
    for i, opt in enumerate(["Off", "Disk Images"]):
        n = "set_" + opt_name("auto_extract", opt)
        L.append('option cx=%d cy=%d w=%d h=%d key=auto_extract option="%s" label="" img=art/%s_off.png img_on=art/%s_on.png'
                 % (x + 318 + i * 104, y(yy + 500), 98, 34, opt, n, n))
    L.append('qlinks "BROWSE" = volume,pan,cutoff,resonance,rev_mix,drive,pad_vel,transpose')
    L.append("")

    # SETUP
    L.append("[tab SETUP]")
    L.append("art file=art/bg_setup.png x=0 y=%d w=1280 h=628 fit=stretch" % Y_OFF)
    header_lines(L)
    for cx, cy, r, key, _, col in SETUP_KNOBS:
        L.append(knob_line(cx, cy, r, key, col))
    for key, opts, x0, cy, _ in SETUP_OPTIONS:
        for i, opt in enumerate(opts):
            n = "set_" + opt_name(key, opt)
            L.append('option cx=%d cy=%d w=%d h=%d key=%s option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                     % (x0 + OPT_W // 2 + i * (OPT_W + 6), y(cy), OPT_W, OPT_H, key, opt, n, n))
    L.append('qlinks "SETUP" = polyphony,glide,bend,voice_mode,volume,pan,transpose,tune,vel_sens,filter_vel,interp,pad_base,rev_damp,mem_limit,prog_change,auto_extract')
    L.append("")

    # MOD
    L.append("[tab MOD]")
    L.append("art file=art/bg_mod.png x=0 y=%d w=1280 h=628 fit=stretch" % Y_OFF)
    header_lines(L)
    for i, (x, yy) in enumerate(LFO_BOX):
        n = i + 1
        for k, opt in enumerate(LFO_WAVES):
            nm = opt_name("lfo%d_wave" % n, opt)
            L.append('option cx=%d cy=%d w=58 h=30 key=lfo%d_wave option="%s" label="" img=art/%s_off.png img_on=art/%s_on.png'
                     % (x + 50 + k * 60, y(yy + 82), n, opt, nm, nm))
        L.append(knob_line(x + 80, yy + 178, 34, "lfo%d_rate" % n, CYAN, 120))
        L.append('popup cx=%d cy=%d w=200 h=38 label="" key=lfo%d_sync text_size=17' % (x + 270, y(yy + 158), n))
        for k, opt in enumerate(("Free", "Note")):
            nm = opt_name("lfo%d_retrig" % n, opt)
            L.append('option cx=%d cy=%d w=92 h=32 key=lfo%d_retrig option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                     % (x + 222 + k * 98, y(yy + 232), n, opt, nm, nm))
    x, yy, w, h = MX
    for i in range(8):
        n, ry = i + 1, yy + MX_ROW0 + i * MX_STEP
        L.append('popup cx=%d cy=%d w=250 h=38 label="" key=mod%d_src text_size=16' % (x + 200, y(ry), n))
        L.append('popup cx=%d cy=%d w=250 h=38 label="" key=mod%d_dst text_size=16' % (x + 480, y(ry), n))
        # a small knob, not slider_h: MPC's horizontal slider is a square 150 px filmstrip box that overlaps the rows
        L.append('knob cx=%d cy=%d r=17 key=mod%d_amt label="" name=hide value=hide strip=art/%s.png' % (x + 700, y(ry), n, KNOB_STRIPS[CYAN]))
        L.append('readout cx=%d cy=%d w=66 h=32 label="" key=mod%d_amt text_size=16' % (x + 786, y(ry), n))
    L.append('qlinks "MOD" = mod1_amt,mod2_amt,mod3_amt,mod4_amt,mod5_amt,mod6_amt,mod7_amt,mod8_amt,lfo1_rate,lfo1_wave,lfo1_sync,lfo1_retrig,lfo2_rate,lfo2_wave,lfo2_sync,lfo2_retrig')
    L.append("")

    # INFO
    L.append("[tab INFO]")
    L.append("art file=art/bg_info.png x=0 y=%d w=1280 h=628 fit=stretch" % Y_OFF)
    header_lines(L)
    L.append('qlinks "INFO" = volume,pan,cutoff,resonance,rev_mix,drive,pad_vel,transpose')
    return "\n".join(L) + "\n"


CSS = """/* Omni Sampler: the renderer draws only controls; panels, labels and the display are art/bg_*.png. */
.box, .box-label { display: none; }
/* pad numbers on the pad images */
.btn-label, .button-label { font-weight: 700; }
"""


def main():
    with open(os.path.join(HERE, "params.json"), "w") as f:
        json.dump(params(), f, indent=1)
        f.write("\n")
    images()
    with open(os.path.join(HERE, "skin.css"), "w") as f:
        f.write(CSS)
    with open(os.path.join(HERE, "layout.conf"), "w") as f:
        f.write(layout())
    print("wrote params.json, skin.css, layout.conf and art/")


if __name__ == "__main__":
    main()
