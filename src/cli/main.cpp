#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <map>
#include <thread>
#include <vector>
#include <string>
#include <filesystem>
#include <functional>
#include <CLI/CLI.hpp>
#include <tiffio.h>
#include "core/Types.h"
#include "core/Log.h"
#include "core/RawLoader.h"
#include "core/FileScanner.h"
#include "core/BracketGrouper.h"
#include "core/BatchProcessor.h"
#include "core/MergeJob.h"
#include "core/MetadataCopier.h"
#include "core/DemoGenerator.h"
#include "core/PathUtil.h"
#include "core/TiffUtil.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

// Exit codes
constexpr int kExitOk = 0;
constexpr int kExitError = 1;        // bad arguments / nothing to do
constexpr int kExitMergeFailed = 2;  // at least one bracket failed
constexpr int kExitCancelled = 130;  // Ctrl+C

std::atomic<bool> g_cancelRequested{false};

#if defined(_WIN32)
BOOL WINAPI onConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_cancelRequested = true;
        return TRUE;
    }
    return FALSE;
}
#endif

int printInfo(const fs::path& infoPath) {
    std::cout << "Loading file: " << hdr::toUtf8(infoPath) << "\n";
    auto [status, img] = hdr::RawLoader::loadFull(infoPath);
    if (!status) {
        std::cerr << "Error: " << status.message << "\n";
        return kExitError;
    }

    std::cout << "========================================\n";
    std::cout << "RAW / DNG INFO: " << img.originalFileName << "\n";
    std::cout << "========================================\n";
    std::cout << "Camera:         " << img.exposure.make << " " << img.exposure.model << "\n";
    std::cout << "Visible Size:   " << img.width << " x " << img.height << "\n";
    std::cout << "Shutter:        " << img.exposure.exposureTime << " s\n";
    std::cout << "Aperture:       f/" << img.exposure.fNumber << "\n";
    std::cout << "ISO:            " << img.exposure.iso << "\n";
    std::cout << "Rel. Exposure:  " << img.exposure.relativeExposure() << "\n";
    std::cout << "Date/Time:      " << img.exposure.dateTime << "\n";
    std::cout << "CFA Pattern:    " << img.cfa.name() << "\n";
    std::cout << "Black Levels:   [" << img.blackLevel[0] << ", " << img.blackLevel[1]
              << ", " << img.blackLevel[2] << ", " << img.blackLevel[3] << "]\n";
    std::cout << "White Level:    " << img.whiteLevel << "\n";
    std::cout << "Baseline Exp:   " << img.color.baselineExposure << " EV\n";
    std::cout << "Active Area:    [" << img.activeArea[0] << ", " << img.activeArea[1]
              << ", " << img.activeArea[2] << ", " << img.activeArea[3] << "]\n";
    std::cout << "Default Crop:   origin=[" << img.defaultCropOrigin[0] << ", " << img.defaultCropOrigin[1]
              << "] size=[" << img.defaultCropSize[0] << ", " << img.defaultCropSize[1] << "]\n";
    std::cout << "OpcodeList1:    " << img.opcodeList1.size() << " bytes\n";
    std::cout << "OpcodeList2:    " << img.opcodeList2.size() << " bytes\n";
    std::cout << "OpcodeList3:    " << img.opcodeList3.size() << " bytes\n";
    if (!img.asShotNeutral.empty()) {
        std::cout << "AsShotNeutral:  [";
        for (size_t i = 0; i < img.asShotNeutral.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << img.asShotNeutral[i];
        }
        std::cout << "]\n";
    }
    std::cout << "WB Multipliers: [" << img.color.camMul[0] << ", " << img.color.camMul[1]
              << ", " << img.color.camMul[2] << "]\n";
    std::cout << "========================================\n";
    return kExitOk;
}

