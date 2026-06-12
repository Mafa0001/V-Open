// src/renderer/ModelImporter.cpp
#include "renderer/ModelImporter.h"
#include <spdlog/spdlog.h>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#endif

namespace vts::renderer {

std::optional<std::string> browseForModelFile() {
#ifdef _WIN32
    char szFile[MAX_PATH] = {};

    OPENFILENAMEA ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = nullptr;
    ofn.lpstrFilter  =
        "3D Models (*.vrm;*.glb;*.gltf)\0*.vrm;*.glb;*.gltf\0"
        "VRM Files (*.vrm)\0*.vrm\0"
        "glTF Binary (*.glb)\0*.glb\0"
        "glTF ASCII (*.gltf)\0*.gltf\0"
        "All Files (*.*)\0*.*\0";
    ofn.lpstrFile    = szFile;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrTitle   = "Open 3D Model";
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        spdlog::info("[ModelImporter] Selected: {}", szFile);
        return std::string(szFile);
    }

    DWORD err = CommDlgExtendedError();
    if (err != 0)
        spdlog::warn("[ModelImporter] Dialog error: 0x{:X}", err);
    // err == 0 means user cancelled — that's fine
    return std::nullopt;

#else
    spdlog::warn("[ModelImporter] Native file dialog not implemented on this platform.");
    return std::nullopt;
#endif
}

} // namespace vts::renderer
