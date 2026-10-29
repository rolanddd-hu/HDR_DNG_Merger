#include <catch2/catch_test_macros.hpp>
#include "core/Types.h"

TEST_CASE("Types basic tests", "[types]") {
    hdr::CfaPattern pattern;
    REQUIRE(pattern.name() == "RGGB");
    REQUIRE(pattern.at(0, 0) == 0); // R
    REQUIRE(pattern.at(0, 1) == 1); // G
    REQUIRE(pattern.at(1, 0) == 1); // G
    REQUIRE(pattern.at(1, 1) == 2); // B

    auto shifted = pattern.shifted(1, 1);
    REQUIRE(shifted.name() == "BGGR");
}
