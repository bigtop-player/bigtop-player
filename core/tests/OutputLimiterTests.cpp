#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "bigtop/Gain.h"
#include "bigtop/OutputLimiter.h"

using Catch::Approx;
using namespace bigtop;

// Bit-exact pass-through below the ceiling: the only effect is the lookahead delay.
TEST_CASE("Below the ceiling the signal passes unchanged, only delayed") {
    OutputLimiter lim(48000); std::vector<float> l(4800), r(4800);
    for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = 0.5f * std::sin(i * 0.05f);
    auto in = l; lim.process(l.data(), r.data(), 4800); const int d = lim.latencyFrames();
    CHECK(d == 72); for (int i = d; i < 4800; ++i) CHECK(l[i] == in[i - d]);
    CHECK(lim.gainReductionDb() == 0.0f);
}

TEST_CASE("An overdriven mix never exceeds the ceiling") {             // +12 dB sine and a sudden jump
    OutputLimiter lim(48000); std::vector<float> l(48000), r(48000);
    for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = (i < 24000 ? 0.1f : 3.98f) * std::sin(i * 0.05f);
    // 48000 is not a multiple of 256: the last block is short, so we never write past the end.
    for (int off = 0; off < 48000; off += 256) lim.process(l.data() + off, r.data() + off, std::min(256, 48000 - off));
    for (float v : l) CHECK(std::abs(v) <= float(dbToGain(-0.3)) + 1e-6f);
    CHECK(lim.gainReductionDb() > 6.0f);
}

// 0.1 s of a 4.0 DC overload, then 1 s at 0.25 (below the ceiling): the release
// (100 ms time constant) must have brought the gain all the way back to unity.
TEST_CASE("Gain recovers after the overload ends") {
    OutputLimiter lim(48000);
    std::vector<float> l(4800, 4.0f), r(4800, 4.0f);
    lim.process(l.data(), r.data(), 4800);
    CHECK(lim.gainReductionDb() == Approx(12.34).margin(0.05));   // 20*log10(4.0 / ceiling)
    std::vector<float> l2(48000, 0.25f), r2(48000, 0.25f);
    lim.process(l2.data(), r2.data(), 48000);
    CHECK(lim.gainReductionDb() == Approx(0).margin(0.1));
}

TEST_CASE("Non-finite samples come out as silence") {
    OutputLimiter lim(48000); const float ceiling = float(dbToGain(-0.3));
    std::vector<float> l(1000, 0.5f), r(1000, 0.5f);
    l[100] = std::numeric_limits<float>::quiet_NaN();
    r[200] = std::numeric_limits<float>::infinity();
    l[300] = -std::numeric_limits<float>::infinity();
    lim.process(l.data(), r.data(), 1000);
    // Flush the delay line so every input sample has come out.
    std::vector<float> l2(100, 0.0f), r2(100, 0.0f);
    lim.process(l2.data(), r2.data(), 100);
    for (const auto* buf : {&l, &r, &l2, &r2})
        for (float v : *buf) { CHECK(std::isfinite(v)); CHECK(std::abs(v) <= ceiling); }
    const int d = lim.latencyFrames();
    CHECK(l[100 + d] == 0.0f); CHECK(r[200 + d] == 0.0f); CHECK(l[300 + d] == 0.0f);
    CHECK(l[400 + d] == 0.5f);   // the bad samples did not drive any gain reduction
}

// The limiter works frame by frame, so how the host slices the audio must not matter.
TEST_CASE("Any block size gives the same output") {
    std::vector<float> src(4800);
    for (size_t i = 0; i < src.size(); ++i) src[i] = (i % 1000 < 500 ? 3.0f : 0.3f) * std::sin(i * 0.07f);
    auto run = [&](int block) {
        OutputLimiter lim(48000); std::vector<float> l = src, r = src;
        for (int off = 0; off < 4800; off += block) lim.process(l.data() + off, r.data() + off, block);
        return l;
    };
    const auto ref = run(4800);
    CHECK(run(1) == ref);
    CHECK(run(64) == ref);
}

// Linked stereo: one gain for both channels, so R = 0.5 * L must survive limiting
// exactly. The ceiling clamp would cut only L (R never reaches it), so this also
// fails whenever the clamp, not the gain ramp, does the limiting. A 50 Hz sine
// falls slowly while above the ceiling (far longer than the 73-frame window),
// which is what used to overflow the sliding-window minimum.
TEST_CASE("Limiting keeps the stereo ratio: the gain, not the clamp, limits") {
    OutputLimiter lim(48000);
    const int n = 48000 * 5 / 2;   // 0.5 s overload, then 2 s at 0.25
    std::vector<float> l(n), r(n);
    for (int i = 0; i < n; ++i) {
        l[i] = (i < 24000 ? 4.0f : 0.25f) * float(std::sin(2.0 * 3.14159265358979 * 50.0 * i / 48000.0));
        r[i] = 0.5f * l[i];
    }
    for (int i = 0; i < n; ++i) lim.process(&l[i], &r[i], 1);
    int broken = 0;
    // Scaling by 0.5 is exact in float, so (0.5x)*g == 0.5*(x*g) bit for bit
    // unless the clamp touched L.
    for (int i = 0; i < n; ++i) if (r[i] != 0.5f * l[i]) ++broken;
    CHECK(broken == 0);
}

TEST_CASE("reset clears the delay line and the gain state") {
    OutputLimiter lim(48000);
    std::vector<float> l(4800, 4.0f), r(4800, 4.0f);
    lim.process(l.data(), r.data(), 4800);
    lim.reset();
    CHECK(lim.gainReductionDb() == 0.0f);
    std::vector<float> l2(100, 0.25f), r2(100, 0.25f);
    lim.process(l2.data(), r2.data(), 100);
    CHECK_FALSE(std::signbit(lim.gainReductionDb()));   // idle publishes +0, not -0
    lim.reset(); std::fill(l2.begin(), l2.end(), 0.25f); std::fill(r2.begin(), r2.end(), 0.25f);
    lim.process(l2.data(), r2.data(), 100);
    CHECK(l2[0] == 0.0f);    // the delay line holds silence, not the old overload
    CHECK(l2[99] == 0.25f);
}
