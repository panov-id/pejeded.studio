#include "emulators/Mc707Engine.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <cstring>
#include <string>
#include <vector>

namespace pejeded::mc707 {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxNotes = 4;
constexpr int kVoices = 16;
constexpr int kMaxRpn = 48;
constexpr int kDelaySize = 1 << 17;  // per side, enough for 16 steps at 40 BPM? no: about 2.7 s at 48 kHz

enum Kind {
    KICK, SNARE, CLAP, TICK, HAT, OPENHAT, TOM, RIM, PERC, NOISE, // drums
    BASS, LEAD, PLUCK, PAD                                        // notes
};

int drumKind(const std::string& s) {
    static const char* names[] = {"KICK", "SNARE", "CLAP", "TICK", "HAT", "OPENHAT", "TOM", "RIM", "PERC", "NOISE"};
    for (int i = 0; i < 10; ++i)
        if (s == names[i]) return i;
    return NOISE;
}

int noteKind(const std::string& t) {
    if (t == "BASS") return BASS;
    if (t == "LEAD") return LEAD;
    if (t == "PLUCK") return PLUCK;
    return PAD;
}

// ---------- formulas: compiled to postfix off the audio thread ----------
enum Op : unsigned char { NUM, VAR_N, VAR_S, VAR_B, VAR_RNDF, FN_RND, FN_SIN, DRUM, ADD, SUB, MUL, DIV, MOD, NEG };
struct Tok {
    Op op = NUM;
    double v = 0;   // number, or drum kind for DRUM
};
struct Formula {
    bool present = false;
    int n = 0;
    std::array<Tok, kMaxRpn> rpn{};
};

struct FormulaCompiler {
    const std::string& s;
    size_t i = 0;
    std::vector<Tok> out;
    bool ok = true;
    void skip() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; }
    void expr() {
        term();
        for (;;) {
            skip();
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                char c = s[i++];
                term();
                out.push_back({c == '+' ? ADD : SUB, 0});
            } else return;
        }
    }
    void term() {
        unary();
        for (;;) {
            skip();
            if (i < s.size() && (s[i] == '*' || s[i] == '/' || s[i] == '%')) {
                char c = s[i++];
                unary();
                out.push_back({c == '*' ? MUL : c == '/' ? DIV : MOD, 0});
            } else return;
        }
    }
    void unary() {
        skip();
        if (i < s.size() && s[i] == '-') { ++i; unary(); out.push_back({NEG, 0}); return; }
        primary();
    }
    void primary() {
        skip();
        if (i >= s.size()) { ok = false; return; }
        if (s[i] == '(') {
            ++i; expr(); skip();
            if (i < s.size() && s[i] == ')') ++i; else ok = false;
            return;
        }
        if (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.') {
            size_t st = i;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
            out.push_back({NUM, std::stod(s.substr(st, i - st))});
            return;
        }
        size_t st = i;
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
        std::string w = s.substr(st, i - st);
        if (w.empty()) { ok = false; return; }
        skip();
        bool call = i < s.size() && s[i] == '(';
        if (w == "RND" || w == "SIN") {
            if (call) {
                ++i; expr(); skip();
                if (i < s.size() && s[i] == ')') ++i; else ok = false;
                out.push_back({w == "RND" ? FN_RND : FN_SIN, 0});
            } else if (w == "RND") out.push_back({VAR_RNDF, 0});
            else ok = false;
            return;
        }
        if (w == "N") out.push_back({VAR_N, 0});
        else if (w == "S") out.push_back({VAR_S, 0});
        else if (w == "B") out.push_back({VAR_B, 0});
        else {
            static const char* d[] = {"KICK", "SNARE", "CLAP", "TICK", "HAT", "OPENHAT", "TOM", "RIM", "PERC", "NOISE"};
            bool found = false;
            for (int k = 0; k < 10; ++k)
                if (w == d[k]) { out.push_back({DRUM, static_cast<double>(k)}); found = true; }
            if (!found) ok = false;
        }
    }
};

