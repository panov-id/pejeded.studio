#pragma once
// studio.db (spec section 16). One writer thread; audio threads never touch it.
// Without a database (missing dir, damaged, newer schema) the app works and says so.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;

namespace pejeded {

class Store {
public:
    enum class State { Closed, Open, Damaged, Newer, Failed };
    static constexpr int kSchema = 1;

    Store() = default;
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // Opens or creates `file`; runs quick_check. Damaged: the file is left as it is, state()==Damaged.
    State open(const std::string& file);
    State state() const { return st; }
    std::string stateText() const;
    void close();

    // Damaged file: rename to studio.db.broken-DATE and start an empty one, or take the last backup.
    State startEmpty();
    State restoreLastBackup();

    // Queue (never blocks the caller long); the writer thread writes once a second in one transaction.
    void event(const std::string& kind, const std::string& data);
    void flush();  // write the queue now (tests, exit)

    // Forever tables into backups/studio-YYYY-MM-DD.db; keeps the last 7 days. Returns the file or "".
    std::string backup(const std::string& date = "");
    std::vector<std::string> backups() const;

    // Read-only connection: a SELECT, up to 200 rows, as text lines. Errors come back as one line "error: ...".
    std::vector<std::string> query(const std::string& sql) const;
    std::vector<std::string> info() const;  // size and rows per table

    std::string file() const { return path; }

private:
    void writer();
    bool exec(const char* sql);
    std::string path;
    State st = State::Closed;
    sqlite3* db = nullptr;
    std::mutex m;
    std::condition_variable cv;
    struct Ev { std::int64_t t; std::string kind, data; };
    std::deque<Ev> queue;
    std::thread th;
    std::atomic<bool> stopping{false};
    std::atomic<int> pending{0};
};

} // namespace pejeded
