#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>
#include "emulators/Pattern.h"

using namespace pejeded::mc707;

static std::string readFile(const std::string& p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static std::string hits(const Line& l, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        Hit h = l.steps[static_cast<size_t>(i)].hit;
        s += h == Hit::None ? '0' : h == Hit::Normal ? 'x' : 'X';
    }
    return s;
}

static const Line& sound(const Track& t, const std::string& name) {
    for (const auto& l : t.lines)
        if (l.sound == name) return l;
    FAIL("no sound " << name);
    return t.lines.front();
}

TEST_CASE("spec DEMO example parses as written in section 14") {
    auto r = parsePattern(readFile(PEJEDED_TEST_DATA "/demo.txt"));
    INFO(r.errorLine << ": " << r.error);
    REQUIRE(r.ok);
    const auto& p = r.pattern;
    CHECK(p.name == "DEMO");
    CHECK(p.tempo == 126.0);
    CHECK(p.length == 64);
    CHECK(p.grid == 16);
    REQUIRE(p.tracks.size() == 5);
    const auto& drums = p.tracks[0];
    CHECK(drums.type == "DRUMS");
    REQUIRE(drums.lines.size() == 4);
    for (const auto& l : drums.lines) CHECK(l.steps.size() == 64);
    CHECK(hits(sound(drums, "KICK"), 16) == "X000x000x000x000");
    CHECK(hits(sound(drums, "KICK"), 64).substr(48) == "X000x000X0x0x0XX");
    CHECK(hits(sound(drums, "TICK"), 8) == "x0x0x0x0");
    CHECK(sound(drums, "TICK").params.at("SCATTER") == "1");
    CHECK(hits(sound(drums, "CLAP"), 16) == "0000x0000000x000");
    CHECK(sound(drums, "CLAP").params.at("PINGPONG") == "3");
    CHECK(sound(drums, "CLAP").params.at("REVERB") == "0.4");
    CHECK(hits(sound(drums, "HAT"), 16) == "x0x0x0x0x0x0x0x0");
    CHECK(sound(drums, "HAT").formulas.at("PAN") == "SIN(32) * 0.8");
    CHECK(sound(drums, "HAT").formulas.at("LEVEL") == "0.5 + KICK * 0.3");

    const auto& bass = p.tracks[1];
    CHECK(bass.type == "BASS");
    CHECK(bass.params.at("OCTAVE") == "2");
    const auto& bs = bass.lines[0].steps;
    CHECK(bs[0].noteStart);
    CHECK(bs[0].noteAccent);
    CHECK(bs[0].notes == std::vector<int>{36});
    CHECK(!bs[1].noteStart);
    CHECK(bs[1].hit == Hit::Normal);
    CHECK(bs[4].hit == Hit::None);
    CHECK(bs[8].notes == std::vector<int>{41});
    CHECK(!bs[8].noteAccent);

    const auto& pluck = p.tracks[2];
    CHECK(pluck.params.at("ARP") == "UP");
    CHECK(pluck.params.at("SPIN") == "8");
    CHECK(pluck.lines[0].steps[0].notes == std::vector<int>{60, 64, 67});
    CHECK(pluck.lines[0].steps[32].notes == std::vector<int>{65, 69, 60});

    const auto& pad = p.tracks[3];
    CHECK(pad.lines[0].steps[0].notes == std::vector<int>{69, 72, 76});
    CHECK(!pad.lines[0].steps[0].noteAccent);
    CHECK(pad.params.at("WIDTH") == "1");

    const auto& lead = p.tracks[4];
    CHECK(lead.params.at("SCALE") == "AMIN");
    CHECK(lead.lines[0].formulas.at("NOTE") == "N*2 % 7 + B");
    CHECK(hits(lead.lines[0], 16) == "X0x0x0x0X0xxx0x0");
}

TEST_CASE("letter case matters only in hits and notes") {
    auto a = parsePattern("name t\nlength 16\n[1] drums\nkick X000 x000 X000 x000 sc\n");
    INFO(a.error);
    REQUIRE(a.ok);
    CHECK(a.pattern.name == "T");
    CHECK(hits(a.pattern.tracks[0].lines[0], 16) == "X000x000X000x000");
}

TEST_CASE("errors name the line in plain words") {
    struct Case {
        const char* text;
        int line;
        const char* part;
    };
    const Case cases[] = {
        {"NAME T\nLENGTH 16\n[1] DRUMS\nKICK X000 X000 X000 X000 X\n", 4, "longer than LENGTH"},
        {"NAME T\nLENGTH 8\n[1] DRUMS\nKICK X\n", 2, "LENGTH must be 16 to 256"},
        {"NAME T\n[1] DRUMS\nGONG X\n", 3, "unknown drum sound"},
        {"NAME T\n[9] DRUMS\nKICK X\n", 2, "track number must be 1 to 8"},
        {"NAME T\n[1] DRUMS\nKICK (10\n", 3, "not closed"},
        {"NAME T\n[1] DRUMS\nKICK (10)4\n", 3, "repeat needs X"},
        {"NAME T\nXX = X000\n[1] DRUMS\nKICK XX\n", 2, "only X"},
        {"NAME T\nKICK = X000\n[1] DRUMS\nKICK X\n", 2, "format word"},
        {"NAME T\n[1] BASS\n1[C]1 0\n10\n", 4, "one step line"},
        {"NAME T\n[1] BASS\n/4\n", 3, "only in DRUMS"},
        {"NAME T\n[1] DRUMS\nKICK X FOO=1\n", 3, "unknown parameter FOO"},
        {"NAME T\n[1] DRUMS\nKICK X SC 0101\n", 3, "steps after parameters"},
        {"[1] DRUMS\nKICK X\n", 1, "NAME is missing"},
        {"SONG LIVE\nDEMO X4\n", 1, "song file"},
    };
    for (const auto& c : cases) {
        auto r = parsePattern(c.text);
        INFO(c.text << " -> " << r.errorLine << ": " << r.error);
        CHECK_FALSE(r.ok);
        CHECK(r.errorLine == c.line);
        CHECK(r.error.find(c.part) != std::string::npos);
    }
}

TEST_CASE("comment after # but sharp inside brackets") {
    auto r = parsePattern("NAME T # x\n[1] LEAD OCT=3\n1[C#]1 0 # held C#3\n");
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK(r.pattern.tracks[0].lines[0].steps[0].notes == std::vector<int>{49});
}

TEST_CASE("built-in default.txt has 8 tracks and parses") {
    auto r = parsePattern(readFile(PEJEDED_RESOURCES "/patterns/default.txt"));
    INFO(r.errorLine << ": " << r.error);
    REQUIRE(r.ok);
    REQUIRE(r.pattern.tracks.size() == 8);
    for (int i = 0; i < 8; ++i) CHECK(r.pattern.tracks[static_cast<size_t>(i)].number == i + 1);
    CHECK(r.pattern.tracks[5].params.at("PHASE") == "INV");
}