Formula compile(const std::map<std::string, std::string>& f, const char* key) {
    Formula r;
    auto it = f.find(key);
    if (it == f.end()) return r;
    FormulaCompiler c{it->second, 0, {}, true};
    c.expr();
    c.skip();
    if (!c.ok || c.i != it->second.size() || c.out.size() > static_cast<size_t>(kMaxRpn)) return r;  // parser accepted the text; bad = ignored
    r.present = true;
    r.n = static_cast<int>(c.out.size());
    std::copy(c.out.begin(), c.out.end(), r.rpn.begin());
    return r;
}

// ---------- prepared pattern ----------
struct PStep {
    unsigned char hit = 0;
    bool noteStart = false;
    bool accent = false;
    int nNotes = 0;
    std::array<int, kMaxNotes> notes{};
    int holdSteps = 1;
};

struct PLine {
    int kind = NOISE;
    std::vector<PStep> steps;
    float level = 1.0f;
    float pan = 0.0f;
    bool scatter = false;
    int pingpong = 0;
    float reverb = 0.0f;
    Formula fNote, fPan, fLevel;
};

struct PTrack {
    bool present = false;
    bool drums = false;
    int kind = PAD;
    float pan = 0.0f, width = 0.0f, level = 0.8f, reverb = 0.0f;
    bool inv = false;
    double autopan = 0.0, spin = 0.0;  // beats per cycle
    int pingpong = 0;
    int octave = 4;
    int scaleRoot = 60;
    int scaleLen = 7;
    std::array<int, 7> scale{0, 2, 4, 5, 7, 9, 11};
    int arp = 0;            // 0 off, 1 up, 2 down, 3 updown, 4 random
    double arpSteps = 1.0;  // arp note length in steps
    std::vector<PLine> lines;
};

struct Prepared {
    double tempo = 120.0;
    int length = 16;
    int grid = 16;
    std::array<PTrack, kTracks> tracks;
};

float num(const std::map<std::string, std::string>& m, const char* k, float def) {
    auto it = m.find(k);
    if (it == m.end()) return def;
    try { return std::stof(it->second); } catch (...) { return def; }
}

void setScale(PTrack& t, const std::string& name) {
    // name like AMIN, CMAJ, APENT, F#MIN
    std::string root = name.substr(0, name.size() > 1 && name[1] == '#' ? 2 : 1);
    std::string q = name.substr(root.size());
    int r = noteToMidi(root, t.octave);
    if (r < 0) r = noteToMidi("C", t.octave);
    t.scaleRoot = r;
    if (q == "MIN") { t.scale = {0, 2, 3, 5, 7, 8, 10}; t.scaleLen = 7; }
    else if (q == "PENT") { t.scale = {0, 3, 5, 7, 10, 0, 0}; t.scaleLen = 5; }
    else { t.scale = {0, 2, 4, 5, 7, 9, 11}; t.scaleLen = 7; }
}