int dumpTiff(const fs::path& dumpPath) {
    std::cout << "Dumping TIFF directories for: " << hdr::toUtf8(dumpPath) << "\n";
    TIFF* tif = hdr::openTiff(dumpPath, "r");
    if (!tif) {
        std::cerr << "Failed to open TIFF file\n";
        return kExitError;
    }

    std::function<void(int, bool)> printDir;
    printDir = [&](int dIndex, bool isSub) {
        uint32_t w = 0, h = 0, subfile = 0;
        uint16_t comp = 0, photo = 0, bps = 0, spp = 0, sampleFormat = 0;
        TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
        TIFFGetField(tif, TIFFTAG_SUBFILETYPE, &subfile);
        TIFFGetField(tif, TIFFTAG_COMPRESSION, &comp);
        TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photo);
        TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bps);
        TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
        TIFFGetField(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat);

        uint8_t* dngVer = nullptr;
        TIFFGetField(tif, TIFFTAG_DNGVERSION, &dngVer);

        std::cout << (isSub ? "  [SubIFD " : "[IFD ") << dIndex << "] "
                  << w << "x" << h << ", subfile=" << subfile << ", comp=" << comp
                  << ", photo=" << photo << ", bps=" << bps << ", spp=" << spp
                  << ", format=" << sampleFormat;
        if (dngVer) {
            std::cout << ", dngVer=" << (int)dngVer[0] << "." << (int)dngVer[1] << "." << (int)dngVer[2] << "." << (int)dngVer[3];
        }
        std::cout << "\n";

        uint16_t subIfdCount = 0;
        uint64_t* subIfdOffsets = nullptr;
        if (!isSub && TIFFGetField(tif, TIFFTAG_SUBIFD, &subIfdCount, &subIfdOffsets) && subIfdOffsets) {
            std::cout << "   SubIFDs found: " << subIfdCount << "\n";
            // Copy: the array belongs to the current directory, which TIFFSetSubDirectory replaces.
            const std::vector<uint64_t> offsets(subIfdOffsets, subIfdOffsets + subIfdCount);
            const tdir_t parent = TIFFCurrentDirectory(tif);
            for (uint16_t si = 0; si < offsets.size(); ++si) {
                if (TIFFSetSubDirectory(tif, offsets[si])) {
                    printDir(si, true);
                }
            }
            TIFFSetDirectory(tif, parent);
        }
    };

    int dIndex = 0;
    do {
        printDir(dIndex++, false);
    } while (TIFFReadDirectory(tif));

    TIFFClose(tif);
    return kExitOk;
}

