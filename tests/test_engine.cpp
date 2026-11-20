#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>
#include "emulators/Mc707Engine.h"

using namespace pejeded::mc707;

namespace {
struct Buf {
    std::vector<std::vector<float>> ch;
    std::vector<float*> ptr;
    explicit Buf(int n, int frames) : ch(static_cast<size_t>(n), std::vector<float>(static_cast<size_t>(frames))) {
        for (auto& c : ch) ptr.push_back(c.data());
    }
};

// renders `seconds` in 512-frame blocks, returns per-channel energy
std::vector<double> run(Engine& e, double seconds, std::vector<std::vector<float>>* keep = nullptr,
                        const std::vector<float>* ext = nullptr, std::vector<double>* tracksOnly = nullptr) {
    const int block = 512;
    Buf b(kChannels, block), t(2, block), x(2, block);
    std::vector<double> en(kChannels, 0.0);
    if (tracksOnly) tracksOnly->assign(2, 0.0);
    int blocks = static_cast<int>(seconds * 48000 / block);
    for (int i = 0; i < blocks; ++i) {
        if (ext) for (int c = 0; c < 2; ++c) std::fill(x.ch[static_cast<size_t>(c)].begin(), x.ch[static_cast<size_t>(c)].end(), (*ext)[static_cast<size_t>(c)]);
        e.render(b.ptr.data(), block, ext ? x.ptr.data() : nullptr, t.ptr.data());
        for (int c = 0; c < kChannels; ++c)
            for (float v : b.ch[static_cast<size_t>(c)]) en[static_cast<size_t>(c)] += double(v) * v;
        if (tracksOnly) for (int c = 0; c < 2; ++c) for (float v : t.ch[static_cast<size_t>(c)]) (*tracksOnly)[static_cast<size_t>(c)] += double(v) * v;
        if (keep) for (int c = 0; c < kChannels; ++c) (*keep)[static_cast<size_t>(c)].insert((*keep)[static_cast<size_t>(c)].end(), b.ch[static_cast<size_t>(c)].begin(), b.ch[static_cast<size_t>(c)].end());
    }
    return en;
}

Pattern parse(const char* text) {
    auto r = parsePattern(text);
    INFO(r.errorLine << ": " << r.error);
    REQUIRE(r.ok);
    return r.pattern;
}
} // namespace

TEST_CASE("track 3 sounds only in its own pair and in MIX") {
    Engine e(48000);
    e.setPattern(parse("NAME T\nTEMPO 120\n[3] DRUMS\nKICK X000 X000 X000 X000\n"));
    auto en = run(e, 2.0);
    CHECK(en[6] > 1.0);   // ch 7-8 = track 3
    CHECK(en[7] > 1.0);
    CHECK(en[0] > 1.0);   // MIX
    for (int c = 2; c < 18; ++c)
        if (c != 6 && c != 7) CHECK(en[static_cast<size_t>(c)] == 0.0);
    CHECK(en[18] == 0.0);  // EXT IN silent without decks
}

TEST_CASE("switched off track is silent, others play") {
    Engine e(48000);
    e.setPattern(parse("NAME T\n[1] DRUMS\nKICK X000 X000 X000 X000\n[2] BASS\n1[C]111 0000 1[C]111 0000\n"));
    e.setTrackOn(1, false);
    auto en = run(e, 2.0);
    CHECK(en[2] == 0.0);
    CHECK(en[3] == 0.0);
    CHECK(en[4] > 1.0);
}

TEST_CASE("same seed gives the same sound, sample by sample") {
    const char* text = "NAME T\n[1] DRUMS\nHAT XXXX XXXX XXXX XXXX SC\nNOISE X000 0000 0000 0000\n";
    std::vector<std::vector<float>> a(kChannels), b(kChannels);
    Engine e1(48000, 7), e2(48000, 7);
    e1.setPattern(parse(text));
    e2.setPattern(parse(text));
    run(e1, 1.0, &a);
    run(e2, 1.0, &b);
    CHECK(a == b);
}

