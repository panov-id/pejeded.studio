#include "app/StartCheck.h"

#include <cstdio>

namespace pejeded {
namespace {
std::string mb(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f MB", v);
    return b;
}
std::string orDefault(const std::string& s) { return s.empty() ? "system default" : s; }
}

std::vector<CheckRow> startCheck(const StartFacts& f) {
    std::vector<CheckRow> r;
    if (!f.inputPresent) r.push_back({"Sound in", false, f.input + " is lost", "wizard:1"});
    else if (!f.inputMoving) r.push_back({"Sound in", false, f.input + ": levels do not move", "wizard:1"});
    else r.push_back({"Sound in", true, f.input + ": levels move", ""});

    r.push_back(f.controllerPresent ? CheckRow{"Controller", true, f.controller, ""}
                                    : CheckRow{"Controller", false, f.controller + " is lost", "wizard:2"});

    if (!f.masterOpen) r.push_back({"Master", false, orDefault(f.master) + " is not open", "wizard:3"});
    else r.push_back({"Master", true, orDefault(f.master), ""});
    if (!f.phonesOpen) r.push_back({"Phones", false, f.phones + " is not open", "wizard:3"});
    else r.push_back({"Phones", true, f.phones.empty() ? "same device as Master" : f.phones, ""});

    if (f.tracksFolder.empty()) r.push_back({"Tracks folder", false, "not chosen", "wizard:4"});
    else if (!f.tracksReadable) r.push_back({"Tracks folder", false, f.tracksFolder + " cannot be read", "wizard:4"});
    else r.push_back({"Tracks folder", true, f.tracksFolder, ""});

    if (f.recordFolder.empty()) r.push_back({"Recordings", false, "folder not chosen", "wizard:5"});
    else if (!f.recordWritable) r.push_back({"Recordings", false, f.recordFolder + " cannot be written", "wizard:5"});
    else if (f.recordFreeGb < 10) r.push_back({"Recordings", false, "only " + std::to_string(int(f.recordFreeGb)) + " GB free", "wizard:5"});
    else r.push_back({"Recordings", true, f.recordFolder, ""});
    r.push_back(f.ffmpegFound ? CheckRow{"ffmpeg", true, "found", ""}
                              : CheckRow{"ffmpeg", false, "not found: video is not recorded", ""});

    if (f.dbState == "open") r.push_back({"Database", true, "open, " + mb(f.dbSizeMb), ""});
    else if (f.dbState == "damaged") r.push_back({"Database damaged", false, "Restore takes the last copy, Start empty keeps the damaged file", "db:restore|empty"});
    else if (f.dbState == "newer") r.push_back({"Database", false, "made by a newer version: the app works without it", ""});
    else r.push_back({"Database", false, "not open: the app works without it, statistics are not saved", ""});
    if (f.dbState == "open" && f.dbSizeMb > f.cleanAboveMb && f.dbOldMb > 0)
        r.push_back({"Database " + mb(f.dbSizeMb), false, mb(f.dbOldMb) + " of old statistics can be removed", "db:clean"});
    return r;
}

std::string statusPhrase(const std::vector<CheckRow>& rows) {
    int n = 0; const CheckRow* first = nullptr;
    for (const auto& r : rows) if (!r.ok) { if (!first) first = &r; ++n; }
    if (!first) return "all ok";
    std::string s = first->name + ": " + first->text;
    return n == 1 ? s : std::to_string(n) + " problems, " + s;
}

std::string tempoPathLabel(const std::string& p) {
    if (p == "usb") return "USB";
    if (p == "dj202") return "DJ-202";
    return "Other MIDI";
}

} // namespace pejeded
