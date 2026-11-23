// Portable C++11 stereo effects for OmniSampler. No plug-in or ROM dependency.
// The catalog uses recovered display labels. Coefficients, routing interpretations
// and presets are original designs. Z-FX slot numbers are local identifiers.
// One instance per stereo insert. prepare and select while processing is stopped.
// select clears tails. Set controls on the audio thread at a block boundary.
// Distinct planar buffers, finite normalized audio, mix=1 for a wet return.
#ifndef OMNISAMPLER_FX_BANK_HPP
#define OMNISAMPLER_FX_BANK_HPP
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace omnisampler {
namespace fxbank {
enum class Engine { Bypass, Reverb, Early, Gated, Reverse, Chorus, Celeste,
    Symphonic, Flanger, Phaser, Rotary, Tremolo, Pan, TriggerPan, Delay, Lcr,
    Echo, PingPong, Multitap, Tape, ChorusDelay, FlangerDelay, PhaserDelay,
    Distortion, Drive, Amp, Enhancer, Eq2, Eq3, Wah, Touch,
    Compressor, Expander, Gate, Correct, Shift };
enum class Route { Mono, Stereo, Cross, Pan };
struct Program {
    const char* bank;
    const char* name;
    int slot, xgMsb, xgLsb;
    Engine engine;
    Route route;
    float size, character;
};
inline const Program* programs(std::size_t& count) {
    static const Program list[] = {
        {"700/reverb", "NO EFFECT", 0, 0, 0, Engine::Bypass, Route::Stereo, 1.0000f, 0.5000f},
        {"700/reverb", "HALL 1", 1, 1, 0, Engine::Reverb, Route::Stereo, 1.3000f, 0.6500f},
        {"700/reverb", "HALL 2", 2, 1, 1, Engine::Reverb, Route::Stereo, 1.6000f, 0.7200f},
        {"700/reverb", "ROOM 1", 3, 2, 0, Engine::Reverb, Route::Stereo, 0.4800f, 0.3700f},
        {"700/reverb", "ROOM 2", 4, 2, 1, Engine::Reverb, Route::Stereo, 0.6300f, 0.4000f},
        {"700/reverb", "ROOM 3", 5, 2, 2, Engine::Reverb, Route::Stereo, 0.7500f, 0.4500f},
        {"700/reverb", "STAGE 1", 6, 3, 0, Engine::Reverb, Route::Stereo, 0.8300f, 0.4900f},
        {"700/reverb", "STAGE 2", 7, 3, 1, Engine::Reverb, Route::Stereo, 1.0400f, 0.5300f},
        {"700/reverb", "PLATE", 8, 4, 0, Engine::Reverb, Route::Stereo, 0.9000f, 0.7500f},
        {"700/reverb", "WHITE ROOM", 9, 16, 0, Engine::Reverb, Route::Stereo, 0.6300f, 0.6300f},
        {"700/reverb", "TUNNEL", 10, 17, 0, Engine::Reverb, Route::Stereo, 1.7500f, 0.7700f},
        {"700/reverb", "CANYON", 11, 18, 0, Engine::Reverb, Route::Stereo, 2.1000f, 0.7900f},
        {"700/reverb", "BASEMENT", 12, 19, 0, Engine::Reverb, Route::Stereo, 0.5500f, 0.3300f},
        {"700/chorus", "NO EFFECT", 0, 0, 0, Engine::Bypass, Route::Stereo, 1.0000f, 0.5000f},
        {"700/chorus", "CHORUS 1", 1, 65, 0, Engine::Chorus, Route::Stereo, 0.8300f, 0.3600f},
        {"700/chorus", "CHORUS 2", 2, 65, 1, Engine::Chorus, Route::Stereo, 1.0100f, 0.4800f},
        {"700/chorus", "CHORUS 3", 3, 65, 2, Engine::Chorus, Route::Stereo, 1.1900f, 0.6000f},
        {"700/chorus", "CHORUS 4", 4, 65, 8, Engine::Chorus, Route::Stereo, 1.3700f, 0.7200f},
        {"700/chorus", "CELESTE 1", 5, 66, 0, Engine::Celeste, Route::Stereo, 0.8300f, 0.3600f},
        {"700/chorus", "CELESTE 2", 6, 66, 1, Engine::Celeste, Route::Stereo, 1.0100f, 0.4800f},
        {"700/chorus", "CELESTE 3", 7, 66, 2, Engine::Celeste, Route::Stereo, 1.1900f, 0.6000f},
        {"700/chorus", "CELESTE 4", 8, 66, 8, Engine::Celeste, Route::Stereo, 1.3700f, 0.7200f},
        {"700/chorus", "FLANGER 1", 9, 67, 0, Engine::Flanger, Route::Stereo, 0.8300f, 0.3600f},
        {"700/chorus", "FLANGER 2", 10, 67, 1, Engine::Flanger, Route::Stereo, 1.0100f, 0.4800f},
        {"700/chorus", "FLANGER 3", 11, 67, 8, Engine::Flanger, Route::Stereo, 1.1900f, 0.6000f},
        {"700/variation", "NO EFFECT", 0, 0, 0, Engine::Bypass, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "HALL 1", 1, 1, 0, Engine::Reverb, Route::Stereo, 1.3000f, 0.6500f},
        {"700/variation", "HALL 2", 2, 1, 1, Engine::Reverb, Route::Stereo, 1.6000f, 0.7200f},
        {"700/variation", "ROOM 1", 3, 2, 0, Engine::Reverb, Route::Stereo, 0.4800f, 0.3700f},
        {"700/variation", "ROOM 2", 4, 2, 1, Engine::Reverb, Route::Stereo, 0.6300f, 0.4000f},
        {"700/variation", "ROOM 3", 5, 2, 2, Engine::Reverb, Route::Stereo, 0.7500f, 0.4500f},
        {"700/variation", "STAGE 1", 6, 3, 0, Engine::Reverb, Route::Stereo, 0.8300f, 0.4900f},
        {"700/variation", "STAGE 2", 7, 3, 1, Engine::Reverb, Route::Stereo, 1.0400f, 0.5300f},
        {"700/variation", "PLATE", 8, 4, 0, Engine::Reverb, Route::Stereo, 0.9000f, 0.7500f},
        {"700/variation", "DELAY LCR", 9, 5, 0, Engine::Lcr, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "DELAY L,R", 10, 6, 0, Engine::Delay, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "ECHO", 11, 7, 0, Engine::Echo, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "CROSSDELAY", 12, 8, 0, Engine::Delay, Route::Cross, 1.0000f, 0.5000f},
        {"700/variation", "ER1", 13, 9, 0, Engine::Early, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "ER2", 14, 9, 1, Engine::Early, Route::Stereo, 1.6000f, 0.5000f},
        {"700/variation", "GATE REV", 15, 10, 0, Engine::Gated, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "REVRS GATE", 16, 11, 0, Engine::Reverse, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "KARAOKE 1", 17, 20, 0, Engine::Echo, Route::Stereo, 0.3200f, 0.3600f},
        {"700/variation", "KARAOKE 2", 18, 20, 1, Engine::Echo, Route::Stereo, 0.4100f, 0.4800f},
        {"700/variation", "KARAOKE 3", 19, 20, 2, Engine::Echo, Route::Stereo, 0.5000f, 0.6000f},
        {"700/variation", "THRU", 20, 64, 0, Engine::Bypass, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "CHORUS 1", 21, 65, 0, Engine::Chorus, Route::Stereo, 0.8300f, 0.3600f},
        {"700/variation", "CHORUS 2", 22, 65, 1, Engine::Chorus, Route::Stereo, 1.0100f, 0.4800f},
        {"700/variation", "CHORUS 3", 23, 65, 2, Engine::Chorus, Route::Stereo, 1.1900f, 0.6000f},
        {"700/variation", "CHORUS 4", 24, 65, 8, Engine::Chorus, Route::Stereo, 1.3700f, 0.7200f},
        {"700/variation", "CELESTE 1", 25, 66, 0, Engine::Celeste, Route::Stereo, 0.8300f, 0.3600f},
        {"700/variation", "CELESTE 2", 26, 66, 1, Engine::Celeste, Route::Stereo, 1.0100f, 0.4800f},
        {"700/variation", "CELESTE 3", 27, 66, 2, Engine::Celeste, Route::Stereo, 1.1900f, 0.6000f},
        {"700/variation", "CELESTE 4", 28, 66, 8, Engine::Celeste, Route::Stereo, 1.3700f, 0.7200f},
        {"700/variation", "FLANGER 1", 29, 67, 0, Engine::Flanger, Route::Stereo, 0.8300f, 0.3600f},
        {"700/variation", "FLANGER 2", 30, 67, 1, Engine::Flanger, Route::Stereo, 1.0100f, 0.4800f},
        {"700/variation", "FLANGER 3", 31, 67, 8, Engine::Flanger, Route::Stereo, 1.1900f, 0.6000f},
        {"700/variation", "SYMPHONIC", 32, 68, 0, Engine::Symphonic, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "ROTARY SP", 33, 69, 0, Engine::Rotary, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "TREMOLO", 34, 70, 0, Engine::Tremolo, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "AUTO PAN", 35, 71, 0, Engine::Pan, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "PHASER 1", 36, 72, 0, Engine::Phaser, Route::Stereo, 0.8300f, 0.3600f},
        {"700/variation", "PHASER 2", 37, 72, 8, Engine::Phaser, Route::Stereo, 1.0100f, 0.4800f},
        {"700/variation", "DISTORTION", 38, 73, 0, Engine::Distortion, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "OVERDRIVE", 39, 74, 0, Engine::Drive, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "AMP SIM", 40, 75, 0, Engine::Amp, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "3-BAND EQ", 41, 76, 0, Engine::Eq3, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "2-BAND EQ", 42, 77, 0, Engine::Eq2, Route::Stereo, 1.0000f, 0.5000f},
        {"700/variation", "AUTO WAH", 43, 78, 0, Engine::Wah, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "CHORUS / MONO", 0, -1, -1, Engine::Chorus, Route::Mono, 1.0000f, 0.5000f},
        {"Z-FX", "CHORUS / STEREO", 1, -1, -1, Engine::Chorus, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "CHORUS / XOVER", 2, -1, -1, Engine::Chorus, Route::Cross, 1.0000f, 0.5000f},
        {"Z-FX", "FLANGER / MONO", 3, -1, -1, Engine::Flanger, Route::Mono, 1.0000f, 0.5000f},
        {"Z-FX", "FLANGER / STEREO", 4, -1, -1, Engine::Flanger, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "FLANGER / PAN", 5, -1, -1, Engine::Flanger, Route::Pan, 1.0000f, 0.5000f},
        {"Z-FX", "FLANGER / XOVER", 6, -1, -1, Engine::Flanger, Route::Cross, 1.0000f, 0.5000f},
        {"Z-FX", "PHASER / MONO", 7, -1, -1, Engine::Phaser, Route::Mono, 1.0000f, 0.5000f},
        {"Z-FX", "PHASER / STEREO", 8, -1, -1, Engine::Phaser, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "PHASER / PAN", 9, -1, -1, Engine::Phaser, Route::Pan, 1.0000f, 0.5000f},
        {"Z-FX", "PHASER / XOVER", 10, -1, -1, Engine::Phaser, Route::Cross, 1.0000f, 0.5000f},
        {"Z-FX", "DELAY / MONO", 11, -1, -1, Engine::Delay, Route::Mono, 1.0000f, 0.5000f},
        {"Z-FX", "DELAY / STEREO", 12, -1, -1, Engine::Delay, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "DELAY / PAN", 13, -1, -1, Engine::Delay, Route::Pan, 1.0000f, 0.5000f},
        {"Z-FX", "DELAY / XOVER", 14, -1, -1, Engine::Delay, Route::Cross, 1.0000f, 0.5000f},
        {"Z-FX", "DELAY / PING PONG", 15, -1, -1, Engine::PingPong, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "DELAY / MULTI TAP", 16, -1, -1, Engine::Multitap, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "PAN / AUTO PAN", 17, -1, -1, Engine::Pan, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "PAN / TRIGGER PAN", 18, -1, -1, Engine::TriggerPan, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "COMPRESSOR/LIMITER", 19, -1, -1, Engine::Compressor, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "DISTORTION", 20, -1, -1, Engine::Distortion, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "ENHANCER", 21, -1, -1, Engine::Enhancer, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "NO EFFECT", 22, -1, -1, Engine::Bypass, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "EXPANDER", 23, -1, -1, Engine::Expander, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "TAPE ECHO", 24, -1, -1, Engine::Tape, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "DIGITAL EQ", 25, -1, -1, Engine::Eq3, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "ROTARY SPEAKER", 26, -1, -1, Engine::Rotary, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "CHORUS+DELAY", 27, -1, -1, Engine::ChorusDelay, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "FLANGER+DELAY", 28, -1, -1, Engine::FlangerDelay, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "PHASER+DELAY", 29, -1, -1, Engine::PhaserDelay, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "NOISE GATE", 30, -1, -1, Engine::Gate, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "PITCH CORRECTOR", 31, -1, -1, Engine::Correct, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "PITCH SHIFTER", 32, -1, -1, Engine::Shift, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "WAH / AUTO WAH", 33, -1, -1, Engine::Wah, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "WAH / TOUCH WAH", 34, -1, -1, Engine::Touch, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "REVERB / SMALL ROOM", 35, -1, -1, Engine::Reverb, Route::Stereo, 0.4500f, 0.4000f},
        {"Z-FX", "REVERB / BIG ROOM", 36, -1, -1, Engine::Reverb, Route::Stereo, 0.8800f, 0.4800f},
        {"Z-FX", "REVERB / SMALL HALL", 37, -1, -1, Engine::Reverb, Route::Stereo, 1.1000f, 0.5800f},
        {"Z-FX", "REVERB / BIG HALL", 38, -1, -1, Engine::Reverb, Route::Stereo, 1.7000f, 0.7400f},
        {"Z-FX", "REVERB / LIVE HOUSE", 39, -1, -1, Engine::Reverb, Route::Stereo, 0.9500f, 0.6200f},
        {"Z-FX", "REVERB / STUDIO", 40, -1, -1, Engine::Reverb, Route::Stereo, 0.5800f, 0.3500f},
        {"Z-FX", "REVERB / BRIGHT HALL", 41, -1, -1, Engine::Reverb, Route::Stereo, 1.4000f, 0.7800f},
        {"Z-FX", "REVERB / MEDIUM HALL", 42, -1, -1, Engine::Reverb, Route::Stereo, 1.2500f, 0.6700f},
        {"Z-FX", "REVERB / AUDITORIUM", 43, -1, -1, Engine::Reverb, Route::Stereo, 1.6500f, 0.6400f},
        {"Z-FX", "REVERB / PLATE 1", 44, -1, -1, Engine::Reverb, Route::Stereo, 0.8200f, 0.7400f},
        {"Z-FX", "REVERB / PLATE 2", 45, -1, -1, Engine::Reverb, Route::Stereo, 0.9500f, 0.7800f},
        {"Z-FX", "REVERB / DRUM BOOTH", 46, -1, -1, Engine::Reverb, Route::Stereo, 0.3700f, 0.3400f},
        {"Z-FX", "REVERB / MEDIUM ROOM", 47, -1, -1, Engine::Reverb, Route::Stereo, 0.7200f, 0.4800f},
        {"Z-FX", "REVERB / THEATER", 48, -1, -1, Engine::Reverb, Route::Stereo, 1.3400f, 0.6100f},
        {"Z-FX", "REVERB / WAREHOUSE", 49, -1, -1, Engine::Reverb, Route::Stereo, 1.8200f, 0.7300f},
        {"Z-FX", "REVERB / VOCAL PLATE", 50, -1, -1, Engine::Reverb, Route::Stereo, 0.7600f, 0.6800f},
        {"Z-FX", "REVERB / NON LINEAR", 51, -1, -1, Engine::Gated, Route::Stereo, 1.0000f, 0.5000f},
        {"Z-FX", "REVERB / REVERSE", 52, -1, -1, Engine::Reverse, Route::Stereo, 1.0000f, 0.5000f},
    };
    count = sizeof(list)/sizeof(list[0]);
    return list;
}

// Format 40 from the 700-FX editor. Call when mapping an XG LFO rate.
inline float xgRateHz(int raw) {
    static const float rates[128] = {
            0.00f, 0.04f, 0.08f, 0.12f, 0.16f, 0.21f, 0.25f, 0.29f,
            0.33f, 0.37f, 0.42f, 0.46f, 0.50f, 0.54f, 0.58f, 0.63f,
            0.67f, 0.71f, 0.75f, 0.79f, 0.84f, 0.88f, 0.92f, 0.96f,
            1.00f, 1.05f, 1.09f, 1.13f, 1.17f, 1.22f, 1.26f, 1.30f,
            1.34f, 1.38f, 1.43f, 1.47f, 1.51f, 1.55f, 1.59f, 1.64f,
            1.68f, 1.72f, 1.76f, 1.80f, 1.85f, 1.89f, 1.93f, 1.97f,
            2.01f, 2.06f, 2.10f, 2.14f, 2.18f, 2.22f, 2.27f, 2.31f,
            2.35f, 2.39f, 2.43f, 2.48f, 2.52f, 2.56f, 2.60f, 2.65f,
            2.69f, 2.77f, 2.86f, 2.94f, 3.02f, 3.11f, 3.19f, 3.28f,
            3.36f, 3.44f, 3.53f, 3.61f, 3.70f, 3.86f, 4.03f, 4.20f,
            4.37f, 4.54f, 4.71f, 4.87f, 5.04f, 5.21f, 5.38f, 5.55f,
            5.72f, 6.05f, 6.39f, 6.72f, 7.06f, 7.40f, 7.73f, 8.07f,
            8.41f, 8.74f, 9.08f, 9.42f, 9.75f, 10.0f, 10.7f, 11.4f,
            12.1f, 12.7f, 13.4f, 14.1f, 14.8f, 15.4f, 16.1f, 16.8f,
            17.4f, 18.1f, 19.5f, 20.8f, 22.2f, 23.5f, 24.8f, 26.2f,
            27.5f, 28.9f, 30.2f, 31.6f, 32.9f, 34.3f, 37.0f, 39.7f,
        };
    return rates[std::max(0,std::min(127,raw))];
}

struct Controls {
    float mix = .5f, rateHz = .7f, depth = .65f, feedback = .35f;
    float timeMs = 320.f, tone = .55f, driveDb = 12.f, thresholdDb = -24.f;
    float ratio = 4.f, pitchSemitones = 7.f;
    float lowDb = 0.f, midDb = 0.f, highDb = 0.f;
};
class DelayLine {
    std::vector<float> samples;
    std::size_t head = 0;
public:
    void resize(std::size_t n) { samples.assign(std::max<std::size_t>(2,n),0.f); head=0; }
    void clear() { std::fill(samples.begin(),samples.end(),0.f); head=0; }
    float tap(float frames) const {
        if(samples.size()<2) return 0.f;
        frames=std::max(1.f,std::min(frames,static_cast<float>(samples.size()-2)));
        const auto n=static_cast<std::size_t>(frames);
        const auto i=head>=n ? head-n : head+samples.size()-n;
        const auto j=i ? i-1 : samples.size()-1;
        return samples[i]+(frames-static_cast<float>(n))*(samples[j]-samples[i]);
    }
    // Omni 2.4.30 (CPU): tap() split in two. where() does tap()'s clamp and integer / fraction split once (for delays
    // that only change with a control), at() reads it: the same samples and arithmetic as tap(frames), bit for bit.
    struct Where { std::size_t n=1; float frac=0; bool ok=false; };
    Where where(float frames) const {
        Where w;
        if(samples.size()<2) return w;
        frames=std::max(1.f,std::min(frames,static_cast<float>(samples.size()-2)));
        w.n=static_cast<std::size_t>(frames); w.frac=frames-static_cast<float>(w.n); w.ok=true;
        return w;
    }
    float at(const Where& w) const {
        if(!w.ok) return 0.f;
        const auto i=head>=w.n ? head-w.n : head+samples.size()-w.n;
        const auto j=i ? i-1 : samples.size()-1;
        return samples[i]+w.frac*(samples[j]-samples[i]);
    }
    void push(float v) {
        if(samples.empty()) return;
        samples[head]=std::abs(v)<1.e-24f ? 0.f : v;
        if (++head==samples.size()) head=0;
    }
};
class Rack {
    using Pair=std::array<float,2>;
    static float bounded(float x,float lo,float hi,float fallback) {
        return std::isfinite(x)?std::max(lo,std::min(hi,x)):fallback;
    }
    static float gain(float db) { return std::pow(10.f,.05f*db); }
    // Omni 2.4.30 (CPU): floor without a libm call (armv7 has no rounding instruction: std::floor was a function call
    // several times per sample in every LFO engine). Exactly std::floor: +-0 and values past 2^23 (already whole) as is.
    static float ffloor(float x) {
        if(!(x>-8388608.f && x<8388608.f) || x==0.f) return x;
        const float t=static_cast<float>(static_cast<int>(x));
        return t>x ? t-1.f : t;
    }
    static float frac(float x) { return x-ffloor(x); }
    float sine(float cycles) const {
        float pos=frac(cycles)*1024.f;
        unsigned i=static_cast<unsigned>(pos)&1023u;
        return wave[i]+(pos-ffloor(pos))*(wave[i+1]-wave[i]);
    }
    Program patch{"","",0,-1,-1,Engine::Bypass,Route::Stereo,1,.5f};
    Controls c;
    float fs=0,phase=0,grain=0,ratioNow=1,correction=1;
    float attack=0,release=0,loPole=0,hiPole=0,threshold=0,drive=1;
    float lowGain=1,midGain=1,highGain=1,ratioSet=1,env=0;
    float previous=0,period=0,elapsed=0,triggerSide=1;
    bool armed=false;
    std::array<float,1025> wave{};
    DelayLine mod[2],echo[2],room[8];
    // Omni 2.4.30 (CPU): the reflection / reverb taps depend only on the controls, the program and fs: computed in
    // cache() (from set / select / prepare), not twice per sample per tap (Live II: ER / GATE / REVERSE were 4.4 %)
    DelayLine::Where reflWhere[2][10], roomWhere[8];
    float reflGain[10]{}, phaseInc=0;
    void cache() {
        static const float times[]={.031f,.093f,.153f,.231f,.359f,.461f,.607f,.737f,.859f,.983f};
        static const float ms[]={23.9f,29.7f,33.1f,37.9f,41.1f,43.7f,47.3f,53.1f};
        const Engine e=patch.engine;
        for(unsigned k=0;k<10;++k) {
            float t=times[k];
            float level=e==Engine::Reverse?t:(e==Engine::Gated?.85f:1.f-.75f*t);
            reflGain[k]=(k%3==1?-.27f:.27f)*level;
            float span=(e==Engine::Early?.24f:.75f)*c.timeMs*patch.size;
            for(unsigned ch=0;ch<2;++ch) reflWhere[ch][k]=echo[ch].where(.001f*fs*span*t*(ch?.94f:1.f));
        }
        for(unsigned i=0;i<8;++i) roomWhere[i]=room[i].where(.001f*fs*ms[i]*patch.size);
        phaseInc=fs ? c.rateHz*patch.size/fs : 0.f;
    }
    std::array<float,8> damping{};
    Pair low{},high{},band{},integrator{},echoTone{};
    float allpass[2][6]{};
    float multiplier(float hz) const { return 1.f-std::exp(-6.2831853f*hz/fs); }
    Pair modulation(Pair x,Engine e) {
        Pair y{};
        for(unsigned ch=0;ch<2;++ch) {
            float p=phase+(patch.route==Route::Mono?0.f:.25f*ch);
            float w=.5f+.5f*sine(p);
            if(e==Engine::Phaser || e==Engine::PhaserDelay) {
                float a=.12f+.80f*(1.f-c.depth*w);
                float v=x[ch]+c.feedback*.65f*band[ch];
                for(float& state:allpass[ch]) {
                    float out=state-a*v;
                    state=v+a*out;
                    v=out;
                }
                y[ch]=v; band[ch]=v;
            } else {
                bool flange=e==Engine::Flanger || e==Engine::FlangerDelay;
                float base=flange?.4f:(e==Engine::Rotary?1.8f:9.f);
                float sweep=flange?4.f:9.f;
                y[ch]=mod[ch].tap(.001f*fs*patch.size*(base+sweep*c.depth*w));
                if(e==Engine::Celeste || e==Engine::Symphonic)
                    y[ch]=.5f*y[ch]+.5f*mod[ch].tap(.001f*fs*(18.f+7.f*c.depth*sine(p+.33f)));
                if(e==Engine::Symphonic)
                    y[ch]=.65f*y[ch]+.35f*mod[ch].tap(.001f*fs*(26.f+6.f*c.depth*sine(p+.67f)));
                if(e==Engine::Rotary) y[ch]*=.72f+.28f*sine(p+.25f);
            }
        }
        for(unsigned ch=0;ch<2;++ch)
            mod[ch].push(x[ch]+c.feedback*.75f*y[patch.route==Route::Cross?1-ch:ch]);
        if(patch.route==Route::Cross && e==Engine::Phaser) std::swap(band[0],band[1]);
        return y;
    }
    Pair delays(Pair x,Engine e) {
        Pair y{};
        for(unsigned ch=0;ch<2;++ch) {
            float delay=.001f*fs*c.timeMs*patch.size;
            if(patch.route==Route::Stereo && e!=Engine::PingPong) delay*=ch?.86f:1.f;
            if(e==Engine::Tape) delay*=1.f+.002f*sine(phase);
            y[ch]=echo[ch].tap(delay);
            if(e==Engine::Lcr) {
                float center=.5f*(echo[0].tap(delay*.5f)+echo[1].tap(delay*.5f));
                y[ch]=.65f*y[ch]+.35f*center;
            }
            if(e==Engine::Echo) y[ch]=.75f*y[ch]+.25f*echo[ch].tap(delay*.5f);
            if(e==Engine::Multitap || e==Engine::Tape)
                y[ch]=.5f*y[ch]+.3f*echo[ch].tap(delay*.67f)+.2f*echo[ch].tap(delay*.33f);
        }
        for(unsigned ch=0;ch<2;++ch) {
            float fb=y[(e==Engine::PingPong || patch.route==Route::Cross)?1-ch:ch];
            echoTone[ch]+=(.08f+.88f*c.tone)*(fb-echoTone[ch]);
            float input=e==Engine::PingPong?(ch?0.f:.5f*(x[0]+x[1])):x[ch];
            float v=input+c.feedback*echoTone[ch];
            echo[ch].push(e==Engine::Tape?std::tanh(v):v);
        }
        return y;
    }
    Pair reflections(Pair x,Engine e) {
        Pair y{};
        (void)e;   // Omni 2.4.30: the taps and gains come from cache()
        for(unsigned ch=0;ch<2;++ch) {
            for(unsigned k=0;k<10;++k)
                y[ch]+=reflGain[k]*echo[ch].at(reflWhere[ch][k]);
            echo[ch].push(x[ch]);
        }
        return y;
    }
    Pair reverb(Pair x) {
        float sum=0.f; Pair y{};
        for(unsigned i=0;i<8;++i) {
            float tap=room[i].at(roomWhere[i]);   // Omni 2.4.30: cache()
            damping[i]+=(.10f+.7f*c.tone)*(tap-damping[i]);
            sum+=damping[i];
            y[i%2]+=(i&2?-.33f:.33f)*tap;
        }
        // Orthogonal Householder feedback matrix with diagonal damping.
        float fb=std::min(.985f,.29f+.47f*patch.character+.22f*c.depth+.12f*c.timeMs/1600.f);
        for(unsigned i=0;i<8;++i)
            room[i].push((i&4?-.42f:.42f)*x[i%2]+fb*(.25f*sum-damping[i]));
        return y;
    }
    Pair spectral(Pair x,Engine e) {
        for(unsigned ch=0;ch<2;++ch) {
            float v=x[ch];
            low[ch]+=loPole*(v-low[ch]);
            high[ch]+=hiPole*(v-high[ch]);
            float treble=v-high[ch];
            switch(e) {
            case Engine::Eq2: x[ch]=v+(lowGain-1)*low[ch]+(highGain-1)*treble; break;
            case Engine::Eq3: x[ch]=lowGain*low[ch]+midGain*(high[ch]-low[ch])+highGain*treble; break;
            case Engine::Distortion: x[ch]=std::max(-.8f,std::min(.8f,v*drive)); break;
            case Engine::Drive: x[ch]=std::tanh(v*drive); break;
            case Engine::Amp:
                band[ch]+=(.05f+.55f*c.tone)*(std::tanh((v-.6f*low[ch])*drive)-band[ch]);
                x[ch]=band[ch]; break;
            case Engine::Enhancer: x[ch]=v+.35f*c.depth*(std::tanh(treble*drive+.2f)-std::tanh(.2f)); break;
            default: break;
            }
        }
        return x;
    }
    Pair wah(Pair x,bool touch) {
        float control=touch?std::min(1.f,env*4.f):.5f+.5f*sine(phase);
        float hz=270.f+2600.f*c.depth*control;
        float g=std::tan(3.14159265f*std::min(.4f,hz/fs));
        float q=.95f-.72f*c.tone;
        for(unsigned ch=0;ch<2;++ch) {
            float b=(band[ch]+g*(x[ch]-integrator[ch]))/(1.f+g*(g+q));
            float l=integrator[ch]+g*b;
            band[ch]=2.f*b-band[ch]; integrator[ch]=2.f*l-integrator[ch];
            x[ch]=b*.8f;
        }
        return x;
    }
    Pair pitch(Pair x,bool correct) {
        float mono=.5f*(x[0]+x[1]);
        if(correct) {
            // Hysteretic positive crossings; intended for sustained monophonic input.
            elapsed+=1;
            if(mono<-.005f) armed=true;
            if(armed && previous<=0 && mono>0) {
                if(elapsed>=fs/1100.f && elapsed<=fs/65.f) {
                    period=period==0?elapsed:period+.25f*(elapsed-period);
                    float note=69.f+12.f*std::log2(fs/(440.f*period));
                    correction=std::exp2((std::round(note)-note)/12.f);
                }
                elapsed=0; armed=false;
            }
            if(elapsed>fs*.05f) { correction=1; period=0; }
            previous=mono;
        }
        ratioNow+=attack*((correct?correction:ratioSet)-ratioNow);
        const float window=.045f*fs;
        grain=frac(grain+(1.f-ratioNow)/window);
        float other=frac(grain+.5f),w=.5f-.5f*sine(grain+.25f);
        for(unsigned ch=0;ch<2;++ch) {
            float v=x[ch];
            x[ch]=w*mod[ch].tap(2+grain*window)+(1-w)*mod[ch].tap(2+other*window);
            mod[ch].push(v);
        }
        return x;
    }
public:
    bool prepare(float hz) {
        if(!(hz>=8000 && hz<=192000)) return false;
        fs=hz;
        for(auto& d:mod) d.resize(static_cast<std::size_t>(fs*.06f)+4);
        for(auto& d:echo) d.resize(static_cast<std::size_t>(fs*1.65f)+4);
        for(auto& d:room) d.resize(static_cast<std::size_t>(fs*.115f)+4);
        for(unsigned i=0;i<=1024;++i) wave[i]=std::sin(6.2831853f*i/1024.f);
        attack=1.f-std::exp(-1.f/(fs*.003f)); release=1.f-std::exp(-1.f/(fs*.08f));
        loPole=multiplier(250.f); hiPole=multiplier(2800.f);
        set(c); reset(); return true;
    }
    bool select(std::size_t index) {
        std::size_t n; auto list=programs(n);
        if(index>=n) return false;
        patch=list[index]; reset(); cache(); return true;
    }
    void reset() {
        for(auto& d:mod)d.clear();
        for(auto& d:echo)d.clear();
        for(auto& d:room)d.clear();
        damping.fill(0); low={}; high={}; band={}; integrator={}; echoTone={};
        for(auto& channel:allpass)std::fill(channel,channel+6,0.f);
        phase=grain=env=elapsed=period=previous=0;
        armed=false; triggerSide=correction=ratioNow=1;
    }
    void set(Controls v) {
        const float previousMix=c.mix;
        v.mix=bounded(v.mix,0,1,.5f); v.rateHz=bounded(v.rateHz,0,40,.7f);
        v.depth=bounded(v.depth,0,1,.65f); v.feedback=bounded(v.feedback,-.94f,.94f,.35f);
        v.timeMs=bounded(v.timeMs,1,1600,320); v.tone=bounded(v.tone,0,1,.55f);
        v.ratio=bounded(v.ratio,1,40,4);
        threshold=gain(bounded(v.thresholdDb,-80,0,-24));
        drive=gain(bounded(v.driveDb,0,36,12));
        ratioSet=std::exp2(bounded(v.pitchSemitones,-24,24,7)/12.f);
        lowGain=gain(bounded(v.lowDb,-24,18,0)); midGain=gain(bounded(v.midDb,-24,18,0));
        highGain=gain(bounded(v.highDb,-24,18,0)); c=v;
        cache();   // Omni 2.4.30
        if(fs && ((previousMix>0 && c.mix==0) || (previousMix==0 && c.mix>0))) reset();
    }
    void process(float* left,float* right,std::size_t frames) noexcept {
        Engine e=patch.engine;
        if(!fs || !left || !right || left==right || e==Engine::Bypass || c.mix==0) return;
        for(std::size_t i=0;i<frames;++i) {
            Pair dry{{left[i],right[i]}},x=dry;
            if(patch.route==Route::Mono) x[0]=x[1]=.5f*(x[0]+x[1]);
            float level=std::max(std::abs(x[0]),std::abs(x[1]));
            float oldEnv=env;
            env+=(level>env?attack:release)*(level-env);
            Pair y=x;
            switch(e) {
            case Engine::Reverb: y=reverb(x); break;
            case Engine::Gated: case Engine::Reverse: case Engine::Early: y=reflections(x,e); break;
            case Engine::Chorus: case Engine::Celeste: case Engine::Flanger: case Engine::Phaser:
            case Engine::Symphonic: case Engine::Rotary: y=modulation(x,e); break;
            case Engine::ChorusDelay: case Engine::FlangerDelay: case Engine::PhaserDelay: {
                auto m=modulation(x,e);
                for(unsigned ch=0;ch<2;++ch)m[ch]=.5f*(x[ch]+m[ch]);
                auto d=delays(m,Engine::Delay);
                for(unsigned ch=0;ch<2;++ch)y[ch]=.55f*m[ch]+.45f*d[ch];
                break;
            }
            case Engine::Delay: case Engine::Lcr: case Engine::Echo: case Engine::Tape:
            case Engine::PingPong: case Engine::Multitap: y=delays(x,e); break;
            case Engine::Distortion: case Engine::Drive: case Engine::Amp: case Engine::Enhancer:
            case Engine::Eq2: case Engine::Eq3: y=spectral(x,e); break;
            case Engine::Wah: case Engine::Touch: y=wah(x,e==Engine::Touch); break;
            case Engine::Correct: case Engine::Shift: y=pitch(x,e==Engine::Correct); break;
            case Engine::TriggerPan:
                if(oldEnv<threshold && env>=threshold) triggerSide=-triggerSide;
                y[0]*=1.f-c.depth*.5f*(1+triggerSide);
                y[1]*=1.f-c.depth*.5f*(1-triggerSide); break;
            case Engine::Tremolo:
                for(auto& v:y)v*=1.f-c.depth*(.5f+.5f*sine(phase));
                break;
            case Engine::Compressor: case Engine::Gate: case Engine::Expander: {
                float g=1.f;
                if(e==Engine::Compressor && env>threshold) g=std::pow(threshold/env,1.f-1.f/c.ratio);
                if(e==Engine::Gate) g=std::min(1.f,std::max(0.f,(env/threshold-.5f)*2.f));
                if(e==Engine::Expander && env<threshold) g=std::pow(env/threshold,c.ratio-1.f);
                for(auto& v:y)v*=g;
                break;
            }
            case Engine::Pan: case Engine::Bypass: break;
            }
            if(e==Engine::Pan || patch.route==Route::Pan) {
                float p=c.depth*sine(phase);
                y[0]*=std::sqrt(1.f-p); y[1]*=std::sqrt(1.f+p);
            }
            left[i]=dry[0]+c.mix*(y[0]-dry[0]); right[i]=dry[1]+c.mix*(y[1]-dry[1]);
            phase=frac(phase+phaseInc);   // Omni 2.4.30: cache()
        }
    }
};
} // namespace fxbank
} // namespace omnisampler
#endif
