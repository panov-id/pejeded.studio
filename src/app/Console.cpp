#include "app/Console.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>

namespace pejeded {

std::vector<std::string> splitWords(const std::string& line) {
    std::vector<std::string> w;
    std::istringstream in(line);
    std::string s;
    while (in >> s) w.push_back(s);
    return w;
}

void Settings::define(const std::string& name, const std::string& def, const std::string& help,
                      std::function<std::string(const std::string&)> check) {
    std::lock_guard<std::mutex> l(m);
    keys[name] = Key{def, help, std::move(check)};
}
bool Settings::has(const std::string& name) const {
    std::lock_guard<std::mutex> l(m);
    return keys.count(name) > 0;
}
std::string Settings::get(const std::string& name) const {
    std::lock_guard<std::mutex> l(m);
    auto it = keys.find(name);
    return it == keys.end() ? std::string() : it->second.value;
}
std::string Settings::set(const std::string& name, const std::string& value) {
    {
        std::lock_guard<std::mutex> l(m);
        auto it = keys.find(name);
        if (it == keys.end()) return "unknown setting " + name;
        if (it->second.check) {
            std::string why = it->second.check(value);
            if (!why.empty()) return name + ": " + why;
        }
        it->second.value = value;
    }
    if (onChange) onChange(name, value);
    return "";
}
std::vector<std::string> Settings::names() const {
    std::lock_guard<std::mutex> l(m);
    std::vector<std::string> n;
    for (const auto& k : keys) n.push_back(k.first);
    return n;
}

Console::Console(Settings& s) : settings(s) {
    clock = [] {
        std::time_t t = std::time(nullptr);
        char b[16];
        std::strftime(b, sizeof b, "%H:%M:%S", std::localtime(&t));
        return std::string(b);
    };
    add("help", "list of commands", [](const auto&, Console& c) {
        for (const auto& k : c.cmds) c.print(k.first + "  " + k.second.help);
        c.print("!command  run a system command, e.g. !arecord -l");
    });
    add("clear", "clear the console window", [](const auto&, Console& c) { c.clear(); });
    add("log", "log N: show the last N log lines", [](const auto& a, Console& c) {
        size_t n = 20;
        if (a.size() > 1) {
            try { n = static_cast<size_t>(std::max(1, std::stoi(a[1]))); }
            catch (...) { c.log("console", "log: number expected", 1); return; }
        }
        auto all = c.lines();
        size_t from = all.size() > n ? all.size() - n : 0;
        for (size_t i = from; i < all.size(); ++i) c.print("  " + all[i].time + " " + all[i].area + " " + all[i].text);
    });
    add("get", "get KEY: read a setting", [](const auto& a, Console& c) {
        if (a.size() < 2) { for (const auto& n : c.settings.names()) c.print(n + " = " + c.settings.get(n)); return; }
        if (!c.settings.has(a[1])) { c.log("console", "unknown setting " + a[1], 1); return; }
        c.print(a[1] + " = " + c.settings.get(a[1]));
    });
    add("set", "set KEY VALUE: change a setting", [](const auto& a, Console& c) {
        if (a.size() < 3) { c.log("console", "set: KEY VALUE expected", 1); return; }
        std::string v = a[2];
        for (size_t i = 3; i < a.size(); ++i) v += " " + a[i];
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);  // set KEY "" clears, set KEY "a b" keeps spaces
        std::string why = c.settings.set(a[1], v);
        if (why.empty()) c.print(a[1] + " = " + v);
        else c.log("console", why, 1);
    });
}

void Console::add(const std::string& name, const std::string& help, Handler h) {
    // a second command with the same name is refused: it once hid the spec 15 status command
    if (cmds.count(name)) { log("console", "command " + name + " is already there; the second one is not added", 2); return; }
    cmds[name] = Cmd{help, std::move(h)};
}

void Console::log(const std::string& area, const std::string& text, int level) {
    std::lock_guard<std::mutex> l(m);
    log_.push_back(LogLine{clock(), area, text, level});
    while (log_.size() > kMaxLog) log_.pop_front();
}

std::vector<LogLine> Console::lines() const {
    std::lock_guard<std::mutex> l(m);
    return {log_.begin(), log_.end()};
}

void Console::clear() {
    std::lock_guard<std::mutex> l(m);
    log_.clear();
}

std::string Console::complete(const std::string& prefix) const {
    std::vector<std::string> all;
    for (const auto& k : cmds) all.push_back(k.first);
    auto w = splitWords(prefix);
    if ((w.size() == 1 && !prefix.empty() && prefix.back() == ' ') || w.size() == 2) {
        if (w[0] == "get" || w[0] == "set") {
            std::string p = w.size() == 2 ? w[1] : "";
            for (const auto& n : settings.names())
                if (n.rfind(p, 0) == 0) return w[0] + " " + n + " ";
        }
        return prefix;
    }
    for (const auto& n : all)
        if (!prefix.empty() && n.rfind(prefix, 0) == 0) return n + " ";
    return prefix;
}

void Console::loadHistory(const std::string& path) {
    histPath = path;
    std::ifstream in(path);
    std::string l;
    while (std::getline(in, l)) if (!l.empty()) hist.push_back(l);
    while (hist.size() > kMaxHistory) hist.pop_front();
}

void Console::saveHistory() const {
    if (histPath.empty()) return;
    std::ofstream out(histPath, std::ios::trunc);
    for (const auto& l : hist) out << l << '\n';
}

void Console::run(const std::string& line) {
    auto w = splitWords(line);
    if (w.empty()) return;
    hist.push_back(line);
    while (hist.size() > kMaxHistory) hist.pop_front();
    saveHistory();
    log("console", "> " + line);
    if (onCommand) onCommand(line);
    if (line.find_first_not_of(' ') != std::string::npos && line[line.find_first_not_of(' ')] == '!') {
        std::string cmd = line.substr(line.find('!') + 1);
        log("system", "run: " + cmd);
        if (systemRunner) systemRunner(cmd, *this);
        else log("system", "system commands are not available here", 1);
        return;
    }
    auto it = cmds.find(w[0]);
    if (it == cmds.end()) {
        log("console", "unknown command " + w[0] + " (type help)", 1);
        return;
    }
    it->second.h(w, *this);
}

} // namespace pejeded
