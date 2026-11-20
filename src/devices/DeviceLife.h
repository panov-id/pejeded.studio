#pragma once
// Device life (spec 6, "Потеря устройства"): Absent -> Opening -> Running -> Lost -> Opening.
// Pure logic: the app reports once a second whether the device is open and whether an open try
// worked; this says what to do and what to show. No clock inside, so tests step it by hand.
#include <string>

namespace pejeded {

class DeviceLife {
public:
    enum class State { Absent, Opening, Running, Lost };
    explicit DeviceLife(std::string name) : who(std::move(name)) {}

    // Called once a second. `open`: the device works now. Returns true when the app should try to open it.
    bool tick(bool open) {
        switch (st) {
            case State::Absent:  st = open ? State::Running : State::Opening; return !open;
            case State::Opening: if (open) st = State::Running; return !open;
            case State::Running: if (!open) { st = State::Lost; ++losses; } return false;
            case State::Lost:    st = open ? State::Running : State::Opening; return !open;
        }
        return false;
    }
    State state() const { return st; }
    int lossCount() const { return losses; }
    bool wasLost() const { return losses > 0; }
    // Bottom bar phrase; "" when there is nothing to warn about.
    std::string warning() const {
        if (st == State::Running) return "";
        if (losses == 0) return who + ": not open, trying again every second";
        return who + ": lost, trying again every second";
    }
    static const char* name(State s) {
        switch (s) {
            case State::Absent: return "absent";
            case State::Opening: return "opening";
            case State::Running: return "running";
            case State::Lost: return "lost";
        }
        return "?";
    }
private:
    std::string who;
    State st = State::Absent;
    int losses = 0;
};

} // namespace pejeded
