#ifdef _WIN32
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#endif

#define GLM_ENABLE_EXPERIMENTAL

#include <iostream>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <imgui_impl_glfw.h>

#include "rhi/RHI.h"
#include "renderer/StickmanRenderer.h"
#include "renderer/ModelRenderer.h"
#include "renderer/ModelImporter.h"
#include "renderer/StudioEnvironment.h"
#include "renderer/LightingSystem.h"
#include "renderer/Camera.h"
#include "tracking/TrackingReceiver.h"
#include <ixwebsocket/IXNetSystem.h>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>

static std::atomic<float> g_audioLevel{0.f};
static std::atomic<bool> g_audioExplosionTriggered{false};
static std::atomic<bool> g_audioThreadRunning{true};

static void audioLoopbackThread() {
    CoInitialize(nullptr);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) return;

    while (g_audioThreadRunning) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
        if (FAILED(hr)) {
            device->Release();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        WAVEFORMATEX* pwfx = nullptr;
        audioClient->GetMixFormat(&pwfx);

        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, pwfx, nullptr);
        if (FAILED(hr)) {
            CoTaskMemFree(pwfx);
            audioClient->Release();
            device->Release();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
        if (FAILED(hr)) {
            CoTaskMemFree(pwfx);
            audioClient->Release();
            device->Release();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        hr = audioClient->Start();
        if (FAILED(hr)) {
            captureClient->Release();
            CoTaskMemFree(pwfx);
            audioClient->Release();
            device->Release();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        while (g_audioThreadRunning) {
            UINT32 packetLength = 0;
            hr = captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr) || packetLength == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            BYTE* pData = nullptr;
            UINT32 numFramesRead = 0;
            DWORD flags = 0;
            hr = captureClient->GetBuffer(&pData, &numFramesRead, &flags, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    float maxVal = 0.f;
                    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                        WAVEFORMATEXTENSIBLE* pEx = (WAVEFORMATEXTENSIBLE*)pwfx;
                        if (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                            float* fData = (float*)pData;
                            for (UINT32 i = 0; i < numFramesRead * pwfx->nChannels; ++i) {
                                float val = std::abs(fData[i]);
                                if (val > maxVal) maxVal = val;
                            }
                        } else if (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
                            int16_t* sData = (int16_t*)pData;
                            for (UINT32 i = 0; i < numFramesRead * pwfx->nChannels; ++i) {
                                float val = std::abs(sData[i]) / 32768.f;
                                if (val > maxVal) maxVal = val;
                            }
                        }
                    }
                    g_audioLevel.store(maxVal);

                    if (maxVal > 0.45f) {
                        g_audioExplosionTriggered.store(true);
                    }
                } else {
                    g_audioLevel.store(0.f);
                }
                captureClient->ReleaseBuffer(numFramesRead);
            } else {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        audioClient->Stop();
        captureClient->Release();
        CoTaskMemFree(pwfx);
        audioClient->Release();
        device->Release();
    }

    enumerator->Release();
    CoUninitialize();
}
#else
static std::atomic<bool> g_audioExplosionTriggered{false};
static std::atomic<bool> g_audioThreadRunning{false};
#endif

namespace fs = std::filesystem;

uint32_t g_width  = 1280;
uint32_t g_height = 720;

struct WindowCtx {
    vts::rhi::IRHIContext*   rhi    = nullptr;
    vts::renderer::Camera*   camera = nullptr;
};
static WindowCtx g_windowCtx;

// ─── GLFW callbacks ───────────────────────────────────────────────────────────
static void framebuffer_size_callback(GLFWwindow*, int w, int h) {
    g_width  = static_cast<uint32_t>(w);
    g_height = static_cast<uint32_t>(h);
    if (g_windowCtx.rhi) g_windowCtx.rhi->resize(w, h);
}

static void scroll_callback(GLFWwindow* window, double xoff, double yoff) {
    ImGui_ImplGlfw_ScrollCallback(window, xoff, yoff);
    if (ImGui::GetIO().WantCaptureMouse) return;
    if (g_windowCtx.camera) g_windowCtx.camera->onScroll(static_cast<float>(yoff));
}

// ─── Rising-edge key helper ───────────────────────────────────────────────────
struct KeyToggle {
    int  key;
    bool wasDown = false;
    bool fired(GLFWwindow* w) {
        bool down = glfwGetKey(w, key) == GLFW_PRESS;
        bool edge = down && !wasDown;
        wasDown   = down;
        return edge;
    }
};

#ifdef _WIN32
static void getScreenQuadrants(glm::vec3& tl, glm::vec3& tr, glm::vec3& bl, glm::vec3& br) {
    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        tl = tr = bl = br = glm::vec3(0.18f, 0.18f, 0.22f);
        return;
    }
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, 1, 1);
    HGDIOBJ hOld = SelectObject(hdcMem, hBitmap);
    SetStretchBltMode(hdcMem, COLORONCOLOR);
    
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    int hw = w / 2;
    int hh = h / 2;
    
    auto getAvg = [&](int x, int y, int sw, int sh) -> glm::vec3 {
        StretchBlt(hdcMem, 0, 0, 1, 1, hdcScreen, x, y, sw, sh, SRCCOPY);
        COLORREF p = GetPixel(hdcMem, 0, 0);
        return glm::vec3(GetRValue(p)/255.f, GetGValue(p)/255.f, GetBValue(p)/255.f);
    };
    
    tl = getAvg(0, 0, hw, hh);
    tr = getAvg(hw, 0, hw, hh);
    bl = getAvg(0, hh, hw, hh);
    br = getAvg(hw, hh, hw, hh);
    
    SelectObject(hdcMem, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}
#else
static void getScreenQuadrants(glm::vec3& tl, glm::vec3& tr, glm::vec3& bl, glm::vec3& br) {
    tl = tr = bl = br = glm::vec3(0.18f, 0.18f, 0.22f);
}
#endif

// ─── Win32 Python launcher ────────────────────────────────────────────────────
#ifdef _WIN32
static void killProcess(HANDLE& hProcess, DWORD& pid) {
    if (hProcess != NULL) {
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
        hProcess = NULL;
        spdlog::info("Terminated Python process (PID: {})", pid);
    } else if (pid != 0) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
    }
    pid = 0;
}

