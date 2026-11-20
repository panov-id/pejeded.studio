#include <catch2/catch_test_macros.hpp>
#include "devices/DeviceLife.h"
using namespace pejeded;
using S = DeviceLife::State;

TEST_CASE("device life: open at start runs, a loss warns and retries every tick, the return runs again") {
    DeviceLife d("Master output");
    CHECK_FALSE(d.tick(true));
    CHECK(d.state() == S::Running);
    CHECK(d.warning().empty());
    CHECK_FALSE(d.tick(false));            // the loss itself
    CHECK(d.state() == S::Lost);
    CHECK(d.warning() == "Master output: lost, trying again every second");
    CHECK(d.tick(false));                  // a second later: try to open
    CHECK(d.state() == S::Opening);
    CHECK(d.tick(false));                  // and every second after
    CHECK_FALSE(d.tick(true));             // back without a restart
    CHECK(d.state() == S::Running);
    CHECK(d.warning().empty());
    CHECK(d.lossCount() == 1);
}

TEST_CASE("device life: absent at start keeps trying and is not called lost") {
    DeviceLife d("Sound in");
    CHECK(d.tick(false));
    CHECK(d.state() == S::Opening);
    CHECK(d.warning() == "Sound in: not open, trying again every second");
    CHECK_FALSE(d.wasLost());
    CHECK_FALSE(d.tick(true));
    CHECK(d.state() == S::Running);
}

TEST_CASE("device life: every state has a name") {
    for (auto s : {S::Absent, S::Opening, S::Running, S::Lost}) CHECK(std::string(DeviceLife::name(s)) != "?");
}
