#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "emulators/PatternReloader.h"

using namespace pejeded::mc707;

static double energy(Engine& e, int track) {
    std::vector<std::vector<float>> ch(kChannels, std::vector<float>(512));
    std::vector<float*> p;
    for (auto& c : ch) p.push_back(c.data());
    double en = 0;
    for (int i = 0; i < 200; ++i) {
        e.render(p.data(), 512);
        for (float v : ch[static_cast<size_t>(2 + (track - 1) * 2)]) en += double(v) * v;
    }
    return en;
}

TEST_CASE("bad file keeps the old pattern playing, fixed file replaces it") {
    Engine e(48000);
    PatternReloader r;
    REQUIRE(r.apply("NAME A\n[1] DRUMS\nKICK X000 X000 X000 X000\n", "p.txt", &e));
    CHECK(energy(e, 1) > 1.0);

    for (int round = 0; round < 3; ++round) {
        CHECK_FALSE(r.apply("NAME B\n[2] DRUMS\nGONG X\n", "p.txt", &e));
        CHECK(r.lastError().find("p.txt line 3: unknown drum sound") == 0);
        CHECK(r.playingName() == "A");
        CHECK(energy(e, 1) > 1.0);  // sound goes on
        CHECK_FALSE(r.apply("", "p.txt", &e));  // empty or removed file
        CHECK(r.playingName() == "A");
    }

    REQUIRE(r.apply("NAME C\n[2] DRUMS\nKICK X000 X000 X000 X000\n", "p.txt", &e));
    CHECK(r.lastError().empty());
    CHECK(r.playingName() == "C");
    energy(e, 2);  // switch happens at the next bar
    CHECK(energy(e, 2) > 1.0);
    CHECK(energy(e, 1) == 0.0);
}

TEST_CASE("a new engine gets the last good pattern") {
    PatternReloader r;
    r.apply("NAME A\n[4] DRUMS\nKICK X000 X000 X000 X000\n", "p.txt", nullptr);
    Engine e(48000);
    r.attach(e);
    CHECK(energy(e, 4) > 1.0);
}