static bool startPythonTracker(const std::string& scriptPath, DWORD& outPid, HANDLE& outProcessHandle) {
    if (!fs::exists(scriptPath)) {
        spdlog::error("Python script not found: {}", scriptPath); return false;
    }
    fs::path scriptDir = fs::path(scriptPath).parent_path();
    
    std::string pythonExe = "python";
    fs::path venvPython = fs::path(scriptPath).parent_path().parent_path() / ".venv" / "Scripts" / "python.exe";
    if (fs::exists(venvPython)) {
        pythonExe = "\"" + venvPython.string() + "\"";
    } else {
        fs::path vtsVenvPython = fs::current_path() / ".venv" / "Scripts" / "python.exe";
        if (fs::exists(vtsVenvPython)) {
            pythonExe = "\"" + vtsVenvPython.string() + "\"";
        }
    }

    std::string cmd    = pythonExe + " \"" + scriptPath + "\" --parent-pid " + std::to_string(GetCurrentProcessId());

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hLog = CreateFileA("tracker_output.log", GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    STARTUPINFO si = { sizeof(si) };
    si.dwFlags     = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = hLog;
    si.hStdError   = hLog;
    PROCESS_INFORMATION pi;
    std::vector<char> cmdLine(cmd.begin(), cmd.end()); cmdLine.push_back('\0');

    if (CreateProcess(NULL, cmdLine.data(), NULL, NULL, TRUE,
                      CREATE_NO_WINDOW, NULL, scriptDir.string().c_str(), &si, &pi)) {
        spdlog::info("Python tracker started (PID: {})", pi.dwProcessId);
        outPid = pi.dwProcessId;
        outProcessHandle = pi.hProcess;
        CloseHandle(pi.hThread);
        CloseHandle(hLog);
        return true;
    }
    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
    spdlog::error("Failed to start Python tracker. Error: {}", GetLastError());
    return false;
}
#endif

// ─── Settings persistence ─────────────────────────────────────────────────────
static int   s_inferW      = 480, s_inferH = 270;
static float s_faceCutoff  = 1.0f, s_faceBeta = 0.30f;
static float s_handCutoff  = 1.5f, s_handBeta = 0.20f;
static float s_eulerCutoff = 2.0f, s_eulerBeta = 0.40f;
static float s_posVisThr   = 0.60f;
static int   s_cameraIndex = 0;
static std::string s_loadedModelPath = "";
static bool  s_useAvatar = false;
static bool  s_armTracking = true;
static bool  s_handTracking = true;
static bool  s_streamMode = false;
static bool  s_alwaysOnTop = false;
static bool  s_borderlessWindow = false;
static bool  s_screenReactiveLighting = false;
static bool  s_hoveringModel = false;
static bool  s_hoveringBone = false;
static bool  s_clickThrough = false;
static std::string s_trackerErrorMsg = "";
static bool  s_screenAffectsAmbient = true;
static bool  s_screenAffectsKey = true;
static bool  s_screenAffectsFill = true;
static float s_screenMinBrightness = 0.15f;
static float s_screenStrength = 1.0f;

static float s_avatarScale = 1.0f;

static int s_fpsLimit = 60;

static bool s_virtualViewport = false;
static vts::rhi::TextureHandle g_virtualColorTex = nullptr;
static vts::rhi::TextureHandle g_virtualDepthTex = nullptr;
static vts::rhi::FramebufferHandle g_virtualFB = nullptr;
static vts::rhi::RenderPassHandle g_virtualRP = nullptr;
static ImVec2 s_viewportPos{0.f, 0.f};
static ImVec2 s_viewportSize{1920.f, 1080.f};
static bool s_viewportHovered = false;

static glm::vec3 s_avatarTranslation{0.f};

static void loadTrackerSettings(vts::renderer::StudioEnvironment& studio, vts::renderer::LightingSystem& lighting) {
    std::ifstream f("tracker_settings.json");
    if (!f.is_open()) return;
    try {
        nlohmann::json j;
        f >> j;
        if (j.contains("infer_w")) s_inferW = j["infer_w"];
        if (j.contains("infer_h")) s_inferH = j["infer_h"];
        if (j.contains("face_min_cutoff")) s_faceCutoff = j["face_min_cutoff"];
        if (j.contains("face_beta")) s_faceBeta = j["face_beta"];
        if (j.contains("hand_min_cutoff")) s_handCutoff = j["hand_min_cutoff"];
        if (j.contains("hand_beta")) s_handBeta = j["hand_beta"];
        if (j.contains("euler_min_cutoff")) s_eulerCutoff = j["euler_min_cutoff"];
        if (j.contains("euler_beta")) s_eulerBeta = j["euler_beta"];
        if (j.contains("pose_vis_threshold")) s_posVisThr = j["pose_vis_threshold"];
        if (j.contains("camera_index")) s_cameraIndex = j["camera_index"];
        if (j.contains("loaded_model_path")) s_loadedModelPath = j["loaded_model_path"].get<std::string>();
        if (j.contains("use_avatar")) s_useAvatar = j["use_avatar"].get<bool>();
        if (j.contains("arm_tracking")) s_armTracking = j["arm_tracking"].get<bool>();
        if (j.contains("hand_tracking")) s_handTracking = j["hand_tracking"].get<bool>();
        if (j.contains("stream_mode")) s_streamMode = j["stream_mode"].get<bool>();
        if (j.contains("always_on_top")) s_alwaysOnTop = j["always_on_top"].get<bool>();
        if (j.contains("borderless_window")) s_borderlessWindow = j["borderless_window"].get<bool>();
        if (j.contains("screen_reactive_lighting")) s_screenReactiveLighting = j["screen_reactive_lighting"].get<bool>();
        if (j.contains("click_through")) s_clickThrough = j["click_through"].get<bool>();
        if (j.contains("screen_affects_ambient")) s_screenAffectsAmbient = j["screen_affects_ambient"].get<bool>();
        if (j.contains("screen_affects_key")) s_screenAffectsKey = j["screen_affects_key"].get<bool>();
        if (j.contains("screen_affects_fill")) s_screenAffectsFill = j["screen_affects_fill"].get<bool>();
        if (j.contains("screen_min_brightness")) s_screenMinBrightness = j["screen_min_brightness"].get<float>();
        if (j.contains("screen_strength")) s_screenStrength = j["screen_strength"].get<float>();
        if (j.contains("fps_limit")) s_fpsLimit = j["fps_limit"].get<int>();

        if (j.contains("virtual_viewport")) s_virtualViewport = j["virtual_viewport"].get<bool>();
        if (j.contains("avatar_scale")) s_avatarScale = j["avatar_scale"].get<float>();
        if (j.contains("avatar_translation") && j["avatar_translation"].is_array() && j["avatar_translation"].size() >= 3) {
            s_avatarTranslation = glm::vec3(
                j["avatar_translation"][0].get<float>(),
                j["avatar_translation"][1].get<float>(),
                j["avatar_translation"][2].get<float>()
            );
        }

        // Studio environment settings
        auto& sp = studio.params();
        if (j.contains("studio")) {
            const auto& js = j["studio"];
            if (js.contains("bg_mode")) sp.bgMode = (vts::renderer::BackgroundMode)js["bg_mode"].get<int>();
            if (js.contains("bg_color") && js["bg_color"].is_array() && js["bg_color"].size() >= 3)
                sp.bgColor = glm::vec4(js["bg_color"][0], js["bg_color"][1], js["bg_color"][2], js.size() >= 4 ? js["bg_color"][3].get<float>() : 1.f);
            if (js.contains("bg_color_top") && js["bg_color_top"].is_array() && js["bg_color_top"].size() >= 3)
                sp.bgColorTop = glm::vec4(js["bg_color_top"][0], js["bg_color_top"][1], js["bg_color_top"][2], js.size() >= 4 ? js["bg_color_top"][3].get<float>() : 1.f);
            if (js.contains("bg_color_bot") && js["bg_color_bot"].is_array() && js["bg_color_bot"].size() >= 3)
                sp.bgColorBot = glm::vec4(js["bg_color_bot"][0], js["bg_color_bot"][1], js["bg_color_bot"][2], js.size() >= 4 ? js["bg_color_bot"][3].get<float>() : 1.f);
            if (js.contains("show_grid")) sp.showGrid = js["show_grid"].get<bool>();
            if (js.contains("grid_spacing")) sp.gridSpacing = js["grid_spacing"].get<float>();
            if (js.contains("grid_extent")) sp.gridExtent = js["grid_extent"].get<float>();
            if (js.contains("grid_color") && js["grid_color"].is_array() && js["grid_color"].size() >= 3)
                sp.gridColor = glm::vec3(js["grid_color"][0], js["grid_color"][1], js["grid_color"][2]);
            if (js.contains("grid_alpha")) sp.gridAlpha = js["grid_alpha"].get<float>();
        }

        // Lighting settings
        auto& lp = lighting.params();
        if (j.contains("lighting")) {
            const auto& jl = j["lighting"];
            if (jl.contains("enable_shadows")) lp.enableShadows = jl["enable_shadows"].get<bool>();
            if (jl.contains("enable_ray_tracing")) lp.enableRayTracing = jl["enable_ray_tracing"].get<bool>();
            if (jl.contains("ambient_color") && jl["ambient_color"].is_array() && jl["ambient_color"].size() >= 3)
                lp.ambientColor = glm::vec3(jl["ambient_color"][0], jl["ambient_color"][1], jl["ambient_color"][2]);
            if (jl.contains("ambient_intensity")) lp.ambientIntensity = jl["ambient_intensity"].get<float>();

            auto loadLight = [](const nlohmann::json& jlDir, vts::renderer::DirectionalLight& L) {
                if (jlDir.contains("direction") && jlDir["direction"].is_array() && jlDir["direction"].size() >= 3)
                    L.direction = glm::vec3(jlDir["direction"][0], jlDir["direction"][1], jlDir["direction"][2]);
                if (jlDir.contains("color") && jlDir["color"].is_array() && jlDir["color"].size() >= 3)
                    L.color = glm::vec3(jlDir["color"][0], jlDir["color"][1], jlDir["color"][2]);
                if (jlDir.contains("intensity")) L.intensity = jlDir["intensity"].get<float>();
                if (jlDir.contains("enabled")) L.enabled = jlDir["enabled"].get<bool>();
                if (jlDir.contains("cast_shadows")) L.castShadows = jlDir["cast_shadows"].get<bool>();
            };

            if (jl.contains("key_light")) loadLight(jl["key_light"], lp.keyLight);
            if (jl.contains("fill_light")) loadLight(jl["fill_light"], lp.fillLight);
            if (jl.contains("rim_light")) loadLight(jl["rim_light"], lp.rimLight);
        }
        lighting.update();
        studio.rebuildGrid();
    } catch (const std::exception& e) {
        spdlog::error("Failed to parse tracker settings JSON: {}", e.what());
    }
}

static void saveTrackerSettings(const vts::renderer::StudioEnvironment& studio, const vts::renderer::LightingSystem& lighting, const vts::renderer::ModelRenderer& avatarRenderer) {
    nlohmann::json j;
    j["infer_w"] = s_inferW;
    j["infer_h"] = s_inferH;
    j["face_min_cutoff"] = s_faceCutoff;
    j["face_beta"] = s_faceBeta;
    j["hand_min_cutoff"] = s_handCutoff;
    j["hand_beta"] = s_handBeta;
    j["euler_min_cutoff"] = s_eulerCutoff;
    j["euler_beta"] = s_eulerBeta;
    j["pose_vis_threshold"] = s_posVisThr;
    j["camera_index"] = s_cameraIndex;
    j["loaded_model_path"] = s_loadedModelPath;
    j["use_avatar"] = s_useAvatar;
    j["arm_tracking"] = s_armTracking;
    j["hand_tracking"] = s_handTracking;
    j["stream_mode"] = s_streamMode;
    j["always_on_top"] = s_alwaysOnTop;
    j["borderless_window"] = s_borderlessWindow;
    j["screen_reactive_lighting"] = s_screenReactiveLighting;
    j["click_through"] = s_clickThrough;
    j["screen_affects_ambient"] = s_screenAffectsAmbient;
    j["screen_affects_key"] = s_screenAffectsKey;
    j["screen_affects_fill"] = s_screenAffectsFill;
    j["screen_min_brightness"] = s_screenMinBrightness;
    j["screen_strength"] = s_screenStrength;
    j["fps_limit"] = s_fpsLimit;

    // Studio environment settings
    const auto& sp = studio.params();
    nlohmann::json js;
    js["bg_mode"] = (int)sp.bgMode;
    js["bg_color"] = { sp.bgColor.x, sp.bgColor.y, sp.bgColor.z, sp.bgColor.w };
    js["bg_color_top"] = { sp.bgColorTop.x, sp.bgColorTop.y, sp.bgColorTop.z, sp.bgColorTop.w };
    js["bg_color_bot"] = { sp.bgColorBot.x, sp.bgColorBot.y, sp.bgColorBot.z, sp.bgColorBot.w };
    js["show_grid"] = sp.showGrid;
    js["grid_spacing"] = sp.gridSpacing;
    js["grid_extent"] = sp.gridExtent;
    js["grid_color"] = { sp.gridColor.x, sp.gridColor.y, sp.gridColor.z };
    js["grid_alpha"] = sp.gridAlpha;
    j["studio"] = js;

    // Lighting settings
    const auto& lp = lighting.params();
    nlohmann::json jl;
    jl["enable_shadows"] = lp.enableShadows;
    jl["enable_ray_tracing"] = lp.enableRayTracing;
    jl["ambient_color"] = { lp.ambientColor.x, lp.ambientColor.y, lp.ambientColor.z };
    jl["ambient_intensity"] = lp.ambientIntensity;

    auto saveLight = [](const vts::renderer::DirectionalLight& L) {
        nlohmann::json jlDir;
        jlDir["direction"] = { L.direction.x, L.direction.y, L.direction.z };
        jlDir["color"] = { L.color.x, L.color.y, L.color.z };
        jlDir["intensity"] = L.intensity;
        jlDir["enabled"] = L.enabled;
        jlDir["cast_shadows"] = L.castShadows;
        return jlDir;
    };

    jl["key_light"] = saveLight(lp.keyLight);
    jl["fill_light"] = saveLight(lp.fillLight);
    jl["rim_light"] = saveLight(lp.rimLight);
    j["lighting"] = jl;

    j["virtual_viewport"] = s_virtualViewport;
    j["avatar_scale"] = s_avatarScale;
    j["avatar_translation"] = { s_avatarTranslation.x, s_avatarTranslation.y, s_avatarTranslation.z };
    j["physics_enabled"] = avatarRenderer.isPhysicsEnabled();

    nlohmann::json jChains = nlohmann::json::array();
    for (const auto& chain : avatarRenderer.getPhysicsChains()) {
        nlohmann::json jc;
        jc["root_bone"] = chain.rootBoneName;
        jc["stiffness"] = chain.params.stiffness;
        jc["gravity"] = chain.params.gravity;
        jc["drag"] = chain.params.drag;
        jc["wind"] = chain.params.wind;
        jChains.push_back(jc);
    }
    j["physics_chains"] = jChains;

    std::ofstream f("tracker_settings.json");
    if (f.is_open()) {
        f << j.dump(2);
    }
}

static void applyPremiumTheme() {
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 8.f;
    style.ChildRounding = 6.f;
    style.FrameRounding = 6.f;
    style.GrabRounding = 6.f;
    style.PopupRounding = 6.f;
    style.ScrollbarRounding = 6.f;
    style.TabRounding = 6.f;
    
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                   = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.52f, 0.55f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.09f, 0.09f, 0.12f, 0.94f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.12f, 0.12f, 0.16f, 0.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.11f, 0.11f, 0.14f, 0.94f);
    colors[ImGuiCol_Border]                 = ImVec4(0.22f, 0.23f, 0.27f, 1.00f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.24f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.32f, 0.32f, 0.40f, 1.00f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.12f, 0.12f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.18f, 0.18f, 0.24f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.09f, 0.09f, 0.12f, 0.75f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.12f, 0.12f, 0.16f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.24f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]    = ImVec4(0.30f, 0.30f, 0.40f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]     = ImVec4(0.40f, 0.40f, 0.50f, 1.00f);
    colors[ImGuiCol_CheckMark]              = ImVec4(0.68f, 0.51f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.58f, 0.41f, 0.88f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.68f, 0.51f, 0.98f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.20f, 0.21f, 0.27f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.28f, 0.29f, 0.37f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.38f, 0.39f, 0.47f, 1.00f);
    colors[ImGuiCol_Header]                 = ImVec4(0.24f, 0.24f, 0.32f, 0.70f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.28f, 0.29f, 0.39f, 0.80f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.38f, 0.39f, 0.49f, 1.00f);
    colors[ImGuiCol_Separator]              = ImVec4(0.22f, 0.23f, 0.27f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.32f, 0.33f, 0.43f, 0.78f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.42f, 0.43f, 0.53f, 1.00f);
    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.24f, 0.24f, 0.32f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.32f, 0.33f, 0.43f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.42f, 0.43f, 0.53f, 0.95f);
    colors[ImGuiCol_Tab]                    = ImVec4(0.15f, 0.16f, 0.21f, 0.86f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.28f, 0.29f, 0.37f, 0.80f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.24f, 0.25f, 0.33f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.09f, 0.10f, 0.13f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.14f, 0.15f, 0.20f, 1.00f);
}

