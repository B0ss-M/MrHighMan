// 2.4.39: the Decent Sampler full mapping (reports/DECENT_FULL_MAPPING_SPEC.md section 6.3): small presets written at run
// time (one 1 s 440 Hz WAV), loaded through the reader, every mapped field checked. ASan build, as probe.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "core/vfs.hpp"
#include "formats/format.hpp"

using namespace omni;
static int fails = 0;
static void check(bool ok, const std::string &what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }
static const std::string DIR = "/tmp/omni-decent-map-test";

static void write_wav(const std::string &path, int frames, int channels = 1) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };
    const uint32_t data = uint32_t(frames * channels * 2);
    f.write("RIFF", 4); u32(36 + data); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(uint16_t(channels)); u32(44100);
    u32(44100 * 2 * uint32_t(channels)); u16(uint16_t(2 * channels)); u16(16); f.write("data", 4); u32(data);
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++) u16(uint16_t(int16_t(8000 * std::sin(2 * M_PI * 440 * i / 44100.0))));
}

static Instrument load(const std::string &name, const std::string &xml, std::string *err = nullptr) {
    const std::string p = DIR + "/" + name + ".dspreset";
    std::ofstream(p) << xml;
    try {
        Vfs vfs;
        Location loc = vfs.resolve(p);
        const FormatReader *r = find_reader(*loc.volume, loc.inner);
        if (!r) throw ParseError("no reader");
        return r->load(loc.volume, loc.inner, 0);
    } catch (const std::exception &e) {
        if (err) *err = e.what();
        else std::printf("  %s: %s\n", name.c_str(), e.what());
        return Instrument();
    }
}
static bool has(const Instrument &in, const char *k, double v, double tol = 0.6) {
    for (const auto &kv : in.settings) if (kv.first == k) return std::fabs(kv.second - v) <= tol;
    return false;
}
static double get(const Instrument &in, const char *k) {
    for (const auto &kv : in.settings) if (kv.first == k) return kv.second;
    return NAN;
}
static bool warned(const Instrument &in, const char *what) {
    for (const auto &w : in.warnings) if (w.find(what) != std::string::npos) return true;
    return false;
}
static std::string head(const std::string &groups_attr, const std::string &body, const std::string &rest = "") {
    return "<DecentSampler><groups " + groups_attr + ">" + body + "</groups>" + rest + "</DecentSampler>";
}
static const std::string S = "<sample path=\"s.wav\" rootNote=\"69\"/>";

