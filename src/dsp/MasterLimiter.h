#pragma once
// Soft limiter on the Master output (spec 11, Master row). Real-time safe: no allocation, no locks.
// Below the ceiling the signal passes unchanged; above it the gain drops at once (no overshoot)
// and recovers over `releaseMs`. The deepest gain reduction of a block is returned for the clip mark.
#include <algorithm>
#include <cmath>

namespace pejeded {

class MasterLimiter {
public:
    static constexpr float kCeiling = 0.977f;  // -0.2 dBFS
    void prepare(double rate, double releaseMs = 120.0) {
        release = float(std::exp(-1.0 / (rate * releaseMs / 1000.0)));
        gain = 1.0f;
    }
    // Processes `n` frames of `ch` channels in place; returns the deepest reduction in dB (>= 0).
    float process(float* const* data, int ch, int n) {
        float minGain = 1.0f;
        for (int i = 0; i < n; ++i) {
            float peak = 0;
            for (int c = 0; c < ch; ++c) if (data[c]) peak = std::max(peak, std::fabs(data[c][i]));
            float want = peak > kCeiling ? kCeiling / peak : 1.0f;
            gain = want < gain ? want : want - (want - gain) * release;  // instant down, slow up
            for (int c = 0; c < ch; ++c) if (data[c]) data[c][i] *= gain;
            minGain = std::min(minGain, gain);
        }
        return -20.0f * std::log10(minGain);
    }
private:
    float gain = 1.0f, release = 0.999f;
};

} // namespace pejeded
