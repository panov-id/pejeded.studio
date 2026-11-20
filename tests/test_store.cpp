#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include "sqlite3.h"
#include "store/Store.h"

using namespace pejeded;
namespace fs = std::filesystem;

namespace {
struct TempDir {
    fs::path p;
    TempDir() {
        static int n = 0;
        p = fs::temp_directory_path() / ("pejeded-store-" + std::to_string(::getpid()) + "-" + std::to_string(++n));
        fs::remove_all(p);
        fs::create_directories(p);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(p, ec); }
};
}

TEST_CASE("new database gets the schema, events are written by the writer thread") {
    TempDir d;
    Store s;
    REQUIRE(s.open((d.p / "studio.db").string()) == Store::State::Open);
    s.event("note", "first");
    s.event("console", "help");
    s.flush();
    auto r = s.query("SELECT kind, data FROM events ORDER BY id");
    REQUIRE(r.size() == 3);
    CHECK(r[1] == "note | first");
    CHECK(r[2] == "console | help");
    CHECK(s.query("PRAGMA user_version")[1] == "1");
    CHECK(fs::exists(d.p / "studio.db-wal"));  // WAL journal
    CHECK(s.query("SELECT count(*) FROM sqlite_schema WHERE type='table'")[1] == "13");
}

TEST_CASE("sql is read-only and cut at 200 rows") {
    TempDir d;
    Store s;
    REQUIRE(s.open((d.p / "studio.db").string()) == Store::State::Open);
    CHECK(s.query("DELETE FROM events")[0] == "error: only SELECT is allowed");
    CHECK(s.query("DROP TABLE tracks")[0] == "error: only SELECT is allowed");
    CHECK(s.query("SELEC 1")[0].rfind("error: ", 0) == 0);
    for (int i = 0; i < 250; ++i) s.event("e", std::to_string(i));
    s.flush();
    auto r = s.query("SELECT id FROM events");
    CHECK(r.size() == 202);  // header + 200 + "cut"
    CHECK(r.back() == "... more than 200 rows, cut");
}

TEST_CASE("backup copies forever tables, keeps 7 days") {
    TempDir d;
    Store s;
    REQUIRE(s.open((d.p / "studio.db").string()) == Store::State::Open);
    s.event("note", "keep me");
    s.flush();
    for (int day = 1; day <= 9; ++day) REQUIRE(!s.backup("2026-11-0" + std::to_string(day)).empty());
    auto list = s.backups();
    REQUIRE(list.size() == 7);
    CHECK(fs::path(list.front()).filename() == "studio-2026-11-03.db");
    Store b;
    REQUIRE(b.open(list.back()) == Store::State::Open);
    CHECK(b.query("SELECT data FROM events")[1] == "keep me");
}

TEST_CASE("damaged file: app goes on without the database, Start empty keeps the broken file") {
    TempDir d;
    auto f = (d.p / "studio.db").string();
    {
        std::ofstream o(f, std::ios::binary);
        o << "SQLite format 3";
        for (int i = 0; i < 5000; ++i) o << char(i * 7);
    }
    Store s;
    CHECK(s.open(f) == Store::State::Damaged);
    s.event("x", "y");  // ignored, nothing crashes
    CHECK(s.query("SELECT 1")[0].rfind("error: ", 0) == 0);
    CHECK(s.startEmpty() == Store::State::Open);
    bool broken = false;
    for (const auto& e : fs::directory_iterator(d.p))
        if (e.path().filename().string().rfind("studio.db.broken-", 0) == 0) broken = true;
    CHECK(broken);
}

TEST_CASE("damaged file: Restore takes the last backup") {
    TempDir d;
    auto f = (d.p / "studio.db").string();
    {
        Store s;
        REQUIRE(s.open(f) == Store::State::Open);
        s.event("note", "from backup");
        s.flush();
        REQUIRE(!s.backup("2026-11-18").empty());
    }
    std::error_code ec;
    fs::remove(f + "-wal", ec);
    fs::remove(f + "-shm", ec);
    {
        std::ofstream o(f, std::ios::binary | std::ios::trunc);
        o << "garbage, not a database at all, really";
    }
    Store s;
    REQUIRE(s.open(f) == Store::State::Damaged);
    REQUIRE(s.restoreLastBackup() == Store::State::Open);
    CHECK(s.query("SELECT data FROM events")[1] == "from backup");
}

TEST_CASE("folder that cannot be created: works without the database") {
    Store s;
    CHECK(s.open("/proc/no-such/studio.db") == Store::State::Failed);
    s.event("x", "y");
    CHECK(s.info()[0].rfind("database cannot be opened", 0) == 0);
}

TEST_CASE("database from a newer version is left alone") {
    TempDir d;
    auto f = (d.p / "studio.db").string();
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(f.c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "CREATE TABLE future(x); PRAGMA user_version = 2;", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
    Store s;
    CHECK(s.open(f) == Store::State::Newer);
    Store again;
    CHECK(again.open(f) == Store::State::Newer);  // untouched: still version 2
}
