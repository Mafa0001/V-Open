// src/renderer/ModelImporter.h
#pragma once
#include <optional>
#include <string>

namespace vts::renderer {

/// Opens a native OS file dialog and returns the chosen model path, or
/// std::nullopt if the user cancelled or an error occurred.
/// Filters: *.vrm, *.glb, *.gltf
/// Thread: must be called from the main (rendering) thread on Windows.
std::optional<std::string> browseForModelFile();

} // namespace vts::renderer
