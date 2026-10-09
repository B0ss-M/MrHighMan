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
    for (int i = 0; i < 4; i++) { settings.slot_hi[i].store(127); settings.slot_gain[i].store(1); }
    mixl_.assign(4096, 0);
    mixr_.assign(4096, 0);
    pvl_.assign(4096, 0);
    pvr_.assign(4096, 0);
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
    case 0x90: if (d2) note_on(chan, d1, d2); else note_off(chan, d1); break;
    case 0x80: note_off(chan, d1); break;
    case 0xD0: at_ = d1 / 127.0f; break;
    case 0xA0: at_ = d2 / 127.0f; break;
    case 0xE0: bend_ = float((d2 << 7 | d1) - 8192) / 8192.0f; break;
    case 0xB0:
        switch (d1) {
        case 1: modwheel_ = d2 / 127.0f; break;
        case 7: cc7_ = d2 / 127.0f; break;
        case 10: cc10_ = (d2 - 64) / 64.0f; break;
        case 11: cc11_ = d2 / 127.0f; break;
        case 64:
            sustain_ = d2 >= 64;
            if (!sustain_)
                for (auto &v : voices_)
                    if (v.on && v.held_by_pedal) { v.held_by_pedal = false; if (!v.one_shot) { v.released = true; v.amp.release(); v.fenv.release(); v.penv.release(); } }
            break;
        case 120: all_off(true); break;
        case 121: bend_ = 0; modwheel_ = 0; cc7_ = 1; cc10_ = 0; cc11_ = 1; break;
        case 123: case 124: case 125: case 126: case 127:
            for (int c = 0; c < 16; c++) for (int n = 0; n < 128; n++) if (held_[c][n]) note_off(c, n);
            all_off(false);
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
    retire_preview();
    for (auto &v : voices_)
        if (v.on) { v.released = true; v.held_by_pedal = false; v.looping = false; v.fading = true; v.amp.fast_release(0.005f); }
    std::memset(held_, 0, sizeof held_);
    held_count_ = 0;
    for (auto &k : key_down) k.store(0);
    sustain_ = false;
    last_mono_note_ = -1;
    bend_ = 0; modwheel_ = 0; at_ = 0;
}

void Sampler::all_off(bool hard) {
    for (auto &v : voices_) {
        if (!v.on) continue;
        if (hard) v.on = false;
        else { v.released = true; v.fading = true; v.amp.fast_release(0.01f); }
    }
    sustain_ = false;
}

