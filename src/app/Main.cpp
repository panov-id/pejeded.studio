#include "devices/DeviceLife.h"
#include "dsp/MasterLimiter.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

#include "app/AppInfo.h"
#include "app/Console.h"
#include "app/SettingsFile.h"
#include "app/StartCheck.h"
#include "store/Store.h"
#include "emulators/Mc707Engine.h"
#include "emulators/Pattern.h"
#include "emulators/PatternReloader.h"

namespace {
using namespace pejeded;

constexpr int kSources = 9;  // tracks 1-8 + EXT IN (section 6)

// Audio: the emulator renders 20 channels; Master gets the tracks-only sum.
class Audio : public juce::AudioIODeviceCallback {
public:
    std::array<std::atomic<float>, kSources> peak{};
    std::unique_ptr<mc707::Engine> engine;
    mc707::PatternReloader reloader;
    std::atomic<double> ownTempo{0};  // emulator.tempo; kept across engine re-creation
    std::atomic<int> toneLeft{0};
    std::atomic<float> masterPeak{0};  // bottom bar meter, decays like the source meters
    std::atomic<bool> masterClip{false}; // set when the limiter removes over 1 dB (spec 11); the bar clears it
    std::atomic<bool> inputGone{false};  // the sound-in device is not delivering (simulated loss of the emulator)
    std::atomic<float> masterGain{1.0f};  // audioOut.masterGainDb as a factor (spec 11, Master row)
    std::array<std::atomic<float>, 2> masterLR{};  // block peak after the limiter, left and right     // Setup wizard step 3: frames of test tone still to play

    void audioDeviceAboutToStart(juce::AudioIODevice* d) override {
        const int n = std::max(d->getCurrentBufferSizeSamples(), 4096);
        for (auto& c : chans) c.assign(static_cast<size_t>(n), 0.0f);
        for (auto& c : master) c.assign(static_cast<size_t>(n), 0.0f);
        for (int i = 0; i < mc707::kChannels; ++i) chanPtr[static_cast<size_t>(i)] = chans[static_cast<size_t>(i)].data();
        for (int i = 0; i < 2; ++i) masterPtr[static_cast<size_t>(i)] = master[static_cast<size_t>(i)].data();
        capacity = n;
        rate = d->getCurrentSampleRate();
        engine = std::make_unique<mc707::Engine>(d->getCurrentSampleRate());
        reloader.attach(*engine);
        engine->setOwnTempo(ownTempo.load());
        limiter.prepare(rate);
    }
    void audioDeviceStopped() override {  // a closed device shows silence, not its last level
        masterPeak.store(0); for (auto& v : masterLR) v.store(0); for (auto& v : peak) v.store(0);
    }

    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* out, int numOut, int frames,
                                          const juce::AudioIODeviceCallbackContext&) override {
        if (!engine || frames > capacity) {
            for (int c = 0; c < numOut; ++c) if (out[c]) juce::FloatVectorOperations::clear(out[c], frames);
            return;
        }
        engine->render(chanPtr.data(), frames, nullptr, masterPtr.data());
        if (inputGone.load()) {  // a lost input gives no sound and no sources (spec 6)
            for (int c = 0; c < mc707::kChannels; ++c) juce::FloatVectorOperations::clear(chanPtr[size_t(c)], frames);
            for (int c = 0; c < 2; ++c) juce::FloatVectorOperations::clear(masterPtr[size_t(c)], frames);
        }
        for (int c = 0; c < numOut; ++c)
            if (out[c]) juce::FloatVectorOperations::copy(out[c], masterPtr[static_cast<size_t>(c % 2)], frames);
        int tone = toneLeft.load();
        if (tone > 0) {  // 440 Hz at -14 dB on top of the mix
            int n = std::min(tone, frames);
            for (int i = 0; i < n; ++i) {
                float v = 0.2f * float(std::sin(phase));
                phase += 2.0 * juce::MathConstants<double>::pi * 440.0 / rate;
                for (int c = 0; c < numOut; ++c) if (out[c]) out[c][i] += v;
            }
            toneLeft.store(tone - n);
        }
        {
            const float mg = masterGain.load();
            if (std::abs(mg - 1.0f) > 1e-6f) for (int c = 0; c < numOut; ++c) if (out[c]) juce::FloatVectorOperations::multiply(out[c], mg, frames);
            if (limiter.process(out, numOut, frames) > 1.0f) masterClip.store(true);
            float p = 0;
            for (int c = 0; c < 2; ++c) {
                float q = 0;
                if (c < numOut && out[c]) { auto r = juce::FloatVectorOperations::findMinAndMax(out[c], frames); q = std::max(-r.getStart(), r.getEnd()); }
                masterLR[size_t(c)].store(q);
                p = std::max(p, q);
            }
            masterPeak.store(p);
        }
        for (int s = 0; s < kSources; ++s) {
            float p = 0;
            for (int side = 0; side < 2; ++side) {
                auto r = juce::FloatVectorOperations::findMinAndMax(chanPtr[static_cast<size_t>(2 + s * 2 + side)], frames);
                p = std::max({p, -r.getStart(), r.getEnd()});
            }
            float old = peak[static_cast<size_t>(s)].load();
            peak[static_cast<size_t>(s)].store(std::max(p, old * 0.9f));
        }
    }

private:
    std::array<std::vector<float>, mc707::kChannels> chans;
    std::array<std::vector<float>, 2> master;
    std::array<float*, mc707::kChannels> chanPtr{};
    std::array<float*, 2> masterPtr{};
    int capacity = 0;
    double rate = 48000, phase = 0;
    pejeded::MasterLimiter limiter;
};

// Sources panel: 9 meters (section 15). A switched-off track shows dim.
class Sources : public juce::Component, private juce::Timer {
public:
    explicit Sources(Audio& a) : audio(a) { startTimerHz(30); }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xcc101010));
        const float w = getWidth() / float(kSources);
        for (int s = 0; s < kSources; ++s) {
            bool on = s == 8 || !audio.engine || audio.engine->trackOn(s + 1);
            float v = std::min(1.0f, shown[static_cast<size_t>(s)]);
            auto r = juce::Rectangle<float>(s * w + 6, 8, w - 12, getHeight() - 32.0f);
            g.setColour(juce::Colours::darkgrey);
            g.drawRect(r);
            g.setColour(on ? juce::Colours::limegreen : juce::Colours::darkgreen.withAlpha(0.4f));
            g.fillRect(r.removeFromBottom(r.getHeight() * v));
            g.setColour(on ? juce::Colours::white : juce::Colours::grey);
            g.drawText(s == 8 ? "EXT" : juce::String(s + 1), juce::Rectangle<float>(s * w, getHeight() - 22.0f, w, 18),
                       juce::Justification::centred);
        }
    }
    void mouseUp(const juce::MouseEvent& e) override {
        int s = int(e.position.x / (getWidth() / float(kSources)));
        if (s < 8 && audio.engine) audio.engine->setTrackOn(s + 1, !audio.engine->trackOn(s + 1));
        repaint();
    }

private:
    void timerCallback() override {
        for (int s = 0; s < kSources; ++s) shown[static_cast<size_t>(s)] = audio.peak[static_cast<size_t>(s)].load();
        repaint();
    }
    Audio& audio;
    std::array<float, kSources> shown{};
};

// Everything the window and the console share.
// Phones on its own device (spec 6): its bus is the cue bus, empty until the decks (stage 5);
// until then it carries the test tone only. Same rules as Master: no locks, no allocation.
class PhonesOut : public juce::AudioIODeviceCallback {
public:
    std::atomic<int> toneLeft{0};
    std::atomic<float> peakOut{0};
    void audioDeviceAboutToStart(juce::AudioIODevice* d) override { rate = d->getCurrentSampleRate(); }
    void audioDeviceStopped() override { peakOut.store(0); }
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* out, int numOut, int frames,
                                          const juce::AudioIODeviceCallbackContext&) override {
        for (int c = 0; c < numOut; ++c) if (out[c]) juce::FloatVectorOperations::clear(out[c], frames);
        int tone = toneLeft.load();
        float p = 0;
        if (tone > 0) {
            int n = std::min(tone, frames);
            for (int i = 0; i < n; ++i) {
                float v = 0.2f * float(std::sin(phase));
                phase += 2.0 * juce::MathConstants<double>::pi * 440.0 / rate;
                for (int c = 0; c < numOut; ++c) if (out[c]) out[c][i] = v;
                p = std::max(p, std::abs(v));
            }
            toneLeft.store(tone - n);
        }
        peakOut.store(p);
    }
private:
    double rate = 48000, phase = 0;
};

// why the app is quitting, written to the events table (soak runs: was it us or the system)
static const char* quitReason = nullptr;

struct Studio {
    Audio audio;
    PhonesOut phonesOut;
    juce::AudioDeviceManager phonesDev;  // opened only when Phones is not the Master device
    bool phonesOwn = false;
    juce::AudioDeviceManager devices;
    Settings settings;
    Console console{settings};
    Store store;
    SettingsFile settingsFile;
    bool firstRun = false;  // settings.json was made just now: Setup wizard opens before Start check
    DeviceLife master{"Master output"};
    DeviceLife input{"Sound in"};
    juce::uint32 inputBackAt = 0;  // console "lose input MS"
    juce::uint32 holdClosedUntil = 0;  // console "lose master MS": the reopen is refused until then (simulation)
};

// Sends juce::Logger lines into the console log.
class ConsoleLogger : public juce::Logger {
public:
    explicit ConsoleLogger(Console& c) : console(c) {}
    void logMessage(const juce::String& m) override {
        int level = m.containsIgnoreCase("error") || m.contains(" line ") ? 2 : 0;
        console.log(m.upToFirstOccurrenceOf(":", false, false).containsChar(' ') ? "app" : "app", m.toStdString(), level);
        std::fprintf(stderr, "%s\n", m.toRawUTF8());
    }
private:
    Console& console;
};

