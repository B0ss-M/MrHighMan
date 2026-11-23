// Omni Sampler: voices, note handling and rendering (see sampler.hpp).
#include "sampler.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace omni {

static inline double clampd_(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

void Program::index() {
    by_key.assign(128, {});
    rr.assign(inst.zones.size(), 0);
    for (size_t i = 0; i < inst.zones.size(); i++) {
        const Zone &z = inst.zones[i];
        if (!pcm[i] || pcm[i]->frames() == 0) continue;
        for (int k = z.key_lo; k <= z.key_hi && k < 128; k++) by_key[size_t(k)].push_back(int(i));
    }
}

Sampler::Sampler() {
    arp_sink_.s = this;   // 2.4
    for (int i = 0; i < 4; i++) { settings.slot_hi[i].store(127); settings.slot_gain[i].store(1); }
    mixl_.assign(4096, 0);
    mixr_.assign(4096, 0);
    pvl_.assign(4096, 0);
    pvr_.assign(4096, 0);
    fxb_.prepare(SR);   // 2.4.30: allocates here only
    fxb_.select(0);
    make_fx_expansions(fxx_);   // 2.4.33: the bay's expansion slots (empty ones stay nullptr); 2.4.35: B / C share a rack
    for (int k = 0; k < 3; k++)
        if (fxx_[k]) fxx_[k]->prepare(SR, int(mixl_.size()));
}

Sampler::~Sampler() {
    delete prog_;
    delete prev_;
    delete prev_req_.exchange(nullptr);
    delete prev_retired_.exchange(nullptr);
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
}

bool Sampler::set_program(Program *p) {
    Program *expected = nullptr;
    return pending_.compare_exchange_strong(expected, p);
}

Program *Sampler::take_retired() { return retired_.exchange(nullptr); }

// ---------------------------------------------------------------------------------------------------------------
// MIDI

void Sampler::midi(const uint8_t *m, int len) {
    if (len < 1) return;
    // events come on the audio thread just before render: a program that arrived meanwhile takes this note
    if (pending_.load() && !swap_fade_) swap_program();
    int status = m[0] & 0xF0, chan = m[0] & 0x0F;
    int d1 = len > 1 ? m[1] & 0x7F : 0, d2 = len > 2 ? m[2] & 0x7F : 0;
    switch (status) {
    case 0x90:
        if (arp.settings.on.load()) { if (d2) arp.key_on(d1, curve_vel(d2), chan); else arp.key_off(d1, chan); break; }   // 2.4: the ARP takes the keys
        if (d2) { d2 = curve_vel(d2); note_on(chan, d1, d2); synth.note_on(d1, d2); } else { note_off(chan, d1); synth.note_off(d1); }
        break;
    case 0x80:
        if (arp.settings.on.load()) { arp.key_off(d1, chan); }
        note_off(chan, d1); synth.note_off(d1);   // (also ends a note held from before the ARP was turned on)
        break;
    case 0xD0: at_ = d1 / 127.0f; synth.set_at(at_); break;   // 2.3.3: the synth's MATRIX reads them too
    case 0xA0: at_ = d2 / 127.0f; synth.set_at(at_); break;
    case 0xE0: bend_ = float((d2 << 7 | d1) - 8192) / 8192.0f; synth.set_bend(bend_); break;
    case 0xB0:
        cc_val_[d1 & 127] = uint8_t(d2 & 127);
        switch (d1) {
        case 1: modwheel_ = d2 / 127.0f; synth.set_wheel(modwheel_); break;
        case 7: cc7_ = d2 / 127.0f; break;
        case 10: cc10_ = (d2 - 64) / 64.0f; break;
        case 11: cc11_ = d2 / 127.0f; break;
        case 64:
            sustain_ = d2 >= 64;
            synth.sustain(sustain_);   // 2.3
            if (!sustain_)
                for (auto &v : voices_)
                    if (v.on && v.held_by_pedal) { v.held_by_pedal = false; if (!v.one_shot) { v.released = true; v.amp.release(); v.fenv.release(); v.penv.release(); } }
            break;
        case 120: all_off(true); synth.all_off(true); arp.reset(arp_sink_); break;
        case 121: bend_ = 0; modwheel_ = 0; at_ = 0; cc7_ = 1; cc10_ = 0; cc11_ = 1; synth.set_bend(0); synth.set_wheel(0); synth.set_at(0); break;   // 2.4.2: the synth's wheel / aftertouch too
        case 123: case 124: case 125: case 126: case 127:
            for (int c = 0; c < 16; c++) for (int n = 0; n < 128; n++) if (held_[c][n]) note_off(c, n);
            all_off(false);
            synth.all_off(false);
            break;
        }
        break;
    default: break;
    }
}

Sampler::Voice *Sampler::alloc_voice() {
    int limit = settings.polyphony.load();
    if (prog_ && prog_->inst.polyphony > 0 && prog_->inst.polyphony < limit) limit = prog_->inst.polyphony;
    if (limit < 1) limit = 1;
    if (limit > MAX_VOICES - 8) limit = MAX_VOICES - 8;   // keep slots for the fade-out tails of stolen voices
    int sounding = 0;
    Voice *oldest = nullptr, *oldest_released = nullptr;
    for (auto &v : voices_) {
        if (!v.on || v.fading) continue;
        sounding++;
        if (!oldest || v.age < oldest->age) oldest = &v;
        if (v.released && (!oldest_released || v.age < oldest_released->age)) oldest_released = &v;
    }
    if (sounding >= limit) {
        Voice *victim = oldest_released ? oldest_released : oldest;
        if (victim) { victim->fading = true; victim->amp.fast_release(0.004f); }
    }
    for (auto &v : voices_) if (!v.on) return &v;
    // no free slot at all: take the oldest fading or oldest voice (hard cut)
    Voice *best = nullptr;
    for (auto &v : voices_) if (!best || (v.fading && !best->fading) || (v.fading == best->fading && v.age < best->age)) best = &v;
    return best;
}

void Sampler::preview(Pcm *p) {
    if (!p) { prev_stop_.store(true); return; }
    delete prev_req_.exchange(p);   // one never started (a newer tap came first)
}

void Sampler::retire_preview() {   // audio thread: hand the finished preview back to be freed
    if (!prev_) return;
    delete prev_retired_.exchange(prev_);   // the last one not collected yet (rare): freed here
    prev_ = nullptr;
    previewing.store(0);
}

// PANIC: every voice fades out at once (5 ms, no click), and nothing is left held: keys, pedal, mono state, wheels
void Sampler::do_panic() {
    arp.reset(arp_sink_);   // 2.4
    retire_preview();
    synth.all_off(true);   // 2.3
    for (auto &v : voices_)
        if (v.on) { v.released = true; v.held_by_pedal = false; v.looping = false; v.fading = true; v.amp.fast_release(0.005f); }
    std::memset(held_, 0, sizeof held_);
    held_count_ = 0;
    for (auto &k : key_down) k.store(0);
    sustain_ = false;
    std::memset(cc_val_, 0, sizeof cc_val_);
    last_mono_note_ = -1;
    bend_ = 0; modwheel_ = 0; at_ = 0;
}

// 2.4.32 (user bug: a pop, then the SYNTH / whole output silent until RESET; PANIC did nothing). A NaN or inf that reaches
// the mix stays in every recursive state it passes (filters, FX lines, the EQ's biquads, which nothing cleared) and the
// output is silence from then on. The block's sum is checked once (one add per sample); when it is not finite the block
// is muted, every voice stops and every FX state is cleared, so the next note plays again
void Sampler::heal_nonfinite() {
    all_off(true);
    synth.all_off(true);
    eq_.clear(); comp_.clear(); bus_.clear(); limiter_.clear(); chorus_.clear(); ensemble_.clear(); delay_.clear();
    reverb_.clear(); plate_.clear(); lofi_.clear(); phaser_.clear(); flanger_.clear(); drive_.clear(); fxb_.reset();
    for (auto &x : fxx_) if (x) x->reset();   // 2.4.33
    nonfinite_heals.fetch_add(1, std::memory_order_relaxed);
}

void Sampler::all_off(bool hard) {
    for (auto &v : voices_) {
        if (!v.on) continue;
        if (hard) v.on = false;
        else { v.released = true; v.fading = true; v.amp.fast_release(0.01f); }
    }
    sustain_ = false;
    std::memset(cc_val_, 0, sizeof cc_val_);
}

// 2.4.6: VELOCITY CURVE (SETUP): how hard a key must be struck. Soft: v^0.6 (velocity 40 plays as 63, light
// keyboards / pads reach the loud layers); Hard: v^1.6; Fixed: every note at 127. Once per note-on, before the zones are
// chosen (velocity layers follow it) and before the synth and the ARP get it
int Sampler::curve_vel(int v) const {
    const int c = settings.vel_curve.load(std::memory_order_relaxed);
    if (c == 1 || v <= 0) return v;
    if (c == 3) return 127;
    const double e = c == 0 ? 0.6 : 1.6;
    return std::max(1, std::min(127, int(std::lround(127.0 * std::pow(v / 127.0, e)))));
}

void Sampler::note_on(int chan, int note, int vel) {
    {   // 2.4.35: the bay's expansion slot hears the note (the XV-3080 KEYSYNC FLANGER restarts)
        const int slot = settings.fxb_slot.load();
        if (slot >= 1 && slot <= 3 && fxx_[slot - 1]) fxx_[slot - 1]->note_on();
    }
    for (int i = 0; i < 2; i++)
        if (settings.lfo_retrig[i].load() == 1) { lfo_phase_[i] = 0; gsrc_[MS_LFO1 + i] = 0; }
    last_note.store(note);
    notes_played.fetch_add(1);
    bool others_held = held_count_ > 0;
    if (!held_[chan][note]) { held_[chan][note] = true; held_count_++; key_down[note].fetch_add(1); }
    if (settings.layer_mode.load() == 1) {   // keyswitch notes choose the slot and make no sound
        int ks = settings.ks_base.load();
        if (note >= ks && note < ks + 4) { active_slot.store(note - ks); return; }
    }
    if (!prog_ || swap_fade_) return;
    // 1.8.2: a note lock's fixed velocity (before the zones are chosen: it picks the velocity layer too)
    if (settings.lock[note & 127].on.load() && settings.lock[note & 127].vel.load() > 0) vel = settings.lock[note & 127].vel.load();
    note_vel_[chan & 15][note & 127] = uint8_t(std::max(1, std::min(127, vel)));   // 2.4.27

    int mode = settings.voice_mode.load();
    float glide_semis = 0;
    if (mode != 0) {
        if (last_mono_note_ >= 0 && settings.glide_s.load() > 0.0005f && (mode == 2 ? others_held : true))
            glide_semis = float(last_mono_note_ - note);
        for (auto &v : voices_) if (v.on && !v.released) { v.released = true; v.fading = true; v.amp.fast_release(mode == 2 && others_held ? 0.03f : 0.008f); }
    }
    last_mono_note_ = note;

    rand_ = rand_ * 1664525u + 1013904223u;
    double rnd = double(rand_ >> 8) / double(1u << 24);
    note_age0_ = age_counter_;
    bool shown = false;
    // 1.7.3: each slot answers at its key shift: the zones mapped at note - shift (one pass per distinct shift)
    for (int si = 0; si < 4; si++) {
    int shift = settings.slot_shift[si].load(), zn = note - shift;
    bool done = false;
    for (int sj = 0; sj < si; sj++) if (settings.slot_shift[sj].load() == shift) done = true;
    if (done || zn < 0 || zn > 127) continue;
    for (int zi : prog_->by_key[size_t(zn)]) {
        const Zone &z = prog_->inst.zones[size_t(zi)];
        if (settings.slot_shift[zone_slot(z)].load() != shift) continue;
        if (vel < std::max(1, z.vel_lo) && !(z.vel_lo == 0 && vel >= 1 && z.vel_hi >= vel)) continue;
        if (vel > z.vel_hi) continue;
        if (z.midi_channel >= 0 && z.midi_channel != chan) continue;
        if (z.cc_num >= 0 && (cc_val_[z.cc_num] < z.cc_lo || cc_val_[z.cc_num] > z.cc_hi)) continue;
        if (!slot_plays(z, note)) continue;
        Trigger tr = z.trigger;
        if (tr == Trigger::Release) continue;
        if (tr == Trigger::First && others_held) continue;
        if (tr == Trigger::Legato && !others_held) continue;
        if (z.play_logic == PlayLogic::RoundRobin) {
            uint32_t c = prog_->rr[size_t(zi)]++;
            int len = std::max(1, z.seq_length);
            if (int(c % uint32_t(len)) != z.seq_position - 1) continue;
        } else if (z.play_logic == PlayLogic::Random) {
            if (rnd < z.rand_lo || rnd >= z.rand_hi) continue;
        }
        start_voice(zi, chan, note, vel, false, glide_semis, zn);
        if (!shown) {
            shown = true;
            if (last_zone.load() != zi || last_vel.load() != vel || last_zone_prog.load() != prog_->serial) {
                last_zone.store(zi);
                last_vel.store(vel);
                last_zone_prog.store(prog_->serial);
                layer_events.fetch_add(1);
            }
        }
    }
    }
}

void Sampler::note_off(int chan, int note) {
    if (held_[chan][note]) { held_[chan][note] = false; held_count_--; key_down[note].fetch_sub(1); }
    else {
        // the key isn't down on this channel: a note-off on another channel than its note-on (a source that changed
        // channel while the note was held). Release it wherever it is, or the note rings on (a looped sound: forever).
        for (int c = 0; c < 16; c++)
            if (held_[c][note]) { held_[c][note] = false; held_count_--; key_down[note].fetch_sub(1); }
    }
    bool any_chan = true;   // voices on another channel go too, unless this channel's own voice is there
    for (auto &v : voices_) if (v.on && v.note == note && v.chan == chan && !v.released) { any_chan = false; break; }
    int vel = 0;
    for (auto &v : voices_) {
        if (!v.on || v.note != note || v.released) continue;
        // channel-bound zones (a multi's parts) only answer their own channel
        if (v.chan != chan && (!any_chan || v.zone->midi_channel >= 0)) continue;
        vel = std::max(vel, v.vel);
        if (v.one_shot) continue;
        if (sustain_) { v.held_by_pedal = true; continue; }
        v.released = true;
        v.amp.release(); v.fenv.release(); v.penv.release();
    }
    if (!prog_ || swap_fade_) return;
    // release triggers
    if (vel == 0) vel = note_vel_[chan & 15][note & 127] ? note_vel_[chan & 15][note & 127] : 64;   // 2.4.27 (audit #10): its own velocity

    note_age0_ = age_counter_;
    for (int si = 0; si < 4; si++) {
        int shift = settings.slot_shift[si].load(), zn = note - shift;
        bool done = false;
        for (int sj = 0; sj < si; sj++) if (settings.slot_shift[sj].load() == shift) done = true;
        if (done || zn < 0 || zn > 127) continue;
        for (int zi : prog_->by_key[size_t(zn)]) {
            const Zone &z = prog_->inst.zones[size_t(zi)];
            if (settings.slot_shift[zone_slot(z)].load() != shift) continue;
            if (z.trigger != Trigger::Release || vel < z.vel_lo || vel > z.vel_hi) continue;
            if (z.cc_num >= 0 && (cc_val_[z.cc_num] < z.cc_lo || cc_val_[z.cc_num] > z.cc_hi)) continue;
            if (z.midi_channel >= 0 && z.midi_channel != chan) continue;
            if (!slot_plays(z, note)) continue;
            start_voice(zi, chan, note, vel, true, 0, zn);
        }
    }
    if (settings.voice_mode.load() != 0 && held_count_ > 0) {
        // mono: fall back to the most recent still held note (simple last-note priority)
        // (note_on counts it as held again: undo its key first so the pad light is not left on)
        for (int n = 127; n >= 0; n--)
            if (held_[chan][n]) { held_[chan][n] = false; held_count_--; key_down[n].fetch_sub(1); note_on(chan, n, 100); break; }
    }
}

bool Sampler::slot_plays(const Zone &z, int note) const {
    int s = z.slot < 0 || z.slot > 3 ? 0 : z.slot;
    if (settings.slot_mute[s].load()) return false;
    if (settings.layer_mode.load() == 1) return s == active_slot.load();
    return note >= settings.slot_lo[s].load() && note <= settings.slot_hi[s].load();
}

void Sampler::start_voice(int zi, int chan, int note, int vel, bool release_trigger, float glide_semis, int znote) {
    if (znote < 0) znote = note;
    const Zone &z = prog_->inst.zones[size_t(zi)];
    const Pcm *pcm = prog_->pcm[size_t(zi)].get();
    if (!pcm || pcm->frames() == 0) return;

    // choke groups
    if (z.exclusive_group > 0)
        for (auto &o : voices_) {
            if (!o.on || o.fading) continue;
            int by = o.zone->off_by < 0 ? o.zone->exclusive_group : o.zone->off_by;
            // not the zones this same note started a moment ago (layered zones of one choke group)
            if (by == z.exclusive_group && !(o.note == note && o.age > note_age0_)) { o.fading = true; o.amp.fast_release(0.004f); }
        }

    Voice *vp = alloc_voice();
    if (!vp) return;
    Voice &v = *vp;
    v = Voice();
    v.on = true;
    v.zone = &z;
    v.pcm = pcm;
    v.zi = zi; v.note = note; v.znote = znote; v.vel = vel; v.chan = chan;
    v.age = ++age_counter_;
    int64_t frames = pcm->frames();
    v.start = std::min<int64_t>(std::max<int64_t>(0, z.start), frames - 1);
    v.stop = z.stop > 0 ? std::min<int64_t>(z.stop, frames) : frames;
    if (v.stop <= v.start) v.stop = frames;
    v.one_shot = z.one_shot && !release_trigger;
    // 1.8.2: playback mode and polarity: the note's lock, else its slot's; Rev turns the zone's own direction round
    const Settings::NoteLock &lk = settings.lock[note & 127];
    const bool locked = lk.on.load() != 0;
    const int pm = locked && lk.mode.load() > 0 ? lk.mode.load() - 1 : settings.slot_play[zone_slot(z)].load();
    const int pinv = locked && lk.inv.load() > 0 ? lk.inv.load() - 1 : settings.slot_inv[zone_slot(z)].load();
    const bool rev = z.reverse != (pm == 1);
    // Rev (REV MODE Full, the default) plays the whole sample back to its start whatever the note-off: a pad tap would
    // otherwise cut it in the tail (often quiet, or a sustain loop), before the attack. Attack, decay and sustain shape
    // it; it fades out by itself so the fade ends at the start (render_voice). 2.1.16: REV MODE Gated: the note-off
    // releases it as Fwd's does. The zones a format reverses itself keep their own envelope and release. Not a
    // wavetable: its one cycle loops and never reaches a start
    if (rev && !z.reverse && !release_trigger && !(z.wt_count > 1 && z.wt_size > 1)) {
        v.rev_tail = true;
        v.one_shot = settings.rev_gated.load() == 0;
    }
    if (!z.loops.empty() && !rev && !v.one_shot) {
        const Loop &l = z.loops[0];
        int64_t ls = std::max<int64_t>(v.start, l.start), le = std::min<int64_t>(l.end + 1, v.stop);
        if (le - ls >= 2) {
            v.looping = true;
            v.loop_start = ls; v.loop_end = le;
            v.loop_type = l.type;
            v.loop_until_release = l.until_release;
            v.loop_tune = l.tune;
        }
    }
    rand_ = rand_ * 1664525u + 1013904223u;
    v.rnd = float(double(rand_ >> 8) / double(1u << 23) - 1.0);
    {   // matrix: sample start offset (a share of the playable length, positive amounts only)
        float acc = 0;
        int vs = zone_slot(z);
        for (int k = 0; k < MOD_SLOTS; k++)
            if (slot_dst_[k] == MD_START && slot_amt_[k] != 0 && (slot_layer_[k] < 0 || slot_layer_[k] == vs))
                acc += slot_amt_[k] * src_val(v, slot_src_[k]);
        if (locked) acc += std::max(0.0f, std::min(0.95f, lk.start.load())) / 0.9f;   // the lock's start, a share of the length
        if (acc > 0 && !rev) v.start = std::min(v.stop - 1, v.start + int64_t(std::min(1.0f, acc) * 0.9f * float(v.stop - v.start)));
    }
    v.reverse_ = rev;
    v.dir = rev ? -1 : 1;
    v.pos = rev ? double(v.stop - 1) : double(v.start);
    if ((pm == 2 || pm == 3) && !rev && !release_trigger) {
        // ping-pong: the zone's loop, else the whole sample, back and forth while held (a one-shot then follows the key)
        v.one_shot = false;
        if (!v.looping) { v.loop_start = v.start; v.loop_end = v.stop; v.loop_until_release = false; v.loop_tune = 0; }
        v.loop_start = std::max(v.loop_start, v.start);   // the backward pass must turn before the start
        v.looping = v.loop_end - v.loop_start >= 4;
        v.loop_type = LoopType::Alternating;
        if (pm == 3 && v.looping) {
            const int ch = pcm->channels;
            v.pp_inv = true;
            v.pp_lo = double(v.loop_start);
            v.pp_k = 1.0 / double(v.loop_end - 1 - v.loop_start);
            for (int c = 0; c < 2; c++) {
                const int cc = c < ch ? c : 0;
                float a = pcm->data[size_t(v.loop_start) * size_t(ch) + size_t(cc)], b = pcm->data[size_t(v.loop_end - 1) * size_t(ch) + size_t(cc)];
                v.pp_c[c] = 2 * a;
                v.pp_d[c] = 2 * (b - a);
            }
        }
    }
    v.polarity = pinv ? -1.0f : 1.0f;
    if (locked) {
        v.lock_semis = lk.semis.load();
        v.lock_cut_mul = std::exp2(lk.cut.load());
        v.lock_pan = lk.pan.load();
    }

    // level
    float vel_sens = settings.vel_sens.load();
    double vel01 = vel / 127.0;
    // 2.4.6: x^(3^curve) as model.hpp documents (it was x^(2 * 3^curve): velocity squared by default, x^6 for SF2 / Kontakt,
    // so a medium hit played at -12 / -36 dB and players had to strike hard). Default 0 = linear: velocity 64 = -6 dB.
    double curve = std::pow(vel01, std::pow(3.0, z.amp_vel_curve));
    double depth = clampd_(z.amp_vel_depth * vel_sens, 0, 1);
    double velgain = (1 - depth) + depth * curve;
    double db = z.gain_db + prog_->inst.gain_db + z.amp_key_tracking * (znote - z.root);
    double g = velgain * std::pow(10.0, db / 20.0);
    // crossfades (equal power)
    if (z.key_xfade_lo > 0 && znote < z.key_lo + z.key_xfade_lo) g *= std::sqrt(double(znote - z.key_lo + 1) / (z.key_xfade_lo + 1));
    if (z.key_xfade_hi > 0 && znote > z.key_hi - z.key_xfade_hi) g *= std::sqrt(double(z.key_hi - znote + 1) / (z.key_xfade_hi + 1));
    if (z.vel_xfade_lo > 0 && vel < z.vel_lo + z.vel_xfade_lo) g *= std::sqrt(double(vel - z.vel_lo + 1) / (z.vel_xfade_lo + 1));
    if (z.vel_xfade_hi > 0 && vel > z.vel_hi - z.vel_xfade_hi) g *= std::sqrt(double(z.vel_hi - vel + 1) / (z.vel_xfade_hi + 1));
    v.gain = float(g) * settings.slot_gain[z.slot < 0 || z.slot > 3 ? 0 : z.slot].load();
    if (locked) v.gain *= lk.gain.load();
    float pan = float(clampd_(z.pan, -1, 1));
    v.pan_l = std::cos((pan + 1) * PI_F / 4);
    v.pan_r = std::sin((pan + 1) * PI_F / 4);

    // envelopes
    Envelope ae = z.amp_env;
    if (!ae.set) { ae = Envelope(); ae.release = 0.02; }
    if (ae.release < 0.004) ae.release = 0.004;
    float tvel = float(1.0 - ae.time_vel_tracking * (vel01 - 0.5));   // + tracking shortens towards high velocities
    float tkey = float(std::exp2(-ae.time_key_tracking * (note - 60) / 24.0));
    v.amp.begin(ae, tvel * tkey, tkey, settings.attack_add.load() + settings.d_attack.load(), settings.release_add.load() + settings.d_release.load(),
                settings.sustain_scale.load(), settings.d_sustain.load());
    v.amp.decay_s = std::max(0.0f, v.amp.decay_s * settings.decay_scale.load() + settings.d_decay.load());
    v.amp.attack_s = std::max(0.0f, v.amp.attack_s);              // the faders may take a zone's times below zero
    v.amp.release_s = std::max(0.004f, v.amp.release_s);
    if (v.amp.stage == Env::Attack) v.amp.enter(Env::Attack);   // re-read the changed times
    // 2.4.25 (audit 2026-11-21 #1): a release / one-shot voice holds full level only when its zone has no envelope of
    // its own (drum kits, plain samples: the panel's SUSTAIN must not cut them); a reader's own sustain (SFZ ampeg_sustain,
    // NKI, EXS, Akai ...) is kept, so authored release tails and decaying one-shots decay as written
    if ((release_trigger || (v.one_shot && !v.rev_tail)) && !z.amp_env.set) { v.amp.sustain = 1; }
    if (z.filter.env.set) v.fenv.begin(z.filter.env, 1, 1, 0, 0, 1);
    if (z.pitch_env.set) v.penv.begin(z.pitch_env, 1, 1, 0, 0, 1);
    if (z.amp_lfo.set) v.alfo.begin(z.amp_lfo, v.age);
    if (z.pitch_lfo.set) v.plfo.begin(z.pitch_lfo, v.age + 7);
    else { Lfo vib; vib.wave = LfoWave::Sine; vib.rate_hz = 5.5; v.plfo.begin(vib, v.age + 7); }
    if (z.filter.lfo.set) v.flfo.begin(z.filter.lfo, v.age + 13);
    int mt = int(settings.filter_type.load());
    for (int c = 0; c < 2; c++) {
        v.zf[c].setup(z.filter.type, z.filter.poles);
        v.mf[c].setup(mt == 1 ? FilterType::HighPass : mt == 2 ? FilterType::BandPass : FilterType::LowPass, 2);
    }
    if (glide_semis != 0) {
        v.glide = glide_semis;
        v.glide_inc = -glide_semis / (settings.glide_s.load() * SR);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// rendering

void Sampler::block_mod(Voice &v, int n, double &step, float &cutoff_mul) {
    const Zone &z = *v.zone;
    float pe = z.pitch_env.set ? v.penv.level : 0;
    float pl = v.plfo.advance(n);
    float al = z.amp_lfo.set ? v.alfo.advance(n) : 0;
    float fl = z.filter.lfo.set ? v.flfo.advance(n) : 0;
    (void)al;
    float bend_cents = bend_ >= 0 ? bend_ * float(z.bend_up) : -bend_ * float(z.bend_down);
    float br = settings.bend_range.load();
    if (br > 0) bend_cents = bend_ * br * 100.0f;
    double semis = (v.znote - z.root) * z.key_tracking + z.tune + settings.transpose.load() + settings.tune_cents.load() / 100.0 +
                   settings.slot_tune[z.slot < 0 || z.slot > 3 ? 0 : z.slot].load() +
                   bend_cents / 100.0 + pe * z.pitch_env_depth * (MAX_ENVELOPE_DEPTH / 100.0) +
                   pl * (z.pitch_lfo.set ? z.pitch_lfo_depth * (MAX_ENVELOPE_DEPTH / 100.0) : 0) + pl * modwheel_ * 0.5 + v.glide;
    if (v.glide != 0) {
        v.glide += v.glide_inc * float(n);
        if ((v.glide_inc < 0 && v.glide < 0) || (v.glide_inc > 0 && v.glide > 0)) v.glide = 0;
    }
    // the matrix's per-voice destinations
    float m_pitch = 0, m_cut = 0, m_res = 0, m_vol = 0, m_pan = 0, m_wt = 0;
    int vslot = zone_slot(z);
    for (int k = 0; k < MOD_SLOTS; k++) {
        if (slot_amt_[k] == 0 || slot_src_[k] == MS_OFF) continue;
        if (slot_layer_[k] >= 0 && slot_layer_[k] != vslot) continue;   // a route for another slot
        float x = slot_amt_[k] * src_val(v, slot_src_[k]);
        switch (slot_dst_[k]) {
        case MD_PITCH: m_pitch += x; break;
        case MD_CUTOFF: m_cut += x; break;
        case MD_RES: m_res += x; break;
        case MD_VOL: m_vol += x; break;
        case MD_PAN: m_pan += x; break;
        case MD_WTPOS: m_wt += x; break;
        default: break;
        }
    }
    semis += m_pitch * MOD_PITCH_SEMIS + v.lock_semis;
    if (v.loop_tune != 0 && v.looping && v.pos >= double(v.loop_start) && !(v.released && v.loop_until_release)) semis += v.loop_tune;
    v.mod_cut_mul = m_cut != 0 ? std::exp2(m_cut * MOD_CUTOFF_OCT) : 1.0f;
    v.mod_res = m_res * MOD_RES;
    v.mod_gain = std::max(0.0f, std::min(2.0f, 1.0f + m_vol));
    v.mod_pan = std::max(-1.0f, std::min(1.0f, m_pan));
    v.mod_wt = m_wt;
    step = std::exp2(semis / 12.0) * double(v.pcm->rate) / double(SR);
    float cents = 0;
    if (z.filter.type != FilterType::None) {
        cents += float(z.filter.env_depth * MAX_ENVELOPE_DEPTH) * (z.filter.env.set ? v.fenv.level : 0);
        cents += float(z.filter.vel_depth * 9600.0) * (v.vel / 127.0f);
        cents += float(z.filter.key_tracking * 100.0) * float(v.znote - z.root);
        cents += float(z.filter.lfo_depth * MAX_ENVELOPE_DEPTH) * fl;
        cents += float(z.filter.modwheel_depth * MAX_ENVELOPE_DEPTH) * modwheel_;
    }
    cutoff_mul = std::exp2(cents / 1200.0f) * v.mod_cut_mul;
}

float Sampler::src_val(const Voice &v, int s) const {
    switch (s) {
    case MS_VEL: return v.vel / 127.0f;
    case MS_KEY: return std::max(-1.0f, std::min(1.0f, (v.znote - 60) / 64.0f));
    case MS_RAND: return v.rnd;
    case MS_ENV: return v.amp.level;
    default: return s > MS_OFF && s < MS_COUNT ? gsrc_[s] : 0.0f;
    }
}

void Sampler::transport(double ppq, double tempo, int playing, int ppq_valid) {
    if (tempo > 1) { tempo_ = tempo; synth.set_tempo(tempo); }   // 2.3.2: the synth's LFOs follow it too
    playing_ = playing != 0;
    ppq_valid_ = ppq_valid != 0;
    if (ppq_valid_) ppq_ = ppq;
    arp.transport(ppq, tempo_, playing_, ppq_valid_);   // 2.4
}

// beats (quarter notes) per LFO cycle for each sync setting; 0 = free
// the FX delay's times (beats): 1/32, 1/16 T, 1/16, 1/16 ., 1/8 T, 1/8, 1/8 ., 1/4 T, 1/4, 1/4 ., 1/2, 1 bar
static const double DELAY_BEATS[] = {0.125, 1.0 / 6, 0.25, 0.375, 1.0 / 3, 0.5, 0.75, 2.0 / 3, 1, 1.5, 2, 4};
constexpr int DELAY_COUNT = int(sizeof DELAY_BEATS / sizeof DELAY_BEATS[0]);
static const double SYNC_BEATS[] = {0, 16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3, 1.0 / 3, 1.0 / 6, 1.5, 0.75, 0.375};
constexpr int SYNC_COUNT = int(sizeof SYNC_BEATS / sizeof SYNC_BEATS[0]);

void Sampler::update_mod(int frames) {
    cut_mod_ = false;
    g_drive_add_ = g_rev_add_ = 0;
    for (int k = 0; k < MOD_SLOTS; k++) {
        slot_src_[k] = settings.mod_src[k].load();
        slot_dst_[k] = settings.mod_dst[k].load();
        slot_amt_[k] = settings.mod_amt[k].load();
        int ly = settings.mod_layer[k].load();
        slot_layer_[k] = ly >= 1 && ly <= 4 ? ly - 1 : -1;
        if (slot_src_[k] <= MS_OFF || slot_src_[k] >= MS_COUNT || slot_dst_[k] <= MD_OFF || slot_dst_[k] >= MD_COUNT) slot_amt_[k] = 0;
        if (slot_amt_[k] != 0 && (slot_dst_[k] == MD_CUTOFF || slot_dst_[k] == MD_RES)) cut_mod_ = true;
    }
    gsrc_[MS_WHEEL] = modwheel_;
    gsrc_[MS_AT] = at_;
    gsrc_[MS_BEND] = bend_;
    auto global = [](int s) { return s == MS_LFO1 || s == MS_LFO2 || s == MS_WHEEL || s == MS_AT || s == MS_BEND; };
    for (int i = 0; i < 2; i++) {
        float rmod = 0;
        for (int k = 0; k < MOD_SLOTS; k++)
            if (slot_dst_[k] == MD_LFO1_RATE + i && slot_amt_[k] != 0 && global(slot_src_[k])) rmod += slot_amt_[k] * gsrc_[slot_src_[k]];
        int sync = settings.lfo_sync[i].load();
        double beats = sync > 0 && sync < SYNC_COUNT ? SYNC_BEATS[sync] : 0;
        double hz = beats > 0 ? tempo_ / 60.0 / beats : std::max(0.001f, settings.lfo_rate[i].load());
        hz *= std::exp2(double(rmod) * MOD_RATE_OCT);
        double prev = lfo_phase_[i];
        if (beats > 0 && playing_ && ppq_valid_ && rmod == 0 && settings.lfo_retrig[i].load() == 0) {
            double q = ppq_ / beats;                        // locked to the song position (no fmod: glibc 2.38)
            lfo_phase_[i] = q - std::floor(q);
        } else {
            lfo_phase_[i] += hz * frames / SR;
            lfo_phase_[i] -= std::floor(lfo_phase_[i]);
        }
        if (lfo_phase_[i] < prev || lfo_sh_[i] == 0) {   // a new cycle: the next sample & hold value
            rand_ = rand_ * 1664525u + 1013904223u;
            lfo_sh_[i] = float(double(rand_ >> 8) / double(1u << 23) - 1.0);
            if (lfo_sh_[i] == 0) lfo_sh_[i] = 1e-6f;
        }
        float p = float(lfo_phase_[i]), val;
        switch (settings.lfo_wave[i].load()) {
        case 1: val = p < 0.5f ? 4 * p - 1 : 3 - 4 * p; break;
        case 2: val = 2 * p - 1; break;
        case 3: val = 1 - 2 * p; break;
        case 4: val = p < 0.5f ? 1.0f : -1.0f; break;
        case 5: val = lfo_sh_[i]; break;
        default: val = std::sin(2 * PI_F * p); break;
        }
        gsrc_[MS_LFO1 + i] = val;
        lfo_out[i].store(val);
    }
    for (int k = 0; k < MOD_SLOTS; k++) {
        if (slot_amt_[k] == 0 || !global(slot_src_[k])) continue;
        float x = slot_amt_[k] * gsrc_[slot_src_[k]];
        if (slot_dst_[k] == MD_DRIVE) g_drive_add_ += x;
        else if (slot_dst_[k] == MD_REVERB) g_rev_add_ += x;
    }
    if (playing_ && ppq_valid_) ppq_ += double(frames) / SR * tempo_ / 60.0;
}

// 2.1.1: SINC, 8-point Hann-windowed sinc (after discoDSP's SFZero v3 akaiSincInterpolate, MIT): taps i0-3 .. i0+4,
// the cutoff lowered to 1 / step when the sample plays faster than the output (no aliasing when pitched up). SFZero
// computes 16 sin/cos per sample; here they are a table: 9 cutoffs (2^(-j/4), one octave and a quarter per two
// steps, to 4x speed) x 257 phases x 8 taps, each phase normalised to unity DC gain, linearly interpolated by phase.
namespace {
constexpr int SINC_PHASES = 256, SINC_CUTS = 9;
struct SincTable {
    float c[SINC_CUTS][SINC_PHASES + 1][8];
    SincTable() {
        const double pi = 3.14159265358979323846;
        for (int j = 0; j < SINC_CUTS; j++) {
            const double fc = std::exp2(-j / 4.0);
            for (int ph = 0; ph <= SINC_PHASES; ph++) {
                const double frac = double(ph) / SINC_PHASES;
                double w[8], sum = 0;
                for (int q = 0; q < 8; q++) {
                    const double x = double(q - 3) - frac;
                    const double win = 0.5 + 0.5 * std::cos(x / 4.0 * pi);
                    const double s = std::fabs(x) < 1e-12 ? fc : std::sin(fc * pi * x) / (pi * x);
                    sum += (w[q] = s * win);
                }
                for (int q = 0; q < 8; q++) c[j][ph][q] = float(w[q] / sum);
            }
        }
    }
};
const SincTable &sinc_table() { static const SincTable t; return t; }
// the cutoff for a playback step (frames of the sample per output frame)
const float (*sinc_for(double step))[8] {
    int j = step > 1.0 ? int(std::lround(4.0 * std::log2(step))) : 0;
    return sinc_table().c[j < 0 ? 0 : j >= SINC_CUTS ? SINC_CUTS - 1 : j];
}
inline void sinc_coefs(const float (*tab)[8], float t, float *k) {
    float f = t * float(SINC_PHASES);
    int ph = int(f);
    if (ph >= SINC_PHASES) ph = SINC_PHASES - 1;
    if (ph < 0) ph = 0;
    const float u = f - float(ph), *a = tab[ph], *b = tab[ph + 1];
    for (int q = 0; q < 8; q++) k[q] = a[q] + (b[q] - a[q]) * u;
}
}  // namespace

static inline float hermite(float xm1, float x0, float x1, float x2, float t) {
    float c = (x1 - xm1) * 0.5f;
    float v = x0 - x1;
    float w = c + v;
    float a = w + v + (x2 - x0) * 0.5f;
    float b = w + a;
    return ((a * t - b) * t + c) * t + x0;
}

void Sampler::render_voice(Voice &v, float *outl, float *outr, int n) {
    const Zone &z = *v.zone;
    // 2.4.27 (velocity audit #1, CPU): a zone with its own envelope that has decayed to a silent sustain ends here (it
    // rendered silently and held a voice until key-up: 36 % of TRITON zones are looped with sustain 0). Once per block.
    if (!z.seq_step && z.amp_env.set && v.amp.stage == Env::Sustain && v.amp.level <= 1e-5f) { v.on = false; return; }
    if (z.seq_step) {
        // a wave sequence step: silent and in place until its time comes, gone once its crossfade out is done
        if (v.amp.stage == Env::Delay) {
            for (int i = 0; i < n && v.amp.stage == Env::Delay; i++) v.amp.tick();
            if (v.amp.stage == Env::Delay) return;
        } else if (v.amp.stage == Env::Sustain && v.amp.level <= 0) {
            v.on = false;
            return;
        }
    }
    double step;
    float cutoff_mul;
    block_mod(v, n, step, cutoff_mul);
    if (v.rev_tail && !v.rev_faded && step > 0) {
        // Rev: the fade starts when the time left back to the start is the release time, at most half the reversed
        // sample (2.1.16: a release longer than the sample used to start the fade at the note-on, before the attack, and
        // the note was silent). A note-off's release (Gated) that would outlast the sample is cut to end at the start
        const double left_s = (v.pos - double(v.start)) / step / SR;
        const float fade = std::max(0.004f, std::min(v.amp.release_s, float(0.5 * double(v.stop - v.start) / step / SR)));
        if (!v.released && left_s <= fade) {
            v.rev_faded = v.released = true;
            v.amp.release_s = fade;
            v.amp.release(); v.fenv.release(); v.penv.release();
        } else if (v.released && v.amp.stage == Env::Release && v.amp.inc > 0
                   && (1.0 - v.amp.t) / v.amp.inc / SR > left_s && left_s <= fade) {
            v.rev_faded = true;
            v.amp.release_s = float(std::max(0.004, left_s));
            v.amp.enter(Env::Release);
        }
    }
    const int ch = v.pcm->channels;
    const int nch = ch > 1 ? 2 : 1;
    if (v.zf[0].on) {   // 2.4.7: computed once, the other channel copies (it was set per channel: the same tan / exp twice)
        v.zf[0].set(float(z.filter.cutoff) * cutoff_mul, std::max(0.0f, std::min(1.0f, float(z.filter.resonance) + v.mod_res)));
        for (int c = 1; c < nch; c++) v.zf[c].copy_coefs(v.zf[0]);
    }
    float mc = settings.cutoff_hz.load(), mr = settings.resonance.load();
    float fv = settings.filter_vel.load();
    if (fv > 0) mc *= std::exp2(-4.0f * fv * (1.0f - v.vel / 127.0f));
    mc *= v.mod_cut_mul * v.lock_cut_mul;
    mr = std::max(0.0f, std::min(1.0f, mr + v.mod_res));
    bool master_on = !(int(settings.filter_type.load()) == 0 && mc >= 19500 && mr < 0.01f) || cut_mod_;
    if (master_on) { v.mf[0].set(mc, mr); for (int c = 1; c < nch; c++) v.mf[c].copy_coefs(v.mf[0]); }   // 2.4.7: once
    float amp_lfo_gain = 1;
    if (z.amp_lfo.set && z.amp_lfo_depth != 0)
        amp_lfo_gain = std::pow(10.0f, float(z.amp_lfo_depth * MAX_VOLUME_DEPTH) * v.alfo.value / 20.0f);
    const int interp = settings.interpolation.load();
    const float (*sinc)[8] = interp == 3 ? sinc_for(step) : nullptr;   // 2.1.1
    const int mlo = interp == 3 ? 3 : 1, mhi = interp == 3 ? 5 : 3;     // taps before / after a fast-path read
    const int16_t *d = v.pcm->data.data();
    // the matrix's pan and the slot's (1.8) as one balance, read per block so a turned knob moves held notes too
    const float bal = std::max(-1.0f, std::min(1.0f, v.mod_pan + v.lock_pan + settings.slot_pan[z.slot < 0 || z.slot > 3 ? 0 : z.slot].load()));
    const float bal_l = bal > 0 ? 1 - bal : 1, bal_r = bal < 0 ? 1 + bal : 1;
    const float gl = v.polarity * v.gain * v.mod_gain * bal_l * v.pan_l * amp_lfo_gain * (1.0f / 32768.0f),
                gr = v.polarity * v.gain * v.mod_gain * bal_r * v.pan_r * amp_lfo_gain * (1.0f / 32768.0f);
    const bool zf_on = v.zf[0].on, fenv_on = z.filter.env.set, penv_on = z.pitch_env.set;
    // (the cycles that fit the audio: a preset's wt_count past a shorter sample read beyond its data)
    const int64_t wt_fit = z.wt_count > 1 && z.wt_size > 1 ? v.pcm->frames() / z.wt_size : 0;
    if (wt_fit >= 2) {
        // A wavetable: loop one cycle of wt_size frames (v.pos is the phase) and crossfade between the two cycles either
        // side of the position (the WT POSITION knob plus the matrix), set per sub-block
        const int N = z.wt_size, C = int(std::min<int64_t>(z.wt_count, wt_fit));
        float wp = std::max(0.0f, std::min(1.0f, settings.wt_pos.load() + v.mod_wt)) * float(C - 1);
        int fa = std::min(C - 2, int(wp));
        float fr = wp - float(fa);
        const int16_t *A = d + size_t(fa) * size_t(N) * size_t(ch), *B = A + size_t(N) * size_t(ch);
        double pos = v.pos;
        if (pos >= double(N)) pos -= double(N) * std::floor(pos / double(N));   // a sample start offset: into the cycle
        for (int i = 0; i < n; i++) {
            if (fenv_on) v.fenv.tick();
            if (penv_on) v.penv.tick();
            int i0 = int(pos);
            if (i0 >= N) i0 = N - 1;
            int i1 = i0 + 1 == N ? 0 : i0 + 1;
            float t = float(pos - double(i0));
            float a0 = A[i0 * ch], a1 = A[i1 * ch], b0 = B[i0 * ch], b1 = B[i1 * ch];
            float a = a0 + (a1 - a0) * t, b = b0 + (b1 - b0) * t, sl = a + (b - a) * fr, sr = sl;
            if (ch > 1) {
                a0 = A[i0 * ch + 1]; a1 = A[i1 * ch + 1]; b0 = B[i0 * ch + 1]; b1 = B[i1 * ch + 1];
                a = a0 + (a1 - a0) * t; b = b0 + (b1 - b0) * t; sr = a + (b - a) * fr;
            }
            pos += step;
            if (pos >= double(N)) pos -= double(N) * std::floor(pos / double(N));
            float g = v.amp.tick();
            if (v.amp.done()) { v.on = false; break; }
            if (zf_on) { sl = v.zf[0].tick(sl); sr = ch > 1 ? v.zf[1].tick(sr) : sl; }
            if (master_on) { sl = v.mf[0].tick(sl); sr = ch > 1 ? v.mf[1].tick(sr) : sl; }
            outl[i] += sl * g * gl;
            outr[i] += sr * g * gr;
        }
        v.pos = pos;
        return;
    }
    for (int i = 0; i < n; i++) {
        // Fast path: a run that can reach no loop point, end or start this block, forwards or (1.8) backwards
        // (backward loops, the backward half of alternating loops, reversed samples). It works in 32-bit indices: on
        // ARMv7 every double <-> int64 conversion of the generic path below is a library call.
        {
            bool loop_active = v.looping && !(v.released && v.loop_until_release);
            double pos = v.pos, lo, hi;
            int k = 0;
            if (v.dir > 0) {
                int64_t lim = loop_active ? std::min(v.loop_end - 1, v.stop) : v.stop;   // first index the run may not read
                hi = double(lim - mhi); lo = double(v.start + mlo);
                if (pos >= lo && pos <= hi) k = int((hi - pos) / step) + 1;
            } else if (loop_active || v.reverse_) {   // released out of a backward loop: the generic path turns it round
                // reads p[i0 - 1 .. i0 + 2] (SINC: i0 - 3 .. i0 + 4): keep them inside the loop (while it loops) or the sample
                int64_t bottom = loop_active ? std::max(v.loop_start, v.start) : v.start;
                int64_t top = loop_active ? std::min(v.loop_end, v.stop) : v.stop;
                lo = double(bottom + mlo); hi = double(top - mhi);
                if (pos >= lo && pos <= hi) k = int((pos - lo) / step) + 1;
            }
            const double dstep = v.dir > 0 ? step : -step;
            const bool ppi = v.pp_inv && v.dir < 0;   // 1.8.2: the inverted backward pass of a ping-pong
            if (k > 0) {
                if (k > n - i) k = n - i;
                for (int e = i + k; i < e; i++) {
                    if (fenv_on) v.fenv.tick();
                    if (penv_on) v.penv.tick();
                    int i0 = int(pos);
                    float t = float(pos - double(i0));
                    float sl, sr;
                    if (interp == 2) {
                        const int16_t *p = d + (i0 - 1) * ch;
                        sl = hermite(p[0], p[ch], p[2 * ch], p[3 * ch], t);
                        sr = ch > 1 ? hermite(p[1], p[ch + 1], p[2 * ch + 1], p[3 * ch + 1], t) : sl;
                    } else if (interp == 1) {
                        const int16_t *p = d + i0 * ch;
                        sl = float(p[0]) + float(p[ch] - p[0]) * t;
                        sr = ch > 1 ? float(p[1]) + float(p[ch + 1] - p[1]) * t : sl;
                    } else if (interp == 3) {
                        float k8[8];
                        sinc_coefs(sinc, t, k8);
                        const int16_t *p = d + (i0 - 3) * ch;
                        float a = 0, b = 0;
                        if (ch > 1) for (int q = 0; q < 8; q++) { a += k8[q] * float(p[q * ch]); b += k8[q] * float(p[q * ch + 1]); }
                        else for (int q = 0; q < 8; q++) a += k8[q] * float(p[q]);
                        sl = a;
                        sr = ch > 1 ? b : sl;
                    } else {
                        const int16_t *p = d + i0 * ch;
                        sl = p[0];
                        sr = ch > 1 ? float(p[1]) : sl;
                    }
                    if (ppi) {
                        float u = float((pos - v.pp_lo) * v.pp_k);
                        sl = v.pp_c[0] + v.pp_d[0] * u - sl;
                        sr = v.pp_c[1] + v.pp_d[1] * u - sr;
                    }
                    pos += dstep;
                    float a = v.amp.tick();
                    if (v.amp.done()) { v.on = false; v.pos = pos; return; }
                    if (zf_on) { sl = v.zf[0].tick(sl); sr = ch > 1 ? v.zf[1].tick(sr) : sl; }
                    if (master_on) { sl = v.mf[0].tick(sl); sr = ch > 1 ? v.mf[1].tick(sr) : sl; }
                    outl[i] += sl * a * gl;
                    outr[i] += sr * a * gr;
                }
                v.pos = pos;
                if (i >= n) break;
            }
        }
        if (fenv_on) v.fenv.tick();
        if (penv_on) v.penv.tick();
        // loop / end handling (the generic path: one sample near a loop point, the ends, or playing backwards)
        if (v.looping && !(v.released && v.loop_until_release)) {
            if (v.loop_type == LoopType::Forward) {
                if (v.dir > 0 && v.pos >= double(v.loop_end)) v.pos -= double(v.loop_end - v.loop_start);
            } else if (v.loop_type == LoopType::Backward) {
                if (v.dir > 0 && v.pos >= double(v.loop_end)) { v.pos = double(v.loop_end) - (v.pos - double(v.loop_end)) - 1; v.dir = -1; }
                else if (v.dir < 0 && v.pos < double(v.loop_start)) v.pos += double(v.loop_end - v.loop_start);
            } else {
                if (v.dir > 0 && v.pos >= double(v.loop_end - 1)) { v.pos = 2.0 * double(v.loop_end - 1) - v.pos; v.dir = -1; }
                else if (v.dir < 0 && v.pos < double(v.loop_start)) { v.pos = 2.0 * double(v.loop_start) - v.pos; v.dir = 1; }
            }
        } else if (v.looping && v.dir < 0 && !v.reverse_) {
            v.dir = 1;   // released out of a backward loop: play on forwards to the end
        }
        if ((v.dir > 0 && v.pos >= double(v.stop)) || (v.dir < 0 && v.pos < double(v.start))) { v.on = false; break; }

        int64_t i0 = int64_t(v.pos);
        float t = float(v.pos - double(i0));
        float sl, sr;
        auto fetch = [&](int64_t k, int c) -> float {
            if (v.looping && !(v.released && v.loop_until_release) && v.loop_type == LoopType::Forward && v.dir > 0 && k >= v.loop_end)
                k = v.loop_start + (k - v.loop_end) % (v.loop_end - v.loop_start);
            if (k < v.start || k >= v.stop) return 0.0f;
            return float(d[size_t(k) * ch + c]);
        };
        if (interp == 0) {
            sl = float(d[size_t(i0) * ch]);
            sr = ch > 1 ? float(d[size_t(i0) * ch + 1]) : sl;
        } else if (interp == 1) {
            float a = fetch(i0, 0), b = fetch(i0 + 1, 0);
            sl = a + (b - a) * t;
            if (ch > 1) { float a2 = fetch(i0, 1), b2 = fetch(i0 + 1, 1); sr = a2 + (b2 - a2) * t; } else sr = sl;
        } else if (interp == 3) {   // 2.1.1: near a loop point or an end, each tap through fetch (wraps, 0 outside)
            float k8[8];
            sinc_coefs(sinc, t, k8);
            float a = 0, b = 0;
            for (int q = 0; q < 8; q++) {
                a += k8[q] * fetch(i0 - 3 + q, 0);
                if (ch > 1) b += k8[q] * fetch(i0 - 3 + q, 1);
            }
            sl = a;
            sr = ch > 1 ? b : sl;
        } else {
            if (i0 - 1 >= v.start && i0 + 2 < (v.looping ? std::min(v.loop_end, v.stop) : v.stop)) {
                const int16_t *p = d + size_t(i0 - 1) * ch;
                sl = hermite(p[0], p[ch], p[2 * ch], p[3 * ch], t);
                sr = ch > 1 ? hermite(p[1], p[ch + 1], p[2 * ch + 1], p[3 * ch + 1], t) : sl;
            } else {
                sl = hermite(fetch(i0 - 1, 0), fetch(i0, 0), fetch(i0 + 1, 0), fetch(i0 + 2, 0), t);
                sr = ch > 1 ? hermite(fetch(i0 - 1, 1), fetch(i0, 1), fetch(i0 + 1, 1), fetch(i0 + 2, 1), t) : sl;
            }
        }
        if (v.pp_inv && v.dir < 0 && v.looping && !(v.released && v.loop_until_release)) {
            float u = float((v.pos - v.pp_lo) * v.pp_k);
            sl = v.pp_c[0] + v.pp_d[0] * u - sl;
            sr = v.pp_c[1] + v.pp_d[1] * u - sr;
        }
        v.pos += step * v.dir;

        float a = v.amp.tick();
        if (v.amp.done()) { v.on = false; break; }
        if (v.zf[0].on) { sl = v.zf[0].tick(sl); sr = ch > 1 ? v.zf[1].tick(sr) : sl; }
        if (master_on) { sl = v.mf[0].tick(sl); sr = ch > 1 ? v.mf[1].tick(sr) : sl; }
        outl[i] += sl * a * gl;
        outr[i] += sr * a * gr;
    }
}

void Sampler::swap_program() {
    Program *next = pending_.load();
    if (!next) return;
    if (!swap_fade_) {
        bool any = false;
        for (auto &v : voices_) if (v.on) { any = true; v.fading = true; v.amp.fast_release(0.006f); }
        synth.set_program(nullptr);   // 2.3.4: no new note picks the program going away
        if (synth.program_voices()) { any = true; synth.fade_program(); }   // 2.3.4: SAMPLE oscillators reading slots
        swap_fade_ = any ? 1 : 2;
        if (any) return;
    }
    for (auto &v : voices_) if (v.on) return;   // wait for the fades
    if (synth.program_voices()) return;
    if (retired_.load()) return;                // the loader has not collected the last one yet
    retired_.store(prog_);
    prog_ = pending_.exchange(nullptr);
    synth.set_program(prog_);   // 2.3.4
    if (prog_) {
        if (prog_->by_key.size() != 128) prog_->index();   // (the loader indexes it: no allocation on the audio thread)
        double a = 0;
        for (auto &z : prog_->inst.zones)
            if (z.amp_env.set) a = std::max(a, z.amp_env.delay + z.amp_env.attack + z.amp_env.hold);
        slowest_attack.store(float(a));
    }
    swap_fade_ = 0;
}

// Flush denormals to zero while rendering (filter and reverb tails decay into them, and they are slow on ARMv7's
// VFP). The host's FPU mode is restored afterwards: this is MPC's audio thread.
namespace {
struct FlushDenormals {
#if defined(__arm__) && defined(__ARM_PCS_VFP)
    uint32_t saved;
    FlushDenormals() {
        __asm__ volatile("vmrs %0, fpscr" : "=r"(saved));
        uint32_t v = saved | (1u << 24);   // FZ
        __asm__ volatile("vmsr fpscr, %0" : : "r"(v));
    }
    ~FlushDenormals() { __asm__ volatile("vmsr fpscr, %0" : : "r"(saved)); }
#elif defined(__aarch64__)
    uint64_t saved;
    FlushDenormals() {
        __asm__ volatile("mrs %0, fpcr" : "=r"(saved));
        __asm__ volatile("msr fpcr, %0" : : "r"(saved | (1ull << 24)));   // FZ
    }
    ~FlushDenormals() { __asm__ volatile("msr fpcr, %0" : : "r"(saved)); }
#elif defined(__SSE__) || defined(__x86_64__)
    unsigned saved;
    FlushDenormals() { saved = __builtin_ia32_stmxcsr(); __builtin_ia32_ldmxcsr(saved | 0x8040); }   // FTZ | DAZ
    ~FlushDenormals() { __builtin_ia32_ldmxcsr(saved); }
#endif
};
}  // namespace

// 2.4: with the ARP on, the block is rendered in pieces that end where its next step or note-off falls, so each note
// starts on its exact frame
void Sampler::render(int16_t *out, int frames) {
    const int cap = int(mixl_.size());   // the mix buffers' size: a larger host block in pieces
    while (frames > cap) { render(out, cap); out += 2 * cap; frames -= cap; }
    if (!arp.settings.on.load() && !arp.busy()) { render_block(out, frames); return; }   // (turned off: process() ends its notes once)
    int done = 0;
    while (done < frames) {
        const int n = arp.process(arp_sink_, frames - done);
        render_block(out + 2 * done, n);
        arp.advance(n);
        done += n;
    }
}

void Sampler::render_block(int16_t *out, int frames) {
    FlushDenormals ftz;
    if (panic_req_.exchange(false)) do_panic();
    if (prev_stop_.exchange(false)) retire_preview();
    if (Pcm *n = prev_req_.exchange(nullptr)) { retire_preview(); prev_ = n; prev_pos_ = 0; previewing.store(1); }
    bool pv = prev_ != nullptr && frames <= int(pvl_.size());
    if (pv) {   // the preview, resampled to the engine's rate, played once
        const Pcm &p = *prev_;
        double step = double(p.rate) / SR;
        int64_t n = p.frames();
        int ch = p.channels;
        for (int i = 0; i < frames; i++) {
            int64_t k = int64_t(prev_pos_);
            if (k + 1 >= n) { for (int j = i; j < frames; j++) pvl_[size_t(j)] = pvr_[size_t(j)] = 0; prev_pos_ = double(n); break; }
            float t = float(prev_pos_ - double(k));
            const int16_t *a = &p.data[size_t(k) * size_t(ch)], *b = a + ch;
            float l = (a[0] + (b[0] - a[0]) * t) * (0.8f / 32768.0f);
            float r = ch > 1 ? (a[1] + (b[1] - a[1]) * t) * (0.8f / 32768.0f) : l;
            pvl_[size_t(i)] = l; pvr_[size_t(i)] = r;
            prev_pos_ += step;
        }
    }
    if (pending_.load()) swap_program();
    update_mod(frames);
    float *l = mixl_.data(), *r = mixr_.data();
    std::fill(l, l + frames, 0.f);
    std::fill(r, r + frames, 0.f);
    int active = 0;
    if (prog_)
        for (auto &v : voices_) {
            if (!v.on) continue;
            for (int o = 0; o < frames && v.on; o += 64) render_voice(v, l + o, r + o, std::min(64, frames - o));
            if (v.on) active++;
        }
    // 2.3: the SYNTH layer, into the same mix before the FX (it plays with or without a program loaded)
    synth.render(l, r, frames);
    active += synth.active.load(std::memory_order_relaxed);
    active_voices.store(active);
    // 2.3.1: IDLE SKIP. Nothing playing and the output under -100 dB for half a second (every tail rung out): the FX
    // are skipped until the next note (they kept processing silence: up to ~3% of a Live II core with nothing playing)
    if (active == 0 && !pv && quiet_frames_ >= int64_t(SR / 2)) {
        std::memset(out, 0, sizeof(int16_t) * size_t(2 * frames));
        peak.store(peak.load() * 0.9f);
        gain_red_db.store(0);
        return;
    }
    float vol = std::pow(10.0f, settings.volume_db.load() / 20.0f) * cc7_ * cc7_ * cc11_;
    float pan = std::max(-1.0f, std::min(1.0f, settings.pan.load() + cc10_));
    float pl = pan > 0 ? 1 - pan : 1, pr = pan < 0 ? 1 + pan : 1;
    // 2.2: the reverb's type: Off (no key lit: no reverb whatever the mix), Classic (Freeverb, 1.0 to 2.1) or Plate /
    // Hall / Room (PlateReverb); cleared when it comes on or changes type, so no old tail comes back
    const int rsel = std::max(0, std::min(4, settings.reverb_type.load())), rtype = rsel - 1;   // rtype: 0 Classic, 1.. Plate
    float mix = rsel == 0 ? 0.0f : std::max(0.0f, std::min(1.0f, settings.reverb_mix.load() + g_rev_add_)),
          drive = std::max(0.0f, std::min(1.0f, settings.drive.load() + g_drive_add_));
    if (mix > 0.001f) {
        if (rtype != rev_type_) {
            if (rtype == 0) reverb_.clear();
            else { plate_.set_type(rtype); plate_.clear(); }
        }
        if (rtype == 0) reverb_.set(settings.reverb_size.load(), settings.reverb_damp.load());
        else plate_.set(settings.reverb_size.load(), settings.reverb_damp.load(), settings.reverb_predelay.load());
    }
    rev_type_ = mix > 0.001f ? rtype : -1;
    // 1.8: the FX page, before the reverb: EQ, compressor, chorus, delay. Each is skipped while off; the chorus and
    // delay lines are cleared when switched back on, so no old audio comes back
    eq_.set(settings.eq_low.load(), settings.eq_mid.load(), settings.eq_high.load());
    // 2.2: the compressor's type: Off (none chosen: AMOUNT does nothing), Classic (1.8's), Glue / Punch (BusComp)
    const int mtype = std::max(0, std::min(3, settings.comp_type.load()));
    float comp = mtype == 0 ? 0.0f : settings.comp.load();
    const bool comp_on = comp > 0.001f;
    if (comp_on) {
        if (!comp_on_ || mtype != comp_type_) {
            if (mtype == 1) comp_.clear();
            else bus_.clear();
        }
        if (mtype == 1) comp_.set(comp); else bus_.set(mtype, comp);
    }
    comp_type_ = mtype;
    comp_on_ = comp_on;
    // 2.2: the limiter (63 samples of delay while on); cleared when switched on
    const bool lim_on = settings.limiter.load() != 0;
    if (lim_on && !limiter_on_) limiter_.clear();
    limiter_on_ = lim_on;
    // 2.2: the delay's type: Off (none lit: no delay whatever the mix), Classic, Tape, Analog
    const int dtype = std::max(0, std::min(3, settings.delay_type.load()));
    // 2.2: the chorus's type: Off (none lit: no chorus whatever the mix), Classic (1.8's), Ens I / II / I+II (Ensemble)
    const int ctype = std::max(0, std::min(4, settings.chorus_type.load()));
    float cmix = ctype == 0 ? 0.0f : settings.chorus_mix.load(), dmix = dtype == 0 ? 0.0f : settings.delay_mix.load();
    if (cmix > 0.001f) {
        if (!chorus_on_ || ctype != chorus_type_) { chorus_.clear(); ensemble_.clear(); }
        if (ctype == 1) chorus_.set(settings.chorus_rate.load(), settings.chorus_depth.load(), cmix);
        else ensemble_.set(ctype - 1, settings.chorus_rate.load(), settings.chorus_depth.load(), cmix);
    }
    chorus_on_ = cmix > 0.001f;
    chorus_type_ = ctype;
    if (dmix > 0.001f) {
        if (!delay_on_) delay_.clear();
        int dv = std::max(0, std::min(DELAY_COUNT - 1, settings.delay_div.load()));
        double tempo = tempo_ > 20 ? tempo_ : 120;
        delay_.set(float(DELAY_BEATS[dv] * 60.0 / tempo), settings.delay_fb.load(), settings.delay_tone.load(), dmix, settings.delay_ping.load() != 0, dtype);
    }
    delay_on_ = dmix > 0.001f;
    // 1.8.2: the MOD FX page, after the compressor: lo-fi, phaser, flanger, tremolo / auto-pan
    float lmix = settings.lofi_mix.load(), pmix = settings.phaser_mix.load(), fmix = settings.flanger_mix.load(),
          tdepth = settings.trem_depth.load();
    if (lmix > 0.001f && !lofi_on_) lofi_.clear();
    if (lmix > 0.001f) lofi_.set(settings.lofi_rate.load(), settings.lofi_bits.load(), lmix);
    lofi_on_ = lmix > 0.001f;
    if (pmix > 0.001f) {
        if (!phaser_on_) phaser_.clear();
        phaser_.set(settings.phaser_rate.load(), settings.phaser_depth.load(), settings.phaser_fb.load(), pmix);
    }
    phaser_on_ = pmix > 0.001f;
    if (fmix > 0.001f) {
        if (!flanger_on_) flanger_.clear();
        flanger_.set(settings.flanger_rate.load(), settings.flanger_depth.load(), settings.flanger_fb.load(), fmix);
    }
    flanger_on_ = fmix > 0.001f;
    if (tdepth > 0.001f) {
        int ts = settings.trem_sync.load();
        double beats = ts > 0 && ts < SYNC_COUNT ? SYNC_BEATS[ts] : 0;
        double tempo = tempo_ > 20 ? tempo_ : 120;
        trem_.set(float(beats > 0 ? tempo / 60.0 / beats : settings.trem_rate.load()), tdepth, settings.trem_pan.load() != 0,
                  settings.trem_square.load() != 0);
    }
    const bool lofi_on = lmix > 0.001f, trem_on = tdepth > 0.001f;
    const bool eq_on = eq_.on;
    float pk = 0, sum = 0;   // sum: 2.4.32, the non-finite check
    // 2.2: the drive's type: Off (no type chosen: the DRIVE knob does nothing), else 2x oversampled (Drive)
    const int dsel = std::max(0, std::min(4, settings.drive_type.load()));
    if (dsel > 0) {
        if (dsel != drive_type_) drive_.clear();
        drive_.set(dsel, drive);
    }
    drive_type_ = dsel;
    {   // 2.4.30: FX BANK, once per block on the mix (off = program 0 / NO EFFECT / mix 0: nothing runs)
        // 2.4.33: the bay's slot: A runs the factory rack; B..D run their expansion bank (A is then off: program 0)
        const int slot = std::max(0, std::min(3, settings.fxb_slot.load()));
        const int fsel = slot == 0 ? settings.fxb_prog.load() : 0;
        const bool byp = settings.fxb_bypass.load() != 0, was_byp = fxb_byp_;
        if (fsel != fxb_sel_) { if (!fxb_.select(size_t(std::max(0, fsel)))) fxb_.select(0); fxb_sel_ = fsel; }
        else if (was_byp && !byp) fxb_.reset();   // back from BYPASS: no tail from before it
        fxb_byp_ = byp;
        auto controls = [this] {
            omnisampler::fxbank::Controls c;
            c.mix = settings.fxb_mix.load(); c.rateHz = settings.fxb_rate.load(); c.depth = settings.fxb_depth.load();
            c.feedback = settings.fxb_fb.load(); c.timeMs = settings.fxb_time.load(); c.tone = settings.fxb_tone.load();
            c.driveDb = settings.fxb_drive.load(); c.thresholdDb = settings.fxb_thr.load(); c.ratio = settings.fxb_ratio.load();
            c.pitchSemitones = settings.fxb_pitch.load(); c.lowDb = settings.fxb_low.load(); c.midDb = settings.fxb_mid.load();
            c.highDb = settings.fxb_high.load();
            return c;
        };
        if (fsel > 0 && !byp) {
            const omnisampler::fxbank::Controls c = controls();
            if (!fxb_set_ || std::memcmp(&c, &fxb_c_, sizeof c) != 0) { fxb_.set(c); fxb_c_ = c; fxb_set_ = true; }   // pow()s only on a change
            fxb_.process(l, r, size_t(frames));
        }
        if (slot > 0 && fxx_[slot - 1]) {   // 2.4.33: an expansion bank (an empty slot: nothing runs, the mix passes dry)
            FxExpansion &x = *fxx_[slot - 1];
            const int k = slot - 1, n = x.count();
            const int p = std::max(0, std::min(n, settings.fxb_xprog[k].load()));
            if (p != fxx_sel_[k] || slot != fxx_slot_) {   // a new program, or back in this slot: no old tail
                if (p > 0) x.select(p);
                fxx_sel_[k] = p;
            } else if (was_byp && !byp) x.reset();
            if (p > 0 && !byp) {
                const omnisampler::fxbank::Controls c = controls();
                if (!fxx_set_[k] || std::memcmp(&c, &fxx_c_[k], sizeof c) != 0) { x.set(c); fxx_c_[k] = c; fxx_set_[k] = true; }
                x.tempo(tempo_);   // 2.4.35 (Camel: tempo gates / delays)
                x.process(l, r, frames);
            }
        }
        fxx_slot_ = slot;
    }
    for (int i = 0; i < frames; i++) {
        float a = l[i], b = r[i];
        if (eq_on) eq_.tick(a, b);
        if (comp_on) { if (mtype == 1) comp_.tick(a, b); else bus_.tick(a, b); }
        if (lofi_on) lofi_.tick(a, b);
        if (phaser_on_) phaser_.tick(a, b);
        if (flanger_on_) flanger_.tick(a, b);
        if (trem_on) trem_.tick(a, b);
        if (chorus_on_) { if (ctype == 1) chorus_.tick(a, b); else ensemble_.tick(a, b); }
        if (delay_on_) delay_.tick(a, b);
        if (mix > 0.001f) {
            float wl, wr;
            if (rtype == 0) reverb_.tick(a, b, wl, wr);
            else plate_.tick(a, b, wl, wr);
            a = a * (1 - mix * 0.5f) + wl * mix;
            b = b * (1 - mix * 0.5f) + wr * mix;
        }
        if (pv) { a += pvl_[size_t(i)]; b += pvr_[size_t(i)]; }   // the preview: after the reverb, at the plugin's volume
        a *= vol * pl; b *= vol * pr;
        if (dsel > 0) drive_.tick(a, b);
        if (lim_on) limiter_.tick(a, b);   // 2.2: before the soft limit, which it then never reaches
        sum += a + b;
        // soft limit above -1 dBFS instead of hard clipping
        auto lim = [](float x) { float ax = std::fabs(x); return ax < 0.89f ? x : (x > 0 ? 1 : -1) * (0.89f + 0.11f * std::tanh((ax - 0.89f) / 0.11f)); };
        a = lim(a); b = lim(b);
        pk = std::max(pk, std::max(std::fabs(a), std::fabs(b)));
        out[2 * i] = int16_t(std::lrint(a * 32767.0f));
        out[2 * i + 1] = int16_t(std::lrint(b * 32767.0f));
    }
    if (!std::isfinite(sum)) {   // 2.4.32: inf / NaN in the mix: this block silent, every state cleared (heal_nonfinite)
        std::memset(out, 0, sizeof(int16_t) * size_t(2 * frames));
        heal_nonfinite();
        pk = 0;
    }
    peak.store(std::max(pk, peak.load() * 0.9f));
    quiet_frames_ = active == 0 && !pv && pk < 1e-5f ? quiet_frames_ + frames : 0;   // 2.3.1: idle skip
    {   // 2.2: the GR meter: the compressor's reduction plus the limiter's, falling back at about 40 dB a second
        float red = 0;
        if (comp_on && mtype == 1) red += std::max(0.0f, 20 * std::log10(std::max(1e-6f, comp_.makeup / std::max(1e-6f, comp_.g))));
        else if (comp_on) red += bus_.red_db;
        if (lim_on) red += std::max(0.0f, -20 * std::log10(std::max(1e-6f, limiter_.last_g)));
        float shown = gain_red_db.load();
        gain_red_db.store(std::max(red, shown - 40.0f * float(frames) / SR));
    }
    if (pv && prev_pos_ >= double(prev_->frames() - 1)) retire_preview();
}

}  // namespace omni