void Sampler::note_on(int chan, int note, int vel) {
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
    if (vel == 0) vel = 64;
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
    if (!z.loops.empty() && !z.reverse && !v.one_shot) {
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
        for (int k = 0; k < MOD_SLOTS; k++)
            if (slot_dst_[k] == MD_START && slot_amt_[k] != 0) acc += slot_amt_[k] * src_val(v, slot_src_[k]);
        if (acc > 0 && !z.reverse) v.start = std::min(v.stop - 1, v.start + int64_t(std::min(1.0f, acc) * 0.9f * float(v.stop - v.start)));
    }
    v.reverse_ = z.reverse;
    v.dir = z.reverse ? -1 : 1;
    v.pos = z.reverse ? double(v.stop - 1) : double(v.start);

    // level
    float vel_sens = settings.vel_sens.load();
    double vel01 = vel / 127.0;
    double curve = std::pow(vel01, 2.0 * std::pow(3.0, z.amp_vel_curve));
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
    float pan = float(clampd_(z.pan, -1, 1));
    v.pan_l = std::cos((pan + 1) * PI_F / 4);
    v.pan_r = std::sin((pan + 1) * PI_F / 4);

    // envelopes
    Envelope ae = z.amp_env;
    if (!ae.set) { ae = Envelope(); ae.release = 0.02; }
    if (ae.release < 0.004) ae.release = 0.004;
    float tvel = float(1.0 - ae.time_vel_tracking * (vel01 - 0.5));   // + tracking shortens towards high velocities
    float tkey = float(std::pow(2.0, -ae.time_key_tracking * (note - 60) / 24.0));
    v.amp.begin(ae, tvel * tkey, tkey, settings.attack_add.load() + settings.d_attack.load(), settings.release_add.load() + settings.d_release.load(),
                settings.sustain_scale.load(), settings.d_sustain.load());
    v.amp.decay_s = std::max(0.0f, v.amp.decay_s * settings.decay_scale.load() + settings.d_decay.load());
    v.amp.attack_s = std::max(0.0f, v.amp.attack_s);              // the faders may take a zone's times below zero
    v.amp.release_s = std::max(0.004f, v.amp.release_s);
    if (v.amp.stage == Env::Attack) v.amp.enter(Env::Attack);   // re-read the changed times
    if (release_trigger || v.one_shot) { v.amp.sustain = 1; }
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
    for (int k = 0; k < MOD_SLOTS; k++) {
        if (slot_amt_[k] == 0 || slot_src_[k] == MS_OFF) continue;
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
    semis += m_pitch * MOD_PITCH_SEMIS;
    if (v.loop_tune != 0 && v.looping && v.pos >= double(v.loop_start) && !(v.released && v.loop_until_release)) semis += v.loop_tune;
    v.mod_cut_mul = m_cut != 0 ? std::exp2(m_cut * MOD_CUTOFF_OCT) : 1.0f;
    v.mod_res = m_res * MOD_RES;
    v.mod_gain = std::max(0.0f, std::min(2.0f, 1.0f + m_vol));
    v.mod_pan = std::max(-1.0f, std::min(1.0f, m_pan));
    v.mod_wt = m_wt;
    step = std::pow(2.0, semis / 12.0) * double(v.pcm->rate) / double(SR);
    float cents = 0;
    if (z.filter.type != FilterType::None) {
        cents += float(z.filter.env_depth * MAX_ENVELOPE_DEPTH) * (z.filter.env.set ? v.fenv.level : 0);
        cents += float(z.filter.vel_depth * 9600.0) * (v.vel / 127.0f);
        cents += float(z.filter.key_tracking * 100.0) * float(v.znote - z.root);
        cents += float(z.filter.lfo_depth * MAX_ENVELOPE_DEPTH) * fl;
        cents += float(z.filter.modwheel_depth * MAX_ENVELOPE_DEPTH) * modwheel_;
    }
    cutoff_mul = std::pow(2.0f, cents / 1200.0f) * v.mod_cut_mul;
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
    if (tempo > 1) tempo_ = tempo;
    playing_ = playing != 0;
    ppq_valid_ = ppq_valid != 0;
    if (ppq_valid_) ppq_ = ppq;
}

// beats (quarter notes) per LFO cycle for each sync setting; 0 = free
static const double SYNC_BEATS[] = {0, 16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3, 1.0 / 3, 1.0 / 6, 1.5, 0.75, 0.375};
constexpr int SYNC_COUNT = int(sizeof SYNC_BEATS / sizeof SYNC_BEATS[0]);

void Sampler::update_mod(int frames) {
    cut_mod_ = false;
    g_drive_add_ = g_rev_add_ = 0;
    for (int k = 0; k < MOD_SLOTS; k++) {
        slot_src_[k] = settings.mod_src[k].load();
        slot_dst_[k] = settings.mod_dst[k].load();
        slot_amt_[k] = settings.mod_amt[k].load();
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
    double step;
    float cutoff_mul;
    block_mod(v, n, step, cutoff_mul);
    const int ch = v.pcm->channels;
    const int nch = ch > 1 ? 2 : 1;
    if (v.zf[0].on) for (int c = 0; c < nch; c++) v.zf[c].set(float(z.filter.cutoff) * cutoff_mul, std::max(0.0f, std::min(1.0f, float(z.filter.resonance) + v.mod_res)));
    float mc = settings.cutoff_hz.load(), mr = settings.resonance.load();
    float fv = settings.filter_vel.load();
    if (fv > 0) mc *= std::pow(2.0f, -4.0f * fv * (1.0f - v.vel / 127.0f));
    mc *= v.mod_cut_mul;
    mr = std::max(0.0f, std::min(1.0f, mr + v.mod_res));
    bool master_on = !(int(settings.filter_type.load()) == 0 && mc >= 19500 && mr < 0.01f) || cut_mod_;
    if (master_on) for (int c = 0; c < nch; c++) v.mf[c].set(mc, mr);
    float amp_lfo_gain = 1;
    if (z.amp_lfo.set && z.amp_lfo_depth != 0)
        amp_lfo_gain = std::pow(10.0f, float(z.amp_lfo_depth * MAX_VOLUME_DEPTH) * v.alfo.value / 20.0f);
    const int interp = settings.interpolation.load();
    const int16_t *d = v.pcm->data.data();
    const float bal_l = v.mod_pan > 0 ? 1 - v.mod_pan : 1, bal_r = v.mod_pan < 0 ? 1 + v.mod_pan : 1;
    const float gl = v.gain * v.mod_gain * bal_l * v.pan_l * amp_lfo_gain * (1.0f / 32768.0f),
                gr = v.gain * v.mod_gain * bal_r * v.pan_r * amp_lfo_gain * (1.0f / 32768.0f);
    const bool zf_on = v.zf[0].on, fenv_on = z.filter.env.set, penv_on = z.pitch_env.set;
    if (z.wt_count > 1 && z.wt_size > 1) {
        // A wavetable: loop one cycle of wt_size frames (v.pos is the phase) and crossfade between the two cycles either
        // side of the position (the WT POSITION knob plus the matrix), set per sub-block
        const int N = z.wt_size, C = z.wt_count;
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
        // Fast path: a forward run that can reach no loop point, end or start this block. It works in 32-bit
        // indices: on ARMv7 every double <-> int64 conversion of the generic path below is a library call.
        if (v.dir > 0) {
            bool loop_active = v.looping && !(v.released && v.loop_until_release);
            int64_t lim = loop_active ? std::min(v.loop_end - 1, v.stop) : v.stop;   // first index the run may not read
            double hi = double(lim - 3), lo = double(v.start + 1);
            double pos = v.pos;
            if (pos >= lo && pos <= hi) {
                int k = int((hi - pos) / step) + 1;
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
                    } else {
                        const int16_t *p = d + i0 * ch;
                        sl = p[0];
                        sr = ch > 1 ? float(p[1]) : sl;
                    }
                    pos += step;
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
        swap_fade_ = any ? 1 : 2;
        if (any) return;
    }
    for (auto &v : voices_) if (v.on) return;   // wait for the fades
    if (retired_.load()) return;                // the loader has not collected the last one yet
    retired_.store(prog_);
    prog_ = pending_.exchange(nullptr);
    if (prog_) {
        prog_->index();
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
#elif defined(__SSE__) || defined(__x86_64__)
    unsigned saved;
    FlushDenormals() { saved = __builtin_ia32_stmxcsr(); __builtin_ia32_ldmxcsr(saved | 0x8040); }   // FTZ | DAZ
    ~FlushDenormals() { __builtin_ia32_ldmxcsr(saved); }
#endif
};
}  // namespace

void Sampler::render(int16_t *out, int frames) {
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
    active_voices.store(active);
    float vol = std::pow(10.0f, settings.volume_db.load() / 20.0f) * cc7_ * cc7_ * cc11_;
    float pan = std::max(-1.0f, std::min(1.0f, settings.pan.load() + cc10_));
    float pl = pan > 0 ? 1 - pan : 1, pr = pan < 0 ? 1 + pan : 1;
    float mix = std::max(0.0f, std::min(1.0f, settings.reverb_mix.load() + g_rev_add_)),
          drive = std::max(0.0f, std::min(1.0f, settings.drive.load() + g_drive_add_));
    if (mix > 0.001f) reverb_.set(settings.reverb_size.load(), settings.reverb_damp.load());
    float pk = 0;
    for (int i = 0; i < frames; i++) {
        float a = l[i], b = r[i];
        if (mix > 0.001f) {
            float wl, wr;
            reverb_.tick(a, b, wl, wr);
            a = a * (1 - mix * 0.5f) + wl * mix;
            b = b * (1 - mix * 0.5f) + wr * mix;
        }
        if (pv) { a += pvl_[size_t(i)]; b += pvr_[size_t(i)]; }   // the preview: after the reverb, at the plugin's volume
        a *= vol * pl; b *= vol * pr;
        if (drive > 0.001f) {
            float k = 1 + drive * 8;
            a = std::tanh(a * k) / std::tanh(k) ;
            b = std::tanh(b * k) / std::tanh(k);
        }
        // soft limit above -1 dBFS instead of hard clipping
        auto lim = [](float x) { float ax = std::fabs(x); return ax < 0.89f ? x : (x > 0 ? 1 : -1) * (0.89f + 0.11f * std::tanh((ax - 0.89f) / 0.11f)); };
        a = lim(a); b = lim(b);
        pk = std::max(pk, std::max(std::fabs(a), std::fabs(b)));
        out[2 * i] = int16_t(std::lrint(a * 32767.0f));
        out[2 * i + 1] = int16_t(std::lrint(b * 32767.0f));
    }
    peak.store(std::max(pk, peak.load() * 0.9f));
    if (pv && prev_pos_ >= double(prev_->frames() - 1)) retire_preview();
}

}  // namespace omni