// "!cmd": runs /bin/sh -c in a separate process on its own thread; one at a time.
class SystemRunner : private juce::Thread {
public:
    explicit SystemRunner(Console& c) : juce::Thread("system command"), console(c) {}
    ~SystemRunner() override { stop(); stopThread(3000); }
    void start(const std::string& cmd) {
        if (isThreadRunning()) { console.log("system", "a command is still running (Ctrl+C stops it)", 1); return; }
        command = cmd;
        startThread();
    }
    void stop() { if (proc) proc->kill(); }
private:
    void run() override {
        proc = std::make_unique<juce::ChildProcess>();
        juce::StringArray args{"/bin/sh", "-c", juce::String(command) + " 2>&1 </dev/null"};
        if (!proc->start(args)) { post("cannot start /bin/sh", 2); return; }
        juce::String pending;
        char buf[512];
        for (;;) {
            int n = proc->readProcessOutput(buf, sizeof buf);
            if (n <= 0) { if (!proc->isRunning()) break; juce::Thread::sleep(10); continue; }
            pending += juce::String::fromUTF8(buf, n);
            while (pending.containsChar('\n')) {
                post(pending.upToFirstOccurrenceOf("\n", false, false).toStdString(), 0);
                pending = pending.fromFirstOccurrenceOf("\n", false, false);
            }
            if (threadShouldExit()) break;
        }
        if (pending.isNotEmpty()) post(pending.toStdString(), 0);
        post("exit code " + std::to_string(proc->getExitCode()), 0);
        proc.reset();
    }
    void post(const std::string& s, int level) { console.log("system", s, level); }
    Console& console;
    std::string command;
    std::unique_ptr<juce::ChildProcess> proc;
};

// Opens the Master output named in settings; a missing device is reported as Lost and nothing else is picked.
void applyOutput(Studio& st) {
    auto setup = st.devices.getAudioDeviceSetup();
    juce::StringArray names;
    if (auto* type = st.devices.getCurrentDeviceTypeObject()) {
        type->scanForDevices();
        names = type->getDeviceNames(false);
    }
    auto mainIn = juce::String(st.settings.get("devices.main"));
    if (mainIn != "MC-707 (emulator)") {
        juce::StringArray ins;
        if (auto* type = st.devices.getCurrentDeviceTypeObject()) ins = type->getDeviceNames(true);
        if (!ins.contains(mainIn))
            st.console.log("audio", "Main input lost: " + mainIn.toStdString() + " (no substitute; set devices.main to choose another)", 1);
        else
            st.console.log("audio", "Main input: " + mainIn.toStdString() + " (USB inputs are opened in stage 8)", 1);
    }
    auto phones = juce::String(st.settings.get("audioOut.phones.device"));
    if (phones.isNotEmpty() && !names.contains(phones))
        st.console.log("audio", "Phones output lost: " + phones.toStdString() + " (not opened)", 1);
    auto want = juce::String(st.settings.get("audioOut.master.device"));
    if (want.isNotEmpty()) {
        if (!names.contains(want)) {
            st.console.log("audio", "Master output lost: " + want.toStdString() + " (type reconnect when it is back)", 1);
            return;
        }
        setup.outputDeviceName = want;
    } else if (auto* type = st.devices.getCurrentDeviceTypeObject()) {
        // empty = system default; without this a failed device name from before stays and Master never comes back
        int i = type->getDefaultDeviceIndex(false);
        if (i >= 0 && i < names.size()) setup.outputDeviceName = names[i];
    }
    setup.bufferSize = juce::String(st.settings.get("audioOut.blockSize")).getIntValue();
    setup.sampleRate = 48000;  // PipeWire and DJ-202 run at 48 kHz (spec 6); a device without it falls back to its own rate
    auto err = st.devices.setAudioDeviceSetup(setup, true);
    auto* d = st.devices.getCurrentAudioDevice();
    if (err.isNotEmpty() || !d) st.console.log("audio", "Master output: " + err.toStdString(), 2);
    else st.console.log("audio", "Master output: " + d->getName().toStdString() + ", " + std::to_string(int(d->getCurrentSampleRate())) + " Hz, block " + std::to_string(d->getCurrentBufferSizeSamples()));
    // Phones: same device as Master -> Master's channels carry it; another device -> its own manager
    bool own = phones.isNotEmpty() && names.contains(phones) && !(d && d->getName() == phones);
    if (!own) {
        if (st.phonesOwn) { st.phonesDev.removeAudioCallback(&st.phonesOut); st.phonesDev.closeAudioDevice(); st.console.log("audio", "Phones output: same device as Master"); }
        st.phonesOwn = false;
        return;
    }
    if (!st.phonesOwn) { st.phonesDev.initialise(0, 2, nullptr, false); st.phonesDev.addAudioCallback(&st.phonesOut); }
    auto ps = st.phonesDev.getAudioDeviceSetup();
    ps.outputDeviceName = phones;
    ps.inputDeviceName = "";
    ps.sampleRate = 48000;
    ps.bufferSize = setup.bufferSize;
    auto perr = st.phonesDev.setAudioDeviceSetup(ps, true);
    auto* pd = st.phonesDev.getCurrentAudioDevice();
    st.phonesOwn = true;
    if (perr.isNotEmpty() || !pd) st.console.log("audio", "Phones output: " + perr.toStdString(), 2);
    else st.console.log("audio", "Phones output: " + pd->getName().toStdString() + " (own device), " + std::to_string(int(pd->getCurrentSampleRate())) + " Hz, block " + std::to_string(pd->getCurrentBufferSizeSamples()));
}

// Where settings and data live (spec 16): XDG folders on Linux, ~/Library/Application Support on macOS.
juce::File appDir(const char* kind) {
    auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
#if JUCE_MAC
    juce::ignoreUnused(kind);
    return home.getChildFile("Library/Application Support/PEJEDED STUDIO");
#else
    if (juce::String(kind) == "state") return home.getChildFile(".local/state/pejeded-studio");
    return juce::String(kind) == "config" ? home.getChildFile(".config/pejeded-studio") : home.getChildFile(".local/share/pejeded-studio");
#endif
}

// Gathers what the Start check rows need from the running app.
StartFacts gatherFacts(Studio& st) {
    StartFacts f;
    f.input = st.settings.get("devices.main");
    juce::StringArray ins;
    if (auto* type = st.devices.getCurrentDeviceTypeObject()) ins = type->getDeviceNames(true);
    bool emu = f.input == "MC-707 (emulator)";
    f.inputPresent = emu || ins.contains(juce::String(f.input));
    float peak = 0;
    for (auto& p : st.audio.peak) peak = std::max(peak, p.load());
    f.inputMoving = peak > 1e-4f;  // meter peaks of the sources (USB inputs: stage 8)
    f.controller = st.settings.get("devices.controller");
    f.controllerPresent = f.controller == "DJ-202 (emulator)";
    f.master = st.settings.get("audioOut.master.device");
    f.phones = st.settings.get("audioOut.phones.device");
    auto* d = st.devices.getCurrentAudioDevice();
    f.masterOpen = d && (f.master.empty() || d->getName().toStdString() == f.master);
    auto* pd = st.phonesDev.getCurrentAudioDevice();
    f.phonesOpen = f.phones.empty() || (d && d->getName().toStdString() == f.phones) || (st.phonesOwn && pd && pd->getName().toStdString() == f.phones);
    f.tracksFolder = st.settings.get("tracks.folder");
    f.tracksReadable = !f.tracksFolder.empty() && juce::File(f.tracksFolder).isDirectory();
    f.recordFolder = st.settings.get("record.folder");
    juce::File rec(f.recordFolder);
    f.recordWritable = !f.recordFolder.empty() && rec.isDirectory() && rec.hasWriteAccess();
    f.recordFreeGb = f.recordWritable ? double(rec.getBytesFreeOnVolume()) / 1e9 : 0;
    auto dirs = juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("PATH", ""), ":", "");
    dirs.addArray({"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"});  // a macOS app gets a short PATH
    for (auto dir : dirs)
        f.ffmpegFound = f.ffmpegFound || juce::File(dir).getChildFile("ffmpeg").existsAsFile();  // no child process on the UI thread
    switch (st.store.state()) {
        case Store::State::Open: f.dbState = "open"; break;
        case Store::State::Damaged: f.dbState = "damaged"; break;
        case Store::State::Newer: f.dbState = "newer"; break;
        case Store::State::Failed:
        case Store::State::Closed: f.dbState = "failed"; break;
    }
    juce::File db(appDir("data").getChildFile("studio.db"));
    f.dbSizeMb = double(db.getSize()) / 1e6;
    f.cleanAboveMb = juce::String(st.settings.get("db.suggestCleanAboveMb")).getDoubleValue();
    return f;
}

void printCheck(Studio& st) {
    for (const auto& r : startCheck(gatherFacts(st)))
        st.console.log("check", (r.ok ? "ok       " : "PROBLEM  ") + r.name + ": " + r.text + (r.fix.empty() ? "" : "  [Fix: " + r.fix + "]"), r.ok ? 0 : 1);
}