TEST_CASE("EXT IN goes to ch 19-20 and MIX, not to the tracks-only sum") {
    Engine e(48000);
    std::vector<float> ext{0.5f, -0.25f};
    std::vector<double> tr;
    auto en = run(e, 0.5, nullptr, &ext, &tr);
    CHECK(en[18] > 0.0);
    CHECK(en[19] > 0.0);
    CHECK(std::abs(en[0] - en[18]) < 1e-6);
    CHECK(tr[0] == 0.0);
    CHECK(tr[1] == 0.0);
}

TEST_CASE("PHASE=INV puts the right side in antiphase") {
    Engine e(48000);
    e.setPattern(parse("NAME T\n[6] PAD INV\n1[C]111 1111 1111 1111\n"));
    std::vector<std::vector<float>> k(kChannels);
    run(e, 1.0, &k);
    double corr = 0;
    for (size_t i = 0; i < k[12].size(); ++i) corr += double(k[12][i]) * k[13][i];
    CHECK(corr < 0.0);  // ch 13-14 = track 6
}

TEST_CASE("PAN moves the sound to one side") {
    Engine e(48000);
    e.setPattern(parse("NAME T\n[1] DRUMS PAN=-1\nKICK X000 X000 X000 X000\n"));
    auto en = run(e, 1.0);
    CHECK(en[2] > 100 * en[3]);
}

TEST_CASE("a new engine plays the pattern tempo from the first block") {
    Engine e(48000.0, 1);
    e.setPattern(parse("NAME T\nTEMPO 124\n[3] DRUMS\nKICK X000 X000 X000 X000\n"));
    Buf b(kChannels, 512), t(2, 512);
    e.render(b.ptr.data(), 512, nullptr, t.ptr.data());
    CHECK(std::abs(e.tempo() - 124.0) < 1e-9);
}

TEST_CASE("tempo is the pattern tempo even before the first block") {
    Engine e(48000.0, 1);
    e.setPattern(parse("NAME T\nTEMPO 124\n[3] DRUMS\nKICK X000 X000 X000 X000\n"));
    CHECK(std::abs(e.tempo() - 124.0) < 1e-9);
}

TEST_CASE("before the first block the last set pattern gives the tempo") {
    Engine e(48000.0, 1);
    CHECK(std::abs(e.tempo() - 120.0) < 1e-9);
    e.setPattern(parse("NAME A\nTEMPO 124\n[3] DRUMS\nKICK X000 X000 X000 X000\n"));
    e.setPattern(parse("NAME B\nTEMPO 98\n[3] DRUMS\nKICK X000 X000 X000 X000\n"));
    CHECK(std::abs(e.tempo() - 98.0) < 1e-9);
    Buf b(kChannels, 512), t(2, 512);
    e.render(b.ptr.data(), 512, nullptr, t.ptr.data());
    CHECK(std::abs(e.tempo() - 98.0) < 1e-9);
    e.setOwnTempo(130);
    CHECK(std::abs(e.tempo() - 130.0) < 1e-9);
}

TEST_CASE("the emulator keeps time at any device rate: 44100, 48000 and 96000 reach the same step") {
    // 120 BPM, a step is 125 ms; the checks fall in the middle of steps 18 and 24, far from an edge
    double rates[3] = {44100.0, 48000.0, 96000.0};
    int at[3][2] = {};
    for (int r = 0; r < 3; ++r) {
        Engine e(rates[r]);
        e.setPattern(parse("NAME T\nTEMPO 120\n[3] DRUMS\nKICK X000 X000 X000 X000\n"));
        int block = int(rates[r]) / 1000;  // 1 ms
        Buf b(kChannels, block);
        for (int i = 0; i < 3062; ++i) {
            e.render(b.ptr.data(), block);
            if (i + 1 == 2312) at[r][0] = e.currentStep();
        }
        at[r][1] = e.currentStep();
    }
    for (int r = 0; r < 3; ++r) {
        INFO("rate " << rates[r]);
        CHECK(at[r][0] == at[1][0]);
        CHECK(at[r][1] == at[1][1]);
        CHECK(at[r][1] != at[r][0]);  // the playhead moved
    }
}