static void applyLoadedModelSettings(vts::renderer::ModelRenderer& avatarRenderer) {
    avatarRenderer.setModelScale(s_avatarScale);
    avatarRenderer.setModelTranslation(s_avatarTranslation);
    
    std::ifstream f("tracker_settings.json");
    if (f.is_open()) {
        try {
            nlohmann::json j;
            f >> j;
            if (j.contains("physics_enabled")) {
                avatarRenderer.setPhysicsEnabled(j["physics_enabled"].get<bool>());
            }
            if (j.contains("physics_chains")) {
                avatarRenderer.clearPhysicsChains();
                for (const auto& jc : j["physics_chains"]) {
                    if (jc.contains("root_bone")) {
                        std::string root = jc["root_bone"].get<std::string>();
                        vts::renderer::ModelRenderer::PhysicsParams params;
                        if (jc.contains("stiffness")) params.stiffness = jc["stiffness"].get<float>();
                        if (jc.contains("gravity")) params.gravity = jc["gravity"].get<float>();
                        if (jc.contains("drag")) params.drag = jc["drag"].get<float>();
                        if (jc.contains("wind")) params.wind = jc["wind"].get<float>();
                        avatarRenderer.addPhysicsChain(root, params);
                    }
                }
            }
        } catch (...) {}
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main() {
    // ── Pre-load settings for window hints ───────────────────────────
    {
        std::ifstream f("tracker_settings.json");
        if (f.is_open()) {
            try {
                nlohmann::json j;
                f >> j;
                if (j.contains("borderless_window")) s_borderlessWindow = j["borderless_window"].get<bool>();
                if (j.contains("always_on_top")) s_alwaysOnTop = j["always_on_top"].get<bool>();
                if (j.contains("loaded_model_path")) s_loadedModelPath = j["loaded_model_path"].get<std::string>();
                if (j.contains("use_avatar")) s_useAvatar = j["use_avatar"].get<bool>();
                if (j.contains("arm_tracking")) s_armTracking = j["arm_tracking"].get<bool>();
                if (j.contains("hand_tracking")) s_handTracking = j["hand_tracking"].get<bool>();
                if (j.contains("stream_mode")) s_streamMode = j["stream_mode"].get<bool>();
                if (j.contains("fps_limit")) s_fpsLimit = j["fps_limit"].get<int>();
            } catch (...) {}
        }
    }

#ifdef _WIN32
    std::thread audioThread(audioLoopbackThread);
    audioThread.detach();
#endif

    ix::initNetSystem();
    spdlog::set_level(spdlog::level::info);
    spdlog::info("Starting V-Open (Head/Upper-Body + Studio Environment)");

    DWORD pythonPid = 0;
    HANDLE pythonProcessHandle = NULL;

    // ── Find and launch Python tracker ────────────────────────────────────────
    fs::path pythonScript;
    for (const auto& p : {
        fs::current_path() / "Tracker_VideoProcessing" / "main.py",
        fs::path(__FILE__).parent_path().parent_path() / "Tracker_VideoProcessing" / "main.py",
        fs::path("C:/Users/Marti/PycharmProjects/Tracker/main.py"),
    }) {
        if (fs::exists(p)) { pythonScript = p; spdlog::info("Found Python script: {}", p.string()); break; }
    }

    if (!pythonScript.empty()) {
#ifdef _WIN32
        if (startPythonTracker(pythonScript.string(), pythonPid, pythonProcessHandle)) {
            spdlog::info("Waiting for tracker to initialise…");
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
#endif
    } else {
        spdlog::warn("Python script not found — start tracker manually.");
    }

    // ── GLFW / Window ─────────────────────────────────────────────────────────
    if (!glfwInit()) { spdlog::error("GLFW init failed"); return -1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    if (s_borderlessWindow) {
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    }
    if (s_alwaysOnTop) {
        glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    }

    GLFWwindow* window = glfwCreateWindow(g_width, g_height, "V-Open", nullptr, nullptr);
    if (!window) { spdlog::error("GLFW window creation failed"); glfwTerminate(); return -1; }

    glfwMaximizeWindow(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    auto rhi = vts::rhi::createDefaultRHIContext();
    if (!rhi) { spdlog::error("Failed to create RHI context"); glfwDestroyWindow(window); glfwTerminate(); return -1; }

    try {
        rhi->init(window, g_width, g_height);
        spdlog::info("RHI: {}", rhi->backendName());

        // ── Create virtual viewport resources ──────────────────────────────
        {
            vts::rhi::TextureDesc colDesc;
            colDesc.width = 1920;
            colDesc.height = 1080;
            colDesc.format = vts::rhi::PixelFormat::RGBA8_UNORM;
            colDesc.isRenderTarget = true;
            g_virtualColorTex = rhi->createTexture(colDesc);

            vts::rhi::TextureDesc depthDesc;
            depthDesc.width = 1920;
            depthDesc.height = 1080;
            depthDesc.format = vts::rhi::PixelFormat::Depth32F;
            depthDesc.isDepthStencil = true;
            g_virtualDepthTex = rhi->createTexture(depthDesc);

            g_virtualFB = rhi->createFramebuffer(g_virtualColorTex, g_virtualDepthTex);

            vts::rhi::RenderPassDesc rpDesc;
            rpDesc.colorFormat = vts::rhi::PixelFormat::RGBA8_UNORM;
            rpDesc.depthFormat = vts::rhi::PixelFormat::Depth32F;
            rpDesc.clearColor = true;
            rpDesc.clearDepth = true;
            rpDesc.clearColorVal = {0.0f, 0.0f, 0.0f, 0.0f}; // transparent clear
            g_virtualRP = rhi->createRenderPass(rpDesc);
        }

        vts::renderer::Camera camera(window);
        g_windowCtx = { rhi.get(), &camera };
        glfwSetWindowUserPointer(window, &g_windowCtx);
        glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
        glfwSetScrollCallback(window, scroll_callback);

        // ── Renderers ──────────────────────────────────────────────────────────
        vts::renderer::StickmanRenderer stickmanRenderer(*rhi);
        stickmanRenderer.init();

        vts::renderer::ModelRenderer avatarRenderer(*rhi);

        // Lighting system (abstracted from environment)
        vts::renderer::LightingSystem lighting(*rhi);
        lighting.init();

        // Studio environment (background, grid)
        vts::renderer::StudioEnvironment studio(*rhi, lighting);
        studio.init();

        // Load settings
        loadTrackerSettings(studio, lighting);
        applyPremiumTheme();

        // ── Try to auto-load a model ────────────────────────────────────────────
        bool loaded = false;
        if (!s_loadedModelPath.empty() && fs::exists(s_loadedModelPath)) {
            spdlog::info("Loading persisted model: {}", s_loadedModelPath);
            if (avatarRenderer.loadModel(s_loadedModelPath)) {
                s_useAvatar = true;
                loaded = true;
                applyLoadedModelSettings(avatarRenderer);
                spdlog::info("✓ Persisted model loaded: {}", avatarRenderer.getModelName());
            }
        }
        
        if (!loaded) {
            std::vector<std::string> modelPaths = {
                (fs::current_path() / "model.vrm").string(),
                (fs::current_path() / "avatar.glb").string(),
                (fs::current_path() / "model.gltf").string(),
                (fs::current_path() / "avatar.vrm").string(),
                "C:/Users/Marti/CLionProjects/VStudio/model.vrm",
            };
            for (const auto& path : modelPaths) {
                if (fs::exists(path)) {
                    spdlog::info("Auto-loading model: {}", path);
                    if (avatarRenderer.loadModel(path)) {
                        s_useAvatar = true;
                        s_loadedModelPath = path;
                        loaded = true;
                        applyLoadedModelSettings(avatarRenderer);
                        spdlog::info("✓ Model loaded: {}", avatarRenderer.getModelName());
                        break;
                    }
                }
            }
        }
        
        if (!s_useAvatar) {
            spdlog::warn("No model found — using stickman. Load via Settings > Avatar > Browse…");
        } else {
            avatarRenderer.setArmTrackingEnabled(s_armTracking);
            avatarRenderer.setHandTrackingEnabled(s_handTracking);
        }

        // ── Tracking ────────────────────────────────────────────────────────────
        vts::tracking::TrackingState    trackingState;
        std::string wsUrl = "ws://localhost:8765";
        if (!pythonScript.empty()) {
            fs::path portFilePath = pythonScript.parent_path() / "tracker_port.txt";
            std::ifstream portFile(portFilePath);
            if (portFile.is_open()) {
                int port = 8765;
                if (portFile >> port) {
                    wsUrl = "ws://localhost:" + std::to_string(port);
                    spdlog::info("Dynamic tracker port read: {}", port);
                }
            }
        }
        vts::tracking::TrackingReceiver receiver(trackingState, wsUrl);
        receiver.start();

        vts::tracking::TrackingFrame currentFrame;
        auto lastReconnectAttempt = std::chrono::steady_clock::now();
        auto lastFrameTime        = std::chrono::steady_clock::now();

        auto saveSettings = [&] { saveTrackerSettings(studio, lighting, avatarRenderer); };

        // ── Key toggles ─────────────────────────────────────────────────────────
        KeyToggle kTab   {GLFW_KEY_TAB};
        KeyToggle kSpace {GLFW_KEY_SPACE};
        KeyToggle kF     {GLFW_KEY_F};
        KeyToggle kB     {GLFW_KEY_B};
        KeyToggle kP     {GLFW_KEY_P};
        KeyToggle kM     {GLFW_KEY_M};
        KeyToggle kR     {GLFW_KEY_R};
        KeyToggle kV     {GLFW_KEY_V};
        KeyToggle kH     {GLFW_KEY_H};
        KeyToggle kEsc   {GLFW_KEY_ESCAPE};

        struct BoneOverrideState {
            glm::vec3 rotation{0.f};
            glm::vec3 translation{0.f};
            bool active = false;
        };
        static std::unordered_map<std::string, BoneOverrideState> s_boneOverrides;
        static std::string s_selectedBoneName = "";
        static std::string s_activeDragBone = "";
        static int s_activeDragAxis = -1; // -1 = none, 0 = X, 1 = Y, 2 = Z
        static bool s_isDraggingTranslation = false;
        static bool s_drawJointNodes = true;
        static bool s_drawSkeletonLines = false;
        static bool s_hoveringGizmo = false;
        // ── UI state ────────────────────────────────────────────────────────────
        static bool showSettings = true;
        static bool showRig      = true;
        static char modelPathBuf[512] = {};

        // ── Main loop ───────────────────────────────────────────────────────────
        while (!glfwWindowShouldClose(window)) {
            // Frame rate limiter
            static auto lastFrameEndTime = std::chrono::high_resolution_clock::now();
            if (s_fpsLimit > 0) {
                double targetFrameTime = 1.0 / s_fpsLimit;
                auto nowTime = std::chrono::high_resolution_clock::now();
                double elapsed = std::chrono::duration<double>(nowTime - lastFrameEndTime).count();
                if (elapsed < targetFrameTime) {
                    double sleepTime = targetFrameTime - elapsed;
                    std::this_thread::sleep_for(std::chrono::duration<double>(sleepTime));
                }
            }
            lastFrameEndTime = std::chrono::high_resolution_clock::now();

            glfwPollEvents();

            int fbW = 0, fbH = 0;
            glfwGetFramebufferSize(window, &fbW, &fbH);
            if (fbW > 0 && fbH > 0 && (fbW != (int)g_width || fbH != (int)g_height)) {
                g_width = fbW;
                g_height = fbH;
                rhi->resize(g_width, g_height);
            }

            auto  now = std::chrono::steady_clock::now();
            float dt  = std::chrono::duration<float>(now - lastFrameTime).count();
            lastFrameTime = now;
            dt = glm::clamp(dt, 0.0001f, 0.1f);

            // Reconnect watchdog
            if (!receiver.isConnected()) {
                if (now - lastReconnectAttempt > std::chrono::seconds(5)) {
                    spdlog::info("Reconnecting to tracker…");
                    receiver.stop(); receiver.start();
                    lastReconnectAttempt = now;
                }
            }

            // Hot keys
            if (!ImGui::GetIO().WantCaptureKeyboard) {
                if (kTab  .fired(window)) camera.toggleMode();
                if (kSpace.fired(window)) stickmanRenderer.frozen_ = !stickmanRenderer.frozen_;
                if (kF    .fired(window)) stickmanRenderer.depthViz_    = !stickmanRenderer.depthViz_;
                if (kB    .fired(window)) stickmanRenderer.drawBones_   = !stickmanRenderer.drawBones_;
                if (kP    .fired(window)) stickmanRenderer.drawPoints_  = !stickmanRenderer.drawPoints_;
                if (kM    .fired(window)) stickmanRenderer.drawFaceMesh_= !stickmanRenderer.drawFaceMesh_;
                if (kV    .fired(window) && avatarRenderer.hasModel()) {
                    s_useAvatar = !s_useAvatar;
                    saveSettings();
                    spdlog::info("Render mode: {}", s_useAvatar ? "Avatar" : "Stickman");
                }
                if (kH    .fired(window)) {
                    showSettings = !showSettings;
                }
                if (kEsc  .fired(window)) {
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
                if (kR.fired(window) && !stickmanRenderer.frozen_) {
                    auto frame = trackingState.get();
                    camera.setOrbitTarget(frame.getRootPosition());
                    spdlog::info("[Camera] Orbit re-centred");
                }
            }

            double mx, my;
            glfwGetCursorPos(window, &mx, &my);
            ImVec2 rawMousePos(static_cast<float>(mx), static_cast<float>(my));
            ImGui::GetIO().MousePos = rawMousePos;

            ImVec2 mousePos = rawMousePos;
            if (s_virtualViewport) {
                float localX = rawMousePos.x - (s_viewportPos.x + ImGui::GetStyle().WindowPadding.x);
                float localY = rawMousePos.y - (s_viewportPos.y + ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y);
                mousePos.x = (s_viewportSize.x > 0.001f) ? (localX / s_viewportSize.x) * 1920.f : 0.f;
                mousePos.y = (s_viewportSize.y > 0.001f) ? (localY / s_viewportSize.y) * 1080.f : 0.f;
            }

            auto projectToGLFW = [&](float vx, float vy) -> ImVec2 {
                if (s_virtualViewport) {
                    float localX = (vx / 1920.f) * s_viewportSize.x;
                    float localY = (vy / 1080.f) * s_viewportSize.y;
                    return ImVec2(
                        s_viewportPos.x + ImGui::GetStyle().WindowPadding.x + localX,
                        s_viewportPos.y + ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y + localY
                    );
                }
                return ImVec2(vx, vy);
            };

            static bool s_lastMouseDown = false;
            bool mouseDown = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS ||
                              glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
            static bool s_cameraDragging = false;

            if (mouseDown && !s_lastMouseDown) {
                // Click initiated on empty space (or model body, but not UI/bones/gizmos)
                if (!ImGui::GetIO().WantCaptureMouse && !s_hoveringBone && !s_hoveringGizmo) {
                    s_cameraDragging = true;
                }
            }
            if (!mouseDown) {
                s_cameraDragging = false;
            }
            s_lastMouseDown = mouseDown;

            bool shouldCapture = true;
            if (s_clickThrough) {
                shouldCapture = s_hoveringModel || ImGui::GetIO().WantCaptureMouse || s_cameraDragging || !s_activeDragBone.empty();
            }
            glfwSetWindowAttrib(window, GLFW_MOUSE_PASSTHROUGH, shouldCapture ? GLFW_FALSE : GLFW_TRUE);

            camera.inputEnabled = s_cameraDragging;
            camera.update(dt);
            currentFrame = trackingState.get();

#ifdef _WIN32
            // Poll for Python process crash
            static auto s_lastCrashCheck = std::chrono::steady_clock::now();
            auto nowCheck = std::chrono::steady_clock::now();
            if (pythonProcessHandle != NULL && nowCheck - s_lastCrashCheck > std::chrono::milliseconds(500)) {
                s_lastCrashCheck = nowCheck;
                DWORD exitCode = 0;
                if (GetExitCodeProcess(pythonProcessHandle, &exitCode)) {
                    if (exitCode != STILL_ACTIVE) {
                        spdlog::error("Python tracker process terminated with exit code: {}", exitCode);
                        CloseHandle(pythonProcessHandle);
                        pythonProcessHandle = NULL;
                        pythonPid = 0;
                        
                        // Read standard log output file to capture stack trace
                        std::ifstream logFile("tracker_output.log");
                        if (logFile.is_open()) {
                            std::string line;
                            std::vector<std::string> lastLines;
                            while (std::getline(logFile, line)) {
                                if (!line.empty()) {
                                    lastLines.push_back(line);
                                    if (lastLines.size() > 8) {
                                        lastLines.erase(lastLines.begin());
                                    }
                                }
                            }
                            s_trackerErrorMsg = "Exit code " + std::to_string(exitCode) + "\n";
                            for (const auto& l : lastLines) {
                                s_trackerErrorMsg += l + "\n";
                            }
                        } else {
                            s_trackerErrorMsg = "Crashed with exit code " + std::to_string(exitCode);
                        }
                    }
                }
            }
#endif

            if (s_useAvatar && avatarRenderer.hasModel())
                avatarRenderer.updateFromTracking(currentFrame, dt);

            // ── Reactive Background & Lighting ──────────────────────────────────────
            auto& sp = studio.params();
            auto& lp = lighting.params();

            // Audio loopback and keyboard explosion trigger
            static float s_explosionTimer = 0.f;
            static glm::vec3 s_explosionPos{0.f};

            if (g_audioExplosionTriggered.exchange(false) || (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)) {
                if (s_explosionTimer <= 0.01f) {
                    s_explosionTimer = 1.0f;
                    
                    // Determine brightest screen quadrant to accurately position light source
                    glm::vec3 tl{0.f}, tr{0.f}, bl{0.f}, br{0.f};
                    getScreenQuadrants(tl, tr, bl, br);
                    
                    auto getLum = [](const glm::vec3& c) {
                        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
                    };
                    float lTL = getLum(tl);
                    float lTR = getLum(tr);
                    float lBL = getLum(bl);
                    float lBR = getLum(br);
                    
                    glm::vec3 localExplosionOffset(0.f, 0.5f, 0.8f); // fallback center
                    std::string quadrantName = "Center";
                    
                    float maxL = lTL;
                    localExplosionOffset = glm::vec3(-1.2f, 0.8f, 0.6f);
                    quadrantName = "Top-Left";
                    
                    if (lTR > maxL) {
                        maxL = lTR;
                        localExplosionOffset = glm::vec3(1.2f, 0.8f, 0.6f);
                        quadrantName = "Top-Right";
                    }
                    if (lBL > maxL) {
                        maxL = lBL;
                        localExplosionOffset = glm::vec3(-1.2f, -0.1f, 0.6f);
                        quadrantName = "Bottom-Left";
                    }
                    if (lBR > maxL) {
                        maxL = lBR;
                        localExplosionOffset = glm::vec3(1.2f, -0.1f, 0.6f);
                        quadrantName = "Bottom-Right";
                    }
                    
                    s_explosionPos = currentFrame.getRootPosition() + localExplosionOffset;
                    spdlog::info("[Explosion] Triggered point light at {} quadrant. World Pos: ({:.2f}, {:.2f}, {:.2f})", 
                                 quadrantName, s_explosionPos.x, s_explosionPos.y, s_explosionPos.z);
                }
            }

            if (s_explosionTimer > 0.f) {
                s_explosionTimer -= dt * 1.5f;
                if (s_explosionTimer < 0.f) s_explosionTimer = 0.f;
            }

            // Update explosion point light parameters
            lp.explosionPos = glm::vec4(s_explosionPos, 2.5f); // 2.5m radius
            lp.explosionColor = glm::vec4(1.0f, 0.35f, 0.05f, s_explosionTimer * 2.5f); // Fade intensity

            if (s_screenReactiveLighting) {
                glm::vec3 tl{0.f}, tr{0.f}, bl{0.f}, br{0.f};
                static auto s_lastScreenPoll = std::chrono::steady_clock::now();
                auto nowPoll = std::chrono::steady_clock::now();
                if (nowPoll - s_lastScreenPoll > std::chrono::milliseconds(50)) {
                    getScreenQuadrants(tl, tr, bl, br);
                    s_lastScreenPoll = nowPoll;
                }
                float blendFactor = glm::clamp(dt * 4.0f, 0.f, 1.f);
                
                glm::vec3 s_targetScreenLeft = (tl + bl) * 0.5f;
                glm::vec3 s_targetScreenRight = (tr + br) * 0.5f;
                
                auto applyModifiers = [](const glm::vec3& color) {
                    glm::vec3 res = color * s_screenStrength;
                    res.r = std::max(res.r, s_screenMinBrightness);
                    res.g = std::max(res.g, s_screenMinBrightness);
                    res.b = std::max(res.b, s_screenMinBrightness);
                    return res;
                };
                
                glm::vec3 leftColor = applyModifiers(s_targetScreenLeft);
                glm::vec3 rightColor = applyModifiers(s_targetScreenRight);
                glm::vec3 avgColor = (leftColor + rightColor) * 0.5f;
                
                // Average of both for ambient light
                if (s_screenAffectsAmbient) {
                    lp.ambientColor = glm::mix(lp.ambientColor, avgColor, blendFactor);
                }
                
                // Left screen color drives the left fill light
                if (s_screenAffectsFill) {
                    lp.fillLight.color = glm::mix(lp.fillLight.color, leftColor, blendFactor);
                    lp.fillLight.intensity = glm::mix(lp.fillLight.intensity, 0.2f + 0.8f * glm::length(leftColor), blendFactor);
                }
                
                // Right screen color drives the right key light
                if (s_screenAffectsKey) {
                    lp.keyLight.color = glm::mix(lp.keyLight.color, rightColor, blendFactor);
                    lp.keyLight.intensity = glm::mix(lp.keyLight.intensity, 0.2f + 0.8f * glm::length(rightColor), blendFactor);
                }
            } else if (sp.reactiveBG) {
                if (currentFrame.hasHeadEuler()) {
                    // Interpolate colors based on head yaw/pitch/roll angles
                    float normYaw   = glm::clamp((currentFrame.headEuler.yaw + 30.f) / 60.f, 0.f, 1.f);
                    float normPitch = glm::clamp((currentFrame.headEuler.pitch + 20.f) / 40.f, 0.f, 1.f);

                    // Target background color mixes based on yaw/pitch
                    glm::vec4 targetTop = glm::mix(glm::vec4(0.06f, 0.06f, 0.22f, 1.f), glm::vec4(0.20f, 0.05f, 0.15f, 1.f), normYaw);
                    glm::vec4 targetBot = glm::mix(glm::vec4(0.02f, 0.02f, 0.06f, 1.f), glm::vec4(0.05f, 0.02f, 0.08f, 1.f), normPitch);
                    glm::vec4 targetSolid = glm::mix(targetBot, targetTop, 0.5f);

                    float t = glm::clamp(dt * sp.reactiveSpeed, 0.f, 1.f);
                    sp.bgColorTop = glm::mix(sp.bgColorTop, targetTop, t);
                    sp.bgColorBot = glm::mix(sp.bgColorBot, targetBot, t);
                    sp.bgColor    = glm::mix(sp.bgColor, targetSolid, t);

                    // Modify Directional Lights based on reactivity (Key Light & Fill Light colors/intensities)
                    glm::vec3 keyColorTarget = glm::mix(glm::vec3(1.0f, 0.95f, 0.85f), glm::vec3(0.9f, 0.6f, 1.0f), normYaw);
                    glm::vec3 ambientColorTarget = glm::mix(glm::vec3(0.18f, 0.18f, 0.22f), glm::vec3(0.12f, 0.10f, 0.20f), normPitch);
                    
                    lp.keyLight.color = glm::mix(lp.keyLight.color, keyColorTarget, t);
                    lp.ambientColor   = glm::mix(lp.ambientColor, ambientColorTarget, t);
                    lp.keyLight.intensity = glm::mix(lp.keyLight.intensity, 1.0f + 0.3f * normPitch, t);
                }
            }

            // Apply explosion color flash on top of background
            if (s_explosionTimer > 0.01f) {
                glm::vec4 flashTop = glm::vec4(1.0f, 0.45f, 0.1f, 1.f);
                glm::vec4 flashBot = glm::vec4(0.35f, 0.08f, 0.0f, 1.f);
                float tFlash = s_explosionTimer;
                
                sp.bgColorTop = glm::mix(sp.bgColorTop, flashTop, tFlash);
                sp.bgColorBot = glm::mix(sp.bgColorBot, flashBot, tFlash);
                sp.bgColor    = glm::mix(sp.bgColor, flashBot, tFlash);
            }

            if (s_useAvatar && avatarRenderer.hasModel()) {
                glm::mat4 modelMat = glm::scale(
                    glm::translate(glm::mat4(1.f), avatarRenderer.getModelTranslation()),
                    glm::vec3(avatarRenderer.getModelScale()));
                glm::mat4 hipMat = modelMat * avatarRenderer.getBoneTransform("Hips");
                glm::mat4 chestMat = modelMat * (avatarRenderer.getBoneTransform("Chest") == glm::mat4(1.f) ? avatarRenderer.getBoneTransform("Spine") : avatarRenderer.getBoneTransform("Chest"));
                glm::mat4 headMat = modelMat * avatarRenderer.getBoneTransform("Head");
                lp.shadowHipPos = glm::vec3(hipMat[3]);
                lp.shadowChestPos = glm::vec3(chestMat[3]);
                lp.shadowHeadPos = glm::vec3(headMat[3]);

                float scale = avatarRenderer.getModelScale();
                lp.shadowHipRadius = 0.25f * scale;
                lp.shadowChestRadius = 0.22f * scale;
                lp.shadowHeadRadius = 0.18f * scale;
            } else {
                lp.shadowHipPos = glm::vec3(0.f);
                lp.shadowChestPos = glm::vec3(0.f);
                lp.shadowHeadPos = glm::vec3(0.f);
                lp.shadowHipRadius = 0.25f;
                lp.shadowChestRadius = 0.22f;
                lp.shadowHeadRadius = 0.18f;
            }

            lighting.update();

            if (rhi->beginFrame()) {
                float screenW = s_virtualViewport ? 1920.f : float(g_width);
                float screenH = s_virtualViewport ? 1080.f : float(g_height);

                vts::rhi::Viewport viewport{0, 0, screenW, screenH, 0, 1};
                vts::rhi::Scissor  scissor {0, 0, static_cast<uint32_t>(screenW), static_cast<uint32_t>(screenH)};

                if (s_virtualViewport) {
                    rhi->beginRenderPass(g_virtualRP, g_virtualFB);
                } else {
                    rhi->beginRenderPass(nullptr, nullptr);
                }
                rhi->setViewport(viewport);
                rhi->setScissor(scissor);

                // ── Background ────────────────────────────────────────────────
                studio.renderBackground(static_cast<uint32_t>(screenW), static_cast<uint32_t>(screenH));

                float     aspect = (screenH > 0) ? screenW / screenH : 1.f;
                glm::mat4 view   = camera.viewMatrix();
                glm::mat4 proj   = camera.projMatrix(aspect);

                s_hoveringBone = false;
                s_hoveringModel = false;
                if (s_useAvatar && avatarRenderer.hasModel()) {
                    glm::mat4 modelMat = glm::scale(
                        glm::translate(glm::mat4(1.f), avatarRenderer.getModelTranslation()),
                        glm::vec3(avatarRenderer.getModelScale()));
                    glm::mat4 vp = proj * view * modelMat;
                    // Using local mapped mousePos

                    // 1. Check if mouse is hovering over the actual shape of the model (mesh collision vertices)
                    const auto& meshCollisionPoints = avatarRenderer.getSkinnedCollisionPositions();
                    float thresholdSq = 35.f * 35.f;
                    for (const auto& pt : meshCollisionPoints) {
                        glm::vec4 clipPos = vp * glm::vec4(pt, 1.f);
                        if (clipPos.w > 0.01f) {
                            glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
                            float screenX_raw = (ndc.x * 0.5f + 0.5f) * screenW;
                            float screenY_raw = (1.0f - (ndc.y * 0.5f + 0.5f)) * screenH;
                            ImVec2 p = projectToGLFW(screenX_raw, screenY_raw);
                            float screenX = p.x; float screenY = p.y;
                            float dx = mousePos.x - screenX;
                            float dy = mousePos.y - screenY;
                            if (dx*dx + dy*dy < thresholdSq) {
                                s_hoveringModel = true;
                                break;
                            }
                        }
                    }

                    // 2. Also check if close to any joint nodes to ensure full coverage
                    if (!s_hoveringModel) {
                        const auto& bones = avatarRenderer.getBoneList();
                        for (const auto& b : bones) {
                            glm::vec4 worldPos = glm::vec4(glm::vec3(b.world[3]), 1.f);
                            glm::vec4 clipPos = vp * worldPos;
                            if (clipPos.w > 0.01f) {
                                glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
                                float screenX_raw = (ndc.x * 0.5f + 0.5f) * screenW;
                                float screenY_raw = (1.0f - (ndc.y * 0.5f + 0.5f)) * screenH;
                                ImVec2 p = projectToGLFW(screenX_raw, screenY_raw);
                                float screenX = p.x; float screenY = p.y;
                                float dx = mousePos.x - screenX;
                                float dy = mousePos.y - screenY;
                                if (dx*dx + dy*dy < 20.f * 20.f) {
                                    s_hoveringModel = true;
                                    break;
                                }
                            }
                        }
                    }
                }

                // ── Scene ─────────────────────────────────────────────────────
                if (s_useAvatar && avatarRenderer.hasModel()) {
                    lighting.update();
                    avatarRenderer.render(view, proj, lighting.getLightingUB());
                } else {
                    stickmanRenderer.render(currentFrame, view, proj);
                }

                // ── Grid overlay ──────────────────────────────────────────────
                studio.renderGrid(view, proj);

                // ── ImGui ─────────────────────────────────────────────────────
                rhi->imguiNewFrame();

                // ── 3D Viewport Bone Selection & Gizmo Overlay ────────────────
                if (s_useAvatar && avatarRenderer.hasModel() && !s_streamMode) {
                    bool isHoveringAnyHandle = false;
                    const auto& bones = avatarRenderer.getBoneList();
                    glm::mat4 modelMat = glm::scale(
                        glm::translate(glm::mat4(1.f), avatarRenderer.getModelTranslation()),
                        glm::vec3(avatarRenderer.getModelScale()));
                    glm::mat4 vp = proj * view * modelMat;
                    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
                    // Using local mapped mousePos
                    float minDistance = 25.f; // threshold in pixels for easier clicking
                    std::string hoveredBone = "";

                    s_hoveringBone = !hoveredBone.empty();

                    // Check if mouse clicked inside the window but not on ImGui panels
                    bool clickTriggered = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().WantCaptureMouse;

                    // Handle active drag releases
                    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        s_activeDragBone = "";
                        s_activeDragAxis = -1;
                    }

                    // Process active drag updates
                    if (!s_activeDragBone.empty() && s_activeDragAxis >= 0) {
                        ImVec2 delta = ImGui::GetIO().MouseDelta;
                        float deltaVal = delta.x - delta.y; // positive for dragging right/up

                        auto& state = s_boneOverrides[s_activeDragBone];
                        state.active = true;

                        float sensitivityRot = 0.5f;     // degrees per pixel
                        float sensitivityTrans = 0.002f;  // meters per pixel

                        if (s_isDraggingTranslation) {
                            if (s_activeDragAxis == 0) state.translation.x += deltaVal * sensitivityTrans;
                            else if (s_activeDragAxis == 1) state.translation.y += deltaVal * sensitivityTrans;
                            else if (s_activeDragAxis == 2) state.translation.z += deltaVal * sensitivityTrans;
                        } else {
                            if (s_activeDragAxis == 0) state.rotation.x += deltaVal * sensitivityRot;
                            else if (s_activeDragAxis == 1) state.rotation.y += deltaVal * sensitivityRot;
                            else if (s_activeDragAxis == 2) state.rotation.z += deltaVal * sensitivityRot;
                        }

                        // Apply the updated override transform matrix
                        for (const auto& b : bones) {
                            if (b.name == s_activeDragBone) {
                                glm::vec3 translation = glm::vec3(b.local[3]) + state.translation;
                                glm::vec3 scale(
                                    glm::length(glm::vec3(b.local[0])),
                                    glm::length(glm::vec3(b.local[1])),
                                    glm::length(glm::vec3(b.local[2]))
                                );
                                glm::mat3 rotOnlyMat;
                                rotOnlyMat[0] = scale.x > 0.0001f ? glm::vec3(b.local[0]) / scale.x : glm::vec3(1,0,0);
                                rotOnlyMat[1] = scale.y > 0.0001f ? glm::vec3(b.local[1]) / scale.y : glm::vec3(0,1,0);
                                rotOnlyMat[2] = scale.z > 0.0001f ? glm::vec3(b.local[2]) / scale.z : glm::vec3(0,0,1);
                                glm::quat baseRot = glm::quat_cast(rotOnlyMat);

                                glm::quat offsetRot = glm::quat(glm::vec3(glm::radians(state.rotation.x), glm::radians(state.rotation.y), glm::radians(state.rotation.z)));
                                glm::quat finalRot = baseRot * offsetRot;

                                glm::mat4 T = glm::translate(glm::mat4(1.f), translation);
                                glm::mat4 R = glm::mat4_cast(finalRot);
                                glm::mat4 S = glm::scale(glm::mat4(1.f), scale);
                                glm::mat4 over = T * R * S;

                                avatarRenderer.setBoneOverride(b.name, over);
                                break;
                            }
                        }
                    }

                    // Draw bone connections (skeleton lines)
                    if (s_drawSkeletonLines) {
                        for (const auto& b : bones) {
                            if (b.parentIdx >= 0 && b.parentIdx < (int)bones.size()) {
                                const auto& p = bones[b.parentIdx];
                                glm::vec4 wPosChild = glm::vec4(glm::vec3(b.world[3]), 1.f);
                                glm::vec4 wPosParent = glm::vec4(glm::vec3(p.world[3]), 1.f);
                                glm::vec4 clipChild = vp * wPosChild;
                                glm::vec4 clipParent = vp * wPosParent;
                                if (clipChild.w > 0.01f && clipParent.w > 0.01f) {
                                    glm::vec3 ndcChild = glm::vec3(clipChild) / clipChild.w;
                                    ImVec2 pChild = projectToGLFW((ndcChild.x * 0.5f + 0.5f) * screenW, (1.0f - (ndcChild.y * 0.5f + 0.5f)) * screenH);
                                    float xChild = pChild.x; float yChild = pChild.y;

                                    glm::vec3 ndcParent = glm::vec3(clipParent) / clipParent.w;
                                    ImVec2 pParent = projectToGLFW((ndcParent.x * 0.5f + 0.5f) * screenW, (1.0f - (ndcParent.y * 0.5f + 0.5f)) * screenH);
                                    float xParent = pParent.x; float yParent = pParent.y;

                                    drawList->AddLine(ImVec2(xChild, yChild), ImVec2(xParent, yParent), IM_COL32(0, 255, 255, 180), 2.0f);
                                }
                            }
                        }
                    }

                    for (const auto& b : bones) {
                        glm::vec4 worldPos = glm::vec4(glm::vec3(b.world[3]), 1.f);
                        glm::vec4 clipPos = vp * worldPos;
                        if (clipPos.w > 0.01f) {
                            glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
                            float screenX_raw = (ndc.x * 0.5f + 0.5f) * screenW;
                            float screenY_raw = (1.0f - (ndc.y * 0.5f + 0.5f)) * screenH;
                            ImVec2 p = projectToGLFW(screenX_raw, screenY_raw);
                            float screenX = p.x; float screenY = p.y;

                            float dx = mousePos.x - screenX;
                            float dy = mousePos.y - screenY;
                            float dist = std::sqrt(dx*dx + dy*dy);

                            bool isSelected = (b.name == s_selectedBoneName);
                            bool isHovered = (dist < 15.f);

                            if (isHovered && dist < minDistance) {
                                hoveredBone = b.name;
                                minDistance = dist;
                            }

                            // Draw bone node circle
                            if (s_drawJointNodes) {
                                ImU32 color = isSelected ? IM_COL32(255, 215, 0, 240) : (isHovered ? IM_COL32(255, 100, 100, 220) : IM_COL32(80, 160, 240, 150));
                                float radius = isSelected ? 8.f : 5.f;
                                drawList->AddCircleFilled(ImVec2(screenX, screenY), radius, color);
                                drawList->AddCircle(ImVec2(screenX, screenY), radius + 1.5f, IM_COL32(0, 0, 0, 180), 0, 1.0f);
                            }
                        }
                    }

                    // Handle selection pick
                    if (clickTriggered && !hoveredBone.empty()) {
                        s_selectedBoneName = hoveredBone;
                    }

                    // Draw active colliders visualization
                    const auto& collidersList = avatarRenderer.getColliders();
                    for (const auto& col : collidersList) {
                        glm::mat4 colBoneWorld = avatarRenderer.getBoneTransform(col.boneName);
                        glm::vec3 colWorldPos = glm::vec3(colBoneWorld * glm::vec4(col.offset, 1.0f));

                        glm::vec4 centerClip = vp * glm::vec4(colWorldPos, 1.f);
                        glm::vec4 edgeClip = vp * glm::vec4(colWorldPos + glm::vec3(col.radius, 0.f, 0.f), 1.f);

                        if (centerClip.w > 0.01f && edgeClip.w > 0.01f) {
                            glm::vec3 centerNdc = glm::vec3(centerClip) / centerClip.w;
                            glm::vec3 edgeNdc = glm::vec3(edgeClip) / edgeClip.w;
                            ImVec2 pC = projectToGLFW((centerNdc.x * 0.5f + 0.5f) * screenW, (1.0f - (centerNdc.y * 0.5f + 0.5f)) * screenH);
                            float cX = pC.x; float cY = pC.y;
                            ImVec2 pE = projectToGLFW((edgeNdc.x * 0.5f + 0.5f) * screenW, (1.0f - (edgeNdc.y * 0.5f + 0.5f)) * screenH);
                            float eX = pE.x; float eY = pE.y;
                            float screenRadius = glm::distance(glm::vec2(cX, cY), glm::vec2(eX, eY));

                            // Draw main collider sphere outline and translucent fill
                            drawList->AddCircle(ImVec2(cX, cY), screenRadius, IM_COL32(255, 80, 80, 200), 32, 2.0f);
                            drawList->AddCircleFilled(ImVec2(cX, cY), screenRadius, IM_COL32(255, 80, 80, 35), 32);

                            // Draw small center point
                            drawList->AddCircleFilled(ImVec2(cX, cY), 3.0f, IM_COL32(255, 80, 80, 225));

                            // Draw capsule bounds for torso approximation
                            std::string boneNameLower = col.boneName;
                            std::transform(boneNameLower.begin(), boneNameLower.end(), boneNameLower.begin(), ::tolower);
                            if (boneNameLower == "spine" || boneNameLower == "chest" || boneNameLower == "upperchest") {
                                for (float offsetStep : { -0.12f, -0.06f, 0.06f, 0.12f }) {
                                    glm::vec3 extraWorldPos = glm::vec3(colBoneWorld * glm::vec4(col.offset + glm::vec3(0.f, offsetStep, 0.f), 1.0f));
                                    glm::vec4 extraClip = vp * glm::vec4(extraWorldPos, 1.f);
                                    if (extraClip.w > 0.01f) {
                                        glm::vec3 extraNdc = glm::vec3(extraClip) / extraClip.w;
                                        ImVec2 pEx = projectToGLFW((extraNdc.x * 0.5f + 0.5f) * screenW, (1.0f - (extraNdc.y * 0.5f + 0.5f)) * screenH);
                                        float exX = pEx.x; float exY = pEx.y;
                                        drawList->AddCircle(ImVec2(exX, exY), screenRadius, IM_COL32(255, 120, 120, 120), 16, 1.2f);
                                    }
                                }
                            }
                        }
                    }

                    // Draw complex mesh-based collision points (translucent points wrapping the mesh)
                    const auto& meshCollisionPoints = avatarRenderer.getSkinnedCollisionPositions();
                    for (const auto& pt : meshCollisionPoints) {
                        glm::vec4 clipPos = vp * glm::vec4(pt, 1.f);
                        if (clipPos.w > 0.01f) {
                            glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
                            float screenX_raw = (ndc.x * 0.5f + 0.5f) * screenW;
                            float screenY_raw = (1.0f - (ndc.y * 0.5f + 0.5f)) * screenH;
                            ImVec2 p = projectToGLFW(screenX_raw, screenY_raw);
                            float screenX = p.x; float screenY = p.y;
                            
                            // Draw as a very small, soft translucent point
                            drawList->AddCircleFilled(ImVec2(screenX, screenY), 2.0f, IM_COL32(255, 160, 60, 140));
                        }
                    }

                    // Render axes & name for selected/hovered bone
                    for (const auto& b : bones) {
                        bool isSelected = (b.name == s_selectedBoneName);
                        bool isHovered = (b.name == hoveredBone);

                        if (isSelected || isHovered) {
                            glm::vec3 origin = glm::vec3(b.world[3]);
                            ImVec2 pOrg;
                            
                            glm::vec4 clip = vp * glm::vec4(origin, 1.f);
                            if (clip.w > 0.01f) {
                                glm::vec3 ndc = glm::vec3(clip) / clip.w;
                                float screenX_raw = (ndc.x * 0.5f + 0.5f) * screenW;
                                float screenY_raw = (1.0f - (ndc.y * 0.5f + 0.5f)) * screenH;
                                ImVec2 p = projectToGLFW(screenX_raw, screenY_raw);
                                float screenX = p.x; float screenY = p.y;
                                pOrg = ImVec2(screenX, screenY);

                                if (isHovered) {
                                    // Tooltip/label
                                    ImVec2 textSize = ImGui::CalcTextSize(b.name.c_str());
                                    drawList->AddRectFilled(ImVec2(screenX + 10.f, screenY - 8.f), ImVec2(screenX + 14.f + textSize.x, screenY + 8.f), IM_COL32(20, 20, 20, 220), 3.f);
                                    drawList->AddRect(ImVec2(screenX + 10.f, screenY - 8.f), ImVec2(screenX + 14.f + textSize.x, screenY + 8.f), IM_COL32(255, 215, 0, 255), 3.f, 0, 1.f);
                                    drawList->AddText(ImVec2(screenX + 12.f, screenY - 6.f), IM_COL32(255, 255, 255, 255), b.name.c_str());
                                }

                                if (isSelected) {
                                    // Draw local axes gizmo
                                    glm::vec3 right = glm::normalize(glm::vec3(b.world[0]));
                                    glm::vec3 up    = glm::normalize(glm::vec3(b.world[1]));
                                    glm::vec3 fwd   = glm::normalize(glm::vec3(b.world[2]));
                                    float axisLen = 0.15f; // size in meters

                                    auto projectVec = [&](const glm::vec3& p3d) -> ImVec2 {
                                        glm::vec4 c = vp * glm::vec4(p3d, 1.f);
                                        if (c.w > 0.01f) {
                                            glm::vec3 n = glm::vec3(c) / c.w;
                                            ImVec2 pProj = projectToGLFW((n.x * 0.5f + 0.5f) * screenW, (1.0f - (n.y * 0.5f + 0.5f)) * screenH);
                                             return pProj;
                                        }
                                        return pOrg;
                                    };

                                    ImVec2 pX = projectVec(origin + right * axisLen);
                                    ImVec2 pY = projectVec(origin + up * axisLen);
                                    ImVec2 pZ = projectVec(origin + fwd * axisLen);

                                    float handleRadius = 12.f;
                                    auto checkHandleHover = [&](const ImVec2& p) -> bool {
                                        float dx = mousePos.x - p.x;
                                        float dy = mousePos.y - p.y;
                                        return (dx*dx + dy*dy) < (handleRadius * handleRadius);
                                    };

                                    bool isHoverX = checkHandleHover(pX);
                                    bool isHoverY = checkHandleHover(pY);
                                    bool isHoverZ = checkHandleHover(pZ);

                                    // Initiate drag on mouse click
                                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                        if (isHoverX) { s_activeDragBone = b.name; s_activeDragAxis = 0; s_isDraggingTranslation = ImGui::GetIO().KeyShift; }
                                        else if (isHoverY) { s_activeDragBone = b.name; s_activeDragAxis = 1; s_isDraggingTranslation = ImGui::GetIO().KeyShift; }
                                        else if (isHoverZ) { s_activeDragBone = b.name; s_activeDragAxis = 2; s_isDraggingTranslation = ImGui::GetIO().KeyShift; }
                                    }

                                    // X Axis (Red)
                                    drawList->AddLine(pOrg, pX, IM_COL32(255, 50, 50, 255), isHoverX ? 4.5f : 3.0f);
                                    drawList->AddCircleFilled(pX, isHoverX ? 7.0f : 5.0f, IM_COL32(255, 50, 50, 255));
                                    drawList->AddText(ImVec2(pX.x + 4.f, pX.y - 6.f), IM_COL32(255, 120, 120, 255), "X");

                                    // Y Axis (Green)
                                    drawList->AddLine(pOrg, pY, IM_COL32(50, 255, 50, 255), isHoverY ? 4.5f : 3.0f);
                                    drawList->AddCircleFilled(pY, isHoverY ? 7.0f : 5.0f, IM_COL32(50, 255, 50, 255));
                                    drawList->AddText(ImVec2(pY.x + 4.f, pY.y - 6.f), IM_COL32(120, 255, 120, 255), "Y");

                                    // Z Axis (Blue)
                                    drawList->AddLine(pOrg, pZ, IM_COL32(50, 50, 255, 255), isHoverZ ? 4.5f : 3.0f);
                                    drawList->AddCircleFilled(pZ, isHoverZ ? 7.0f : 5.0f, IM_COL32(50, 50, 255, 255));
                                    drawList->AddText(ImVec2(pZ.x + 4.f, pZ.y - 6.f), IM_COL32(120, 120, 255, 255), "Z");
                                    if (isHoverX || isHoverY || isHoverZ) {
                                        isHoveringAnyHandle = true;
                                    }
                                }
                            }
                        }
                    }
                    s_hoveringGizmo = isHoveringAnyHandle;
                }

                // ── Status overlay ────────────────────────────────────────────
                if (!s_streamMode) {
                    ImGui::SetNextWindowPos ({10, 10}, ImGuiCond_Always);
                    ImGui::SetNextWindowSize({300, 0}, ImGuiCond_Always);
                    ImGui::SetNextWindowBgAlpha(0.65f);
                    ImGui::Begin("##status", nullptr,
                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoFocusOnAppearing);

                    if (receiver.isConnected())
                        ImGui::TextColored({0.2f,0.9f,0.2f,1}, "● Tracker");
                    else
                        ImGui::TextColored({0.9f,0.2f,0.2f,1}, "● Tracker (disconnected)");

                    ImGui::Text("Render  %.0f fps", ImGui::GetIO().Framerate);
                    ImGui::Text("Tracker %.1f fps", receiver.stats().fps.load());

                    // Head Euler overlay
                    if (currentFrame.hasHeadEuler()) {
                        ImGui::TextColored({0.9f,0.7f,0.2f,1},
                            "Head  Y:%.1f° P:%.1f° R:%.1f°",
                            currentFrame.headEuler.yaw,
                            currentFrame.headEuler.pitch,
                            currentFrame.headEuler.roll);
                    }

                    if (s_useAvatar && avatarRenderer.hasModel()) {
                        ImGui::TextColored({0.2f,0.8f,1.0f,1}, "◆ %s", avatarRenderer.getModelName().c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("[V]")) { s_useAvatar = false; saveSettings(); }
                    } else {
                        ImGui::TextColored({0.8f,0.8f,0.2f,1}, "◇ Stickman");
                        if (avatarRenderer.hasModel()) { ImGui::SameLine(); if (ImGui::SmallButton("[V]")) { s_useAvatar = true; saveSettings(); } }
                    }

                    {
                        const char* modeStr = camera.mode() == vts::renderer::CameraMode::Orbit ? "Orbit" : "Fly";
                        if (stickmanRenderer.frozen_)
                            ImGui::TextColored({1.f,0.65f,0.f,1}, "⏸ FROZEN  [%s]", modeStr);
                        else
                            ImGui::TextDisabled("[%s]  Tab=switch  Space=freeze", modeStr);
                    }

                    ImGui::Text("Face %s  LH %s  RH %s  Pose %s",
                        currentFrame.hasFaceWorld()  ? "✓" : "–",
                        currentFrame.hasLeftHand()   ? "✓" : "–",
                        currentFrame.hasRightHand()  ? "✓" : "–",
                        currentFrame.hasWorldPose()  ? "✓" : "–");

                    // Warnings
                    std::vector<std::string> warnings;
                    if (!s_trackerErrorMsg.empty()) {
                        warnings.push_back("Tracker process crashed!");
                    }
                    if (receiver.isConnected()) {
                        if (!currentFrame.hasFaceWorld()) warnings.push_back("Face tracking missing");
                        if (!currentFrame.hasWorldPose()) warnings.push_back("Body tracking missing");
                        if (!currentFrame.hasLeftHand()) warnings.push_back("Left hand tracking missing");
                        if (!currentFrame.hasRightHand()) warnings.push_back("Right hand tracking missing");

                        // Camera angle warning
                        glm::vec3 camForward = -glm::normalize(glm::vec3(view[0][2], view[1][2], view[2][2]));
                        glm::vec3 avatarForward(0.f, 0.f, -1.f);
                        float dotFwd = glm::dot(camForward, avatarForward);
                        if (dotFwd < -0.3f) {
                            warnings.push_back("Camera angle: BACK (Inaccurate)");
                        } else if (dotFwd < 0.4f) {
                            warnings.push_back("Camera angle: SIDE (Inaccurate)");
                        }
                    }

                    if (!warnings.empty()) {
                        ImGui::Separator();
                        ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.f}, "Warnings / Errors:");
                        for (const auto& w : warnings) {
                            ImGui::BulletText("%s", w.c_str());
                        }
                        if (!s_trackerErrorMsg.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, {1.0f, 0.4f, 0.4f, 1.0f});
                            ImGui::TextWrapped("%s", s_trackerErrorMsg.c_str());
                            ImGui::PopStyleColor();
                        }
                    }

                    ImGui::End();
                }

                // ── Sleek floating control panel (Top Right) ──────────────────
                if (!s_streamMode) {
                    ImGui::SetNextWindowPos({(float)g_width - 60.f, 10.f}, ImGuiCond_Always);
                    ImGui::SetNextWindowSize({50.f, 50.f}, ImGuiCond_Always);
                    ImGui::SetNextWindowBgAlpha(0.35f);
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 4.f));
                    ImGui::Begin("##control_toggle", nullptr,
                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing);
                    if (ImGui::Button(showSettings ? "[-]" : "[+]", ImVec2(42.f, 42.f))) {
                        showSettings = !showSettings;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(showSettings ? "Minimize Settings Panel" : "Restore Settings Panel");
                    }
                    ImGui::End();
                    ImGui::PopStyleVar();
                }

                // ── Settings window ───────────────────────────────────────────
                ImGui::SetNextWindowPos ({10, 170}, ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize({420, 520}, ImGuiCond_FirstUseEver);
                if (ImGui::Begin("Settings", &showSettings)) {
                    if (ImGui::BeginTabBar("SettingsTabs")) {

                        // ── Avatar tab ─────────────────────────────────────────
                        if (ImGui::BeginTabItem("Avatar")) {
                            // Browse button
                            if (ImGui::Button("Browse Model…  (VRM / GLB / glTF)", {-1, 0})) {
                                auto chosen = vts::renderer::browseForModelFile();
                                if (chosen.has_value()) {
                                    bool ok = avatarRenderer.loadModel(chosen.value());
                                    if (ok) {
                                        s_useAvatar = true;
                                        s_loadedModelPath = chosen.value();
                                        applyLoadedModelSettings(avatarRenderer);
                                        saveSettings();
                                        spdlog::info("Loaded: {}", chosen.value());
                                    } else {
                                        spdlog::error("Failed to load: {}", chosen.value());
                                    }
                                }
                            }

                            if (avatarRenderer.hasModel()) {
                                ImGui::TextColored({0.2f,0.8f,1.0f,1}, "Loaded: %s", avatarRenderer.getModelName().c_str());

                                float prog = avatarRenderer.getLoadProgress();
                                if (prog < 1.f) ImGui::ProgressBar(prog);

                                ImGui::SameLine();
                                if (ImGui::Button("Unload")) { avatarRenderer.unloadModel(); s_loadedModelPath = ""; s_useAvatar = false; saveSettings(); }

                                ImGui::SeparatorText("Transform");
                                float scale = avatarRenderer.getModelScale();
                                if (ImGui::SliderFloat("Scale##ms",  &scale, 0.01f, 5.f, "%.2f")) {
                                    avatarRenderer.setModelScale(scale);
                                    s_avatarScale = scale;
                                    saveSettings();
                                }
                                glm::vec3 trans = avatarRenderer.getModelTranslation();
                                if (ImGui::DragFloat3("Offset##mt", &trans.x, 0.01f, -5.f, 5.f)) {
                                    avatarRenderer.setModelTranslation(trans);
                                    s_avatarTranslation = trans;
                                    saveSettings();
                                }
                                if (ImGui::Button("Reset to Defaults##transreset", {-1, 0})) {
                                    s_avatarScale = 1.0f;
                                    s_avatarTranslation = glm::vec3(0.f);
                                    avatarRenderer.setModelScale(s_avatarScale);
                                    avatarRenderer.setModelTranslation(s_avatarTranslation);
                                    saveSettings();
                                }

                                ImGui::SeparatorText("Expressions (VRM)");
                                const char* exprs[] = {"happy","angry","sad","surprised","blink","a","i","u","e","o"};
                                for (int i = 0; i < 10; i++) {
                                    if (ImGui::Button(exprs[i])) avatarRenderer.setExpression(exprs[i], 1.0f);
                                    if (i != 9) ImGui::SameLine();
                                }

                                ImGui::SeparatorText("Tracking Options");
                                bool armTracking = avatarRenderer.isArmTrackingEnabled();
                                if (ImGui::Checkbox("Enable Arm Tracking##armtrack", &armTracking)) {
                                    avatarRenderer.setArmTrackingEnabled(armTracking);
                                }
                                ImGui::SameLine();
                                bool handTracking = avatarRenderer.isHandTrackingEnabled();
                                if (ImGui::Checkbox("Enable Hand & Finger Tracking##handtrack", &handTracking)) {
                                    avatarRenderer.setHandTrackingEnabled(handTracking);
                                }

                                ImGui::SeparatorText("Blend Shapes (auto from tracking)");
                                bool ab = avatarRenderer.getAutoBlendShapes();
                                if (ImGui::Checkbox("Auto##abs", &ab)) avatarRenderer.setAutoBlendShapes(ab);
                                ImGui::SameLine();
                                if (ImGui::Button("Reset all")) avatarRenderer.resetBlendShapes();

                                // Show active blendshapes
                                if (ImGui::TreeNode("Active weights")) {
                                    for (const auto& [k, v] : avatarRenderer.getAllBlendShapeWeights()) {
                                        if (v > 0.01f) ImGui::Text("  %s: %.3f", k.c_str(), v);
                                    }
                                    ImGui::TreePop();
                                }

                                ImGui::SeparatorText("Bone Physics");
                                bool physicsEnabled = avatarRenderer.isPhysicsEnabled();
                                if (ImGui::Checkbox("Enable Spring Physics", &physicsEnabled)) {
                                    avatarRenderer.setPhysicsEnabled(physicsEnabled);
                                    saveSettings();
                                }
                                
                                if (physicsEnabled) {
                                    ImGui::Indent();
                                    
                                    // Preset chains
                                    if (ImGui::Button("Quick Setup: Hair & Ears")) {
                                        // Standard VRM bone names for ears and hair
                                        vts::renderer::ModelRenderer::PhysicsParams hairParams{ 3.5f, 1.0f, 0.35f, 0.5f };
                                        vts::renderer::ModelRenderer::PhysicsParams earParams{ 5.0f, 0.8f, 0.25f, 0.2f };
                                        
                                        // Attempt to auto-detect and add hair/ear root bones
                                        const auto& bones = avatarRenderer.getBoneList();
                                        for (const auto& b : bones) {
                                            std::string nameLower = b.name;
                                            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                                            
                                            // Add hair chain if bone name contains "hair" and has a parent containing "head"
                                            if (nameLower.find("hair") != std::string::npos) {
                                                // Only add roots (bones with "hair" whose parent does NOT contain "hair")
                                                bool parentIsHair = false;
                                                if (b.parentIdx >= 0) {
                                                    std::string parentName = bones[b.parentIdx].name;
                                                    std::transform(parentName.begin(), parentName.end(), parentName.begin(), ::tolower);
                                                    if (parentName.find("hair") != std::string::npos) {
                                                        parentIsHair = true;
                                                    }
                                                }
                                                if (!parentIsHair) {
                                                    avatarRenderer.addPhysicsChain(b.name, hairParams);
                                                }
                                            }
                                            
                                            // Add ear chain
                                            if (nameLower.find("ear") != std::string::npos && 
                                                nameLower.find("head") == std::string::npos) {
                                                bool parentIsEar = false;
                                                if (b.parentIdx >= 0) {
                                                    std::string parentName = bones[b.parentIdx].name;
                                                    std::transform(parentName.begin(), parentName.end(), parentName.begin(), ::tolower);
                                                    if (parentName.find("ear") != std::string::npos) {
                                                        parentIsEar = true;
                                                    }
                                                }
                                                if (!parentIsEar) {
                                                    avatarRenderer.addPhysicsChain(b.name, earParams);
                                                }
                                            }
                                        }
                                        saveSettings();
                                    }
                                    
                                    ImGui::SameLine();
                                    if (ImGui::Button("Clear Chains")) {
                                        avatarRenderer.clearPhysicsChains();
                                        saveSettings();
                                    }

                                    auto chains = avatarRenderer.getPhysicsChains();
                                    ImGui::Text("Active Physics Chains: %d", (int)chains.size());

                                    // Global Physics Settings
                                    if (!chains.empty()) {
                                        ImGui::SeparatorText("Global Settings (All Chains)");
                                        static float s_globalStiffness = 3.5f;
                                        static float s_globalGravity = 1.0f;
                                        static float s_globalDrag = 0.35f;
                                        static float s_globalWind = 0.5f;

                                        bool globDirty = false;
                                        globDirty |= ImGui::SliderFloat("Global Stiffness##globstiff", &s_globalStiffness, 0.1f, 15.f, "%.1f");
                                        globDirty |= ImGui::SliderFloat("Global Gravity##globgrav", &s_globalGravity, 0.0f, 5.f, "%.2f");
                                        globDirty |= ImGui::SliderFloat("Global Drag##globdrag", &s_globalDrag, 0.01f, 0.99f, "%.2f");
                                        globDirty |= ImGui::SliderFloat("Global Wind##globwind", &s_globalWind, 0.0f, 5.0f, "%.2f");

                                        if (globDirty) {
                                            for (const auto& c : chains) {
                                                vts::renderer::ModelRenderer::PhysicsParams params;
                                                params.stiffness = s_globalStiffness;
                                                params.gravity = s_globalGravity;
                                                params.drag = s_globalDrag;
                                                params.wind = s_globalWind;
                                                avatarRenderer.addPhysicsChain(c.rootBoneName, params);
                                            }
                                            saveSettings();
                                        }
                                    }
                                    
                                    // Allow custom chain creation via bone selection
                                    static char rootBoneInput[128] = "";
                                    ImGui::InputText("Bone Root##physroot", rootBoneInput, IM_ARRAYSIZE(rootBoneInput));
                                    ImGui::SameLine();
                                    if (ImGui::Button("Add Chain##addphys")) {
                                        if (rootBoneInput[0] != '\0') {
                                            vts::renderer::ModelRenderer::PhysicsParams params; // default params
                                            avatarRenderer.addPhysicsChain(rootBoneInput, params);
                                            rootBoneInput[0] = '\0';
                                            saveSettings();
                                        }
                                    }
                                    
                                    if (!chains.empty()) {
                                        if (ImGui::TreeNode("Chain Settings")) {
                                            for (size_t ci = 0; ci < chains.size(); ++ci) {
                                                ImGui::PushID(ci);
                                                if (ImGui::TreeNode(chains[ci].rootBoneName.c_str())) {
                                                    auto params = chains[ci].params;
                                                    bool dirty = false;
                                                    dirty |= ImGui::SliderFloat("Stiffness", &params.stiffness, 0.1f, 15.f, "%.1f");
                                                    dirty |= ImGui::SliderFloat("Gravity", &params.gravity, 0.0f, 5.f, "%.2f");
                                                    dirty |= ImGui::SliderFloat("Drag", &params.drag, 0.01f, 0.99f, "%.2f");
                                                    dirty |= ImGui::SliderFloat("Wind Strength", &params.wind, 0.0f, 5.0f, "%.2f");
                                                    
                                                    if (dirty) {
                                                        avatarRenderer.addPhysicsChain(chains[ci].rootBoneName, params);
                                                        saveSettings();
                                                    }
                                                    ImGui::TreePop();
                                                }
                                                ImGui::PopID();
                                            }
                                            ImGui::TreePop();
                                        }
                                    }

                                    ImGui::SeparatorText("Colliders");
                                    if (ImGui::Button("Quick Setup: Full Body Colliders")) {
                                        const auto& bones = avatarRenderer.getBoneList();
                                        
                                        auto findAndAdd = [&](const std::vector<std::string>& keywords, const std::vector<std::string>& exclude, float radius, const glm::vec3& offset) {
                                            for (const auto& b : bones) {
                                                std::string nameLower = b.name;
                                                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                                                
                                                bool match = true;
                                                for (const auto& kw : keywords) {
                                                    if (nameLower.find(kw) == std::string::npos) { match = false; break; }
                                                }
                                                if (match) {
                                                    for (const auto& ex : exclude) {
                                                        if (nameLower.find(ex) != std::string::npos) { match = false; break; }
                                                    }
                                                }
                                                if (match) {
                                                    vts::renderer::ModelRenderer::PhysicsCollider col;
                                                    col.boneName = b.name;
                                                    col.radius = radius;
                                                    col.offset = offset;
                                                    avatarRenderer.addCollider(col);
                                                    break;
                                                }
                                            }
                                        };

                                        // Apply body colliders (head, neck, chest, spine, hips, shoulders, upper arms, lower arms)
                                        findAndAdd({"head"}, {}, 0.16f, glm::vec3(0.f, 0.08f, 0.f));
                                        findAndAdd({"neck"}, {}, 0.09f, glm::vec3(0.f, 0.04f, 0.f));
                                        findAndAdd({"upper", "chest"}, {}, 0.18f, glm::vec3(0.f, 0.05f, 0.f));
                                        findAndAdd({"upperchest"}, {}, 0.18f, glm::vec3(0.f, 0.05f, 0.f));
                                        findAndAdd({"chest"}, {"upper"}, 0.18f, glm::vec3(0.f, 0.05f, 0.f));
                                        findAndAdd({"spine"}, {}, 0.18f, glm::vec3(0.f, 0.05f, 0.f));
                                        findAndAdd({"hips"}, {}, 0.22f, glm::vec3(0.f, 0.f, 0.f));
                                        findAndAdd({"hip"}, {"left", "right"}, 0.22f, glm::vec3(0.f, 0.f, 0.f));
                                        findAndAdd({"left", "shoulder"}, {}, 0.08f, glm::vec3(0.f, 0.f, 0.f));
                                        findAndAdd({"right", "shoulder"}, {}, 0.08f, glm::vec3(0.f, 0.f, 0.f));
                                        findAndAdd({"left", "upper", "arm"}, {}, 0.08f, glm::vec3(0.f, 0.f, 0.f));
                                        findAndAdd({"left", "upperarm"}, {}, 0.08f, glm::vec3(0.f, 0.f, 0.f));
                                        findAndAdd({"right", "upper", "arm"}, {}, 0.08f, glm::vec3(0.f, 0.f, 0.f));
                                        findAndAdd({"right", "upperarm"}, {}, 0.08f, glm::vec3(0.f, 0.f, 0.f));
                                        findAndAdd({"left", "lower", "arm"}, {}, 0.07f, glm::vec3(0.f, 0.f, 0.f));
                                        findAndAdd({"left", "forearm"}, {}, 0.07f, glm::vec3(0.f, 0.f, 0.f));
                                        findAndAdd({"left", "lowerarm"}, {}, 0.07f, glm::vec3(0.f, 0.f, 0.f));
                                        findAndAdd({"right", "lower", "arm"}, {}, 0.07f, glm::vec3(0.f, 0.f, 0.f));
                                        findAndAdd({"right", "forearm"}, {}, 0.07f, glm::vec3(0.f, 0.f, 0.f));
                                        findAndAdd({"right", "lowerarm"}, {}, 0.07f, glm::vec3(0.f, 0.f, 0.f));
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button("Clear Colliders")) {
                                        avatarRenderer.clearColliders();
                                    }

                                    auto colliders = avatarRenderer.getColliders();
                                    ImGui::Text("Active Colliders: %d", (int)colliders.size());

                                    float meshColRad = avatarRenderer.getMeshCollisionRadius();
                                    if (ImGui::SliderFloat("Mesh Collision Radius##meshcolrad", &meshColRad, 0.01f, 0.25f, "%.3f m")) {
                                        avatarRenderer.setMeshCollisionRadius(meshColRad);
                                    }

                                    static char colBoneInput[128] = "";
                                    static float colRadiusInput = 0.15f;
                                    static glm::vec3 colOffsetInput{0.f, 0.f, 0.f};
                                    
                                    ImGui::InputText("Collider Bone##colbone", colBoneInput, IM_ARRAYSIZE(colBoneInput));
                                    ImGui::SliderFloat("Radius##colradius", &colRadiusInput, 0.01f, 1.0f, "%.2f m");
                                    ImGui::DragFloat3("Offset##coloffset", &colOffsetInput.x, 0.005f, -0.5f, 0.5f, "%.3f");
                                    
                                    if (ImGui::Button("Add Collider##addcol")) {
                                        if (colBoneInput[0] != '\0') {
                                            vts::renderer::ModelRenderer::PhysicsCollider col;
                                            col.boneName = colBoneInput;
                                            col.offset = colOffsetInput;
                                            col.radius = colRadiusInput;
                                            avatarRenderer.addCollider(col);
                                            colBoneInput[0] = '\0';
                                        }
                                    }

                                    if (!colliders.empty()) {
                                        if (ImGui::TreeNode("Collider Settings")) {
                                            for (size_t colI = 0; colI < colliders.size(); ++colI) {
                                                ImGui::PushID(colI + 1000);
                                                if (ImGui::TreeNode(colliders[colI].boneName.c_str())) {
                                                    auto col = colliders[colI];
                                                    bool colDirty = false;
                                                    colDirty |= ImGui::SliderFloat("Radius", &col.radius, 0.01f, 1.0f, "%.2f m");
                                                    colDirty |= ImGui::DragFloat3("Offset", &col.offset.x, 0.005f, -0.5f, 0.5f, "%.3f");
                                                    if (colDirty) {
                                                        avatarRenderer.addCollider(col);
                                                    }
                                                    ImGui::TreePop();
                                                }
                                                ImGui::PopID();
                                            }
                                            ImGui::TreePop();
                                        }
                                    }
                                    ImGui::Unindent();
                                }
                            } else {
                                ImGui::TextDisabled("No model loaded");
                                ImGui::BulletText("Use \"Browse Model…\" above");
                                ImGui::BulletText("Or place model.vrm / avatar.glb next to the exe");
                            }
                            ImGui::EndTabItem();
                        }

                         // ── Tracker tab ───────────────────────────────────────
                        if (ImGui::BeginTabItem("Tracker")) {
                            ImGui::SeparatorText("Tracker Process Controls");
                            if (pythonPid != 0) {
                                ImGui::TextColored({0.2f, 0.9f, 0.2f, 1.f}, "● Tracker Running (PID: %lu)", pythonPid);
                                if (ImGui::Button("Stop Tracker Process", {-1, 0})) {
                                    killProcess(pythonProcessHandle, pythonPid);
                                    pythonPid = 0;
                                }
                            } else {
                                ImGui::TextColored({0.9f, 0.2f, 0.2f, 1.f}, "○ Tracker Stopped");
                                if (ImGui::Button("Start Tracker Process", {-1, 0})) {
                                    if (!pythonScript.empty()) {
                                        s_trackerErrorMsg = "";
                                        startPythonTracker(pythonScript.string(), pythonPid, pythonProcessHandle);
                                    } else {
                                        spdlog::error("Cannot start: tracker script not found.");
                                    }
                                }
                            }

                            if (!s_trackerErrorMsg.empty()) {
                                ImGui::SeparatorText("Tracker Crash Report");
                                ImGui::PushStyleColor(ImGuiCol_Text, {1.f, 0.4f, 0.4f, 1.f});
                                ImGui::TextWrapped("%s", s_trackerErrorMsg.c_str());
                                ImGui::PopStyleColor();
                            }

                            ImGui::SeparatorText("Inference Resolution");
                            ImGui::TextDisabled("Lower = faster fps, less accuracy");
                            ImGui::SliderInt("Width##iw",  &s_inferW, 160, 1280, "%d px");
                            ImGui::SliderInt("Height##ih", &s_inferH, 90,  720,  "%d px");

                            ImGui::SeparatorText("Webcam Device");
                            ImGui::SliderInt("Camera Index##camidx", &s_cameraIndex, 0, 5, "%d");

                            ImGui::SeparatorText("Visibility Gate (upper-body)");
                            ImGui::SliderFloat("Min vis##pv", &s_posVisThr, 0.f, 1.f, "%.2f");

                            if (ImGui::Button("Apply & Save Settings", {-1, 0})) {
                                saveSettings();
                                spdlog::info("Settings saved. Restart the tracker process to apply webcam device or resolution changes.");
                            }
                            ImGui::EndTabItem();
                        }

                        // ── Smoothing tab ─────────────────────────────────────
                        if (ImGui::BeginTabItem("Smoothing")) {
                            ImGui::TextDisabled("min_cutoff: base smoothing (lower = smoother)");
                            ImGui::TextDisabled("beta: speed adaptation (higher = more responsive)");
                            ImGui::Spacing();
                            bool dirty = false;
                            ImGui::SeparatorText("Face / Head");
                            dirty |= ImGui::SliderFloat("Cutoff##fc", &s_faceCutoff, 0.1f, 10.f, "%.2f");
                            dirty |= ImGui::SliderFloat("Beta##fb",   &s_faceBeta,   0.0f, 1.0f, "%.3f");
                            ImGui::SeparatorText("Head Euler Angles");
                            dirty |= ImGui::SliderFloat("Cutoff##ec", &s_eulerCutoff, 0.1f, 10.f, "%.2f");
                            dirty |= ImGui::SliderFloat("Beta##eb",   &s_eulerBeta,   0.0f, 1.0f, "%.3f");
                            ImGui::SeparatorText("Hands");
                            dirty |= ImGui::SliderFloat("Cutoff##hc", &s_handCutoff, 0.1f, 10.f, "%.2f");
                            dirty |= ImGui::SliderFloat("Beta##hb",   &s_handBeta,   0.0f, 1.0f, "%.3f");
                            if (dirty) saveSettings();
                            ImGui::Spacing();
                            ImGui::Checkbox("Draw Skeleton Lines", &s_drawSkeletonLines);
                            ImGui::SameLine();
                            ImGui::Checkbox("Draw Joint Nodes", &s_drawJointNodes);
                            ImGui::SameLine();
                            ImGui::Checkbox("Stream Mode", &s_streamMode);
                            if (ImGui::Button("Reset to v4.0 defaults")) {
                                s_faceCutoff = 1.0f; s_faceBeta   = 0.30f;
                                s_eulerCutoff= 2.0f; s_eulerBeta  = 0.40f;
                                s_handCutoff = 1.5f; s_handBeta   = 0.20f;
                                saveSettings();
                            }
                            ImGui::EndTabItem();
                        }

                        // ── Stickman tab ──────────────────────────────────────
                        if (ImGui::BeginTabItem("Stickman")) {
                            ImGui::SeparatorText("Geometry");
                            ImGui::Checkbox("Face Mesh  [M]",  &stickmanRenderer.drawFaceMesh_);
                            ImGui::Checkbox("Hand Bones  [B]", &stickmanRenderer.drawBones_);
                            ImGui::Checkbox("Points  [P]",     &stickmanRenderer.drawPoints_);
                            ImGui::SeparatorText("Debug");
                            ImGui::Checkbox("Depth Viz  [F]", &stickmanRenderer.depthViz_);
                            if (stickmanRenderer.depthViz_)
                                ImGui::TextColored({1,0.4f,0.4f,1}, "Red=near  Green=mid  Blue=far");
                            ImGui::SeparatorText("Sizes");
                            ImGui::SliderFloat("Point size##ps", &stickmanRenderer.pointSize_, 1.f, 20.f, "%.1f px");
                            ImGui::SliderFloat("Line width##lw", &stickmanRenderer.lineWidth_,  0.5f, 8.f, "%.1f px");
                            ImGui::EndTabItem();
                        }

                        // ── Studio tab ────────────────────────────────────────
                        if (ImGui::BeginTabItem("Studio")) {
                            if (studio.renderUI()) {
                                saveSettings();
                            }
                            ImGui::Separator();
                            if (ImGui::Checkbox("Desktop Screen-Reactive Lighting", &s_screenReactiveLighting)) {
                                saveSettings();
                            }
                            ImGui::TextDisabled("Samples the desktop screen in real-time to adjust lighting on the character.");
                            if (s_screenReactiveLighting) {
                                ImGui::Indent();
                                if (ImGui::Checkbox("Affects Ambient Light##ambs", &s_screenAffectsAmbient)) saveSettings();
                                if (ImGui::Checkbox("Affects Key Light##keys", &s_screenAffectsKey)) saveSettings();
                                if (ImGui::Checkbox("Affects Fill Light##fills", &s_screenAffectsFill)) saveSettings();
                                if (ImGui::SliderFloat("Brightness Boost##boost", &s_screenStrength, 0.1f, 3.0f, "%.2fx")) saveSettings();
                                if (ImGui::SliderFloat("Min Brightness##minbr", &s_screenMinBrightness, 0.0f, 1.0f, "%.2f")) saveSettings();
                                ImGui::Unindent();
                            }
                            
                            ImGui::SeparatorText("Audio Explosion Trigger");
                            float audioVal = g_audioLevel.load();
                            ImGui::ProgressBar(audioVal, ImVec2(-1, 0), "");
                            ImGui::TextDisabled("Real-time loopback level (Explosions trigger above 0.45).");

                            ImGui::EndTabItem();
                        }

                        // ── OBS Stream tab ────────────────────────────────────
                        if (ImGui::BeginTabItem("OBS Stream")) {
                            ImGui::SeparatorText("Stream Controls");
                            if (ImGui::Checkbox("Stream Mode (Hide UI/Gizmos)", &s_streamMode)) {
                                saveSettings();
                            }
                            ImGui::TextDisabled("Hides status overlays, warning panels, bone nodes, and gizmos.");
                            ImGui::TextDisabled("Press [H] to toggle this Settings window.");

                            if (ImGui::Checkbox("Virtual Viewport (Render to Offscreen Window)", &s_virtualViewport)) {
                                saveSettings();
                            }
                            ImGui::TextDisabled("Renders model to a 1080p offscreen buffer and shows it inside a floating UI window.");
                            ImGui::Spacing();
                            if (ImGui::Checkbox("Enable Mouse Click-Through", &s_clickThrough)) {
                                saveSettings();
                            }
                            ImGui::TextDisabled("Passes mouse clicks on transparent/empty space to software behind.");

                            ImGui::SeparatorText("Window Rendering Options");
                            if (ImGui::Checkbox("Borderless Window", &s_borderlessWindow)) {
                                glfwSetWindowAttrib(window, GLFW_DECORATED, s_borderlessWindow ? GLFW_FALSE : GLFW_TRUE);
                                saveSettings();
                            }
                            ImGui::SameLine();
                            if (ImGui::Checkbox("Always on Top", &s_alwaysOnTop)) {
                                glfwSetWindowAttrib(window, GLFW_FLOATING, s_alwaysOnTop ? GLFW_TRUE : GLFW_FALSE);
                                saveSettings();
                            }

                            ImGui::SeparatorText("Performance & Framerate");
                            if (ImGui::SliderInt("FPS Limit##fpslim", &s_fpsLimit, 10, 240, s_fpsLimit > 0 ? "%d FPS" : "Unlimited")) {
                                saveSettings();
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Unlimited##unl")) {
                                s_fpsLimit = -1;
                                saveSettings();
                            }

                            ImGui::SeparatorText("OBS Presets");
                            if (ImGui::Button("Preset: Chroma Green Screen", {-1, 0})) {
                                auto& sp = studio.params();
                                sp.bgMode = vts::renderer::BackgroundMode::SolidColor;
                                sp.bgColor = glm::vec4(0.f, 1.f, 0.f, 1.f);
                                sp.showGrid = false;
                                s_streamMode = true;
                                studio.rebuildGrid();
                                saveSettings();
                                spdlog::info("[OBS] Applied Chroma Green Screen preset.");
                            }
                            if (ImGui::Button("Preset: Chroma Blue Screen", {-1, 0})) {
                                auto& sp = studio.params();
                                sp.bgMode = vts::renderer::BackgroundMode::SolidColor;
                                sp.bgColor = glm::vec4(0.f, 0.f, 1.f, 1.f);
                                sp.showGrid = false;
                                s_streamMode = true;
                                studio.rebuildGrid();
                                saveSettings();
                                spdlog::info("[OBS] Applied Chroma Blue Screen preset.");
                            }
                            if (ImGui::Button("Preset: Transparent Background", {-1, 0})) {
                                auto& sp = studio.params();
                                sp.bgMode = vts::renderer::BackgroundMode::SolidColor;
                                sp.bgColor = glm::vec4(0.f, 0.f, 0.f, 0.f);
                                sp.showGrid = false;
                                s_streamMode = true;
                                studio.rebuildGrid();
                                saveSettings();
                                spdlog::info("[OBS] Applied Transparent Background preset.");
                            }
                            ImGui::TextDisabled("Note: Transparent background requires Window/Game Capture");
                            ImGui::TextDisabled("with alpha/transparency enabled in OBS.");
                            ImGui::EndTabItem();
                        }

                        // ── Rig tab ───────────────────────────────────────────
                        if (ImGui::BeginTabItem("Rig")) {
                            if (!avatarRenderer.hasModel()) {
                                ImGui::TextDisabled("Load a model to inspect its rig.");
                            } else {
                                const auto& bones = avatarRenderer.getBoneList();
                                ImGui::Text("%d bones loaded", (int)bones.size());
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Clear all overrides")) {
                                    avatarRenderer.clearAllBoneOverrides();
                                    s_boneOverrides.clear();
                                }

                                static char boneFilter[128] = "";
                                ImGui::InputText("Filter##bonefilter", boneFilter, IM_ARRAYSIZE(boneFilter));

                                ImGui::Checkbox("Draw Skeleton Lines", &s_drawSkeletonLines);
                                ImGui::SameLine();
                                ImGui::Checkbox("Draw Joint Nodes", &s_drawJointNodes);

                                ImGui::Separator();
                                ImGui::BeginChild("BoneList", {0, 300}, true);
                                for (int i = 0; i < (int)bones.size(); ++i) {
                                    const auto& b = bones[i];
                                    
                                    if (boneFilter[0] != '\0') {
                                        std::string nameLower = b.name;
                                        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                                        std::string filterLower = boneFilter;
                                        std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);
                                        if (nameLower.find(filterLower) == std::string::npos) {
                                            continue;
                                        }
                                    }

                                    ImGui::PushID(i);
                                    
                                    // Highlight if selected via 3D viewport clicking
                                    bool isSelected = (b.name == s_selectedBoneName);
                                    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanAvailWidth;
                                    if (isSelected) {
                                        nodeFlags |= ImGuiTreeNodeFlags_Selected;
                                        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                                    }

                                    bool open = ImGui::TreeNodeEx(b.name.c_str(), nodeFlags);
                                    if (ImGui::IsItemClicked()) {
                                        s_selectedBoneName = b.name;
                                    }

                                    if (open) {
                                        // Show world position
                                        glm::vec3 pos = glm::vec3(b.world[3]);
                                        ImGui::TextDisabled("  World: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);

                                        auto& state = s_boneOverrides[b.name];
                                        bool overridden = state.active;
                                        if (ImGui::Checkbox("Override Transform", &overridden)) {
                                            state.active = overridden;
                                            if (!overridden) {
                                                avatarRenderer.clearBoneOverride(b.name);
                                            } else {
                                                state.rotation = glm::vec3(0.f);
                                                state.translation = glm::vec3(0.f);
                                                avatarRenderer.setBoneOverride(b.name, b.local);
                                            }
                                        }

                                        if (state.active) {
                                            bool dirty = false;
                                            ImGui::Text("Rotation Offset (Pitch/Yaw/Roll):");
                                            dirty |= ImGui::SliderFloat3("Rot##rot", &state.rotation.x, -180.f, 180.f, "%.1f");
                                            ImGui::Text("Translation Offset (X/Y/Z):");
                                            dirty |= ImGui::DragFloat3("Trans##trans", &state.translation.x, 0.005f, -1.f, 1.f, "%.3f");

                                            if (dirty) {
                                                glm::vec3 translation = glm::vec3(b.local[3]) + state.translation;
                                                glm::vec3 scale(
                                                    glm::length(glm::vec3(b.local[0])),
                                                    glm::length(glm::vec3(b.local[1])),
                                                    glm::length(glm::vec3(b.local[2]))
                                                );
                                                glm::mat3 rotOnlyMat;
                                                rotOnlyMat[0] = scale.x > 0.0001f ? glm::vec3(b.local[0]) / scale.x : glm::vec3(1,0,0);
                                                rotOnlyMat[1] = scale.y > 0.0001f ? glm::vec3(b.local[1]) / scale.y : glm::vec3(0,1,0);
                                                rotOnlyMat[2] = scale.z > 0.0001f ? glm::vec3(b.local[2]) / scale.z : glm::vec3(0,0,1);
                                                glm::quat baseRot = glm::quat_cast(rotOnlyMat);

                                                glm::quat offsetRot = glm::quat(glm::vec3(glm::radians(state.rotation.x), glm::radians(state.rotation.y), glm::radians(state.rotation.z)));
                                                glm::quat finalRot = baseRot * offsetRot;

                                                glm::mat4 T = glm::translate(glm::mat4(1.f), translation);
                                                glm::mat4 R = glm::mat4_cast(finalRot);
                                                glm::mat4 S = glm::scale(glm::mat4(1.f), scale);
                                                glm::mat4 over = T * R * S;

                                                avatarRenderer.setBoneOverride(b.name, over);
                                            }
                                        }

                                        if (ImGui::SmallButton("Clear override##cb")) {
                                            state.active = false;
                                            state.rotation = glm::vec3(0.f);
                                            state.translation = glm::vec3(0.f);
                                            avatarRenderer.clearBoneOverride(b.name);
                                        }

                                        ImGui::TreePop();
                                    }
                                    ImGui::PopID();
                                }
                                ImGui::EndChild();
                            }
                            ImGui::EndTabItem();
                        }

                        // ── Camera tab ────────────────────────────────────────
                        if (ImGui::BeginTabItem("Camera")) {
                            bool isOrbit = camera.mode() == vts::renderer::CameraMode::Orbit;
                            ImGui::TextUnformatted(isOrbit ? "Mode: Orbit" : "Mode: Fly");
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Switch  [Tab]")) camera.toggleMode();

                            ImGui::SeparatorText("Orbit");
                            ImGui::TextDisabled("LMB drag = rotate   RMB drag = pan   Scroll = zoom");
                            if (ImGui::Button("Re-centre on face  [R]") && !stickmanRenderer.frozen_) {
                                auto frame = trackingState.get();
                                camera.setOrbitTarget(frame.getRootPosition());
                            }

                            ImGui::SeparatorText("Fly");
                            ImGui::TextDisabled("Hold RMB + WASD/QE to move   Shift = sprint   Scroll = speed");

                            ImGui::SeparatorText("Freeze");
                            bool frozen = stickmanRenderer.frozen_;
                            if (ImGui::Checkbox("Freeze pose  [Space]", &frozen))
                                stickmanRenderer.frozen_ = frozen;

                            ImGui::SeparatorText("Projection");
                            ImGui::SliderFloat("FOV",        &camera.fovDeg,    20.f, 110.f, "%.0f°");
                            ImGui::SliderFloat("Near plane", &camera.nearPlane, 0.001f, 1.f, "%.3f m");
                            ImGui::SliderFloat("Far plane",  &camera.farPlane,  5.f, 100.f,  "%.0f m");

                            ImGui::SeparatorText("Fly speed");
                            ImGui::SliderFloat("Base speed", &camera.flySpeed_,  0.1f, 20.f, "%.1f m/s");
                            ImGui::SliderFloat("Mouse sens", &camera.mouseSens_, 0.01f, 1.f, "%.2f");
                            ImGui::EndTabItem();
                        }

                        ImGui::EndTabBar();
                    }
                }
                ImGui::End();

                if (s_virtualViewport) {
                    rhi->endRenderPass();
                    rhi->beginRenderPass(nullptr, nullptr);
                    vts::rhi::Viewport scViewport{0, 0, float(g_width), float(g_height), 0, 1};
                    vts::rhi::Scissor  scScissor {0, 0, g_width, g_height};
                    rhi->setViewport(scViewport);
                    rhi->setScissor(scScissor);
                }

                // Render Virtual Viewport Window
                if (s_virtualViewport) {
                    ImGui::Begin("Virtual Viewport", nullptr, ImGuiWindowFlags_NoScrollbar);
                    s_viewportPos = ImGui::GetWindowPos();
                    s_viewportSize = ImGui::GetContentRegionAvail();
                    s_viewportHovered = ImGui::IsWindowHovered();
                    ImGui::Image((ImTextureID)(uintptr_t)g_virtualColorTex->id, s_viewportSize, ImVec2(0, 1), ImVec2(1, 0));
                    ImGui::End();
                }

                rhi->imguiRender();
                rhi->endRenderPass();
                rhi->endFrame();
            }
        }

        spdlog::info("Shutting down…");
        receiver.stop();
        rhi->shutdown();

    } catch (const std::exception& e) {
        spdlog::critical("Exception: {}", e.what());
    }

#ifdef _WIN32
    if (pythonProcessHandle != NULL || pythonPid != 0) killProcess(pythonProcessHandle, pythonPid);
#endif
    glfwDestroyWindow(window);
    glfwTerminate();
    ix::uninitNetSystem();
    spdlog::info("VTuberStudio exited");
    return 0;
}