void addStageOneCommands(Studio& st, SystemRunner& sys) {
    auto& c = st.console;
    c.add("check", "Start check: what is ready and what is not", [&st](const auto&, Console&) { printCheck(st); });
    c.add("status", "devices, sample rate and block, audio drops", [&st](const auto&, Console& con) {
        auto mainIn = juce::String(st.settings.get("devices.main"));
        juce::StringArray ins;
        if (auto* type = st.devices.getCurrentDeviceTypeObject()) ins = type->getDeviceNames(true);
        bool emu = mainIn == "MC-707 (emulator)";
        con.print("main input: " + mainIn.toStdString() + (emu ? " (playing)" : ins.contains(mainIn) ? " (present, not opened until stage 8)" : " (LOST)"));
        auto* d = st.devices.getCurrentAudioDevice();
        if (!d) { con.log("audio", "no audio output open", 1); return; }
        con.print("audio out: " + d->getName().toStdString() + " (" + d->getTypeName().toStdString() + ")");
        con.print("rate " + std::to_string(int(d->getCurrentSampleRate())) + " Hz, block " +
                  std::to_string(d->getCurrentBufferSizeSamples()) + ", latency " +
                  std::to_string(d->getOutputLatencyInSamples()) + " samples");
        con.print("audio drops: " + std::to_string(d->getXRunCount()));
        if (auto* pd = st.phonesDev.getCurrentAudioDevice(); st.phonesOwn && pd)
            con.print("phones out: " + pd->getName().toStdString() + " (own device), " + std::to_string(int(pd->getCurrentSampleRate())) + " Hz, drops " + std::to_string(pd->getXRunCount()) + ", level " + juce::String(juce::Decibels::gainToDecibels(st.phonesOut.peakOut.load(), -100.0f), 1).toStdString() + " dB");
        else con.print("phones out: same device as Master");
        con.print("pattern: " + st.audio.reloader.playingName() +
                  (st.audio.reloader.lastError().empty() ? "" : "  (file error: " + st.audio.reloader.lastError() + ")"));
        con.print("emulator MC-707: tempo " + std::to_string(int(st.audio.engine ? st.audio.engine->tempo() : 0)) + " BPM");
    });
    c.add("devices", "list of audio devices", [&st](const auto&, Console& con) {
        con.print("MC-707 emulator (built in)");
        for (auto* type : st.devices.getAvailableDeviceTypes()) {
            type->scanForDevices();
            for (auto& n : type->getDeviceNames(false)) con.print(type->getTypeName().toStdString() + " out: " + n.toStdString());
            for (auto& n : type->getDeviceNames(true)) con.print(type->getTypeName().toStdString() + " in: " + n.toStdString());
        }
    });
    c.add("midi", "midi: list the MIDI ports the system sees (stage 1 check)", [](const auto&, Console& con) {
        auto ins = juce::MidiInput::getAvailableDevices(), outs = juce::MidiOutput::getAvailableDevices();
        for (auto& d : ins) con.print("midi in: " + d.name.toStdString());
        for (auto& d : outs) con.print("midi out: " + d.name.toStdString());
        con.print("midi: " + std::to_string(ins.size()) + " in, " + std::to_string(outs.size()) + " out");
    });
    c.add("lose", "lose master|input MS: lose the Master output or the sound input for MS ms (device loss test)", [&st](const auto& a, Console& con) {
        if (a.size() < 2 || (a[1] != "master" && a[1] != "input")) { con.log("console", "lose: 'lose master MS' or 'lose input MS'", 1); return; }
        int ms = a.size() > 2 ? juce::jlimit(0, 60000, juce::String(a[2]).getIntValue()) : 3000;
        if (a[1] == "input") {
            st.inputBackAt = juce::Time::getMillisecondCounter() + juce::uint32(ms);
            st.audio.inputGone = true;
            con.log("audio", "simulated loss of Sound in for " + std::to_string(ms) + " ms", 1);
            return;
        }
        st.holdClosedUntil = juce::Time::getMillisecondCounter() + juce::uint32(ms);
        st.devices.closeAudioDevice();
        con.log("audio", "simulated loss of the Master output for " + std::to_string(ms) + " ms", 1);
    });
    c.add("reconnect", "close and open the audio device again", [&st](const auto&, Console& con) {
        st.devices.closeAudioDevice();
        applyOutput(st);
        con.print(st.devices.getCurrentAudioDevice() ? "audio device open again" : "no audio output open");
    });
    c.systemRunner = [&sys](const std::string& cmd, Console&) { sys.start(cmd); };
    c.add("db", "database: size and tables; db backup: take a copy now", [&st](const auto& a, Console& con) {
        if (a.size() > 1 && a[1] == "backup") {
            auto f = st.store.backup();
            con.log("db", f.empty() ? "backup failed: " + st.store.stateText() : "backup: " + f, f.empty() ? 2 : 0);
            return;
        }
        if (a.size() > 1) { con.log("db", "db: unknown word " + a[1] + " (db, db backup)", 1); return; }
        for (const auto& l : st.store.info()) con.print(l);
    });
    c.add("sql", "sql SELECT ...: read-only query, up to 200 rows", [&st](const auto& a, Console& con) {
        std::string q;
        for (size_t i = 1; i < a.size(); ++i) q += (i > 1 ? " " : "") + a[i];
        if (q.empty()) { con.log("db", "sql: a SELECT query expected", 1); return; }
        for (const auto& l : st.store.query(q)) con.log("db", l, l.rfind("error: ", 0) == 0 ? 2 : 0);
    });
}

// Console layer (section 15): slides from the top, 40 % of the window, log above, command line below.
class ConsolePanel : public juce::Component, private juce::Timer, private juce::TextEditor::Listener {
public:
    ConsolePanel(Console& c, SystemRunner& s) : console(c), sys(s) {
        addAndMakeVisible(logView);
        addAndMakeVisible(input);
        logView.setMultiLine(true);
        logView.setReadOnly(true);
        logView.setCaretVisible(false);
        logView.setScrollbarsShown(true);
        auto mono = juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::plain);
        logView.setFont(juce::Font(mono));
        input.setFont(juce::Font(mono));
        for (auto* e : {&logView, &input}) {
            e->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xee0b0b0b));
            e->setColour(juce::TextEditor::textColourId, juce::Colours::lightgrey);
            e->setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff303030));
        }
        input.addListener(this);
        input.setTextToShowWhenEmpty("help  |  !command", juce::Colours::grey);
        startTimerHz(10);
    }
    std::function<void(int)> onDragHeight;  // bottom edge dragged to this height in pixels
    static constexpr int kGrip = 6;
    void paint(juce::Graphics& g) override { g.setColour(juce::Colour(0xff3a3a3a)); g.fillRect(getLocalBounds().removeFromBottom(kGrip)); }
    void mouseMove(const juce::MouseEvent& e) override { setMouseCursor(e.y >= getHeight() - kGrip ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor); }
    void mouseDrag(const juce::MouseEvent& e) override { if (e.mouseDownPosition.y >= getHeight() - kGrip && onDragHeight) onDragHeight(e.y); }
    void resized() override {
        auto r = getLocalBounds();
        r.removeFromBottom(kGrip);
        input.setBounds(r.removeFromBottom(28));
        logView.setBounds(r);
    }
    void focusInput() { input.grabKeyboardFocus(); }
    juce::String inputText() const { return input.getText(); }
    bool keyPressed(const juce::KeyPress& k) override {
        if (k.getModifiers().isCtrlDown() && k.getKeyCode() == 'C' && !input.getHighlightedRegion().getLength()) { sys.stop(); return true; }
        if (k == juce::KeyPress::upKey || k == juce::KeyPress::downKey) {
            const auto& h = console.history();
            if (h.empty()) return true;
            histPos = std::clamp(histPos + (k == juce::KeyPress::upKey ? -1 : 1), 0, int(h.size()));
            input.setText(histPos < int(h.size()) ? juce::String(h[size_t(histPos)]) : juce::String(), false);
            return true;
        }
        if (k == juce::KeyPress::tabKey) {
            input.setText(console.complete(input.getText().toStdString()), false);
            input.moveCaretToEnd();
            return true;
        }
        if (k == juce::KeyPress::pageUpKey || k == juce::KeyPress::pageDownKey) return logView.keyPressed(k);
        return false;
    }

private:
    void textEditorReturnKeyPressed(juce::TextEditor&) override {
        auto line = input.getText().toStdString();
        input.clear();
        console.run(line);
        histPos = int(console.history().size());
        shown = size_t(-1);
    }
    void timerCallback() override {
        auto lines = console.lines();
        if (lines.size() == shown && (lines.empty() || lines.back().text == lastText)) return;
        shown = lines.size();
        lastText = lines.empty() ? "" : lines.back().text;
        logView.clear();
        for (const auto& l : lines) {
            logView.setColour(juce::TextEditor::textColourId,
                              l.level == 2 ? juce::Colours::orangered : l.level == 1 ? juce::Colours::orange : juce::Colours::lightgrey);
            logView.moveCaretToEnd();
            logView.insertTextAtCaret(juce::String(l.time + "  " + l.area + "  " + l.text + "\n"));
        }
        logView.moveCaretToEnd();
    }
    Console& console;
    SystemRunner& sys;
    juce::TextEditor logView, input;
    int histPos = 0;
    size_t shown = size_t(-1);
    std::string lastText;
};

// Start check overlay (spec 15): rows with ok / problem, Fix per problem, Go always enabled.
// Sound and picture keep running underneath; nothing here is modal.
class CheckPanel : public juce::Component, private juce::Timer {
public:
    explicit CheckPanel(Studio& s) : st(s) {
        go.onClick = [this] { if (onGo) onGo(); };
        addAndMakeVisible(go);
    }
    void visibilityChanged() override { if (isVisible()) startTimer(1000); else stopTimer(); }
    void timerCallback() override {  // live state: a row turns ok as soon as the problem is gone
        auto now = startCheck(gatherFacts(st));
        bool same = now.size() == rows.size();
        for (size_t i = 0; same && i < now.size(); ++i) same = now[i].ok == rows[i].ok && now[i].text == rows[i].text;
        if (!same) refresh();
    }
    std::function<void()> onGo;
    std::function<void(const std::string& fix)> onFix;
    void refresh() {
        rows = startCheck(gatherFacts(st));
        buttons.clear();
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            if (r.ok || r.fix.empty()) { buttons.emplace_back(); continue; }
            std::vector<std::unique_ptr<juce::TextButton>> b;
            if (r.fix == "db:restore|empty") {
                b.push_back(std::make_unique<juce::TextButton>("Restore"));
                b.back()->onClick = [this] { fix("db:restore"); };
                b.push_back(std::make_unique<juce::TextButton>("Start empty"));
                b.back()->onClick = [this] { fix("db:empty"); };
            } else {
                b.push_back(std::make_unique<juce::TextButton>(r.fix == "db:clean" ? "Clean" : "Fix"));
                auto f = r.fix;
                b.back()->onClick = [this, f] { fix(f); };
            }
            for (auto& x : b) addAndMakeVisible(*x);
            buttons.push_back(std::move(b));
        }
        resized();
        repaint();
    }
    void paint(juce::Graphics& g) override {
        g.setColour(juce::Colour(0xee101010));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
        g.setColour(juce::Colours::white);
        g.setFont(18.0f);
        g.drawText("Start check", 16, 8, getWidth() - 32, 28, juce::Justification::left);
        g.setFont(14.0f);
        for (size_t i = 0; i < rows.size(); ++i) {
            int y = 44 + int(i) * kRow;
            g.setColour(rows[i].ok ? juce::Colour(0xff4caf50) : juce::Colour(0xffff9800));
            g.fillEllipse(16.0f, float(y + 8), 10.0f, 10.0f);
            g.setColour(juce::Colours::white);
            g.drawText(rows[i].name, 36, y, 150, kRow, juce::Justification::centredLeft);
            g.setColour(juce::Colours::lightgrey);
            g.drawText(rows[i].text, 190, y, getWidth() - 190 - 200, kRow, juce::Justification::centredLeft, true);
        }
    }
    void resized() override {
        for (size_t i = 0; i < buttons.size(); ++i) {
            int x = getWidth() - 16;
            for (auto it = buttons[i].rbegin(); it != buttons[i].rend(); ++it) {
                x -= 90;
                (*it)->setBounds(x, 44 + int(i) * kRow + 3, 84, kRow - 6);
            }
        }
        go.setBounds(getWidth() - 106, getHeight() - 44, 90, 32);
    }
    int wantedHeight() const { return 44 + int(rows.size()) * kRow + 56; }
    const std::vector<CheckRow>& current() const { return rows; }
    // Clicks a button by its label, as a person would (console "press", end-to-end tests).
    bool press(const juce::String& label) {
        if (label == "Go") { go.onClick(); return true; }
        for (auto& row : buttons)
            for (auto& b : row)
                if (b->getButtonText() == label) { b->onClick(); return true; }
        return false;
    }

