// Omni Sampler 2.4: the ARP tab's step sequencer / arpeggiator (arp.hpp).
#include "arp.hpp"
#include <algorithm>
#include <cmath>

namespace omni {

namespace {
constexpr double SR = 44100.0;
// each scale's notes in semitones from its root (the first is 0)
struct Scale { int n; int iv[12]; };
const Scale SCALE_TABLE[Arp::SCALES] = {
    {12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},   // Chromatic
    {7, {0, 2, 4, 5, 7, 9, 11}},                    // Major
    {7, {0, 2, 3, 5, 7, 8, 10}},                    // Minor
    {7, {0, 2, 3, 5, 7, 9, 10}},                    // Dorian
    {7, {0, 1, 3, 5, 7, 8, 10}},                    // Phrygian
    {7, {0, 2, 4, 6, 7, 9, 11}},                    // Lydian
    {7, {0, 2, 4, 5, 7, 9, 10}},                    // Mixolydian
    {7, {0, 1, 3, 5, 6, 8, 10}},                    // Locrian
    {7, {0, 2, 3, 5, 7, 8, 11}},                    // Harmonic Minor
    {7, {0, 2, 3, 5, 7, 9, 11}},                    // Melodic Minor
    {5, {0, 2, 4, 7, 9}},                           // Major Pentatonic
    {5, {0, 3, 5, 7, 10}},                          // Minor Pentatonic
    {6, {0, 3, 5, 6, 7, 10}},                       // Blues
    {6, {0, 2, 4, 6, 8, 10}},                       // Whole Tone
    {5, {0, 2, 3, 7, 8}},                           // Hirajoshi
    {7, {0, 1, 4, 5, 7, 8, 10}},                    // Phrygian Dominant
};
int fdiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
int fmod_(int a, int b) { int m = a % b; return m < 0 ? m + b : m; }
}  // namespace

const char *const Arp::SCALE_NAMES[Arp::SCALES] = {"Chromatic", "Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian",
                                                   "Locrian", "Harm Minor", "Mel Minor", "Major Pent", "Minor Pent", "Blues",
                                                   "Whole Tone", "Hirajoshi", "Phryg Dom"};
const char *const Arp::RATE_NAMES[Arp::RATES] = {"1/4", "1/8", "1/16", "1/32", "1/8 T", "1/16 T", "1/32 T"};
const double Arp::RATE_BEATS[Arp::RATES] = {1.0, 0.5, 0.25, 0.125, 1.0 / 3, 1.0 / 6, 1.0 / 12};   // quarter notes a step

Arp::Arp() {
    rng_ ^= uint32_t(reinterpret_cast<uintptr_t>(this) >> 4) * 2654435761u;   // (each instance its own CHANCE / Random)
    if (!rng_) rng_ = 0x9E3779B9u;
    for (int i = 0; i < STEPS; i++) { mask[i].store(0); vel[i].store(100); }
    for (int i = 0; i < STEPS * ROWS; i++) { ngate[i].store(0); nvel[i].store(0); nskip[i].store(0); nratch[i].store(0); nslide[i].store(0); noct[i].store(0); }
}

int Arp::scale_size(int scale) { return SCALE_TABLE[std::max(0, std::min(Arp::SCALES - 1, scale))].n; }

int Arp::degree_of(int key, int scale, int root) {
    const Scale &s = SCALE_TABLE[std::max(0, std::min(Arp::SCALES - 1, scale))];
    const int rel = key - root, oct = fdiv(rel, 12), pc = fmod_(rel, 12);
    int i = 0;
    while (i + 1 < s.n && s.iv[i + 1] <= pc) i++;
    return oct * s.n + i;
}

int Arp::note_of(int degree, int scale, int root) {
    const Scale &s = SCALE_TABLE[std::max(0, std::min(Arp::SCALES - 1, scale))];
    return root + fdiv(degree, s.n) * 12 + s.iv[fmod_(degree, s.n)];
}

bool Arp::in_scale(int key, int scale, int root) { return note_of(degree_of(key, scale, root), scale, root) == key; }

int Arp::seq_note(int key, int row, int scale, int root, int base) {
    root = fmod_(root, 12);
    const int R = degree_of(60 + root, scale, root);   // the root at C3: plays the pattern as written
    const int kd = degree_of(key, scale, root);
    const int chroma = key - note_of(kd, scale, root);   // an off-scale key: the pattern a semitone (or so) up
    const int n = note_of(R + base + (kd - R) + row, scale, root) + chroma;
    return n >= 0 && n <= 127 ? n : -1;
}

int Arp::arp_note(const int *pool, int n, int row) {
    if (n <= 0) return -1;
    const int x = pool[row % n] + 12 * (row / n);
    return x >= 0 && x <= 127 ? x : -1;
}

int Arp::pool(int *out) const {
    int n = 0;
    const uint8_t *src = any_held_ ? held_ : latched_;
    for (int k = 0; k < 128; k++) if (src[k]) out[n++] = k;
    return n;
}

void Arp::key_on(int note, int velocity, int chan) {
    note &= 127;
    chan &= 15;
    if (!any_held_) ord_i_ = 0;   // a new phrase: ORDER starts over
    if (!any_held_ && settings.latch.load()) { std::fill(latched_, latched_ + 128, 0); latched_any_ = false; }   // a new chord replaces the latched one
    if (!held_[note]) order_[note] = ++order_n_;
    held_[note] = uint8_t(std::max(1, std::min(127, velocity)));
    chmask_[note] |= uint16_t(1u << chan);
    chan_[note] = uint8_t(chan);
    if (settings.latch.load()) { latched_[note] = held_[note]; lchan_[note] = chan_[note]; latched_any_ = true; }
    any_held_ = true;
    keys_rev.fetch_add(1);
}

void Arp::key_off(int note, int chan) {
    note &= 127;
    if (!held_[note]) return;
    if (chan >= 0 && (chmask_[note] >> (chan & 15) & 1)) {   // one of several channels lets go: the pitch is still held
        chmask_[note] &= uint16_t(~(1u << (chan & 15)));
        if (chmask_[note]) {
            if (chan_[note] == (chan & 15)) { int c = 0; while (!(chmask_[note] >> c & 1)) c++; chan_[note] = uint8_t(c); }
            return;
        }
    }
    chmask_[note] = 0;
    held_[note] = 0;
    any_held_ = false;
    for (int k = 0; k < 128; k++) if (held_[k]) { any_held_ = true; break; }
    keys_rev.fetch_add(1);
}

void Arp::note_off_all(Out &out) {
    for (int i = 0; i < snd_n_; i++) out.arp_off_ch(snd_[i].chan, snd_[i].note);
    snd_n_ = 0;
    pend_n_ = 0;
}

void Arp::reset(Out &out) {
    note_off_all(out);
    std::fill(held_, held_ + 128, 0);
    std::fill(latched_, latched_ + 128, 0);
    std::fill(chmask_, chmask_ + 128, 0);
    any_held_ = latched_any_ = false;
    running_ = false;
    step_now.store(-1);
    last_step_ = -1;   // 2.4.14
    keys_rev.fetch_add(1);
}

void Arp::stop(Out &out) {
    note_off_all(out);   // keys up: the pattern's notes end (a tied note would otherwise hold on for bars)
    running_ = false;
    step_now.store(-1);
    last_step_ = -1;     // 2.4.29 (GRID audit F1): a new phrase is not "moving back" into the old one's run (its step 0 was silent)
}

double Arp::beats_per_step() const { return RATE_BEATS[std::max(0, std::min(RATES - 1, settings.rate.load()))]; }

double Arp::step_time(long k) const {
    const double b = beats_per_step();
    return double(k) * b + ((k & 1) ? settings.swing.load() * 0.5 * b : 0.0);
}

void Arp::transport(double ppq, double bpm, bool playing, bool valid) {
    if (bpm > 1) bpm_ = bpm;
    // MPC can say "playing" while its position stands still: that is not a transport to follow
    if (playing && valid) {
        host_still_ = host_last_ >= 0 && std::fabs(ppq - host_last_) < 1e-9 ? host_still_ + 1 : 0;
        host_last_ = ppq;
    } else {
        host_still_ = 0;
        host_last_ = -1;
    }
    const bool follow = playing && valid && host_still_ < 2;
    if (follow) {
        // a jump (start, loop, locate): carry on from the step at the new position, not every step in between
        if (!host_ || std::fabs(ppq - pos_) > beats_per_step() * 0.5) {
            pos_ = ppq;
            const double b = beats_per_step();
            next_k_ = long(std::ceil(ppq / b - 1e-9));
            if (next_k_ > 0 && step_time(next_k_ - 1) >= ppq - 1e-9) next_k_--;
        } else pos_ = ppq;   // (small drifts: the host's position wins)
    } else if (host_) {      // the transport stopped: run on our own from here
        next_k_ = long(std::ceil(pos_ / beats_per_step() - 1e-9));
    }
    host_ = follow;
}

void Arp::start_free() {
    pos_ = 0;
    next_k_ = 0;
    last_step_ = -1;   // 2.4.29 (audit F1)
}

int Arp::next_ordered(const int *pl, int pn, int *out) {
    const int ord = settings.order.load();
    if (ord == O_CHORD) {
        for (int i = 0; i < pn; i++) out[i] = pl[i];
        return pn;
    }
    int base[128], bn = 0;
    if (ord == O_PLAYED) {   // the keys in the order they went down
        for (int i = 0; i < pn; i++) base[bn++] = pl[i];
        std::sort(base, base + bn, [this](int a, int b) { return order_[a] < order_[b]; });
    } else {
        for (int i = 0; i < pn; i++) base[bn++] = pl[i];
    }
    const int oct = std::max(1, std::min(4, settings.octaves.load()));
    int ext[512], n = 0;
    for (int o = 0; o < oct; o++)
        for (int i = 0; i < bn; i++)
            if (base[i] + 12 * o <= 127) ext[n++] = base[i] + 12 * o;
    if (n == 0) return 0;
    int idx = 0;
    const int i = ord_i_++;
    switch (ord) {
        case O_DOWN: idx = n - 1 - i % n; break;
        case O_UPDOWN: case O_DOWNUP: {
            const int p = std::max(1, 2 * n - 2), j = i % p;
            idx = j < n ? j : 2 * n - 2 - j;
            if (ord == O_DOWNUP) idx = n - 1 - idx;
            break;
        }
        case O_RANDOM:
            rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
            idx = int(rng_ % uint32_t(n));
            if (n > 1 && idx == last_rand_) idx = (idx + 1) % n;
            last_rand_ = idx;
            break;
        default: idx = i % n; break;   // Up, As Played
    }
    out[0] = ext[idx];
    return 1;
}

void Arp::sound(Out &out, int note, int chan, int v, int64_t dur) {
    for (int i = 0; i < snd_n_; i++)   // the same note still sounding (on any channel): end it first, on the channel it started on
        if (snd_[i].note == note) { out.arp_off_ch(snd_[i].chan, note); snd_[i] = snd_[--snd_n_]; break; }
    if (snd_n_ == 64) { out.arp_off_ch(snd_[0].chan, snd_[0].note); snd_[0] = snd_[--snd_n_]; }
    out.arp_on_ch(chan, note, v);
    snd_[snd_n_++] = {note, chan, std::max<int64_t>(64, dur)};
}

uint32_t Arp::rnd() {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return rng_;
}

int Arp::step_of(long k) {
    int pages[4], np = 0;
    for (auto &c : settings.chain) { const int p = c.load(); if (p >= 0 && p < 4) pages[np++] = p; }
    const int total = np ? np * 16 : std::max(1, std::min(STEPS, settings.length.load()));
    long i = ((k % total) + total) % total;
    switch (settings.direction.load()) {
        case 1: i = total - 1 - i; break;                                  // back
        case 2: if (total > 1) { const long p = 2L * total - 2, j = ((k % p) + p) % p; i = j < total ? j : p - j; } break;
        case 3:                                                            // random walk: a step on or back, now and then a leap
            if (k != walk_k_) {
                const uint32_t r = rnd() % 8;
                walk_ = r < 4 ? walk_ + 1 : r < 6 ? walk_ - 1 : r == 6 ? walk_ : int(rnd() % uint32_t(total));
                walk_ = ((walk_ % total) + total) % total;
                walk_k_ = k;
            }
            i = walk_;
            break;
        default: break;
    }
    return np ? pages[i / 16] * 16 + int(i % 16) : int(i);
}

void Arp::play_step(Out &out, long k) {
    const int len = std::max(1, std::min(STEPS, settings.length.load()));
    const int s = step_of(k);
    step_now.store(s);
    // 2.4.14 (audit B4): which way the playhead came: TIE continues a run only from the step played just before it
    // (DIRECTION Back / Ping-Pong / Random and CHAIN played runs from the pattern's neighbour: Back skipped them, a chained
    // page dropped a note when the page before it in memory ended on the same row, a random leap into a run was silent)
    const int prev = last_step_;
    last_step_ = s;
    const uint16_t m = mask[s].load(std::memory_order_relaxed);
    if (!m) return;
    const int mode = settings.mode.load(), scale = settings.scale.load(), root = fmod_(settings.root.load(), 12);
    const bool tie = settings.tie.load() != 0;
    const bool chained = settings.chain[0].load() >= 0 || settings.chain[1].load() >= 0 || settings.chain[2].load() >= 0 || settings.chain[3].load() >= 0;
    const int run_end = chained ? (s / 16 + 1) * 16 : len;   // (a chained page's runs stop at its end)
    const int run_start = chained ? (s / 16) * 16 : 0;       // 2.4.14 (and start at its start)
    const bool backward = prev == s + 1;                     // 2.4.14: moving back a step (Back, Ping-Pong's return, a walk)
    int pl[128];
    const int pn = pool(pl);
    if (pn == 0) return;
    const uint8_t *vels = any_held_ ? held_ : latched_, *chs = any_held_ ? chan_ : lchan_;   // (2.4.30: and the channels)
    int key = pl[0];
    if (mode == 0)   // SEQ: the key pressed last
        for (int i = 0, best = -1; i < pn; i++) if (order_[pl[i]] > best) { best = order_[pl[i]]; key = pl[i]; }
    const double step_frames = beats_per_step() * 60.0 / bpm_ * SR;
    const double gate = std::max(0.05f, std::min(1.0f, settings.gate.load()));
    const int sv = vel[s].load(std::memory_order_relaxed);
    const int base = settings.base.load(), ord = settings.order.load();
    struct N { int note, v, chan; int64_t dur; int ratch; };
    N ns[160];
    int nn = 0;
    bool chord_done = false;   // 2.4.29 (audit F3): ORDER Chord plays the held chord once per step (was once per lit row:
                               // 3 rows = 9 note-ons, 6 cancelled at once: clicks and wasted voices)
    for (int r = 0; r < ROWS; r++) {
        if (!(m >> r & 1)) continue;
        int run = 1;   // TIE: a dot right after one on the same row continues it; the first holds to the run's end
        if (tie) {
            // 2.4.14: the middle of a run the playhead is going through (it came from the run's neighbour) is held, not struck
            if (backward) {
                if (s + 1 < run_end && (mask[s + 1].load(std::memory_order_relaxed) >> r & 1)) continue;
                while (s - run >= run_start && (mask[s - run].load(std::memory_order_relaxed) >> r & 1)) run++;   // back to its start
            } else {
                if (prev == s - 1 && s - 1 >= run_start && (mask[s - 1].load(std::memory_order_relaxed) >> r & 1)) continue;
                while (s + run < run_end && (mask[s + run].load(std::memory_order_relaxed) >> r & 1)) run++;
            }
        }
        const int ni = s * ROWS + r;
        // 2.4.2: CHANCE (the note may skip this time round)
        const int skip = nskip[ni].load(std::memory_order_relaxed);
        if (skip) {
            rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
            if (int(rng_ % 100u) < skip) continue;
        }
        const int ng = ngate[ni].load(std::memory_order_relaxed);
        const double g = ng ? std::max(0.05, std::min(1.0, ng / 100.0)) : gate;
        int64_t dur = int64_t(step_frames * (run - 1 + g)) - 32;
        if (nslide[ni].load(std::memory_order_relaxed)) dur = int64_t(step_frames * (run + 0.12));   // SLIDE: into the next
        int got[128], gn = 0;
        if (mode == 0) { got[0] = seq_note(key, r, scale, root, base); gn = 1; }
        // 2.4.11: one key held: the rows walk the scale from it (as SEQ) - a drawn or GENERATEd melody stays a melody
        // (it was the key in octaves: "everything on one note"); two or more: the r-th key, an octave up past the last
        else if (ord == O_PATTERN) { got[0] = pn == 1 ? seq_note(pl[0], r, scale, root, base) : arp_note(pl, pn, r); gn = 1; }
        else {
            if (ord == O_CHORD && chord_done) continue;
            gn = next_ordered(pl, pn, got);
            chord_done = ord == O_CHORD;
        }
        const int shift = 12 * (std::max(-3, std::min(3, settings.octave.load())) + std::max(-2, std::min(2, int(noct[ni].load(std::memory_order_relaxed)))));
        for (int g = 0; g < gn; g++) got[g] = got[g] < 0 || got[g] + shift < 0 || got[g] + shift > 127 ? -1 : got[g] + shift;
        const int nv = nvel[ni].load(std::memory_order_relaxed);
        const int rt = std::max(1, std::min(4, int(nratch[ni].load(std::memory_order_relaxed))));
        for (int g = 0; g < gn && nn < 160; g++) {
            if (got[g] < 0) continue;
            int src = mode == 0 ? key : ord == O_PATTERN ? pl[r % pn] : pl[0];   // the key this note came from
            if (mode != 0 && ord != O_PATTERN) {   // (any octave of it)
                for (int i = 0; i < pn; i++) if ((got[g] - shift - pl[i]) % 12 == 0 && got[g] - shift >= pl[i]) { src = pl[i]; break; }
            }
            int v = vels[src];
            v = v * sv / 100;
            if (nv) v = v * nv / 100;
            const float hv = settings.human_vel.load();   // 2.4.2 HUMANIZE velocity
            if (hv > 0) v = int(v * (1.0f + hv * 0.4f * (float(rnd() % 2001) / 1000.0f - 1.0f)));
            ns[nn++] = {got[g], std::max(1, std::min(127, v)), chs[src], dur, rt};
        }
    }
    if (nn == 0) return;
    // STRUM: the step's notes one after another over up to a step, low to high / high to low / alternating
    const float strum = std::max(0.0f, std::min(1.0f, settings.strum.load()));
    if (strum > 0 && nn > 1) {
        std::sort(ns, ns + nn, [](const N &a, const N &b) { return a.note < b.note; });
        const int dir = settings.strum_dir.load();
        bool down = dir == 1 || (dir == 2 && strum_flip_);
        if (dir == 2) strum_flip_ = !strum_flip_;
        if (down) std::reverse(ns, ns + nn);
    }
    const double spread = strum > 0 && nn > 1 ? step_frames * strum : 0;
    const float ht = settings.human_time.load();   // 2.4.2 HUMANIZE time: the step's notes a little late, together
    const int64_t late = ht > 0 ? int64_t(step_frames * 0.25 * ht * double(rnd() % 1001) / 1000.0) : 0;
    for (int i = 0; i < nn; i++) {
        const int64_t wait = (nn > 1 ? int64_t(spread * i / (nn - 1)) : 0) + late;
        const int64_t dur = std::max<int64_t>(64, ns[i].dur - wait);   // strummed notes end together
        // RATCHET: the note struck 2-4 times in its first step; the last strike holds on through a tied run
        const int rt = ns[i].ratch;
        const double hit = step_frames / rt;
        for (int h = 0; h < rt; h++) {
            const int64_t w = wait + int64_t(hit * h);
            const int64_t d = h + 1 < rt ? std::max<int64_t>(64, int64_t(hit * std::max(0.3, gate)) - 16) : std::max<int64_t>(64, dur - int64_t(hit * h));
            if (w <= 0) sound(out, ns[i].note, ns[i].chan, ns[i].v, d);
            else if (pend_n_ < 64) pend_[pend_n_++] = {ns[i].note, ns[i].chan, ns[i].v, w, d};
        }
    }
}

int Arp::process(Out &out, int frames) {
    const bool on = settings.on.load() != 0;
    if (!on) { if (busy()) reset(out); return frames; }   // turned off: its notes end, its keys are forgotten
    if (!settings.latch.load() && latched_any_) { std::fill(latched_, latched_ + 128, 0); latched_any_ = false; }   // LATCH off
    int pl[128];
    const int pn = pool(pl);
    const bool keys = pn > 0;
    if (!keys) {
        if (running_) stop(out);
        // notes still in their gate play out
    } else if (!running_) {
        running_ = true;
        if (!host_) start_free();
    }
    if (keys) {   // the key row 1 follows now, for the skin's row names
        int k = pl[0];
        if (settings.mode.load() == 0)
            for (int i = 0, best = -1; i < pn; i++) if (order_[pl[i]] > best) { best = order_[pl[i]]; k = pl[i]; }
        ref_note.store(k);
    }
    // strummed notes due now, then the notes whose gate ends now
    for (int i = 0; i < pend_n_;)
        if (pend_[i].wait <= 0) { sound(out, pend_[i].note, pend_[i].chan, pend_[i].vel, pend_[i].dur); pend_[i] = pend_[--pend_n_]; } else i++;
    for (int i = 0; i < snd_n_;)
        if (snd_[i].left <= 0) { out.arp_off_ch(snd_[i].chan, snd_[i].note); snd_[i] = snd_[--snd_n_]; } else i++;
    const double fpb = 60.0 / bpm_ * SR;   // frames per beat
    if (running_) {
        // the steps due now (a few at most; after a stall, skip to the current one)
        int guard = 0;
        while (step_time(next_k_) <= pos_ + 0.5 / fpb && guard++ < 4) play_step(out, next_k_++);
        if (step_time(next_k_) <= pos_) next_k_ = long(std::ceil(pos_ / beats_per_step()));
    }
    for (int i = 0; i < pend_n_;)   // (a strum with no wait left at all)
        if (pend_[i].wait <= 0) { sound(out, pend_[i].note, pend_[i].chan, pend_[i].vel, pend_[i].dur); pend_[i] = pend_[--pend_n_]; } else i++;
    // frames to the next event: the next step, a strummed note, or a gate ending
    int64_t n = frames;
    if (running_) n = std::min<int64_t>(n, int64_t(std::ceil((step_time(next_k_) - pos_) * fpb)));
    for (int i = 0; i < snd_n_; i++) n = std::min<int64_t>(n, snd_[i].left);
    for (int i = 0; i < pend_n_; i++) n = std::min<int64_t>(n, pend_[i].wait);
    return int(std::max<int64_t>(1, std::min<int64_t>(frames, n)));
}

void Arp::advance(int frames) {
    pos_ += double(frames) * bpm_ / 60.0 / SR;
    for (int i = 0; i < snd_n_; i++) snd_[i].left -= frames;
    for (int i = 0; i < pend_n_; i++) pend_[i].wait -= frames;
}

}  // namespace omni
