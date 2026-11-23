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
import re   # 2.4.30: the FX BANK catalog (fxb_programs)
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
_v = json.load(open(os.path.join(HERE, "vst.json")))["version"]
VERSION = "%d.%d.%d" % (_v // 1000, _v // 100 % 10, _v % 100)   # 1800 -> 1.8.0
REPO = os.environ.get("MPC_VST") or os.path.join(os.path.dirname(HERE), "mpc-vst-plugins")   # sd88me/mpc-vst-plugins checkout
ART = os.path.join(HERE, "art", os.environ.get("OMNI_SKIN", ""))   # 1.8.13: each skin's art in its own folder
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
# 1.8.13: three skins, chosen live by the "skin" parameter (SETUP): design.py with no OMNI_SKIN builds all three
# (each in a child run with its own settings and art/<skin>/) and merges their layouts, each skin's lines shown only
# while it is selected (when=skin:N). Original = the flat 1.8.12 look; Silver = the light hardware panel; Modules =
# the panel with colour-coded built-in modules.
SKINS = [("original", "Original", {"OMNI_RACK": "0", "OMNI_MODULES": "0"}),
         ("silver", "Silver", {"OMNI_RACK": "1", "OMNI_THEME": "light", "OMNI_MODULES": "0"}),
         ("modules", "Modules", {"OMNI_RACK": "1", "OMNI_THEME": "light", "OMNI_MODULES": "1"}),
         # 1.8.14: Studio = black anodised hardware (the user 2026-11-05: "look like a real hardware VST instrument")
         ("studio", "Studio", {"OMNI_RACK": "1", "OMNI_THEME": "studio", "OMNI_MODULES": "1"})]
# the skins built (OMNI_SKINS=silver,modules or all): Modules only by default. The user's limit on the Live II was 150 MB (raised to ~230 MB on 2026-11-17)
# of MPC's decoded image memory; at the 128 frames MPC needs, Modules alone is 146 MB, Silver + Modules 172 MB
_want = [n.strip() for n in os.environ.get("OMNI_SKINS", "studio").split(",") if n.strip() and n.strip() != "all"]
SKINS = [s_ for s_ in SKINS if not _want or s_[0] in _want]
SKIN_SWITCH = len(SKINS) > 1   # one skin: no switch, no when= tags
SKIN = os.environ.get("OMNI_SKIN", "")
MODULES = os.environ.get("OMNI_MODULES", "1") != "0"
RACK = os.environ.get("OMNI_RACK", "1") != "0"
METER_FRAMES = 128   # meter strips (wave bars, tiles, LEDs): MPC always cuts a FilmStrip into 128 frames (64 broke, 2026-11-04)   # 1.8.13: BROWSE in the rack-mount look (OMNI_RACK=0: the flat one)
WAVE_BARS = 48
DELAY_TIMES = ["1/32", "1/16 T", "1/16", "1/16 .", "1/8 T", "1/8", "1/8 .", "1/4 T", "1/4", "1/4 .", "1/2", "1 Bar"]
FX_KEYS = {"eq_low", "eq_mid", "eq_high", "comp", "cho_mix", "cho_rate", "cho_depth", "dly_mix", "dly_time", "dly_fb",
           "dly_tone", "dly_ping"}
MODFX_KEYS = ["phs_mix", "phs_rate", "phs_depth", "phs_fb", "flg_mix", "flg_rate", "flg_depth", "flg_fb",
              "trm_depth", "trm_rate", "trm_sync", "trm_mode", "trm_shape", "lofi_mix", "lofi_bits", "lofi_rate"]   # 1.8.2
FX_KEYS |= set(MODFX_KEYS)
# 2.4.30: FX BANK (FX tab, third page; engine/omni_fx_bank.hpp). The program list comes from the header's catalog, in its
# order, without its NO EFFECT / THRU entries (plugin.cpp fxb_catalog does the same); shown with neutral card names
FXB_KEYS = ["fxb_prog", "fxb_mix", "fxb_rate", "fxb_depth", "fxb_fb", "fxb_time", "fxb_tone", "fxb_drive", "fxb_thr",
            "fxb_ratio", "fxb_pitch", "fxb_low", "fxb_mid", "fxb_high", "fxb_bypass"]
FX_KEYS |= set(FXB_KEYS)
FXB_SLOTS = ["A", "B", "C", "D"]   # 2.4.33: the bay's slots (A factory, B..D expansions of up to 999 programs: 2.4.35)
FX_KEYS |= {"fxb_slot", "fxb_prog_b", "fxb_prog_c", "fxb_prog_d"}
FXB_CARDS = {"700/reverb": "REV", "700/chorus": "MOD", "700/variation": "VAR", "Z-FX": "MULTI"}


def fxb_programs():
    """[(card, name)] of the FX BANK's programs (Bypass ones left out), read from src/engine/omni_fx_bank.hpp"""
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "engine", "omni_fx_bank.hpp")).read()
    out = []
    for bank, name, engine in re.findall(r'\{"([^"]+)",\s*"([^"]+)",\s*-?\d+,\s*-?\d+,\s*-?\d+,\s*Engine::(\w+)', src):
        if engine != "Bypass":
            out.append((FXB_CARDS.get(bank, "FX"), name.replace(" / ", " ")))
    assert len(out) > 100, "FX BANK catalog not found"
    return out


FXB_OPTIONS = ["Off"] + ["%s %s" % p for p in fxb_programs()]
TREM_MODES = ["Tremolo", "Auto-Pan"]
TREM_SHAPES = ["Sine", "Square"]
SOUND_KEYS = {"volume", "pan", "transpose", "tune", "cutoff", "resonance", "filter_type", "filter_vel", "attack", "decay",
              "sustain", "release", "vel_sens", "bend", "polyphony", "voice_mode", "glide", "interp", "sinc", "rev_mix", "rev_size",
              "rev_damp", "drive", "wt_pos", "env_attack", "env_decay", "env_sustain", "env_release"} | {"lfo%d_%s" % (i, k) for i in (1, 2) for k in ("wave", "rate", "sync", "retrig")} | \
             {"mod%d_%s" % (i, k) for i in range(1, 9) for k in ("src", "dst", "amt", "slot")} | FX_KEYS
MOD_ROUTE_SLOTS = ["All", "A", "B", "C", "D"]
# 2.3: the SYNTH tab
SYN_TYPES = ["Classic", "Wavetable", "Sample", "Macro"]          # the oscillator types (engine OscType order)
SYN_PMODES = ["Reset", "Free", "Random", "Spread"]              # phase modes (engine PhaseMode order)
SYN_WMODES = ["Off", "Bend", "PWM", "Mirror", "Sync", "FM"]      # warp modes (engine WarpMode order; FM: from the other osc)
SYN_BARS = 32
# 2.4.9: the OSC screen's pick keys (the user: hard to find one table among thousands): key suffix, label; x of the first
# key's centre from the module's left, key width (the three end at x0 + 594, inside the screen's x0 + 600)
SYN_PICK = [("fprev", "< FOLDER"), ("rnd", "RANDOM"), ("rndall", "RND ALL")]
SYN_PICK_X, SYN_PICK_W = 391, 78
SYN_ACC = {"a": (90, 220, 255), "b": (255, 170, 70)}            # OSC A cyan, OSC B orange
SYN_X0 = {"a": 16, "b": 648}                                    # the oscillator modules (616 x 554 from y 66)
SYN_ROW1 = ("pos", "detune", "spread", "warp", "pan", "level")
SYN_ROW2 = ("oct", "semi", "fine", "phase", "rand", "unison")
SYN_LABELS = {"pos": "POSITION", "detune": "DETUNE", "spread": "SPREAD", "warp": "WARP", "pan": "PAN", "level": "LEVEL",
              "oct": "OCTAVE", "semi": "SEMI", "fine": "FINE", "phase": "PHASE", "rand": "RANDOM", "unison": "UNISON"}
# 2.3.1: the SYNTH tab's MIX / FILTER page
SYN_FTYPES = ["LP 12", "LP 24", "Ladder", "BP", "HP", "Notch", "Comb"]   # engine FilterType order
SYN_SUB_SHAPES = ["Sine", "Tri", "Square"]
SYN_NOISE_COLS = ["White", "Pink", "Crackle"]
SM_SUB, SM_NOISE, SM_OMOD = (16, 66, 300, 270), (332, 66, 300, 270), (648, 66, 616, 270)
SM_FLT, SM_ROUTE = (16, 352, 924, 268), (956, 352, 308, 268)
SM_SCREEN = (32, 404, 380, 200)                    # the filter's response screen
SM_AREA = (0, 58, W, 570)                          # what bg_synth2 covers (plugin coordinates): the body
SM_GREEN, SM_VIOLET, SM_BLUE, SM_CYAN, SM_ORANGE = (110, 230, 140), (170, 120, 255), (90, 150, 255), (90, 220, 255), (255, 170, 70)
# 2.3.2: the SYNTH tab's ENV / LFO page
SYN_DESTS = ["Off", "Pitch", "Cutoff", "Resonance", "Position A", "Position B", "Warp A", "Warp B", "Level", "Pan", "FM", "Balance"]
# 2.4.13: + Custom (a Vital shape: a preset's, or a .vitallfo from <Library>/LFOs; grown at the end: plugin.cpp grown_opt)
SYN_LFO_SHAPES = ["Sine", "Tri", "Saw", "Square", "S&H", "Random", "Custom"]
SYN_LFO_SW, SYN_LFO_SP = 52, 56   # 2.4.13: the shape keys' width and pitch (seven in the panel; were 60 / 64)
SYN_LFO_TRIGS = ["Free Run", "Retrig", "One-Shot"]
SYN_LFO_ROUTES = ["Off", "Amp", "Filter", "Mod"]   # 2.3.6 (the keys: Amp, Filter, Mod in the envelopes' colours)
SYN_LFO_RKEYS = ["amp", "flt", "mod"]   # the route keys' params: syn_lfo<n>_rk_<key> (plugin.cpp LFO_RK)
SE_ENVS = [("a", "AMP ENV", (16, 66, 408, 290), SM_ORANGE, ("syn_attack", "syn_decay", "syn_sustain", "syn_release", "syn_level"), "LEVEL"),
           ("f", "FILTER ENV", (436, 66, 408, 290), SM_CYAN, ("syn_fe_attack", "syn_fe_decay", "syn_fe_sustain", "syn_fe_release", "syn_fenv"), "AMOUNT"),
           ("m", "MOD ENV", (856, 66, 408, 290), SM_VIOLET, ("syn_me_attack", "syn_me_decay", "syn_me_sustain", "syn_me_release", "syn_me_amt"), "AMOUNT")]
SE_LFOS = [(1, (16, 368, 408, 252), SM_GREEN), (2, (436, 368, 408, 252), SM_BLUE)]
SE_MAC = (856, 368, 408, 252)
# 2.4.17: the MOD 2 page: MOD ENV 2 / 3 over LFO 3 / 4 (the same panels as ENV's), a note on the right
SE_ENVS2 = [("n", "MOD ENV 2", (16, 66, 408, 290), SM_VIOLET, ("syn_me2_attack", "syn_me2_decay", "syn_me2_sustain", "syn_me2_release", "syn_me2_amt"), "AMOUNT"),
            ("o", "MOD ENV 3", (436, 66, 408, 290), SM_BLUE, ("syn_me3_attack", "syn_me3_decay", "syn_me3_sustain", "syn_me3_release", "syn_me3_amt"), "AMOUNT")]
SE_LFOS2 = [(3, (16, 368, 408, 252), SM_CYAN), (4, (436, 368, 408, 252), SM_ORANGE)]
SE_NOTE2 = (856, 66, 408, 554)
SE_MOD_ENV_KEY = {"m": "syn_me", "n": "syn_me2", "o": "syn_me3"}   # a mod envelope's key prefix by its bars' letter
ENV_BARS = 24
# 2.3.3: the SYNTH tab's MATRIX and VOICE pages
SYN_MX_SRCS = ["Off", "LFO 1", "LFO 2", "Mod Env", "Amp Env", "Filter Env", "Velocity", "Note", "Mod Wheel", "Aftertouch",
               "Macro 1", "Macro 2", "Macro 3", "Macro 4", "Random",
               "LFO 3", "LFO 4", "Mod Env 2", "Mod Env 3"]      # synth.hpp SynthSrc order (2.4.17: the last four appended)
SYN_MX_DESTS = SYN_DESTS + ["LFO 1 Rate", "LFO 2 Rate", "Detune A", "Detune B", "Sub Level", "Noise Level", "Drive"]   # SynthDest
# 2.4.9 (the user: envelopes to more destinations): every SynthDest, in its order (synth.hpp), for the MOD ENV, the macros
# and the MATRIX (the LFOs' own list, hidden since 2.3.6, stays SYN_DESTS: they reach the rest through the MATRIX). The
# lists only grow at the end; plugin.cpp GROWN_OPTS keeps a project's own value when MPC replays its older normalized one
SYN_ALL_DESTS = SYN_MX_DESTS + ["Spread A", "Spread B", "Ring", "Feedback", "Noise Tone", "Sub Pan", "Glide",
                                "LFO 3 Rate", "LFO 4 Rate",   # 2.4.17: appended (synth.hpp SD_LFO3_RATE, SD_LFO4_RATE)
                                "Level A", "Level B"]         # 2.4.35: appended (synth.hpp SD_LEVEL_A, SD_LEVEL_B)
SYN_VMODES = ["Poly", "Mono", "Legato"]
SV_VOICE, SV_PITCH, SV_RANGE = (16, 66, 616, 270), (648, 66, 616, 270), (16, 352, 1248, 268)
MX_SRC_X, MX_DST_X, MX_AMT_X = 168, 398, 560   # 2.3.3: the MATRIX columns (centres, from a module's left)
SV_KEYS = (40, 470, 900, 90)     # the KEY RANGE keyboard picture (plugin coordinates)
# 2.4.12 MUTATE / BREED row over the keyboard: STORE A, STORE B, BREED, MUTATE, UNDO centres; SUBTLE/MEDIUM/WILD from MUT_AMT_X
MUT_Y, MUT_X, MUT_AMT_X = 441, (100, 218, 336, 470, 892), 572
# 2.3.3: the settings shown as steppers (a readout between < >) instead of drop-downs: a drop-down's list costs an
# image per entry per drop-down (the MATRIX's 16 lists were ~25 MB decoded)
STEPPED = ["syn_mx%d_%s" % (n, k) for n in range(1, 9) for k in ("src", "dst")] + \
          ["syn_lfo1_dest", "syn_lfo2_dest", "syn_lfo1_sync", "syn_lfo2_sync", "syn_me_dest"]
# 2.4: the ARP tab (src/engine/arp.cpp: RATE_NAMES, SCALE_NAMES in the same order)
ARP_MODES = ["Seq", "Arp"]
ARP_RATES = ["1/4", "1/8", "1/16", "1/32", "1/8 T", "1/16 T", "1/32 T"]
ARP_SCALES = ["Chromatic", "Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian", "Harm Minor",
              "Mel Minor", "Major Pent", "Minor Pent", "Blues", "Whole Tone", "Hirajoshi", "Phryg Dom"]
ARP_ROOTS = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
ARP_PAGES = ["A", "B", "C", "D"]
ARP_ROWS = ARP_COLS = 16
# 2.4.1: how ARP mode takes the held keys, its octaves, how a MIDI file snaps, strum directions, the grid's touch mode
ARP_ORDERS = ["Pattern", "Up", "Down", "Up/Down", "Down/Up", "Random", "As Played", "Chord"]
ARP_OCTS = ["1 Oct", "2 Oct", "3 Oct", "4 Oct"]
ARP_SNAPS = ["Auto", "1/32", "1/16", "1/8", "1/4", "1 Bar"]
ARP_STRUM_DIRS = ["Up", "Down", "Alt"]
ARP_DRAWS = ["Tap", "Draw"]
# 2.4.2 GEN page
ARP_DIRS = ["Forward", "Back", "Ping-Pong", "Random"]
ARP_CONTOURS = ["Rise", "Fall", "Arch", "Valley", "Wave", "Random",
                # 2.4.9 (appended: plugin.cpp ArpMode, GROWN_OPTS): the MOTIF modes the user asked for, and two more
                "Ostinato", "Orch Riff", "High to Low", "Low to High", "Call + Answer", "Pedal", "Arpeggio"]
ARP_CHAIN = ["-", "A", "B", "C", "D"]
MUT_AMOUNTS = ["Subtle", "Medium", "Wild"]   # 2.4.12
MUT_KEYS = [("mut_store_a", "STORE A"), ("mut_store_b", "STORE B"), ("mut_breed", "BREED"), ("mut_go", "MUTATE"), ("mut_undo", "UNDO")]
ARP_BREED_KEYS = [("arp_b_store_a", "STORE A"), ("arp_b_store_b", "STORE B"), ("arp_b_breed", "BREED")]
ARP_VOICES = ["Invert Up", "Invert Down", "Drop 2", "Drop 3", "Open", "Close"]   # 2.4.14 VOICE (plugin.cpp VoiceMode)
ARP_GEN_KEYS = [("arp_g_invert", "INVERT"), ("arp_g_reverse", "REVERSE"), ("arp_g_double", "DOUBLE"), ("arp_g_half", "HALF"),
                ("arp_g_mutate_go", "MUTATE"), ("arp_g_fill", "FILL"), ("arp_g_undo", "UNDO")]
# the DRAW pad over the grid (an XYPad; build: add_xypad.py): the 16 x 16 cells plus a margin of one column on the left
# and 14 px below, where the pad's resting position (0, 0) lands off the cells (plugin.cpp XY_* has the same numbers)
XY_ML, XY_MB = 52, 14
AG_X0, AG_DX, AG_Y1, AG_DY = 432, 52, 598, 27   # the grid: column 1's centre x, column pitch, row 1's (bottom) y, row pitch
AG_CW, AG_CH = 46, 23                           # a cell
AG_LED_Y = 166                                  # the playhead lights over the columns
AA_L, AA_R = (16, 66, 300, 554), (332, 66, 932, 554)   # the ARP page's two modules
ARP_HDR_DX = 56   # 2.4.9: STRUM, LATCH, TIE, FOLLOW moved right for SHIFT's up / down keys
COMP_TYPES = ["Off", "Classic", "Glue", "Punch"]   # 2.2: the compressor types (FX, a list); Off first
CHORUS_TYPES = ["Off", "Classic", "Ens I", "Ens II", "Ens I+II"]   # 2.2: the chorus types (FX); Off has no key
DELAY_TYPES = ["Off", "Classic", "Tape", "Analog"]   # 2.2: the delay types (FX); Off has no key
DRIVE_TYPES = ["Off", "Classic", "Tape", "Tube", "Fold"]   # 2.2: the drive types (FX), the engine's drive_type order
REV_TYPES = ["Off", "Classic", "Plate", "Hall", "Room"]   # 2.2: the reverb types (FX), the engine's reverb_type order; Off has no key
PLAY_MODES = ["Fwd", "Rev", "Ping-Pong", "PP Inv"]   # 1.8.2: the engine's slot_play order
LFO_WAVES = ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H"]
LFO_SYNC = ["Free", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4 T", "1/8 T", "1/16 T", "1/4 .", "1/8 .",
            "1/16 ."]
