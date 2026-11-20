#include <catch2/catch_test_macros.hpp>
#include "app/SettingsFile.h"

using namespace pejeded;

namespace {
struct Dir {
    juce::File d = juce::File::createTempFile("pejeded-settings");
    Dir() { d.createDirectory(); }
    ~Dir() { d.deleteRecursively(); }
    juce::File f() const { return d.getChildFile("settings.json"); }
};
}

TEST_CASE("no file: defaults, then a change survives a restart") {
    Dir d;
    {
        Settings s;
        SettingsFile sf;
        CHECK(sf.load(d.f(), s) == SettingsFile::Loaded::Fresh);
        CHECK(s.get("audioOut.blockSize") == "256");
        CHECK(s.set("audioOut.master.device", "PipeWire Sound Server").empty());
        CHECK(s.set("visual.rate", "60").empty());
        REQUIRE(sf.save(s));
    }
    Settings s;
    SettingsFile sf;
    CHECK(sf.load(d.f(), s) == SettingsFile::Loaded::Read);
    CHECK(s.get("audioOut.master.device") == "PipeWire Sound Server");
    CHECK(s.get("visual.rate") == "60");
    CHECK_FALSE(d.d.getChildFile("settings.json.tmp").exists());
}

TEST_CASE("unknown keys are kept, missing keys get defaults, bad values fall back") {
    Dir d;
    d.f().replaceWithText(R"({"version":1,"future":{"x":5},"visual":{"rate":45},"audioOut":{"blockSize":512}})");
    Settings s;
    SettingsFile sf;
    REQUIRE(sf.load(d.f(), s) == SettingsFile::Loaded::Read);
    CHECK(s.get("visual.rate") == "30");
    CHECK(s.get("audioOut.blockSize") == "512");
    CHECK(s.get("audioOut.cueMix") == "0.5");
    REQUIRE(sf.save(s));
    auto back = juce::JSON::parse(d.f());
    CHECK(static_cast<int>(back["future"]["x"]) == 5);
}

TEST_CASE("checks reject bad values with a reason") {
    Dir d;
    Settings s;
    SettingsFile sf;
    sf.load(d.f(), s);
    CHECK(s.set("audioOut.blockSize", "300") == "audioOut.blockSize: must be 128, 256, 512 or 1024");
    CHECK(s.set("audioOut.monitor", "yes") == "audioOut.monitor: must be true or false");
    CHECK(s.set("devices.tempoPath", "wifi") == "devices.tempoPath: must be usb or dj202");
    CHECK(s.set("audioOut.cueMix", "abc") == "audioOut.cueMix: must be a number");
    CHECK(s.set("audioOut.master.channels", "[1, 2]").empty());
}

TEST_CASE("broken file is renamed, newer file is not overwritten") {
    Dir d;
    d.f().replaceWithText("{ not json");
    {
        Settings s;
        SettingsFile sf;
        CHECK(sf.load(d.f(), s) == SettingsFile::Loaded::Broken);
        CHECK(d.d.getChildFile("settings.broken.json").existsAsFile());
        CHECK(s.get("visual.rate") == "30");
    }
    d.f().replaceWithText(R"({"version":9,"visual":{"rate":60}})");
    Settings s;
    SettingsFile sf;
    CHECK(sf.load(d.f(), s) == SettingsFile::Loaded::Newer);
    CHECK(s.get("visual.rate") == "30");
    CHECK_FALSE(sf.save(s));
    CHECK(d.f().loadFileAsString().contains("\"version\":9"));
}

TEST_CASE("every number and switch in the defaults is saved as a number or a switch, not as text") {
    Dir d;
    Settings s;
    SettingsFile sf;
    sf.load(d.f(), s);
    REQUIRE(sf.save(s));
    auto file = juce::JSON::parse(d.f().loadFileAsString());
    int checked = 0;
    for (const auto& kv : SettingsFile::defaultValues()) {
        juce::var def = SettingsFile::defaults(), got = file;
        for (auto part : juce::StringArray::fromTokens(juce::String(kv.first), ".", "")) {
            def = def.getProperty(juce::Identifier(part), {});
            got = got.getProperty(juce::Identifier(part), {});
        }
        if (def.isString()) continue;
        INFO(kv.first);
        CHECK_FALSE(got.isString());
        CHECK(got.isBool() == def.isBool());
        ++checked;
    }
    CHECK(checked >= 10);  // blockSize, rate, tempo, gains, levels, db limits
}

TEST_CASE("Master gain accepts -24 to 12 dB and refuses text and values outside") {
    Dir d;
    Settings s;
    SettingsFile sf;
    sf.load(d.f(), s);
    CHECK(s.set("audioOut.masterGainDb", "-12").empty());
    CHECK(s.set("audioOut.masterGainDb", "12").empty());
    CHECK(s.set("audioOut.masterGainDb", "-24").empty());
    CHECK_FALSE(s.set("audioOut.masterGainDb", "13").empty());
    CHECK_FALSE(s.set("audioOut.masterGainDb", "-25").empty());
    CHECK_FALSE(s.set("audioOut.masterGainDb", "loud").empty());
    CHECK(s.get("audioOut.masterGainDb") == "-24");  // a refused value leaves the last good one
}
