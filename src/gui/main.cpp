#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>
#include <nfd.h>
#include <spdlog/spdlog.h>

#include "../core/Log.h"
#include "../core/FileScanner.h"
#include "../core/MetadataCopier.h"
#include "../core/PathUtil.h"
#include "Theme.h"
#include "AppController.h"
#include "panels/Toolbar.h"
#include "panels/BracketPanel.h"
#include "panels/PreviewPanel.h"
#include "panels/AdjustPanel.h"
#include "panels/MergeSettingsPanel.h"
#include "panels/BatchQueuePanel.h"
#include "panels/LogPanel.h"
#include "panels/StatusBar.h"

#include <iostream>

#if defined(_WIN32)
#include <windows.h>
#endif
#include <filesystem>

namespace fs = std::filesystem;

// The GUI is a windows-subsystem app without a console, so fatal startup errors need a dialog.
static void showFatalError(const char* message) {
    std::cerr << message << "\n";
#if defined(_WIN32)
    MessageBoxA(nullptr, message, "HDR DNG Merger", MB_OK | MB_ICONERROR);
#endif
}

static void glfwErrorCallback(int error, const char* description) {
    spdlog::error("GLFW Error {}: {}", error, description);
}

int main(int argc, char** argv) {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        showFatalError("Failed to initialize GLFW");
        return 1;
    }

    // OpenGL 3.3 Core profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    auto initialSettings = hdr::AppSettings::load();
    int initW = std::clamp(initialSettings.windowWidth, 800, 7680);
    int initH = std::clamp(initialSettings.windowHeight, 600, 4320);

    GLFWwindow* window = glfwCreateWindow(initW, initH, "HDR DNG Merger v" HDR_VERSION_STRING, nullptr, nullptr);
    if (!window) {
        showFatalError("Failed to create the window (OpenGL 3.3 is required)");
        glfwTerminate();
        return 1;
    }

    if (initialSettings.windowMaximized) {
        glfwMaximizeWindow(window);
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable VSync

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        showFatalError("Failed to load OpenGL functions");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    NFD_Init();
    hdr::Logger::init(hdr::AppSettings::getSettingsDirectory() / "logs");
    hdr::MetadataCopier::initialize();
    spdlog::info("HDR DNG Merger GUI started");

    // Initialize Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDebugHighlightIdConflicts = false;

    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);

    hdr::gui::Theme::applyDarkTheme(xscale);
    hdr::gui::Theme::setupFonts(xscale);

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // The controller owns GL objects, so it must be destroyed while the context still exists.
    {
    hdr::gui::AppController controller;
    if (!controller.initRenderResources()) {
        spdlog::error("Failed to initialize OpenGL render resources / tone shader");
    }

    glfwSetWindowUserPointer(window, &controller);

    glfwSetWindowSizeCallback(window, [](GLFWwindow* win, int width, int height) {
        auto* ctrl = static_cast<hdr::gui::AppController*>(glfwGetWindowUserPointer(win));
        if (ctrl && !glfwGetWindowAttrib(win, GLFW_MAXIMIZED)) {
            if (width >= 800 && height >= 600) {
                ctrl->settings().windowWidth = width;
                ctrl->settings().windowHeight = height;
            }
        }
    });

    glfwSetWindowMaximizeCallback(window, [](GLFWwindow* win, int maximized) {
        auto* ctrl = static_cast<hdr::gui::AppController*>(glfwGetWindowUserPointer(win));
        if (ctrl) {
            ctrl->settings().windowMaximized = (maximized != 0);
        }
    });

    glfwSetDropCallback(window, [](GLFWwindow* win, int count, const char** paths) {
        auto* ctrl = static_cast<hdr::gui::AppController*>(glfwGetWindowUserPointer(win));
        if (!ctrl || count <= 0) return;

        // GLFW hands out UTF-8 paths
        std::vector<fs::path> files;
        for (int i = 0; i < count; ++i) {
            const fs::path p = hdr::pathFromUtf8(paths[i]);
            std::error_code ec;
            if (fs::is_directory(p, ec)) {
                ctrl->addFolder(p);
            } else if (hdr::FileScanner::isSupportedExtension(p)) {
                files.push_back(p);
            }
        }
        if (!files.empty()) {
            ctrl->addFiles(files);
        }
    });

    // Check command line arguments for initial folder, or reopen last opened folder
    std::error_code ec;
    if (argc > 1) {
        // With the UTF-8 active code page from the manifest, argv is UTF-8.
        const fs::path argPath = hdr::pathFromUtf8(argv[1]);
        if (fs::exists(argPath, ec)) {
            if (fs::is_directory(argPath, ec)) {
                controller.addFolder(argPath);
            } else {
                controller.addFiles({argPath});
            }
        }
    } else if (!controller.settings().lastInputFolder.empty()) {
        const fs::path lastDir = hdr::pathFromUtf8(controller.settings().lastInputFolder);
        if (fs::is_directory(lastDir, ec)) {
            controller.addFolder(lastDir);
        }
    }

    bool dockspaceConfigured = false;

    // Main Loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        controller.update();

        if (controller.requestExit) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        // 1. Render Top Menu Bar and Fixed Toolbar
        hdr::gui::Toolbar::render(controller);

        // 2. Dockspace setup: strictly fill space between Toolbar and StatusBar
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        float toolbarHeight = hdr::gui::Toolbar::kHeight;
        float statusBarHeight = 28.0f;

        if (controller.resetLayoutRequested) {
            controller.resetLayoutRequested = false;
            dockspaceConfigured = false;
        }

        ImVec2 dockPos(viewport->WorkPos.x, viewport->WorkPos.y + toolbarHeight);
        ImVec2 dockSize(viewport->WorkSize.x, std::max(100.0f, viewport->WorkSize.y - toolbarHeight - statusBarHeight));

        ImGui::SetNextWindowPos(dockPos);
        ImGui::SetNextWindowSize(dockSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags hostWindowFlags = ImGuiWindowFlags_NoDocking |
                                          ImGuiWindowFlags_NoTitleBar |
                                          ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus |
                                          ImGuiWindowFlags_NoNavFocus |
                                          ImGuiWindowFlags_NoBackground;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("MainDockSpaceHost", nullptr, hostWindowFlags);
        ImGui::PopStyleVar(3);

        ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
        ImGuiDockNodeFlags dockspaceFlags =
            ImGuiDockNodeFlags_NoDockingSplit |
            ImGuiDockNodeFlags_NoUndocking |
            ImGuiDockNodeFlags_NoWindowMenuButton |
            ImGuiDockNodeFlags_NoCloseButton;

        ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), dockspaceFlags);

        ImGuiDockNode* rootNode = ImGui::DockBuilderGetNode(dockspaceId);
        if (!dockspaceConfigured || rootNode == nullptr || rootNode->IsEmpty()) {
            dockspaceConfigured = true;
            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace | dockspaceFlags);
            ImGui::DockBuilderSetNodeSize(dockspaceId, dockSize);

            ImGuiID dockMain = dockspaceId;
            ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.26f, nullptr, &dockMain);
            ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.22f, nullptr, &dockMain);
            ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.26f, nullptr, &dockMain);
            ImGuiID dockRightTop, dockRightBottom;
            ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Up, 0.44f, &dockRightTop, &dockRightBottom);
            ImGuiID dockCenter = dockMain;

            ImGui::DockBuilderDockWindow("Brackets", dockLeft);
            ImGui::DockBuilderDockWindow("Preview", dockCenter);
            ImGui::DockBuilderDockWindow("Merge Settings", dockRightTop);
            ImGui::DockBuilderDockWindow("Adjustments", dockRightBottom);
            ImGui::DockBuilderDockWindow("Batch Queue", dockBottom);
            ImGui::DockBuilderDockWindow("Log", dockBottom);

            ImGui::DockBuilderFinish(dockspaceId);
        }

        ImGui::End(); // MainDockSpaceHost

        // 3. Render Permanent UI Panels
        hdr::gui::BracketPanel::render(controller);
        hdr::gui::PreviewPanel::render(controller);
        hdr::gui::AdjustPanel::render(controller);
        hdr::gui::MergeSettingsPanel::render(controller);
        hdr::gui::BatchQueuePanel::render(controller);
        hdr::gui::LogPanel::render();
        hdr::gui::StatusBar::render(controller);

        // Rendering
        ImGui::Render();
        int displayW = 0, displayH = 0;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.106f, 0.114f, 0.133f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Save final window dimensions and state
    int isMax = glfwGetWindowAttrib(window, GLFW_MAXIMIZED);
    controller.settings().windowMaximized = (isMax != 0);
    if (!isMax) {
        int curW = 0, curH = 0;
        glfwGetWindowSize(window, &curW, &curH);
        if (curW >= 800 && curH >= 600) {
            controller.settings().windowWidth = curW;
            controller.settings().windowHeight = curH;
        }
    }
    controller.settings().save();

    glfwSetWindowUserPointer(window, nullptr);
    controller.shutdownRenderResources();
    } // AppController destroyed here (joins its worker threads)

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    NFD_Quit();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
