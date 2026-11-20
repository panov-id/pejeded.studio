#include "emulators/Pattern.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>

namespace pejeded::mc707 {
namespace {

struct ParseError : std::runtime_error {
    int line;
    ParseError(int l, const std::string& m) : std::runtime_error(m), line(l) {}
};

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

// Removes a comment: '#' outside [...] (inside brackets it is a sharp).
std::string stripComment(const std::string& s) {
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '[') ++depth;
        else if (s[i] == ']') depth = std::max(0, depth - 1);
        else if (s[i] == '#' && depth == 0) return s.substr(0, i);
    }
    return s;
}

std::vector<std::string> words(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

const std::set<std::string> kHeader = {"NAME", "TEMPO", "LENGTH", "GRID", "SONG"};
const std::set<std::string> kTypes = {"DRUMS", "BASS", "LEAD", "PLUCK", "PAD"};
const std::set<std::string> kSounds = {"KICK", "SNARE", "CLAP", "TICK", "HAT", "OPENHAT", "TOM", "RIM", "PERC", "NOISE"};
// Words a fragment cannot be named (section 14). B is allowed: in formulas B is always the bar number.
const std::set<std::string> kFormulaWords = {"NOTE", "PAN", "LEVEL", "N", "S", "RND", "SIN"};
const std::map<std::string, std::string> kParamLong = {
    {"P", "PAN"}, {"PAN", "PAN"}, {"W", "WIDTH"}, {"WIDTH", "WIDTH"}, {"LV", "LEVEL"}, {"LEVEL", "LEVEL"},
    {"OCT", "OCTAVE"}, {"OCTAVE", "OCTAVE"}, {"INV", "PHASE"}, {"PHASE", "PHASE"}, {"AP", "AUTOPAN"},
    {"AUTOPAN", "AUTOPAN"}, {"SP", "SPIN"}, {"SPIN", "SPIN"}, {"SC", "SCATTER"}, {"SCATTER", "SCATTER"},
    {"PP", "PINGPONG"}, {"PINGPONG", "PINGPONG"}, {"RV", "REVERB"}, {"REVERB", "REVERB"},
    {"ARP", "ARP"}, {"SCALE", "SCALE"}};
const std::set<std::string> kFlags = {"INV", "SC", "SCATTER"};

bool isParamToken(const std::string& tok) {
    std::string u = upper(tok);
    auto eq = u.find('=');
    std::string key = eq == std::string::npos ? u : u.substr(0, eq);
    if (eq == std::string::npos) return kFlags.count(key) > 0;
    return true; // any KEY=VALUE is a parameter; an unknown key is reported by addParam
}

void addParam(std::map<std::string, std::string>& params, const std::string& tok, int line) {
    std::string u = upper(tok);
    auto eq = u.find('=');
    std::string key = eq == std::string::npos ? u : u.substr(0, eq);
    std::string val = eq == std::string::npos ? "1" : u.substr(eq + 1);
    auto it = kParamLong.find(key);
    if (it == kParamLong.end()) throw ParseError(line, "unknown parameter " + key);
    if (key == "INV") val = "INV";
    if (eq != std::string::npos && val.empty()) throw ParseError(line, "parameter " + key + " has no value");
    params[it->second] = val;
}

int chordRootAndQuality(const std::string& u, std::string& quality) {
    // u is upper-case, e.g. "AMIN", "C7", "F#MAJ"
    size_t i = 1;
    if (i < u.size() && u[i] == '#') ++i;
    quality = u.substr(i);
    return static_cast<int>(i);
}

std::vector<int> parseNotes(const std::string& inside, int octave, bool& accent, int line) {
    std::vector<int> out;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : inside) {
        if (c == ',') { parts.push_back(trim(cur)); cur.clear(); }
        else cur += c;
    }
    parts.push_back(trim(cur));
    if (parts.empty() || parts[0].empty()) throw ParseError(line, "empty note in [ ]");
    accent = std::isupper(static_cast<unsigned char>(parts[0][0])) != 0;
    if (parts.size() == 1) {
        std::string u = upper(parts[0]);
        std::string q;
        int rootLen = chordRootAndQuality(u, q);
        if (q == "MAJ" || q == "MIN" || q == "7") {
            int root = noteToMidi(u.substr(0, static_cast<size_t>(rootLen)), octave);
            if (root < 0) throw ParseError(line, "unknown chord " + parts[0]);
            std::vector<int> iv = q == "MAJ" ? std::vector<int>{0, 4, 7}
                                 : q == "MIN" ? std::vector<int>{0, 3, 7} : std::vector<int>{0, 4, 7, 10};
            for (int d : iv) out.push_back(root + d);
            return out;
        }
    }
    for (auto& p : parts) {
        int n = noteToMidi(p, octave);
        if (n < 0) throw ParseError(line, "unknown note " + p);
        out.push_back(n);
    }
    return out;
}

