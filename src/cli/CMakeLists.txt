find_package(CLI11 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_executable(hdrmerge_cli
    main.cpp
)

target_link_libraries(hdrmerge_cli PRIVATE
    hdrcore
    CLI11::CLI11
    spdlog::spdlog
)

hdr_configure_windows_app(hdrmerge_cli "HDR DNG Merger command line tool")