private:
    void fix(const std::string& f) {
        if (f == "db:restore") { st.store.restoreLastBackup(); st.console.log("db", "Restore: " + st.store.stateText(), 1); }
        else if (f == "db:empty") { st.store.startEmpty(); st.console.log("db", "Start empty: " + st.store.stateText(), 1); }
        else if (onFix) onFix(f);
        refresh();
    }
    static constexpr int kRow = 28;
    Studio& st;
    std::vector<CheckRow> rows;
    std::vector<std::vector<std::unique_ptr<juce::TextButton>>> buttons;
    juce::TextButton go{"Go"};
};

// Setup wizard (spec 15): six steps, each asks one thing and lets you check it at once.
class WizardPanel : public juce::Component, private juce::Timer {
public:
    explicit WizardPanel(Studio& s) : st(s) {
        for (auto* b : {&back, &skip, &next, &test, &browse}) addAndMakeVisible(*b);
        for (auto* c : {&pickA, &pickB}) addChildComponent(*c);
        addChildComponent(path);
        back.onClick = [this] { show(step - 1); };
        skip.onClick = [this] { if (step == 6) finish(); else show(step + 1); };
        next.onClick = [this] { apply(); if (step == 6) finish(); else show(step + 1); };
        test.onClick = [this] { runTest(); };
        browse.onClick = [this] { choose(); };
        pickA.onChange = [this] { apply(); };
        pickB.onChange = [this] { apply(); };
        path.onReturnKey = [this] { apply(); };
    }
    std::function<void()> onDone;
    int current() const { return step; }
    void show(int s) {
        step = juce::jlimit(1, 6, s);
        note.clear();
        pickA.clear(juce::dontSendNotification);
        pickB.clear(juce::dontSendNotification);
        juce::StringArray outs, ins;
        if (auto* type = st.devices.getCurrentDeviceTypeObject()) { outs = type->getDeviceNames(false); ins = type->getDeviceNames(true); }
        auto fill = [](juce::ComboBox& c, const juce::StringArray& items, const juce::String& cur) {
            for (int i = 0; i < items.size(); ++i) c.addItem(items[i], i + 1);
            c.setText(cur, juce::dontSendNotification);
        };
        if (step == 1) { ins.insert(0, "MC-707 (emulator)"); fill(pickA, ins, st.settings.get("devices.main")); }
        if (step == 2) fill(pickA, {"DJ-202 (emulator)"}, st.settings.get("devices.controller"));  // the real DJ-202: stage 8
        if (step == 3) {
            outs.insert(0, "system default"); fill(pickA, outs, orDefault(st.settings.get("audioOut.master.device")));
            outs.set(0, "same as Master"); fill(pickB, outs, orSame(st.settings.get("audioOut.phones.device")));
        }
        if (step == 4) path.setText(st.settings.get("tracks.folder"), false);
        if (step == 5) path.setText(st.settings.get("record.folder"), false);
        pickA.setVisible(step <= 3);
        pickB.setVisible(step == 3);
        path.setVisible(step == 4 || step == 5);
        browse.setVisible(step == 4 || step == 5);
        test.setVisible(step == 1 || step == 2 || step == 3 || step == 5);
        test.setButtonText(step == 3 ? "Play test tone" : step == 5 ? "Test write" : "Check");
        back.setEnabled(step > 1);
        next.setButtonText(step == 6 ? "Finish" : "Next");
        timerCallback();  // the check line belongs to this step at once
        resized();
        repaint();
    }
    // Clicks a button or picks an item by its text, as a person would (console "press").
    bool press(const juce::String& label) {
        for (auto* b : {&back, &skip, &next, &test, &browse})
            if (b->isVisible() && b->getButtonText() == label) { b->onClick(); return true; }
        return false;
    }
    bool pick(const juce::String& text, bool second) {
        auto& c = second ? pickB : pickA;
        if (!c.isVisible()) return false;
        for (int i = 0; i < c.getNumItems(); ++i)
            if (c.getItemText(i) == text) { c.setSelectedItemIndex(i, juce::sendNotificationSync); return true; }
        return false;
    }
    bool type(const juce::String& text) {
        if (!path.isVisible()) return false;
        path.setText(text, false);
        apply();
        return true;
    }
    void visibilityChanged() override { if (isVisible()) startTimer(500); else stopTimer(); }
    void paint(juce::Graphics& g) override {
        g.setColour(juce::Colour(0xee101010));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
        g.setColour(juce::Colours::white);
        g.setFont(18.0f);
        g.drawText("Setup wizard  " + juce::String(step) + " of 6: " + kTitles[step - 1], 16, 8, getWidth() - 32, 28, juce::Justification::left);
        g.setFont(14.0f);
        g.setColour(juce::Colours::lightgrey);
        g.drawText(kAsk[step - 1], 16, 40, getWidth() - 32, 24, juce::Justification::left);
        if (step == 3) {
            g.drawText("Master", 16, 72, 80, 26, juce::Justification::centredLeft);
            g.drawText("Phones", 16, 104, 80, 26, juce::Justification::centredLeft);
        }
        if (step == 6) {
            auto rows = startCheck(gatherFacts(st));
            for (size_t i = 0; i < rows.size(); ++i) {
                int y = 72 + int(i) * 22;
                g.setColour(rows[i].ok ? juce::Colour(0xff4caf50) : juce::Colour(0xffff9800));
                g.fillEllipse(16.0f, float(y + 6), 10.0f, 10.0f);
                g.setColour(juce::Colours::white);
                g.drawText(rows[i].name, 36, y, 150, 22, juce::Justification::centredLeft);
                g.setColour(juce::Colours::lightgrey);
                g.drawText(rows[i].text, 190, y, getWidth() - 206, 22, juce::Justification::centredLeft, true);
            }
            g.drawText("Speed test (section 9) is offered here from stage 2.", 16, 72 + int(rows.size()) * 22 + 4, getWidth() - 32, 22, juce::Justification::left);
        } else {
            g.setColour(checkOk ? juce::Colour(0xff4caf50) : juce::Colour(0xffff9800));
            g.drawText(checkText, 16, 144, getWidth() - 32, 24, juce::Justification::left, true);
            g.setColour(juce::Colours::lightgrey);
            g.drawText(note, 16, 172, getWidth() - 32, 24, juce::Justification::left, true);
        }
    }
    void resized() override {
        pickA.setBounds(100, 72, getWidth() - 116 - (step == 3 ? 0 : 140), 26);
        pickB.setBounds(100, 104, getWidth() - 116, 26);
        path.setBounds(16, 72, getWidth() - 140, 26);
        browse.setBounds(getWidth() - 116, 72, 100, 26);
        test.setBounds(step == 3 ? 16 : getWidth() - 136, step == 3 ? 136 - 30 + 30 : 72, 120, 26);
        if (step == 3) test.setBounds(getWidth() - 136, 136, 120, 26);
        if (step == 5) test.setBounds(getWidth() - 136, 104, 120, 26);
        int y = getHeight() - 44;
        next.setBounds(getWidth() - 106, y, 90, 32);
        skip.setBounds(getWidth() - 202, y, 90, 32);
        back.setBounds(16, y, 90, 32);
    }
    int wantedHeight() const { return 72 + 9 * 22 + 30 + 56; }

private:
    static constexpr const char* kTitles[6] = {"Sound in", "Controller", "Sound out", "Tracks folder", "Recording folder", "Check"};
    static constexpr const char* kAsk[6] = {
        "Which device brings the sound in? Check: the levels move.",
        "Which controller plays the decks? Check: press any button on it.",
        "Where do Master and Phones go? Check: a test tone in each output.",
        "Which folder holds your tracks?",
        "Where do recordings go? Check: a test write of two seconds.",
        "The same list as Start check."};
    static juce::String orDefault(const std::string& s) { return s.empty() ? "system default" : juce::String(s); }
    static juce::String orSame(const std::string& s) { return s.empty() ? "same as Master" : juce::String(s); }
    void set(const std::string& key, const juce::String& v) {
        auto why = st.settings.set(key, v.toStdString());
        if (!why.empty()) { note = why; st.console.log("wizard", why, 1); }
        else st.console.log("wizard", key + " = " + v.toStdString());
    }
    void apply() {
        if (step == 1 && pickA.getText().isNotEmpty()) set("devices.main", pickA.getText());
        if (step == 2 && pickA.getText().isNotEmpty()) set("devices.controller", pickA.getText());
        if (step == 3) {
            auto m = pickA.getText(), ph = pickB.getText();
            if (m.isNotEmpty()) set("audioOut.master.device", m == "system default" ? juce::String() : m);
            if (ph.isNotEmpty()) set("audioOut.phones.device", ph == "same as Master" ? juce::String() : ph);
        }
        if (step == 4 || step == 5) {
            auto dir = path.getText().trim();
            if (dir.isNotEmpty() && !juce::File::isAbsolutePath(dir)) { note = "a full path is needed"; repaint(); return; }
            if (step == 5 && dir.isNotEmpty()) juce::File(dir).createDirectory();
            set(step == 4 ? "tracks.folder" : "record.folder", dir);
        }
        timerCallback();
    }
    void choose() {
        chooser = std::make_unique<juce::FileChooser>(step == 4 ? "Tracks folder" : "Recording folder", juce::File(path.getText()));
        juce::Component::SafePointer<WizardPanel> self(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [self](const juce::FileChooser& fc) {
                                 if (!self || fc.getResult() == juce::File()) return;
                                 self->path.setText(fc.getResult().getFullPathName(), false);
                                 self->apply();
                             });
    }
    void runTest() {
        if (step == 3) {
            int frames = int((st.devices.getCurrentAudioDevice() ? st.devices.getCurrentAudioDevice()->getCurrentSampleRate() : 48000) * 1.0);
            st.audio.toneLeft.store(frames);
            if (st.phonesOwn) st.phonesOut.toneLeft.store(frames);
            note = st.phonesOwn ? "a 440 Hz tone plays for one second on Master and on Phones" : "a 440 Hz tone plays for one second on the opened output";
            st.console.log("wizard", "test tone: 1 s, 440 Hz");
        } else if (step == 5) {
            juce::File dir(st.settings.get("record.folder"));
            auto probe = dir.getChildFile(".pejeded-test-write");
            juce::MemoryBlock two(size_t(48000 * 2 * 2 * 2), true);  // two seconds of stereo 16-bit silence
            bool ok = dir.isDirectory() && probe.replaceWithData(two.getData(), two.getSize()) && probe.getSize() == juce::int64(two.getSize());
            probe.deleteFile();
            note = ok ? "test write ok: " + juce::String(two.getSize() / 1024) + " KB written and removed (a real recording comes in stage 4)"
                      : "test write failed in " + dir.getFullPathName();
            st.console.log("wizard", note.toStdString(), ok ? 0 : 1);
        }
        timerCallback();
    }
    void timerCallback() override {
        auto f = gatherFacts(st);
        auto rows = startCheck(f);
        auto rowOf = [&](const char* name) -> const CheckRow* {
            for (const auto& r : rows) if (r.name == name) return &r;
            return nullptr;
        };
        const CheckRow* r = nullptr;
        if (step == 1) r = rowOf("Sound in");
        if (step == 2) r = rowOf("Controller");
        if (step == 3) { r = rowOf("Phones"); if (!r || r->ok) r = rowOf("Master"); }
        if (step == 4) r = rowOf("Tracks folder");
        if (step == 5) r = rowOf("Recordings");
        juce::String t = r ? juce::String(r->name + ": " + r->text) : juce::String();
        bool ok = r && r->ok;
        if (t != checkText || ok != checkOk || step == 6) { checkText = t; checkOk = ok; repaint(); }
    }
    void finish() { st.console.log("wizard", "finished at step " + std::to_string(step)); if (onDone) onDone(); }

