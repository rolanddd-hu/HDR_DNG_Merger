find_package(OpenGL REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)
find_package(nfd CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_executable(hdrmerge_gui
    main.cpp
    TonePresets.h
    Theme.h
    Theme.cpp
    AppController.h
    AppController.cpp
    render/GlTexture.h
    render/GlTexture.cpp
    render/ToneShader.h
    render/ToneShader.cpp
    panels/Toolbar.h
    panels/Toolbar.cpp
    panels/BracketPanel.h
    panels/BracketPanel.cpp
    panels/PreviewPanel.h
    panels/PreviewPanel.cpp
    panels/AdjustPanel.h
    panels/AdjustPanel.cpp
    panels/MergeSettingsPanel.h
    panels/MergeSettingsPanel.cpp
    panels/BatchQueuePanel.h
    panels/BatchQueuePanel.cpp
    panels/LogPanel.h
    panels/LogPanel.cpp
    panels/StatusBar.h
    panels/StatusBar.cpp
)

target_link_libraries(hdrmerge_gui PRIVATE
    hdrcore
    OpenGL::GL
    glfw
    glad::glad
    imgui::imgui
    implot::implot
    nfd::nfd
    spdlog::spdlog
)

# Windows GUI subsystem (no console window) while keeping the standard main() entry point.
if(WIN32)
  set_target_properties(hdrmerge_gui PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MSVC)
    target_link_options(hdrmerge_gui PRIVATE /ENTRY:mainCRTStartup)
  endif()
endif()
hdr_configure_windows_app(hdrmerge_gui "HDR DNG Merger")