std::unique_ptr<Prepared> prepare(const Pattern& p) {
    auto r = std::make_unique<Prepared>();
    r->tempo = p.tempo;
    r->length = p.length;
    r->grid = p.grid;
    for (const auto& tr : p.tracks) {
        PTrack& t = r->tracks[static_cast<size_t>(tr.number - 1)];
        t.present = true;
        t.drums = tr.type == "DRUMS";
        t.kind = t.drums ? NOISE : noteKind(tr.type);
        t.pan = num(tr.params, "PAN", 0.0f);
        t.width = std::clamp(num(tr.params, "WIDTH", 0.0f), 0.0f, 1.0f);
        t.level = std::clamp(num(tr.params, "LEVEL", 0.8f), 0.0f, 1.0f);
        t.reverb = std::clamp(num(tr.params, "REVERB", 0.0f), 0.0f, 1.0f);
        t.inv = tr.params.count("PHASE") > 0;
        t.autopan = num(tr.params, "AUTOPAN", 0.0f);
        t.spin = num(tr.params, "SPIN", 0.0f);
        t.pingpong = static_cast<int>(num(tr.params, "PINGPONG", 0.0f));
        t.octave = static_cast<int>(num(tr.params, "OCTAVE", 4.0f));
        setScale(t, tr.params.count("SCALE") ? tr.params.at("SCALE") : std::string("CMAJ"));
        if (tr.params.count("ARP")) {
            std::string a = tr.params.at("ARP");
            auto slash = a.find('/');
            std::string mode = a.substr(0, slash);
            t.arp = mode == "UP" ? 1 : mode == "DOWN" ? 2 : mode == "UPDOWN" ? 3 : mode == "RANDOM" ? 4 : 0;
            if (slash != std::string::npos) {
                int d = std::atoi(a.c_str() + slash + 1);
                if (d > 0) t.arpSteps = 16.0 / d;
            }
        }
        for (const auto& ln : tr.lines) {
            PLine l;
            l.kind = t.drums ? drumKind(ln.sound) : t.kind;
            l.level = std::clamp(num(ln.params, "LEVEL", 1.0f), 0.0f, 1.0f);
            l.pan = num(ln.params, "PAN", 0.0f);
            l.scatter = ln.params.count("SCATTER") > 0;
            l.pingpong = static_cast<int>(num(ln.params, "PINGPONG", 0.0f));
            l.reverb = std::clamp(num(ln.params, "REVERB", 0.0f), 0.0f, 1.0f);
            if (l.pingpong > 0 && t.pingpong == 0) t.pingpong = l.pingpong;
            l.fNote = compile(ln.formulas, "NOTE");
            l.fPan = compile(ln.formulas, "PAN");
            l.fLevel = compile(ln.formulas, "LEVEL");
            l.steps.resize(ln.steps.size());
            for (size_t s = 0; s < ln.steps.size(); ++s) {
                const Step& st = ln.steps[s];
                PStep& ps = l.steps[s];
                ps.hit = static_cast<unsigned char>(st.hit);
                ps.noteStart = st.noteStart;
                ps.accent = st.hit == Hit::Accent;
                ps.nNotes = std::min(static_cast<int>(st.notes.size()), kMaxNotes);
                for (int k = 0; k < ps.nNotes; ++k) ps.notes[static_cast<size_t>(k)] = st.notes[static_cast<size_t>(k)];
            }
            // hold length of every note start: following hits without a new bracket
            if (!t.drums) {
                const size_t n = l.steps.size();
                for (size_t s = 0; s < n; ++s) {
                    if (!l.steps[s].noteStart) continue;
                    int h = 1;
                    for (size_t k = s + 1; k < n && l.steps[k].hit != 0 && !l.steps[k].noteStart; ++k) ++h;
                    l.steps[s].holdSteps = h;
                }
            }
            t.lines.push_back(std::move(l));
        }
    }
    return r;
}

struct Voice {
    bool active = false;
    int kind = NOISE;
    double phase = 0, phase2 = 0, freq = 0, age = 0, gate = 0;
    float level = 0, pan = 0;
    float lp = 0;  // filter state
};

struct Delay {
    std::vector<float> l, r;
    int pos = 0;
    void init() { l.assign(kDelaySize, 0.0f); r.assign(kDelaySize, 0.0f); }
    float readL(int d) const { return l[static_cast<size_t>((pos - d) & (kDelaySize - 1))]; }
    float readR(int d) const { return r[static_cast<size_t>((pos - d) & (kDelaySize - 1))]; }
};

