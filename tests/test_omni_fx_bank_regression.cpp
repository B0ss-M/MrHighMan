#ifndef OMNI_FX_BANK_HEADER
#define OMNI_FX_BANK_HEADER "/var/folders/54/8rjtg1k52p32h8jz_h4s4xqh0000gn/T/rea-extracted-afb924ef-0ec2-442f-9285-864ae78f045c/OmniFxBank.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include OMNI_FX_BANK_HEADER

using namespace omnisampler::fxbank;

static int failures = 0;

static void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    DelayLine line;
    check(line.tap(1.0f) == 0.0f, "default DelayLine tap is safe");
    line.push(0.5f);
    line.resize(0);
    check(line.tap(1.0f) == 0.0f, "zero-size DelayLine is normalised");

    std::size_t count = 0;
    const Program *catalog = programs(count);
    std::size_t delay = count;
    for (std::size_t i = 0; i < count; ++i)
        if (catalog[i].engine == Engine::Delay) { delay = i; break; }
    check(delay < count, "delay fixture exists");

    Rack rack;
    check(rack.prepare(44100.0f), "rack prepares");
    check(rack.select(delay), "rack selects delay");
    Controls c;
    c.mix = 1.0f;
    c.feedback = 0.8f;
    c.timeMs = 10.0f;
    rack.set(c);

    float left[1024] = {}, right[1024] = {};
    left[0] = right[0] = 1.0f;
    rack.process(left, right, 1024);

    c.mix = 0.0f;
    rack.set(c);
    std::memset(left, 0, sizeof left);
    std::memset(right, 0, sizeof right);
    rack.process(left, right, 1024);
    c.mix = 1.0f;
    rack.set(c);
    std::memset(left, 0, sizeof left);
    std::memset(right, 0, sizeof right);
    rack.process(left, right, 1024);
    float peak = 0.0f;
    for (float x : left) peak = std::max(peak, std::fabs(x));
    for (float x : right) peak = std::max(peak, std::fabs(x));
    check(peak == 0.0f, "mix off clears and does not revive a stale delay tail");

    std::printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
