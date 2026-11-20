#include <filesystem>
#include <catch2/catch_test_macros.hpp>
#include "app/Console.h"

using namespace pejeded;

namespace {
struct Fixture {
    Settings s;
    Console c{s};
    Fixture() {
        c.clock = [] { return std::string("12:00:00"); };
        s.define("visual.rate", "60", "picture updates per second", [](const std::string& v) -> std::string {
            try { int n = std::stoi(v); return n == 30 || n == 60 ? "" : "must be 30 or 60"; }
            catch (...) { return "must be a number"; }
        });
    }
    bool has(const std::string& part) {
        for (const auto& l : c.lines()) if (l.text.find(part) != std::string::npos) return true;
        return false;
    }
};
}

TEST_CASE("stage 1 commands exist and help lists them") {
    Fixture f;
    f.c.run("help");
    for (const char* n : {"help", "clear", "log", "get", "set", "!command"}) {
        INFO(n);
        CHECK(f.has(n));
    }
}

TEST_CASE("set checks the value, get reads it") {
    Fixture f;
    f.c.run("set visual.rate 30");
    CHECK(f.s.get("visual.rate") == "30");
    f.c.run("set visual.rate 45");
    CHECK(f.s.get("visual.rate") == "30");
    CHECK(f.has("visual.rate: must be 30 or 60"));
    f.c.run("set no.such 1");
    CHECK(f.has("unknown setting no.such"));
    f.c.run("get visual.rate");
    CHECK(f.has("visual.rate = 30"));
}

TEST_CASE("unknown command, clear, log N, history and Tab") {
    Fixture f;
    f.c.run("frobnicate");
    CHECK(f.has("unknown command frobnicate"));
    for (int i = 0; i < 5; ++i) f.c.log("audio", "line " + std::to_string(i));
    f.c.run("log 2");
    CHECK(f.has("  12:00:00 audio line 4"));
    f.c.run("clear");
    CHECK(f.c.lines().empty());
    CHECK(f.c.history().back() == "clear");
    CHECK(f.c.complete("he") == "help ");
    CHECK(f.c.complete("set vis") == "set visual.rate ");
}

TEST_CASE("log keeps the last 2000 lines, history the last 500") {
    Fixture f;
    for (int i = 0; i < 2100; ++i) f.c.log("t", std::to_string(i));
    CHECK(f.c.lines().size() == Console::kMaxLog);
    CHECK(f.c.lines().front().text == "100");
    for (int i = 0; i < 600; ++i) f.c.run("help");
    CHECK(f.c.history().size() == Console::kMaxHistory);
}

TEST_CASE("!command goes to the system runner, not to the command table") {
    Fixture f;
    std::string got;
    f.c.systemRunner = [&](const std::string& cmd, Console&) { got = cmd; };
    f.c.run("!echo hi");
    CHECK(got == "echo hi");
}

TEST_CASE("a second command with the same name does not replace the first") {
    Settings s;
    Console c{s};
    int first = 0, second = 0;
    c.add("twice", "first", [&](const auto&, Console&) { ++first; });
    c.add("twice", "second", [&](const auto&, Console&) { ++second; });
    c.run("twice");
    CHECK(first == 1);
    CHECK(second == 0);
}

TEST_CASE("history is kept in a file, up to 500 lines, and read back on the next start") {
    auto path = (std::filesystem::temp_directory_path() / "pejeded-console-history-test").string();
    {
        Settings s;
        Console c{s};
        c.loadHistory(path);
        for (int i = 0; i < 510; ++i) c.run("get x" + std::to_string(i));
    }
    Settings s;
    Console c{s};
    c.loadHistory(path);
    REQUIRE(c.history().size() == 500);
    CHECK(c.history().front() == "get x10");
    CHECK(c.history().back() == "get x509");
    std::remove(path.c_str());
}

TEST_CASE("set KEY with two quotes stores an empty value, quotes around a value are dropped") {
    Settings s;
    s.define("out.name", "x", "output", [](const std::string&) { return std::string(); });
    Console c{s};
    c.run("set out.name \"\"");
    CHECK(s.get("out.name").empty());
    c.run("set out.name \"Pipe Wire\"");
    CHECK(s.get("out.name") == "Pipe Wire");
}