    Studio& st;
    int step = 1;
    juce::String checkText, note;
    bool checkOk = false;
    juce::ComboBox pickA, pickB;
    juce::TextEditor path;
    juce::TextButton back{"Back"}, skip{"Skip"}, next{"Next"}, test{"Check"}, browse{"Browse"};
    std::unique_ptr<juce::FileChooser> chooser;
};

// Settings (spec 15): tabs; each row edits one key of settings.json and takes effect at once.
class SettingsPanel : public juce::Component {
public:
    struct Row {
        const char* label;
        std::string key;   // "" for a button row
        enum Kind { Pick, Text, Button } kind;
        std::function<juce::StringArray()> items;  // Pick
        std::function<void(SettingsPanel&)> action; // Button
    };
    explicit SettingsPanel(Studio& s) : st(s) {
        auto outs = [this] { juce::StringArray n{"system default"}; if (auto* t2 = st.devices.getCurrentDeviceTypeObject()) n.addArray(t2->getDeviceNames(false)); return n; };
        auto phones = [this] { juce::StringArray n{"same as Master"}; if (auto* t2 = st.devices.getCurrentDeviceTypeObject()) n.addArray(t2->getDeviceNames(false)); return n; };
        auto ins = [this] { juce::StringArray n{"MC-707 (emulator)"}; if (auto* t2 = st.devices.getCurrentDeviceTypeObject()) n.addArray(t2->getDeviceNames(true)); return n; };
        tabs.push_back({"Devices", {
            {"Sound in", "devices.main", Row::Pick, ins, {}},
            {"Controller", "devices.controller", Row::Pick, [] { return juce::StringArray{"DJ-202 (emulator)"}; }, {}},
        }, "Tempo goes via, MIDI ports and the channel map come with stages 3 and 8."});
        tabs.push_back({"Audio out", {
            {"Master", "audioOut.master.device", Row::Pick, outs, {}},
            {"Phones", "audioOut.phones.device", Row::Pick, phones, {}},
            {"Block size", "audioOut.blockSize", Row::Pick, [] { return juce::StringArray{"64", "128", "256", "512", "1024"}; }, {}},
            {"Master gain, dB", "audioOut.masterGainDb", Row::Text, {}, {}},
        }, "Master gain: -24 to 12 dB, Enter saves it. Monitor comes with the decks (stage 5)."});
        tabs.push_back({"Folders", {
            {"Tracks", "tracks.folder", Row::Text, {}, {}},
            {"Recordings", "record.folder", Row::Text, {}, {}},
            {"Patterns", "patterns.folder", Row::Text, {}, {}},
        }, "A full path; Enter saves it."});
        tabs.push_back({"Recording", {}, "Comes with recording (stage 4)."});
        tabs.push_back({"Look", {}, "Comes with the cube (stage 2)."});
        tabs.push_back({"Keys", {}, "Comes with the actions of section 13 (stage 3)."});
        tabs.push_back({"Maintenance", {
            {"Reconnect devices", "", Row::Button, {}, [](SettingsPanel& sp) { sp.st.console.run("reconnect"); }},
            {"Open log folder", "", Row::Button, {}, [](SettingsPanel& sp) { appDir("data").startAsProcess(); sp.st.console.log("settings", "opened " + appDir("data").getFullPathName().toStdString()); }},
            {"Reset settings", "", Row::Button, {}, [](SettingsPanel& sp) { sp.resetSettings(); }},
        }, "Reset asks again on the button itself."});
        for (size_t i = 0; i < tabs.size(); ++i) {
            auto b = std::make_unique<juce::TextButton>(tabs[i].name);
            b->setClickingTogglesState(false);
            b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3a6ea5));  // the open tab
            b->onClick = [this, i] { open(int(i)); };
            addAndMakeVisible(*b);
            tabButtons.push_back(std::move(b));
        }
        addAndMakeVisible(close);
        close.onClick = [this] { if (onClose) onClose(); };
    }
    std::function<void()> onClose;
    void open(int tab) {
        current = juce::jlimit(0, int(tabs.size()) - 1, tab);
        for (size_t i = 0; i < tabButtons.size(); ++i) tabButtons[i]->setToggleState(int(i) == current, juce::dontSendNotification);
        editors.clear();
        armed = -1;
        for (size_t r = 0; r < tabs[size_t(current)].rows.size(); ++r) {
            const auto& row = tabs[size_t(current)].rows[r];
            if (row.kind == Row::Pick) {
                auto c = std::make_unique<juce::ComboBox>();
                auto items = row.items();
                for (int i = 0; i < items.size(); ++i) c->addItem(items[i], i + 1);
                auto v = juce::String(st.settings.get(row.key));
                c->setText(v.isEmpty() ? items[0] : v, juce::dontSendNotification);
                auto key = row.key; auto none = items[0];
                auto* cp = c.get();
                c->onChange = [this, key, none, cp] {
                    auto v2 = cp->getText();
                    bool blank = (key == "audioOut.master.device" || key == "audioOut.phones.device") && v2 == none;
                    put(key, blank ? juce::String() : v2);
                };
                editors.push_back(std::move(c));
            } else if (row.kind == Row::Text) {
                auto e = std::make_unique<juce::TextEditor>();
                e->setText(st.settings.get(row.key), false);
                auto key = row.key; auto* ep = e.get();
                e->onReturnKey = [this, key, ep] { put(key, ep->getText().trim()); };
                editors.push_back(std::move(e));
            } else {
                auto b = std::make_unique<juce::TextButton>(row.label);
                auto* bp = b.get();
                b->onClick = [this, r, bp] { press(bp->getButtonText()); juce::ignoreUnused(r); };
                editors.push_back(std::move(b));
            }
            addAndMakeVisible(*editors.back());
        }
        resized();
        repaint();
    }
    int currentTab() const { return current; }
    bool openByName(const juce::String& name) {
        for (size_t i = 0; i < tabs.size(); ++i) if (name.equalsIgnoreCase(tabs[i].name)) { open(int(i)); return true; }
        return false;
    }
    // Clicks a button or a tab by its text, as a person would (console "press").
    bool press(const juce::String& label) {
        if (label == "Close") { close.onClick(); return true; }
        if (openByName(label)) return true;
        const auto& rows = tabs[size_t(current)].rows;
        for (size_t r = 0; r < rows.size(); ++r) {
            if (rows[r].kind != Row::Button) continue;
            bool isConfirm = label == "Confirm" && armed == int(r);
            if (label != rows[r].label && !isConfirm) continue;
            if (std::string(rows[r].label) == "Reset settings" && armed != int(r)) {  // asks again on the button itself
                armed = int(r);
                static_cast<juce::TextButton*>(editors[r].get())->setButtonText("Confirm");
                juce::Component::SafePointer<SettingsPanel> self(this);
                juce::Timer::callAfterDelay(4000, [self, r] { if (self && self->armed == int(r)) { self->armed = -1; self->open(self->current); } });
                return true;
            }
            armed = -1;
            rows[r].action(*this);
            return true;
        }
        return false;
    }
    bool pick(const juce::String& label, const juce::String& item) {
        const auto& rows = tabs[size_t(current)].rows;
        for (size_t r = 0; r < rows.size(); ++r) {
            if (label != rows[r].label) continue;
            if (auto* c = dynamic_cast<juce::ComboBox*>(editors[r].get()))
                for (int i = 0; i < c->getNumItems(); ++i)
                    if (c->getItemText(i) == item) { c->setSelectedItemIndex(i, juce::sendNotificationSync); return true; }
            if (auto* e = dynamic_cast<juce::TextEditor*>(editors[r].get())) { e->setText(item, false); e->onReturnKey(); return true; }
        }
        return false;
    }
    void paint(juce::Graphics& g) override {
        g.setColour(juce::Colour(0xee101010));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
        g.setColour(juce::Colours::white);
        g.setFont(18.0f);
        g.drawText("Settings", 16, 8, 200, 28, juce::Justification::left);
        g.setFont(14.0f);
        const auto& tab = tabs[size_t(current)];
        for (size_t r = 0; r < tab.rows.size(); ++r)
            if (tab.rows[r].kind != Row::Button) {
                g.setColour(juce::Colours::lightgrey);
                g.drawText(tab.rows[r].label, 16, kTop + int(r) * kRow, 140, kRow - 6, juce::Justification::centredLeft);
            }
        g.setColour(juce::Colours::grey);
        g.drawText(tab.note, 16, kTop + int(tab.rows.size()) * kRow + 4, getWidth() - 32, 22, juce::Justification::left, true);
        g.setColour(noteOk ? juce::Colour(0xff4caf50) : juce::Colour(0xffff9800));
        g.drawText(status, 16, getHeight() - 36, getWidth() - 140, 24, juce::Justification::left, true);
    }
    void resized() override {
        int x = 16;
        for (auto& b : tabButtons) { int w = 20 + int(b->getButtonText().length()) * 8; b->setBounds(x, 44, w, 26); x += w + 4; }
        close.setBounds(getWidth() - 106, getHeight() - 44, 90, 32);
        const auto& rows = tabs[size_t(current)].rows;
        for (size_t r = 0; r < editors.size() && r < rows.size(); ++r) {
            int y = kTop + int(r) * kRow;
            if (rows[r].kind == Row::Button) editors[r]->setBounds(16, y, 200, kRow - 6);
            else editors[r]->setBounds(160, y, getWidth() - 176, kRow - 6);
        }
    }
    int wantedHeight() const { return 360; }

