#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "TestUtil.h"
#include "core/BatchProcessor.h"
#include "core/DngWriter.h"
#include "core/FileScanner.h"
#include "core/MergeJob.h"
#include "core/MetadataCopier.h"
#include "core/PathUtil.h"
#include "core/RawLoader.h"
#include "core/Settings.h"
#include <filesystem>
#include <fstream>

using namespace hdr;
namespace fs = std::filesystem;
using testutil::TempDir;

namespace {

FileEntry entryFor(const fs::path& p) {
    FileEntry e;
    e.path = p;
    auto [status, info] = RawLoader::loadMetadata(p);
    e.status = status;
    e.info = info;
    return e;
}

// Writes a small Int16 DNG with the given exposure time and returns its path.
fs::path writeFrame(const fs::path& dir, const std::string& name, double exposureTime, float scale) {
    const int W = 64, H = 64;
    RawImage meta = testutil::makeMetaSource(W, H);
    meta.exposure.exposureTime = exposureTime;
    ImageF cfa(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            cfa.at(y, x) = std::min(1.0f, scale * (0.05f + 0.9f * x / W));
    const fs::path p = dir / name;
    DngWriteParams params;
    params.bits = FloatBits::Int16;
    REQUIRE(DngWriter::writeFloatDng(p, cfa, W, H, meta, params).ok);
    return p;
}

} // namespace

TEST_CASE("Output file names are sanitised", "[safety]") {
    CHECK(MergeJob::sanitizeFilename("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j");
    CHECK(MergeJob::sanitizeFilename("..") == "HDR");
    CHECK(MergeJob::sanitizeFilename("  name. ") == "name");
    CHECK(MergeJob::sanitizeFilename("CON") == "_CON");
    CHECK(MergeJob::sanitizeFilename("lpt1.txt") == "_lpt1.txt");

    BracketGroup group;
    FileEntry f;
    f.path = "dir/RAW_0001.dng";
    group.files = {f};
    const std::string name = MergeJob::formatOutputFilename("..\\..\\evil/{first}", group, 0);
    CHECK(name.find('/') == std::string::npos);
    CHECK(name.find('\\') == std::string::npos);
    CHECK(fs::path(name).filename().string() == name);
    CHECK(name.substr(name.size() - 4) == ".dng");
    CHECK(MergeJob::formatOutputFilename("{first}.DNG", group, 0) == "RAW_0001.dng");
}

TEST_CASE("Batch output names never collide", "[safety]") {
    std::vector<BracketGroup> groups(3);
    for (size_t i = 0; i < groups.size(); ++i) {
        FileEntry f;
        f.path = "frame" + std::to_string(i) + ".dng";
        groups[i].files = {f};
    }
    const auto paths = MergeJob::planOutputPaths(groups, "same", "out");
    REQUIRE(paths.size() == 3);
    CHECK(paths[0].filename() == "same.dng");
    CHECK(paths[1].filename() == "same_2.dng");
    CHECK(paths[2].filename() == "same_3.dng");
}

TEST_CASE("A merge never overwrites its own input files", "[safety]") {
    TempDir dir("test_overwrite");
    const fs::path a = writeFrame(dir.path, "A.dng", 0.001, 0.25f);
    const fs::path b = writeFrame(dir.path, "B.dng", 0.004, 1.0f);
    const auto sizeBefore = fs::file_size(a);

    BracketGroup group;
    group.name = "g";
    group.files = {entryFor(a), entryFor(b)};

    // The default pattern "{first}" in the input folder would resolve to the first input
    auto status = MergeJob::execute(group, dir.path / MergeJob::formatOutputFilename("{first}", group, 0), MergeSettings{});
    CHECK_FALSE(status.ok);
    CHECK(fs::file_size(a) == sizeBefore);
    // Different spelling of the same path is caught too
    CHECK_FALSE(MergeJob::checkOutputPath(dir.path / "." / "a.DNG", group).ok);

    // A proper output name works and leaves no temporary file behind
    MergeSettings settings;
    settings.align = AlignMode::None;
    const fs::path out = dir.path / "merged.dng";
    REQUIRE(MergeJob::execute(group, out, settings).ok);
    CHECK(fs::exists(out));
    int tmpFiles = 0;
    for (const auto& e : fs::directory_iterator(dir.path)) tmpFiles += e.path().extension() == ".tmp";
    CHECK(tmpFiles == 0);
}

TEST_CASE("Failed or cancelled merges leave no partial output", "[safety]") {
    TempDir dir("test_partial");
    const fs::path a = writeFrame(dir.path, "A.dng", 0.001, 0.25f);

    BracketGroup group;
    group.name = "broken";
    FileEntry missing;
    missing.path = dir.path / "does_not_exist.dng";
    group.files = {entryFor(a), missing};

    const fs::path out = dir.path / "out.dng";
    CHECK_FALSE(MergeJob::execute(group, out, MergeSettings{}).ok);
    CHECK_FALSE(fs::exists(out));

    std::stop_source stop;
    stop.request_stop();
    group.files = {entryFor(a)};
    CHECK_FALSE(MergeJob::execute(group, out, MergeSettings{}, nullptr, stop.get_token()).ok);
    CHECK_FALSE(fs::exists(out));

    size_t files = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir.path)) files++;
    CHECK(files == 1);  // only the input
}