// Small Schroeder reverb: 4 combs + 2 allpasses per side, fixed buffers.
struct Reverb {
    static constexpr int kComb[4] = {1557, 1617, 1491, 1422};
    static constexpr int kAll[2] = {225, 556};
    std::array<std::vector<float>, 8> comb;
    std::array<std::vector<float>, 4> all;
    std::array<int, 8> cpos{};
    std::array<int, 4> apos{};
    void init() {
        for (int s = 0; s < 2; ++s) {
            for (int k = 0; k < 4; ++k) comb[static_cast<size_t>(s * 4 + k)].assign(static_cast<size_t>(kComb[k] + s * 23), 0.0f);
            for (int k = 0; k < 2; ++k) all[static_cast<size_t>(s * 2 + k)].assign(static_cast<size_t>(kAll[k] + s * 23), 0.0f);
        }
    }
    float process(int side, float x) {
        float y = 0;
        for (int k = 0; k < 4; ++k) {
            auto& b = comb[static_cast<size_t>(side * 4 + k)];
            int& p = cpos[static_cast<size_t>(side * 4 + k)];
            float o = b[static_cast<size_t>(p)];
            b[static_cast<size_t>(p)] = x + o * 0.80f;
            p = (p + 1) % static_cast<int>(b.size());
            y += o;
        }
        y *= 0.25f;
        for (int k = 0; k < 2; ++k) {
            auto& b = all[static_cast<size_t>(side * 2 + k)];
            int& p = apos[static_cast<size_t>(side * 2 + k)];
            float o = b[static_cast<size_t>(p)];
            float in = y + o * 0.5f;
            b[static_cast<size_t>(p)] = in;
            p = (p + 1) % static_cast<int>(b.size());
            y = o - in * 0.5f;
        }
        return y;
    }
};

} // namespace

struct Engine::Impl {
    double sr;
    std::uint32_t seed, rng;
    std::atomic<Prepared*> pending{nullptr};
    std::atomic<Prepared*> retired{nullptr};
    Prepared* cur = nullptr;
    std::array<std::atomic<bool>, kTracks> on{};
    std::atomic<double> ownTempo{0.0};
    std::atomic<double> tempoNow{120.0};
    std::atomic<bool> started{false};  // set by the audio thread on its first block
    std::atomic<int> stepNow{-1};
    long long stepCount = -1;     // steps since start
    double stepPos = 1e9;         // samples into the current step (big = trigger at once)
    double beat = 0;              // beats since start, for AUTOPAN and SPIN
    std::array<std::array<Voice, kVoices>, kTracks> voices{};
    std::array<std::vector<int>, kTracks> hitCount;  // per line, for N
    std::array<Delay, kTracks> width, ping;
    std::array<Reverb, kTracks> rev;
    std::array<float, kTracks> pingSend{};
    // arp state per track
    std::array<int, kTracks> arpIdx{}, arpDir{}, arpN{};
    std::array<std::array<int, kMaxNotes>, kTracks> arpNotes{};
    std::array<double, kTracks> arpLeft{}, arpPos{};
    std::array<float, kTracks> arpLevel{};

    Impl(double s, std::uint32_t sd) : sr(s), seed(sd ? sd : 1), rng(sd ? sd : 1) {
        for (auto& o : on) o.store(true);
        for (int t = 0; t < kTracks; ++t) {
            width[static_cast<size_t>(t)].init();
            ping[static_cast<size_t>(t)].init();
            rev[static_cast<size_t>(t)].init();
            hitCount[static_cast<size_t>(t)].assign(16, 0);
        }
    }
    ~Impl() {
        delete pending.load();
        delete retired.load();
        delete cur;
    }

    float rnd() {  // xorshift32, fixed seed: same pattern from start sounds the same
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return static_cast<float>(rng & 0xFFFFFF) / 16777216.0f;
    }

    double drumValue(int kind, int step) const {
        for (const auto& t : cur->tracks) {
            if (!t.present || !t.drums) continue;
            for (const auto& l : t.lines)
                if (l.kind == kind) return l.steps[static_cast<size_t>(step % static_cast<int>(l.steps.size()))].hit;
        }
        return 0;
    }

