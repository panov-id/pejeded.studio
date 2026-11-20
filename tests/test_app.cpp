#include <catch2/catch_test_macros.hpp>
#include <string>
#include "app/AppInfo.h"

TEST_CASE("app title is fixed in code") {
    REQUIRE(std::string(pejeded::appTitle()) == "PEJEDED STUDIO");
}