// Parses step text (spaces already removed) into steps.
struct StepParser {
    const std::string& s;
    size_t i = 0;
    int line;
    int octave;
    bool noteTrack;
    std::vector<Step> parseSeq(bool inGroup) {
        std::vector<Step> out;
        while (i < s.size()) {
            char c = s[i];
            if (c == ')') {
                if (!inGroup) throw ParseError(line, "')' without '('");
                return out;
            }
            if (c == '(') {
                ++i;
                auto inner = parseSeq(true);
                if (i >= s.size() || s[i] != ')') throw ParseError(line, "'(' is not closed");
                ++i;
                if (i >= s.size() || std::toupper(static_cast<unsigned char>(s[i])) != 'X')
                    throw ParseError(line, "repeat needs X and a number after ')'");
                ++i;
                size_t start = i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                if (start == i) throw ParseError(line, "repeat needs a number after X");
                int n = std::stoi(s.substr(start, i - start));
                if (n < 1) throw ParseError(line, "repeat count must be at least 1");
                for (int k = 0; k < n; ++k) out.insert(out.end(), inner.begin(), inner.end());
                continue;
            }
            if (c == '[') {
                if (out.empty() || out.back().hit == Hit::None) throw ParseError(line, "[note] must follow 1, x or X");
                size_t close = s.find(']', i);
                if (close == std::string::npos) throw ParseError(line, "'[' is not closed");
                bool acc = false;
                out.back().notes = parseNotes(s.substr(i + 1, close - i - 1), octave, acc, line);
                out.back().noteStart = true;
                out.back().noteAccent = acc;
                out.back().hit = acc ? Hit::Accent : Hit::Normal;
                i = close + 1;
                continue;
            }
            Step st;
            if (c == '0') st.hit = Hit::None;
            else if (c == '1' || c == 'x') st.hit = Hit::Normal;
            else if (c == 'X') st.hit = Hit::Accent;
            else throw ParseError(line, std::string("unexpected '") + c + "' in steps");
            out.push_back(st);
            ++i;
        }
        if (inGroup) throw ParseError(line, "'(' is not closed");
        return out;
    }
};

std::vector<Step> evenHits(const std::string& u, int length, int line) {
    // "/N" or "/N+K"
    std::string body = u.substr(1);
    int n = 0, k = 0;
    auto plus = body.find('+');
    try {
        n = std::stoi(body.substr(0, plus));
        if (plus != std::string::npos) k = std::stoi(body.substr(plus + 1));
    } catch (...) {
        throw ParseError(line, "even hits are written /N or /N+K");
    }
    if (n < 1 || k < 0) throw ParseError(line, "even hits are written /N or /N+K");
    std::vector<Step> out(static_cast<size_t>(length));
    for (int s = k; s < length; s += n) out[static_cast<size_t>(s)].hit = Hit::Normal;
    return out;
}

void fitLength(std::vector<Step>& steps, int length, int line) {
    if (steps.empty()) throw ParseError(line, "no steps");
    if (static_cast<int>(steps.size()) > length)
        throw ParseError(line, "line has " + std::to_string(steps.size()) + " steps, longer than LENGTH " + std::to_string(length));
    std::vector<Step> out(static_cast<size_t>(length));
    for (int s = 0; s < length; ++s) out[static_cast<size_t>(s)] = steps[static_cast<size_t>(s) % steps.size()];
    // a held note repeated by the cycle starts again at the cycle start
    steps = out;
}