int main() {
    register_all();
    (void)!std::system(("rm -rf " + DIR + " && mkdir -p " + DIR).c_str());
    write_wav(DIR + "/s.wav", 44100);
    write_wav(DIR + "/ir_room.wav", 22050);    // 0.5 s: a room IR
    write_wav(DIR + "/ir_cab.wav", 1500);      // 34 ms: a cabinet IR
    write_wav(DIR + "/wt.wav", 2048 * 4);      // 4 cycles of 2048 (guess_cycle may refuse: the oscillator then warns)

    // 1. menu: option 2 enables group 1, disables group 0
    {
        std::string ui = "<ui><tab><menu value=\"2\"><option name=\"A\"><binding type=\"general\" level=\"group\" position=\"0\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"true\"/><binding type=\"general\" level=\"group\" position=\"1\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"false\"/></option>"
                         "<option name=\"B\"><binding type=\"general\" level=\"group\" position=\"0\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"false\"/><binding type=\"general\" level=\"group\" position=\"1\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"true\"/></option></menu></tab></ui>";
        Instrument in = load("menu", head("", "<group><sample path=\"s.wav\" rootNote=\"60\"/></group><group><sample path=\"s.wav\" rootNote=\"72\"/></group>", ui));
        check(in.zones.size() == 1 && in.zones[0].root == 72, "menu option 2: only group 1 plays");
    }
    // 2. button state 1 disables group 0
    {
        std::string ui = "<ui><tab><button value=\"1\"><state name=\"Off\"><binding type=\"general\" level=\"group\" position=\"0\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"true\"/></state><state name=\"On\"><binding type=\"general\" level=\"group\" position=\"0\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"false\"/></state></button></tab></ui>";
        Instrument in = load("button", head("", "<group>" + S + "</group><group><sample path=\"s.wav\" rootNote=\"50\"/></group>", ui));
        check(in.zones.size() == 1 && in.zones[0].root == 50, "button state 1: group 0 off");
    }
    // 3. knob -> GROUP_TUNING
    {
        std::string ui = "<ui><tab><labeled-knob minValue=\"-12\" maxValue=\"12\" value=\"7\"><binding type=\"amp\" level=\"group\" position=\"0\" parameter=\"GROUP_TUNING\"/></labeled-knob></tab></ui>";
        Instrument in = load("tune", head("", "<group>" + S + "</group>", ui));
        check(!in.zones.empty() && std::fabs(in.zones[0].tune - 7) < 1e-6, "knob GROUP_TUNING 7 -> tune +7");
    }
    // 4. gain effect: bare number = dB; linear unit
    {
        Instrument a = load("gain_db", head("", "<group>" + S + "</group>", "<effects><effect type=\"gain\" level=\"-6\"/></effects>"));
        Instrument b = load("gain_lin", head("", "<group>" + S + "</group>", "<effects><effect type=\"gain\" levelUnit=\"linear\" level=\"0.5\"/></effects>"));
        check(std::fabs(a.gain_db + 6) < 0.01, "gain level=-6 -> -6 dB (was -96)");
        check(std::fabs(b.gain_db + 6.02) < 0.01, "gain linear 0.5 -> -6.02 dB");
    }
    // 5. filters
    {
        Instrument a = load("lp", head("", "<group>" + S + "</group>", "<effects><effect type=\"lowpass\" frequency=\"1000\" resonance=\"2\"/></effects>"));
        check(!a.zones.empty() && a.zones[0].filter.type == FilterType::LowPass && std::fabs(a.zones[0].filter.cutoff - 1000) < 1 && a.zones[0].filter.resonance > 0.2, "lowpass 1000 Hz, Q 2");
        Instrument b = load("lp1", head("", "<group>" + S + "</group>", "<effects><effect type=\"lowpass_1pl\" frequency=\"2000\"/></effects>"));
        check(!b.zones.empty() && b.zones[0].filter.poles == 1, "lowpass_1pl -> 1 pole");
        Instrument c = load("lpenv", head("", "<group>" + S + "</group>", "<effects><effect type=\"lowpass\" frequency=\"500\" envelope_amount=\"3000\" envelope_attack=\"0.01\" envelope_decay=\"0.3\" envelope_sustain=\"0\" envelope_release=\"0.5\"/></effects>"));
        check(!c.zones.empty() && c.zones[0].filter.env.set && c.zones[0].filter.env_depth > 0.2 && std::fabs(c.zones[0].filter.env.decay - 0.3) < 1e-6, "filter envelope_* -> filter env");
        Instrument d = load("notch", head("", "<group>" + S + "</group>", "<effects><effect type=\"notch\" frequency=\"3000\" q=\"3\"/></effects>"));
        check(!d.zones.empty() && d.zones[0].filter.type == FilterType::BandReject, "notch");
    }
    // 6. delay seconds -> divisions at 120 BPM; feedback, mix, ping-pong
    {
        Instrument a = load("dly", head("", "<group>" + S + "</group>", "<effects><effect type=\"delay\" delayTime=\"0.5\" feedback=\"0.3\" wetLevel=\"0.4\" stereoOffset=\"0.01\"/></effects>"));
        check(has(a, "dly_type", 1) && has(a, "dly_time", 8) && has(a, "dly_fb", 30) && has(a, "dly_mix", 40) && has(a, "dly_ping", 1), "delay 0.5 s -> 1/4, fb 30, mix 40, ping-pong");
        Instrument b = load("dly2", head("", "<group>" + S + "</group>", "<effects><effect type=\"delay\" delayTime=\"0.25\" wetLevel=\"0.5\"/></effects>"));
        check(has(b, "dly_time", 5), "delay 0.25 s -> 1/8");
    }
    // 8. chorus / phaser / bit crusher / shaper / folder / compressor / limiter / convolution / pitch shift / EQ
    {
        Instrument a = load("cho", head("", "<group>" + S + "</group>", "<effects><effect type=\"chorus\" mix=\"0.5\" modDepth=\"0.2\" modRate=\"0.5\"/></effects>"));
        check(has(a, "cho_type", 1) && has(a, "cho_mix", 50) && has(a, "cho_depth", 20) && has(a, "cho_rate", 50), "chorus mix 50, depth 20, 0.5 Hz = knob 50");
        Instrument b = load("phs", head("", "<group>" + S + "</group>", "<effects><effect type=\"phaser\" mix=\"1\" modDepth=\"0.5\" modRate=\"0.05\" feedback=\"-0.5\"/></effects>"));
        check(has(b, "phs_mix", 100) && has(b, "phs_rate", 0) && has(b, "phs_fb", 45), "phaser");
        Instrument c = load("bit", head("", "<group>" + S + "</group>", "<effects><effect type=\"bit_crusher\" bitDepth=\"8\" sampleRateReduction=\"4\"/></effects>"));
        check(has(c, "lofi_bits", 8) && has(c, "lofi_mix", 100) && get(c, "lofi_rate") > 30 && get(c, "lofi_rate") < 45, "bit crusher 8 bit, rate / 4");
        Instrument d = load("shp", head("", "<group>" + S + "</group>", "<effects><effect type=\"wave_shaper\" drive=\"100\"/></effects>"));
        check(has(d, "drive_type", 3) && get(d, "drive") > 60, "wave shaper -> Tube drive");
        Instrument e = load("fld", head("", "<group>" + S + "</group>", "<effects><effect type=\"wave_folder\" drive=\"10\"/></effects>"));
        check(has(e, "drive_type", 4), "wave folder -> Fold drive");
        Instrument f = load("cmp", head("", "<group>" + S + "</group>", "<effects><effect type=\"compressor\" ratio=\"4\" threshold=\"-20\"/><effect type=\"limiter\"/></effects>"));
        check(has(f, "comp_type", 1) && get(f, "comp") > 20 && has(f, "limiter", 1), "compressor + limiter");
        Instrument g = load("cvr", head("", "<group>" + S + "</group>", "<effects><effect type=\"convolution\" mix=\"0.3\" irFile=\"ir_room.wav\"/></effects>"));
        check(has(g, "rev_type", 4) && has(g, "rev_mix", 30) && warned(g, "convolution reverb replaced"), "convolution 0.5 s IR -> Room reverb");
        Instrument h = load("cvc", head("", "<group>" + S + "</group>", "<effects><effect type=\"convolution\" mix=\"1\" irFile=\"ir_cab.wav\"/></effects>"));
        check(std::isnan(get(h, "rev_mix")) && warned(h, "cabinet"), "convolution 34 ms IR -> skipped, warned");
        Instrument i = load("psh", head("", "<group>" + S + "</group>", "<effects><effect type=\"pitch_shift\" pitchShift=\"12\" mix=\"1\"/></effects>"));
        check(!i.zones.empty() && std::fabs(i.zones[0].tune - 12) < 1e-6, "pitch shift fully wet -> tune +12");
        Instrument j = load("eq", head("", "<group>" + S + "</group>", "<effects><effect type=\"peak\" frequency=\"1000\" gain=\"2\"/><effect type=\"low_shelf\" frequency=\"100\" gain=\"0.5\"/></effects>"));
        check(has(j, "eq_mid", 6.02, 0.1) && has(j, "eq_low", -6.02, 0.1), "peak +6 dB mid, low shelf -6 dB");
        Instrument k = load("rev", head("", "<group>" + S + "</group>", "<effects><effect type=\"reverb\" wetLevel=\"0.3\" roomSize=\"0.5\" damping=\"0.2\"/></effects>"));
        check(has(k, "rev_mix", 30) && has(k, "rev_size", 50) && has(k, "rev_damp", 20), "reverb");
    }
    // 9. LFO vibrato on GROUP_TUNING (the guide's example): a zone pitch LFO at 5 Hz
    {
        std::string mods = "<modulators><lfo shape=\"sine\" frequency=\"5\" modAmount=\"0.15\" scope=\"global\"><binding type=\"amp\" level=\"group\" position=\"0\" parameter=\"GROUP_TUNING\" modBehavior=\"add\" translation=\"linear\" translationOutputMin=\"-0.5\" translationOutputMax=\"0.5\"/></lfo></modulators>";
        Instrument in = load("vib", head("", "<group>" + S + "</group>", mods));
        check(!in.zones.empty() && in.zones[0].pitch_lfo.set && std::fabs(in.zones[0].pitch_lfo.rate_hz - 5) < 1e-6 && in.zones[0].pitch_lfo_depth > 0, "LFO -> GROUP_TUNING: zone pitch LFO 5 Hz");
    }
    // 10. mod wheel -> filter; 11. velocity -> filter (<midi><velocity>, as Basitar)
    {
        std::string mods = "<modulators><midiCC number=\"1\"><binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"FX_FILTER_FREQUENCY\" modBehavior=\"add\" translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"5000\"/></midiCC></modulators>";
        Instrument a = load("cc1", head("", "<group>" + S + "</group>", "<effects><effect type=\"lowpass\" frequency=\"500\"/></effects>" + mods));
        check(!a.zones.empty() && a.zones[0].filter.modwheel_depth > 0, "mod wheel -> filter (zone)");
        std::string midi = "<midi><velocity><binding modAmount=\"0.4\" level=\"group\" parameter=\"FX_FILTER_FREQUENCY\" position=\"0\" type=\"effect\"/></velocity></midi>";
        Instrument b = load("vel", head("", "<group>" + S + "<effects><effect type=\"lowpass_4pl\" frequency=\"300\"/></effects></group>", midi));
        check(!b.zones.empty() && b.zones[0].filter.vel_depth > 0, "<midi><velocity> -> filter velocity depth");
    }
    // 12. modulation overflow: 10 LFO routes to pan -> slots used, extras dropped with a warning
    {
        std::string mods = "<modulators>";
        for (int i = 0; i < 10; i++)
            mods += "<lfo shape=\"sine\" frequency=\"" + std::to_string(1 + i) + "\"><binding type=\"amp\" level=\"instrument\" parameter=\"PAN\" translation=\"linear\" translationOutputMin=\"-1\" translationOutputMax=\"1\"/></lfo>";
        mods += "</modulators>";
        Instrument in = load("over", head("", "<group>" + S + "</group>", mods));
        check(has(in, "mod1_src", 1) && has(in, "mod1_dst", 5) && has(in, "mod2_src", 2) && warned(in, "modulation routes dropped"), "LFO -> pan: LFO 1 / 2 in the matrix, the rest dropped (warned)");
    }
    // 13. envelope modulator -> pitch and cutoff
    {
        std::string mods = "<modulators><envelope attack=\"0.1\" decay=\"0.5\" sustain=\"0\" release=\"0.2\"><binding type=\"amp\" level=\"group\" position=\"0\" parameter=\"GROUP_TUNING\" translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"12\"/><binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"FX_FILTER_FREQUENCY\" translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"4000\"/></envelope></modulators>";
        Instrument in = load("envmod", head("", "<group>" + S + "</group>", "<effects><effect type=\"lowpass\" frequency=\"800\"/></effects>" + mods));
        check(!in.zones.empty() && in.zones[0].pitch_env.set && in.zones[0].pitch_env_depth > 0 && in.zones[0].filter.env.set, "envelope -> pitch env + filter env");
    }
    // 14. tags: polyphony 1 on a subset, volume, disabled
    {
        std::string tags = "<tags><tag name=\"a\" polyphony=\"1\"/><tag name=\"b\" volume=\"0.5\"/><tag name=\"c\" enabled=\"false\"/></tags>";
        Instrument in = load("tags", head("", "<group tags=\"a\">" + S + "</group><group tags=\"b\"><sample path=\"s.wav\" rootNote=\"50\"/></group><group tags=\"c\"><sample path=\"s.wav\" rootNote=\"40\"/></group>", tags));
        bool mono_a = false, half_b = false, c_gone = true;
        for (const Zone &z : in.zones) {
            if (z.root == 69) mono_a = z.exclusive_group > 0 && z.off_by == z.exclusive_group;
            if (z.root == 50) half_b = std::fabs(z.gain_db + 6.02) < 0.05;
            if (z.root == 40) c_gone = false;
        }
        check(mono_a, "tag polyphony 1 on one group -> its zones choke each other");
        check(half_b, "tag volume 0.5 -> -6 dB");
        check(c_gone, "tag disabled -> zones left out");
    }
    // 15/16/17. choke, release-trigger decay, legato drop, envelope options
    {
        Instrument a = load("rtd", head("", "<group>" + S + "<sample path=\"s.wav\" rootNote=\"69\" trigger=\"release\" releaseTriggerDecay=\"3dB\"/><sample path=\"s.wav\" rootNote=\"70\" previousNote=\"F2\"/></group>"));
        check(a.zones.size() == 2 && warned(a, "release trigger decay") && warned(a, "legato transition"), "release decay warned, legato sample dropped");
        Instrument b = load("envopt", head("", "<group><sample path=\"s.wav\" rootNote=\"69\" ampEnvEnabled=\"false\"/><sample path=\"s.wav\" rootNote=\"70\" ampEnvDelay=\"0.1\" ampEnvHold=\"0.2\" loopEnabled=\"true\" loopStart=\"100\" loopEnd=\"900\" loopDuringRelease=\"false\" reversed=\"true\"/></group>"));
        bool one = false, dh = false;
        for (const Zone &z : b.zones) {
            if (z.root == 69) one = z.one_shot;
            if (z.root == 70) dh = std::fabs(z.amp_env.delay - 0.1) < 1e-6 && std::fabs(z.amp_env.hold - 0.2) < 1e-6 && !z.loops.empty() && z.loops[0].until_release && z.reverse;
        }
        check(one, "ampEnvEnabled false -> one shot");
        check(dh, "ampEnvDelay / Hold, loopDuringRelease false, reversed");
    }
    // 19. glide on a monophonic preset; poly -> warned
    {
        Instrument a = load("glide", head("glideTime=\"0.3\" glideMode=\"legato\" tags=\"m\"", "<group>" + S + "</group>", "<tags><tag name=\"m\" polyphony=\"1\"/></tags>"));
        check(a.polyphony == 1 && has(a, "glide", 300) && has(a, "voice_mode", 2), "glide 0.3 s, mono -> Legato, 300 ms");
        Instrument b = load("glidep", head("glideTime=\"0.3\"", "<group>" + S + "</group>"));
        check(warned(b, "glide needs a monophonic"), "glide on a poly preset: warned");
        Instrument c = load("bend", head("pitchBendRange=\"12\"", "<group>" + S + "</group>"));
        check(has(c, "bend", 12) && !c.zones.empty() && c.zones[0].bend_up == 1200, "pitchBendRange 12");
    }
    // 20. oscillators: saw only (a synth preset, no zones), sine -12 = sub, noise; FM; pluck
    {
        Instrument a = load("osc", head("", "<group><oscillator waveform=\"saw\"/></group><group groupTuning=\"-12\"><oscillator waveform=\"sine\"/></group><group volume=\"0.5\"><oscillator waveform=\"noise\"/></group>"));
        check(a.zones.empty() && has(a, "syn_on", 1) && has(a, "syn_sub_on", 1) && has(a, "syn_noise_on", 1), "oscillators -> SYNTH (saw, sub, noise); loads without zones");
        Instrument b = load("fm", head("", "<group fmAlgorithm=\"5\" fmOp2Ratio=\"2\" fmOp2Level=\"0.5\"><oscillator waveform=\"fm6op\"/></group>"));
        check(has(b, "syn_on", 1) && get(b, "syn_fm") > 40 && warned(b, "2-operator"), "FM -> 2-op (FM 50)");
        Instrument c = load("pluck", head("", "<group><oscillator waveform=\"pluck1\" damping=\"0.2\"/></group>"));
        check(has(c, "syn_on", 1) && warned(c, "pluck1"), "pluck1 approximated, warned");
    }
    // 22. routing: main + bus at 0.2 x 0.5 -> +0.83 dB
    {
        Instrument in = load("route", head("", "<group output1Target=\"MAIN_OUTPUT\" output1Volume=\"1\" output2Target=\"BUS_1\" output2Volume=\"0.2\">" + S + "</group>", "<buses><bus busVolume=\"0.5\" output1Target=\"MAIN_OUTPUT\"/></buses>"));
        check(!in.zones.empty() && std::fabs(in.zones[0].gain_db - 20 * std::log10(1.1)) < 0.05 && warned(in, "bus and aux"), "bus routing flattened (+0.83 dB), warned");
    }
    // 23. note sequences / arpeggiator: reported
    {
        Instrument in = load("seq", head("", "<group>" + S + "</group>", "<noteSequences><sequence name=\"a\" length=\"4\"><note position=\"0\" length=\"1\" note=\"60\" velocity=\"1\"/></sequence></noteSequences>"));
        check(!in.zones.empty() && warned(in, "note sequences are not loaded"), "note sequences: warned");
    }
    // 25. translation table on a 0..1 knob -> filter cutoff 1100 Hz
    {
        std::string ui = "<ui><tab><labeled-knob minValue=\"0\" maxValue=\"1\" value=\"0.5\"><binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"FX_FILTER_FREQUENCY\" translation=\"table\" translationTable=\"0,33;0.3,150;0.4,450;0.5,1100;0.7,4100;0.9,11000;1.0001,22000\"/></labeled-knob></tab></ui>";
        Instrument in = load("table", head("", "<group>" + S + "</group>", "<effects><effect type=\"lowpass\" frequency=\"22000\"/></effects>" + ui));
        check(!in.zones.empty() && std::fabs(in.zones[0].filter.cutoff - 1100) < 1, "translation table: knob 0.5 -> 1100 Hz");
    }
    // 26. malformed
    {
        std::string err;
        load("bad", "<DecentSampler><ui/></DecentSampler>", &err);
        check(err.find("no <groups>") != std::string::npos, "no <groups>: refused with the reason");
        Instrument in = load("nan", head("volume=\"nan\"", "<group><sample path=\"s.wav\" rootNote=\"x\" loNote=\"inf\" attack=\"-5\"/></group>"));
        check(!in.zones.empty() && std::isfinite(in.gain_db) && in.zones[0].amp_env.attack >= 0, "NaN / bad attributes: loads, finite");
    }
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
