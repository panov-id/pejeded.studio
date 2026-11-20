#pragma once
// Start check rows (spec section 15): each row is ok or a problem with a Fix target.
// Pure logic: the app gathers the facts, the overlay and the `check` command show the rows.
#include <string>
#include <vector>

namespace pejeded {

struct StartFacts {
    std::string input;           // devices.main
    bool inputPresent = false;   // emulator or a found device
    bool inputMoving = false;    // levels move
    std::string controller;
    bool controllerPresent = false;
    std::string master, phones;  // names asked in settings ("" = system default)
    bool masterOpen = false, phonesOpen = true;
    std::string tracksFolder;    // "" = not chosen
    bool tracksReadable = false;
    std::string recordFolder;
    bool recordWritable = false;
    double recordFreeGb = 0;
    bool ffmpegFound = false;
    std::string dbState;         // Store::stateText(): "open", "damaged", "newer", "failed", "none"
    double dbSizeMb = 0;
    double dbOldMb = 0;          // detailed rows older than cleanAfterDays
    double cleanAboveMb = 500;
};

struct CheckRow {
    std::string name;
    bool ok = true;
    std::string text;
    std::string fix;             // "settings:devices", "wizard:3", "db:restore|empty", "db:clean", "" = none
};

std::vector<CheckRow> startCheck(const StartFacts& f);

// Bottom bar state phrase (spec 15): "all ok", the first problem, or "N problems: first".
std::string statusPhrase(const std::vector<CheckRow>& rows);
// Tempo path label for the bottom bar: devices.tempoPath "usb" -> "USB", "dj202" -> "DJ-202".
std::string tempoPathLabel(const std::string& tempoPath);

} // namespace pejeded