private:
    struct Tab { const char* name; std::vector<Row> rows; const char* note; };
    void put(const std::string& key, const juce::String& v) {
        auto why = st.settings.set(key, v.toStdString());
        noteOk = why.empty();
        status = noteOk ? juce::String(key + " = ") + v : juce::String(why);
        st.console.log("settings", status.toStdString(), noteOk ? 0 : 1);
        repaint();
    }
    void resetSettings() {
        auto file = st.settingsFile.file();
        auto copy = file.getSiblingFile("settings.before-reset.json");
        file.copyFileTo(copy);
        for (const auto& [k, v] : SettingsFile::defaultValues()) st.settings.set(k, v);
        status = "settings reset to defaults; the old file is kept as " + copy.getFileName();
        noteOk = true;
        st.console.log("settings", status.toStdString());
        open(current);
    }
    static constexpr int kTop = 84, kRow = 36;
    Studio& st;
    std::vector<Tab> tabs;
    std::vector<std::unique_ptr<juce::TextButton>> tabButtons;
    std::vector<std::unique_ptr<juce::Component>> editors;
    juce::TextButton close{"Close"};
    int current = 0, armed = -1;
    juce::String status;
    bool noteOk = true;
};

// Top bar (spec 15): always on view; the title is fixed in code.
class TopBar : public juce::Component {
public:
    TopBar() {
        for (auto* b : {&decks, &sources, &look, &console, &wizard, &settings, &help}) {
            addAndMakeVisible(*b);
            b->onClick = [this, b] { if (onButton) onButton(b->getButtonText()); };
        }
    }
    std::function<void(const juce::String&)> onButton;
    static constexpr int kHeight = 36;
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff0c0c0c));
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
        g.drawText(pejeded::appTitle(), 16, 0, 200, getHeight(), juce::Justification::centredLeft);
    }
    void resized() override {
        int x = getWidth() - 8;
        for (auto* b : {&help, &settings, &wizard, &console, &look, &sources, &decks}) {
            int w = 24 + int(b->getButtonText().length()) * 8;
            x -= w + 4;
            b->setBounds(x, 5, w, getHeight() - 10);
        }
    }
    bool press(const juce::String& label) {
        for (auto* b : {&decks, &sources, &look, &console, &wizard, &settings, &help})
            if (b->getButtonText() == label) { b->onClick(); return true; }
        return false;
    }
private:
    juce::TextButton decks{"DECKS"}, sources{"Sources"}, look{"Look"}, console{"Console"}, wizard{"Setup wizard"}, settings{"Settings"}, help{"?"};
};

// Bottom bar (spec 15): always on view. Rec, tempo with its source and path, Master meter, state phrase.
// Once a second: is the Master output open; when lost, try to open it again (spec 6).
void tickDevices(Studio& st) {
    bool open = st.devices.getCurrentAudioDevice() != nullptr;
    auto before = st.master.state();
    bool tryOpen = st.master.tick(open);
    if (st.master.state() != before)
        st.console.log("audio", std::string("Master output: ") + DeviceLife::name(before) + " -> " + DeviceLife::name(st.master.state()),
                       st.master.state() == DeviceLife::State::Running ? 0 : 1);
    if (tryOpen && juce::Time::getMillisecondCounter() >= st.holdClosedUntil) applyOutput(st);
    auto inBefore = st.input.state();
    bool tryIn = st.input.tick(!st.audio.inputGone.load());
    if (st.input.state() != inBefore)
        st.console.log("audio", std::string("Sound in: ") + DeviceLife::name(inBefore) + " -> " + DeviceLife::name(st.input.state()),
                       st.input.state() == DeviceLife::State::Running ? 0 : 1);
    if (tryIn && juce::Time::getMillisecondCounter() >= st.inputBackAt) st.audio.inputGone = false;  // the emulator comes back
}

class StatusBar : public juce::Component, private juce::Timer {
public:
    explicit StatusBar(Studio& s) : st(s) {
        addAndMakeVisible(rec);
        rec.setEnabled(false);  // recording comes with stage 4
        rec.setTooltip("Recording comes with stage 4");
        startTimerHz(15);
    }
    static constexpr int kHeight = 30;
    std::string phrase() const { return state; }
    std::string text() const { return tempoText + " | Master " + juce::String(meterDb(level), 1).toStdString() + " dB" + (clipShown ? " CLIP" : "") + " | " + state; }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff0c0c0c));
        g.setFont(14.0f);
        g.setColour(juce::Colours::lightgrey);
        g.drawText(tempoText, 80, 0, 300, getHeight(), juce::Justification::centredLeft);
        // spec 11: -36..0 dB, left and right, amber above -3 dB, a white tick for the held maximum
        for (size_t c = 0; c < 2; ++c) {
            auto bar = juce::Rectangle<float>(390, 8.0f + float(c) * 8.0f, 160, 6);
            g.setColour(juce::Colour(0xff222222)); g.fillRect(bar);
            float w = dbToX(shownDb[c], bar.getWidth()), w3 = dbToX(-3.0f, bar.getWidth());
            g.setColour(juce::Colour(0xff33cc33)); g.fillRect(bar.withWidth(std::min(w, w3)));
            if (w > w3) { g.setColour(juce::Colour(0xffffb300)); g.fillRect(bar.withX(bar.getX() + w3).withWidth(w - w3)); }
            g.setColour(juce::Colours::white); g.fillRect(bar.getX() + dbToX(holdDb[c], bar.getWidth()) - 1, bar.getY(), 2.0f, bar.getHeight());
        }
        auto m = juce::Rectangle<float>(390, 8, 160, 14);
        g.setColour(clipShown ? juce::Colours::red : juce::Colour(0xff333333));
        g.fillRect(m.getRight() + 4, m.getY(), 10.0f, m.getHeight());
        g.setColour(juce::Colours::grey); g.drawText("Master", 556 + 14, 0, 60, getHeight(), juce::Justification::centredLeft);
        g.setColour(state == "all ok" ? juce::Colour(0xff4caf50) : juce::Colour(0xffff9800));
        g.drawText(state, 650, 0, getWidth() - 660, getHeight(), juce::Justification::centredRight, true);
    }
    void resized() override { rec.setBounds(8, 4, 60, getHeight() - 8); }
    std::function<void()> onWarning;  // a click on the warning opens Start check (spec 15)
    void mouseUp(const juce::MouseEvent& e) override { if (e.x >= 650 && state != "all ok" && onWarning) onWarning(); }
private:
    void timerCallback() override {
        level = st.audio.masterPeak.load();
        const auto now = juce::Time::getMillisecondCounter();
        for (size_t c = 0; c < 2; ++c) {
            float db = meterDb(st.audio.masterLR[c].load());
            shownDb[c] = std::max(db, shownDb[c] - 20.0f / 15.0f);  // falls 20 dB a second at 15 Hz
            if (db >= holdDb[c] || now > holdUntil[c]) { holdDb[c] = db; holdUntil[c] = now + 1500; }
        }
        if (st.audio.masterClip.exchange(false)) clipUntil = now + 3000;
        clipShown = juce::Time::getMillisecondCounter() < clipUntil;
        if (++tick % 15 == 0 || state.empty()) {  // the check reads disks: once a second
            tickDevices(st);
            auto lost = st.master.warning();
            if (auto w = st.input.warning(); !w.empty()) lost = lost.empty() ? w : lost + "; " + w;  // every lost device is named
            state = lost.empty() ? statusPhrase(startCheck(gatherFacts(st))) : lost;
            double bpm = st.audio.engine ? st.audio.engine->tempo() : 0;
            tempoText = juce::String(bpm, 1).toStdString() + " BPM  MC-707 emulator  via " + tempoPathLabel(st.settings.get("devices.tempoPath"));
        }
        repaint();
    }
    Studio& st;
    juce::TextButton rec{"Rec"};
    static float meterDb(float a) { return a > 1e-6f ? 20.0f * std::log10(a) : -120.0f; }
    static float dbToX(float db, float width) { return width * (std::clamp(db, -36.0f, 0.0f) + 36.0f) / 36.0f; }
    std::array<float, 2> shownDb{-120, -120}, holdDb{-120, -120};
    std::array<juce::uint32, 2> holdUntil{};
    float level = 0; bool clipShown = false; juce::uint32 clipUntil = 0; int tick = 0;
    std::string state, tempoText;
};