    double eval(const Formula& f, int n, int step) {
        double st[kMaxRpn];
        int sp = 0;
        for (int k = 0; k < f.n; ++k) {
            const Tok& t = f.rpn[static_cast<size_t>(k)];
            switch (t.op) {
                case NUM: st[sp++] = t.v; break;
                case VAR_N: st[sp++] = n; break;
                case VAR_S: st[sp++] = step; break;
                case VAR_B: st[sp++] = step / std::max(1, cur->grid); break;
                case VAR_RNDF: st[sp++] = rnd(); break;
                case DRUM: st[sp++] = drumValue(static_cast<int>(t.v), step); break;
                case FN_RND: { double k2 = std::floor(st[sp - 1]); st[sp - 1] = k2 >= 1 ? std::floor(rnd() * k2) : 0; break; }
                case FN_SIN: { double p = st[sp - 1]; st[sp - 1] = p != 0 ? std::sin(2 * kPi * step / p) : 0; break; }
                case NEG: st[sp - 1] = -st[sp - 1]; break;
                default: {
                    double b = st[--sp], a = st[sp - 1];
                    if (t.op == ADD) a += b;
                    else if (t.op == SUB) a -= b;
                    else if (t.op == MUL) a *= b;
                    else if (t.op == DIV) a = b != 0 ? a / b : 0;
                    else a = b != 0 ? std::fmod(a, b) : 0;
                    st[sp - 1] = a;
                }
            }
            if (sp <= 0 || sp >= kMaxRpn) return 0;
        }
        return sp == 1 ? st[0] : 0;
    }

    int degreeToMidi(const PTrack& t, int deg) const {
        int len = t.scaleLen;
        int oct = deg >= 0 ? deg / len : -((-deg + len - 1) / len);
        int idx = deg - oct * len;
        return t.scaleRoot + oct * 12 + t.scale[static_cast<size_t>(idx)];
    }

    void start(int ti, int kind, double freq, double gateSec, float level, float pan) {
        auto& vs = voices[static_cast<size_t>(ti)];
        Voice* v = &vs[0];
        for (auto& c : vs) {
            if (!c.active) { v = &c; break; }
            if (c.age > v->age) v = &c;
        }
        *v = Voice{};
        v->active = true;
        v->kind = kind;
        v->freq = freq;
        v->gate = gateSec;
        v->level = level;
        v->pan = pan;
    }

