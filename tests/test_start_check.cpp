#include <catch2/catch_test_macros.hpp>
#include "app/StartCheck.h"

using namespace pejeded;

namespace {
StartFacts good() {
    StartFacts f;
    f.input = "MC-707 (emulator)"; f.inputPresent = true; f.inputMoving = true;
    f.controller = "DJ-202 (emulator)"; f.controllerPresent = true;
    f.masterOpen = true; f.phonesOpen = true;
    f.tracksFolder = "/music"; f.tracksReadable = true;
    f.recordFolder = "/rec"; f.recordWritable = true; f.recordFreeGb = 100; f.ffmpegFound = true;
    f.dbState = "open"; f.dbSizeMb = 4;
    return f;
}
const CheckRow* row(const std::vector<CheckRow>& r, const std::string& name) {
    for (const auto& x : r) if (x.name == name) return &x;
    return nullptr;
}
}

TEST_CASE("all good: every row ok") {
    auto r = startCheck(good());
    REQUIRE(r.size() == 8);
    for (const auto& x : r) CHECK(x.ok);
}

TEST_CASE("each made problem shows in its row with the right Fix") {
    struct Case { void (*make)(StartFacts&); const char* name; const char* fix; };
    const Case cases[] = {
        {[](StartFacts& f) { f.inputPresent = false; }, "Sound in", "wizard:1"},
        {[](StartFacts& f) { f.inputMoving = false; }, "Sound in", "wizard:1"},
        {[](StartFacts& f) { f.controllerPresent = false; }, "Controller", "wizard:2"},
        {[](StartFacts& f) { f.masterOpen = false; }, "Master", "wizard:3"},
        {[](StartFacts& f) { f.phones = "X"; f.phonesOpen = false; }, "Phones", "wizard:3"},
        {[](StartFacts& f) { f.tracksFolder = ""; }, "Tracks folder", "wizard:4"},
        {[](StartFacts& f) { f.tracksReadable = false; }, "Tracks folder", "wizard:4"},
        {[](StartFacts& f) { f.recordWritable = false; }, "Recordings", "wizard:5"},
        {[](StartFacts& f) { f.recordFreeGb = 2; }, "Recordings", "wizard:5"},
        {[](StartFacts& f) { f.ffmpegFound = false; }, "ffmpeg", ""},
        {[](StartFacts& f) { f.dbState = "damaged"; }, "Database damaged", "db:restore|empty"},
        {[](StartFacts& f) { f.dbState = "failed"; }, "Database", ""},
        {[](StartFacts& f) { f.dbState = "newer"; }, "Database", ""},
        {[](StartFacts& f) { f.dbSizeMb = 612; f.dbOldMb = 410; }, "Database 612 MB", "db:clean"},
    };
    for (const auto& c : cases) {
        auto f = good();
        c.make(f);
        auto r = startCheck(f);
        auto* x = row(r, c.name);
        INFO(c.name);
        REQUIRE(x);
        CHECK_FALSE(x->ok);
        CHECK(x->fix == c.fix);
        int bad = 0;
        for (const auto& y : r) bad += y.ok ? 0 : 1;
        CHECK(bad == 1);
    }
}

TEST_CASE("bottom bar phrase: all ok, one problem, several problems") {
    std::vector<CheckRow> rows{{"Master", true, "system default", ""}};
    CHECK(statusPhrase(rows) == "all ok");
    rows.push_back({"Tracks folder", false, "not chosen", "wizard:4"});
    CHECK(statusPhrase(rows) == "Tracks folder: not chosen");
    rows.push_back({"Recordings", false, "folder not chosen", "wizard:5"});
    CHECK(statusPhrase(rows) == "2 problems, Tracks folder: not chosen");
}

TEST_CASE("bottom bar tempo path: every devices.tempoPath value has its own label") {
    CHECK(tempoPathLabel("usb") == "USB");
    CHECK(tempoPathLabel("dj202") == "DJ-202");
    CHECK(tempoPathLabel("anything else") == "Other MIDI");
}
