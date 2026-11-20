#pragma once
// Console core (spec section 15): log, command line, history. No JUCE, no audio thread.
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pejeded {

struct LogLine {
    std::string time;   // HH:MM:SS
    std::string area;   // audio, pattern, console, system...
    std::string text;
    int level = 0;      // 0 info, 1 warning, 2 error
};

// Settings by name with checks (the same checks the Settings window uses).
class Settings {
public:
    struct Key {
        std::string value;
        std::string help;
        std::function<std::string(const std::string&)> check;  // returns "" if ok, else the reason
    };
    void define(const std::string& name, const std::string& def, const std::string& help,
                std::function<std::string(const std::string&)> check);
    bool has(const std::string& name) const;
    std::string get(const std::string& name) const;
    std::string set(const std::string& name, const std::string& value);  // "" ok, else reason
    std::vector<std::string> names() const;
    std::function<void(const std::string& key, const std::string& value)> onChange;  // after a good set

private:
    mutable std::mutex m;
    std::map<std::string, Key> keys;
};

class Console {
public:
    using Handler = std::function<void(const std::vector<std::string>& args, Console& c)>;
    static constexpr size_t kMaxLog = 2000;
    static constexpr size_t kMaxHistory = 500;

    explicit Console(Settings& s);

    void add(const std::string& name, const std::string& help, Handler h);
    void run(const std::string& line);  // parses and runs one command line
    void log(const std::string& area, const std::string& text, int level = 0);
    void print(const std::string& text) { log("console", text); }

    std::vector<LogLine> lines() const;
    void clear();
    const std::deque<std::string>& history() const { return hist; }
    std::string complete(const std::string& prefix) const;  // Tab
    // History file (spec 16: ~/.local/state/pejeded-studio/console-history, up to 500 lines).
    // After loadHistory every command is written back at once; a missing or unwritable file is not an error.
    void loadHistory(const std::string& path);

    // Called for "!cmd"; the app gives a runner that starts a separate process.
    std::function<void(const std::string& cmd, Console& c)> systemRunner;
    std::function<std::string()> clock;  // returns HH:MM:SS; tests set a fixed one
    std::function<void(const std::string& line)> onCommand;  // e.g. the event log in studio.db

private:
    struct Cmd {
        std::string help;
        Handler h;
    };
    Settings& settings;
    std::map<std::string, Cmd> cmds;
    std::string histPath;
    void saveHistory() const;
    mutable std::mutex m;
    std::deque<LogLine> log_;
    std::deque<std::string> hist;
};

std::vector<std::string> splitWords(const std::string& line);

} // namespace pejeded
