#pragma once
#include <tiffio.h>
#include <filesystem>
#include <cstdint>

namespace hdr {

// Counts libtiff errors reported for one TIFF handle (libtiff's own write paths do not always
// propagate failures, e.g. while flushing in TIFFClose).
struct TiffErrorState {
    int errors = 0;
};

// Opens a TIFF with Unicode path support. libtiff errors/warnings go to spdlog instead of stderr
// and, when `state` is given, are counted there. The state must outlive the handle.
TIFF* openTiff(const std::filesystem::path& path, const char* mode, TiffErrorState* state = nullptr);

// Reads a scalar floating-point tag whether libtiff stores it as float or double internally.
bool getTiffFloat(TIFF* tif, uint32_t tag, double& out);

}  // namespace hdr
