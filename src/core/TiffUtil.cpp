#include "TiffUtil.h"
#include <spdlog/spdlog.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace hdr {

namespace {

// DNG tags libtiff 4.7 does not define. Unknown tags would be rejected by TIFFSetField and read
// back as anonymous fields with a 32-bit count, so register them for every TIFF handle.
TIFFExtendProc g_parentExtender = nullptr;

void extendDngTags(TIFF* tif) {
    static const TIFFFieldInfo fields[] = {
        { TIFFTAG_OPCODELIST1, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_UNDEFINED, FIELD_CUSTOM, 1, 1, const_cast<char*>("OpcodeList1") },
        { TIFFTAG_OPCODELIST2, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_UNDEFINED, FIELD_CUSTOM, 1, 1, const_cast<char*>("OpcodeList2") },
        { TIFFTAG_OPCODELIST3, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_UNDEFINED, FIELD_CUSTOM, 1, 1, const_cast<char*>("OpcodeList3") },
        { TIFFTAG_FORWARDMATRIX1, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SRATIONAL, FIELD_CUSTOM, 1, 1, const_cast<char*>("ForwardMatrix1") },
        { TIFFTAG_FORWARDMATRIX2, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SRATIONAL, FIELD_CUSTOM, 1, 1, const_cast<char*>("ForwardMatrix2") },
    };
    // Only add what this libtiff build does not already know.
    for (const auto& f : fields) {
        if (!TIFFFindField(tif, f.field_tag, TIFF_ANY)) {
            TIFFMergeFieldInfo(tif, &f, 1);
        }
    }
    if (g_parentExtender) g_parentExtender(tif);
}

void installTagExtender() {
    static std::once_flag once;
    std::call_once(once, [] { g_parentExtender = TIFFSetTagExtender(extendDngTags); });
}

std::string formatMessage(const char* module, const char* fmt, va_list ap) {
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    return module && *module ? std::string(module) + ": " + buf : std::string(buf);
}

int onTiffError(TIFF*, void* userData, const char* module, const char* fmt, va_list ap) {
    spdlog::error("libtiff: {}", formatMessage(module, fmt, ap));
    if (userData) static_cast<TiffErrorState*>(userData)->errors++;
    return 1;  // handled; do not fall through to the global stderr handler
}

int onTiffWarning(TIFF*, void*, const char* module, const char* fmt, va_list ap) {
    spdlog::debug("libtiff: {}", formatMessage(module, fmt, ap));
    return 1;
}

}  // namespace

TIFF* openTiff(const std::filesystem::path& path, const char* mode, TiffErrorState* state) {
    installTagExtender();
    TIFFOpenOptions* opts = TIFFOpenOptionsAlloc();
    TIFFOpenOptionsSetErrorHandlerExtR(opts, onTiffError, state);
    TIFFOpenOptionsSetWarningHandlerExtR(opts, onTiffWarning, nullptr);
#if defined(_WIN32)
    TIFF* tif = TIFFOpenWExt(path.wstring().c_str(), mode, opts);
#else
    TIFF* tif = TIFFOpenExt(path.string().c_str(), mode, opts);
#endif
    TIFFOpenOptionsFree(opts);
    return tif;
}

bool getTiffFloat(TIFF* tif, uint32_t tag, double& out) {
    const TIFFField* field = TIFFFieldWithTag(tif, tag);
    if (!field) return false;
    switch (TIFFFieldSetGetSize(field)) {
        case 8: {
            double d = 0.0;
            if (!TIFFGetField(tif, tag, &d)) return false;
            out = d;
            return true;
        }
        case 4: {
            float f = 0.f;
            if (!TIFFGetField(tif, tag, &f)) return false;
            out = f;
            return true;
        }
        default:
            return false;
    }
}

}  // namespace hdr
