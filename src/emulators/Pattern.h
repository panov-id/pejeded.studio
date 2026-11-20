#pragma once
// MC-707 emulator pattern file (spec section 14). Format approved by the owner: do not change it.
#include <map>
#include <string>
#include <vector>

namespace pejeded::mc707 {

enum class Hit : unsigned char { None = 0, Normal = 1, Accent = 2 };

struct Step {
    Hit hit = Hit::None;
    bool noteStart = false;      // a [note] starts here (note tracks)
    bool noteAccent = false;
    std::vector<int> notes;      // MIDI note numbers, C4 = 60; empty = hold / none
};

struct Line {
    std::string sound;                        // drums: KICK, HAT...; note tracks: empty
    std::vector<Step> steps;                  // expanded to the pattern LENGTH
    std::map<std::string, std::string> params; // long names: PAN, LEVEL, SCATTER...
    std::map<std::string, std::string> formulas; // NOTE, PAN, LEVEL -> text
    int sourceLine = 0;
};

struct Track {
    int number = 0;                           // 1..8
    std::string type;                         // DRUMS, BASS, LEAD, PLUCK, PAD
    std::map<std::string, std::string> params;
    std::vector<Line> lines;
    int sourceLine = 0;
};

struct Pattern {
    std::string name;
    double tempo = 120.0;
    int length = 16;
    int grid = 16;
    std::vector<Track> tracks;
};

struct ParseResult {
    bool ok = false;
    Pattern pattern;
    int errorLine = 0;            // 1-based
    std::string error;            // plain words
};

ParseResult parsePattern(const std::string& text);

// Note name like "C#3" or "a" (octave from defaultOctave) -> MIDI number; -1 if not a note.
int noteToMidi(const std::string& name, int defaultOctave);

} // namespace pejeded::mc707