# the engine's ModSrc / ModDst enums (src/engine/sampler.hpp), in order
MOD_SRC = ["Off", "LFO 1", "LFO 2", "Mod Wheel", "Aftertouch", "Pitch Bend", "Velocity", "Key", "Random", "Amp Env"]
MOD_DST = ["Off", "Pitch", "Cutoff", "Resonance", "Volume", "Pan", "Sample Start", "Drive", "Reverb Mix", "LFO 1 Rate", "LFO 2 Rate",
           "WT Position"]   # 1.7: appended
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
    num("mem_limit", "Memory Limit", 64, 2048, 512, "MB")   # 2.4.14 (user): 512 MB; 32-bit MPCs (Live II) at most 1024, 64-bit (Live III, 8 GB) 2048 (plugin.cpp MEM_MAX_MB)
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
    for k, n in (("prev", "Browser Row Up"), ("next", "Browser Row Down"), ("up", "Browser Up"),
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
    # 1.7: the browser list's scroll position (a fader beside it; the data wheel and a Q-Link move it a row at a time)
    add("br_scroll", "Browser Scroll", min=0, max=1, default=1, live=True)
    add("info_page", "Formats Page", options=["1", "2", "3"], default=0)   # INFO: which page of the formats list
    # 1.7: wavetables: the position through their cycles (a sound setting: saved with an instrument, Q-Link, matrix target)
    num("wt_pos", "WT Position", 0, 100, 0, "%")
    # 1.7.1: the envelope faders in real values (times on a curve: units 0..1000 = 0..60 s, sustain %); set from the preset
    # on load, so they need "live" (SOUND_KEYS)
    for k, n, d in (("env_attack", "Attack", 0), ("env_decay", "Decay", 0), ("env_sustain", "Sustain", 100), ("env_release", "Release", 69)):
        add(k, n, min=0, max=100 if k == "env_sustain" else 1000, default=d, display="int", dynamic_display=True)
    # 1.7.2: a tapped kit plays its preview (NI .previews/<file>.ogg, Battery previews/<file>.ogg)
    add("br_preview", "Browser Preview", options=["Off", "On"], default=1)
    # 1.7.3: a simple scroll, one row at a time: the row buttons beside the list, and Q-Link 1 as an encoder (an option
    # param steps one option per nudge; the engine moves a row and returns it to the middle). Replaces the 1.7 fader
    # (br_scroll, kept above so the later indices don't move), which jumped by list length
    trigger("br_rowup", "Browser Page Up")
    trigger("br_rowdown", "Browser Page Down")
    add("br_row", "Browser Scroll", options=["Up", "-", "Down"], default=1, live=True)
    # 1.7.3: each slot's key shift: its whole mapping moves up or down the keyboard, pitch unchanged (a kit from C1
    # played from C0, leaving C1 up for a bass); the slot's key range moves with it
    for i, L_ in ((1, "A"), (2, "B"), (3, "C"), (4, "D")):
        add("slot%d_shift" % i, "Slot %s Key Shift" % L_, min=-48, max=48, default=0, display="int", dynamic_display=True)
    # 1.7.3: BROWSE's SELECTED box in three lines, wrapped by the engine, so a long folder or file name shows in full
    for i in range(1, 4):
        text("br_sel%d" % i, "Browser Selected %d" % i)
    # 1.8: each slot's pan (LAYERS; saved with projects and patches)
    for i, L_ in ((1, "A"), (2, "B"), (3, "C"), (4, "D")):
        add("slot%d_pan" % i, "Slot %s Pan" % L_, min=-100, max=100, default=0, display="int", dynamic_display=True)
    # 1.8: the FX page: EQ, compressor, chorus and a tempo-synced delay, before the reverb (sound settings: saved with
    # instruments, patches and projects)
    num("eq_low", "EQ Low", -12, 12, 0, "dB")
    num("eq_mid", "EQ Mid", -12, 12, 0, "dB")
    num("eq_high", "EQ High", -12, 12, 0, "dB")
    num("comp", "Compressor", 0, 100, 0, "%")
    num("cho_mix", "Chorus Mix", 0, 100, 0, "%")
    add("cho_rate", "Chorus Rate", min=0, max=100, default=50, display="int", dynamic_display=True)
    num("cho_depth", "Chorus Depth", 0, 100, 50, "%")
    num("dly_mix", "Delay Mix", 0, 100, 0, "%")
    add("dly_time", "Delay Time", options=DELAY_TIMES, default=5)
    num("dly_fb", "Delay Feedback", 0, 95, 35, "%")
    num("dly_tone", "Delay Damping", 0, 100, 30, "%")
    add("dly_ping", "Delay Mode", options=["Stereo", "Ping-Pong"], default=0)
    # 1.8: the instrument slot each matrix route acts on (All = every slot, as before; appended so no index moves)
    for n in range(1, 9):
        add("mod%d_slot" % n, "Mod %d Slot" % n, options=MOD_ROUTE_SLOTS, default=0)
    # 1.8.2: PLAY's second page (MPC's sub-page, PADS): each slot's playback mode and polarity, and note locks (what a
    # note plays with: mode, polarity, tune, start, cutoff, level, pan, velocity) edited for the note last played or
    # tapped; the pads follow the notes played and mark the locked ones
    for i, L_ in ((1, "A"), (2, "B"), (3, "C"), (4, "D")):
        add("slot%d_play" % i, "Slot %s Playback" % L_, options=PLAY_MODES, default=0)
    for i, L_ in ((1, "A"), (2, "B"), (3, "C"), (4, "D")):
        add("slot%d_inv" % i, "Slot %s Polarity" % L_, options=["Normal", "Invert"], default=0)
    add("lock_note", "Lock Note", min=0, max=127, default=60, display="int", dynamic_display=True, live=True)
    add("lock_mode", "Lock Playback", options=["Slot"] + PLAY_MODES, default=0, live=True)
    add("lock_inv", "Lock Polarity", options=["Slot", "Normal", "Invert"], default=0, live=True)
    num("lock_tune", "Lock Tune", -24, 24, 0, "st", live=True)
    num("lock_fine", "Lock Fine", -100, 100, 0, "ct", live=True)
    num("lock_start", "Lock Start", 0, 95, 0, "%", live=True)
    add("lock_cut", "Lock Cutoff", min=-100, max=100, default=0, display="int", dynamic_display=True, live=True)
    num("lock_level", "Lock Level", -24, 12, 0, "dB", live=True)
    add("lock_pan", "Lock Pan", min=-100, max=100, default=0, display="int", dynamic_display=True, live=True)
    add("lock_vel", "Lock Velocity", min=0, max=127, default=0, display="int", dynamic_display=True, live=True)
    trigger("lock_clear", "Clear Lock")
    text("lock_info", "Lock")
    add("pad_follow", "Pad Follow", options=["Off", "On"], default=1)
    for p in range(1, 17):
        add("plock_%d" % p, "Pad %d Lock" % p, min=0, max=3, default=0, display="int", live=True)
    # 1.8.2: FX's second page (MPC's sub-page, MOD FX): phaser, flanger, tremolo / auto-pan, lo-fi (sound settings, like
    # the FX page; appended so no index moves)
    num("phs_mix", "Phaser Mix", 0, 100, 0, "%")
    add("phs_rate", "Phaser Rate", min=0, max=100, default=30, display="int", dynamic_display=True)
    num("phs_depth", "Phaser Depth", 0, 100, 60, "%")
    num("phs_fb", "Phaser Feedback", 0, 90, 40, "%")
    num("flg_mix", "Flanger Mix", 0, 100, 0, "%")
    add("flg_rate", "Flanger Rate", min=0, max=100, default=20, display="int", dynamic_display=True)
    num("flg_depth", "Flanger Depth", 0, 100, 50, "%")
    num("flg_fb", "Flanger Feedback", 0, 90, 50, "%")
    num("trm_depth", "Tremolo Depth", 0, 100, 0, "%")
    add("trm_rate", "Tremolo Rate", min=0, max=100, default=60, display="int", dynamic_display=True)
    add("trm_sync", "Tremolo Sync", options=LFO_SYNC, default=0)
    add("trm_mode", "Tremolo Mode", options=TREM_MODES, default=0)
    add("trm_shape", "Tremolo Shape", options=TREM_SHAPES, default=0)
    num("lofi_mix", "Lo-Fi Mix", 0, 100, 0, "%")
    num("lofi_bits", "Lo-Fi Bits", 2, 16, 8, "bit")
    add("lofi_rate", "Lo-Fi Rate", min=0, max=100, default=40, display="int", dynamic_display=True)
    # settings an extracted instrument restores reach MPC's controls through the live polling
    for p in P:
        if p["key"] in SOUND_KEYS:
            p["live"] = True
    # last, so every earlier index (saved projects, Q-Link maps) stays put: pressing the data wheel on BROWSE opens or
    # loads the highlighted row (the "wheel" field over the folder line)
    trigger("br_enter", "Browser Open")
    # the data wheel's own control (the "wheel" field over the folder line), separate from Q-Link 1's br_row: it moves
    # the highlight. refresh_display: the engine puts it back on "-" after each step and MPC must read that back
    add("br_wheel", "Browser Wheel", options=["Up", "-", "Down"], default=1, live=True, refresh_display=True)
    # 1.8.13 rack look: BROWSE's LEDs (read-only): an instrument is loading / a note is sounding
    add("led_busy", "Loading LED", min=0, max=1, default=0, display="int", live=True)
    add("led_notes", "Notes LED", min=0, max=1, default=0, display="int", live=True)
    # 1.8.13: the skin (SETUP): which look the layout shows; a project keeps it, an instrument / patch does not
    # 2.1.1: always present, so index 354 stays "skin" as params.lock has it (a skins build saved projects with it); with
    # one skin it does nothing and SETUP has no switch
    if SKIN_SWITCH:
        add("skin", "Skin", options=[n for _, n, _ in SKINS], default=len(SKINS) - 1)
    else:
        add("skin", "Skin", options=[SKINS[0][1], "-"], default=0)
    # 2.1.1: HQ SINC (SETUP): 8-point sinc interpolation, over Interpolation's choice. A param of its own, appended:
    # a fourth Interpolation option would rescale that list's saved 0..1 values (an old project's Cubic would load as Sinc)
    add("sinc", "HQ Sinc", options=["Off", "On"], default=0, live=True)
    # 2.1.3: SAMPLE FOLDERS (BROWSE): Hide leaves an instrument's own sample folder out of the list (a Heat Up preset's,
    # Kontakt's "<name> Samples"...); Show lists every folder. Appended (params.lock)
    add("br_folders", "Sample Folders", options=["Hide", "Show"], default=0)
    # 2.1.5: the logo's golden spark: read-only, the engine picks the frame from the clock (a 10 s loop). Appended
    add("logo_fx", "Logo Spark", min=0, max=127, default=0, display="int", live=True)
    # 2.1.16: Rev (PADS): Full = the whole sample back to its start whatever the note-off (a pad tap plays it all);
    # Gated = the note-off releases it like Fwd. One setting for every Rev, slot or lock: appended, so no index or
    # option count moves
    add("rev_mode", "Rev Mode", options=["Full", "Gated"], default=0)
    # 2.1.16: RESET (SETUP): two taps put Omni back as a new instance has it; reset_info says what the first tap armed
    trigger("reset_all", "Reset Omni")
    text("reset_info", "Reset")
    # 2.2: the reverb's type (FX): Classic = Freeverb as before; Plate / Hall / Room = Dattorro's plate (dsp.hpp
    # PlateReverb), and its pre-delay. Appended
    add("rev_type", "Reverb Type", options=REV_TYPES, default=0)
    num("rev_predelay", "Reverb Pre-Delay", 0, 250, 10, "ms")
    # 2.2: the drive's type (FX, a list under DRIVE): Off by default, Classic = the drive as before, Tape / Tube / Fold;
    # all at twice the sample rate (dsp.hpp Drive). Appended
    add("drive_type", "Drive Type", options=DRIVE_TYPES, default=0)
    # 2.2: the delay's type (FX, keys under its knobs): Off by default (no key lit), Classic = the delay as before, Tape,
    # Analog (dsp.hpp Delay). Appended
    add("dly_type", "Delay Type", options=DELAY_TYPES, default=0)
    # 2.2: the chorus's type (FX, keys under its knobs): Off by default (no key lit), Classic = the chorus as before, and
    # the ensemble's three modes (dsp.hpp Ensemble). Appended
    add("cho_type", "Chorus Type", options=CHORUS_TYPES, default=0)
    # 2.2: the compressor's type (FX, a list under AMOUNT): Off by default, Classic = as before, Glue, Punch (dsp.hpp
    # BusComp); and the output's lookahead limiter (dsp.hpp Limiter), off by default. Appended
    add("comp_type", "Compressor Type", options=COMP_TYPES, default=0)
    add("limiter", "Limiter", options=["Off", "On"], default=0)
    # 2.2: the GR meter (FX, beside AMOUNT): the compressor's and the limiter's gain reduction together, 0..24 dB as
    # 0..127; read-only, the engine reports it as it moves. Appended
    add("gr_meter", "Gain Reduction", min=0, max=127, default=0, display="int", live=True)
    # 2.3: the SYNTH layer (the SYNTH tab): SYNTH ON plays it with slots A-D; two oscillators, OSC A (sa_) and OSC B (sb_):
    # a type, the index in that type's list (Classic: the factory waves; Wavetable: the factory tables, then the
    # Library's Oscillators folder), the OSC page's knobs, phase and warp modes, < > LOAD, the name and frame readouts
    # and the screen's 32 bars. Appended
    add("syn_on", "Synth", options=["Off", "On"], default=0)
    num("syn_level", "Synth Level", -24, 6, 0, "dB")
    add("syn_poly", "Synth Voices", min=1, max=12, default=8, display="int")
    num("syn_attack", "Synth Attack", 0, 10000, 2, "ms")
    num("syn_decay", "Synth Decay", 1, 10000, 300, "ms")
    num("syn_sustain", "Synth Sustain", 0, 100, 100, "%")
    num("syn_release", "Synth Release", 1, 10000, 300, "ms")
    for x, X in (("a", "A"), ("b", "B")):
        add("s%s_on" % x, "Osc %s" % X, options=["Off", "On"], default=1 if x == "a" else 0)
        add("s%s_type" % x, "Osc %s Type" % X, options=SYN_TYPES, default=1 if x == "a" else 0)
        add("s%s_table" % x, "Osc %s Table" % X, min=0, max=4095, default=0 if x == "a" else 2, display="int")   # 2.3.2: 255 -> 4095
        num("s%s_pos" % x, "Osc %s Position" % X, 0, 100, 0, "", dynamic_display=True)   # 2.4.9: the frame / "n%" (plugin.cpp); no unit, as the other dynamic_display params
        num("s%s_detune" % x, "Osc %s Detune" % X, 0, 100, 20, "ct")
        num("s%s_spread" % x, "Osc %s Spread" % X, 0, 100, 50, "%")
        num("s%s_warp" % x, "Osc %s Warp" % X, 0, 100, 0, "%")
        add("s%s_pan" % x, "Osc %s Pan" % X, min=-100, max=100, default=0, display="int")
        num("s%s_level" % x, "Osc %s Level" % X, 0, 100, 80, "%")
        add("s%s_oct" % x, "Osc %s Octave" % X, min=-4, max=4, default=0, display="int")
        add("s%s_semi" % x, "Osc %s Semi" % X, min=-12, max=12, default=0, display="int")
        num("s%s_fine" % x, "Osc %s Fine" % X, -100, 100, 0, "ct")
        num("s%s_phase" % x, "Osc %s Phase" % X, 0, 360, 0, "deg")
        num("s%s_rand" % x, "Osc %s Random" % X, 0, 100, 0, "%")
        add("s%s_unison" % x, "Osc %s Unison" % X, min=1, max=8, default=1, display="int")
        add("s%s_pmode" % x, "Osc %s Phase Mode" % X, options=SYN_PMODES, default=0)
        add("s%s_wmode" % x, "Osc %s Warp Mode" % X, options=SYN_WMODES, default=0)
        trigger("s%s_prev" % x, "Osc %s Previous" % X)
        trigger("s%s_next" % x, "Osc %s Next" % X)
        trigger("s%s_load" % x, "Osc %s Load" % X)
        text("s%s_name" % x, "Osc %s Name" % X)
        text("s%s_info" % x, "Osc %s Info" % X)
        for b in range(1, SYN_BARS + 1):
            add("s%s_w%d" % (x, b), "Osc %s Bar %d" % (X, b), min=0, max=127, default=64, display="int", live=True)
    # 2.3.1: the SYNTH tab's MIX / FILTER page; the filter envelope (its knobs come with the ENV page). Appended
    add("syn_sub_on", "Sub", options=["Off", "On"], default=0)
    add("syn_sub_shape", "Sub Shape", options=SYN_SUB_SHAPES, default=0)
    add("syn_sub_oct", "Sub Octave", options=["-1 Oct", "-2 Oct"], default=0)
    num("syn_sub_level", "Sub Level", 0, 100, 60, "%")
    add("syn_sub_pan", "Sub Pan", min=-100, max=100, default=0, display="int")
    add("syn_noise_on", "Noise", options=["Off", "On"], default=0)
    add("syn_noise_color", "Noise Colour", options=SYN_NOISE_COLS, default=1)
    add("syn_noise_key", "Noise Pitch", options=["Free", "Follow Key"], default=0)
    num("syn_noise_level", "Noise Level", 0, 100, 30, "%")
    add("syn_noise_pan", "Noise Pan", min=-100, max=100, default=0, display="int")
    num("syn_noise_tone", "Noise Tone", 0, 100, 70, "%")
    num("syn_fm", "FM B to A", 0, 100, 0, "%")
    num("syn_feedback", "Feedback", 0, 100, 0, "%")
    num("syn_ring", "Ring", 0, 100, 0, "%")
    add("syn_balance", "Balance", min=-100, max=100, default=0, display="int")
    add("syn_sync", "Sync B to A", options=["Off", "On"], default=0)
    add("syn_flt_on", "Synth Filter", options=["Off", "On"], default=0)
    add("syn_flt_type", "Synth Filter Type", options=SYN_FTYPES, default=1)
    add("syn_cutoff", "Synth Cutoff", min=0, max=100, default=75, display="int", dynamic_display=True)
    num("syn_res", "Synth Resonance", 0, 100, 0, "%")
    num("syn_drive", "Synth Drive", 0, 100, 0, "%")
    num("syn_keytrack", "Key Track", 0, 100, 0, "%")
    add("syn_fenv", "Filter Env Amount", min=-100, max=100, default=0, display="int")
    num("syn_fvel", "Filter Velocity", 0, 100, 0, "%")
    num("syn_fat", "Fat", 0, 100, 0, "%")
    num("syn_fmix", "Filter Mix", 0, 100, 100, "%")
    for k, n in (("a", "Osc A"), ("b", "Osc B"), ("sub", "Sub"), ("noise", "Noise")):
        add("syn_fr_%s" % k, "%s to Filter" % n, options=["Out", "In"], default=0 if k == "sub" else 1)
    add("syn_pan", "Synth Pan", min=-100, max=100, default=0, display="int")
    num("syn_fe_attack", "Filter Env Attack", 0, 10000, 1, "ms")
    num("syn_fe_decay", "Filter Env Decay", 1, 10000, 450, "ms")
    num("syn_fe_sustain", "Filter Env Sustain", 0, 100, 20, "%")
    num("syn_fe_release", "Filter Env Release", 1, 10000, 300, "ms")
    # 2.3.2: the ENV / LFO page: the mod envelope, two LFOs, four macros (each with a destination until the MATRIX page);
    # the envelope screens' bars. Appended
    for x, X in (("a", "A"), ("b", "B")):   # 2.3.2: FOLDER (the next folder of the list)
        trigger("s%s_folder" % x, "Osc %s Folder" % X)
    add("syn_me_on", "Mod Env", options=["Off", "On"], default=0)
    num("syn_me_attack", "Mod Env Attack", 0, 10000, 400, "ms")
    num("syn_me_decay", "Mod Env Decay", 1, 10000, 500, "ms")
    num("syn_me_sustain", "Mod Env Sustain", 0, 100, 0, "%")
    num("syn_me_release", "Mod Env Release", 1, 10000, 500, "ms")
    add("syn_me_dest", "Mod Env Destination", options=SYN_ALL_DESTS, default=0)   # 2.4.9: was SYN_DESTS
    add("syn_me_amt", "Mod Env Amount", min=-100, max=100, default=50, display="int")
    for n in (1, 2):
        add("syn_lfo%d_shape" % n, "Synth LFO %d Shape" % n, options=SYN_LFO_SHAPES, default=1 if n == 1 else 4)
        add("syn_lfo%d_rate" % n, "Synth LFO %d Rate" % n, min=0, max=100, default=50, display="int", dynamic_display=True)
        add("syn_lfo%d_sync" % n, "Synth LFO %d Sync" % n, options=LFO_SYNC, default=0)
        add("syn_lfo%d_trig" % n, "Synth LFO %d Trigger" % n, options=SYN_LFO_TRIGS, default=1)
        add("syn_lfo%d_dest" % n, "Synth LFO %d Destination" % n, options=SYN_DESTS, default=0)
        num("syn_lfo%d_depth" % n, "Synth LFO %d Depth" % n, 0, 100, 50, "%")
    for n in (1, 2, 3, 4):
        num("syn_macro%d" % n, "Macro %d" % n, 0, 100, 0, "%")
    for n in (1, 2, 3, 4):
        add("syn_macro%d_dest" % n, "Macro %d Destination" % n, options=SYN_ALL_DESTS, default=0)   # 2.4.9: was SYN_DESTS
    for e in ("a", "f", "m"):
        for b in range(1, ENV_BARS + 1):
            add("syn_%se_b%d" % (e, b), "Env %s Bar %d" % (e.upper(), b), min=0, max=127, default=0, display="int", live=True)
    # 2.3.3: VOICE and MATRIX. Appended (after the 2.3.2 envelope bars: params.lock)
    add("syn_vmode", "Synth Voice Mode", options=SYN_VMODES, default=0)
    num("syn_glide", "Synth Glide", 0, 2000, 0, "ms")
    add("syn_glide_mode", "Synth Glide Mode", options=["Always", "Legato"], default=0)
    add("syn_bend", "Synth Bend Range", min=0, max=24, default=2, display="int")
    num("syn_vel", "Synth Velocity", 0, 100, 100, "%")
    add("syn_key_lo", "Synth Low Key", min=0, max=127, default=0, display="int", dynamic_display=True)
    add("syn_key_hi", "Synth High Key", min=0, max=127, default=127, display="int", dynamic_display=True)
    add("syn_transpose", "Synth Transpose", min=-24, max=24, default=0, display="int")
    num("syn_drift", "Synth Drift", 0, 100, 0, "%")
    for n in range(1, 9):
        add("syn_mx%d_src" % n, "Route %d Source" % n, options=SYN_MX_SRCS, default=0)
        add("syn_mx%d_dst" % n, "Route %d Destination" % n, options=SYN_ALL_DESTS, default=0)   # 2.4.9: was SYN_MX_DESTS
        add("syn_mx%d_amt" % n, "Route %d Amount" % n, min=-100, max=100, default=0, display="int")
    # 2.3.3: the steppers' arrows (a stepper in place of a drop-down: its list costs no images). Appended
    for k in STEPPED:
        trigger(k + "_prev", k + " previous")
        trigger(k + "_next", k + " next")
    # 2.3.5: the synth's factory presets (plugin.cpp SYN_PRESETS; max leaves room for more without moving saves). Appended
    add("syn_preset", "Synth Preset", min=0, max=255, default=0, display="int", dynamic_display=True)
    trigger("syn_preset_prev", "Synth Preset previous")
    trigger("syn_preset_next", "Synth Preset next")
    # 2.3.6: each LFO's route: one of the three envelopes' sections (AMP: level, FILTER: cutoff, MOD: the mod env's
    # destination), three keys in place of the destination arrows; Off keeps the older destination. Appended
    for n in (1, 2):
        add("syn_lfo%d_route" % n, "Synth LFO %d Route" % n, options=SYN_LFO_ROUTES, default=0)
    # 2.4: the ARP tab: a 64-step pattern (pages A-D of 16) from a MIDI file or drawn on the grid, played as a sequence
    # transposed by the key held (SEQ) or as an arpeggio of the keys held (ARP). Appended
    add("arp_on", "Arp", options=["Off", "On"], default=0)
    add("arp_mode", "Arp Mode", options=ARP_MODES, default=0)
    add("arp_rate", "Arp Rate", options=ARP_RATES, default=2)
    add("arp_length", "Arp Length", min=1, max=64, default=16, display="int")
    num("arp_swing", "Arp Swing", 0, 100, 0, "%")
    num("arp_gate", "Arp Gate", 5, 100, 60, "%")
    add("arp_latch", "Arp Latch", options=["Off", "On"], default=0)
    add("arp_scale", "Arp Scale", options=ARP_SCALES, default=1)
    add("arp_root", "Arp Root", options=ARP_ROOTS, default=0)
    add("arp_page", "Arp Page", options=ARP_PAGES, default=0)
    trigger("arp_prev", "Arp MIDI File previous")
    trigger("arp_next", "Arp MIDI File next")
    trigger("arp_load", "Arp Load MIDI File")
    trigger("arp_clear", "Arp Clear Page")
    text("arp_name", "Arp MIDI File")
    text("arp_info", "Arp Info")
    for r in range(1, ARP_ROWS + 1):
        text("arp_row%d" % r, "Arp Row %d" % r)
    for c in range(1, ARP_COLS + 1):
        add("arp_led%d" % c, "Arp Step %d Light" % c, min=0, max=127, default=0, display="int", live=True)
    for r in range(1, ARP_ROWS + 1):   # the grid as shown (the page's 16 steps): a tap toggles a dot
        for c in range(1, ARP_COLS + 1):
            # 2.4.1: a picture code (plugin.cpp arp_cell_code, strip_arp_cell); a tap is MPC's toggle sending 0 / 127
            add("arp_c%d_%d" % (r, c), "Arp Row %d Step %d" % (r, c), min=0, max=127, default=0, display="int", live=True)
    # 2.4.1 (appended: params.lock)
    add("arp_order", "Arp Order", options=ARP_ORDERS, default=0)
    add("arp_oct", "Arp Octaves", options=ARP_OCTS, default=0)
    add("arp_snap", "Arp MIDI Snap", options=ARP_SNAPS, default=0)
    add("arp_tie", "Arp Tie", options=["Off", "On"], default=0)
    num("arp_strum", "Arp Strum", 0, 100, 0, "%")
    add("arp_strum_dir", "Arp Strum Direction", options=ARP_STRUM_DIRS, default=0)
    add("arp_draw", "Arp Grid Touch", options=ARP_DRAWS, default=0)
    add("arp_follow", "Arp Page Follow", options=["Off", "On"], default=1)
    trigger("arp_shift_l", "Arp Shift Left")
    trigger("arp_shift_r", "Arp Shift Right")
    text("arp_status", "Arp Status")
    add("arp_xy_x", "Arp Draw X", min=0, max=1, default=0, live=True)
    add("arp_xy_y", "Arp Draw Y", min=0, max=1, default=0, live=True)
    add("arp_octave", "Arp Octave", min=-3, max=3, default=0, display="int")
    num("arp_note_gate", "Arp Note Gate", 5, 100, 60, "%")
    # ---- LFO ROUTE keys (appended: keep after everything else). Each LFO's AMP / FILTER / MOD key is a Toggle on its own
    # param, lit while syn_lfo<n>_route is that route; a tap on the lit key turns the route Off (a radio group can't).
    # Not in the plugin state (plugin.cpp lfo_route_tap derives them from the route); live: the plugin reports them
    for n in (1, 2):
        for rk, opt in zip(SYN_LFO_RKEYS, SYN_LFO_ROUTES[1:]):
            add("syn_lfo%d_rk_%s" % (n, rk), "Synth LFO %d Route %s" % (n, opt), min=0, max=1, default=0, live=True)
    # ---- end LFO ROUTE keys
    # 2.4.2 (appended): the selected note's expression, and arp_sel (a note is selected: the plugin sets it; the ARP
    # page shows these knobs instead of the pattern's while it is On)
    num("arp_n_vel", "Arp Note Velocity", 1, 200, 100, "%")
    num("arp_n_chance", "Arp Note Chance", 0, 100, 100, "%")
    add("arp_n_ratchet", "Arp Note Ratchet", min=1, max=4, default=1, display="int")
    add("arp_n_oct", "Arp Note Octave", min=-2, max=2, default=0, display="int")
    add("arp_n_slide", "Arp Note Slide", options=["Off", "On"], default=0)
    add("arp_sel", "Arp Note Selected", options=["Off", "On"], default=0, live=True)
    # 2.4.2 (appended): the GEN page: GROOVE and MELODY
    add("arp_dir", "Arp Direction", options=ARP_DIRS, default=0)
    num("arp_h_time", "Arp Humanize Time", 0, 100, 0, "%")
    num("arp_h_vel", "Arp Humanize Velocity", 0, 100, 0, "%")
    for c in range(1, 5):
        add("arp_chain%d" % c, "Arp Chain %d" % c, options=ARP_CHAIN, default=0)
    add("arp_e_hits", "Arp Euclid Hits", min=0, max=64, default=5, display="int")
    add("arp_e_rotate", "Arp Euclid Rotate", min=0, max=63, default=0, display="int")
    add("arp_g_contour", "Arp Gen Contour", options=ARP_CONTOURS, default=2)
    num("arp_g_density", "Arp Gen Density", 5, 100, 55, "%")
    add("arp_g_range", "Arp Gen Range", min=2, max=16, default=8, display="int")
    num("arp_g_sync", "Arp Gen Syncopation", 0, 100, 25, "%")
    num("arp_g_mutate", "Arp Gen Mutate", 0, 100, 30, "%")
    trigger("arp_g_generate", "Arp Generate")
    for k, lab in ARP_GEN_KEYS:
        trigger(k, "Arp " + lab.title())
    trigger("arp_e_apply", "Arp Euclid Apply")
    # 2.4.6 (appended: params.lock): the keyboard's velocity curve (SETUP, PLAYBACK; plugin.cpp: not a sound setting)
    add("vel_curve", "Velocity Curve", options=VEL_CURVES, default=1)
    # 2.4.9 (appended: params.lock): the GRID's SHIFT up / down (the selected note, else the pattern, a row)
    trigger("arp_shift_u", "Arp Shift Up")
    trigger("arp_shift_d", "Arp Shift Down")
    # 2.4.9 (appended: params.lock): the OSC screens' < FOLDER, RANDOM (this folder) and RND ALL (plugin.cpp TRIGGERS)
    for x in ("a", "b"):
        X = x.upper()
        trigger("s%s_fprev" % x, "Osc %s Prev Folder" % X)
        trigger("s%s_rnd" % x, "Osc %s Random" % X)
        trigger("s%s_rndall" % x, "Osc %s Random All" % X)
    # 2.4.11 (appended: params.lock): GENERATE's ARTICULATION / EXPRESSION / GHOSTS + FILLS (plugin.cpp arp_articulate; 50 = natural)
    num("arp_g_artic", "Arp Gen Articulation", 0, 100, 50, "%")
    num("arp_g_expr", "Arp Gen Expression", 0, 100, 50, "%")
    num("arp_g_ghost", "Arp Gen Ghosts", 0, 100, 50, "%")
    # 2.4.12 (appended: params.lock): MUTATE / BREED (SYNTH > VOICE) and the GRID's BREED (ARP > GEN; plugin.cpp mut_action)
    add("mut_amount", "Mutate Amount", options=MUT_AMOUNTS, default=1)
    for k, lab in MUT_KEYS:
        trigger(k, lab.title())
    for k, lab in ARP_BREED_KEYS:
        trigger(k, "Arp Breed " + lab.title())
    # 2.4.13 (appended: params.lock): a CUSTOM LFO's shape: its name, < > through <Library>/LFOs (plugin.cpp syn_lfo_step_now)
    for n in (1, 2):
        text("syn_lfo%d_file" % n, "Synth LFO %d Custom Shape" % n)
        trigger("syn_lfo%d_file_prev" % n, "Synth LFO %d Shape previous" % n)
        trigger("syn_lfo%d_file_next" % n, "Synth LFO %d Shape next" % n)
    # 2.4.14 (appended: params.lock): the GRID's chords: the chord readout and VOICE (inversions / spread in key and scale)
    text("arp_chord", "Arp Chord")
    add("arp_v_mode", "Arp Voicing", options=ARP_VOICES, default=0)
    num("arp_v_span", "Arp Voicing Span", 12, 48, 24, "st")
    trigger("arp_v_go", "Arp Voicing Apply")
    # 2.4.17 (appended: params.lock): the SYNTH's MOD 2 page (MOD ENV 2 / 3, LFO 3 / 4) and MATRIX 2 (routes 9-16).
    # Defaults = plugin.cpp NUM_PARAMS. The envelope screens' bars: syn_ne_b* (MOD ENV 2), syn_oe_b* (MOD ENV 3)
    for m in (2, 3):
        add("syn_me%d_on" % m, "Mod Env %d" % m, options=["Off", "On"], default=0)
        num("syn_me%d_attack" % m, "Mod Env %d Attack" % m, 0, 10000, 400, "ms")
        num("syn_me%d_decay" % m, "Mod Env %d Decay" % m, 1, 10000, 500, "ms")
        num("syn_me%d_sustain" % m, "Mod Env %d Sustain" % m, 0, 100, 0, "%")
        num("syn_me%d_release" % m, "Mod Env %d Release" % m, 1, 10000, 500, "ms")
        add("syn_me%d_dest" % m, "Mod Env %d Destination" % m, options=SYN_ALL_DESTS, default=0)
        add("syn_me%d_amt" % m, "Mod Env %d Amount" % m, min=-100, max=100, default=50, display="int")
        trigger("syn_me%d_dest_prev" % m, "syn_me%d_dest previous" % m)
        trigger("syn_me%d_dest_next" % m, "syn_me%d_dest next" % m)
    for n in (3, 4):
        add("syn_lfo%d_shape" % n, "Synth LFO %d Shape" % n, options=SYN_LFO_SHAPES, default=0 if n == 3 else 2)
        add("syn_lfo%d_rate" % n, "Synth LFO %d Rate" % n, min=0, max=100, default=50, display="int", dynamic_display=True)
        add("syn_lfo%d_sync" % n, "Synth LFO %d Sync" % n, options=LFO_SYNC, default=0)
        add("syn_lfo%d_trig" % n, "Synth LFO %d Trigger" % n, options=SYN_LFO_TRIGS, default=1)
        num("syn_lfo%d_depth" % n, "Synth LFO %d Depth" % n, 0, 100, 50, "%")
        add("syn_lfo%d_route" % n, "Synth LFO %d Route" % n, options=SYN_LFO_ROUTES, default=0)
        for rk, opt in zip(SYN_LFO_RKEYS, SYN_LFO_ROUTES[1:]):
            add("syn_lfo%d_rk_%s" % (n, rk), "Synth LFO %d Route %s" % (n, opt), min=0, max=1, default=0, live=True)
        text("syn_lfo%d_file" % n, "Synth LFO %d Custom Shape" % n)
        trigger("syn_lfo%d_file_prev" % n, "Synth LFO %d Shape previous" % n)
        trigger("syn_lfo%d_file_next" % n, "Synth LFO %d Shape next" % n)
    for e in ("n", "o"):
        for b in range(1, ENV_BARS + 1):
            add("syn_%se_b%d" % (e, b), "Env %s Bar %d" % (e.upper(), b), min=0, max=127, default=0, display="int", live=True)
    for n in range(9, 17):
        add("syn_mx%d_src" % n, "Route %d Source" % n, options=SYN_MX_SRCS, default=0)
        add("syn_mx%d_dst" % n, "Route %d Destination" % n, options=SYN_ALL_DESTS, default=0)
        add("syn_mx%d_amt" % n, "Route %d Amount" % n, min=-100, max=100, default=0, display="int")
    # 2.4.30 (appended: params.lock): FX BANK. Defaults = plugin.cpp NUM_PARAMS; units as the engine's Rack takes them
    # (rate: 0..100 -> 0.05..40 Hz, fxb_rate_display)
    add("fxb_prog", "FX Bank Program", options=FXB_OPTIONS, default=0, live=True)
    num("fxb_mix", "FX Bank Mix", 0, 100, 50, "%", live=True)
    add("fxb_rate", "FX Bank Rate", min=0, max=100, default=39, display="int", dynamic_display=True, live=True)
    num("fxb_depth", "FX Bank Depth", 0, 100, 65, "%", live=True)
    num("fxb_fb", "FX Bank Feedback", -94, 94, 35, "%", live=True)
    num("fxb_time", "FX Bank Time", 1, 1600, 320, "ms", live=True)
    num("fxb_tone", "FX Bank Tone", 0, 100, 55, "%", live=True)
    num("fxb_drive", "FX Bank Drive", 0, 36, 12, "dB", live=True)
    num("fxb_thr", "FX Bank Threshold", -80, 0, -24, "dB", live=True)
    num("fxb_ratio", "FX Bank Ratio", 1, 40, 4, ":1", live=True)
    num("fxb_pitch", "FX Bank Pitch", -24, 24, 7, "st", live=True)
    num("fxb_low", "FX Bank Low", -24, 18, 0, "dB", live=True)
    num("fxb_mid", "FX Bank Mid", -24, 18, 0, "dB", live=True)
    num("fxb_high", "FX Bank High", -24, 18, 0, "dB", live=True)
    add("fxb_bypass", "FX Bank Bypass", options=FXB_BYPASS, default=0, live=True)
    text("fxb_info", "FX Bank Card")
    trigger("fxb_prog_prev", "FX Bank Program previous")   # the OLED's < > (118 programs: no drop-down list fits the screen)
    trigger("fxb_prog_next", "FX Bank Program next")
    # 2.4.32 (appended: params.lock): the 3-digit program screen and its keypad (plugin.cpp fxb_entry)
    add("fxb_pad", "FX Bank Keypad", options=["Closed", "Open"], default=0, live=True)
    text("fxb_num", "FX Bank Program Number")
    trigger("fxb_num_tap", "FX Bank Program Number tap")
    for d_ in range(10):
        trigger("fxb_k%d" % d_, "FX Bank Key %d" % d_)
    trigger("fxb_kclr", "FX Bank Key CLR")
    trigger("fxb_kenter", "FX Bank Key ENTER")
    # 2.4.33 (appended): the bay's slot keys A-D and each expansion slot's program; fxb_pname = the OLED's top line
    add("fxb_slot", "FX Bank Slot", options=FXB_SLOTS, default=0, live=True)
    for sl_ in "bcd":
        add("fxb_prog_%s" % sl_, "FX Bank Slot %s Program" % sl_.upper(), min=0, max=999, default=0, display="int", live=True)   # 2.4.35: was 256
    text("fxb_pname", "FX Bank Program Name")
    # 2.0 (OMNI_PUBLIC=1, the public release): public 1.8.1 ended with br_enter, br_wheel at 297/298; the private line
    # (1.8.12, the user's MPC) inserted its new params before them and was relocked. The public build keeps 1.8.1's
    # order so public users' projects restore: those two go back right after mod8_slot, everything newer follows.
    if os.environ.get("OMNI_PUBLIC") == "1":
        moved = [p for p in P if p["key"] in ("br_enter", "br_wheel")]
        rest = [p for p in P if p["key"] not in ("br_enter", "br_wheel")]
        at = next(i for i, p in enumerate(rest) if p["key"] == "mod8_slot") + 1
        P[:] = rest[:at] + moved + rest[at:]
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
        self.ops = None   # 1.8.13: a page background in the rack look: the rack parts, drawn under the canvas at save()
        if RACK and tuple(fill) == tuple(BG) + (255,):
            self.ops, fill = [], (0, 0, 0, 0)
        self.im = Image.new("RGBA", (w * SS, h * SS), fill)
        self.d = ImageDraw.Draw(self.im)

    def S(self, *v):
        return [round(x * SS) for x in v]

    def rrect(self, x, y, w, h, r, fill=None, outline=None, width=1):
        if self.ops is not None and fill is not None and tuple(fill[:3]) in (tuple(DARK[:3]), (14, 17, 22)):   # a well: glass
            self.ops.append(("glass", x, y, w, h, min(r, 8)))
            return
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
        if self.ops is not None and tuple(fill[:3]) in (tuple(INK), tuple(INK_DIM)):
            import rack
            painted = any(op[0] == "module" and len(op) > 5 and op[5] and op[1] <= x <= op[1] + op[3] and op[2] <= y <= op[2] + op[4]
                          for op in self.ops)
            if rack.LIGHT and not painted:   # 1.8.13: on a light module the lettering is printed dark
                fill = rack.PRINT if tuple(fill[:3]) == tuple(INK) else rack.PRINT_DIM
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

    def final(self):
        """the finished image at plugin size (in the rack look: the plate and parts under the canvas)"""
        im = self.im.resize((self.w, self.h), Image.LANCZOS)
        return rack_page(self.ops, im) if self.ops is not None else im

    def save(self, name):
        self.final().save(os.path.join(ART, name + ".png"))


# 1.8.13: each section a built-in hardware module, painted by what it does (the user: "each section to look like built
# in hardware modules" in different colours); untitled ones (LAYERS' slots) are given their colour
SECTION_COLOURS = [(("PITCH",), (70, 132, 255)),
                   (("VOICE", "PLAYBACK", "OUTPUT"), (40, 190, 190)),
                   (("REVERB", "CHORUS", "DELAY", "PHASER", "FLANGER", "TREMOLO", "LO-FI", "EFFECTS"), (160, 110, 255)),
                   (("FILTER", "EQ", "COMPRESSOR"), (255, 150, 50)),
                   (("ENVELOPE", "LFO", "NOTE LOCK"), (235, 72, 84)),
                   (("MOD MATRIX", "LAYER MAP", "LOCATIONS", "NOTES", "ABOUT"), (130, 138, 150))]


def section_colour(title):
    if not MODULES:
        return None
    t = (title or "").upper()
    for keys, col in SECTION_COLOURS:
        if any(t.startswith(k) for k in keys):
            return col
    return (130, 138, 150) if t else None   # the formats lists etc.: graphite


def body(c, x, y, w, h, r=14, colour=None):
    """The brushed panel: a vertical gradient, faint horizontal grain, a light top edge and a dark border."""
    if c.ops is not None:   # 1.8.13: a raised, screwed-down module (painted when it has a colour)
        c.ops.append(("module", x, y, w, h, colour))
        return
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
    import rack
    if RACK and rack.STUDIO:   # 1.8.14: a silkscreened tag: a thin printed outline, the legend in light grey
        c.rrect(cx - tw / 2 - 9, cy - 10, tw + 18, 20, 3, outline=(120, 126, 136, 255), width=1)
        c.text(cx, cy + 1, label, 12, (196, 201, 209), "SemiBold")
        return
    c.rrect(cx - tw / 2 - 9, cy - 11, tw + 18, 22, 5, fill=(18, 40, 58), outline=rgba(colour, 200), width=1.2)
    c.text(cx, cy + 1, label, 12, colour, "SemiBold")


def section_title(c, cx, cy, title, tag=None, tag_x=None):
    c.text(cx, cy, title, 20, INK, "SemiBold")
    if tag:
        chip(c, tag_x, cy, tag)


def title_glow(c, cx, cy, text, size):
    c.glow(lambda l: l.text(cx, cy, text, size, CYAN, "Title", spacing=3), 6, 1.6)


def topbar(c, left_label=None):
    if c.ops is not None:   # 1.8.13
        c.ops.append(("header", left_label))
        return
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


def panel(c, x, y, w, h, title=None, title_y=None, colour=None):
    body(c, x, y, w, h, 12, colour or (section_colour(title) if c.ops is not None else None))
    if title and c.ops is not None:
        c.ops.append(("title", x + w / 2, title_y or y + 26, title, x, x + w))
    elif title:
        c.text(x + w / 2, title_y or y + 26, title, 19, INK, "SemiBold")


def knob_label(c, cx, cy, label):
    if c.ops is not None:   # 1.8.13: printed lettering
        c.ops.append(("label", cx, cy, label))
        return
    c.text(cx, cy, label, 13, INK_DIM, "SemiBold")


WAVE_X0, WAVE_STEP, WAVE_CY, WAVE_W, WAVE_H = 376, 11, 228, 8, 120   # bars: x of the first, spacing, centre line, size
PTILE_X0, PTILE_STEP, PTILE_CY, PTILE_W = 368, 17.5, 330, 16


def display_panel(c, x, y, w, h):
    """The centre display: dark glass, a centre line and grid for the live waveform bars, a keyboard under the zone map."""
    if c.ops is not None:   # 1.8.13: a QLED panel
        c.ops.append(("glass", x, y, w, h, 8, True))
    else:
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
# 1.7.1: the faders show the envelope as it plays (the preset's own until moved); the older offsets stay as hidden params
FADERS = [(996, "env_attack", "ATTACK"), (1068, "env_decay", "DECAY"), (1140, "env_sustain", "SUSTAIN"), (1212, "env_release", "RELEASE")]
FADER_CY, FADER_W, FADER_H = 346, 34, 124
SEG_GROUPS = [("filter_type", ["LP", "HP", "BP"], 360), ("voice_mode", ["Poly", "Mono", "Legato"], 570),
              ("interp", ["None", "Linear", "Cubic"], 780)]
SEG_W, SEG_H, SEG_Y = 58, 30, 84
if RACK:   # 1.8.13: the third group ran past the centre module into FILTER and the wells touched the screen's bezel
    SEG_GROUPS = [(k, o, x) for (k, o, _), x in zip(SEG_GROUPS, (362, 560, 758))]
    SEG_W, SEG_H, SEG_Y = 52, 28, 82
DISPLAY = (352, 106, 576, 334)
CHIPS = [("layer_root", 372, 112), ("layer_keys", 492, 176), ("layer_vel", 676, 126), ("layer_hit", 810, 100)]
PADS_Y, PAD_W, PAD_H = 590, 70, 50
PAD_X0, PAD_STEP = 86, 74.5


def strip_knob(name, colour, frames=65, size=96):
    """A knob filmstrip, minimum first: dark ring track, the value arc in colour (with glow), black cap, white
    pointer. 270 degrees of travel from 135 (lower left) clockwise."""
    if RACK:   # 1.8.13: FigCraft, the rack look
        import rack
        return rack.encoder_strip(os.path.join(ART, name + ".png"), colour, size, frames)
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


def strip_fader(name, w, h, frames=65, colour=(80, 150, 255)):
    """A vertical fader filmstrip (minimum first): a dark slot, a glowing blue fill up to the thumb, a metal thumb."""
    if RACK:   # 1.8.13: FigCraft, the rack look
        import rack
        return rack.fader_strip(os.path.join(ART, name + ".png"), w, h, frames, colour)
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


def strip_wave(name, w, h, frames=METER_FRAMES):
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


def strip_gr(name, w, h, frames=METER_FRAMES, segs=12):
    """2.2: the GR meter: a dark slot, `segs` segments lit from the top down (frame f: round(f * segs / 127)), amber
    turning red at the bottom (more reduction)."""
    im = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    cache = {}
    for f in range(frames):
        n = round(f * segs / (frames - 1))
        if n not in cache:
            c = Canvas(w, h)
            c.rrect(0, 0, w, h, 3, fill=(8, 10, 14, 255), outline=(60, 70, 84), width=1)
            sh = (h - 4) / segs
            for k in range(segs):
                col = tuple(round(a + (b - a) * k / (segs - 1)) for a, b in zip((255, 190, 60), (255, 70, 60)))
                top = 2 + k * sh
                if k < n:
                    c.rrect(2, top + 1, w - 4, sh - 2, 1.5, fill=rgba(col))
                else:
                    c.rrect(2, top + 1, w - 4, sh - 2, 1.5, fill=(26, 30, 38, 255))
            cache[n] = c.im.resize((w, h), Image.LANCZOS)
        im.paste(cache[n], (0, f * h))
    im.save(os.path.join(ART, name + ".png"))


def strip_states(name, w, h, states, draw, frames=METER_FRAMES):
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
    bx, by, bw, bh = BODY
    if c.ops is not None and MODULES:   # 1.8.13: each section its own hardware module
        for (x0, x1), cuts, names in ((LEFT_X, (228, 390), ("PITCH", "VOICE", "REVERB")),
                                      (RIGHT_X, (228, 446), ("FILTER", "ENVELOPE", "ENVELOPE"))):
            ys = [by] + [v + 2 for v in cuts] + [by + bh]
            for j, nm in enumerate(names):
                body(c, x0 - 8, ys[j] + (0 if j == 0 else 1), x1 - x0 + 12, ys[j + 1] - ys[j] - (3 if j < 2 else 0) - (0 if j == 0 else 1),
                     colour=section_colour(nm))
        body(c, CENTER_X[0] - 2, by, CENTER_X[1] - CENTER_X[0] + 4, bh, colour=(130, 138, 150))
    else:
        body(c, *BODY)
    split = c.ops is not None and MODULES
    for xv in (() if split else (LEFT_X[1] + 4, CENTER_X[1] + 4)):
        c.line([(xv, by + 14), (xv, by + bh - 14)], (12, 15, 19, 200), 1.5)
        c.line([(xv + 1.5, by + 14), (xv + 1.5, by + bh - 14)], (80, 90, 104, 90), 1)
    for (x0, x1), yv in (() if split else
                         (((LEFT_X[0], LEFT_X[1]), 228), ((LEFT_X[0], LEFT_X[1]), 390), ((RIGHT_X[0], RIGHT_X[1]), 228),
                          ((RIGHT_X[0], RIGHT_X[1]), 446), ((CENTER_X[0] + 8, CENTER_X[1] - 8), 450))):
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
    if RACK:   # 1.8.13: FigCraft, the rack look
        import rack
        return rack.pad_image(os.path.join(ART, name + ("_on" if lit else "_off") + ".png"), w, h, lit, tint)
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
    if RACK:   # 1.8.13: FigCraft keys
        import rack
        return rack.key_image(os.path.join(ART, name + ("_on" if lit else "_off") + ".png"), w, h, label, lit, accent, 13)
    c = Canvas(w, h)
    if lit:
        c.grad(1, 1, w - 2, h - 2, 7, (44, 110, 160), (22, 60, 98), outline=rgba(accent), width=1.4)
        c.text(w / 2, h / 2 + 1, label, 13, (210, 246, 255), "Bold")
    else:
        c.grad(1, 1, w - 2, h - 2, 7, (44, 51, 61), (30, 35, 43))
        c.text(w / 2, h / 2 + 1, label, 13, INK_DIM, "SemiBold")
    c.save(name + ("_on" if lit else "_off"))


def tint_key(name, lit, accent):
    """2.3.6: a key in its own colour: unlit, a coloured rim; lit, filled with the colour (the LFO route keys)"""
    from PIL import Image, ImageChops
    path = os.path.join(ART, name + ("_on" if lit else "_off") + ".png")
    im = Image.open(path).convert("RGBA")
    a = im.getchannel("A")
    col = Image.new("RGBA", im.size, tuple(accent) + (255,))
    if lit:   # the face washed with the colour, the lettering kept light
        wash = Image.blend(im, col, 0.45)
        wash.putalpha(a)
        im = wash
    else:     # a rim: the key's own edge (its alpha minus the alpha shrunk by 2 px) in the colour
        from PIL import ImageFilter
        inner = a.filter(ImageFilter.MinFilter(5))
        rim = ImageChops.subtract(a, inner).point(lambda v: int(v * 0.9))
        layer = col.copy()
        layer.putalpha(rim)
        im.alpha_composite(layer)
    im.save(path)


def opt_name(key, opt):
    return "opt_%s_%s" % (key, "".join(ch if ch.isalnum() else "_" for ch in opt.lower()))


def arrow_image(name, w, h, direction, lit, colour=BLUE):
    if RACK:   # 1.8.13: FigCraft keys
        import rack
        return rack.arrow_key(os.path.join(ART, name + ("_on" if lit else "_off") + ".png"), w, h, direction, lit, colour)
    c = Canvas(w, h)
    if lit:
        c.grad(1, 1, w - 2, h - 2, 7, tuple(min(255, v + 40) for v in colour), colour, outline=(220, 240, 255), width=1.4)
    else:
        c.grad(1, 1, w - 2, h - 2, 7, tuple(round(v * 0.75) for v in colour), tuple(round(v * 0.45) for v in colour),
               outline=(20, 24, 30), width=1)
    arrow_glyph(c, w / 2, h / 2, direction, (240, 248, 255))
    c.save(name + ("_on" if lit else "_off"))


def button_image(name, w, h, label, lit, accent=CYAN, size=15):
    if RACK:   # 1.8.13: FigCraft keys
        import rack
        return rack.key_image(os.path.join(ART, name + ("_on" if lit else "_off") + ".png"), w, h, label, lit, accent, size)
    c = Canvas(w, h)
    if lit:
        c.grad(1, 1, w - 2, h - 2, 8, (44, 110, 160), (22, 60, 98), outline=rgba(accent), width=1.5)
    else:
        c.grad(1, 1, w - 2, h - 2, 8, (48, 56, 68), (32, 38, 46), outline=(70, 80, 95), width=1.2)
    if label:
        c.text(w / 2, h / 2 + 1, label, size, (220, 246, 255) if lit else INK, "SemiBold")
    c.save(name + ("_on" if lit else "_off"))


# ---------------------------------------------------------------------------------------------------------------
# 1.8.2: PLAY's second page (MPC's sub-page PADS): PLAYBACK (each slot's mode and polarity) and NOTE LOCK. Its art
# covers the body only; the header and the pad row stay

P2_AREA = (0, 58, W, 498)                     # what bg_play2 covers (plugin coordinates)
PB_BOX, NL_BOX = (16, 66, 616, 480), (648, 66, 616, 480)
PB_ROW_Y0, PB_ROW_STEP = 136, 92              # the slot rows: name line; the mode segments 40 below (2.1.16: 102 -> 92)
RV_Y, RV_X0, RV_W = 514, 214, 110             # 2.1.16: REV MODE (Full / Gated) under the slot rows
PM_W, PM_X0, POL_W, POL_X0, SEG2_H = 84, 40, 70, 420, 30
NL_INFO_Y = 116
NL_KNOB_Y1, NL_KNOB_Y2 = 210, 334
NL_KNOBS = [(712, NL_KNOB_Y1, "lock_note", "NOTE", CYAN), (812, NL_KNOB_Y1, "lock_tune", "TUNE", BLUE),
            (912, NL_KNOB_Y1, "lock_fine", "FINE", VIOLET), (1012, NL_KNOB_Y1, "lock_start", "START", ORANGE),
            (1112, NL_KNOB_Y1, "lock_cut", "CUTOFF", CYAN), (1212, NL_KNOB_Y1, "lock_level", "LEVEL", BLUE),
            (712, NL_KNOB_Y2, "lock_pan", "PAN", CYAN), (812, NL_KNOB_Y2, "lock_vel", "VELOCITY", BLUE)]
NL_MODE_Y, NL_INV_Y, NL_SEG_W, NL_SEG_X0 = 432, 498, 92, 676
PLOCK_W, PLOCK_H, PLOCK_DY = 40, 6, 28        # a pad's lock mark: under the pad


def bg_play2():
    c = Canvas(W, H, BG + (255,))
    body(c, *BODY)
    for (x, yy, w, h), title in ((PB_BOX, "PLAYBACK"), (NL_BOX, "NOTE LOCK")):
        if c.ops is not None:   # 1.8.13: modules
            panel(c, x, yy, w, h, title, yy + 24)
            continue
        c.grad(x, yy, w, h, 12, (36, 42, 51), (26, 30, 37), outline=(70, 80, 95), width=1.2)
        c.text(x + w / 2, yy + 24, title, 19, INK, "SemiBold")
    x, yy, w, h = PB_BOX
    for i in range(4):
        ry = PB_ROW_Y0 + i * PB_ROW_STEP
        col = SLOT_COLOURS[i]
        c.glow(lambda l, ry=ry, col=col: l.rrect(PM_X0, ry - 16, 34, 32, 7, fill=rgba(col)), 3, 1.1)
        c.text(PM_X0 + 17, ry, "ABCD"[i], 19, BG, "Bold")
        c.rrect(PM_X0 + 44, ry - 16, 540, 32, 7, fill=DARK, outline=(64, 74, 88), width=1)
        sy = ry + 40
        c.rrect(PM_X0 - 3, sy - SEG2_H / 2 - 3, 4 * PM_W + 6, SEG2_H + 6, 9, fill=(14, 17, 22), outline=(66, 76, 90), width=1.2)
        c.rrect(POL_X0 - 3, sy - SEG2_H / 2 - 3, 2 * POL_W + 6, SEG2_H + 6, 9, fill=(14, 17, 22), outline=(66, 76, 90), width=1.2)
    c.text(PM_X0, RV_Y, "REV MODE", 12, INK_DIM, "SemiBold", "lm")
    c.rrect(RV_X0 - 3, RV_Y - SEG2_H / 2 - 3, 2 * RV_W + 6, SEG2_H + 6, 9, fill=(14, 17, 22), outline=(66, 76, 90), width=1.2)
    x, yy, w, h = NL_BOX
    c.rrect(x + 20, NL_INFO_Y - 16, w - 40, 32, 7, fill=DARK, outline=(64, 74, 88), width=1)
    for cx, cy, key, label, col in NL_KNOBS:
        knob_label(c, cx, cy - 26 - 16, label)
    knob_label(c, 1010, NL_KNOB_Y2 - 42, "PAD BANK")
    c.rrect(950, NL_KNOB_Y2 - 16, 120, 32, 7, fill=DARK, outline=(64, 74, 88), width=1)
    for yv, lab, n in ((NL_MODE_Y, "PLAYBACK (SLOT = THE SLOT'S OWN)", 5), (NL_INV_Y, "POLARITY", 3)):
        c.text(NL_SEG_X0, yv - 30, lab, 12, INK_DIM, "SemiBold", "lm")
        c.rrect(NL_SEG_X0 - 3, yv - SEG2_H / 2 - 3, n * NL_SEG_W + 6, SEG2_H + 6, 9, fill=(14, 17, 22), outline=(66, 76, 90), width=1.2)
    c.text(1130, NL_INV_Y - 30, "PADS FOLLOW NOTES", 12, INK_DIM, "SemiBold")
    c.rrect(1020 - 3, NL_INV_Y - SEG2_H / 2 - 3, 220 + 6, SEG2_H + 6, 9, fill=(14, 17, 22), outline=(66, 76, 90), width=1.2)
    ax, ay, aw, ah = P2_AREA
    c.final().crop((ax, ay, ax + aw, ay + ah)).save(os.path.join(ART, "bg_play2.png"))


def draw_plock(c, st):   # 0 nothing, 1 locked, 2 the note NOTE LOCK shows, 3 both
    w, h = c.w, c.h
    if st & 1:
        c.glow(lambda l: l.rrect(1, 1, w - 2, h - 2, 2, fill=rgba(ORANGE)), 1.2, 1.1)
    if st & 2:
        c.rrect(0.5, 0.5, w - 1, h - 1, 2, outline=rgba(CYAN), width=1.4)


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


# 1.8.13: BROWSE in the rack-mount look (rack.py, rendered by FigCraft), after the user's reference picture and design
# guide: rack ears (rack.EAR each side), the browser as a recessed glass screen, LOCATIONS module, the DATA zone (dial =
# the data wheel's field, UP / DOWN / ENTER keys under LOAD / NOTES / PREVIEW LEDs), the bottom I/O strip (VOLUME and
# PAN pots, AUTO EXTRACT, SAVE TO LIBRARY; the phones jack, USB and card slot are decoration). Plugin coordinates.
RK_SCREEN = (16, 64, 732, 470)          # the bezel; inside: folder line, 10 rows, page line
RK_ROWS_Y, RK_TH, RK_GAP = 118, 34, 3
RK_SIDE = (764, 64, 500, 274)           # LOCATIONS
RK_DIAL = (1200, 390, 36)               # DATA dial: cx, cy, r
RK_KEYS = [("br_rowup", "rk_up", "UP", 862, "led_busy", "amber", "LOAD"), ("br_rowdown", "rk_down", "DOWN", 936, "led_notes", "green", "NOTES"),
           ("br_enter", "rk_enter", "ENTER", 1010, "br_preview", "cyan", "PREVIEW")]
RK_KEY_Y, RK_LED_Y = 408, 372
RK_SLOT_Y = 452
RK_PROG = (780, 470, 468, 78)            # 2.1.16: the edges of LOCATIONS' screens (780..1248; was 808..1264)
RK_STRIP_Y = 596                        # the bottom I/O strip's centre line


def rack_header(s, left_label=None):
    """1.8.13: every page's header in the rack look: the page key, the logo behind glass, the format window"""
    import rack
    kw = 300
    if left_label == "SUPPORTED FORMATS":   # the INFO page's PG 1-3 keys go between this key and the logo
        left_label, kw = "FORMATS", 220
    if left_label == "SYNTH":   # 2.3.5: the PRESET stepper goes between this key and the logo
        kw = 116
        rack.glass(s, 178, 10, 216, 36, 6)
    if left_label:
        rack.key(s, 16, 8, kw, 40, 6)
        for k in range(3):
            s.rect(30, 20 + k * 7, 16, 2, 1, rack.hexc(rack.PRINT_DIM))
        s.text(60, 28, left_label, 18, rack.hexc(rack.PRINT), "start", 600, 1)
    rack.glass(s, 440, 4, 400, 48, 6, True)
    rack.omega_logo(s, 640, 28)
    rack.glass(s, 946, 10, 220, 36, 6)


def rack_page(ops, top):
    """1.8.13: a flat page's background in the rack look: the plate, then what the page asked for (header, modules,
    titles, labels, wells) in order, under everything else it drew (top, plugin size, transparent where untouched)"""
    import rack
    s = rack.Svg(W, H)
    rack.faceplate(s)
    painted = [op[1:5] for op in ops if op[0] == "module" and len(op) > 5 and op[5]]

    def on_paint(px, py):
        return any(x <= px <= x + w and y <= py <= y + h for x, y, w, h in painted)

    for op in ops:
        k = op[0]
        if k == "header":
            rack_header(s, op[1])
        elif k == "module":
            rack.module(s, *op[1:5], colour=op[5] if len(op) > 5 else None)
        elif k == "title":
            _, cx, cy, txt, x0, x1 = op
            s.text(cx, cy, txt, 18, rack.hexc(INK if on_paint(cx, cy) else rack.PRINT), "middle", 600, 2.2)
            tw = len(txt) * 12 + 30
            if x1 - x0 > tw + 80:
                for a, b in ((x0 + 28, cx - tw / 2), (cx + tw / 2, x1 - 28)):
                    s.rect(a, cy, b - a, 1, 0, "#ffffff" if on_paint(cx, cy) else "#a7aeb8" if rack.LIGHT else rack.hexc(EDGE),
                           extra='opacity="0.25"' if on_paint(cx, cy) else "")
        elif k == "label":
            light = on_paint(op[1], op[2]) or not rack.LIGHT
            rack.label(s, op[1], op[2], op[3], 13, fill=(214, 222, 232) if light else rack.PRINT, spacing=0.4)   # larger, clearer
        elif k == "glass":
            rack.glass(s, *op[1:])
    im = rack.render(s)
    im.alpha_composite(top)
    return im


def bg_browse_rack():
    import rack
    s = rack.Svg(W, H)
    rack.faceplate(s)
    # header: the page key, the logo behind glass, the format window (PANIC is a key image)
    rack_header(s, "BROWSE")
    # the screen
    x, yy, w, h = RK_SCREEN
    rack.screen(s, x, yy, w, h)
    rack.glass(s, x + 14, yy + 12, w - 28, 34, 6)                    # folder line (the wheel field is on it too)
    rack.glass(s, x + 14, yy + h - 43, w - 28, 30, 6)                # page line
    rack.label(s, x + w / 2, yy + h + 13, "Presets from disk images are saved to the Plugin Library", 11)
    # LOCATIONS
    x, yy, w, h = RK_SIDE
    rack.module(s, x, yy, w, h, "LOCATIONS")
    rack.glass(s, x + 16, yy + 166, w - 32, 22, 5)                   # library path
    rack.label(s, x + w / 2, yy + 202, "Selected", 12)
    rack.glass(s, x + 16, yy + 212, w - 32, 54, 7, True)
    # DATA zone
    cx, cy, r = RK_DIAL
    rack.data_dial(s, cx, cy, r)
    rack.label(s, cx - r - 30, cy - 20, "Turn", 9)    # (1.8.13 fix: "Data" under the dial was hidden by slot D's key)
    rack.label(s, cx - r - 30, cy - 8, "Press", 9)
    rack.label(s, cx - r - 30, cy + 7, "Data", 11)
    for _, _, _, kx, _, _, lab in RK_KEYS:
        rack.led_socket(s, kx - 22, RK_LED_Y, 4.5)
        rack.label(s, kx - 13, RK_LED_Y, lab, 10, "start")
    # slot and instrument
    rack.label(s, RK_PROG[0], RK_SLOT_Y, "Load into slot", 12, "start")
    x, yy, w, h = RK_PROG
    rack.glass(s, x, yy, w, h, 8, True)
    # the I/O strip: a recessed band across the plate
    sy = RK_STRIP_Y
    s.rect(rack.EAR + 8, sy - 34, W - 2 * rack.EAR - 16, 1, 0, "#000", extra='opacity="0.6"')
    s.rect(rack.EAR + 8, sy - 33, W - 2 * rack.EAR - 16, 1, 0, "#ffffff", extra='opacity="0.07"')
    for lx, lab in ((96, "Volume"), (176, "Phones"), (256, "Pan L/R"), (392, "Sample folders"), (556, "USB"), (672, "Card"),
                    (930, "Auto extract")):
        rack.label(s, lx, sy - 22, lab, 10)
    rack.jack(s, 176, sy + 4)
    rack.usb_port(s, 556, sy + 4)     # 2.1.3: USB and card moved right for SAMPLE FOLDERS
    rack.card_slot(s, 672, sy + 4)
    im = rack.render(s)
    im.save(os.path.join(ART, "bg_browse.png"))


def rack_images():
    """the rack BROWSE's keys (rk_*), LEDs and pots (their own names: the other pages keep theirs)"""
    import rack
    for lit in (0, 1):
        sfx = "_on" if lit else "_off"
        for name, label_txt, w, h in (("rk_drives", "DRIVES", 214, 38), ("rk_library", "PLUGIN LIBRARY", 214, 38),
                                       ("rk_up_dir", "FOLDER UP", 214, 38), ("rk_refresh", "REFRESH", 214, 38),
                                       ("rk_setlib", "SET LIBRARY HERE", 214, 38), ("rk_panic", "PANIC", 92, 36),
                                       ("rk_extract", "SAVE TO LIBRARY", 200, 36), ("rk_up", "UP", 64, 30),
                                       ("rk_down", "DOWN", 64, 30), ("rk_enter", "ENTER", 64, 30),
                                       ("rk_prev_off", "OFF", 104, 32), ("rk_prev_on", "ON", 104, 32),
                                       ("rk_ae_off", "OFF", 92, 30), ("rk_ae_disk", "DISK IMAGES", 112, 30),
                                       ("rk_sf_hide", "HIDE", 76, 30), ("rk_sf_show", "SHOW", 76, 30)):
            rack.key_image(os.path.join(ART, name + sfx + ".png"), w, h, label_txt, lit, size=13 if w < 120 else 14)
        for i in range(4):
            rack.key_image(os.path.join(ART, "rk_slot_%d%s.png" % (i, sfx)), 50, 28, "ABCD"[i], lit, SLOT_COLOURS[i], 14)
    for name, colour in (("led_amber", "amber"), ("led_green", "green"), ("led_cyan", "cyan")):
        rack.led_strip(os.path.join(ART, name + ".png"), rack.LED[colour], 14)
    rack.knob_strip(os.path.join(ART, "rk_pot.png"), 56)


SETUP_PANELS = [("VOICE", (16, 66, 616, 300)), ("OUTPUT", (648, 66, 616, 300)), ("PLAYBACK", (16, 380, 616, 240)),
                ("EFFECTS & SYSTEM", (648, 380, 616, 240))]
SETUP_KNOBS = [(150, 190, 34, "polyphony", "VOICES", CYAN), (330, 190, 34, "glide", "GLIDE (MS)", BLUE),
               (510, 190, 34, "bend", "BEND RANGE", CYAN),
               (730, 210, 34, "volume", "VOLUME (DB)", CYAN), (875, 210, 34, "pan", "PAN", CYAN),
               (1020, 210, 34, "transpose", "TRANSPOSE (ST)", BLUE), (1165, 210, 34, "tune", "FINE TUNE (CT)", VIOLET),
               (110, 480, 34, "vel_sens", "VELOCITY SENS %", BLUE), (250, 480, 34, "filter_vel", "FILTER VEL %", CYAN),   # 2.4.9: up 20 (readout hit VELOCITY CURVE)
               (740, 500, 34, "rev_damp", "REVERB DAMP %", VIOLET), (880, 500, 34, "mem_limit", "MEMORY (MB)", ORANGE),
               (1180, 500, 34, "pad_base", "PAD BANK NOTE", CYAN),
               (560, 306, 24, "wt_pos", "WT POSITION", ORANGE)]   # 1.7: wavetables
VEL_CURVES = ["Soft", "Linear", "Hard", "Fixed"]   # 2.4.6
SETUP_OPTIONS = [("voice_mode", ["Poly", "Mono", "Legato"], 184, 310, "VOICE MODE"),
                 ("vel_curve", VEL_CURVES, 40, 596, "VELOCITY CURVE"),   # 2.4.6: under VELOCITY SENS / FILTER VEL; 2.4.9: 590 -> 596
                 ("interp", ["None", "Linear", "Cubic"], 340, 480, "INTERPOLATION"),
                 # 2.1.1: AUTO LOOP moves left to share its row with HQ SINC; both have narrower keys (OPT_W_S)
                 ("auto_loop", ["Off", "On"], 340, 574, "AUTO LOOP"),
                 ("sinc", ["Off", "On"], 482, 574, "HQ SINC"),
                 ] + ([("skin", [n for _, n, _ in SKINS], 856, 590, "SKIN")] if SKIN_SWITCH else []) + [   # 1.8.13
                 ("prog_change", ["Off", "On"], 960, 500, "PROGRAM CHANGE")]
OPT_W, OPT_H = 84, 34
RESET_X, RESET_Y = 1160, 322                         # 2.1.16: RESET (button centre, plugin coordinates)
OPT_W_S = 58                                         # 2.1.1: AUTO LOOP and HQ SINC (both under INTERPOLATION's 340..604)
NARROW_OPTS = ("auto_loop", "sinc", "vel_curve")


def set_opt(key):
    """a SETUP option group's key width and image name prefix (narrow keys get their own images: PROGRAM CHANGE
    shares AUTO LOOP's full-width ones)"""
    return (OPT_W_S, "set_s_") if key in NARROW_OPTS else (OPT_W, "set_")


def bg_setup():
    c = Canvas(W, H, BG + (255,))
    topbar(c, "SETUP")
    for title, box in SETUP_PANELS:
        panel(c, *box, title=title)
    for cx, cy, r, _, label, _ in SETUP_KNOBS:
        knob_label(c, cx, cy - r - 16, label)
    for key, opts, x0, cy, label in SETUP_OPTIONS:
        wtot = len(opts) * set_opt(key)[0] + (len(opts) - 1) * 6
        knob_label(c, x0 + wtot / 2, cy - 34, label)
    c.save("bg_setup")


# 2.3: the SYNTH tab's OSC page: two oscillator modules, each a browser line, a screen, a status strip, 2 x 6 knobs
def bg_synth():
    c = Canvas(W, H, BG + (255,))
    topbar(c, "SYNTH")   # 2.3.1: one title for every sub-page (MPC's dots show which)
    for x in ("a", "b"):
        x0, acc = SYN_X0[x], SYN_ACC[x]
        panel(c, x0, 66, 616, 554, None, colour=acc)
        if c.ops is not None:
            c.ops.append(("glass", x0 + 16, 66 + 52, 584, 34, 6, False))     # the browser line
            c.ops.append(("glass", x0 + 16, 66 + 94, 584, 160, 8, True))     # the screen
            c.ops.append(("glass", x0 + 16, 66 + 258, 584, 28, 6, False))    # the status strip
        else:
            c.rrect(x0 + 16, 66 + 52, 584, 34, 6, fill=DARK)
            c.rrect(x0 + 16, 66 + 94, 584, 160, 8, fill=DARK, outline=(64, 74, 88), width=1)
            c.rrect(x0 + 16, 66 + 258, 584, 28, 6, fill=DARK)
        c.text(x0 + 116, 66 + 25, "OSC " + x.upper(), 20, acc, "Bold", "lm")
        sy = 66 + 94 + 80   # the screen's centre line
        c.line([(x0 + 30, sy), (x0 + 586, sy)], (90, 130, 160, 110), 1)
        c.text(x0 + 30, 66 + 272, "PHASE", 11, INK_DIM, "SemiBold", "lm")
        c.text(x0 + 300, 66 + 272, "WARP", 11, INK_DIM, "SemiBold", "lm")
        for row, keys in enumerate((SYN_ROW1, SYN_ROW2)):
            for i, k in enumerate(keys):
                knob_label(c, x0 + 58 + i * 100, 66 + 304 + row * 118, SYN_LABELS[k])
    c.save("bg_synth")


def bg_synth2():
    """2.3.1: the MIX / FILTER page: SUB, NOISE, OSC MOD over FILTER (a response screen) and ROUTING"""
    c = Canvas(W, H, BG + (255,))
    topbar(c, "SYNTH")
    for (x, yy, w, h), col, title in ((SM_SUB, SM_GREEN, "SUB"), (SM_NOISE, SM_VIOLET, "NOISE"), (SM_OMOD, SM_BLUE, "OSC MOD"),
                                       (SM_FLT, SM_CYAN, "FILTER"), (SM_ROUTE, SM_ORANGE, "ROUTING")):
        panel(c, x, yy, w, h, None, colour=col)
        tx = x + 16 if title in ("OSC MOD", "ROUTING") else x + 108
        c.text(tx, yy + 22, title, 18, col, "Bold", "lm")
    sx, sy, sw, sh = SM_SCREEN
    if c.ops is not None:
        c.ops.append(("glass", sx, sy, sw, sh, 8, True))
    else:
        c.rrect(sx, sy, sw, sh, 8, fill=DARK, outline=(64, 74, 88), width=1)
    x, yy = SM_SUB[0], SM_SUB[1]
    for i, lab in enumerate(("LEVEL", "PAN")):
        knob_label(c, x + 90 + i * 120, yy + 158, lab)
    x, yy = SM_NOISE[0], SM_NOISE[1]
    for i, lab in enumerate(("LEVEL", "PAN", "TONE")):
        knob_label(c, x + 60 + i * 90, yy + 158, lab)
    x, yy = SM_OMOD[0], SM_OMOD[1]
    for i, lab in enumerate(("FM B INTO A", "FEEDBACK", "RING", "BALANCE A / B")):
        knob_label(c, x + 90 + i * 145, yy + 130, lab)
    c.text(x + 16, yy + 250, "FM: B modulates A's phase  \u00b7  FEEDBACK: A modulates itself  \u00b7  SYNC: B restarts with A", 11, INK_DIM, "SemiBold", "lm")
    x, yy = SM_FLT[0], SM_FLT[1]
    for r, row in enumerate((("CUTOFF", "RESONANCE", "DRIVE", "KEY TRACK"), ("ENV AMOUNT", "VELOCITY", "FAT", "MIX"))):
        for i, lab in enumerate(row):
            knob_label(c, x + 470 + i * 118, yy + 46 + r * 96, lab)   # 2.3.2: label, knob (r 22), value: 96 apart, no overlap
    x, yy = SM_ROUTE[0], SM_ROUTE[1]
    c.text(x + 16, yy + 48, "INTO THE FILTER", 11, INK_DIM, "SemiBold", "lm")
    for i, lab in enumerate(("OSC A", "OSC B", "SUB", "NOISE")):
        c.text(x + 16, yy + 70 + i * 32, lab, 13, INK, "SemiBold", "lm")
    for i, lab in enumerate(("OUTPUT LEVEL", "PAN")):
        knob_label(c, x + 80 + i * 150, yy + 186, lab)
    # the body only (as PLAY's PADS page): the header's SYNTH ON / OFF and PANIC stay on top
    ax, ay, aw, ah = SM_AREA
    c.final().crop((ax, ay, ax + aw, ay + ah)).save(os.path.join(ART, "bg_synth2.png"))


def syn_response_images():
    """2.3.1: the filter screen's curve, one image per type (a typical setting: 1 kHz, some resonance)"""
    import math
    sx, sy, sw, sh = SM_SCREEN
    w, h = sw - 16, sh - 36
    for t, name in enumerate(SYN_FTYPES):
        c = Canvas(w, h)
        pts = []
        for j in range(240):
            f = 20 * (1000 ** (j / 239))
            rr, q = f / 1000.0, 2.5
            lp2 = 1 / math.sqrt((1 - rr * rr) ** 2 + (rr / q) ** 2)
            if t == 0: mag = lp2
            elif t == 1: mag = lp2 * lp2
            elif t == 2: mag = lp2 ** 4 * (1 / (1 + rr ** 6)) ** 0 if False else (1 / math.sqrt((1 - rr * rr) ** 2 + (rr / 4) ** 2)) ** 2 / (1 + rr ** 2)
            elif t == 3: mag = (rr / q) / math.sqrt((1 - rr * rr) ** 2 + (rr / q) ** 2) * q
            elif t == 4: mag = rr * rr / math.sqrt((1 - rr * rr) ** 2 + (rr / q) ** 2)
            elif t == 5: mag = abs(1 - rr * rr) / math.sqrt((1 - rr * rr) ** 2 + (rr / 0.7) ** 2)
            else: mag = 1 / abs(1 - 0.7 * complex(math.cos(2 * math.pi * f / 1000), -math.sin(2 * math.pi * f / 1000)))
            db = max(-36, min(18, 20 * math.log10(max(1e-6, mag))))
            pts.append((4 + j * (w - 8) / 239, h - 6 - (db + 36) / 54 * (h - 18)))
        c.glow(lambda l, pts=pts: l.line(pts, rgba(SM_CYAN), 2.4), 2.5, 1.0)
        c.line(pts, rgba(SM_CYAN), 2)
        c.save("syn_resp_%d" % t)   # 2.3.3: no name in the picture (the curve ran over it; the lit key names the type)


def strip_marker(name, w, h, frames=METER_FRAMES, colour=SM_CYAN):
    """2.3.1: a position marker along a thin track (the filter screen's cutoff): frame f at f / 127 of the width"""
    im = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for f in range(frames):
        c = Canvas(w, h)
        c.rrect(0, h / 2 - 1, w, 2, 1, fill=(60, 80, 100, 160))
        px = 4 + (w - 8) * f / (frames - 1)
        c.rrect(px - 3, 0, 6, h, 2, fill=rgba(colour))
        im.paste(c.im.resize((w, h), Image.LANCZOS), (0, f * h))
    im.save(os.path.join(ART, name + ".png"))


def bg_synth3(envs=None, lfos=None, fname="bg_synth3.png"):
    """2.3.2: the ENV / LFO page: three envelopes (screens of bars) over two LFOs (shape screens) and the macros.
    2.4.17: also the MOD 2 page (envs SE_ENVS2, lfos SE_LFOS2: a note in place of the macros)"""
    page2 = envs is not None
    envs = SE_ENVS if envs is None else envs
    lfos = SE_LFOS if lfos is None else lfos
    c = Canvas(W, H, BG + (255,))
    topbar(c, "SYNTH")
    def glass(x, yy, w, h, deep=True):
        if c.ops is not None:
            c.ops.append(("glass", x, yy, w, h, 8, deep))
        else:
            c.rrect(x, yy, w, h, 8, fill=DARK, outline=(64, 74, 88), width=1)
    for e, title, (x, yy, w, h), col, keys, last in envs:
        panel(c, x, yy, w, h, None, colour=col)
        c.text(x + (108 if e in SE_MOD_ENV_KEY else 16), yy + 22, title, 18, col, "Bold", "lm")
        glass(x + 16, yy + 46, w - 32, 120)
        for i, lab in enumerate(("ATTACK", "DECAY", "SUSTAIN", "RELEASE", last)):
            knob_label(c, x + 44 + i * 80, yy + 186, lab)   # under the screen (it ends at yy + 166)
    for n, (x, yy, w, h), col in lfos:
        panel(c, x, yy, w, h, None, colour=col)
        c.text(x + 16, yy + 22, "LFO %d" % n, 18, col, "Bold", "lm")
        glass(x + 16, yy + 46, 180, 92)
        for i, lab in enumerate(("RATE", "DEPTH")):
            knob_label(c, x + 252 + i * 100, yy + 52, lab)
    if page2:   # 2.4.17: how the MOD 2 sources reach a destination
        x, yy, w, h = SE_NOTE2
        panel(c, x, yy, w, h, None, colour=SM_GREEN)
        c.text(x + 16, yy + 22, "MORE MODULATION", 18, SM_GREEN, "Bold", "lm")
        lines = ["MOD ENV 2 AND 3 WORK LIKE MOD ENV:", "SWITCH ON, PICK A DESTINATION, SET AMOUNT.", "",
                 "LFO 3 AND 4: AMP / FILTER / MOD KEYS", "AS ON THE ENV PAGE.", "",
                 "ALL FOUR ARE MATRIX SOURCES:", "LFO 3, LFO 4, MOD ENV 2, MOD ENV 3", "(MATRIX AND MATRIX 2: 16 ROUTES).", "",
                 "VITAL AND OMNISPHERE PATCHES FILL", "THEM WHEN THEY USE MORE THAN TWO", "LFOS OR ONE MOD ENVELOPE."]
        for i, t in enumerate(lines):
            c.text(x + w / 2, yy + 78 + i * 30, t, 13, INK_DIM, "SemiBold", "mm")
        ax, ay, aw, ah = SM_AREA
        c.final().crop((ax, ay, ax + aw, ay + ah)).save(os.path.join(ART, fname))
        return
    x, yy, w, h = SE_MAC
    panel(c, x, yy, w, h, None, colour=SM_ORANGE)
    c.text(x + 16, yy + 22, "MACROS", 18, SM_ORANGE, "Bold", "lm")
    for i in range(4):
        knob_label(c, x + 58 + i * 97, yy + 56, "MACRO %d" % (i + 1))
    c.text(x + w / 2, yy + 222, "EACH MACRO TURNS ITS DESTINATION  \u00b7  Q-LINKS 13-16", 11, INK_DIM, "SemiBold", "mm")
    ax, ay, aw, ah = SM_AREA
    c.final().crop((ax, ay, ax + aw, ay + ah)).save(os.path.join(ART, "bg_synth3.png"))


def stepper_lines(key, cx, cy, w, h, text_size=15):
    """2.3.3: a stepper of our own: an arrow button, a readout of the setting, an arrow button (the skin tools' own
    stepper cuts its arrows out of the page art under them, which on a sub-page is another page's art)"""
    a = "stp%d" % h
    return ['button cx=%d cy=%d label="" key=%s_prev img=art/%s_l_off.png img_on=art/%s_l_on.png w=%d h=%d' % (cx - w // 2 + h // 2, y(cy), key, a, a, h, h),
            'readout cx=%d cy=%d w=%d h=%d label="" key=%s text_size=%d' % (cx, y(cy), w - 2 * h - 8, h, key, text_size),
            'button cx=%d cy=%d label="" key=%s_next img=art/%s_r_off.png img_on=art/%s_r_on.png w=%d h=%d' % (cx + w // 2 - h // 2, y(cy), key, a, a, h, h)]


def bg_arp():
    """2.4: the ARP page: SEQUENCE (mode, MIDI file, scale, rate and feel) beside the GRID (pages A-D, the playhead,
    16 rows of the scale over 16 steps)"""
    c = Canvas(W, H, BG + (255,))
    topbar(c, "ARP")
    x, yy, w, h = AA_L
    panel(c, x, yy, w, h, None, colour=SM_GREEN)
    c.text(x + 16, yy + 22, "SEQUENCE", 18, SM_GREEN, "Bold", "lm")
    for ty, lab in ((154, "MIDI FILE  \u00b7  <LIBRARY>/MIDI"), (262, "SCALE"), (318, "ORDER  (ARP)"), (374, "RATE")):
        c.text(x + 16, ty, lab, 11, INK_DIM, "SemiBold", "lm")
    for ty, lab in ((262, "ROOT"), (318, "OCTAVES"), (374, "MIDI SNAP")):
        c.text(x + 196, ty, lab, 11, INK_DIM, "SemiBold", "lm")
    # (2.4.2: the six knob labels are meters on arp_sel: the pattern's knobs, or the selected note's: strip_label_pair)
    x, yy, w, h = AA_R
    panel(c, x, yy, w, h, None, colour=SM_CYAN)
    c.text(x + 16, yy + 22, "GRID", 18, SM_CYAN, "Bold", "lm")
    gx0, gx1 = AG_X0 - AG_DX // 2, AG_X0 + AG_DX * (ARP_COLS - 1) + AG_DX // 2
    gy0, gy1 = AG_Y1 - AG_DY * (ARP_ROWS - 1) - AG_DY // 2 - 4, AG_Y1 + AG_DY // 2 + 4
    c.rrect(gx0 - 6, gy0 - 34, gx1 - gx0 + 12, gy1 - gy0 + 40, 8, fill=(12, 15, 20))
    # (2.4.1: no fixed beat bands: the lights above the columns show beats and bars at the RATE chosen)
    for r in range(ARP_ROWS):   # row lines
        yr = AG_Y1 - r * AG_DY
        c.line([(gx0, yr + AG_DY / 2), (gx1, yr + AG_DY / 2)], (40, 48, 60, 160), 1)
    c.text(x + 42, gy0 - 20, "ROW", 11, INK_DIM, "SemiBold", "mm")
    # 2.4.1: the grid's second row of keys
    for tx, lab in ((x + 16, "TOUCH"), (x + 200, "SHIFT"), (x + 330 + ARP_HDR_DX, "STRUM")):   # 2.4.9: STRUM on (ARP_HDR_DX)
        c.text(tx, 128, lab, 11, INK_DIM, "SemiBold", "lm")
    ax, ay, aw, ah = 0, 0, W, H
    c.save("bg_arp")


AG2_L, AG2_R = (16, 66, 616, 554), (648, 66, 616, 554)   # 2.4.2: the GEN page's MELODY and GROOVE modules


def bg_arp2():
    """2.4.2: the ARP's GEN page: MELODY (the generator, motif tools) beside GROOVE (direction, feel, Euclid, chain)"""
    c = Canvas(W, H, BG + (255,))
    topbar(c, "ARP")
    x, yy, w, h = AG2_L
    panel(c, x, yy, w, h, None, colour=SM_GREEN)
    c.text(x + 16, yy + 22, "MELODY", 18, SM_GREEN, "Bold", "lm")
    c.text(x + 16, 118, "CONTOUR", 11, INK_DIM, "SemiBold", "lm")
    for i, lab in enumerate(("DENSITY", "RANGE", "SYNCOPATION", "MUTATE")):
        knob_label(c, x + 80 + i * 150, 206, lab)
    c.text(x + 16, 334, "MOTIF", 11, INK_DIM, "SemiBold", "lm")
    # 2.4.11: the help lines made room for GENERATE's expression knobs (applied on the next GENERATE)
    for i, lab in enumerate(("ARTICULATION", "EXPRESSION", "GHOSTS + FILLS")):
        knob_label(c, x + 80 + i * 150, 470, lab)
    x, yy, w, h = AG2_R
    panel(c, x, yy, w, h, None, colour=SM_CYAN)
    c.text(x + 16, yy + 22, "GROOVE", 18, SM_CYAN, "Bold", "lm")
    c.text(x + 16, 118, "DIRECTION", 11, INK_DIM, "SemiBold", "lm")
    for i, lab in enumerate(("SWING", "HUMAN TIME", "HUMAN VEL")):
        knob_label(c, x + 80 + i * 150, 206, lab)
    c.text(x + 16, 318, "EUCLID  \u00b7  HITS SPREAD EVENLY, ON THE SELECTED NOTE'S ROW", 11, INK_DIM, "SemiBold", "lm")
    for i, lab in enumerate(("HITS", "ROTATE")):
        knob_label(c, x + 80 + i * 150, 346, lab)
    c.text(x + 16, 456, "CHAIN  \u00b7  PAGES PLAYED IN TURN (OVERRIDES LENGTH)", 11, INK_DIM, "SemiBold", "lm")
    c.text(x + 16, 542, "BREED  \u00b7  A NEW PATTERN FROM THE BARS OF A AND B (STORE THEM FIRST)", 11, INK_DIM, "SemiBold", "lm")
    ax, ay, aw, ah = SM_AREA
    c.final().crop((ax, ay, ax + aw, ay + ah)).save(os.path.join(ART, "bg_arp2.png"))


def bg_synth4(first=1, fname="bg_synth4.png"):
    """2.3.3: the MATRIX page: 8 routes in two modules (source, destination, amount). 2.4.17: MATRIX 2 = first 9"""
    c = Canvas(W, H, BG + (255,))
    topbar(c, "SYNTH")
    for half, (x, col) in enumerate(((16, SM_VIOLET), (648, SM_BLUE))):
        panel(c, x, 66, 616, 554, None, colour=col)
        c.text(x + 16, 66 + 22, "MATRIX  %d - %d" % (first + half * 4, first + half * 4 + 3), 18, col, "Bold", "lm")
        c.text(x + MX_SRC_X, 66 + 52, "SOURCE", 11, INK_DIM, "SemiBold", "mm")
        c.text(x + MX_DST_X, 66 + 52, "DESTINATION", 11, INK_DIM, "SemiBold", "mm")
        c.text(x + MX_AMT_X, 66 + 52, "AMOUNT", 11, INK_DIM, "SemiBold", "mm")
        for i in range(4):
            ry = 66 + 112 + i * 124
            c.text(x + 30, ry, str(first + half * 4 + i), 22, col, "Bold", "mm")
            if i:
                c.line([(x + 16, ry - 62), (x + 600, ry - 62)], (70, 80, 95, 120), 1)
    ax, ay, aw, ah = SM_AREA
    c.final().crop((ax, ay, ax + aw, ay + ah)).save(os.path.join(ART, fname))


def bg_synth5():
    """2.3.3: the VOICE page: VOICE and PITCH & FEEL over KEY RANGE (a keyboard the range's markers move over)"""
    c = Canvas(W, H, BG + (255,))
    topbar(c, "SYNTH")
    for (x, yy, w, h), col, title in ((SV_VOICE, SM_GREEN, "VOICE"), (SV_PITCH, SM_CYAN, "PITCH & FEEL"), (SV_RANGE, SM_ORANGE, "KEY RANGE")):
        panel(c, x, yy, w, h, None, colour=col)
        c.text(x + 16, yy + 22, title, 18, col, "Bold", "lm")
    x, yy = SV_VOICE[0], SV_VOICE[1]
    for i, lab in enumerate(("VOICES", "GLIDE")):
        knob_label(c, x + 110 + i * 160, yy + 128, lab)
    c.text(x + 470, yy + 128, "GLIDE", 13, INK, "SemiBold", "mm")
    x, yy = SV_PITCH[0], SV_PITCH[1]
    for i, lab in enumerate(("BEND RANGE", "TRANSPOSE", "DRIFT", "VELOCITY")):
        knob_label(c, x + 90 + i * 145, yy + 96, lab)
    c.text(x + 16, yy + 248, "DRIFT: a little random detune per note  \u00b7  VELOCITY: how much it changes the level", 11, INK_DIM, "SemiBold", "lm")
    x, yy = SV_RANGE[0], SV_RANGE[1]
    kx, ky, kw, kh = SV_KEYS
    c.rrect(kx - 4, ky - 4, kw + 8, kh + 8, 6, fill=(10, 12, 16))
    c.rrect(kx, ky, kw, kh, 3, fill=(205, 210, 218))
    for key in range(128):
        px = kx + key * kw / 128
        if key % 12 in (1, 3, 6, 8, 10):
            c.rrect(px, ky, kw / 128, kh * 0.6, 0, fill=(22, 26, 32))
        elif key % 12 in (0, 5):
            c.line([(px, ky), (px, ky + kh)], (120, 128, 140), 1)
        if key % 12 == 0:
            c.text(px + 2, ky + kh + 12, "C%d" % (key // 12 - 2), 10, INK_DIM, "SemiBold", "lm")
    for i, lab in enumerate(("LOW KEY", "HIGH KEY")):
        knob_label(c, x + 1020 + i * 110, yy + 56, lab)
    for i, lab in enumerate(("LEVEL", "PAN")):
        knob_label(c, x + 1020 + i * 110, yy + 168, lab)
    c.text(x + 16, yy + 60, "THE SYNTH PLAYS THESE KEYS (WITH SLOTS A-D, OR SPLIT FROM THEM)  \u00b7  MUTATE / BREED THIS PATCH:", 11, INK_DIM, "SemiBold", "lm")
    ax, ay, aw, ah = SM_AREA
    c.final().crop((ax, ay, ax + aw, ay + ah)).save(os.path.join(ART, "bg_synth5.png"))


def strip_range_marker(name, w, h, colour, frames=METER_FRAMES):
    """2.3.3: a key range end over the keyboard: frame f (0..127, a MIDI note) a bar at that key's x"""
    im = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for f in range(frames):
        c = Canvas(w, h)
        px = (f + 0.5) * w / 128
        c.glow(lambda l, px=px: l.rrect(px - 2.5, 0, 5, h, 2, fill=rgba(colour)), 2.0, 1.2)
        im.paste(c.im.resize((w, h), Image.LANCZOS), (0, f * h))
    im.save(os.path.join(ART, name + ".png"))


def syn_lfo_images():
    """2.3.2: each LFO shape for the LFO screens (164 x 76), in each LFO's colour"""
    import math, random
    for n, _, col in SE_LFOS + SE_LFOS2:   # 2.4.17: + LFO 3 / 4
        for si, name in enumerate(SYN_LFO_SHAPES):
            w, h = 164, 76
            c = Canvas(w, h)
            rnd = random.Random(7 + si)
            vals = [rnd.uniform(-1, 1) for _ in range(5)]
            pts = []
            for j in range(160):
                ph = (j / 159) * 2 % 1
                cyc = int((j / 159) * 2 * 2)
                if si == 0: v = math.sin(2 * math.pi * ph)
                elif si == 1: v = 4 * ph - 1 if ph < 0.5 else 3 - 4 * ph
                elif si == 2: v = 2 * ph - 1
                elif si == 3: v = 1 if ph < 0.5 else -1
                elif si == 4: v = vals[min(4, int(j / 159 * 4.99))]
                elif si == 6:   # 2.4.13 CUSTOM: a drawn shape, in the upper part (its name and < > sit below it)
                    knots = [(0, -0.2), (0.18, 0.95), (0.3, 0.35), (0.55, 0.6), (0.72, -0.9), (0.86, -0.1), (1, -0.2)]
                    for a in range(len(knots) - 1):
                        if knots[a][0] <= ph <= knots[a + 1][0]:
                            f = (ph - knots[a][0]) / max(1e-9, knots[a + 1][0] - knots[a][0]); f = 0.5 - 0.5 * math.cos(math.pi * f)
                            v = knots[a][1] + (knots[a + 1][1] - knots[a][1]) * f
                            break
                    pts.append((4 + j * (w - 8) / 159, 24 - v * 18))
                    continue
                else:
                    t = (j / 159) * 4; k = min(3, int(t)); f = t - k; f = f * f * (3 - 2 * f)
                    v = vals[k] + (vals[k + 1] - vals[k]) * f
                pts.append((4 + j * (w - 8) / 159, h / 2 - v * (h / 2 - 8)))
            c.glow(lambda l, pts=pts: l.line(pts, rgba(col), 2.2), 2.0, 1.0)
            c.line(pts, rgba(col), 2)
            c.save("syn_lfo%d_%d" % (n, si))


def arp_dot_image(lit):
    """2.4: a grid cell: a dark pad, or a lit dot"""
    c = Canvas(AG_CW, AG_CH)
    if lit:
        c.rrect(1, 1, AG_CW - 2, AG_CH - 2, 6, fill=(36, 110, 140), outline=rgba(SM_CYAN), width=1.4)
        c.rrect(AG_CW / 2 - 9, AG_CH / 2 - 5, 18, 10, 5, fill=(200, 245, 255))
    else:
        c.rrect(1, 1, AG_CW - 2, AG_CH - 2, 6, fill=(30, 36, 46), outline=(52, 62, 76), width=1)
    c.save("arp_dot_" + ("on" if lit else "off"))


def strip_led(name, w, h, frames=METER_FRAMES, colour=SM_GREEN):
    """2.4: the ARP playhead's light: frames 0-31 a step past LENGTH (almost black), 32-63 a step played (dim), 64-79
    a beat's first step, 80-95 a bar's first step (at the RATE: plugin.cpp arp_led_value), 96-127 the step playing now"""
    im = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for f in range(frames):
        c = Canvas(w, h)
        fill = (rgba(colour, 255) if f >= 96 else (214, 136, 34, 255) if f >= 80 else (120, 140, 168, 255) if f >= 64
                else (58, 72, 92, 255) if f >= 32 else (22, 25, 31, 255))
        c.rrect(1, 1, w - 2, h - 2, 3, fill=fill)
        im.paste(c.im.resize((w, h), Image.LANCZOS), (0, f * h))
    im.save(os.path.join(ART, name + ".png"))


def strip_arp_cell(name, w, h, frames=METER_FRAMES):
    """2.4.1: a grid cell, w = the column pitch so a held note is one bar. Frame = plugin.cpp arp_cell_code: 0 empty;
    else 1 + 40 past LENGTH + 20 selected + kind: 0 a note's first step, 1 a middle one, 2 + g its last, 10 + g a
    one-step note (g 0-7: its gate in eighths, drawn as the part of its last step it sounds)"""
    im = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    ins, top, bh = 3, 2, h - 4
    for f in range(frames):
        c = Canvas(w, h)
        c.rrect(ins, top, w - 2 * ins, bh, 6, fill=(30, 36, 46), outline=(52, 62, 76), width=1)   # the step
        if 1 <= f <= 80:
            v = f - 1
            dim = v >= 40
            v -= 40 if dim else 0
            sel = v >= 20
            v -= 20 if sel else 0
            fill, line, core = ((214, 136, 34), (255, 196, 90), (255, 236, 190)) if sel else ((36, 110, 140), rgba(SM_CYAN), (200, 245, 255))
            if dim:
                fill, line, core = tuple(int(x * 0.45) for x in fill[:3]), tuple(int(x * 0.5) for x in line[:3]), tuple(int(x * 0.5) for x in core[:3])
            if v == 0:      # first step: rounded at the left, open to the right
                x0, x1 = ins, w + 8
            elif v == 1:    # a middle step: open both sides
                x0, x1 = -8, w + 8
            elif v < 10:    # last step: as far as its gate
                x0, x1 = -8, max(ins + 4, (w - ins) * (v - 1) / 8.0)
            else:           # a one-step note
                x0, x1 = ins, ins + max(6, (w - 2 * ins) * (v - 9) / 8.0)
            c.rrect(x0, top, x1 - x0, bh, 6, fill=rgba(fill), outline=rgba(line), width=1.4)
            if v in (0,) or v >= 10:   # the note's start mark
                c.rrect(ins + 5, h / 2 - 4, 10, 8, 4, fill=rgba(core))
        im.paste(c.im.resize((w, h), Image.LANCZOS), (0, f * h))
    im.save(os.path.join(ART, name + ".png"))


ARP_KNOB_SLOTS = [("arp_length", "LENGTH", "arp_note_gate", "NOTE GATE"), ("arp_swing", "SWING", "arp_n_vel", "VEL"),
                  ("arp_strum", "STRUM", "arp_n_chance", "CHANCE"), ("arp_gate", "GATE", "arp_n_ratchet", "RATCHET"),
                  ("arp_octave", "OCTAVE", "arp_n_oct", "NOTE OCT"), (None, "", "arp_n_slide", "SLIDE")]
LBL_W, LBL_H = 96, 18


def strip_label_pair(name, a, b, w=LBL_W, h=LBL_H, frames=METER_FRAMES):
    """2.4.2: a knob's label that follows arp_sel: frames 0-63 the pattern's knob (a), 64-127 the selected note's (b,
    in amber)"""
    im = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for half, (lab, col) in enumerate(((a, INK_DIM), (b, (255, 196, 90)))):
        c = Canvas(w, h)
        if lab:
            c.text(w / 2, h / 2, lab, 13, col, "SemiBold")
        fr = c.im.resize((w, h), Image.LANCZOS)
        for f in range(half * 64, half * 64 + 64):
            im.paste(fr, (0, f * h))
    im.save(os.path.join(ART, name + ".png"))


def strip_bar_up(name, w, h, frames=METER_FRAMES, colour=CYAN):
    """2.3.2: an envelope screen's bar: frame f fills f / 127 of the height from the bottom"""
    im = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for f in range(frames):
        v = f / (frames - 1)
        c = Canvas(w, h)
        if v > 0.004:
            col = tuple(round(a * (0.5 + 0.5 * v)) for a in colour)
            c.rrect(1, h - 1 - v * (h - 2), w - 2, max(1.5, v * (h - 2)), 2, fill=rgba(col, 235))
        im.paste(c.im.resize((w, h), Image.LANCZOS), (0, f * h))
    im.save(os.path.join(ART, name + ".png"))


def strip_bar_signed(name, w, h, frames=METER_FRAMES, colour=CYAN):
    """2.3: an OSC screen bar: frame f draws from the centre to (f - 64) / 63 of the half height (up positive)."""
    im = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for f in range(frames):
        v = max(-1.0, min(1.0, (f - 64) / 63))
        c = Canvas(w, h)
        mid, half = h / 2, h / 2 - 2
        top, bot = (mid - v * half, mid) if v >= 0 else (mid, mid - v * half)
        col = tuple(round(a * 0.55 + (b - a * 0.55) * abs(v)) for a, b in zip(colour, colour))
        c.rrect(1, top, w - 2, max(1.5, bot - top), 2, fill=rgba(col, 235))
        im.paste(c.im.resize((w, h), Image.LANCZOS), (0, f * h))
    im.save(os.path.join(ART, name + ".png"))


LFO_BOX = [(16, 66), (16, 350)]
LFO_BW, LFO_BH = 400, 270
MX = (432, 66, 832, 554)
MX_ROW0, MX_STEP = 96, 58
MX_SRC, MX_DST, MX_SLOT, MX_PW, MX_SW = 172, 414, 572, 210, 80   # 1.8: column centres, popup widths (source/dest, slot)


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
    # 1.8: source / destination narrower for the SLOT column (All, A-D: the slot the route acts on)
    for label, cx in (("SOURCE", x + MX_SRC), ("DESTINATION", x + MX_DST), ("SLOT", x + MX_SLOT), ("AMOUNT", x + 700)):
        knob_label(c, cx, yy + 58, label)
    for i in range(8):
        ry = yy + MX_ROW0 + i * MX_STEP
        c.rrect(x + 14, ry - 24, w - 28, 48, 8, fill=(26, 31, 38, 255), outline=(52, 60, 72), width=1)
        c.text(x + 40, ry, str(i + 1), 18, CYAN, "Bold")
        arrow_glyph(c, x + (MX_SRC + MX_DST) // 2, ry, "right", (120, 200, 240), 9)
    c.save("bg_mod")


# 1.8: the FX page: EQ / compressor / reverb & drive on top, chorus / delay below; knobs: cx, cy (plugin y), key, label, colour
FX_PANELS = [("EQ", (16, 66, 470, 270)), ("COMPRESSOR", (502, 66, 200, 270)), ("REVERB & DRIVE", (718, 66, 546, 270)),
             ("CHORUS", (16, 350, 470, 270)), ("DELAY", (502, 350, 762, 270))]
FX_KNOBS = [(96, 210, "eq_low", "LOW", CYAN), (251, 210, "eq_mid", "MID", CYAN), (406, 210, "eq_high", "HIGH", CYAN),
            (602, 180, "comp", "AMOUNT", ORANGE),   # 2.2: up 30 for the TYPE list and LIMITER keys
            # 2.2: five knobs (PRE-DELAY added), 105 apart; the TYPE keys under the reverb's four
            (775, 200, "rev_mix", "MIX", VIOLET), (880, 200, "rev_size", "SIZE", VIOLET), (985, 200, "rev_damp", "DAMPING", VIOLET),
            (1090, 200, "rev_predelay", "PRE-DELAY", VIOLET), (1200, 200, "drive", "DRIVE", ORANGE),
            (96, 494, "cho_mix", "MIX", GREEN), (251, 494, "cho_rate", "RATE", GREEN), (406, 494, "cho_depth", "DEPTH", GREEN),
            (582, 494, "dly_mix", "MIX", BLUE), (912, 494, "dly_fb", "FEEDBACK", BLUE), (1052, 494, "dly_tone", "DAMPING", BLUE)]


def bg_fx():
    c = Canvas(W, H, BG + (255,))
    topbar(c, "EFFECTS")
    for title, (x, yy, w, h) in FX_PANELS:
        panel(c, x, yy, w, h, title, yy + 26)
    for cx, cy, _, label, _ in FX_KNOBS:
        knob_label(c, cx, cy - 62, label)
    knob_label(c, 742, 432, "TIME")
    knob_label(c, 1190, 432, "MODE")
    c.text(26 + 444 / 2, 66 + 250, "LOW 150 HZ  ·  MID 1 KHZ  ·  HIGH 6 KHZ", 12, INK_DIM, "SemiBold", "mm")
    # 2.2: TYPE (a list) and LIMITER (keys) took the "ONE-KNOB GLUE" line; the GR meter's label
    knob_label(c, GR_X, 118, "GR")
    # 2.2: the delay's TYPE keys took this line (it said "SYNCED TO MPC'S TEMPO"; TIME's values say it)
    c.save("bg_fx")


# 1.8.2: FX's second page (MPC's sub-page MOD FX): phaser, flanger (top), tremolo / auto-pan, lo-fi (below). Its art
# covers the body under the title bar
GR_X = 676   # 2.2: the GR meter (COMPRESSOR panel, right of AMOUNT)
CT_X0 = 251 - 4 * 98 // 2   # 2.2: the chorus TYPE keys, centred in the CHORUS panel
DT_X0, DT_Y = 883 - 3 * 98 // 2, 586   # 2.2: the delay TYPE keys, centred in the DELAY panel
RT_X0, RT_W, RT_Y = 728, 98, 300   # 2.2: the reverb TYPE keys (REVERB & DRIVE panel, under its knobs)
FX2_AREA = (0, 58, W, 570)
FX2_PANELS = [("PHASER", (16, 66, 616, 270)), ("FLANGER", (648, 66, 616, 270)),
              ("TREMOLO / AUTO-PAN", (16, 350, 616, 270)), ("LO-FI", (648, 350, 616, 270))]
FX2_KNOBS = [(93, 210, "phs_mix", "MIX", VIOLET), (247, 210, "phs_rate", "RATE", VIOLET), (401, 210, "phs_depth", "DEPTH", VIOLET),
             (555, 210, "phs_fb", "FEEDBACK", VIOLET),
             (725, 210, "flg_mix", "MIX", CYAN), (879, 210, "flg_rate", "RATE", CYAN), (1033, 210, "flg_depth", "DEPTH", CYAN),
             (1187, 210, "flg_fb", "FEEDBACK", CYAN),
             (96, 494, "trm_depth", "DEPTH", GREEN), (236, 494, "trm_rate", "RATE", GREEN),
             (750, 494, "lofi_mix", "MIX", ORANGE), (956, 494, "lofi_bits", "BITS", ORANGE), (1162, 494, "lofi_rate", "RATE", ORANGE)]
TR_X0, TR_SEG_W, TR_SYNC_Y, TR_MODE_Y, TR_SHAPE_Y = 344, 134, 438, 506, 570


def bg_fx2():
    c = Canvas(W, H, BG + (255,))
    topbar(c, "EFFECTS")
    for title, (x, yy, w, h) in FX2_PANELS:
        panel(c, x, yy, w, h, title, yy + 26)
    for cx, cy, _, label, _ in FX2_KNOBS:
        knob_label(c, cx, cy - 62, label)
    for yv, lab in ((TR_SYNC_Y, "SYNC (FREE = RATE KNOB)"), (TR_MODE_Y, "MODE"), (TR_SHAPE_Y, "SHAPE")):
        c.text(TR_X0, yv - 30, lab, 12, INK_DIM, "SemiBold", "lm")
    for yv in (TR_MODE_Y, TR_SHAPE_Y):
        c.rrect(TR_X0 - 3, yv - 16 - 3, 2 * TR_SEG_W + 6, 32 + 6, 9, fill=(14, 17, 22), outline=(66, 76, 90), width=1.2)
    c.text(16 + 616 / 2, 66 + 250, "SIX-STAGE SWEEP  ·  STEREO", 12, INK_DIM, "SemiBold", "mm")
    c.text(648 + 616 / 2, 66 + 250, "0.3 - 6 MS  ·  STEREO", 12, INK_DIM, "SemiBold", "mm")
    c.text(648 + 616 / 2, 350 + 250, "BIT DEPTH  ·  SAMPLE RATE", 12, INK_DIM, "SemiBold", "mm")
    ax, ay, aw, ah = FX2_AREA
    # opaque: the panels' glass fill is translucent, and this art lies over the FX page's
    im = c.final().crop((ax, ay, ax + aw, ay + ah))
    Image.alpha_composite(Image.new("RGBA", im.size, BG + (255,)), im).save(os.path.join(ART, "bg_fx2.png"))


# 2.4.30: FX BANK, the FX tab's third page, built from the user's FX Expansion kit (art_src/fx_expansion: concept
# render + slices, 2026-11-22; the user chose "concept look + all 13 controls" and "Omni's top bar + the bay, no footer").
# The kit's bay frame (with its PCB card, OMNI-DSP FXv2 chip, A/M/C LEDs, gold fingers) is the base; the concept's baked
# knobs / labels / fader / OLED text are covered with the card's own solder mask and the live controls sit there instead.
# Coordinates: FXB_FRAME_AT = where the frame slice's top-left lands on the page; FXB_* positions are page coordinates.
FXB_SRC = os.path.join(HERE, "art_src", "fx_expansion")
FXB_FRAME_AT = (56, 100)
# the kit's README palette (README_mpc_fx_spec.md; the user: these colours, as Omni Sampler v2 looks)
FXB_CYAN, FXB_PURPLE, FXB_SILK = (0x00, 0xE5, 0xFF), (0x9B, 0x64, 0xFF), (0x90, 0xA4, 0xAE)    # #00E5FF #9B64FF #90A4AE
FXB_CHASSIS = ((0x1C, 0x20, 0x28), (0x12, 0x15, 0x1B))                                          # #1C2028 -> #12151B
FXB_PCB = ((0x1A, 0x1F, 0x28), (0x12, 0x16, 0x1E))                                              # #1A1F28 -> #12161E
FXB_GOLD = ((0xCD, 0xA2, 0x48), (0xF5, 0xD7, 0x82))                                             # #CDA248, highlight #F5D782
FXB_CARD = (36, 60, 1134, 468)   # the card in the frame slice (its solder mask is re-coloured inside this box)


def _fx(x, y):   # frame-slice coordinates -> page
    return x + FXB_FRAME_AT[0], y + FXB_FRAME_AT[1]


FXB_OLED = (585, 86, 341, 145)               # in frame coordinates: x, y, w, h (the kit's OLED slice at 0.86)
FXB_FADER = (1018, 138)                      # frame: the DRY/WET fader's centre
FXB_KNOB_R, FXB_KNOB_R_SIDE = 24, 36
_ROW_X = [625, 735, 845, 955, 1065]
# (frame x, frame y, key, LABEL, ring colour, radius)
FXB_KNOBS = [(130, 148, "fxb_rate", "RATE", FXB_CYAN, FXB_KNOB_R_SIDE), (130, 318, "fxb_depth", "DEPTH", FXB_CYAN, FXB_KNOB_R_SIDE)] + \
    [(x, 270, k, lab, col, FXB_KNOB_R) for x, k, lab, col in zip(_ROW_X, ("fxb_fb", "fxb_time", "fxb_tone", "fxb_drive", "fxb_pitch"),
                                                                    ("FEEDBACK", "TIME", "TONE", "DRIVE", "PITCH"),
                                                                    (FXB_PURPLE, FXB_CYAN, FXB_PURPLE, FXB_CYAN, FXB_PURPLE))] + \
    [(x, 350, k, lab, col, FXB_KNOB_R) for x, k, lab, col in zip(_ROW_X, ("fxb_thr", "fxb_ratio", "fxb_low", "fxb_mid", "fxb_high"),
                                                                    ("THRESHOLD", "RATIO", "LOW", "MID", "HIGH"),
                                                                    (FXB_CYAN, FXB_PURPLE, FXB_CYAN, FXB_PURPLE, FXB_CYAN))]
FXB_BYPASS = ["Active", "Bypass"]
FXB_KEY_W, FXB_KEY_H, FXB_KEY_Y = 64, 30, 232   # the ON | BYPASS keys (frame y), centred on the left column


def _fxb_img(name):
    return Image.open(os.path.join(FXB_SRC, name)).convert("RGBA")


def _fxb_font(size, weight="SemiBold"):
    return ImageFont.truetype(os.path.join(TITILLIUM, "TitilliumWeb-%s.ttf" % weight), size)


def _fxb_cover(im, box, sample_box):
    """paint `box` of the frame slice with the card's solder mask (the median colour of sample_box, a faint vertical
    gradient and grain like the card's), feathered at its edges"""
    import random
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    fill = Image.new("RGBA", (w, h))
    rnd = random.Random(2430)
    pix = fill.load()
    cy0, cy1 = FXB_CARD[1], FXB_CARD[3]
    for yy in range(h):
        base = _fxb_pcb_at(y0 + yy, cy0, cy1)
        for xx in range(w):
            n = rnd.randint(-1, 1)
            pix[xx, yy] = tuple(max(0, min(255, c + n)) for c in base) + (255,)
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1), radius=10, fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(3))
    im.paste(fill, (x0, y0), mask)


def _fxb_pcb_at(yy, y0, y1):
    f = max(0.0, min(1.0, (yy - y0) / max(1, y1 - y0)))
    return tuple(round(a + (b - a) * f) for a, b in zip(*FXB_PCB))


def _fxb_palette(im):
    """the concept card's green-teal solder mask -> the README's #1A1F28 -> #12161E (each pixel keeps its light / dark
    offset, so traces and pads stay), its gold -> #CDA248 / #F5D782; chip, LEDs, parts and the steel frame unchanged"""
    x0, y0, x1, y1 = FXB_CARD
    px = im.load()
    cls = im.filter(ImageFilter.BoxBlur(2)).load()   # decide on the neighbourhood: no speckles at the recess's edge
    for yy in range(y0, y1):
        pcb = _fxb_pcb_at(yy, y0, y1)
        for xx in range(x0, x1):
            r, g, b, a = px[xx, yy]
            lum = (r + g + b) / 3
            cr, cg, cb, _ = cls[xx, yy]
            clum = (cr + cg + cb) / 3
            if 20 <= clum <= 95 and cg >= cr + 4 and cb >= cr + 3 and max(cr, cg, cb) - min(cr, cg, cb) < 42 and lum <= 110:
                d = (lum - 38) * 0.9
                px[xx, yy] = tuple(max(0, min(255, round(c + d))) for c in pcb) + (a,)
            elif r > 110 and r > b + 45 and g > b + 15:          # gold fingers / pads
                t = max(0.0, min(1.0, (lum - 90) / 120))
                px[xx, yy] = tuple(round(c0 + (c1 - c0) * t) for c0, c1 in zip(*FXB_GOLD)) + (a,)


def fxb_knob_strip(name, colour, frames=65, size=96):
    """the concept's knob: a dark track ring, the lit arc (with glow) and the kit's knurled metal cap turned to the value"""
    cap = _fxb_img("modular_assets/knob_metal_128.png").resize((int(size * 0.72),) * 2, Image.LANCZOS)
    out = Image.new("RGBA", (size, size * frames), (0, 0, 0, 0))
    S2 = 4
    for f in range(frames):
        t = f / (frames - 1)
        big = Image.new("RGBA", (size * S2, size * S2), (0, 0, 0, 0))
        d = ImageDraw.Draw(big)
        r, wid = size * S2 * 0.46, int(size * S2 * 0.075)
        box = (size * S2 / 2 - r, size * S2 / 2 - r, size * S2 / 2 + r, size * S2 / 2 + r)
        d.arc(box, 135, 405, fill=(10, 12, 16, 255), width=wid + 4)
        d.arc(box, 135, 405, fill=(36, 42, 52, 255), width=int(wid * 0.55))
        if t > 0.004:
            glow = Image.new("RGBA", big.size, (0, 0, 0, 0))
            ImageDraw.Draw(glow).arc(box, 135, 135 + 270 * t, fill=colour + (200,), width=wid + 10)
            big = Image.alpha_composite(big, glow.filter(ImageFilter.GaussianBlur(10)))
            ImageDraw.Draw(big).arc(box, 135, 135 + 270 * t, fill=colour + (255,), width=wid)
        fr = big.resize((size, size), Image.LANCZOS)
        turned = cap.rotate(-(-135 + 270 * t), resample=Image.BICUBIC)
        fr.alpha_composite(turned, ((size - cap.width) // 2, (size - cap.height) // 2))
        out.paste(fr, (0, f * size))
    out.save(os.path.join(ART, name + ".png"))


def fxb_fader_strip(name, w=96, h=96, frames=65):
    """the concept's DRY/WET fader: a slot with end marks and the kit's metal cap, left (dry) to right (wet). SQUARE
    frames: the skin cuts a knob's filmstrip into square frames (136 x 46 frames slid up and down on the MPC, 2.4.30)"""
    sl = _fxb_img("sliced_from_concept/slider_dry_wet.png")
    band = sl.crop((0, 44, sl.width, 94))
    bbox = band.convert("L").point(lambda v: 255 if v > 120 else 0).getbbox() or (10, 4, 88, 46)
    cap = band.crop((max(0, bbox[0] - 2), 0, min(band.width, bbox[2] + 2), band.height))
    ch = 40   # the cap's height inside the square frame
    cap = cap.resize((int(cap.width * ch / cap.height), ch), Image.LANCZOS)
    out = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for f in range(frames):
        t = f / (frames - 1)
        fr = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        d = ImageDraw.Draw(fr)
        d.rounded_rectangle((8, h / 2 - 3, w - 8, h / 2 + 3), radius=3, fill=(8, 10, 14, 255), outline=(60, 70, 82, 255))
        for ex in (2, w - 6):
            d.rectangle((ex, h / 2 - 2, ex + 4, h / 2 + 2), fill=(150, 160, 170, 255))
        cx = 8 + cap.width / 2 + (w - 16 - cap.width) * t
        fr.alpha_composite(cap, (int(cx - cap.width / 2), (h - cap.height) // 2))
        out.paste(fr, (0, f * h))
    out.save(os.path.join(ART, name + ".png"))


def fxb_key_images():
    """ON | BYPASS keys in the concept's BYPASS look: navy key, cyan rim and glow when lit"""
    for opt in FXB_BYPASS:
        for lit in (False, True):
            S2 = 4
            w, h = FXB_KEY_W * S2, FXB_KEY_H * S2
            im = Image.new("RGBA", (w + 16 * S2, h + 16 * S2), (0, 0, 0, 0))
            d = ImageDraw.Draw(im)
            o = 8 * S2
            if lit:
                gl = Image.new("RGBA", im.size, (0, 0, 0, 0))
                ImageDraw.Draw(gl).rounded_rectangle((o, o, o + w, o + h), radius=7 * S2, outline=FXB_CYAN + (220,), width=5 * S2)
                im = Image.alpha_composite(im, gl.filter(ImageFilter.GaussianBlur(5 * S2)))
                d = ImageDraw.Draw(im)
            d.rounded_rectangle((o, o, o + w, o + h), radius=7 * S2, fill=(24, 58, 96, 255) if lit else (20, 26, 36, 255),
                                outline=(FXB_CYAN + (255,)) if lit else (64, 76, 92, 255), width=int(1.6 * S2))
            label = "ON" if opt == "Active" else "BYPASS"
            d.text((o + w / 2, o + h / 2), label, font=_fxb_font(int(15 * S2)), anchor="mm",
                   fill=(210, 250, 255, 255) if lit else (120, 134, 150, 255))
            im = im.crop((o - 2 * S2, o - 2 * S2, o + w + 2 * S2, o + h + 2 * S2)).resize((FXB_KEY_W + 4, FXB_KEY_H + 4), Image.LANCZOS)
            im = im.crop((2, 2, 2 + FXB_KEY_W, 2 + FXB_KEY_H))
            im.save(os.path.join(ART, opt_name("fxb_bypass", opt) + ("_on" if lit else "_off") + ".png"))


# 2.4.33: the slot keys A-D on the bay's bottom-left plate (frame coordinates), mirroring PROGRAM # on the right
FXB_SLOT_Y, FXB_SLOT_X0, FXB_SLOT_W, FXB_SLOT_H, FXB_SLOT_GAP = 496, 124, 44, 28, 6


def fxb_slot_images():
    """the slot keys: the ON | BYPASS look, a letter each; lit = the slot in use"""
    S2 = 4
    for opt in FXB_SLOTS:
        for lit in (False, True):
            w, h = FXB_SLOT_W * S2, FXB_SLOT_H * S2
            im = Image.new("RGBA", (w + 16 * S2, h + 16 * S2), (0, 0, 0, 0))
            o = 8 * S2
            if lit:
                gl = Image.new("RGBA", im.size, (0, 0, 0, 0))
                ImageDraw.Draw(gl).rounded_rectangle((o, o, o + w, o + h), radius=6 * S2, outline=FXB_CYAN + (220,), width=5 * S2)
                im = Image.alpha_composite(im, gl.filter(ImageFilter.GaussianBlur(4 * S2)))
            d = ImageDraw.Draw(im)
            d.rounded_rectangle((o, o, o + w, o + h), radius=6 * S2, fill=(24, 58, 96, 255) if lit else (20, 26, 36, 255),
                                outline=(FXB_CYAN + (255,)) if lit else (64, 76, 92, 255), width=int(1.6 * S2))
            d.text((o + w / 2, o + h / 2), opt, font=_fxb_font(int(16 * S2), "Bold"), anchor="mm",
                   fill=(210, 250, 255, 255) if lit else (120, 134, 150, 255))
            im = im.crop((o, o, o + w, o + h)).resize((FXB_SLOT_W, FXB_SLOT_H), Image.LANCZOS)
            im.save(os.path.join(ART, opt_name("fxb_slot", opt) + ("_on" if lit else "_off") + ".png"))


# 2.4.32: the program number: a 3-digit screen on the bay's bottom-right plate (frame coordinates) and its keypad
# (page coordinates, over the chip: no controls under it), shown while fxb_pad is Open
FXB_NUM = (1010, 496, 112, 32)              # frame: centre x, centre y, w, h
FXB_PAD = (300, 158, 236, 300)              # page: x, y, w, h
FXB_KEY_W2, FXB_KEY_H2, FXB_KEY_GAP = 64, 50, 8
FXB_PADKEYS = [("7", "fxb_k7"), ("8", "fxb_k8"), ("9", "fxb_k9"), ("4", "fxb_k4"), ("5", "fxb_k5"), ("6", "fxb_k6"),
            ("1", "fxb_k1"), ("2", "fxb_k2"), ("3", "fxb_k3"), ("CLR", "fxb_kclr"), ("0", "fxb_k0"), ("ENTER", "fxb_kenter")]


def fxb_keypad_art():
    """the keypad's panel, its keys (navy, cyan rim; lit = cyan glow) and the 3-digit screen's touch overlay"""
    x, y, w, h = FXB_PAD
    S2 = 3
    im = Image.new("RGBA", (w * S2, h * S2), (0, 0, 0, 0))
    gl = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ImageDraw.Draw(gl).rounded_rectangle((4 * S2, 4 * S2, (w - 4) * S2, (h - 4) * S2), radius=12 * S2, outline=FXB_CYAN + (200,), width=4 * S2)
    im = Image.alpha_composite(im, gl.filter(ImageFilter.GaussianBlur(4 * S2)))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((4 * S2, 4 * S2, (w - 4) * S2, (h - 4) * S2), radius=12 * S2, fill=FXB_CHASSIS[1] + (250,),
                        outline=FXB_CYAN + (255,), width=int(1.6 * S2))
    d.rounded_rectangle((14 * S2, 12 * S2, (w - 14) * S2, 50 * S2), radius=6 * S2, fill=(6, 10, 14, 255), outline=(52, 70, 84, 255), width=S2)
    d.text((22 * S2, 31 * S2), "PROGRAM #", font=_fxb_font(11 * S2), anchor="lm", fill=FXB_SILK + (255,))
    im.resize((w, h), Image.LANCZOS).save(os.path.join(ART, "fxb_keypad.png"))
    for lab, key in FXB_PADKEYS:
        for lit in (False, True):
            kw, kh = FXB_KEY_W2 * S2, FXB_KEY_H2 * S2
            k = Image.new("RGBA", (kw + 12 * S2, kh + 12 * S2), (0, 0, 0, 0))
            o = 6 * S2
            if lit:
                g = Image.new("RGBA", k.size, (0, 0, 0, 0))
                ImageDraw.Draw(g).rounded_rectangle((o, o, o + kw, o + kh), radius=8 * S2, outline=FXB_CYAN + (230,), width=5 * S2)
                k = Image.alpha_composite(k, g.filter(ImageFilter.GaussianBlur(4 * S2)))
            kd = ImageDraw.Draw(k)
            accent = FXB_PURPLE if lab in ("CLR", "ENTER") else FXB_CYAN
            kd.rounded_rectangle((o, o, o + kw, o + kh), radius=8 * S2, fill=(24, 58, 96, 255) if lit else (20, 26, 36, 255),
                                 outline=accent + (255,) if lit else (64, 76, 92, 255), width=int(1.6 * S2))
            kd.text((o + kw / 2, o + kh / 2), lab, font=_fxb_font((22 if len(lab) == 1 else 15) * S2, "Bold" if len(lab) == 1 else "SemiBold"),
                    anchor="mm", fill=(220, 250, 255, 255) if lit else (196, 208, 220, 255))
            k = k.crop((o, o, o + kw, o + kh)).resize((FXB_KEY_W2, FXB_KEY_H2), Image.LANCZOS)
            k.save(os.path.join(ART, "fxb_key_%s_%s.png" % (key, "on" if lit else "off")))
    _, _, nw, nh = FXB_NUM
    Image.new("RGBA", (nw, nh), (0, 0, 0, 0)).save(os.path.join(ART, "fxb_numtap_off.png"))   # the screen shows through
    t = Image.new("RGBA", (nw * S2, nh * S2), (0, 0, 0, 0))
    ImageDraw.Draw(t).rounded_rectangle((S2, S2, (nw - 1) * S2, (nh - 1) * S2), radius=6 * S2, outline=FXB_CYAN + (255,), width=2 * S2)
    t.resize((nw, nh), Image.LANCZOS).save(os.path.join(ART, "fxb_numtap_on.png"))


def bg_fx3():
    page = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    # the chassis: the kit's anodised palette (#1C2028 -> #12151B) with a fine brushed grain
    import random
    rnd = random.Random(30)
    pd = ImageDraw.Draw(page)
    for yy in range(H):
        f = yy / (H - 1)
        base = [round(a + (b - a) * f) for a, b in zip(*FXB_CHASSIS)]
        pd.line([(0, yy), (W, yy)], fill=tuple(max(0, min(255, c + rnd.randint(-1, 1))) for c in base) + (255,))
    # the title under Omni's bar: DSP FX EXPANSION between two rules (as in the concept)
    pd.text((W / 2, 80), "DSP FX EXPANSION", font=_fxb_font(20), anchor="mm", fill=(196, 208, 216, 255))
    for x0, x1 in ((420, 530), (750, 860)):
        pd.line([(x0, 81), (x1, 81)], fill=(84, 96, 110, 255), width=1)
    # the bay: the kit's frame slice, with the concept's baked controls covered by solder mask
    fr = _fxb_img("sliced_from_concept/expansion_bay_frame.png")
    _fxb_palette(fr)
    screws = [(x, y, fr.crop((x - 16, y - 16, x + 16, y + 16))) for x, y in ((66, 84), (1103, 84), (66, 405), (1103, 405))]
    _fxb_cover(fr, (56, 76, 212, 416), (960, 282, 990, 300))            # MIX / BYPASS / DECAY column
    _fxb_cover(fr, (562, 74, 1098, 424), (960, 282, 990, 300))          # OLED, fader, the bottom knob row
    for x, y, sc in screws:
        fr.alpha_composite(sc, (x - 16, y - 16))
    fd = ImageDraw.Draw(fr)
    # the OLED: the kit's screen, its two text lines cleared (Omni writes the program and the card there)
    ol = _fxb_img("sliced_from_concept/oled_waveform_screen.png")
    od = ImageDraw.Draw(ol)
    bg_c = ol.getpixel((60, 60))[:3]
    for y0, y1 in ((14, 43), (125, 154)):
        od.rectangle((22, y0, 372, y1), fill=bg_c + (255,))
    ox, oy, ow, oh = FXB_OLED
    fr.alpha_composite(ol.resize((ow, oh), Image.LANCZOS), (ox, oy))
    # the DRY/WET block: two readout wells (TIME above, MIX below) and the caption
    fx_, fy_ = FXB_FADER
    for yy in (fy_ - 46, fy_ + 46):
        fd.rounded_rectangle((fx_ - 46, yy - 15, fx_ + 46, yy + 15), radius=6, fill=FXB_CHASSIS[1] + (255,), outline=FXB_CYAN + (90,), width=1)
    fd.text((fx_, fy_ + 72), "DRY/WET", font=_fxb_font(15), anchor="mm", fill=FXB_SILK + (255,))
    # silkscreen labels under the knobs (the concept's), the ON | BYPASS caption well
    for x, y, _, lab, _, r in FXB_KNOBS:
        fd.text((x, y + r + 13), lab, font=_fxb_font(15 if r > 30 else 13), anchor="mm", fill=FXB_SILK + (255,))
    ncx, ncy, nw, nh = FXB_NUM   # 2.4.32: the 3-digit screen on the bottom-right plate
    fd.rounded_rectangle((ncx - nw / 2 - 3, ncy - nh / 2 - 3, ncx + nw / 2 + 3, ncy + nh / 2 + 3), radius=7, fill=(30, 34, 40, 255),
                         outline=(120, 130, 140, 255), width=1)
    fd.rounded_rectangle((ncx - nw / 2, ncy - nh / 2, ncx + nw / 2, ncy + nh / 2), radius=5, fill=(6, 10, 14, 255),
                         outline=FXB_CYAN + (150,), width=1)
    fd.text((ncx - nw / 2 - 12, ncy), "PROGRAM #", font=_fxb_font(13), anchor="rm", fill=FXB_SILK + (255,))
    # 2.4.33: the slot keys' caption and a recessed well under them (the keys are live widgets over it)
    sx0 = FXB_SLOT_X0 - FXB_SLOT_W // 2
    sx1 = FXB_SLOT_X0 + 3 * (FXB_SLOT_W + FXB_SLOT_GAP) + FXB_SLOT_W // 2
    fd.rounded_rectangle((sx0 - 4, FXB_SLOT_Y - FXB_SLOT_H / 2 - 4, sx1 + 4, FXB_SLOT_Y + FXB_SLOT_H / 2 + 4), radius=8,
                         fill=(30, 34, 40, 255), outline=(120, 130, 140, 255), width=1)
    fd.text((sx1 + 14, FXB_SLOT_Y), "SLOT", font=_fxb_font(13), anchor="lm", fill=FXB_SILK + (255,))
    page.alpha_composite(fr, FXB_FRAME_AT)
    ax, ay, aw, ah = FX2_AREA
    page.crop((ax, ay, ax + aw, ay + ah)).save(os.path.join(ART, "bg_fx3.png"))


def omni_tile(path=None):
    """2.4.30: MPC's Sounds > INSTRUMENTS tile (270 x 110, <skin>/Plugin Skins/browser_images/soundsmode.png via vst.json
    "tile"; the user: a picture like the other plugins have, "Ω2" with a graphic, the Ω gold). Palette: the FX kit's
    README (chassis #1C2028 -> #12151B, gold #CDA248 / #F5D782, cyan #00E5FF, purple #9B64FF, silkscreen #90A4AE)."""
    import math as _m
    S2, TW, TH = 4, 270, 110
    W2, H2 = TW * S2, TH * S2
    im = Image.new("RGBA", (W2, H2), (0, 0, 0, 255))
    d = ImageDraw.Draw(im)
    for yy in range(H2):   # anodised chassis
        f = yy / (H2 - 1)
        d.line([(0, yy), (W2, yy)], fill=tuple(round(a + (b - a) * f) for a, b in zip(*FXB_CHASSIS)) + (255,))
    # PCB traces (right half) with pads, cyan and purple like the expansion card
    tr = Image.new("RGBA", im.size, (0, 0, 0, 0))
    td = ImageDraw.Draw(tr)
    for k in range(7):
        yy = (14 + k * 13) * S2
        col = (FXB_CYAN if k % 2 == 0 else FXB_PURPLE) + (70,)
        x0 = (118 + (k * 7) % 20) * S2
        td.line([(x0, yy), (x0 + 30 * S2, yy), (x0 + 40 * S2, yy + 8 * S2 * (1 if k % 3 else -1)), (W2, yy + 8 * S2 * (1 if k % 3 else -1))],
                fill=col, width=int(1.2 * S2))
        td.ellipse((x0 - 3 * S2, yy - 3 * S2, x0 + 3 * S2, yy + 3 * S2), outline=FXB_GOLD[0] + (150,), width=S2)
    im = Image.alpha_composite(im, tr)
    # the waveform (the OLED's), cyan with glow, along the bottom right
    wv = Image.new("RGBA", im.size, (0, 0, 0, 0))
    wd = ImageDraw.Draw(wv)
    cy = 84 * S2
    for i in range(0, 126 * S2, S2 * 2):
        x = 126 * S2 + i
        a = 13 * S2 * abs(_m.sin(i / (9.0 * S2))) * (0.35 + 0.65 * abs(_m.sin(i / (41.0 * S2) + 0.6))) * _m.exp(-i / (150.0 * S2))
        wd.line([(x, cy - a), (x, cy + a)], fill=FXB_CYAN + (235,), width=int(1.1 * S2))
    im = Image.alpha_composite(im, wv.filter(ImageFilter.GaussianBlur(3 * S2)))
    im = Image.alpha_composite(im, wv)
    d = ImageDraw.Draw(im)
    # gold edge fingers along the bottom (the card's), and a thin gold frame
    for k in range(18):
        x = (8 + k * 6) * S2
        d.rectangle((x, H2 - 9 * S2, x + 4 * S2, H2 - 2 * S2), fill=FXB_GOLD[0] + (255,))
    d.rounded_rectangle((S2, S2, W2 - S2, H2 - S2), radius=8 * S2, outline=FXB_GOLD[0] + (160,), width=S2)
    # the gold Ω: a vertical #F5D782 -> #CDA248 gradient through the glyph, with a warm glow, then the 2
    omega_font = ImageFont.truetype(MICHROMA, 74 * S2)
    mask = Image.new("L", im.size, 0)
    ImageDraw.Draw(mask).text((58 * S2, 52 * S2), "Ω", font=omega_font, anchor="mm", fill=255)
    grad = Image.new("RGBA", im.size)
    gd = ImageDraw.Draw(grad)
    for yy in range(H2):
        f = max(0.0, min(1.0, (yy - 14 * S2) / (78 * S2)))
        gd.line([(0, yy), (W2, yy)], fill=tuple(round(b + (a - b) * f) for a, b in zip(*FXB_GOLD)) + (255,))
    glow = Image.new("RGBA", im.size, FXB_GOLD[0] + (0,))
    glow.putalpha(mask.filter(ImageFilter.GaussianBlur(7 * S2)).point(lambda v: int(v * 0.55)))
    im = Image.alpha_composite(im, glow)
    im.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(im)
    d.text((96 * S2, 36 * S2), "2", font=ImageFont.truetype(MICHROMA, 30 * S2), anchor="mm", fill=(236, 244, 250, 255))
    # name
    d.text((126 * S2, 30 * S2), "OMNI SAMPLER", font=ImageFont.truetype(MICHROMA, 12 * S2), anchor="lm", fill=(226, 234, 240, 255))
    d.text((126 * S2, 48 * S2), "MR HIGHMAN  ·  SAMPLER + SYNTH + FX", font=_fxb_font(8 * S2), anchor="lm", fill=FXB_SILK + (255,))
    out = im.resize((TW, TH), Image.LANCZOS).convert("RGB")
    path = path or os.path.join(HERE, "art", "tile.png")
    out.save(path)
    return path


def fxb_art():
    fxb_keypad_art()   # 2.4.32
    ql2_overlays()   # 2.4.30: the Q-LINKS 2 pages' badge + rings
    omni_tile()   # 2.4.30: the Instruments browser tile (art/tile.png, vst.json "tile")
    bg_fx3()
    fxb_knob_strip("fxb_knob_cyan", FXB_CYAN)
    fxb_knob_strip("fxb_knob_purple", FXB_PURPLE)
    fxb_fader_strip("fxb_fader")
    fxb_key_images()
    fxb_slot_images()   # 2.4.33


SLOT_X0, SLOT_STEP, SLOT_Y, SLOT_W, SLOT_H = 16, 314, 66, 300, 336
LMAP = (16, 412, 1248, 208)
LT_X0, LT_STEP, LT_CY, LT_W, LT_H = 96, 34, 470, 32, 44


def bg_layers():
    c = Canvas(W, H, BG + (255,))
    topbar(c, "LAYERS")
    for i in range(4):
        x = SLOT_X0 + i * SLOT_STEP
        panel(c, x, SLOT_Y, SLOT_W, SLOT_H, colour=SLOT_COLOURS[i] if MODULES else None)
        col = SLOT_COLOURS[i]
        c.glow(lambda l, x=x, col=col: l.rrect(x + 14, SLOT_Y + 12, 40, 36, 8, fill=rgba(col)), 3, 1.1)
        c.text(x + 34, SLOT_Y + 31, "ABCD"[i], 22, BG, "Bold")
        c.rrect(x + 62, SLOT_Y + 14, SLOT_W - 76, 32, 7, fill=DARK, outline=(64, 74, 88), width=1)
        for lx, lab in ((x + 56, "LOW KEY"), (x + 150, "HIGH KEY"), (x + 244, "KEY SHIFT")):
            knob_label(c, lx, SLOT_Y + 108, lab)
        for lx, lab in ((x + 56, "VOLUME"), (x + 150, "PAN"), (x + 244, "TRANSPOSE")):
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
        ("S612 / S900 / S950", ".s612 .p9 .s9 .s9c", True),   # 1.8.5: S612
        ("MESA", ".s3p", False),
        ("S5000 / S6000 / Z4 / Z8", ".akp .akm", True),
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
        ("SoundFont 1 / 2", ".sf2 .sbk", True),   # 1.8.5: SoundFont 1
        ("WAV (PCM)", ".wav .wave", True),
        ("WAV float / ADPCM / RF64", ".wav", False),
        ("AIFF / AIFC", ".aif .aiff .aifc", False),
        ("FLAC / Ogg Vorbis", ".flac .ogg", False)]),
    ("DISK IMAGES", (440, 350), [
        ("CD / DVD: ISO 9660, BIN / CUE", ".iso .bin .cue .img", True),
        ("FAT12/16/32: floppy, ZIP, CF", ".img .ima .dsk .raw", True),
        ("Akai S1000 / S3000 CD + HD", ".iso .img .hda .hds", True),
        ("Akai floppies", ".img .dsk", False),
        ("E-mu EOS / EIII CD + disk", ".iso .img .e4 .e3", True),
        ("Floppy: HFE (E-mu FM, IBM MFM/FM)", ".hfe", True),
        ("Floppy: IMD, Teledisk", ".imd .td0", True)]),   # 1.8.5: Teledisk
    ("ROLAND", (864, 350), [
        ("S-700 CD / HD: S-750/760/770", ".iso .img .bin", False),
        ("S-700 floppy, SP-700, DJ-70", ".img", False),
        ("S-50/S-550/S-330/W-30 floppy", ".img .out", False),
        ("S-500 CD-ROM (LAND)", ".iso .bin", True)]),
]
# 1.7: INFO page 2 (PAGE 1 / PAGE 2 in the title bar): newer formats, and what cannot be read. ok None = not readable
INFO_PANELS2 = [
    ("REASON / LOGIC / SYNTH WAVES", (16, 66), [
        ("Reason NN-XT patches", ".sxt", True),
        ("  samples next to the patch", ".wav .aif", True),
        ("Logic EXS24 / Sampler instruments", ".exs", True),   # 1.8
        ("  samples next to it or below", ".wav .aif .caf", True),
        ("Single cycle waveforms", ".wav (256-4096)", True),
        ("Wavetables (Serum, Surge, WaveEdit)", ".wav .wt", True),   # 1.8.1: .wt, chunkless WAVs
        ("  WT POSITION: SETUP + mod matrix", "", True)]),
    ("ROLAND MV / NATIVE INSTRUMENTS", (440, 66), [
        ("MV-8000 / MV-8800 patches", ".mv0", True),
        ("Maschine 2/3 kits (groups)", ".mxgrp", True),
        ("Maschine 2/3 projects", ".mxprj", True),
        ("Maschine 1 kits", ".mgrp", False),
        ("Battery 1-3 / 4 kits", ".kit .nbkt", True),   # 1.8.5: Battery 1-3
        ("  kits play their preview when tapped", ".ogg", True),
        ("Roland JD-800 wave ROMs", "ROM dumps", True)]),   # 1.8.1
    ("NOT READABLE", (864, 66), [
        ("Reason ReFills", ".rfl", None),
        ("Kontakt Player / NKS libraries", ".nki .nkx", None),
        ("NI Massive presets (a synth, no samples)", ".nmsv", None)]),
    # 1.8
    ("ENSONIQ", (16, 350), [
        ("EPS / EPS-16+ / ASR-10 / ASR-88", ".efe", True),
        ("  disks: raw floppy / HD images", ".img .iso", True),
        ("  Giebler disks", ".gkh .eda .ede", True),
        ("  HFE floppies", ".hfe", True),
        ("  patch selects as presets", "", True),
        ("SD-1 / VFX-SD programs (ROMs)", ".syx", True),   # 1.8.1
        ("SQ-1 / KT-76 / MR waves", "ROM dumps", True)]),   # 1.8.1
    ("YAMAHA", (864, 350), [
        ("Motif XS / XF, MOXF libraries", ".x0a .x0v .x3a .x6a ...", True),
        ("Montage / MODX: performances", ".x7l .x7u .x8l .x8u", True),
        ("Montage M", ".y2l .y2u", False),
        ("Motif ES voice + sample files", ".w7a + .w8a", True)]),
    ("KORG", (440, 350), [
        ("Trinity / Triton / Karma multisamples", ".kmp", True),
        ("  samples (8 / 16 bit)", ".ksf", True),
        ("  load scripts (all multisamples)", ".ksc", True),
        ("TRITON content dumps", "manifest.csv", True),   # 1.8
        ("TRITON / TRINITY program banks", ".pcg", True),   # 1.8.5: TRITON
        ("M1 programs + combis (ROM/disk)", ".syx", True),   # 1.8
        ("Wavestation / EX / A/D / SR banks", ".syx", True)]),   # 1.8.8: base + EX waves, ROM bank
    ("YAMAHA", (864, 350), [
        ("Motif XS libraries", ".x0a .x0w .x0v", True),
        ("Motif XF libraries", ".x3a .x3w .x3v", True),
        ("MOXF libraries", ".x6a .x6w .x6v", False),
        ("Montage libraries", ".x7u .x7l .x7a", True),
        ("Montage M / MODX libraries", ".y2* .x8*", False),
        ("Motif ES waveforms", ".w7a .w8a", True)]),
]
INFO_W, INFO_H = 400, 272


def info_panels(c, panels):
    # a later panel at the same place replaces an earlier one (YAMAHA is listed twice; the flat look's opaque panel
    # hid the first, the rack look's modules are under the text)
    last = {pos: k for k, (_, pos, _) in enumerate(panels)}
    panels = [p for k, p in enumerate(panels) if last[p[1]] == k]
    for title, (x, y), rows in panels:
        panel(c, x, y, INFO_W, INFO_H, title, y + 26)
        for i, (name, exts, ok) in enumerate(rows):
            ry = y + 64 + i * 29
            col = GREEN if ok else YELLOW if ok is False else RED
            c.glow(lambda l, ry=ry, col=col: l.circle(x + 22, ry, 4.5, fill=rgba(col)), 2, 1.0)
            c.text(x + 36, ry, name, 14, INK, "SemiBold", "lm")
            c.text(x + INFO_W - 16, ry, exts, 13, INK_DIM, "Regular", "rm")


def info_legend(c, x, y, red=False):
    keys = [(GREEN, "checked against test files"), (YELLOW, "supported, not yet tested on real files")]
    if red:
        keys.append((RED, "detected, but cannot be read (encrypted / no samples)"))
    for i, (col, t) in enumerate(keys):
        c.glow(lambda l, i=i, col=col: l.circle(x + 22, y + i * 18, 4, fill=rgba(col)), 2, 1.0)
        c.text(x + 34, y + i * 18, t, 12, INK_DIM, "SemiBold", "lm")


def bg_info():
    c = Canvas(W, H, BG + (255,))
    topbar(c, "SUPPORTED FORMATS")
    info_panels(c, INFO_PANELS)
    x, y = 864, 350
    info_legend(c, x, y + 192)
    c.text(x + 22, y + 244, "Disk images open like folders; presets played from them", 12, INK_DIM, "Regular", "lm")
    c.text(x + 22, y + 260 - 2, "are saved to the Plugin Library (Extracted).", 12, INK_DIM, "Regular", "lm")
    c.save("bg_info")
    # page 2
    c = Canvas(W, H, BG + (255,))
    topbar(c, "SUPPORTED FORMATS")
    info_panels(c, INFO_PANELS2)
    info_legend(c, 864, 66 + 192, red=True)   # 1.8: under NOT READABLE (KORG is full; the notes moved to page 3)
    c.save("bg_info2")
    # 1.8: page 3, notes and about
    c = Canvas(W, H, BG + (255,))
    topbar(c, "SUPPORTED FORMATS")
    x, y = 16, 66
    panel(c, x, y, 1248, INFO_H, "NOTES", y + 26)
    notes = ["Reason NN-XT patches (.sxt) load with their samples when the samples are files next to the patch (or in a folder",
             "named after it). A patch whose samples live inside a ReFill plays them only if they were exported as files.",
             "ReFills (.rfl) are encrypted by Propellerhead so that only Reason opens them: export their samples and NN-XT",
             "patches with Reason (or the ReFill's own sample folders) and load those.",
             "Kontakt Player / NKS libraries are encrypted the same way: they are detected and reported, not played.",
             "MV-8000 / MV-8800 drum kits start on the MV's pad 1 (A0): they are moved up to MPC's pad 1 (C1).",
             "A single cycle or wavetable plays as an oscillator: band-limited per octave, shaped by the ADSR faders, filter and MOD.",
             "Maschine kits put each sound on its pad from MPC's pad 1; drum synth and plugin sounds have no samples: silent.",
             "Yamaha parts that use the keyboard's own preset waveforms (not stored in the library) are left out, with a warning.",
             "Vintage ROM dumps open on their own, any file name: drum machines (as kits), JD-800, Ensoniq, K2000, SC-55/88. Tap a chip.",   # 1.8.1, SC-88 1.8.12
             "Trackers: each instrument of an .xm / .it / .s3m module (or .xi / .iti / .its file) is a preset. Songs are not played; no .mod."]   # 1.8.2
    for i, t in enumerate(notes):
        c.text(x + 22, y + 56 + i * 20, t, 14, INK, "Regular", "lm")
    y = 350
    panel(c, x, y, 1248, INFO_H, "ABOUT", y + 26)
    about = [("Omni Sampler %s by MR HighMan" % VERSION, 18, INK, "SemiBold"),
             ("Contact: mrhighmanproductions@gmail.com", 18, INK, "SemiBold"),
             ("Bug reports and format requests welcome: say which file, which sampler it came from, and what went wrong.", 14, INK_DIM, "Regular")]
    # the readers' LGPL-3.0 attribution (ConvertWithMoss) ships in licenses/NOTICE.txt with every release
    for i, (t, size, col, wt) in enumerate(about):
        c.text(640, y + 80 + i * 40, t, size, col, wt, "mm")
    c.save("bg_info3")


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
    bg_play2()
    strip_states("plock", PLOCK_W, PLOCK_H, 4, draw_plock)
    bg_browse()
    if RACK:
        bg_browse_rack()   # 1.8.13: replaces the flat BROWSE art
        rack_images()
        import spark   # 2.1.5: the logo's spark (a 128-frame meter strip)
        spark.build(os.path.join(ART, "logo_spark.png"))
    bg_setup()
    bg_info()
    bg_mod()
    bg_layers()
    bg_fx()
    bg_fx2()
    fxb_art()    # 2.4.30: FX BANK (bg_fx3 + its knob / fader strips and ON | BYPASS keys)
    bg_synth()   # 2.3
    bg_synth2()  # 2.3.1
    syn_response_images()
    bg_synth3()  # 2.3.2
    bg_synth3(SE_ENVS2, SE_LFOS2, "bg_synth6.png")   # 2.4.17: MOD 2
    syn_lfo_images()
    bg_synth4()  # 2.3.3
    bg_synth4(9, "bg_synth7.png")   # 2.4.17: MATRIX 2
    bg_synth5()
    bg_arp()     # 2.4
    bg_arp2()    # 2.4.2
    strip_wave("wave_bar", WAVE_W, WAVE_H)
    strip_gr("gr_meter", 14, 84)   # 2.2
    # 2.3.3: one strip for both OSC screens and one for the three envelopes, short: the skin tools pad meter frames to
    # squares (h x h x 128 x 4 bytes decoded each)
    strip_bar_signed("syn_bar", 12, 96, colour=(150, 215, 255))
    strip_bar_up("syn_env", 11, 72, colour=(200, 210, 225))   # 2.3.2 / 2.3.3: one shared, 72 tall
    strip_led("arp_led", 30, 8)   # 2.4
    strip_arp_cell("arp_cell", AG_DX, AG_CH)   # 2.4.1
    for i, (_, a, _, b) in enumerate(ARP_KNOB_SLOTS):   # 2.4.2
        strip_label_pair("arp_lbl%d" % i, a, b)
    strip_states("ptile", PTILE_W, 16, 3, draw_ptile)
    strip_states("ltile", LT_W, LT_H, 16, draw_ltile)
    for col, n in KNOB_STRIPS.items():
        strip_knob(n, col)
    strip_fader("fader", FADER_W, FADER_H, colour=section_colour("ENVELOPE") or (80, 150, 255))
    strip_fader("fader_small", 30, 50)
    for lit in (False, True):
        for key, opts, _ in SEG_GROUPS:
            for opt in opts:
                seg_image(opt_name(key, opt), SEG_W, SEG_H, lit, opt.upper())
        for key, opts, _, _, _ in SETUP_OPTIONS:
            ow, pre = set_opt(key)
            for opt in opts:
                seg_image(pre + opt_name(key, opt), ow, OPT_H, lit, opt.upper())
        arrow_image("prog_prev", 38, 30, "left", lit, BLUE)
        arrow_image("prog_next", 38, 30, "right", lit, VIOLET)
        # 2.4: the ARP page's keys and the grid's dots
        seg_image(opt_name("arp_on", "Off"), 70, 30, lit, "OFF", RED)
        seg_image(opt_name("arp_on", "On"), 110, 30, lit, "ARP ON", GREEN)
        for opt in ARP_MODES:
            seg_image(opt_name("arp_mode", opt), 130, 32, lit, opt.upper(), SM_GREEN)
        for opt in ARP_PAGES:
            seg_image(opt_name("arp_page", opt), 72, 30, lit, "PAGE " + opt, SM_CYAN)
        for opt in ("Off", "On"):
            seg_image(opt_name("arp_latch", opt), 62, 30, lit, opt.upper(), SM_GREEN)
            seg_image(opt_name("arp_tie", opt), 62, 30, lit, opt.upper(), SM_GREEN)
            seg_image(opt_name("arp_follow", opt), 52, 30, lit, opt.upper(), SM_CYAN)
        for opt in ("Off", "On"):   # 2.4.2
            seg_image(opt_name("arp_n_slide", opt), 44, 30, lit, opt.upper(), (255, 196, 90))
        for opt in ARP_DIRS:
            seg_image(opt_name("arp_dir", opt), 130, 32, lit, opt.upper(), SM_CYAN)
        button_image("b_arp_g_generate", 220, 46, "GENERATE", lit, SM_GREEN, 17)
        for k, lab in ARP_GEN_KEYS:
            button_image("b_" + k, 130, 40, lab, lit, RED if k == "arp_g_undo" else SM_GREEN, 14)
        button_image("b_arp_e_apply", 200, 40, "APPLY EUCLID", lit, SM_CYAN, 14)
        button_image("b_arp_v_go", 130, 40, "VOICE", lit, SM_VIOLET, 14)   # 2.4.14
        for k, lab in MUT_KEYS:   # 2.4.12
            button_image("b_" + k, 112 if k != "mut_undo" else 84, 36, lab, lit, RED if k == "mut_undo" else (SM_VIOLET if k == "mut_go" else SM_ORANGE), 13)
        for opt in MUT_AMOUNTS:
            seg_image(opt_name("mut_amount", opt), 80, 30, lit, opt.upper(), SM_VIOLET)
        for k, lab in ARP_BREED_KEYS:
            button_image("b_" + k, 130, 36, lab, lit, SM_CYAN, 13)
        for opt in ARP_DRAWS:
            seg_image(opt_name("arp_draw", opt), 62, 30, lit, opt.upper(), SM_CYAN)
        for opt in ARP_STRUM_DIRS:
            seg_image(opt_name("arp_strum_dir", opt), 52, 30, lit, opt.upper(), SM_CYAN)
        button_image("b_arp_load", 130, 32, "LOAD", lit, SM_GREEN, 14)
        button_image("b_arp_clear", 130, 32, "CLEAR PAGE", lit, RED, 13)
        arp_dot_image(lit)
        Canvas(AG_CW, AG_CH).save("arp_tap_" + ("on" if lit else "off"))   # 2.4.1: the cells' tap layer (clear)
        for key, lab in (("arp_latch", "LATCH"), ("arp_tie", "TIE"), ("arp_follow", "FOLLOW")):
            button_image("b_" + key, 76, 30, lab, lit, SM_CYAN, 13)
        arrow_image("pad_down", 38, 30, "left", lit, BLUE)
        arrow_image("pad_up", 38, 30, "right", lit, VIOLET)
        arrow_image("pg_prev", 70, 34, "up", lit, BLUE)
        arrow_image("pg_next", 70, 34, "down", lit, BLUE)
        arrow_image("row_up", 44, 212, "up", lit, BLUE)
        arrow_image("row_down", 44, 212, "down", lit, BLUE)
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
        button_image("b_reset", 160, 36, "RESET", lit, RED)
        button_image("b_clear", 160, 36, "CLEAR SLOT", lit)
        button_image("b_patch", 160, 36, "SAVE PATCH", lit)
        button_image("b_panic", 92, 36, "PANIC", lit, RED)
        for opt in ("1", "2", "3"):
            seg_image("info_page_" + opt, 58, 30, lit, "PG " + opt)
        for opt in ("Off", "On"):
            seg_image("br_preview_" + opt.lower(), 110, 34, lit, "PREVIEW " + opt.upper())
        for i in range(4):
            seg_image("slot_target_%d" % i, 50, 30, lit, "ABCD"[i], SLOT_COLOURS[i])
            seg_image("slot_load_%d" % i, 200, 30, lit, "LOADS HERE" if lit else "LOAD HERE", SLOT_COLOURS[i])
            for opt in ("Play", "Mute"):
                seg_image("slot%d_%s" % (i, opt.lower()), 90, 30, lit, opt.upper(), SLOT_COLOURS[i] if opt == "Play" else RED)
        for opt in ("Layer", "Keyswitch"):
            seg_image(opt_name("layer_mode", opt), 120, 32, lit, opt.upper())
        for opt in ("Stereo", "Ping-Pong"):
            seg_image(opt_name("dly_ping", opt), 120, 32, lit, opt.upper())
        for opt in TREM_MODES:   # 1.8.2: MOD FX
            seg_image(opt_name("trm_mode", opt), TR_SEG_W, 32, lit, opt.upper(), GREEN)
        for opt in TREM_SHAPES:
            seg_image(opt_name("trm_shape", opt), TR_SEG_W, 32, lit, opt.upper(), GREEN)
        for opt in ("Off", "On"):
            seg_image("set_" + opt_name("auto_loop", opt), OPT_W, OPT_H, lit, opt.upper())
        for key, label in (("drives", "DRIVES"), ("library", "PLUGIN LIBRARY"), ("up", "FOLDER UP"), ("refresh", "REFRESH")):
            button_image("b_" + key, 220, 44, label, lit)
        # 1.8.2: PLAY page 2
        for i in range(4):
            for opt in PLAY_MODES:
                seg_image(opt_name("slot%d_play" % (i + 1), opt), PM_W, SEG2_H, lit, opt.upper(), SLOT_COLOURS[i])
            for opt, lab in (("Normal", "NORM"), ("Invert", "INV")):
                seg_image(opt_name("slot%d_inv" % (i + 1), opt), POL_W, SEG2_H, lit, lab, SLOT_COLOURS[i] if opt == "Normal" else ORANGE)
        for opt, w_, lab in (("Off", 64, "OFF"), ("On", 112, "LIMITER")):   # 2.2
            seg_image(opt_name("limiter", opt), w_, 30, lit, lab, ORANGE)
        for opt in CHORUS_TYPES[1:]:   # 2.2
            seg_image(opt_name("cho_type", opt), RT_W, 30, lit, opt.upper(), GREEN)
        # 2.3: the SYNTH tab's keys
        seg_image(opt_name("syn_on", "Off"), 70, 30, lit, "OFF", RED)
        seg_image(opt_name("syn_on", "On"), 110, 30, lit, "SYNTH ON", GREEN)
        for x in ("a", "b"):
            for opt in ("On", "Off"):
                seg_image(opt_name("s%s_on" % x, opt), 44, 26, lit, opt.upper(), SYN_ACC[x])
            for opt in SYN_TYPES:
                seg_image(opt_name("s%s_type" % x, opt), 96, 26, lit, opt.upper(), SYN_ACC[x])
            arrow_image("syn_%s_prev" % x, 34, 28, "left", lit, SYN_ACC[x])
            arrow_image("syn_%s_next" % x, 34, 28, "right", lit, SYN_ACC[x])
            button_image("b_syn_%s_load" % x, 68, 28, "LOAD", lit, SYN_ACC[x], 13)
            button_image("b_syn_%s_folder" % x, 92, 28, "FOLDER", lit, SYN_ACC[x], 13)   # 2.3.2
            for key, lab in SYN_PICK:   # 2.4.9
                button_image("b_syn_%s_%s" % (x, key), SYN_PICK_W, 22, lab, lit, SYN_ACC[x], 11)
        # 2.3.1: the MIX / FILTER page's keys
        for key, colour in (("syn_sub_on", SM_GREEN), ("syn_noise_on", SM_VIOLET), ("syn_flt_on", SM_CYAN)):
            for opt in ("On", "Off"):
                seg_image(opt_name(key, opt), 44, 26, lit, opt.upper(), colour)
        for opt in SYN_SUB_SHAPES:
            seg_image(opt_name("syn_sub_shape", opt), 88, 28, lit, opt.upper(), SM_GREEN)
        for opt in ("-1 Oct", "-2 Oct"):
            seg_image(opt_name("syn_sub_oct", opt), 130, 28, lit, opt.upper(), SM_GREEN)
        for opt in SYN_NOISE_COLS:
            seg_image(opt_name("syn_noise_color", opt), 88, 28, lit, opt.upper(), SM_VIOLET)
        for opt, w_ in (("Free", 100), ("Follow Key", 160)):
            seg_image(opt_name("syn_noise_key", opt), w_, 28, lit, opt.upper(), SM_VIOLET)
        for opt, w_, lab in (("Off", 90, "SYNC OFF"), ("On", 130, "SYNC B TO A")):
            seg_image(opt_name("syn_sync", opt), w_, 28, lit, lab, SM_BLUE)
        for opt in SYN_FTYPES:
            seg_image(opt_name("syn_flt_type", opt), 84, 26, lit, opt.upper(), SM_CYAN)
        for k in ("a", "b", "sub", "noise"):
            for opt in ("Out", "In"):
                seg_image(opt_name("syn_fr_%s" % k, opt), 70, 26, lit, opt.upper(), SM_ORANGE)
        # 2.3.3: the steppers' arrows (shared)
        for h_ in (26, 34):
            arrow_image("stp%d_l" % h_, h_, h_, "left", lit, CYAN)
            arrow_image("stp%d_r" % h_, h_, h_, "right", lit, CYAN)
        arrow_image("stp34_u", 34, 34, "up", lit, CYAN)     # 2.4.9: the GRID's SHIFT up / down
        arrow_image("stp34_d", 34, 34, "down", lit, CYAN)
        # 2.3.3: the VOICE page's keys
        for opt in SYN_VMODES:
            seg_image(opt_name("syn_vmode", opt), 110, 32, lit, opt.upper(), SM_GREEN)
        for opt in ("Always", "Legato"):
            seg_image(opt_name("syn_glide_mode", opt), 100, 28, lit, opt.upper(), SM_GREEN)
        # 2.3.2: the ENV / LFO page's keys
        for opt in ("On", "Off"):
            seg_image(opt_name("syn_me_on", opt), 44, 26, lit, opt.upper(), SM_VIOLET)
        for e, _, _, col, _, _ in SE_ENVS2:   # 2.4.17: MOD ENV 2 / 3
            for opt in ("On", "Off"):
                seg_image(opt_name(SE_MOD_ENV_KEY[e] + "_on", opt), 44, 26, lit, opt.upper(), col)
        for n, _, col in SE_LFOS + SE_LFOS2:
            for opt in SYN_LFO_SHAPES:
                seg_image(opt_name("syn_lfo%d_shape" % n, opt), SYN_LFO_SW, 26, lit, opt.upper(), col)
            for opt in SYN_LFO_TRIGS:
                seg_image(opt_name("syn_lfo%d_trig" % n, opt), 80, 26, lit, opt.upper(), col)
            for opt, rc in zip(SYN_LFO_ROUTES[1:], (SM_ORANGE, SM_CYAN, SM_VIOLET)):   # 2.3.6: the envelopes' colours
                seg_image(opt_name("syn_lfo%d_route" % n, opt), 68, 26, lit, opt.upper(), rc)
                tint_key(opt_name("syn_lfo%d_route" % n, opt), lit, rc)
        for opt in DELAY_TYPES[1:]:   # 2.2
            seg_image(opt_name("dly_type", opt), RT_W, 30, lit, opt.upper(), BLUE)
        for opt in REV_TYPES[1:]:   # 2.2
            seg_image(opt_name("rev_type", opt), RT_W, 30, lit, opt.upper(), VIOLET)
        for opt, lab in (("Full", "FULL"), ("Gated", "GATED")):
            seg_image(opt_name("rev_mode", opt), RV_W, SEG2_H, lit, lab, CYAN if opt == "Full" else ORANGE)
        for opt in ["Slot"] + PLAY_MODES:
            seg_image(opt_name("lock_mode", opt), NL_SEG_W, SEG2_H, lit, opt.upper())
        for opt, lab in (("Slot", "SLOT"), ("Normal", "NORM"), ("Invert", "INV")):
            seg_image(opt_name("lock_inv", opt), NL_SEG_W, SEG2_H, lit, lab, ORANGE if opt == "Invert" else CYAN)
        for opt in ("Off", "On"):
            seg_image(opt_name("pad_follow", opt), 110, SEG2_H, lit, opt.upper())
        button_image("b_lockclear", 150, 36, "CLEAR LOCK", lit, ORANGE)
    clear_image()


# ---------------------------------------------------------------------------------------------------------------
# layout

def y(v):
    return v + Y_OFF


def knob_line(cx, cy, r, key, colour, width=92):
    # width: the component (and its value text) box; neighbours' boxes must not overlap, or one takes the other's touch
    return ('knob cx=%d cy=%d r=%d key=%s label="" name=hide strip=art/%s.png width=%d'
            % (cx, y(cy), r, key, KNOB_STRIPS[colour], width))


def browse_rack_lines():
    """1.8.13: the rack BROWSE's controls: the same parameters as the flat page, placed on the rack art"""
    import rack
    L = ['readout cx=1056 cy=%d w=200 h=34 label="" key=prog_format text_size=14' % y(28),
         'button cx=1218 cy=%d label="" key=panic img=art/rk_panic_off.png img_on=art/rk_panic_on.png w=92 h=36' % y(28)]
    x, yy, w, h = RK_SCREEN
    L.append('readout cx=%d cy=%d w=%d h=34 label="" key=br_loc text_size=17' % (x + w // 2, y(yy + 29), w - 48))
    # the data wheel scrolls the list's highlight while one of its fields has focus (the folder line or the DATA dial);
    # Enter opens the row. key=br_row: MPC sends the data wheel on this page to Q-Link 1's parameter (br_row)
    L.append('wheel cx=%d cy=%d w=%d h=34 key=br_row enter=br_enter' % (x + w // 2, y(yy + 29), w - 48))
    lw = w - 28 - 50
    L.append('list x=%d y=%d w=%d h=%d cols=1 rows=%d th=%d gap=%d key=br text_size=18'
             % (x + 14, y(RK_ROWS_Y), lw, BROWSER_ROWS * (RK_TH + RK_GAP) - RK_GAP, BROWSER_ROWS, RK_TH, RK_GAP))
    lh = BROWSER_ROWS * (RK_TH + RK_GAP) - RK_GAP
    bx = x + 14 + lw + 6 + 20
    L.append('button cx=%d cy=%d label="" key=br_rowup img=art/row_up_off.png img_on=art/row_up_on.png w=40 h=%d'
             % (bx, y(RK_ROWS_Y + lh // 4), lh // 2 - 4))
    L.append('button cx=%d cy=%d label="" key=br_rowdown img=art/row_down_off.png img_on=art/row_down_on.png w=40 h=%d'
             % (bx, y(RK_ROWS_Y + 3 * lh // 4), lh // 2 - 4))
    by = yy + h - 28
    L.append('button cx=%d cy=%d label="" key=br_prev img=art/pg_prev_off.png img_on=art/pg_prev_on.png w=70 h=28' % (x + 64, y(by)))
    L.append('readout cx=%d cy=%d w=160 h=28 label="" key=br_page text_size=16' % (x + w // 2, y(by)))
    L.append('button cx=%d cy=%d label="" key=br_next img=art/pg_next_off.png img_on=art/pg_next_on.png w=70 h=28' % (x + w - 64, y(by)))
    x, yy, w, h = RK_SIDE
    for i, (key, img) in enumerate((("drives", "rk_drives"), ("library", "rk_library"), ("up", "rk_up_dir"), ("refresh", "rk_refresh"))):
        L.append('button cx=%d cy=%d label="" key=br_%s img=art/%s_off.png img_on=art/%s_on.png w=214 h=38'
                 % (x + 130 + (i % 2) * 240, y(yy + 58 + (i // 2) * 44), key, img, img))
    L.append('button cx=%d cy=%d label="" key=br_setlib img=art/rk_setlib_off.png img_on=art/rk_setlib_on.png w=214 h=38' % (x + 130, y(yy + 146)))
    for i, (opt, img) in enumerate((("Off", "rk_prev_off"), ("On", "rk_prev_on"))):   # kit previews
        L.append('option cx=%d cy=%d w=104 h=32 key=br_preview option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                 % (x + 316 + i * 110, y(yy + 146), opt, img, img))
    L.append('readout cx=%d cy=%d w=%d h=22 label="" key=lib_path text_size=12' % (x + w // 2, y(yy + 177), w - 44))
    for i in range(3):   # the full name, wrapped over three lines
        L.append('readout cx=%d cy=%d w=%d h=18 label="" key=br_sel%d text_size=14' % (x + w // 2, y(yy + 221 + i * 18), w - 52, i + 1))
    cx, cy, r = RK_DIAL   # the DATA dial: a second focus field for the data wheel (tap, then turn; press = Enter)
    L.append('wheel cx=%d cy=%d w=%d h=%d key=br_row enter=br_enter' % (cx, y(cy), 2 * r + 6, 2 * r + 6))
    for key, img, _, kx, led, colour, _ in RK_KEYS:
        L.append('button cx=%d cy=%d label="" key=%s img=art/%s_off.png img_on=art/%s_on.png w=64 h=30' % (kx, y(RK_KEY_Y), key, img, img))
        L.append('meter cx=%d cy=%d w=14 h=14 key=%s strip=art/led_%s.png frames=%d' % (kx - 22, y(RK_LED_Y), led, colour, METER_FRAMES))
    for i in range(4):
        n = "rk_slot_%d" % i
        L.append('option cx=%d cy=%d w=50 h=28 key=target_slot option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                 % (RK_PROG[0] + RK_PROG[2] - 236 + i * 60, y(RK_SLOT_Y), "ABCD"[i], n, n))   # 2.1.16: with the screen
    x, yy, w, h = RK_PROG
    L.append('button cx=%d cy=%d label="" key=prog_prev img=art/prog_prev_off.png img_on=art/prog_prev_on.png w=38 h=28' % (x + 26, y(yy + 19)))
    L.append('readout cx=%d cy=%d w=340 h=30 label="" key=prog_name text_size=17' % (x + w // 2, y(yy + 19)))
    L.append('button cx=%d cy=%d label="" key=prog_next img=art/prog_next_off.png img_on=art/prog_next_on.png w=38 h=28' % (x + w - 26, y(yy + 19)))
    L.append('readout cx=%d cy=%d w=%d h=20 label="" key=prog_info text_size=13' % (x + w // 2, y(yy + 44), w - 24))
    L.append('readout cx=%d cy=%d w=%d h=20 label="" key=status text_size=13' % (x + w // 2, y(yy + 64), w - 24))
    sy = RK_STRIP_Y   # the I/O strip
    for cx_, key in ((96, "volume"), (256, "pan")):
        L.append('knob cx=%d cy=%d r=20 key=%s label="" name=hide value=hide strip=art/rk_pot.png' % (cx_, y(sy + 4), key))
    for i, (opt, img, ow, ox) in enumerate((("Off", "rk_ae_off", 92, 878), ("Disk Images", "rk_ae_disk", 112, 988))):
        L.append('option cx=%d cy=%d w=%d h=30 key=auto_extract option="%s" label="" img=art/%s_off.png img_on=art/%s_on.png'
                 % (ox, y(sy + 4), ow, opt, img, img))
    for opt, img, ox in (("Hide", "rk_sf_hide", 352), ("Show", "rk_sf_show", 432)):   # 2.1.3: SAMPLE FOLDERS
        L.append('option cx=%d cy=%d w=76 h=30 key=br_folders option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                 % (ox, y(sy + 4), opt, img, img))
    L.append('button cx=%d cy=%d label="" key=br_extract img=art/rk_extract_off.png img_on=art/rk_extract_on.png w=200 h=36' % (1154, y(sy + 4)))
    return L


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
         "qlinks_track = env_attack,env_decay,env_sustain,env_release,cutoff,resonance,filter_vel,vel_sens,transpose,bend,tune,glide,drive,rev_mix,volume,pan", ""]

    # PLAY
    L.append("[tab PLAY]")
    L.append("art file=art/bg_play.png x=0 y=%d w=1280 h=628 fit=stretch" % Y_OFF)
    header_lines(L)
    L.append('button cx=74 cy=%d label="" key=prog_prev img=art/prog_prev_off.png img_on=art/prog_prev_on.png w=38 h=30' % y(28))
    L.append('readout cx=222 cy=%d w=240 h=32 label="" key=prog_name text_size=16' % y(28))
    L.append('button cx=368 cy=%d label="" key=prog_next img=art/prog_next_off.png img_on=art/prog_next_on.png w=38 h=30' % y(28))
    # 1.8.2: two MPC sub-pages (the tab's dots; press PLAY again): PLAY, and PADS (playback modes, note locks). The
    # header and the pad row are on both; banks= keeps the rest to its page
    P1 = []
    for cx, cy, r, key, _, col in PLAY_KNOBS:
        P1.append(knob_line(cx, cy, r, key, col, 82 if cx < 340 else 92))
    for fx, key, _ in FADERS:
        P1.append('slider_v cx=%d cy=%d w=%d h=%d label="" key=%s name=hide strip=art/fader.png width=68'
                  % (fx, y(FADER_CY), FADER_W, FADER_H, key))
    for key, opts, x0 in SEG_GROUPS:
        for i, opt in enumerate(opts):
            n = opt_name(key, opt)
            P1.append('option cx=%d cy=%d w=%d h=%d key=%s option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                      % (x0 + SEG_W // 2 + i * SEG_W, y(SEG_Y), SEG_W, SEG_H, key, opt, n, n))
    dx, dy, dw, dh = DISPLAY
    for b in range(WAVE_BARS):
        P1.append('meter cx=%d cy=%d w=%d h=%d key=wave_%d strip=art/wave_bar.png frames=%d exact=1' % (WAVE_X0 + b * WAVE_STEP, y(WAVE_CY), WAVE_W, WAVE_H, b + 1, METER_FRAMES))
    for t in range(32):
        P1.append('meter cx=%d cy=%d w=%d h=16 key=ptile_%d strip=art/ptile.png frames=%d' % (round(PTILE_X0 + t * PTILE_STEP + PTILE_W / 2), y(PTILE_CY), PTILE_W, t + 1, METER_FRAMES))
    P1.append('readout cx=%d cy=%d w=%d h=34 label="" key=layer_name text_size=19' % (dx + dw // 2, y(dy + 26), dw - 40))
    P1.append('readout cx=%d cy=%d w=%d h=26 label="" key=prog_info text_size=14' % (dx + dw // 2, y(dy + 54), dw - 40))
    for key, x0, cw in CHIPS:
        P1.append('readout cx=%d cy=%d w=%d h=28 label="" key=%s text_size=14' % (x0 + cw // 2, y(dy + dh - 28), cw, key))
    P1.append('readout cx=492 cy=%d w=136 h=30 label="" key=status text_size=14' % y(500))
    P1.append('button cx=596 cy=%d label="" key=pad_down img=art/pad_down_off.png img_on=art/pad_down_on.png w=38 h=30' % y(500))
    P1.append('readout cx=672 cy=%d w=110 h=30 label="" key=pad_info text_size=13' % y(500))
    P1.append('button cx=748 cy=%d label="" key=pad_up img=art/pad_up_off.png img_on=art/pad_up_on.png w=38 h=30' % y(500))
    P1.append('readout cx=818 cy=%d w=72 h=30 label="" key=pad_vel text_size=15' % y(500))
    L += [l + ' banks="PLAY"' for l in P1]
    # page 2: its art over the body, PLAYBACK (left), NOTE LOCK (right)
    P2 = ['art file=art/bg_play2.png x=%d y=%d w=%d h=%d fit=stretch' % (P2_AREA[0], y(P2_AREA[1]), P2_AREA[2], P2_AREA[3])]
    for i in range(4):
        n, ry = i + 1, PB_ROW_Y0 + i * PB_ROW_STEP
        P2.append('readout cx=%d cy=%d w=520 h=28 label="" key=slot%d_name text_size=16' % (PM_X0 + 44 + 270, y(ry), n))
        for k, opt in enumerate(PLAY_MODES):
            m = opt_name("slot%d_play" % n, opt)
            P2.append('option cx=%d cy=%d w=%d h=%d key=slot%d_play option="%s" label="" img=art/%s_off.png img_on=art/%s_on.png'
                      % (PM_X0 + PM_W // 2 + k * PM_W, y(ry + 40), PM_W, SEG2_H, n, opt, m, m))
        for k, opt in enumerate(("Normal", "Invert")):
            m = opt_name("slot%d_inv" % n, opt)
            P2.append('option cx=%d cy=%d w=%d h=%d key=slot%d_inv option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                      % (POL_X0 + POL_W // 2 + k * POL_W, y(ry + 40), POL_W, SEG2_H, n, opt, m, m))
    for k, opt in enumerate(("Full", "Gated")):   # 2.1.16
        m = opt_name("rev_mode", opt)
        P2.append('option cx=%d cy=%d w=%d h=%d key=rev_mode option="%s" label="" img=art/%s_off.png img_on=art/%s_on.png'
                  % (RV_X0 + RV_W // 2 + k * RV_W, y(RV_Y), RV_W, SEG2_H, opt, m, m))
    nx, ny, nw, nh = NL_BOX
    P2.append('readout cx=%d cy=%d w=%d h=28 label="" key=lock_info text_size=14' % (nx + nw // 2, y(NL_INFO_Y), nw - 56))
    for cx, cy, key, _, col in NL_KNOBS:
        P2.append(knob_line(cx, cy, 26, key, col, 92))
    P2.append('button cx=926 cy=%d label="" key=pad_down img=art/pad_down_off.png img_on=art/pad_down_on.png w=38 h=30' % y(NL_KNOB_Y2))
    P2.append('readout cx=1010 cy=%d w=110 h=30 label="" key=pad_info text_size=13' % y(NL_KNOB_Y2))
    P2.append('button cx=1094 cy=%d label="" key=pad_up img=art/pad_up_off.png img_on=art/pad_up_on.png w=38 h=30' % y(NL_KNOB_Y2))
    P2.append('button cx=1192 cy=%d label="" key=lock_clear img=art/b_lockclear_off.png img_on=art/b_lockclear_on.png w=150 h=36' % y(NL_KNOB_Y2))
    for k, opt in enumerate(["Slot"] + PLAY_MODES):
        m = opt_name("lock_mode", opt)
        P2.append('option cx=%d cy=%d w=%d h=%d key=lock_mode option="%s" label="" img=art/%s_off.png img_on=art/%s_on.png'
                  % (NL_SEG_X0 + NL_SEG_W // 2 + k * NL_SEG_W, y(NL_MODE_Y), NL_SEG_W, SEG2_H, opt, m, m))
    for k, opt in enumerate(("Slot", "Normal", "Invert")):
        m = opt_name("lock_inv", opt)
        P2.append('option cx=%d cy=%d w=%d h=%d key=lock_inv option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                  % (NL_SEG_X0 + NL_SEG_W // 2 + k * NL_SEG_W, y(NL_INV_Y), NL_SEG_W, SEG2_H, opt, m, m))
    for k, opt in enumerate(("Off", "On")):
        m = opt_name("pad_follow", opt)
        P2.append('option cx=%d cy=%d w=110 h=%d key=pad_follow option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                  % (1075 + k * 110, y(NL_INV_Y), SEG2_H, opt, m, m))
    L += [l + ' banks="PADS"' for l in P2]
    # both pages: the pad row, with each pad's lock mark under it
    L.append('slider_v cx=42 cy=%d w=30 h=50 label="" key=pad_vel name=hide value=hide strip=art/fader_small.png' % y(593))
    for p in range(16):
        cx = PAD_X0 + PAD_STEP * p + PAD_W / 2
        L.append('button cx=%d cy=%d label="%02d" key=pad_%d img=art/pad%d_off.png img_on=art/pad%d_on.png w=%d h=%d'
                 % (cx, y(PADS_Y + 3), p + 1, p + 1, p + 1, p + 1, PAD_W + 12, PAD_H + 12))
    for p in range(16):
        cx = PAD_X0 + PAD_STEP * p + PAD_W / 2
        L.append('meter cx=%d cy=%d w=%d h=%d key=plock_%d strip=art/plock.png frames=%d'
                 % (cx, y(PADS_Y + 3 + PLOCK_DY), PLOCK_W, PLOCK_H, p + 1, METER_FRAMES))
    L.append('qlinks "PLAY" = env_attack,env_decay,env_sustain,env_release,cutoff,resonance,filter_vel,vel_sens,transpose,bend,tune,glide,drive,rev_mix,volume,pan')
    L.append('qlinks "PADS" = lock_note,lock_tune,lock_fine,lock_start,lock_cut,lock_level,lock_pan,lock_vel,'
             'slot1_play,slot2_play,slot3_play,slot4_play,lock_mode,lock_inv,pad_vel,pad_base')
    L.append("")

    # LAYERS
    # 2.3: SYNTH, second (MPC shows five tabs before its ">" pager). Its OSC page; Q-Links: OSC A in columns 1-2, OSC B
    # in 3-4 (position, detune, spread, warp; level, pan, unison, semi)
    L.append("[tab SYNTH]")
    L.append("art file=art/bg_synth.png x=0 y=%d w=1280 h=628 fit=stretch" % Y_OFF)
    S1 = []
    for k, (opt, w_, cx) in enumerate((("Off", 70, 990), ("On", 110, 1092))):
        m = opt_name("syn_on", opt)
        S1.append('option cx=%d cy=%d w=%d h=30 key=syn_on option=%s label="" img=art/%s_off.png img_on=art/%s_on.png' % (cx, y(28), w_, opt, m, m))
    S1 += stepper_lines("syn_preset", 286, 28, 292, 34, 15)   # 2.3.5: the factory presets (every sub-page)
    S1.append('button cx=1218 cy=%d label="" key=panic img=art/b_panic_off.png img_on=art/b_panic_on.png w=92 h=36' % y(28))
    for x in ("a", "b"):
        x0 = SYN_X0[x]
        for k, opt in enumerate(("On", "Off")):
            m = opt_name("s%s_on" % x, opt)
            S1.append('option cx=%d cy=%d w=44 h=26 key=s%s_on option=%s label="" img=art/%s_off.png img_on=art/%s_on.png' % (x0 + 38 + k * 46, y(66 + 25), x, opt, m, m))
        for k, opt in enumerate(SYN_TYPES):
            m = opt_name("s%s_type" % x, opt)
            S1.append('option cx=%d cy=%d w=96 h=26 key=s%s_type option=%s label="" img=art/%s_off.png img_on=art/%s_on.png' % (x0 + 258 + k * 98, y(66 + 25), x, opt, m, m))
        S1.append('button cx=%d cy=%d label="" key=s%s_prev img=art/syn_%s_prev_off.png img_on=art/syn_%s_prev_on.png w=34 h=28' % (x0 + 40, y(66 + 69), x, x, x))
        # 2.3.2: < name > FOLDER LOAD
        S1.append('readout cx=%d cy=%d w=310 h=28 label="" key=s%s_name text_size=15' % (x0 + 228, y(66 + 69), x))
        S1.append('button cx=%d cy=%d label="" key=s%s_next img=art/syn_%s_next_off.png img_on=art/syn_%s_next_on.png w=34 h=28' % (x0 + 404, y(66 + 69), x, x, x))
        S1.append('button cx=%d cy=%d label="" key=s%s_folder img=art/b_syn_%s_folder_off.png img_on=art/b_syn_%s_folder_on.png w=92 h=28' % (x0 + 476, y(66 + 69), x, x, x))
        S1.append('button cx=%d cy=%d label="" key=s%s_load img=art/b_syn_%s_load_off.png img_on=art/b_syn_%s_load_on.png w=68 h=28' % (x0 + 562, y(66 + 69), x, x, x))
        S1.append('readout cx=%d cy=%d w=300 h=20 label="" key=s%s_info text_size=12' % (x0 + 180, y(66 + 108), x))
        for i, (key, _) in enumerate(SYN_PICK):   # 2.4.9: top right of the screen, beside the info line
            S1.append('button cx=%d cy=%d label="" key=s%s_%s img=art/b_syn_%s_%s_off.png img_on=art/b_syn_%s_%s_on.png w=%d h=22'
                      % (x0 + SYN_PICK_X + i * (SYN_PICK_W + 4), y(66 + 109), x, key, x, key, x, key, SYN_PICK_W))
        for b in range(SYN_BARS):
            S1.append('meter cx=%d cy=%d w=12 h=96 key=s%s_w%d strip=art/syn_bar.png frames=%d' % (x0 + 44 + b * 17.3, y(66 + 174), x, b + 1, METER_FRAMES))
        S1.append('popup cx=%d cy=%d w=150 h=24 label="" key=s%s_pmode text_size=14' % (x0 + 170, y(66 + 272), x))
        S1.append('popup cx=%d cy=%d w=170 h=24 label="" key=s%s_wmode text_size=14' % (x0 + 430, y(66 + 272), x))
        col = CYAN if x == "a" else ORANGE
        for row, keys in enumerate((SYN_ROW1, SYN_ROW2)):
            for i, k in enumerate(keys):
                S1.append(knob_line(x0 + 58 + i * 100, 66 + 350 + row * 118, 26, "s%s_%s" % (x, k), col, 96))
    # the header (SYNTH ON / OFF, PANIC) on every sub-page; the rest on its own
    HDR = ("key=syn_on", "key=panic", "key=syn_preset")
    L += [l for l in S1 if any(h in l for h in HDR)]
    L += [l + ' banks="OSC"' for l in S1 if not any(h in l for h in HDR)]
    # 2.3.1: MIX / FILTER, over the OSC page's art
    S2 = ['art file=art/bg_synth2.png x=%d y=%d w=%d h=%d fit=stretch' % (SM_AREA[0], y(SM_AREA[1]), SM_AREA[2], SM_AREA[3])]
    def pair(key, opts, cxs, cy, ws, h=26):
        for opt, cx, w_ in zip(opts, cxs, ws):
            m = opt_name(key, opt)
            S2.append('option cx=%d cy=%d w=%d h=%d key=%s option="%s" label="" img=art/%s_off.png img_on=art/%s_on.png' % (cx, y(cy), w_, h, key, opt, m, m))
    x, yy = SM_SUB[0], SM_SUB[1]
    pair("syn_sub_on", ("On", "Off"), (x + 30, x + 76), yy + 22, (44, 44))
    pair("syn_sub_shape", SYN_SUB_SHAPES, [x + 56 + i * 94 for i in range(3)], yy + 66, (88,) * 3, 28)
    pair("syn_sub_oct", ("-1 Oct", "-2 Oct"), (x + 80, x + 220), yy + 104, (130, 130), 28)
    S2.append(knob_line(x + 90, yy + 204, 26, "syn_sub_level", GREEN, 110))
    S2.append(knob_line(x + 210, yy + 204, 26, "syn_sub_pan", GREEN, 110))
    x, yy = SM_NOISE[0], SM_NOISE[1]
    pair("syn_noise_on", ("On", "Off"), (x + 30, x + 76), yy + 22, (44, 44))
    pair("syn_noise_color", SYN_NOISE_COLS, [x + 56 + i * 94 for i in range(3)], yy + 66, (88,) * 3, 28)
    pair("syn_noise_key", ("Free", "Follow Key"), (x + 70, x + 206), yy + 104, (100, 160), 28)
    for i, k in enumerate(("syn_noise_level", "syn_noise_pan", "syn_noise_tone")):
        S2.append(knob_line(x + 60 + i * 90, yy + 204, 26, k, VIOLET, 86))
    x, yy = SM_OMOD[0], SM_OMOD[1]
    pair("syn_sync", ("Off", "On"), (x + 446, x + 562), yy + 22, (90, 130), 28)
    for i, k in enumerate(("syn_fm", "syn_feedback", "syn_ring", "syn_balance")):
        S2.append(knob_line(x + 90 + i * 145, yy + 180, 28, k, BLUE, 130))
    x, yy = SM_FLT[0], SM_FLT[1]
    pair("syn_flt_on", ("On", "Off"), (x + 30, x + 76), yy + 22, (44, 44))
    pair("syn_flt_type", SYN_FTYPES, [x + 300 + i * 88 for i in range(7)], yy + 22, (84,) * 7)
    sx, sy, sw, sh = SM_SCREEN
    for t in range(len(SYN_FTYPES)):
        S2.append('art file=art/syn_resp_%d.png x=%d y=%d w=%d h=%d fit=stretch when=syn_flt_type:%d' % (t, sx + 8, y(sy + 8), sw - 16, sh - 36, t))
    # (no cutoff marker: a 348-wide meter decodes to 62 MB once the skin tools pad its frames to squares)
    for r, row in enumerate((("syn_cutoff", "syn_res", "syn_drive", "syn_keytrack"), ("syn_fenv", "syn_fvel", "syn_fat", "syn_fmix"))):
        for i, k in enumerate(row):
            S2.append(knob_line(x + 470 + i * 118, yy + 78 + r * 96, 22, k, CYAN, 110))
    x, yy = SM_ROUTE[0], SM_ROUTE[1]
    for i, k in enumerate(("a", "b", "sub", "noise")):
        pair("syn_fr_%s" % k, ("Out", "In"), (x + 190, x + 264), yy + 70 + i * 32, (70, 70))
    S2.append(knob_line(x + 80, yy + 214, 22, "syn_level", CYAN, 120))
    S2.append(knob_line(x + 230, yy + 214, 22, "syn_pan", CYAN, 120))
    L += [l + ' banks="MIX"' for l in S2]
    # 2.3.2: ENV / LFO
    def mod_page(art, envs, lfos, macros):   # 2.4.17: the ENV page, and MOD 2 (MOD ENV 2 / 3, LFO 3 / 4)
        S3 = ['art file=art/%s x=%d y=%d w=%d h=%d fit=stretch' % (art, SM_AREA[0], y(SM_AREA[1]), SM_AREA[2], SM_AREA[3])]
        def opt3(key, opt, cx, cy, w_, h=26):
            m = opt_name(key, opt)
            S3.append('option cx=%d cy=%d w=%d h=%d key=%s option="%s" label="" img=art/%s_off.png img_on=art/%s_on.png' % (cx, y(cy), w_, h, key, opt, m, m))
        knob_col = {"a": ORANGE, "f": CYAN, "m": VIOLET, "n": VIOLET, "o": BLUE}
        for e, title, (x, yy, w, h), col, keys, last in envs:
            if e in SE_MOD_ENV_KEY:
                mk = SE_MOD_ENV_KEY[e]
                opt3(mk + "_on", "On", x + 30, yy + 22, 44)
                opt3(mk + "_on", "Off", x + 76, yy + 22, 44)
                S3 += stepper_lines(mk + "_dest", x + 307, yy + 22, 186, 26, 14)
            for b in range(ENV_BARS):
                S3.append('meter cx=%d cy=%d w=11 h=72 key=syn_%se_b%d strip=art/syn_env.png frames=%d' % (x + 31 + b * 15, y(yy + 112), e, b + 1, METER_FRAMES))
            for i, k in enumerate(keys):
                S3.append(knob_line(x + 44 + i * 80, yy + 224, 26, k, knob_col[e], 76))
        for n, (x, yy, w, h), col in lfos:
            for ri, (opt, rk) in enumerate(zip(SYN_LFO_ROUTES[1:], SYN_LFO_RKEYS)):   # 2.3.6: AMP / FILTER / MOD keys (no arrows)
                # each a Toggle on its own key param: a tap on the lit one turns the route Off (plugin.cpp lfo_route_tap)
                m = opt_name("syn_lfo%d_route" % n, opt)
                S3.append('button cx=%d cy=%d label="" key=syn_lfo%d_rk_%s img=art/%s_off.png img_on=art/%s_on.png w=68 h=26'
                          % (x + 216 + ri * 72, y(yy + 22), n, rk, m, m))
            for si in range(len(SYN_LFO_SHAPES)):
                S3.append('art file=art/syn_lfo%d_%d.png x=%d y=%d w=164 h=76 fit=stretch when=syn_lfo%d_shape:%d' % (n, si, x + 24, y(yy + 54), n, si))
            lc = {1: GREEN, 2: BLUE, 3: CYAN, 4: ORANGE}[n]
            S3.append(knob_line(x + 252, yy + 92, 26, "syn_lfo%d_rate" % n, lc, 96))
            S3.append(knob_line(x + 352, yy + 92, 26, "syn_lfo%d_depth" % n, lc, 96))
            for si, opt in enumerate(SYN_LFO_SHAPES):
                opt3("syn_lfo%d_shape" % n, opt, x + 36 + si * SYN_LFO_SP, yy + 160, SYN_LFO_SW)
            # 2.4.13: CUSTOM: the shape's name and < > through the user's .vitallfo files, over the lower part of its picture
            S3 += [l + " when=syn_lfo%d_shape:Custom" % n for l in stepper_lines("syn_lfo%d_file" % n, x + 106, yy + 115, 164, 26, 12)]
            S3.append('popup cx=%d cy=%d w=140 h=26 label="" key=syn_lfo%d_sync text_size=14' % (x + 86, y(yy + 200), n))   # 2.3.6: a drop-down
            for ti, opt in enumerate(SYN_LFO_TRIGS):
                opt3("syn_lfo%d_trig" % n, opt, x + 200 + ti * 82, yy + 200, 80)
        if not macros:
            return S3
        x, yy, w, h = SE_MAC
        for i in range(4):
            S3.append(knob_line(x + 58 + i * 97, yy + 100, 26, "syn_macro%d" % (i + 1), ORANGE, 92))
            S3.append('popup cx=%d cy=%d w=92 h=26 label="" key=syn_macro%d_dest text_size=12' % (x + 58 + i * 97, y(yy + 176), i + 1))
        return S3
    S3 = mod_page("bg_synth3.png", SE_ENVS, SE_LFOS, True)
    L += [l + ' banks="ENV"' for l in S3]
    L += [l + ' banks="MOD 2"' for l in mod_page("bg_synth6.png", SE_ENVS2, SE_LFOS2, False)]   # 2.4.17
    # 2.3.3: MATRIX
    for first, art, bank in ((1, "bg_synth4.png", "MATRIX"), (9, "bg_synth7.png", "MATRIX 2")):   # 2.4.17: + MATRIX 2
     S4 = ['art file=art/%s x=%d y=%d w=%d h=%d fit=stretch' % (art, SM_AREA[0], y(SM_AREA[1]), SM_AREA[2], SM_AREA[3])]
     for half, x in enumerate((16, 648)):
        for i in range(4):
            n, ry = first + half * 4 + i, 66 + 112 + i * 124
            # 2.3.3: drop-downs (the user's choice, ~25 MB of list images): number 16..44, SOURCE 63..273,
            # DESTINATION 293..503, AMOUNT 515..605 (the lists open where they fit: below, above, or in columns)
            S4.append('popup cx=%d cy=%d w=210 h=34 label="" key=syn_mx%d_src text_size=15' % (x + MX_SRC_X, y(ry), n))
            S4.append('popup cx=%d cy=%d w=210 h=34 label="" key=syn_mx%d_dst text_size=15' % (x + MX_DST_X, y(ry), n))
            S4.append(knob_line(x + MX_AMT_X, ry - 6, 26, "syn_mx%d_amt" % n, VIOLET if half == 0 else BLUE, 90))
     L += [l + ' banks="%s"' % bank for l in S4]
    # 2.3.3: VOICE
    S5 = ['art file=art/bg_synth5.png x=%d y=%d w=%d h=%d fit=stretch' % (SM_AREA[0], y(SM_AREA[1]), SM_AREA[2], SM_AREA[3])]
    x, yy = SV_VOICE[0], SV_VOICE[1]
    for k, opt in enumerate(SYN_VMODES):
        m = opt_name("syn_vmode", opt)
        S5.append('option cx=%d cy=%d w=110 h=32 key=syn_vmode option=%s label="" img=art/%s_off.png img_on=art/%s_on.png' % (x + 190 + k * 116, y(yy + 66), opt, m, m))
    S5.append(knob_line(x + 110, yy + 176, 26, "syn_poly", GREEN, 130))
    S5.append(knob_line(x + 270, yy + 176, 26, "syn_glide", GREEN, 130))
    for k, opt in enumerate(("Always", "Legato")):
        m = opt_name("syn_glide_mode", opt)
        S5.append('option cx=%d cy=%d w=100 h=28 key=syn_glide_mode option=%s label="" img=art/%s_off.png img_on=art/%s_on.png' % (x + 420 + k * 104, y(yy + 168), opt, m, m))
    x, yy = SV_PITCH[0], SV_PITCH[1]
    for i, k in enumerate(("syn_bend", "syn_transpose", "syn_drift", "syn_vel")):
        S5.append(knob_line(x + 90 + i * 145, yy + 150, 30, k, CYAN, 130))
    # (no range markers over the keyboard: the skin tools pad a meter's frames to squares, and a 900-wide marker
    # would decode to 415 MB; the LOW / HIGH KEY knobs show note names)
    x, yy = SV_RANGE[0], SV_RANGE[1]
    S5.append(knob_line(x + 1020, yy + 100, 26, "syn_key_lo", GREEN, 104))
    S5.append(knob_line(x + 1130, yy + 100, 26, "syn_key_hi", ORANGE, 104))
    S5.append(knob_line(x + 1020, yy + 212, 24, "syn_level", ORANGE, 104))
    S5.append(knob_line(x + 1130, yy + 212, 24, "syn_pan", ORANGE, 104))
    # 2.4.12 MUTATE / BREED: one row over the keyboard (keys 470..560; the row 423..459)
    for k, cx in zip([k for k, _ in MUT_KEYS], MUT_X):
        w = 84 if k == "mut_undo" else 112
        S5.append('button cx=%d cy=%d label="" key=%s img=art/b_%s_off.png img_on=art/b_%s_on.png w=%d h=36' % (cx, y(MUT_Y), k, k, k, w))
    for k, opt in enumerate(MUT_AMOUNTS):
        m = opt_name("mut_amount", opt)
        S5.append('option cx=%d cy=%d w=80 h=30 key=mut_amount option=%s label="" img=art/%s_off.png img_on=art/%s_on.png' % (MUT_AMT_X + k * 84, y(MUT_Y), opt, m, m))
    L += [l + ' banks="VOICE"' for l in S5]
    L.append('qlinks "OSC" = sa_pos,sa_detune,sa_spread,sa_warp,sa_level,sa_pan,sa_unison,sa_semi,'
             'sb_pos,sb_detune,sb_spread,sb_warp,sb_level,sb_pan,sb_unison,sb_semi')
    L.append('qlinks "MIX" = syn_sub_level,syn_noise_level,syn_noise_tone,syn_fm,syn_feedback,syn_ring,syn_balance,syn_level,'
             'syn_cutoff,syn_res,syn_drive,syn_keytrack,syn_fenv,syn_fvel,syn_fat,syn_fmix')
    L.append('qlinks "ENV" = syn_attack,syn_decay,syn_sustain,syn_release,syn_fe_attack,syn_fe_decay,syn_fe_sustain,syn_fe_release,'
             'syn_lfo1_rate,syn_lfo1_depth,syn_lfo2_rate,syn_lfo2_depth,syn_macro1,syn_macro2,syn_macro3,syn_macro4')
    # 2.3.3: the sub-pages go in this order (OSC, MIX, ENV, MATRIX, VOICE)
    L.append('qlinks "MOD 2" = syn_me2_attack,syn_me2_decay,syn_me2_sustain,syn_me2_release,syn_me3_attack,syn_me3_decay,syn_me3_sustain,syn_me3_release,'
             'syn_lfo3_rate,syn_lfo3_depth,syn_lfo4_rate,syn_lfo4_depth,syn_me2_amt,syn_me3_amt,syn_macro1,syn_macro2')   # 2.4.17
    L.append('qlinks "MATRIX" = syn_mx1_amt,syn_mx2_amt,syn_mx3_amt,syn_mx4_amt,syn_mx5_amt,syn_mx6_amt,syn_mx7_amt,syn_mx8_amt,'
             'syn_macro1,syn_macro2,syn_macro3,syn_macro4,syn_lfo1_rate,syn_lfo1_depth,syn_lfo2_rate,syn_lfo2_depth')
    L.append('qlinks "MATRIX 2" = syn_mx9_amt,syn_mx10_amt,syn_mx11_amt,syn_mx12_amt,syn_mx13_amt,syn_mx14_amt,syn_mx15_amt,syn_mx16_amt,'
             'syn_macro1,syn_macro2,syn_macro3,syn_macro4,syn_lfo3_rate,syn_lfo3_depth,syn_lfo4_rate,syn_lfo4_depth')   # 2.4.17
    L.append('qlinks "VOICE" = syn_poly,syn_glide,syn_bend,syn_transpose,syn_drift,syn_vel,syn_key_lo,syn_key_hi,'
             'syn_level,syn_pan,syn_macro1,syn_macro2,syn_macro3,syn_macro4,syn_lfo1_rate,syn_lfo2_rate')
    L.append("")

    # 2.4: ARP, third (after SYNTH): SEQUENCE beside the GRID
    L.append("[tab ARP]")
    L.append("art file=art/bg_arp.png x=0 y=%d w=1280 h=628 fit=stretch" % Y_OFF)
    for opt, w_, cx in (("Off", 70, 990), ("On", 110, 1092)):
        m = opt_name("arp_on", opt)
        L.append('option cx=%d cy=%d w=%d h=30 key=arp_on option=%s label="" img=art/%s_off.png img_on=art/%s_on.png' % (cx, y(28), w_, opt, m, m))
    L.append('button cx=1218 cy=%d label="" key=panic img=art/b_panic_off.png img_on=art/b_panic_on.png w=92 h=36' % y(28))
    grid_from = len(L)   # 2.4.2: the rest is the GRID sub-page (banks=); GEN follows
    x0 = AA_L[0]
    for k, opt in enumerate(ARP_MODES):
        m = opt_name("arp_mode", opt)
        L.append('option cx=%d cy=%d w=130 h=32 key=arp_mode option=%s label="" img=art/%s_off.png img_on=art/%s_on.png' % (x0 + 80 + k * 140, y(66 + 56), opt, m, m))
    L.append('button cx=%d cy=%d label="" key=arp_prev img=art/stp34_l_off.png img_on=art/stp34_l_on.png w=34 h=34' % (x0 + 33, y(180)))
    L.append('readout cx=%d cy=%d w=200 h=34 label="" key=arp_name text_size=14' % (x0 + 150, y(180)))
    L.append('button cx=%d cy=%d label="" key=arp_next img=art/stp34_r_off.png img_on=art/stp34_r_on.png w=34 h=34' % (x0 + 267, y(180)))
    for k, (key, img) in enumerate((("arp_load", "b_arp_load"), ("arp_clear", "b_arp_clear"))):
        L.append('button cx=%d cy=%d label="" key=%s img=art/%s_off.png img_on=art/%s_on.png w=130 h=32' % (x0 + 80 + k * 140, y(222), key, img, img))
    for row_y, (lk, rk) in ((288, ("arp_scale", "arp_root")), (344, ("arp_order", "arp_oct")), (400, ("arp_rate", "arp_snap"))):
        L.append('popup cx=%d cy=%d w=170 h=30 label="" key=%s text_size=15' % (x0 + 95, y(row_y), lk))
        L.append('popup cx=%d cy=%d w=98 h=30 label="" key=%s text_size=15' % (x0 + 238, y(row_y), rk))
    # 2.4.2: the pattern's knobs, or (a note selected: arp_sel On) that note's, in the same places; labels follow
    for i, (gk, _, nk, _) in enumerate(ARP_KNOB_SLOTS):
        cx, ly = x0 + 50 + (i % 3) * 100, 426 + (i // 3) * 90
        L.append('meter cx=%d cy=%d w=%d h=%d key=arp_sel strip=art/arp_lbl%d.png frames=%d exact=1' % (cx, y(ly), LBL_W, LBL_H, i, METER_FRAMES))
        if gk:
            L.append(knob_line(cx, ly + 36, 22, gk, GREEN, 96) + " when=arp_sel:Off")
        if nk == "arp_n_slide":
            for k, opt in enumerate(("Off", "On")):
                m = opt_name(nk, opt)
                L.append('option cx=%d cy=%d w=44 h=30 key=%s option=%s label="" img=art/%s_off.png img_on=art/%s_on.png when=arp_sel:On'
                         % (cx - 23 + k * 46, y(ly + 36), nk, opt, m, m))
        else:
            L.append(knob_line(cx, ly + 36, 22, nk, ORANGE, 96) + " when=arp_sel:On")
    gx = AA_R[0]
    for k, opt in enumerate(ARP_PAGES):
        m = opt_name("arp_page", opt)
        L.append('option cx=%d cy=%d w=72 h=30 key=arp_page option=%s label="" img=art/%s_off.png img_on=art/%s_on.png' % (gx + 180 + k * 80, y(66 + 22), opt, m, m))
    L.append('readout cx=%d cy=%d w=420 h=24 label="" key=arp_info text_size=13' % (gx + 690, y(66 + 22)))
    # 2.4.1: TOUCH (Tap: a dot per tap; Draw: drag along a row for a held note, drag a note to move it, its end to
    # shorten or extend it, tap a note to delete it), SHIFT the pattern a step, STRUM direction, page FOLLOW, the status
    for k, opt in enumerate(ARP_DRAWS):
        m = opt_name("arp_draw", opt)
        L.append('option cx=%d cy=%d w=62 h=30 key=arp_draw option=%s label="" img=art/%s_off.png img_on=art/%s_on.png' % (gx + 96 + k * 66, y(128), opt, m, m))
    # 2.4.9: SHIFT < > (a step earlier / later) and ^ v (a row up / down: the pitch); STRUM and the keys after it moved
    # right by ARP_HDR_DX to make room
    for k, (key, img) in enumerate((("arp_shift_l", "stp34_l"), ("arp_shift_r", "stp34_r"), ("arp_shift_u", "stp34_u"), ("arp_shift_d", "stp34_d"))):
        L.append('button cx=%d cy=%d label="" key=%s img=art/%s_off.png img_on=art/%s_on.png w=34 h=34' % (gx + 252 + k * 36, y(128), key, img, img))
    for k, opt in enumerate(ARP_STRUM_DIRS):
        m = opt_name("arp_strum_dir", opt)
        L.append('option cx=%d cy=%d w=52 h=30 key=arp_strum_dir option=%s label="" img=art/%s_off.png img_on=art/%s_on.png' % (gx + 400 + ARP_HDR_DX + k * 56, y(128), opt, m, m))
    for k, key in enumerate(("arp_latch", "arp_tie", "arp_follow")):
        L.append('button cx=%d cy=%d label="" key=%s img=art/b_%s_off.png img_on=art/b_%s_on.png w=76 h=30' % (gx + 600 + ARP_HDR_DX + k * 84, y(128), key, key, key))
    for c_ in range(1, ARP_COLS + 1):
        L.append('meter cx=%d cy=%d w=30 h=8 key=arp_led%d strip=art/arp_led.png frames=%d' % (AG_X0 + (c_ - 1) * AG_DX, y(AG_LED_Y), c_, METER_FRAMES))
    for r in range(1, ARP_ROWS + 1):
        yr = AG_Y1 - (r - 1) * AG_DY
        L.append('readout cx=%d cy=%d w=60 h=%d label="" key=arp_row%d text_size=12' % (gx + 42, y(yr), AG_DY - 3, r))
        for c_ in range(1, ARP_COLS + 1):
            L.append('meter cx=%d cy=%d w=%d h=%d key=arp_c%d_%d strip=art/arp_cell.png frames=%d exact=1'
                     % (AG_X0 + (c_ - 1) * AG_DX, y(yr), AG_DX, AG_CH, r, c_, METER_FRAMES))
            L.append('button cx=%d cy=%d label="" key=arp_c%d_%d img=art/arp_tap_off.png img_on=art/arp_tap_on.png w=%d h=%d'
                     % (AG_X0 + (c_ - 1) * AG_DX, y(yr), r, c_, AG_CW, AG_CH))
    L[grid_from:] = [l + ' banks="GRID"' for l in L[grid_from:]]
    # 2.4.2 GEN: MELODY (the generator, motif tools) and GROOVE (direction, swing / humanize, Euclid, chain)
    G = ['art file=art/bg_arp2.png x=%d y=%d w=%d h=%d fit=stretch' % (SM_AREA[0], y(SM_AREA[1]), SM_AREA[2], SM_AREA[3])]
    gx = AG2_L[0]
    G.append('popup cx=%d cy=%d w=240 h=32 label="" key=arp_g_contour text_size=15' % (gx + 136, y(146)))
    G.append('button cx=%d cy=%d label="" key=arp_g_generate img=art/b_arp_g_generate_off.png img_on=art/b_arp_g_generate_on.png w=220 h=46' % (gx + 470, y(140)))
    for i, k in enumerate(("arp_g_density", "arp_g_range", "arp_g_sync", "arp_g_mutate")):
        G.append(knob_line(gx + 80 + i * 150, 252, 26, k, GREEN, 140))
    for i, (k, _) in enumerate(ARP_GEN_KEYS):
        G.append('button cx=%d cy=%d label="" key=%s img=art/b_%s_off.png img_on=art/b_%s_on.png w=130 h=40'
                 % (gx + 85 + (i % 4) * 148, y(374 + (i // 4) * 56), k, k, k))
    G.append('button cx=%d cy=%d label="" key=arp_v_go img=art/b_arp_v_go_off.png img_on=art/b_arp_v_go_on.png w=130 h=40'
             % (gx + 85 + 3 * 148, y(374 + 56)))   # 2.4.14 VOICE: the 8th MOTIF slot
    G.append('popup cx=%d cy=%d w=190 h=28 label="" key=arp_v_mode text_size=14' % (gx + 300, y(334)))
    for i, k in enumerate(("arp_g_artic", "arp_g_expr", "arp_g_ghost")):   # 2.4.11
        G.append(knob_line(gx + 80 + i * 150, 516, 26, k, GREEN, 140))
    G.append('readout cx=%d cy=%d w=580 h=24 label="" key=arp_info text_size=13' % (gx + 308, y(596)))
    rx = AG2_R[0]
    for k, opt in enumerate(ARP_DIRS):
        m = opt_name("arp_dir", opt)
        G.append('option cx=%d cy=%d w=130 h=32 key=arp_dir option=%s label="" img=art/%s_off.png img_on=art/%s_on.png' % (rx + 80 + k * 142, y(146), opt, m, m))
    for i, k in enumerate(("arp_swing", "arp_h_time", "arp_h_vel")):
        G.append(knob_line(rx + 80 + i * 150, 252, 26, k, CYAN, 140))
    for i, k in enumerate(("arp_e_hits", "arp_e_rotate")):
        G.append(knob_line(rx + 80 + i * 150, 392, 26, k, CYAN, 140))
    G.append('button cx=%d cy=%d label="" key=arp_e_apply img=art/b_arp_e_apply_off.png img_on=art/b_arp_e_apply_on.png w=200 h=40' % (rx + 470, y(392)))
    for c_ in range(4):
        G.append('popup cx=%d cy=%d w=120 h=32 label="" key=arp_chain%d text_size=15' % (rx + 80 + c_ * 142, y(492), c_ + 1))
    for c_, (k, _) in enumerate(ARP_BREED_KEYS):   # 2.4.12 BREED: the pattern from A's and B's bars
        G.append('button cx=%d cy=%d label="" key=%s img=art/b_%s_off.png img_on=art/b_%s_on.png w=130 h=36' % (rx + 80 + c_ * 150, y(578), k, k, k))
    L += [l + ' banks="GEN"' for l in G]
    L.append('qlinks "GRID" = arp_rate,arp_length,arp_swing,arp_gate,arp_scale,arp_root,arp_mode,arp_latch,'
             'arp_page,arp_on,arp_order,arp_oct,arp_strum,arp_snap,arp_octave,arp_note_gate')
    L.append('qlinks "GEN" = arp_g_density,arp_g_range,arp_g_sync,arp_g_mutate,arp_swing,arp_h_time,arp_h_vel,arp_e_hits,'
             'arp_e_rotate,arp_g_contour,arp_dir,arp_length,arp_rate,arp_scale,arp_root,arp_on')
    # (2.4.2: Q-Links follow the page: the note knobs are reached by touch)
    L.append("")
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
        L.append(knob_line(x + 56, SLOT_Y + 148, 26, "slot%d_lo" % n, col, 88))
        L.append(knob_line(x + 150, SLOT_Y + 148, 26, "slot%d_hi" % n, col, 88))
        L.append(knob_line(x + 244, SLOT_Y + 148, 26, "slot%d_shift" % n, col, 88))
        L.append(knob_line(x + 56, SLOT_Y + 248, 26, "slot%d_vol" % n, col, 88))
        L.append(knob_line(x + 150, SLOT_Y + 248, 26, "slot%d_pan" % n, col, 88))
        L.append(knob_line(x + 244, SLOT_Y + 248, 26, "slot%d_tune" % n, col, 88))
        for k, opt in enumerate(("Play", "Mute")):
            m = "slot%d_%s" % (i, opt.lower())
            L.append('option cx=%d cy=%d w=90 h=30 key=slot%d_mute option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                     % (x + SLOT_W // 2 - 48 + k * 96, y(SLOT_Y + 312), n, opt, m, m))
    for t in range(32):
        L.append('meter cx=%d cy=%d w=%d h=%d key=ltile_%d strip=art/ltile.png frames=%d' % (LT_X0 + t * LT_STEP + LT_W // 2, y(LT_CY), LT_W, LT_H, t + 1, METER_FRAMES))
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
    # 1.8: a slot's bank is low key, high key, volume, pan (transpose stays on its touch knob)
    L.append('qlinks "LAYERS" = slot1_lo,slot1_hi,slot1_vol,slot1_pan,slot2_lo,slot2_hi,slot2_vol,slot2_pan,'
             'slot3_lo,slot3_hi,slot3_vol,slot3_pan,slot4_lo,slot4_hi,slot4_vol,slot4_pan')
    L.append("")

    # BROWSE
    L.append("[tab BROWSE]")
    L.append("art file=art/bg_browse.png x=0 y=%d w=1280 h=628 fit=stretch" % Y_OFF)
    if RACK:
        L += browse_rack_lines()
    else:
        header_lines(L)
        x, yy, w, h = BR_LIST
        L.append('readout cx=%d cy=%d w=%d h=34 label="" key=br_loc text_size=17' % (x + w // 2, y(yy + 29), w - 40))
        # the data wheel scrolls the list's highlight while this has focus (a tap on the folder line); Enter opens the row
        # key=br_row: MPC sends the data wheel on this page to Q-Link 1's parameter (br_row), seen on the device 2026-11-02
        L.append('wheel cx=%d cy=%d w=%d h=34 key=br_row enter=br_enter' % (x + w // 2, y(yy + 29), w - 40))
        # 1.7: smaller row text (MPC draws 24 px by default) and long names shortened in the middle by the engine
        L.append('list x=%d y=%d w=%d h=%d cols=1 rows=%d th=40 gap=4 key=br text_size=19' % (x + 14, y(yy + 56), w - 84, BROWSER_ROWS * 44 - 4, BROWSER_ROWS))
        # 1.7.3: one row up / down per tap (the page arrows below move ten)
        L.append('button cx=%d cy=%d label="" key=br_rowup img=art/row_up_off.png img_on=art/row_up_on.png w=44 h=212' % (x + w - 37, y(yy + 56 + 106)))
        L.append('button cx=%d cy=%d label="" key=br_rowdown img=art/row_down_off.png img_on=art/row_down_on.png w=44 h=212'
                 % (x + w - 37, y(yy + 56 + BROWSER_ROWS * 44 - 4 - 106)))
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
        for i, opt in enumerate(("Off", "On")):   # 1.7.2: kit previews
            n = "br_preview_" + opt.lower()
            L.append('option cx=%d cy=%d w=110 h=34 key=br_preview option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                     % (x + 298 + i * 116, y(yy + 156), opt, n, n))
        L.append('readout cx=%d cy=%d w=%d h=24 label="" key=lib_path text_size=12' % (x + w // 2, y(yy + 195), w - 44))
        for i in range(3):   # 1.7.3: the full name, wrapped over three lines (was br_info on one line)
            L.append('readout cx=%d cy=%d w=%d h=18 label="" key=br_sel%d text_size=14' % (x + w // 2, y(yy + 251 + i * 18), w - 52, i + 1))
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
        # 16 (four Q-Link pages, like every other page): the first 8 as before, then the PLAY page's envelope and tuning

    L.append('qlinks "BROWSE" = br_row,volume,pan,cutoff,resonance,rev_mix,drive,pad_vel,'
             'env_attack,env_decay,env_sustain,env_release,filter_vel,vel_sens,transpose,tune')
    L.append("")

    # SETUP
    L.append("[tab SETUP]")
    L.append("art file=art/bg_setup.png x=0 y=%d w=1280 h=628 fit=stretch" % Y_OFF)
    header_lines(L)
    for cx, cy, r, key, _, col in SETUP_KNOBS:
        L.append(knob_line(cx, cy, r, key, col))
    for key, opts, x0, cy, _ in SETUP_OPTIONS:
        ow, pre = set_opt(key)
        for i, opt in enumerate(opts):
            n = pre + opt_name(key, opt)
            L.append('option cx=%d cy=%d w=%d h=%d key=%s option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                     % (x0 + ow // 2 + i * (ow + 6), y(cy), ow, OPT_H, key, opt, n, n))
    # 2.1.16: RESET, bottom right of OUTPUT; the readout beside it asks for the second tap
    L.append('readout cx=%d cy=%d w=300 h=24 label="" key=reset_info text_size=13' % (RESET_X - 250, y(RESET_Y)))
    L.append('button cx=%d cy=%d label="" key=reset_all img=art/b_reset_off.png img_on=art/b_reset_on.png w=160 h=36' % (RESET_X, y(RESET_Y)))
    L.append('qlinks "SETUP" = polyphony,glide,bend,wt_pos,volume,pan,transpose,tune,vel_sens,filter_vel,interp,pad_base,rev_damp,mem_limit,prog_change,auto_extract')
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
        L.append('popup cx=%d cy=%d w=%d h=38 label="" key=mod%d_src text_size=16' % (x + MX_SRC, y(ry), MX_PW, n))
        L.append('popup cx=%d cy=%d w=%d h=38 label="" key=mod%d_dst text_size=16' % (x + MX_DST, y(ry), MX_PW, n))
        L.append('popup cx=%d cy=%d w=%d h=38 label="" key=mod%d_slot text_size=16' % (x + MX_SLOT, y(ry), MX_SW, n))
        # a small knob, not slider_h: MPC's horizontal slider is a square 150 px filmstrip box that overlaps the rows
        L.append('knob cx=%d cy=%d r=17 key=mod%d_amt label="" name=hide value=hide strip=art/%s.png' % (x + 700, y(ry), n, KNOB_STRIPS[CYAN]))
        L.append('readout cx=%d cy=%d w=66 h=32 label="" key=mod%d_amt text_size=16' % (x + 786, y(ry), n))
    L.append('qlinks "MOD" = mod1_amt,mod2_amt,mod3_amt,mod4_amt,mod5_amt,mod6_amt,mod7_amt,mod8_amt,lfo1_rate,lfo1_wave,lfo1_sync,lfo1_retrig,lfo2_rate,lfo2_wave,lfo2_sync,lfo2_retrig')
    L.append("")

    # FX (1.8)
    L.append("[tab FX]")
    L.append("art file=art/bg_fx.png x=0 y=%d w=1280 h=628 fit=stretch" % Y_OFF)
    header_lines(L)
    # 1.8.2: two MPC sub-pages (the tab's dots; press FX again): FX, and MOD FX over it
    F1 = []
    for cx, cy, key, _, col in FX_KNOBS:
        F1.append(knob_line(cx, cy, 34, key, col, 100 if cy == 200 and cx > 700 else 120))
    F1.append('popup cx=%d cy=%d w=120 h=34 label="" key=drive_type text_size=16' % (1200, y(RT_Y)))   # 2.2
    F1.append('popup cx=%d cy=%d w=150 h=32 label="" key=comp_type text_size=16' % (602, y(262)))   # 2.2
    F1.append('meter cx=%d cy=%d w=14 h=84 key=gr_meter strip=art/gr_meter.png frames=%d' % (GR_X, y(180), METER_FRAMES))
    for k, (opt, w_) in enumerate((("Off", 64), ("On", 112))):   # 2.2: OFF | LIMITER
        m = opt_name("limiter", opt)
        F1.append('option cx=%d cy=%d w=%d h=30 key=limiter option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                  % (514 + (w_ // 2 if k == 0 else 64 + 4 + w_ // 2), y(306), w_, opt, m, m))
    for k, opt in enumerate(CHORUS_TYPES[1:]):   # 2.2: Off (the default) has no key
        m = opt_name("cho_type", opt)
        F1.append('option cx=%d cy=%d w=%d h=30 key=cho_type option="%s" label="" img=art/%s_off.png img_on=art/%s_on.png'
                  % (CT_X0 + RT_W // 2 + k * RT_W, y(DT_Y), RT_W, opt, m, m))
    for k, opt in enumerate(DELAY_TYPES[1:]):   # 2.2: Off (the default) has no key
        m = opt_name("dly_type", opt)
        F1.append('option cx=%d cy=%d w=%d h=30 key=dly_type option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                  % (DT_X0 + RT_W // 2 + k * RT_W, y(DT_Y), RT_W, opt, m, m))
    for k, opt in enumerate(REV_TYPES[1:]):   # 2.2: Off (the default) has no key: none lit
        m = opt_name("rev_type", opt)
        F1.append('option cx=%d cy=%d w=%d h=30 key=rev_type option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                  % (RT_X0 + RT_W // 2 + k * RT_W, y(RT_Y), RT_W, opt, m, m))
    F1.append('popup cx=%d cy=%d w=180 h=38 label="" key=dly_time text_size=17' % (742, y(474)))
    for k, opt in enumerate(("Stereo", "Ping-Pong")):
        m = opt_name("dly_ping", opt)
        F1.append('option cx=%d cy=%d w=120 h=32 key=dly_ping option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                  % (1190, y(466 + k * 40), opt, m, m))
    L += [l + ' banks="FX"' for l in F1]
    F2 = ['art file=art/bg_fx2.png x=%d y=%d w=%d h=%d fit=stretch' % (FX2_AREA[0], y(FX2_AREA[1]), FX2_AREA[2], FX2_AREA[3])]
    for cx, cy, key, _, col in FX2_KNOBS:
        F2.append(knob_line(cx, cy, 34, key, col, 120))
    F2.append('popup cx=%d cy=%d w=%d h=38 label="" key=trm_sync text_size=17' % (TR_X0 + TR_SEG_W, y(TR_SYNC_Y), 2 * TR_SEG_W))
    for key, opts, yv in (("trm_mode", TREM_MODES, TR_MODE_Y), ("trm_shape", TREM_SHAPES, TR_SHAPE_Y)):
        for k, opt in enumerate(opts):
            m = opt_name(key, opt)
            F2.append('option cx=%d cy=%d w=%d h=32 key=%s option="%s" label="" img=art/%s_off.png img_on=art/%s_on.png'
                      % (TR_X0 + TR_SEG_W // 2 + k * TR_SEG_W, y(yv), TR_SEG_W, key, opt, m, m))
    L += [l + ' banks="MOD FX"' for l in F2]
    # 2.4.30: FX BANK, the third page (the FX Expansion kit's look; positions from FXB_* in frame coordinates)
    F3 = ['art file=art/bg_fx3.png x=%d y=%d w=%d h=%d fit=stretch' % (FX2_AREA[0], y(FX2_AREA[1]), FX2_AREA[2], FX2_AREA[3])]
    ox, oy, ow, oh = FXB_OLED
    px, py = _fx(ox + ow / 2, oy + 0.17 * oh)
    # the program: < name > in the OLED's top line (a 118-entry drop-down ran past the screen: skin_check EDGE)
    F3.append('readout cx=%d cy=%d w=%d h=30 label="" key=fxb_pname text_size=17' % (px, y(py), ow - 96))   # 2.4.33: the slot's program
    F3.append('button cx=%d cy=%d label="" key=fxb_prog_prev img=art/stp34_l_off.png img_on=art/stp34_l_on.png w=34 h=34'
              % (px - ow / 2 + 26, y(py)))
    F3.append('button cx=%d cy=%d label="" key=fxb_prog_next img=art/stp34_r_off.png img_on=art/stp34_r_on.png w=34 h=34'
              % (px + ow / 2 - 26, y(py)))
    px, py = _fx(ox + ow / 2, oy + 0.83 * oh)
    F3.append('readout cx=%d cy=%d w=%d h=26 label="" key=fxb_info text_size=14' % (px, y(py), ow - 40))
    fx_, fy_ = _fx(*FXB_FADER)
    F3.append('knob cx=%d cy=%d r=43 key=fxb_mix label="" name=hide value=hide strip=art/fxb_fader.png width=104' % (fx_, y(fy_)))
    F3.append('readout cx=%d cy=%d w=88 h=28 label="" key=fxb_time text_size=16' % (fx_, y(fy_ - 46)))
    F3.append('readout cx=%d cy=%d w=88 h=28 label="" key=fxb_mix text_size=16' % (fx_, y(fy_ + 46)))
    for fxx, fyy, key, _, col, r in FXB_KNOBS:
        kx, ky = _fx(fxx, fyy)
        strip = "fxb_knob_cyan" if col == FXB_CYAN else "fxb_knob_purple"
        F3.append('knob cx=%d cy=%d r=%d key=%s label="" name=hide value=hide strip=art/%s.png width=%d'
                  % (kx, y(ky), r, key, strip, 104 if r < 30 else 120))
    for k, opt in enumerate(FXB_BYPASS):
        kx, ky = _fx(130 + (k - 0.5) * (FXB_KEY_W + 4), FXB_KEY_Y)
        m = opt_name("fxb_bypass", opt)
        F3.append('option cx=%d cy=%d w=%d h=%d key=fxb_bypass option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                  % (kx, y(ky), FXB_KEY_W, FXB_KEY_H, opt, m, m))
    # 2.4.33: the bay's slot keys A-D (bottom-left plate)
    for k, opt in enumerate(FXB_SLOTS):
        kx, ky = _fx(FXB_SLOT_X0 + k * (FXB_SLOT_W + FXB_SLOT_GAP), FXB_SLOT_Y)
        m = opt_name("fxb_slot", opt)
        F3.append('option cx=%d cy=%d w=%d h=%d key=fxb_slot option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                  % (kx, y(ky), FXB_SLOT_W, FXB_SLOT_H, opt, m, m))
    # 2.4.32: the 3-digit screen (readout + a see-through key over it) and the keypad, shown while fxb_pad is Open
    ncx, ncy, nw, nh = FXB_NUM
    sx, sy = _fx(ncx, ncy)
    F3.append('readout cx=%d cy=%d w=%d h=%d label="" key=fxb_num text_size=22' % (sx, y(sy), nw - 8, nh - 4))
    F3.append('button cx=%d cy=%d label="" key=fxb_num_tap img=art/fxb_numtap_off.png img_on=art/fxb_numtap_on.png w=%d h=%d'
              % (sx, y(sy), nw, nh))
    px_, py_, pw_, ph_ = FXB_PAD
    F3.append('art file=art/fxb_keypad.png x=%d y=%d w=%d h=%d fit=stretch when=fxb_pad:Open' % (px_, y(py_), pw_, ph_))
    F3.append('readout cx=%d cy=%d w=%d h=34 label="" key=fxb_num text_size=24 when=fxb_pad:Open' % (px_ + pw_ - 70, y(py_ + 31), 100))
    kx0, ky0 = px_ + 14, py_ + 64
    for n_, (lab, key) in enumerate(FXB_PADKEYS):
        cx_ = kx0 + (n_ % 3) * (FXB_KEY_W2 + FXB_KEY_GAP) + FXB_KEY_W2 // 2
        cy_ = ky0 + (n_ // 3) * (FXB_KEY_H2 + FXB_KEY_GAP) + FXB_KEY_H2 // 2
        F3.append('button cx=%d cy=%d label="" key=%s img=art/fxb_key_%s_off.png img_on=art/fxb_key_%s_on.png w=%d h=%d when=fxb_pad:Open'
                  % (cx_, y(cy_), key, key, key, FXB_KEY_W2, FXB_KEY_H2))
    L += [l + ' banks="FX BANK"' for l in F3]
    L.append('qlinks "FX" = eq_low,eq_mid,eq_high,comp,cho_mix,cho_rate,cho_depth,dly_mix,dly_time,dly_fb,dly_tone,dly_ping,'
             'rev_mix,rev_size,rev_damp,drive')
    L.append('qlinks "MOD FX" = ' + ",".join(MODFX_KEYS))
    L.append('qlinks "FX BANK" = ' + ",".join(FXB_KEYS[:14] + ["fxb_bypass", "volume"]))   # 2.4.30: 16 Q-Links
    L.append("")

    # INFO
    L.append("[tab INFO]")
    L.append("art file=art/bg_info.png x=0 y=%d w=1280 h=628 fit=stretch when=info_page:1" % Y_OFF)
    L.append("art file=art/bg_info2.png x=0 y=%d w=1280 h=628 fit=stretch when=info_page:2" % Y_OFF)
    L.append("art file=art/bg_info3.png x=0 y=%d w=1280 h=628 fit=stretch when=info_page:3" % Y_OFF)
    header_lines(L)
    # 1.7: the formats list's pages (1.8: three; PG 3 sits right of the title, which PG 1 / PG 2 stop short of)
    for opt, cx in ((("1", 272), ("2", 336), ("3", 400)) if RACK else (("1", 357), ("2", 421), ("3", 867))):   # rack: left of the logo glass
        n = "info_page_" + opt
        L.append('option cx=%d cy=%d w=58 h=30 key=info_page option=%s label="" img=art/%s_off.png img_on=art/%s_on.png'
                 % (cx, y(28), opt, n, n))
    L.append('qlinks "INFO" = volume,pan,cutoff,resonance,rev_mix,drive,pad_vel,transpose,'
             'env_attack,env_decay,env_sustain,env_release,filter_vel,vel_sens,tune,glide')
    if RACK:   # 2.1.5: the logo's golden spark over every page's logo, after the page's background art
        import spark
        ax, ay, aw, ah = spark.AREA
        line = 'meter cx=%d cy=%d w=%d h=%d key=logo_fx strip=art/logo_spark.png frames=%d exact=1' % (ax + aw // 2, y(ay + ah // 2), aw, ah, spark.FRAMES)
        out, i = [], 0
        while i < len(L):
            out.append(L[i])
            if L[i].startswith("[tab "):
                i += 1
                while i < len(L) and L[i].startswith("art "):
                    out.append(L[i])
                    i += 1
                out.append(line)
                continue
            i += 1
        L = out
    L = qlinks_all_knobs(L)   # 2.4.30
    return "\n".join(L) + "\n"


QL2_OVERLAYS = {}   # 2.4.30: overflow Q-Link page -> (bbox in screen px, [(kind, cx, cy, r|w, h)]), filled by layout()
QL2_BADGE_W, QL2_BADGE_H = 104, 24
QL2_BADGE_AT = {"LAYERS": (712, 507)}   # pages whose free spot looks accidental: beside LAYER MAP's title
# 2.4.32 (user, on the MPC: OSC 2 / MIX 2 / ENV 2 "glitchy, malformed, Q-Link labels in the way"): OFF. shadow_skin.py bakes an
# art line with banks= into an OPAQUE image: the tab's base layer (the OSC page's art and labels) + the overlay, cropped to
# the overlay's box, drawn over the page: OSC labels and rings covered the controls. The tab names say "OSC 2" etc.
QL2_ART = False


def ql2_badge_spot(geo, busy):
    """the badge's top-left: below, right of or left of a highlighted knob, the first spot that covers no control and no
    knob name and stays on the page (screen y 150..712)"""
    BW, BH = QL2_BADGE_W, QL2_BADGE_H
    def free(x, y):
        if x < 4 or x + BW > 1276 or y < 150 or y + BH > 712:
            return False
        return all(x + BW <= a or x >= c or y + BH <= b or y >= d for a, b, c, d in busy)
    for kind, cx, cy, a, b in geo:
        half_w = a if kind == "knob" else a // 2
        half_h = a if kind == "knob" else b // 2
        for x, y in ((cx - BW // 2, cy + half_h + 12), (cx + half_w + 14, cy - BH // 2), (cx - half_w - 14 - BW, cy - BH // 2)):
            if free(x, y):
                return (int(x), int(y))
    kind, cx, cy, a, b = geo[0]   # none of those: the free spot on the page nearest the first ring
    best = None
    for y in range(150, 712 - BH, 4):
        for x in range(4, 1276 - BW, 8):
            if free(x, y):
                d = (x + BW / 2 - cx) ** 2 + (y + BH / 2 - cy) ** 2
                if best is None or d < best[0]:
                    best = (d, x, y)
    if best:
        return (best[1], best[2])
    return (int(cx - BW // 2), int(cy + (a if kind == "knob" else b // 2) + 12))


def ql2_bbox(geo, badge=None):
    """the overlay's box: every ring (knob r + 9, a fader's frame + 7) and the badge"""
    xs, ys = [], []
    for kind, cx, cy, a, b in geo:
        if kind == "knob":
            xs += [cx - a - 10, cx + a + 10]; ys += [cy - a - 10, cy + a + 10]
        else:
            xs += [cx - a // 2 - 8, cx + a // 2 + 8]; ys += [cy - b // 2 - 8, cy + b // 2 + 8]
    if badge:
        xs += [badge[0], badge[0] + QL2_BADGE_W]; ys += [badge[1], badge[1] + QL2_BADGE_H]
    return (max(0, min(xs)), max(0, min(ys)), min(1280, max(xs)), max(ys))


def ql2_overlays():
    """2.4.30: the overflow pages' overlays (art/ql2_*.png): a cyan ring round each knob / fader the page's Q-Links move,
    and a Q-LINKS 2 badge above the first; transparent elsewhere, cropped to that area (image memory)"""
    if not QL2_ART:
        return []
    QL2_OVERLAYS.clear()
    layout()
    S2 = 3
    for slug, (bbox, geo, badge) in QL2_OVERLAYS.items():
        x0, y0, x1, y1 = bbox
        im = Image.new("RGBA", ((x1 - x0) * S2, (y1 - y0) * S2), (0, 0, 0, 0))
        rings = Image.new("RGBA", im.size, (0, 0, 0, 0))
        rd = ImageDraw.Draw(rings)
        for kind, cx, cy, a, b in geo:
            X, Y = (cx - x0) * S2, (cy - y0) * S2
            if kind == "knob":
                R = (a + 7) * S2
                rd.ellipse((X - R, Y - R, X + R, Y + R), outline=CYAN + (255,), width=2 * S2)
            else:
                rd.rounded_rectangle((X - (a // 2 + 5) * S2, Y - (b // 2 + 5) * S2, X + (a // 2 + 5) * S2, Y + (b // 2 + 5) * S2),
                                     radius=6 * S2, outline=CYAN + (255,), width=2 * S2)
        glow = rings.filter(ImageFilter.GaussianBlur(3 * S2))
        im = Image.alpha_composite(im, glow)
        im = Image.alpha_composite(im, rings)
        bx, by = (badge[0] - x0) * S2, (badge[1] - y0) * S2
        d = ImageDraw.Draw(im)
        d.rounded_rectangle((bx, by, bx + QL2_BADGE_W * S2, by + QL2_BADGE_H * S2), radius=12 * S2, fill=(10, 40, 52, 235),
                            outline=CYAN + (255,), width=int(1.5 * S2))
        d.text((bx + QL2_BADGE_W * S2 / 2, by + QL2_BADGE_H * S2 / 2), "Q-LINKS 2", font=font(13, "Bold"), anchor="mm",
               fill=(220, 250, 255, 255))
        im.resize((x1 - x0, y1 - y0), Image.LANCZOS).save(os.path.join(ART, slug + ".png"))
    return sorted(QL2_OVERLAYS)


def qlinks_all_knobs(L):
    """2.4.30 (the user: every knob on every page on the Q-Links). Per tab, each `qlinks` page gets every knob / fader
    shown on it: the knobs it already had keep their order, missing ones follow, spare slots keep the page's other
    entries. A page with more than 16 knobs gets a second Q-Link sub-page "<PAGE> 2" after the tab's own pages (same
    widgets on screen, its Q-Links on the remaining knobs); the widgets of that page are put on both."""
    import re as _re
    tabs, cur = [], None
    for i, l in enumerate(L):
        if l.startswith("[tab "):
            cur = [i, None]; tabs.append(cur)
        elif cur is not None:
            cur[1] = i
    out_add = {}   # qlinks line index -> extra lines after it
    rename = {}    # (tab start) -> {page: "page|page 2"}
    for start, end in tabs:
        if end is None:
            continue
        ql = [(i, _re.match(r'qlinks "([^"]+)" = (.*)$', L[i])) for i in range(start, end + 1) if L[i].startswith("qlinks ")]
        ql = [(i, m.group(1), m.group(2).split(",")) for i, m in ql if m]
        if not ql:
            continue
        pages = [t for _, t, _ in ql]
        knobs = {t: [] for t in pages}
        for i in range(start, end + 1):
            l = L[i]
            if not (l.startswith("knob ") or l.startswith("slider_v ")):
                continue
            k = _re.search(r"\bkey=(\S+)", l).group(1)
            b = _re.search(r'banks="([^"]+)"', l)
            for t in (b.group(1).split("|") if b else pages):
                if t in knobs and k not in knobs[t]:
                    knobs[t].append(k)
        for i, t, old in ql:
            K = knobs[t]
            order = [k for k in old if k in K] + [k for k in K if k not in old]
            others = [o for o in old if o not in K]
            first, rest = order[:16], order[16:]
            if not rest:
                L[i] = 'qlinks "%s" = %s' % (t, ",".join((first + others)[:16]))
                continue
            assert len(rest) <= 16, "page %s has %d knobs" % (t, len(order))
            L[i] = 'qlinks "%s" = %s' % (t, ",".join(first))
            # 2.4.30 (user: "OSC 2 looks like a duplicate"): a Q-LINKS 2 badge + a ring round each knob these Q-Links move
            geo = []
            for j in range(start, end + 1):
                l = L[j]
                if not (l.startswith("knob ") or l.startswith("slider_v ")):
                    continue
                k = _re.search(r"\bkey=(\S+)", l).group(1)
                b = _re.search(r'banks="([^"]+)"', l)
                if k not in rest or (b and t not in b.group(1).split("|")):
                    continue
                num_ = lambda n, d=0: int(float(_re.search(r"\b%s=(-?[0-9.]+)" % n, l).group(1))) if _re.search(r"\b%s=" % n, l) else d
                if l.startswith("knob "):
                    geo.append(("knob", num_("cx"), num_("cy"), num_("r"), 0))
                else:
                    geo.append(("slider", num_("cx"), num_("cy"), num_("w"), num_("h")))
            busy = []   # every control on this page, and the name band above each knob: the badge must not cover them
            for j in range(start, end + 1):
                l = L[j]
                if l.startswith(("art ", "qlinks", "[")):
                    continue
                b = _re.search(r'banks="([^"]+)"', l)
                if b and t not in b.group(1).split("|"):
                    continue
                fnd = lambda n: _re.search(r"\b%s=(-?[0-9.]+)" % n, l)
                if not (fnd("cx") and fnd("cy")):
                    continue
                cx_, cy_ = float(fnd("cx").group(1)), float(fnd("cy").group(1))
                if fnd("r"):
                    r_ = float(fnd("r").group(1))
                    busy.append((cx_ - r_ - 6, cy_ - r_ - 30, cx_ + r_ + 6, cy_ + r_ + 6))   # + its name above
                    busy.append((cx_ - 50, cy_ - r_ - 30, cx_ + 50, cy_ - r_ - 8))
                elif fnd("w") and fnd("h"):
                    w_, h_ = float(fnd("w").group(1)), float(fnd("h").group(1))
                    busy.append((cx_ - w_ / 2, cy_ - h_ / 2, cx_ + w_ / 2, cy_ + h_ / 2))
            slug = "ql2_" + _re.sub(r"[^a-z0-9]+", "_", t.lower()).strip("_")
            badge = QL2_BADGE_AT.get(t) or ql2_badge_spot(geo, busy)
            bbox = ql2_bbox(geo, badge)
            QL2_OVERLAYS[slug] = (bbox, geo, badge)
            # under the controls, over the page's background: just before the first control on this page (its background
            # art comes before its controls; an image over a knob may take its touches)
            bg = start
            for j in range(start + 1, end + 1):
                b = _re.search(r'banks="([^"]+)"', L[j])
                if L[j].startswith(("art ", "qlinks")) or not b or t not in b.group(1).split("|"):
                    continue
                bg = j - 1
                break
            if bg == start:   # a tab without banks= (LAYERS): after its own art lines
                while bg + 1 <= end and L[bg + 1].startswith("art "):
                    bg += 1
            if QL2_ART: out_add.setdefault(bg, []).append('art file=art/%s.png x=%d y=%d w=%d h=%d fit=stretch banks="%s 2"'
                                               % (slug, bbox[0], bbox[1], bbox[2] - bbox[0], bbox[3] - bbox[1], t))
            last = ql[-1][0]   # after the tab's own pages: the sub-page cycle the user knows stays as it was
            out_add.setdefault(last, []).append('qlinks "%s 2" = %s' % (t, ",".join((rest + others)[:16])))
            rename.setdefault(start, {})[t] = "%s|%s 2" % (t, t)
    for start, end in tabs:
        ren = rename.get(start)
        if not ren or end is None:
            continue
        for i in range(start, end + 1):
            b = _re.search(r'banks="([^"]+)"', L[i])
            if b and not L[i].startswith("qlinks"):
                pages = b.group(1).split("|")
                new = []
                for p_ in pages:
                    new += ren.get(p_, p_).split("|")
                L[i] = L[i].replace('banks="%s"' % b.group(1), 'banks="%s"' % "|".join(dict.fromkeys(new)))
    out = []
    for i, l in enumerate(L):
        out.append(l)
        out += out_add.get(i, [])
    return out


CSS = """/* Omni Sampler: the renderer draws only controls; panels, labels and the display are art/bg_*.png. */
.box, .box-label { display: none; }
/* pad numbers on the pad images */
.btn-label, .button-label { font-weight: 700; }
"""


def merge_layouts(texts):
    """the skins' layouts as one: each skin's widget lines tagged when=skin:<n> with their art in art/<skin>/; the
    header and Q-Link lines once (the last skin's); INFO keeps the last skin's lines for every skin (its art already
    follows info_page, and a line has one when=)"""
    def split(t):
        head, tabs, cur = [], [], None
        for ln in t.splitlines():
            if ln.startswith("[tab "):
                cur = [ln, []]
                tabs.append(cur)
            elif cur is None:
                head.append(ln)
            else:
                cur[1].append(ln)
        return head, tabs
    parts = [split(t) for t in texts]
    out = list(parts[-1][0])
    for ti, (tab, last_lines) in enumerate(parts[-1][1]):
        out.append(tab)
        if tab.strip() == "[tab INFO]":
            out += [ln.replace("=art/", "=art/%s/" % SKINS[-1][0]) for ln in last_lines]
            continue
        tail = []
        for si, (_, tabs) in enumerate(parts):
            name, lines = tabs[ti]
            assert name == tab, (name, tab)
            for ln in lines:
                s = ln.strip()
                if not s or s.startswith("#") or s.startswith("qlinks"):
                    if si == len(parts) - 1 and s:
                        tail.append(ln)
                    continue
                assert not SKIN_SWITCH or " when=" not in ln, ln   # (one skin: no when=skin: is added, a line may have its own)
                out.append(ln.replace("=art/", "=art/%s/" % SKINS[si][0]) + (" when=skin:%d" % si if SKIN_SWITCH else ""))
        out += [""] + tail + [""]
    # an image that is the same in several skins (the keys, knobs, faders Silver and Modules share): one file, so MPC
    # decodes it once
    import hashlib
    canon, seen = {}, {}
    for name, _, _ in reversed(SKINS):
        d = os.path.join(HERE, "art", name)
        for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            h = hashlib.sha1(open(os.path.join(d, f), "rb").read()).hexdigest()
            canon["art/%s/%s" % (name, f)] = seen.setdefault(h, "art/%s/%s" % (name, f))
    import re as _re
    text = "\n".join(out) + "\n"
    return _re.sub(r"art/[a-z]+/[^ ]+?\.png", lambda m: canon.get(m.group(0), m.group(0)), text)


def main(text_only=False):
    """all skins (art + params.json, skin.css, layout.conf); text_only: the text files only, the art left as it is"""
    if not SKIN:   # all skins: each in a child run, then one layout
        import subprocess
        texts = []
        for name, _, env in SKINS:
            e = dict(os.environ, OMNI_SKIN=name, OMNI_TEXT_ONLY="1" if text_only else "", **env)
            subprocess.run([sys.executable, os.path.abspath(__file__)], env=e, check=True)
            texts.append(open(os.path.join(HERE, "layout.%s.conf" % name)).read())
            os.remove(os.path.join(HERE, "layout.%s.conf" % name))
        for f in ([] if text_only else os.listdir(os.path.join(HERE, "art"))):   # the single-skin art of earlier versions
            if f.endswith(".png"):
                os.remove(os.path.join(HERE, "art", f))
        with open(os.path.join(HERE, "params.json"), "w") as f:
            json.dump(params(), f, indent=1)
            f.write("\n")
        with open(os.path.join(HERE, "skin.css"), "w") as f:
            f.write(CSS)
        with open(os.path.join(HERE, "layout.conf"), "w") as f:
            f.write(merge_layouts(texts))
        print("wrote params.json, skin.css, layout.conf and art/{%s}/" % ",".join(n for n, _, _ in SKINS))
        return
    if not os.environ.get("OMNI_TEXT_ONLY"):
        images()
    with open(os.path.join(HERE, "layout.%s.conf" % SKIN), "w") as f:
        f.write(layout())
    print("skin %s: art/%s/" % (SKIN, SKIN))
    return


def main_single():
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