TEST_CASE("Non-ASCII paths work end to end", "[safety][unicode]") {
    TempDir dir(fs::temp_directory_path() / pathFromUtf8("hdr_t\xc3\xa9st_\xc5\x91_\xe6\x97\xa5\xe6\x9c\xac"));
    const fs::path a = writeFrame(dir.path, toUtf8(pathFromUtf8("\xc3\xa1rv\xc3\xadzt\xc5\xb1r\xc5\x91_1.dng")), 0.001, 0.25f);
    const fs::path b = writeFrame(dir.path, "frame_2.dng", 0.004, 1.0f);

    auto [status, info] = RawLoader::loadMetadata(a);
    REQUIRE(status.ok);
    auto [fullStatus, img] = RawLoader::loadFull(a);
    REQUIRE(fullStatus.ok);

    const auto scanned = FileScanner::scanPath(dir.path);
    REQUIRE(scanned.size() == 2);

    BracketGroup group;
    group.name = "unicode";
    group.files = {entryFor(a), entryFor(b)};
    MergeSettings settings;
    settings.align = AlignMode::None;
    const fs::path out = dir.path / pathFromUtf8("\xc3\xb6sszef\xc5\xb1z\xc3\xb6tt.dng");
    REQUIRE(MergeJob::execute(group, out, settings).ok);
    REQUIRE(fs::exists(out));
    CHECK(RawLoader::loadMetadata(out).first.ok);
}

TEST_CASE("File sorting is a strict ordering with mixed timestamps", "[safety]") {
    std::vector<FileEntry> entries;
    auto add = [&](const char* name, double ts) {
        FileEntry e;
        e.path = name;
        e.info.timestamp = ts;
        entries.push_back(e);
    };
    add("c.dng", 0.0);
    add("b.dng", 100.0);
    add("a.dng", 0.0);
    add("d.dng", 100.0);
    add("e.dng", 50.0);
    FileScanner::sortEntries(entries);
    std::vector<std::string> order;
    for (const auto& e : entries) order.push_back(e.path.string());
    CHECK(order == std::vector<std::string>{"e.dng", "b.dng", "d.dng", "a.dng", "c.dng"});
}

TEST_CASE("Settings survive malformed files and round-trip UTF-8 paths", "[safety][settings]") {
    TempDir dir("test_settings");
    const fs::path file = dir.path / "settings.json";
    {
        std::ofstream f(file);
        f << R"({"framesPerBracket": -5, "threadCount": "many", "windowWidth": 99999,
                 "merge": {"bits": 7, "clipThreshold": 0.0, "featherRadius": 1000, "namePattern": 42, "deghost": 9},
                 "tone": {"exposure": 50, "toneMap": 99}})";
    }
    AppSettings s = AppSettings::loadFrom(file);
    CHECK(s.framesPerBracket == 2);
    CHECK(s.threadCount == 0);
    CHECK(s.windowWidth == 7680);
    CHECK(s.merge.bits == FloatBits::Auto);
    CHECK(s.merge.clipThreshold >= 0.5f);
    CHECK(s.merge.featherRadius == 64);
    CHECK(s.merge.namePattern == "{first}_HDR");
    CHECK(s.merge.deghost == DeghostLevel::Off);
    CHECK(s.tone.exposure == 5.0f);
    CHECK(s.tone.toneMap == ToneMapOp::Filmic);

    // Garbage instead of JSON gives defaults
    {
        std::ofstream f(file);
        f << "{ not json";
    }
    CHECK(AppSettings::loadFrom(file).framesPerBracket == AppSettings{}.framesPerBracket);

    AppSettings out;
    out.lastInputFolder = "C:\\Fot\xc3\xb3k\\dr\xc3\xb3n";
    out.merge.mode = MergeMode::WeightedAverage;
    out.merge.deghost = DeghostLevel::High;
    out.tone.toneMap = ToneMapOp::AgX;
    REQUIRE(out.saveTo(file));
    CHECK_FALSE(fs::exists(fs::path(file) += ".tmp"));
    AppSettings back = AppSettings::loadFrom(file);
    CHECK(back.lastInputFolder == out.lastInputFolder);
    CHECK(back.merge.mode == MergeMode::WeightedAverage);
    CHECK(back.merge.deghost == DeghostLevel::High);
    CHECK(back.tone.toneMap == ToneMapOp::AgX);
}

TEST_CASE("BatchProcessor runs, reports and cancels jobs", "[batch]") {
    TempDir dir("test_batch");
    const fs::path a = writeFrame(dir.path, "A.dng", 0.001, 0.25f);
    const fs::path b = writeFrame(dir.path, "B.dng", 0.004, 1.0f);

    BracketGroup good;
    good.name = "good";
    good.files = {entryFor(a), entryFor(b)};
    BracketGroup bad;
    bad.name = "bad";
    FileEntry missing;
    missing.path = dir.path / "missing.dng";
    bad.files = {missing};

    MergeSettings settings;
    settings.align = AlignMode::None;

    {
        BatchProcessor processor(2);
        auto j1 = processor.addJob(good, dir.path / "good.dng", settings);
        auto j2 = processor.addJob(bad, dir.path / "bad.dng", settings);
        processor.start();
        processor.waitForIdle();
        CHECK(j1->status.load() == JobStatus::Done);
        CHECK(j2->status.load() == JobStatus::Failed);
        CHECK_FALSE(j2->errorMessage().empty());
        CHECK_FALSE(processor.isRunning());

        // Jobs added after the first run are picked up by the same workers
        settings.skipExisting = true;
        auto j3 = processor.addJob(good, dir.path / "good.dng", settings);
        processor.start();
        processor.waitForIdle();
        CHECK(j3->status.load() == JobStatus::Skipped);

        processor.clearFinished();
        CHECK(processor.getJobs().empty());
    }

    {
        BatchProcessor processor(1);
        auto j = processor.addJob(good, dir.path / "never.dng", settings);
        processor.cancelAll();
        processor.start();
        processor.waitForIdle();
        CHECK(j->status.load() == JobStatus::Cancelled);
        CHECK_FALSE(fs::exists(dir.path / "never.dng"));
    }
}