class Main : public juce::Component {
public:
    Main(Studio& s, SystemRunner& sys) : sources(s.audio), console(s.console, sys), check(s), wizard(s), settingsPanel(s), studio(s) {
        addAndMakeVisible(sources);
        addAndMakeVisible(top);
        console.onDragHeight = [this](int px) {
            double share = juce::jlimit(0.1, 0.9, double(px) / std::max(1, getHeight()));
            studio.settings.set("console.height", juce::String(share, 2).toStdString());  // remembered (spec 15)
            console.setBounds(consoleArea(consoleOpen));
        };
        addAndMakeVisible(bottom);
        bottom.onWarning = [this] { studio.console.log("ui", "bottom bar warning clicked"); showCheck(true); };
        top.onButton = [this](const juce::String& b) {
            if (b == "Console") toggleConsole(!consoleOpen);
            else if (b == "Setup wizard") { showCheck(false); showSettings(""); showWizard(1); }
            else if (b == "Settings") { showCheck(false); showWizard(0); showSettings("Devices"); }
            else studio.console.log("ui", b.toStdString() + ": comes with its stage (DECKS 5, Look 2, Help 7)", 1);
        };
        addChildComponent(console);
        addChildComponent(check);
        addChildComponent(wizard);
        addChildComponent(settingsPanel);
        settingsPanel.onClose = [this] { showSettings(""); };
        check.onGo = [this] { showCheck(false); };
        check.onFix = [this](const std::string& f) {
            if (f.rfind("wizard:", 0) == 0) { showCheck(false); showWizard(std::stoi(f.substr(7))); }
            else if (f.rfind("settings:", 0) == 0) { showCheck(false); showSettings(f.substr(9)); }
            else studio.console.log("check", "Fix " + f + ": no target", 1);
        };
        wizard.onDone = [this] { showWizard(0); showCheck(true); };
        juce::Component::SafePointer<Main> self(this);
        juce::Timer::callAfterDelay(1000, [self] {  // levels need a second of sound
            if (!self) return;
            if (self->studio.firstRun) self->showWizard(1); else self->showCheck(true);
        });
        setWantsKeyboardFocus(true);
    }
    void paint(juce::Graphics& g) override { g.fillAll(juce::Colours::black); }
    void resized() override {
        top.setBounds(0, 0, getWidth(), TopBar::kHeight);
        bottom.setBounds(0, getHeight() - StatusBar::kHeight, getWidth(), StatusBar::kHeight);
        sources.setBounds(getLocalBounds().withTrimmedBottom(StatusBar::kHeight).removeFromBottom(160).reduced(16, 8));
        console.setBounds(consoleArea(consoleOpen));
        placeCheck();
    }
    void showCheck(bool open) {
        if (open) { check.refresh(); placeCheck(); check.setVisible(true); check.toFront(false); /* above the console and the top bar (spec 15) */ }
        else { check.setVisible(false); grabKeyboardFocus(); }
    }
    CheckPanel& checkPanel() { return check; }
    WizardPanel& wizardPanel() { return wizard; }
    TopBar& topBar() { return top; }
    StatusBar& statusBar() { return bottom; }
    std::string openLayers() const {
        std::string s;
        if (consoleOpen) s += " console";
        if (check.isVisible()) s += " check";
        if (wizard.isVisible()) s += " wizard";
        if (settingsPanel.isVisible()) s += " settings";
        return s.empty() ? " none" : s;
    }
    SettingsPanel& settings() { return settingsPanel; }
    void showSettings(const std::string& tab) {  // "" closes
        if (!tab.empty()) {
            if (!settingsPanel.openByName(tab)) settingsPanel.open(0);
            studio.console.log("settings", "opened tab " + std::to_string(settingsPanel.currentTab() + 1));
            placeCheck(); settingsPanel.setVisible(true); settingsPanel.toFront(false); /* above the console and the top bar (spec 15) */
        } else { if (settingsPanel.isVisible()) studio.console.log("settings", "closed"); settingsPanel.setVisible(false); grabKeyboardFocus(); }
    }
    void showWizard(int step) {  // 0 closes
        if (step > 0) { studio.console.log("wizard", "opened at step " + std::to_string(juce::jlimit(1, 6, step))); wizard.show(step); placeCheck(); wizard.setVisible(true); wizard.toFront(false); /* above the console and the top bar (spec 15) */ }
        else { wizard.setVisible(false); grabKeyboardFocus(); }
    }
    bool keyPressed(const juce::KeyPress& k) override {
        auto ch = k.getTextCharacter();
        // the key under Esc: ` ~ in Latin, ё Ё in Russian; keycode fallback for other layouts
        bool tilde = ch == '`' || ch == '~' || ch == 0x451 || ch == 0x401 || k.getKeyCode() == 0x60;
        // Esc closes the top layer: Settings, wizard, check lie over the console (spec 15)
        if (k == juce::KeyPress::escapeKey && settingsPanel.isVisible()) { showSettings(""); return true; }
        if (k == juce::KeyPress::escapeKey && wizard.isVisible()) { showWizard(0); showCheck(true); return true; }
        if (k == juce::KeyPress::escapeKey && check.isVisible()) { showCheck(false); return true; }
        if (tilde || (consoleOpen && k == juce::KeyPress::escapeKey)) { toggleConsole(tilde ? !consoleOpen : false); return true; }
        return false;
    }
    juce::String consoleInput() const { return console.inputText(); }
    void toggleConsole(bool open) {
        consoleOpen = open;
        studio.console.log("ui", open ? "console opened" : "console closed");
        auto& anim = juce::Desktop::getInstance().getAnimator();
        if (open) {
            console.setBounds(consoleArea(false));
            console.setVisible(true);
            top.toFront(false);  // the console slides out from under the top bar
            anim.animateComponent(&console, consoleArea(true), 1.0f, 150, false, 1.0, 1.0);
            console.focusInput();
        } else {
            anim.animateComponent(&console, consoleArea(false), 1.0f, 150, true, 1.0, 1.0);
            grabKeyboardFocus();
        }
    }
private:
    juce::Rectangle<int> consoleArea(bool open) const {
        double share = juce::String(studio.settings.get("console.height")).getDoubleValue();
        int h = int(getHeight() * juce::jlimit(0.1, 0.9, share > 0 ? share : 0.4));
        return {0, open ? TopBar::kHeight : -h, getWidth(), h};
    }
    void placeCheck() {
        int w = std::min(720, getWidth() - 32), h = std::min(check.wantedHeight(), getHeight() - 32);
        check.setBounds((getWidth() - w) / 2, (getHeight() - h) / 2, w, h);
        int wh = std::min(wizard.wantedHeight(), getHeight() - 32);
        wizard.setBounds((getWidth() - w) / 2, (getHeight() - wh) / 2, w, wh);
        int sw = std::min(960, getWidth() - 32), sh = std::min(settingsPanel.wantedHeight(), getHeight() - 32);
        settingsPanel.setBounds((getWidth() - sw) / 2, (getHeight() - sh) / 2, sw, sh);
    }
    Sources sources;
    TopBar top;
    ConsolePanel console;
    CheckPanel check;
    WizardPanel wizard;
    SettingsPanel settingsPanel;
    Studio& studio;
    StatusBar bottom{studio};  // after studio: it is built from it
    bool consoleOpen = false;
};

