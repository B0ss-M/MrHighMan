#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../src/engine/omni_fx_bank.hpp"

using omnisampler::fxbank::Controls;
using omnisampler::fxbank::Rack;
using omnisampler::fxbank::programs;

static int failures = 0;

static void check(bool ok, const char *name, std::size_t program = 0) {
    if (!ok) {
        std::fprintf(stderr, "FAIL program=%zu %s\n", program, name);
        ++failures;
    }
}

static Controls hot_controls() {
    Controls c;
    c.mix = 1.0f;
    c.rateHz = 39.7f;
    c.depth = 1.0f;
    c.feedback = 0.94f;
    c.timeMs = 1600.0f;
    c.tone = 1.0f;
    c.driveDb = 36.0f;
    c.thresholdDb = -80.0f;
    c.ratio = 40.0f;
    c.pitchSemitones = 24.0f;
    c.lowDb = 18.0f;
    c.midDb = 18.0f;
    c.highDb = 18.0f;
    return c;
}

int main() {
    std::size_t count = 0;
    const auto *catalog = programs(count);
    check(count > 0, "catalog is not empty");

    Rack invalid;
    check(!invalid.prepare(7999.0f), "prepare rejects unsupported low sample rate");
    check(!invalid.prepare(192001.0f), "prepare rejects unsupported high sample rate");
    check(!invalid.select(count), "select rejects out-of-range program");

    float buffer_left[1024] = {};
    float buffer_right[1024] = {};
    invalid.process(buffer_left, buffer_right, 1024);
    check(buffer_left[0] == 0.0f && buffer_right[0] == 0.0f,
          "processing before prepare is a no-op");

    const Controls controls = hot_controls();
    for (std::size_t i = 0; i < count; ++i) {
        Rack rack;
        check(rack.prepare(44100.0f), "prepare succeeds", i);
        check(rack.select(i), "select succeeds", i);
        rack.set(controls);

        float peak = 0.0f;
        for (int block = 0; block < 16; ++block) {
            for (std::size_t n = 0; n < 1024; ++n) {
                const float x = (block == 0 && n == 0) ? 1.0f : 0.0f;
                buffer_left[n] = x;
                buffer_right[n] = -0.75f * x;
            }
            rack.process(buffer_left, buffer_right, 1024);
            for (std::size_t n = 0; n < 1024; ++n) {
                check(std::isfinite(buffer_left[n]) && std::isfinite(buffer_right[n]),
                      "output is finite", i);
                peak = std::max(peak, std::max(std::fabs(buffer_left[n]), std::fabs(buffer_right[n])));
            }
        }
        check(peak < 100000.0f, "output remains bounded under hot controls", i);

        rack.reset();
        std::memset(buffer_left, 0, sizeof buffer_left);
        std::memset(buffer_right, 0, sizeof buffer_right);
        rack.process(buffer_left, buffer_right, 1024);
        for (std::size_t n = 0; n < 1024; ++n)
            check(buffer_left[n] == 0.0f && buffer_right[n] == 0.0f,
                  "reset followed by silence remains silent", i);

        Controls invalid_controls = controls;
        invalid_controls.mix = INFINITY;
        invalid_controls.rateHz = NAN;
        invalid_controls.feedback = -INFINITY;
        rack.set(invalid_controls);
        std::memset(buffer_left, 0, sizeof buffer_left);
        std::memset(buffer_right, 0, sizeof buffer_right);
        rack.process(buffer_left, buffer_right, 1024);
        for (std::size_t n = 0; n < 1024; ++n)
            check(std::isfinite(buffer_left[n]) && std::isfinite(buffer_right[n]),
                  "non-finite controls retain finite state", i);
    }

    Rack rack;
    rack.prepare(44100.0f);
    rack.select(0);
    rack.process(buffer_left, buffer_left, 1024);
    check(!catalog[count - 1].name || std::strlen(catalog[count - 1].name) > 0,
          "program names remain readable");

    std::printf("catalog=%zu failures=%d\n", count, failures);
    return failures ? 1 : 0;
}
