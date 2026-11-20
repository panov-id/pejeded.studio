#pragma once
// Sound of the MC-707 emulator (spec section 14). Plain generators; beauty is not the goal.
// Channel map (section 6 defaults): 1-2 MIX, 3-18 tracks 1-8 as stereo pairs, 19-20 EXT IN.
// MIX = tracks + EXT IN. Master of the app gets tracks only (section 6, "Monitor").
// render() runs on the audio thread: no allocation, no locks, no files.
#include <atomic>
#include <cstdint>
#include <memory>

#include "emulators/Pattern.h"

namespace pejeded::mc707 {

constexpr int kChannels = 20;
constexpr int kTracks = 8;

class Engine {
public:
    explicit Engine(double sampleRate, std::uint32_t seed = 1);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Off the audio thread. The new pattern starts at the next bar (section 14).
    void setPattern(const Pattern& p);

    void setTrackOn(int track, bool on);  // track 1..8
    bool trackOn(int track) const;
    void setOwnTempo(double bpm);         // 0 = TEMPO from the file
    double tempo() const;
    int currentStep() const;              // playhead for the matrix, -1 before start

    // Audio thread. out: kChannels pointers, each `frames` long.
    // extIn: 2 pointers (deck sum) or nullptr. tracksOnly: 2 pointers or nullptr (goes to Master).
    void render(float* const* out, int frames, const float* const* extIn = nullptr,
                float* const* tracksOnly = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace pejeded::mc707
