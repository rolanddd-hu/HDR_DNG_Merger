find_package(libraw CONFIG REQUIRED)
find_package(TIFF REQUIRED)
find_package(ZLIB REQUIRED)
find_package(exiv2 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(hdrcore STATIC
    Types.h
    RawImage.h
    RawLoader.h
    RawLoader.cpp
    DngWriter.h
    DngWriter.cpp
    FileScanner.h
    FileScanner.cpp
    BracketGrouper.h
    BracketGrouper.cpp
    ExposureEstimator.h
    ExposureEstimator.cpp
    MergeEngine.h
    MergeEngine.cpp
    ImageAligner.h
    ImageAligner.cpp
    MergeJob.h
    MergeJob.cpp
    MetadataCopier.h
    MetadataCopier.cpp
    Settings.h
    Settings.cpp
    BatchProcessor.h
    BatchProcessor.cpp
    PreviewPipeline.h
    PreviewPipeline.cpp
    DemoGenerator.h
    DemoGenerator.cpp
    Log.h
    Log.cpp
    CfaUtil.h
    Parallel.h
    PathUtil.h
    TiffUtil.h
    TiffUtil.cpp
)

target_include_directories(hdrcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(hdrcore PUBLIC
    libraw::raw_r
    TIFF::TIFF
    ZLIB::ZLIB
    Exiv2::exiv2lib
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)
