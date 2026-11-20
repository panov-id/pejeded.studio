#pragma once
// Applies a new pattern text; a bad text keeps the playing pattern (spec section 14).
// Not for the audio thread.
#include <mutex>
#include <string>
#include "emulators/Mc707Engine.h"

namespace pejeded::mc707 {

class PatternReloader {
public:
    // Returns true when the text parsed and went to the engine (if any).
    bool apply(const std::string& text, const std::string& fileName, Engine* engine);
    // Called when a new engine is made (device start): gives it the last good pattern.
    void attach(Engine& engine);
    std::string lastError() const;
    std::string playingName() const;

private:
    mutable std::mutex m;
    Pattern good;
    bool hasGood = false;
    std::string error;
};

} // namespace pejeded::mc707
