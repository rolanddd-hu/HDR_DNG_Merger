#include <catch2/catch_test_macros.hpp>
#include "core/BracketGrouper.h"

TEST_CASE("BracketGrouper fixed-N grouping", "[grouper]") {
    std::vector<hdr::FileEntry> files;
    for (int i = 0; i < 7; ++i) {
        hdr::FileEntry e;
        e.path = "sample_" + std::to_string(100 + i) + ".dng";
        e.info.make = "CameraBrand";
        e.info.model = "ModelA";
        e.info.width = 4000;
        e.info.height = 2250;
        e.info.exposureTime = 0.001 * (1 << (i % 3)); // 1ms, 2ms, 4ms repeating
        e.info.fNumber = 1.7;
        e.info.iso = 100;
        e.info.timestamp = 1000.0 + i * 2.0;
        files.push_back(e);
    }

    auto res = hdr::BracketGrouper::groupFixedN(files, 3);
    REQUIRE(res.groups.size() == 2);
    REQUIRE(res.ungrouped.size() == 1);
    CHECK(res.groups[0].files.size() == 3);
    CHECK(res.groups[1].files.size() == 3);
    CHECK(res.groups[0].status == hdr::BracketGroup::Status::Ok);
    CHECK(res.groups[1].status == hdr::BracketGroup::Status::Ok);
    CHECK_FALSE(res.warning.empty());
}

TEST_CASE("BracketGrouper validation checks", "[grouper]") {
    std::vector<hdr::FileEntry> files;
    hdr::FileEntry e1, e2;
    e1.info.make = "CameraBrand";
    e1.info.model = "ModelA";
    e1.info.width = 4000;
    e1.info.height = 2250;
    e1.info.exposureTime = 0.001;

    e2 = e1;
    e2.info.model = "ModelB"; // Different camera model

    files = {e1, e2};
    auto res = hdr::BracketGrouper::groupFixedN(files, 2);
    REQUIRE(res.groups.size() == 1);
    CHECK(res.groups[0].status == hdr::BracketGroup::Status::Error);
}
