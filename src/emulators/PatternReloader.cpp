#include "emulators/PatternReloader.h"

namespace pejeded::mc707 {

bool PatternReloader::apply(const std::string& text, const std::string& fileName, Engine* engine) {
    auto r = parsePattern(text);
    std::lock_guard<std::mutex> l(m);
    if (!r.ok) {
        error = fileName + " line " + std::to_string(r.errorLine) + ": " + r.error;
        return false;
    }
    error.clear();
    good = r.pattern;
    hasGood = true;
    if (engine) engine->setPattern(good);
    return true;
}

void PatternReloader::attach(Engine& engine) {
    std::lock_guard<std::mutex> l(m);
    if (hasGood) engine.setPattern(good);
}

std::string PatternReloader::lastError() const {
    std::lock_guard<std::mutex> l(m);
    return error;
}

std::string PatternReloader::playingName() const {
    std::lock_guard<std::mutex> l(m);
    return hasGood ? good.name : std::string();
}

} // namespace pejeded::mc707