    static double mtof(int m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); }

    void triggerStep(int step) {
        const double stepSec = 60.0 / tempoNow.load() / 4.0;
        for (int ti = 0; ti < kTracks; ++ti) {
            PTrack& t = cur->tracks[static_cast<size_t>(ti)];
            if (!t.present) continue;
            auto& hc = hitCount[static_cast<size_t>(ti)];
            if (step == 0) std::fill(hc.begin(), hc.end(), 0);
            for (size_t li = 0; li < t.lines.size(); ++li) {
                PLine& l = t.lines[li];
                const PStep& s = l.steps[static_cast<size_t>(step % static_cast<int>(l.steps.size()))];
                if (s.hit == 0) continue;
                if (!t.drums && !s.noteStart && !l.fNote.present) continue;  // held note continues
                int n = hc[li]++;
                float acc = s.accent ? 1.0f : 0.7f;
                float lvl = t.level * l.level;
                if (l.fLevel.present) lvl = static_cast<float>(std::clamp(eval(l.fLevel, n, step), 0.0, 1.0));
                lvl *= acc;
                float pan = t.drums ? l.pan : 0.0f;
                if (l.fPan.present) pan = static_cast<float>(std::clamp(eval(l.fPan, n, step), -1.0, 1.0));
                if (l.scatter) pan = rnd() * 2.0f - 1.0f;
                if (t.drums) {
                    start(ti, l.kind, 0, 0, lvl, pan);
                    if (l.pingpong > 0) pingSend[static_cast<size_t>(ti)] = 0.6f;
                    continue;
                }
                double gate = (s.noteStart ? s.holdSteps : 1) * stepSec * 0.95;
                if (t.arp && s.nNotes > 0) {
                    arpN[static_cast<size_t>(ti)] = s.nNotes;
                    for (int k = 0; k < s.nNotes; ++k) arpNotes[static_cast<size_t>(ti)][static_cast<size_t>(k)] = s.notes[static_cast<size_t>(k)];
                    arpIdx[static_cast<size_t>(ti)] = 0;
                    arpDir[static_cast<size_t>(ti)] = 1;
                    arpLeft[static_cast<size_t>(ti)] = gate;
                    arpPos[static_cast<size_t>(ti)] = 0;
                    arpLevel[static_cast<size_t>(ti)] = lvl;
                    continue;
                }
                if (s.nNotes > 0) {
                    for (int k = 0; k < s.nNotes; ++k)
                        start(ti, l.kind, mtof(s.notes[static_cast<size_t>(k)]), gate, lvl / std::sqrt(static_cast<float>(s.nNotes)), 0);
                } else {
                    int deg = l.fNote.present ? static_cast<int>(std::floor(eval(l.fNote, n, step))) : 0;
                    start(ti, l.kind, mtof(degreeToMidi(t, deg)), gate, lvl, 0);
                }
            }
        }
    }

    void arpTick(int ti, double dt) {
        const size_t i = static_cast<size_t>(ti);
        if (arpLeft[i] <= 0 || arpN[i] == 0) return;
        PTrack& t = cur->tracks[i];
        const double noteSec = 60.0 / tempoNow.load() / 4.0 * t.arpSteps;
        if (arpPos[i] <= 0) {
            int n = arpN[i];
            int k = arpIdx[i];
            if (t.arp == 4) k = static_cast<int>(rnd() * n) % n;
            start(ti, t.kind, mtof(arpNotes[i][static_cast<size_t>(k)]), noteSec * 0.9, arpLevel[i], 0);
            if (t.arp == 1) arpIdx[i] = (k + 1) % n;
            else if (t.arp == 2) arpIdx[i] = (k - 1 + n) % n;
            else if (t.arp == 3 && n > 1) {
                int nx = k + arpDir[i];
                if (nx >= n || nx < 0) { arpDir[i] = -arpDir[i]; nx = k + arpDir[i]; }
                arpIdx[i] = nx;
            }
            arpPos[i] += noteSec;
        }
        arpPos[i] -= dt;
        arpLeft[i] -= dt;
    }

    float voiceSample(Voice& v, double dt) {
        float x = 0;
        const double a = v.age;
        auto noise = [&]() { return rnd() * 2.0f - 1.0f; };
        switch (v.kind) {
            case KICK: { double f = 45 + 110 * std::exp(-a * 30); v.phase += f * dt; x = static_cast<float>(std::sin(2 * kPi * v.phase) * std::exp(-a * 7)); if (a > 0.5) v.active = false; break; }
            case SNARE: { v.phase += 190 * dt; x = static_cast<float>((0.5 * std::sin(2 * kPi * v.phase) + 0.6 * noise()) * std::exp(-a * 18)); if (a > 0.3) v.active = false; break; }
            case CLAP: { double env = std::exp(-std::fmod(a, 0.011) * 250) * (a < 0.033 ? 1.0 : std::exp(-(a - 0.033) * 15)); x = static_cast<float>(noise() * env * 0.8); if (a > 0.4) v.active = false; break; }
            case TICK: { x = static_cast<float>(noise() * std::exp(-a * 300)); if (a > 0.03) v.active = false; break; }
            case HAT: case OPENHAT: { float nz = noise(); v.lp += 0.6f * (nz - v.lp); x = static_cast<float>((nz - v.lp) * std::exp(-a * (v.kind == HAT ? 60 : 9))); if (a > (v.kind == HAT ? 0.1 : 0.6)) v.active = false; break; }
            case TOM: { double f = 110 + 90 * std::exp(-a * 12); v.phase += f * dt; x = static_cast<float>(std::sin(2 * kPi * v.phase) * std::exp(-a * 9)); if (a > 0.5) v.active = false; break; }
            case RIM: { v.phase += 1700 * dt; x = static_cast<float>((std::fmod(v.phase, 1.0) < 0.5 ? 0.6 : -0.6) * std::exp(-a * 120)); if (a > 0.05) v.active = false; break; }
            case PERC: { v.phase += 820 * dt; x = static_cast<float>(std::sin(2 * kPi * v.phase) * std::exp(-a * 35)); if (a > 0.15) v.active = false; break; }
            case NOISE: { x = static_cast<float>(noise() * std::exp(-a * 6)); if (a > 0.8) v.active = false; break; }
            default: {
                v.phase += v.freq * dt;
                v.phase2 += v.freq * 1.006 * dt;
                double saw = 2 * std::fmod(v.phase, 1.0) - 1;
                double env;
                double rel = v.kind == PAD ? 0.4 : 0.03;
                double att = v.kind == PAD ? 0.25 : 0.003;
                if (a < v.gate) env = std::min(1.0, a / att);
                else env = std::exp(-(a - v.gate) / rel * 3);
                if (v.kind == PLUCK) env *= std::exp(-a * 6);
                if (a > v.gate + rel * 3) v.active = false;
                if (v.kind == BASS) { v.lp += 0.08f * (static_cast<float>(saw) - v.lp); x = v.lp * 1.4f; }
                else if (v.kind == LEAD) x = std::fmod(v.phase, 1.0) < 0.5 ? 0.45f : -0.45f;
                else if (v.kind == PLUCK) x = static_cast<float>(saw * 0.6);
                else x = static_cast<float>((saw + (2 * std::fmod(v.phase2, 1.0) - 1)) * 0.3);
                x = static_cast<float>(x * env);
            }
        }
        v.age += dt;
        return x * v.level;
    }
};