class MainWindow : public juce::DocumentWindow {
public:
    MainWindow(Studio& s, SystemRunner& sys) : DocumentWindow(appTitle(), juce::Colours::black, DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        setContentOwned(new Main(s, sys), false);
        setResizable(true, true);
        setResizeLimits(1024, 600, 16384, 16384);  // smallest window (spec 15)
        centreWithSize(1280, 800);
        setVisible(true);
        getContentComponent()->grabKeyboardFocus();
    }
    // On Linux this also comes from the window manager (WM_DELETE_WINDOW), so log what the window looked like
    void closeButtonPressed() override {
        static char why[160];
        std::snprintf(why, sizeof why, "window close button (after %u s, minimised %d, visible %d, focused %d)",
                      unsigned(juce::Time::getMillisecondCounter() / 1000), int(isMinimised()), int(isShowing()), int(hasKeyboardFocus(true)));
        quitReason = why;
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

// Reads the pattern file again when it changes; a bad file keeps the old pattern playing (section 14).
class PatternWatcher : private juce::Timer {
public:
    PatternWatcher(Audio& a, juce::File f) : audio(a), file(std::move(f)) { load(); startTimer(500); }
    juce::String lastError;
private:
    void timerCallback() override {
        if (file.getLastModificationTime() != stamp) load();
    }
    void load() {
        stamp = file.getLastModificationTime();
        const bool ok = audio.reloader.apply(file.loadFileAsString().toStdString(),
                                             file.getFileName().toStdString(), audio.engine.get());
        lastError = audio.reloader.lastError();
        juce::Logger::writeToLog(ok ? "pattern " + juce::String(audio.reloader.playingName()) + " loaded" : lastError);
    }
    Audio& audio;
    juce::File file;
    juce::Time stamp;
};

class App : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return appTitle(); }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    void initialise(const juce::String& args) override {
        studio = std::make_unique<Studio>();
        logger = std::make_unique<ConsoleLogger>(studio->console);
        juce::Logger::setCurrentLogger(logger.get());
        sys = std::make_unique<SystemRunner>(studio->console);
        {
            auto dir = appDir("data");
            auto state = studio->store.open(dir.getChildFile("studio.db").getFullPathName().toStdString());
            studio->console.log("db", studio->store.stateText(), state == Store::State::Open ? 0 : 1);
            if (state == Store::State::Open) {
                bool today = false;
                auto name = "studio-" + juce::Time::getCurrentTime().formatted("%Y-%m-%d").toStdString() + ".db";
                for (const auto& b : studio->store.backups()) today = today || juce::File(b).getFileName().toStdString() == name;
                if (!today) studio->console.log("db", "backup: " + studio->store.backup());
                studio->store.event("app", "start");
                studio->console.onCommand = [s = studio.get()](const std::string& l) { s->store.event("console", l); };
            }
        }
        addStageOneCommands(*studio, *sys);
        // --exec "cmd; cmd": run console commands at start, print the log, quit (checks and scripts)
        juce::String patternArg = args.trim();
        if (patternArg.startsWith("--exec")) {
            execLine = patternArg.fromFirstOccurrenceOf("--exec", false, false).trim().unquoted();
            patternArg.clear();
        }
        auto config = appDir("config");
        auto how = studio->settingsFile.load(config.getChildFile("settings.json"), studio->settings);
        studio->console.log("settings", studio->settingsFile.file().getFullPathName().toStdString() +
            (how == SettingsFile::Loaded::Broken ? ": file was broken, renamed to settings.broken.json, defaults used"
             : how == SettingsFile::Loaded::Newer ? ": file is from a newer version, defaults used, file not changed"
             : how == SettingsFile::Loaded::Fresh ? ": new file with defaults" : ""),
            how == SettingsFile::Loaded::Broken || how == SettingsFile::Loaded::Newer ? 1 : 0);
        studio->firstRun = how == SettingsFile::Loaded::Fresh;
        {
            auto state = appDir("state");
            state.createDirectory();
            studio->console.loadHistory(state.getChildFile("console-history").getFullPathName().toStdString());
        }
        studio->settings.onChange = [s = studio.get()](const std::string& key, const std::string& value) {
            s->settingsFile.save(s->settings);
            if (key == "audioOut.masterGainDb") s->audio.masterGain = juce::Decibels::decibelsToGain(juce::String(value).getFloatValue(), -200.0f);
            if (key == "emulator.tempo") {
                s->audio.ownTempo = juce::String(value).getDoubleValue();
                if (s->audio.engine) s->audio.engine->setOwnTempo(s->audio.ownTempo.load());
            }
            if (key == "audioOut.master.device" || key == "audioOut.phones.device" || key == "devices.main" || key == "audioOut.blockSize") applyOutput(*s);
        };
        // patterns folder: the built-in default.txt is copied there on first run (section 16)
        auto patterns = studio->settings.get("patterns.folder").empty() ? config.getChildFile("patterns")
                                                                       : juce::File(studio->settings.get("patterns.folder"));
        auto builtIn = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory()
                           .getSiblingFile("resources").getChildFile("patterns/default.txt");
        if (!patterns.getChildFile("default.txt").existsAsFile()) {
            patterns.createDirectory();
            builtIn.copyFileTo(patterns.getChildFile("default.txt"));
        }
        juce::File f = patternArg.isNotEmpty()
            ? juce::File::getCurrentWorkingDirectory().getChildFile(patternArg.unquoted())
            : patterns.getChildFile("default.txt");
        watcher = std::make_unique<PatternWatcher>(studio->audio, f);
        studio->audio.ownTempo = juce::String(studio->settings.get("emulator.tempo")).getDoubleValue();
        studio->audio.masterGain = juce::Decibels::decibelsToGain(juce::String(studio->settings.get("audioOut.masterGainDb")).getFloatValue(), -200.0f);
        auto err = studio->devices.initialiseWithDefaultDevices(0, 2);
        if (err.isNotEmpty()) juce::Logger::writeToLog("audio error: " + err);
        studio->devices.addAudioCallback(&studio->audio);
        applyOutput(*studio);
        juce::Timer::callAfterDelay(1000, [s = studio.get()] { printCheck(*s); });  // levels need a second of sound
        window = std::make_unique<MainWindow>(*studio, *sys);
        studio->console.add("press", "press LABEL: click a Start check button (Go, Fix, Restore, Start empty, Clean)", [this](const auto& a, Console& c) {
            juce::String label;
            for (size_t i = 1; i < a.size(); ++i) label << (i > 1 ? " " : "") << a[i];
            auto* m = dynamic_cast<Main*>(window->getContentComponent());
            bool done = m && (m->settings().isVisible() ? m->settings().press(label) : m->wizardPanel().isVisible() ? m->wizardPanel().press(label)
                              : m->checkPanel().isVisible() ? m->checkPanel().press(label) : false);
            if (!done && m) done = m->topBar().press(label);
            if (!done) c.log("console", "press: no button " + label.toStdString(), 1);
        });
        studio->console.add("settings", "settings [TAB]: open Settings (Devices, Audio out, Folders, Recording, Look, Keys, Maintenance)", [this](const auto& a, Console&) {
            juce::String tab;
            for (size_t i = 1; i < a.size(); ++i) tab << (i > 1 ? " " : "") << a[i];
            if (auto* m = dynamic_cast<Main*>(window->getContentComponent())) m->showSettings(tab.isEmpty() ? "Devices" : tab.toStdString());
        });
        studio->console.add("choose", "choose ROW = VALUE: set a row on the open Settings tab, e.g. choose Block size = 256", [this](const auto& a, Console& c) {
            juce::String all;
            for (size_t i = 1; i < a.size(); ++i) all << (i > 1 ? " " : "") << a[i];
            auto row = all.upToFirstOccurrenceOf("=", false, false).trim(), val = all.fromFirstOccurrenceOf("=", false, false).trim();
            auto* m = dynamic_cast<Main*>(window->getContentComponent());
            if (!m || !m->settings().isVisible() || !m->settings().pick(row, val)) c.log("console", "choose: no row or value " + all.toStdString(), 1);
        });
        studio->console.add("layers", "layers: which layers are open, bottom to top", [this](const auto&, Console& c) {
            auto* m = dynamic_cast<Main*>(window->getContentComponent());
            if (m) c.print("layers:" + m->openLayers());
        });
        studio->console.add("bar", "bar: what the bottom bar shows now", [this](const auto&, Console& c) {
            auto* m = dynamic_cast<Main*>(window->getContentComponent());
            if (m) c.print("bar: " + m->statusBar().text());
        });
        studio->console.add("key", "key esc|tilde: press a key in the main window; key type TEXT: type into the focused field (checks)", [this](const auto& a, Console& c) {
            auto* m = dynamic_cast<Main*>(window->getContentComponent());
            if (!m || a.size() < 2) { c.log("console", "key: esc or tilde", 1); return; }
            if (a[1] == "type") {  // key type TEXT: real key path through the window peer to the focused part
                auto* peer = m->getPeer();
                juce::String text;
                for (size_t i = 2; i < a.size(); ++i) text += (i > 2 ? " " : "") + juce::String(a[i]);
                auto name = [](juce::Component* x) { return x ? (x->getName().isEmpty() ? juce::String(typeid(*x).name()) : x->getName()).toStdString() : std::string("none"); };
                std::string before = name(juce::Component::getCurrentlyFocusedComponent());
                int taken = 0;
                if (peer) for (auto ch : text) taken += peer->handleKeyPress(juce::KeyPress(int(ch), 0, ch)) ? 1 : 0;
                auto* fc = juce::Component::getCurrentlyFocusedComponent();
                // debug for the Mac runner: is there a peer, is the window and the app in front, where focus was
                c.print("key type: peer " + std::string(peer ? "yes" : "no") + ", peer focused " + std::string(peer && peer->isFocused() ? "yes" : "no")
                        + ", app in front " + std::string(juce::Process::isForegroundProcess() ? "yes" : "no")
                        + ", focus before " + before + ", after " + name(fc) + ", keys taken " + std::to_string(taken) + " of " + std::to_string(text.length()));
                c.print("key type: focus " + (fc ? (dynamic_cast<juce::TextEditor*>(fc) ? std::string("text field") : std::string("other")) : std::string("none")) + ", console line '" + m->consoleInput().toStdString() + "'");
                return;
            }
            m->keyPressed(a[1] == "esc" ? juce::KeyPress(juce::KeyPress::escapeKey) : juce::KeyPress('`', 0, '`'));
        });
        studio->console.add("wizard", "wizard [STEP]: open Setup wizard at step 1-6", [this](const auto& a, Console&) {
            if (auto* m = dynamic_cast<Main*>(window->getContentComponent())) m->showWizard(a.size() > 1 ? juce::String(a[1]).getIntValue() : 1);
        });
        studio->console.add("pick", "pick [2] ITEM: choose ITEM in the wizard list (2 = the second list, Phones)", [this](const auto& a, Console& c) {
            bool second = a.size() > 2 && a[1] == "2";
            juce::String item;
            for (size_t i = second ? 2 : 1; i < a.size(); ++i) item << (item.isEmpty() ? "" : " ") << a[i];
            auto* m = dynamic_cast<Main*>(window->getContentComponent());
            if (!m || !m->wizardPanel().pick(item, second)) c.log("console", "pick: no item " + item.toStdString(), 1);
        });
        studio->console.add("type", "type PATH: write PATH into the wizard folder field", [this](const auto& a, Console& c) {
            juce::String text;
            for (size_t i = 1; i < a.size(); ++i) text << (i > 1 ? " " : "") << a[i];
            auto* m = dynamic_cast<Main*>(window->getContentComponent());
            if (!m || !m->wizardPanel().type(text)) c.log("console", "type: no folder field on this step", 1);
        });
        studio->console.add("uishot", "uishot FILE.png: save a picture of the laptop window (with panels)", [this](const auto& a, Console& c) {
            if (a.size() < 2) { c.log("console", "uishot: file name expected", 1); return; }
            auto* m = window->getContentComponent();
            auto img = m->createComponentSnapshot(m->getLocalBounds());
            juce::File shot = juce::File::getCurrentWorkingDirectory().getChildFile(a[1]);
            shot.deleteFile();
            juce::FileOutputStream out(shot);
            juce::PNGImageFormat png;
            c.print(out.openedOk() && png.writeImageToStream(img, out) ? "saved " + shot.getFullPathName().toStdString() : "uishot: cannot write " + a[1]);
        });
        if (execLine.isNotEmpty()) {
            execCmds = juce::StringArray::fromTokens(execLine, ";", "\"");
            juce::Timer::callAfterDelay(1500, [this] { runExec(0); });
        }
    }
    // Runs --exec commands in order; "wait MS" lets the app run (timers, sound) before the next one.
    void runExec(int i) {
        for (; i < execCmds.size(); ++i) {
            auto cmd = execCmds[i].trim();
            if (cmd.startsWith("wait ")) {
                int ms = juce::jlimit(0, 3600000, cmd.fromFirstOccurrenceOf("wait ", false, false).getIntValue());
                juce::Timer::callAfterDelay(ms, [this, i] { runExec(i + 1); });
                return;
            }
            studio->console.run(cmd.toStdString());
        }
        juce::Timer::callAfterDelay(1500, [this] {
            for (const auto& l : studio->console.lines())
                std::printf("%s %s %s\n", l.area.c_str(), l.level ? "!" : " ", l.text.c_str());
            std::fflush(stdout);
            quitReason = "--exec finished";
            systemRequestedQuit();
        });
    }
    juce::StringArray execCmds;
    void shutdown() override {
        studio->devices.removeAudioCallback(&studio->audio);
        watcher.reset();
        window.reset();
        sys.reset();
        studio->store.event("app", "quit: " + std::string(quitReason ? quitReason : "not marked by the app (signal or session end)"));
        studio->store.close();
        juce::Logger::setCurrentLogger(nullptr);
        logger.reset();
        studio.reset();
    }
private:
    std::unique_ptr<Studio> studio;
    std::unique_ptr<ConsoleLogger> logger;
    std::unique_ptr<SystemRunner> sys;
    std::unique_ptr<PatternWatcher> watcher;
    std::unique_ptr<MainWindow> window;
    juce::String execLine;
};
}

START_JUCE_APPLICATION(App)
