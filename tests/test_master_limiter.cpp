#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/MasterLimiter.h"
#include <cmath>
#include <vector>
using namespace pejeded;

namespace {
// One second of a 440 Hz stereo tone at `amp`, through the limiter in blocks of 256.
float run(MasterLimiter& lim, float amp, std::vector<float>& l, float& reduction) {
    const double rate = 48000;
    std::vector<float> r(48000);
    l.assign(48000, 0);
    for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = amp * float(std::sin(2 * 3.14159265358979 * 440 * double(i) / rate));
    reduction = 0;
    for (int off = 0; off < 48000; off += 256) {
        float* b[2] = {l.data() + off, r.data() + off};
        reduction = std::max(reduction, lim.process(b, 2, std::min(256, 48000 - off)));  // 48000 is not a multiple of 256
    }
    float peak = 0;
    for (float v : l) peak = std::max(peak, std::fabs(v));
    return peak;
}
}

TEST_CASE("limiter: a -12 dB tone passes unchanged and lights no clip mark") {
    MasterLimiter lim; lim.prepare(48000);
    std::vector<float> l; float red;
    float peak = run(lim, 0.251f, l, red);
    CHECK_THAT(20 * std::log10(peak), Catch::Matchers::WithinAbs(-12.0, 0.1));
    CHECK(red < 0.01f);
}

TEST_CASE("limiter: the emulator's 1.27 peak never leaves above the ceiling and reports over 1 dB") {
    MasterLimiter lim; lim.prepare(48000);
    std::vector<float> l; float red;
    float peak = run(lim, 1.27f, l, red);
    CHECK(peak <= MasterLimiter::kCeiling + 1e-6f);
    CHECK(red > 1.0f);  // the clip mark lights above 1 dB (spec 11)
}

TEST_CASE("limiter: the same sound gives the same output") {
    MasterLimiter a, b; a.prepare(48000); b.prepare(48000);
    std::vector<float> l1, l2; float x, y;
    run(a, 1.5f, l1, x); run(b, 1.5f, l2, y);
    CHECK(l1 == l2);
}