Engine::Engine(double sampleRate, std::uint32_t seed) : impl(std::make_unique<Impl>(sampleRate, seed)) {}
Engine::~Engine() = default;

void Engine::setPattern(const Pattern& p) {
    delete impl->retired.exchange(nullptr);
    auto prep = prepare(p);
    if (!impl->started.load()) impl->tempoNow.store(prep->tempo);  // shown before the first block
    delete impl->pending.exchange(prep.release());
}

void Engine::setTrackOn(int track, bool on) {
    if (track >= 1 && track <= kTracks) impl->on[static_cast<size_t>(track - 1)].store(on);
}
bool Engine::trackOn(int track) const {
    return track >= 1 && track <= kTracks && impl->on[static_cast<size_t>(track - 1)].load();
}
void Engine::setOwnTempo(double bpm) { impl->ownTempo.store(bpm); }
double Engine::tempo() const {
    const double own = impl->ownTempo.load();
    return own > 0 ? own : impl->tempoNow.load();
}
int Engine::currentStep() const { return impl->stepNow.load(); }

void Engine::render(float* const* out, int frames, const float* const* extIn, float* const* tracksOnly) {
    Impl& m = *impl;
    for (int c = 0; c < kChannels; ++c) std::memset(out[c], 0, sizeof(float) * static_cast<size_t>(frames));
    if (tracksOnly) for (int c = 0; c < 2; ++c) std::memset(tracksOnly[c], 0, sizeof(float) * static_cast<size_t>(frames));
    const double dt = 1.0 / m.sr;
    for (int f = 0; f < frames; ++f) {
        // step clock; a new pattern is taken at a bar start (or at once if none plays)
        m.started.store(true);
        if (!m.cur && m.pending.load()) m.cur = m.pending.exchange(nullptr);  // first pattern: at once
        double bpm = m.ownTempo.load() > 0 ? m.ownTempo.load() : (m.cur ? m.cur->tempo : 120.0);
        m.tempoNow.store(bpm);
        const double stepLen = m.sr * 60.0 / bpm / 4.0;
        if (m.stepPos >= stepLen) {
            m.stepPos -= m.stepPos >= 1e8 ? m.stepPos : stepLen;
            long long next = m.stepCount + 1;
            int grid = m.cur ? std::max(1, m.cur->grid) : 16;
            Prepared* pend = m.pending.load();
            if (pend && (!m.cur || next % grid == 0) && m.retired.load() == nullptr) {
                Prepared* old = m.cur;
                m.cur = m.pending.exchange(nullptr);
                m.retired.store(old);
                if (!old) next = 0;
                else next = next - (next % grid);
                // restart pattern position at a bar of the new pattern
                if (old && old->length != m.cur->length) next = 0;
            }
            m.stepCount = next;
            if (m.cur) {
                int step = static_cast<int>(m.stepCount % m.cur->length);
                m.stepNow.store(step);
                m.triggerStep(step);
            }
        }
        m.stepPos += 1.0;
        m.beat += dt * bpm / 60.0;
        float mixL = 0, mixR = 0;
        for (int ti = 0; ti < kTracks; ++ti) {
            const size_t i = static_cast<size_t>(ti);
            if (!m.cur || !m.cur->tracks[i].present) continue;
            const PTrack& t = m.cur->tracks[i];
            m.arpTick(ti, dt);
            float L = 0, R = 0, mono = 0;
            for (auto& v : m.voices[i]) {
                if (!v.active) continue;
                float x = m.voiceSample(v, dt);
                float p = std::clamp(t.pan + v.pan, -1.0f, 1.0f);
                if (t.autopan > 0) p = std::clamp(p + static_cast<float>(std::sin(2 * kPi * m.beat / t.autopan)), -1.0f, 1.0f);
                float gl = std::sqrt((1 - p) * 0.5f), gr = std::sqrt((1 + p) * 0.5f);
                L += x * gl;
                R += x * gr;
                mono += x;
            }
            if (t.spin > 0) {  // front -> right -> back (antiphase) -> left -> front
                double phi = 2 * kPi * m.beat / t.spin / 2 + kPi / 4;
                L = static_cast<float>(mono * std::cos(phi));
                R = static_cast<float>(mono * std::sin(phi));
            }
            // width: right side blends in a delayed copy (decorrelation)
            Delay& w = m.width[i];
            w.l[static_cast<size_t>(w.pos)] = L;
            w.r[static_cast<size_t>(w.pos)] = R;
            if (t.width > 0) {
                L = (1 - t.width * 0.5f) * L + t.width * 0.5f * w.readR(static_cast<int>(m.sr * 0.013));
                R = (1 - t.width * 0.5f) * R + t.width * 0.5f * w.readL(static_cast<int>(m.sr * 0.007));
            }
            w.pos = (w.pos + 1) & (kDelaySize - 1);
            if (t.inv) R = -R;
            // ping-pong echo: repeats alternate sides, fading
            if (t.pingpong > 0) {
                Delay& d = m.ping[i];
                int dl = std::min(kDelaySize - 1, static_cast<int>(stepLen * t.pingpong));
                float el = d.readR(dl), er = d.readL(dl);
                float in = (L + R) * 0.5f;
                d.l[static_cast<size_t>(d.pos)] = in * m.pingSend[i] + er * 0.5f;
                d.r[static_cast<size_t>(d.pos)] = el * 0.5f;
                d.pos = (d.pos + 1) & (kDelaySize - 1);
                m.pingSend[i] *= 0.9995f;
                L += el;
                R += er;
            }
            float rv = t.reverb;
            for (const auto& l : t.lines) rv = std::max(rv, l.reverb);
            if (rv > 0) {
                float in = (L + R) * 0.5f * 0.3f;
                L += rv * m.rev[i].process(0, in);
                R += rv * m.rev[i].process(1, in);
            }
            if (!m.on[i].load()) { L = 0; R = 0; }
            out[2 + ti * 2][f] = L;
            out[3 + ti * 2][f] = R;
            mixL += L;
            mixR += R;
        }
        if (tracksOnly) { tracksOnly[0][f] = mixL; tracksOnly[1][f] = mixR; }
        float eL = extIn ? extIn[0][f] : 0.0f, eR = extIn ? extIn[1][f] : 0.0f;
        out[18][f] = eL;
        out[19][f] = eR;
        out[0][f] = mixL + eL;
        out[1][f] = mixR + eR;
    }
}

} // namespace pejeded::mc707
