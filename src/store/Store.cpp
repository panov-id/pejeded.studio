#include "store/Store.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>

#include "sqlite3.h"

namespace fs = std::filesystem;

namespace pejeded {
namespace {

const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS tracks (
  id INTEGER PRIMARY KEY, fingerprint TEXT UNIQUE NOT NULL,
  path TEXT, title TEXT, duration_sec REAL,
  bpm REAL, first_beat_sec REAL, beats_per_bar INTEGER DEFAULT 4, steady INTEGER, edited INTEGER DEFAULT 0,
  cue_sec REAL, hot1 REAL, hot2 REAL, hot3 REAL, hot4 REAL,
  added_at INTEGER, analyzed_at INTEGER, updated_at INTEGER);
CREATE TABLE IF NOT EXISTS sets (
  id INTEGER PRIMARY KEY, uid TEXT UNIQUE NOT NULL, name TEXT, started_at INTEGER, ended_at INTEGER,
  tracks INTEGER, notes INTEGER, clash_sec INTEGER, lag_sec INTEGER, xruns INTEGER, video_dropped INTEGER);
CREATE TABLE IF NOT EXISTS set_sources (set_id INTEGER, source INTEGER, name TEXT, color TEXT,
  PRIMARY KEY (set_id, source)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS recordings (id INTEGER PRIMARY KEY, set_id INTEGER, started_at INTEGER, ended_at INTEGER, file TEXT, mode TEXT);
CREATE TABLE IF NOT EXISTS plays (
  id INTEGER PRIMARY KEY, uid TEXT UNIQUE NOT NULL, set_id INTEGER, track_id INTEGER, deck INTEGER,
  started_at INTEGER, audible_sec REAL, bpm REAL);
CREATE TABLE IF NOT EXISTS events  (id INTEGER PRIMARY KEY, set_id INTEGER, t INTEGER, kind TEXT, data TEXT);
CREATE TABLE IF NOT EXISTS notes   (id INTEGER PRIMARY KEY, uid TEXT UNIQUE NOT NULL, set_id INTEGER, t INTEGER, rec_sec REAL, text TEXT);
CREATE TABLE IF NOT EXISTS presets (slot INTEGER PRIMARY KEY, name TEXT, look TEXT, camera TEXT, updated_at INTEGER);
CREATE TABLE IF NOT EXISTS bench   (id INTEGER PRIMARY KEY, t INTEGER, result TEXT, suggested TEXT, applied INTEGER);
CREATE TABLE IF NOT EXISTS seconds (set_id INTEGER, t INTEGER, bpm REAL, tempo_src INTEGER, mix_rms REAL, mix_peak REAL,
  frame_avg_ms REAL, frame_p99_ms REAL, frame_max_ms REAL, frames INTEGER, xruns INTEGER, audio_load REAL,
  video_dropped INTEGER, quality INTEGER, flags INTEGER, PRIMARY KEY (set_id, t)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS sound (set_id INTEGER, t INTEGER, source INTEGER, level REAL, peak REAL, x REAL, y REAL, z REAL, width REAL,
  PRIMARY KEY (set_id, t, source)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS clashes (set_id INTEGER, t INTEGER, a INTEGER, b INTEGER, volume REAL, x REAL, y REAL, z REAL,
  PRIMARY KEY (set_id, t, a, b)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS inputs (id INTEGER PRIMARY KEY, set_id INTEGER, t INTEGER, device TEXT, control TEXT, deck INTEGER, value REAL);
CREATE INDEX IF NOT EXISTS inputs_set_t ON inputs (set_id, t);
CREATE INDEX IF NOT EXISTS events_set_t ON events (set_id, t);
CREATE INDEX IF NOT EXISTS plays_track  ON plays (track_id);
)SQL";

const char* kForever[] = {"tracks", "sets", "set_sources", "recordings", "plays", "events", "notes", "presets", "bench"};
const char* kAll[] = {"tracks", "sets", "set_sources", "recordings", "plays", "events", "notes", "presets", "bench",
                      "seconds", "sound", "clashes", "inputs"};

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string today() {
    std::time_t t = std::time(nullptr);
    char b[16];
    std::strftime(b, sizeof b, "%Y-%m-%d", std::localtime(&t));
    return b;
}

std::string quoteSql(const std::string& s) {
    std::string r = "'";
    for (char c : s) { r += c; if (c == '\'') r += '\''; }
    return r + "'";
}

} // namespace

Store::~Store() { close(); }

bool Store::exec(const char* sql) {
    return db && sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

Store::State Store::open(const std::string& file) {
    close();
    path = file;
    std::error_code ec;
    fs::create_directories(fs::path(file).parent_path(), ec);
    if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        db = nullptr;
        return st = State::Failed;
    }
    sqlite3_busy_timeout(db, 2000);
    sqlite3_stmt* s = nullptr;
    std::string check = "error";
    if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        check = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    if (check != "ok") {
        sqlite3_close(db);
        db = nullptr;
        return st = State::Damaged;
    }
    int ver = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        ver = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (ver > kSchema) {
        sqlite3_close(db);
        db = nullptr;
        return st = State::Newer;
    }
    if (!exec("PRAGMA journal_mode = WAL") || !exec("PRAGMA synchronous = NORMAL") || !exec(kSchemaSql) ||
        !exec(("PRAGMA user_version = " + std::to_string(kSchema)).c_str())) {
        sqlite3_close(db);
        db = nullptr;
        return st = State::Failed;
    }
    stopping = false;
    th = std::thread([this] { writer(); });
    return st = State::Open;
}

std::string Store::stateText() const {
    switch (st) {
        case State::Open: return "database open: " + path;
        case State::Damaged: return "Database damaged: " + path;
        case State::Newer: return "database is from a newer version, working without it";
        case State::Failed: return "database cannot be opened, working without it: " + path;
        default: return "no database";
    }
}

void Store::close() {
    if (th.joinable()) {
        flush();
        stopping = true;
        cv.notify_all();
        th.join();
    }
    if (db) sqlite3_close(db);
    db = nullptr;
    if (st == State::Open) st = State::Closed;
}

Store::State Store::startEmpty() {
    if (st != State::Damaged) return st;
    std::error_code ec;
    fs::rename(path, path + ".broken-" + today(), ec);
    fs::remove(path + "-wal", ec);
    fs::remove(path + "-shm", ec);
    return open(path);
}

Store::State Store::restoreLastBackup() {
    if (st != State::Damaged) return st;
    auto list = backups();
    if (list.empty()) return st;
    std::error_code ec;
    fs::rename(path, path + ".broken-" + today(), ec);
    fs::remove(path + "-wal", ec);
    fs::remove(path + "-shm", ec);
    fs::copy_file(list.back(), path, fs::copy_options::overwrite_existing, ec);
    return open(path);
}

void Store::event(const std::string& kind, const std::string& data) {
    if (st != State::Open) return;
    {
        std::lock_guard<std::mutex> l(m);
        queue.push_back(Ev{nowMs(), kind, data});
        ++pending;
    }
}

void Store::flush() {
    std::unique_lock<std::mutex> l(m);
    if (queue.empty() || !th.joinable()) return;
    cv.notify_all();
    cv.wait_for(l, std::chrono::seconds(3), [this] { return queue.empty(); });
}

void Store::writer() {
    std::unique_lock<std::mutex> l(m);
    while (!stopping) {
        cv.wait_for(l, std::chrono::seconds(1));
        if (queue.empty()) continue;
        std::deque<Ev> batch;
        batch.swap(queue);
        l.unlock();
        bool ok = exec("BEGIN");
        sqlite3_stmt* s = nullptr;
        ok = ok && sqlite3_prepare_v2(db, "INSERT INTO events (set_id, t, kind, data) VALUES (NULL, ?, ?, ?)", -1, &s, nullptr) == SQLITE_OK;
        for (const auto& e : batch) {
            if (!ok) break;
            sqlite3_bind_int64(s, 1, e.t);
            sqlite3_bind_text(s, 2, e.kind.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, e.data.c_str(), -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(s) == SQLITE_DONE;
            sqlite3_reset(s);
        }
        sqlite3_finalize(s);
        if (ok) exec("COMMIT");
        else { exec("ROLLBACK"); }
        l.lock();
        if (!ok) for (auto it = batch.rbegin(); it != batch.rend(); ++it) queue.push_front(*it);  // events are not dropped
        pending = static_cast<int>(queue.size());
        cv.notify_all();
    }
}

std::string Store::backup(const std::string& date) {
    if (st != State::Open) return "";
    fs::path dir = fs::path(path).parent_path() / "backups";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path out = dir / ("studio-" + (date.empty() ? today() : date) + ".db");
    fs::remove(out, ec);
    std::string sql = "ATTACH " + quoteSql(out.string()) + " AS b;";
    for (const char* t : kForever) {
        // same columns and keys as the main schema: create empty copies, then fill them
        sql += std::string("CREATE TABLE b.") + t + " AS SELECT * FROM main." + t + " WHERE 0;";
        sql += std::string("INSERT INTO b.") + t + " SELECT * FROM main." + t + ";";
    }
    sql += "PRAGMA b.user_version = " + std::to_string(kSchema) + ";";
    bool ok = exec("BEGIN") && exec(sql.c_str()) && exec("COMMIT");
    if (!ok) exec("ROLLBACK");
    exec("DETACH b");
    if (!ok) return "";
    auto list = backups();
    while (list.size() > 7) { fs::remove(list.front(), ec); list.erase(list.begin()); }
    return out.string();
}

std::vector<std::string> Store::backups() const {
    std::vector<std::string> r;
    std::error_code ec;
    fs::path dir = fs::path(path).parent_path() / "backups";
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        auto n = e.path().filename().string();
        if (n.rfind("studio-", 0) == 0 && e.path().extension() == ".db") r.push_back(e.path().string());
    }
    std::sort(r.begin(), r.end());
    return r;
}

std::vector<std::string> Store::query(const std::string& sql) const {
    std::vector<std::string> out;
    if (st != State::Open) { out.push_back("error: " + stateText()); return out; }
    sqlite3* ro = nullptr;
    if (sqlite3_open_v2(path.c_str(), &ro, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        out.push_back("error: cannot open read-only");
        sqlite3_close(ro);
        return out;
    }
    sqlite3_busy_timeout(ro, 2000);
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(ro, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) {
        out.push_back(std::string("error: ") + sqlite3_errmsg(ro));
        sqlite3_close(ro);
        return out;
    }
    if (!sqlite3_stmt_readonly(s)) {
        out.push_back("error: only SELECT is allowed");
    } else {
        int cols = sqlite3_column_count(s);
        std::string head;
        for (int c = 0; c < cols; ++c) head += (c ? " | " : "") + std::string(sqlite3_column_name(s, c));
        out.push_back(head);
        int rows = 0, rc = 0;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            if (rows == 200) { out.push_back("... more than 200 rows, cut"); break; }
            std::string line;
            for (int c = 0; c < cols; ++c) {
                const unsigned char* v = sqlite3_column_text(s, c);
                line += (c ? " | " : "") + std::string(v ? reinterpret_cast<const char*>(v) : "NULL");
            }
            out.push_back(line);
            ++rows;
        }
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) out.push_back(std::string("error: ") + sqlite3_errmsg(ro));
    }
    sqlite3_finalize(s);
    sqlite3_close(ro);
    return out;
}

std::vector<std::string> Store::info() const {
    std::vector<std::string> out;
    out.push_back(stateText());
    if (st != State::Open) return out;
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    out.push_back("size " + std::to_string(size / 1024) + " KB, backups " + std::to_string(backups().size()));
    for (const char* t : kAll) {
        auto r = query(std::string("SELECT count(*) FROM ") + t);
        out.push_back(std::string(t) + ": " + (r.size() > 1 ? r[1] : r[0]) + " rows");
    }
    return out;
}

} // namespace pejeded