int runDemo(const std::vector<std::string>& inputPaths, std::string demoDir) {
    if (demoDir.empty()) demoDir = "demo_extreme_hdr";
    fs::path templatePath;
    std::error_code ec;
    if (!inputPaths.empty() && fs::is_regular_file(hdr::pathFromUtf8(inputPaths[0]), ec)) {
        templatePath = hdr::pathFromUtf8(inputPaths[0]);
    } else {
        // Check current directory for any RAW/DNG file
        for (fs::directory_iterator it(".", ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fileEc;
            if (it->is_regular_file(fileEc) && hdr::FileScanner::isSupportedExtension(it->path())) {
                templatePath = it->path();
                break;
            }
        }
    }
    if (templatePath.empty()) {
        std::cerr << "Demo generation requires an existing template RAW/DNG file. Specify a RAW file path or place one in the current directory.\n";
        return kExitError;
    }
    std::cout << "Generating extreme 6-stop HDR demo bracket using template: " << hdr::toUtf8(templatePath) << "\n";
    auto progressCb = [](float p, const std::string& desc) {
        std::cout << "  [" << static_cast<int>(p * 100) << "%] " << desc << "\n";
    };
    auto res = hdr::DemoGenerator::generateBracket(templatePath, hdr::pathFromUtf8(demoDir), progressCb);
    if (!res.status) {
        std::cerr << "Demo generation failed: " << res.status.message << "\n";
        return kExitMergeFailed;
    }
    std::cout << "\n=======================================================\n";
    std::cout << "SUCCESS! Demo files created in: " << demoDir << "\n";
    std::cout << "  1. Dark (-3 EV, 1/4000s): " << res.darkPath.filename().string() << "\n";
    std::cout << "  2. Mid  ( 0 EV, 1/500s ): " << res.midPath.filename().string() << " (Window is 100% BLOWN OUT)\n";
    std::cout << "  3. Bright (+3 EV, 1/60s): " << res.brightPath.filename().string() << " (Shadows boosted)\n";
    std::cout << "  4. Merged HDR DNG:        " << res.hdrPath.filename().string() << "\n";
    std::cout << "=======================================================\n";
    std::cout << "HOW TO VERIFY IN CAPTURE ONE 20:\n";
    std::cout << "  - Open " << res.midPath.filename().string() << " in Capture One.\n";
    std::cout << "    Drag 'Highlights' to -100: Notice the window stays FLAT DULL GRAY (data is clipped).\n";
    std::cout << "  - Now open " << res.hdrPath.filename().string() << " in Capture One.\n";
    std::cout << "    Drag 'Highlights' to -100: BOOM! The complete sunset gradient, test bars, and sun disc appear!\n";
    std::cout << "=======================================================\n";
    return kExitOk;
}

const char* statusLabel(hdr::JobStatus s) {
    switch (s) {
        case hdr::JobStatus::Done: return "OK";
        case hdr::JobStatus::Failed: return "FAILED";
        case hdr::JobStatus::Cancelled: return "CANCELLED";
        case hdr::JobStatus::Skipped: return "SKIPPED (exists)";
        default: return "";
    }
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // The manifest makes argv and narrow paths UTF-8; make the console print UTF-8 as well.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
#endif
    hdr::Logger::init();
    hdr::MetadataCopier::initialize();

    CLI::App app{std::string("HDR DNG Merger CLI v") + HDR_VERSION_STRING};
    app.set_version_flag("-v,--version", std::string("HDR DNG Merger CLI v") + HDR_VERSION_STRING);

    std::string infoPath;
    std::string dumpPath;
    std::vector<std::string> inputPaths;
    int framesPerBracket = 3;
    std::string outDirPath;
    int bitsVal = 0;
    bool skipExisting = false;
    std::string namePattern = "{first}_HDR";
    std::string demoDir;
    int deghostVal = 0;
    int jobs = 0;
    bool noAlign = false;
    bool weighted = false;

    app.add_option("--info", infoPath, "Dump metadata for a raw DNG file");
    app.add_option("--dump", dumpPath, "Dump TIFF directory tags for any DNG file");
    app.add_option("-i,--input", inputPaths, "Input file(s) or directories to scan");
    app.add_option("-n,--frames", framesPerBracket, "Frames per bracket (default: 3)")->check(CLI::Range(2, 9));
    app.add_option("-o,--output", outDirPath, "Output directory for merged DNG files (default: HDR_Output)");
    app.add_option("--bits", bitsVal, "Output: 0 (auto = 16-bit int), 1 (16-bit int, Capture One), 16/24/32 (float)")
        ->check(CLI::IsMember({0, 1, 16, 24, 32}));
    app.add_flag("--skip-existing", skipExisting, "Skip if output file already exists");
    app.add_option("--pattern", namePattern, "Output naming pattern tokens: {first}, {last}, {ref}, {index}, {n}, {iso}, {shutter}, {fnumber}, {camera}, {date}");
    app.add_option("--deghost", deghostVal, "Deghosting sensitivity: 0 (Off), 1 (Low), 2 (Medium), 3 (High)")->check(CLI::Range(0, 3));
    app.add_flag("--weighted", weighted, "Weighted-average merge instead of best-exposure selection");
    app.add_flag("--no-align", noAlign, "Disable Ward MTB translation alignment");
    app.add_option("-j,--jobs", jobs, "Brackets merged in parallel (0 = auto; memory use is capped automatically)")->check(CLI::Range(0, 64));
    app.add_option("--demo", demoDir, "Generate an extreme 6-stop HDR demo bracket (and merged HDR) to test in Capture One");

    CLI11_PARSE(app, argc, argv);

    try {
        if (!demoDir.empty() || app.count("--demo")) {
            return runDemo(inputPaths, demoDir);
        }
        if (!infoPath.empty()) {
            return printInfo(hdr::pathFromUtf8(infoPath));
        }
        if (!dumpPath.empty()) {
            return dumpTiff(hdr::pathFromUtf8(dumpPath));
        }
        if (inputPaths.empty()) {
            std::cout << "HDR DNG Merger CLI. Use --help for options.\n";
            return kExitOk;
        }

        std::vector<hdr::FileEntry> allFiles;
        for (const auto& in : inputPaths) {
            auto scanned = hdr::FileScanner::scanPath(hdr::pathFromUtf8(in));
            allFiles.insert(allFiles.end(), scanned.begin(), scanned.end());
        }
        hdr::FileScanner::sortEntries(allFiles);

        std::cout << "Found " << allFiles.size() << " raw file(s).\n";
        if (allFiles.empty()) {
            std::cerr << "No supported raw files found.\n";
            return kExitError;
        }

        auto grouping = hdr::BracketGrouper::groupFixedN(allFiles, framesPerBracket);
        std::cout << "Grouped into " << grouping.groups.size() << " bracket group(s).\n";
        if (!grouping.warning.empty()) {
            std::cout << "Warning: " << grouping.warning << "\n";
        }
        if (grouping.groups.empty()) {
            return kExitError;
        }

        const fs::path outDir = hdr::pathFromUtf8(outDirPath.empty() ? std::string("HDR_Output") : outDirPath);
        std::error_code ec;
        fs::create_directories(outDir, ec);
        if (ec) {
            std::cerr << "Cannot create output directory " << hdr::toUtf8(outDir) << ": " << ec.message() << "\n";
            return kExitError;
        }

        hdr::MergeSettings settings;
        if (bitsVal == 1) settings.bits = hdr::FloatBits::Int16;
        else if (bitsVal == 16) settings.bits = hdr::FloatBits::F16;
        else if (bitsVal == 24) settings.bits = hdr::FloatBits::F24;
        else if (bitsVal == 32) settings.bits = hdr::FloatBits::F32;
        else settings.bits = hdr::FloatBits::Auto;
        settings.skipExisting = skipExisting;
        settings.namePattern = namePattern;
        settings.writeLightroomSettings = false;
        settings.align = noAlign ? hdr::AlignMode::None : hdr::AlignMode::Translation;
        settings.mode = weighted ? hdr::MergeMode::WeightedAverage : hdr::MergeMode::BestExposure;
        settings.deghost = static_cast<hdr::DeghostLevel>(deghostVal);

        hdr::BatchProcessor processor(static_cast<size_t>(jobs));
        const auto outPaths = hdr::MergeJob::planOutputPaths(grouping.groups, namePattern, outDir);
        for (size_t g = 0; g < grouping.groups.size(); ++g) {
            processor.addJob(grouping.groups[g], outPaths[g], settings);
        }
        processor.start();

        // Report each job once it finishes; Ctrl+C cancels everything (no partial files remain).
        const auto jobList = processor.getJobs();
        std::map<int, bool> reported;
        bool cancelSent = false;
        while (true) {
            if (g_cancelRequested && !cancelSent) {
                std::cout << "\nCancelling...\n";
                processor.cancelAll();
                cancelSent = true;
            }
            for (const auto& job : jobList) {
                const auto s = job->status.load();
                if (hdr::isFinished(s) && !reported[job->id]) {
                    reported[job->id] = true;
                    std::cout << "[" << reported.size() << "/" << jobList.size() << "] " << job->group.name << " -> "
                              << hdr::toUtf8(job->outputPath.filename()) << ": " << statusLabel(s);
                    if (s == hdr::JobStatus::Failed) std::cout << " - " << job->errorMessage();
                    if (s == hdr::JobStatus::Done) std::cout << " (" << std::fixed << std::setprecision(1) << job->elapsedSeconds.load() << " s)";
                    std::cout << "\n";
                }
            }
            if (!processor.isRunning()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        int failed = 0, cancelled = 0;
        for (const auto& job : jobList) {
            failed += job->status.load() == hdr::JobStatus::Failed;
            cancelled += job->status.load() == hdr::JobStatus::Cancelled;
        }
        std::cout << "\nBatch processing finished: " << (jobList.size() - failed - cancelled) << " ok, "
                  << failed << " failed, " << cancelled << " cancelled.\n";
        if (g_cancelRequested) return kExitCancelled;
        return failed > 0 ? kExitMergeFailed : kExitOk;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return kExitError;
    }
}