int toInt(const std::string& v, int line, const std::string& what) {
    try {
        size_t pos = 0;
        int n = std::stoi(v, &pos);
        if (pos != v.size()) throw 0;
        return n;
    } catch (...) {
        throw ParseError(line, what + " must be a whole number");
    }
}

} // namespace

int noteToMidi(const std::string& name, int defaultOctave) {
    if (name.empty()) return -1;
    static const std::map<char, int> base = {{'C', 0}, {'D', 2}, {'E', 4}, {'F', 5}, {'G', 7}, {'A', 9}, {'B', 11}};
    char l = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    auto it = base.find(l);
    if (it == base.end()) return -1;
    int n = it->second;
    size_t i = 1;
    if (i < name.size() && name[i] == '#') { ++n; ++i; }
    int oct = defaultOctave;
    if (i < name.size()) {
        std::string rest = name.substr(i);
        if (!std::all_of(rest.begin(), rest.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            return -1;
        oct = std::stoi(rest);
    }
    return (oct + 1) * 12 + n;
}

ParseResult parsePattern(const std::string& text) {
    ParseResult r;
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string l;
        while (std::getline(in, l)) lines.push_back(l);
    }
    struct RawLine { std::string text; int line; };
    std::map<std::string, std::string> fragments;
    std::vector<std::pair<Track, std::vector<RawLine>>> raw;
    bool seenName = false;
    try {
        for (size_t idx = 0; idx < lines.size(); ++idx) {
            int ln = static_cast<int>(idx) + 1;
            std::string t = trim(stripComment(lines[idx]));
            if (t.empty()) continue;
            auto w = words(t);
            std::string first = upper(w[0]);
            if (first == "SONG") throw ParseError(ln, "this is a song file, not a pattern");
            if (t[0] == '[') {
                auto close = t.find(']');
                if (close == std::string::npos) throw ParseError(ln, "track header needs ]");
                Track tr;
                tr.number = toInt(trim(t.substr(1, close - 1)), ln, "track number");
                if (tr.number < 1 || tr.number > 8) throw ParseError(ln, "track number must be 1 to 8");
                for (auto& [other, _] : raw)
                    if (other.number == tr.number) throw ParseError(ln, "track " + std::to_string(tr.number) + " appears twice");
                auto hw = words(t.substr(close + 1));
                if (hw.empty()) throw ParseError(ln, "track type missing");
                tr.type = upper(hw[0]);
                if (!kTypes.count(tr.type)) throw ParseError(ln, "unknown track type " + tr.type);
                for (size_t k = 1; k < hw.size(); ++k) addParam(tr.params, hw[k], ln);
                tr.sourceLine = ln;
                raw.push_back({tr, {}});
                continue;
            }
            if (raw.empty()) {
                if (kHeader.count(first)) {
                    if (w.size() < 2) throw ParseError(ln, first + " has no value");
                    std::string v = t.substr(t.find_first_of(" \t") + 1);
                    v = trim(v);
                    if (first == "NAME") { r.pattern.name = upper(v); seenName = true; }
                    else if (first == "TEMPO") {
                        try { r.pattern.tempo = std::stod(v); } catch (...) { throw ParseError(ln, "TEMPO must be a number"); }
                        if (r.pattern.tempo < 20 || r.pattern.tempo > 300) throw ParseError(ln, "TEMPO must be 20 to 300");
                    } else if (first == "LENGTH") {
                        r.pattern.length = toInt(v, ln, "LENGTH");
                        if (r.pattern.length < 16 || r.pattern.length > 256) throw ParseError(ln, "LENGTH must be 16 to 256");
                    } else if (first == "GRID") {
                        r.pattern.grid = toInt(v, ln, "GRID");
                        if (r.pattern.grid < 1 || r.pattern.grid > 256) throw ParseError(ln, "GRID must be 1 to 256");
                    }
                    continue;
                }
                auto eq = t.find('=');
                if (eq == std::string::npos) throw ParseError(ln, "expected header, fragment or track");
                std::string name = upper(trim(t.substr(0, eq)));
                if (name.empty() || name.find_first_of(" \t") != std::string::npos) throw ParseError(ln, "bad fragment name");
                if (kHeader.count(name) || kTypes.count(name) || kSounds.count(name) || kFormulaWords.count(name) ||
                    kParamLong.count(name))
                    throw ParseError(ln, "fragment name " + name + " is a format word");
                if (name.find_first_not_of('X') == std::string::npos) throw ParseError(ln, "fragment name cannot be only X");
                std::string body = trim(t.substr(eq + 1));
                for (auto& fw : words(body))
                    if (fragments.count(upper(fw))) throw ParseError(ln, "a fragment cannot contain another fragment");
                fragments[name] = body;
                continue;
            }
            raw.back().second.push_back({t, ln});
        }
        if (!seenName) throw ParseError(1, "NAME is missing");
        if (raw.empty()) throw ParseError(static_cast<int>(lines.size()), "no tracks");

        const int L = r.pattern.length;
        for (auto& [tr, rls] : raw) {
            bool drums = tr.type == "DRUMS";
            int octave = 4;
            if (tr.params.count("OCTAVE")) octave = toInt(tr.params["OCTAVE"], tr.sourceLine, "OCT");
            for (auto& rl : rls) {
                auto w = words(rl.text);
                std::string first = upper(w[0]);
                auto eq = rl.text.find('=');
                bool formula = (first == "NOTE" || first == "PAN" || first == "LEVEL") && eq != std::string::npos &&
                               upper(trim(rl.text.substr(0, eq))) == first;
                if (formula) {
                    if (tr.lines.empty()) throw ParseError(rl.line, "formula must follow a step line");
                    if (first == "NOTE" && drums) throw ParseError(rl.line, "NOTE works only in note tracks");
                    tr.lines.back().formulas[first] = upper(trim(rl.text.substr(eq + 1)));
                    continue;
                }
                Line out;
                out.sourceLine = rl.line;
                size_t k = 0;
                if (drums) {
                    if (!kSounds.count(first)) throw ParseError(rl.line, "unknown drum sound " + first);
                    out.sound = first;
                    k = 1;
                    if (tr.lines.size() >= 16) throw ParseError(rl.line, "DRUMS has at most 16 lines");
                } else if (!tr.lines.empty()) {
                    throw ParseError(rl.line, tr.type + " has one step line");
                }
                std::string steps;
                int depth = 0;
                for (; k < w.size(); ++k) {
                    const std::string& tok = w[k];
                    if (depth == 0 && isParamToken(tok)) break;
                    for (char c : tok) { if (c == '(' || c == '[') ++depth; if (c == ')' || c == ']') --depth; }
                    auto frag = fragments.find(upper(tok));
                    if (frag != fragments.end() && upper(tok).find_first_not_of('X') != std::string::npos)
                        steps += frag->second;
                    else
                        steps += tok;
                    steps += ' ';
                }
                for (; k < w.size(); ++k) {
                    if (!isParamToken(w[k])) throw ParseError(rl.line, "steps after parameters: " + w[k]);
                    if (!drums) throw ParseError(rl.line, "parameters of a note track go in its header");
                    addParam(out.params, w[k], rl.line);
                }
                std::string compact;
                for (char c : steps) if (c != ' ' && c != '\t') compact += c;
                if (compact.empty()) throw ParseError(rl.line, "no steps");
                if (compact[0] == '/') {
                    if (!drums) throw ParseError(rl.line, "/N works only in DRUMS");
                    out.steps = evenHits(compact, L, rl.line);
                } else {
                    StepParser sp{compact, 0, rl.line, octave, !drums};
                    out.steps = sp.parseSeq(false);
                    if (drums)
                        for (auto& st : out.steps)
                            if (st.noteStart) throw ParseError(rl.line, "notes work only in note tracks");
                    fitLength(out.steps, L, rl.line);
                }
                tr.lines.push_back(out);
            }
            if (tr.lines.empty()) throw ParseError(tr.sourceLine, "track " + std::to_string(tr.number) + " has no steps");
            r.pattern.tracks.push_back(tr);
        }
        std::sort(r.pattern.tracks.begin(), r.pattern.tracks.end(),
                  [](const Track& a, const Track& b) { return a.number < b.number; });
        r.ok = true;
    } catch (const ParseError& e) {
        r.ok = false;
        r.errorLine = e.line;
        r.error = e.what();
        r.pattern = Pattern{};
    }
    return r;
}

} // namespace pejeded::mc707
