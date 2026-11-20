#pragma once
#include <string>
#include <utility>
#include <vector>
// settings.json (spec section 16): unknown keys are kept, missing ones get defaults,
// every change is saved through a temp file and a rename, a broken file becomes settings.broken.json,
// a file from a newer version is never overwritten.
#include <juce_core/juce_core.h>

#include "app/Console.h"

namespace pejeded {

class SettingsFile {
public:
    static constexpr int kVersion = 1;
    enum class Loaded { Fresh, Read, Broken, Newer };

    // Loads `file` (or defaults), then defines every leaf as a Settings key "a.b.c" with its check.
    Loaded load(const juce::File& file, Settings& s);
    // Writes the current values back; false (and nothing written) for a newer-version file.
    bool save(const Settings& s);
    juce::File file() const { return path; }
    static juce::var defaults();
    // Every leaf key with its default value as text (Reset settings).
    static std::vector<std::pair<std::string, std::string>> defaultValues();

private:
    juce::File path;
    juce::var tree;
    bool readOnly = false;
};

} // namespace pejeded
