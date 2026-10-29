find_package(Catch2 CONFIG REQUIRED)

add_executable(hdrcore_tests
    test_main.cpp
    test_dng_roundtrip.cpp
    test_grouper.cpp
    test_merge.cpp
    test_preview.cpp
    test_meta_dng.cpp
    test_align.cpp
    test_safety.cpp
    TestUtil.h
)

target_link_libraries(hdrcore_tests PRIVATE
    hdrcore
    Catch2::Catch2WithMain
)

# Same manifest as the applications (UTF-8 code page), so the Unicode path tests are meaningful.
hdr_configure_windows_app(hdrcore_tests "HDR DNG Merger unit tests")

include(CTest)
include(Catch)
catch_discover_tests(hdrcore_tests)
