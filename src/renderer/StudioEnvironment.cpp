// src/renderer/StudioEnvironment.cpp
#define GLM_ENABLE_EXPERIMENTAL
#include "renderer/StudioEnvironment.h"

#include <vector>
#include <cmath>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <imgui.h>

namespace vts::renderer {

static std::string loadShaderFile(const std::string& name) {
    std::vector<std::filesystem::path> searchPaths = {
        std::filesystem::current_path() / "shaders" / name,
        std::filesystem::current_path() / ".." / "shaders" / name,
        std::filesystem::path("C:/Users/Marti/CLionProjects/VStudio/shaders") / name
    };
    for (const auto& path : searchPaths) {
        if (std::filesystem::exists(path)) {
            std::ifstream f(path);
            if (f.is_open()) {
                std::stringstream ss;
                ss << f.rdbuf();
                return ss.str();
            }
        }
    }
    spdlog::error("Failed to load shader: {}", name);
    return "";
}

struct BgUBO {
    int  bgMode;
    int  _pad[3];
    glm::vec4 bgColor;
    glm::vec4 bgColorTop;
    glm::vec4 bgColorBot;
};

struct GridUBO {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 gridColor;
    float     gridAlpha;
    float     _pad[3];
};

StudioEnvironment::StudioEnvironment(rhi::IRHIContext& rhi, LightingSystem& lighting)
    : rhi_(rhi), lighting_(lighting) {}
StudioEnvironment::~StudioEnvironment() = default;

void StudioEnvironment::init() {
    // ── Shaders ─────────────────────────────────────────────────────────────
    rhi::ShaderDesc bgVsDesc{ rhi::ShaderStage::Vertex, loadShaderFile("Background.vert") };
    rhi::ShaderDesc bgFsDesc{ rhi::ShaderStage::Fragment, loadShaderFile("Background.frag") };
    auto bgVS = rhi_.createShader(bgVsDesc);
    auto bgFS = rhi_.createShader(bgFsDesc);

    rhi::ShaderDesc gridVsDesc{ rhi::ShaderStage::Vertex, loadShaderFile("Grid.vert") };
    rhi::ShaderDesc gridFsDesc{ rhi::ShaderStage::Fragment, loadShaderFile("Grid.frag") };
    auto gridVS = rhi_.createShader(gridVsDesc);
    auto gridFS = rhi_.createShader(gridFsDesc);

    // ── Background quad pipeline & buffer ────────────────────────────────────
    rhi::VertexAttribute bgPosAttr{ 0, 0, 0, 2 * sizeof(float), rhi::PixelFormat::RG32F };

    rhi::PipelineDesc bgPipDesc;
    bgPipDesc.vertexShader = bgVS;
    bgPipDesc.fragmentShader = bgFS;
    bgPipDesc.vertexAttributes = { bgPosAttr };
    bgPipDesc.raster.cullMode = rhi::CullMode::None;
    bgPipDesc.depth.depthTest = true;
    bgPipDesc.depth.depthWrite = false;
    bgPipDesc.blend.enabled = false;
    bgPipDesc.topology = rhi::PrimitiveTopology::TriangleList;
    bgPipeline_ = rhi_.createPipeline(bgPipDesc);

    float bgVerts[] = { -1,-1, 1,-1, -1,1, 1,-1, 1,1, -1,1 };
    rhi::BufferDesc bgVbDesc;
    bgVbDesc.size = sizeof(bgVerts);
    bgVbDesc.usage = rhi::BufferUsage::Vertex;
    bgVbDesc.memory = rhi::MemoryType::DeviceLocal;
    bgVbDesc.initialData = bgVerts;
    bgVB_ = rhi_.createBuffer(bgVbDesc);

    rhi::BufferDesc bgUbDesc;
    bgUbDesc.size = sizeof(BgUBO);
    bgUbDesc.usage = rhi::BufferUsage::Uniform;
    bgUbDesc.memory = rhi::MemoryType::HostVisible;
    bgUB_ = rhi_.createBuffer(bgUbDesc);

    // ── Grid pipeline & buffer ───────────────────────────────────────────────
    rhi::VertexAttribute gridPosAttr{ 0, 0, 0, 3 * sizeof(float), rhi::PixelFormat::RGB32F };

    rhi::PipelineDesc gridPipDesc;
    gridPipDesc.vertexShader = gridVS;
    gridPipDesc.fragmentShader = gridFS;
    gridPipDesc.vertexAttributes = { gridPosAttr };
    gridPipDesc.raster.cullMode = rhi::CullMode::None;
    gridPipDesc.raster.polyMode = rhi::PolygonMode::Line;
    gridPipDesc.depth.depthTest = true;
    gridPipDesc.depth.depthWrite = false;
    gridPipDesc.blend.enabled = true;
    gridPipDesc.blend.srcFactor = rhi::BlendFactor::SrcAlpha;
    gridPipDesc.blend.dstFactor = rhi::BlendFactor::OneMinusSrcAlpha;
    gridPipDesc.topology = rhi::PrimitiveTopology::LineList;
    gridPipeline_ = rhi_.createPipeline(gridPipDesc);

    rhi::BufferDesc gridUbDesc;
    gridUbDesc.size = sizeof(GridUBO);
    gridUbDesc.usage = rhi::BufferUsage::Uniform;
    gridUbDesc.memory = rhi::MemoryType::HostVisible;
    gridUB_ = rhi_.createBuffer(gridUbDesc);

    rebuildGrid();
    spdlog::info("[StudioEnvironment] Initialized and bound to lighting system");
}

void StudioEnvironment::rebuildGrid() {
    std::vector<float> verts;
    float ext  = params_.gridExtent;
    float step = params_.gridSpacing;

    for (float v = -ext; v <= ext + 1e-4f; v += step) {
        // Along X
        verts.insert(verts.end(), {-ext, 0.f, v,  ext, 0.f, v});
        // Along Z
        verts.insert(verts.end(), {v, 0.f, -ext,  v, 0.f, ext});
    }

    gridVertexCount_ = (int)(verts.size() / 3);
    if (!gridVB_) {
        rhi::BufferDesc vbDesc;
        vbDesc.size = verts.size() * sizeof(float);
        vbDesc.usage = rhi::BufferUsage::Vertex;
        vbDesc.memory = rhi::MemoryType::HostVisible;
        vbDesc.initialData = verts.data();
        gridVB_ = rhi_.createBuffer(vbDesc);
    } else {
        rhi_.updateBuffer(gridVB_, verts.data(), verts.size() * sizeof(float));
    }
}

void StudioEnvironment::renderBackground(uint32_t /*viewportW*/, uint32_t /*viewportH*/) {
    if (!bgPipeline_ || !bgVB_ || !bgUB_) return;

    BgUBO ubo{};
    ubo.bgMode = (int)params_.bgMode;
    ubo.bgColor = params_.bgColor;
    ubo.bgColorTop = params_.bgColorTop;
    ubo.bgColorBot = params_.bgColorBot;
    rhi_.updateBuffer(bgUB_, &ubo, sizeof(BgUBO));

    rhi::DrawCall dc;
    dc.pipeline = bgPipeline_;
    dc.vertexBuffer = bgVB_;
    dc.uniformBuffer = bgUB_;
    dc.vertexCount = 6;
    rhi_.submit(dc);
}

void StudioEnvironment::renderGrid(const glm::mat4& view, const glm::mat4& proj) {
    if (!params_.showGrid || !gridPipeline_ || !gridVB_ || !gridUB_) return;

    GridUBO ubo{};
    ubo.view = view;
    ubo.proj = proj;
    ubo.gridColor = glm::vec4(params_.gridColor, 1.0f);
    ubo.gridAlpha = params_.gridAlpha;
    rhi_.updateBuffer(gridUB_, &ubo, sizeof(GridUBO));

    rhi::DrawCall dc;
    dc.pipeline = gridPipeline_;
    dc.vertexBuffer = gridVB_;
    dc.uniformBuffer = gridUB_;
    dc.vertexCount = gridVertexCount_;
    rhi_.submit(dc);
}

bool StudioEnvironment::renderUI() {
    bool changed = false;

    // Background settings
    if (ImGui::CollapsingHeader("Background", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* modes[] = {"Solid Colour", "Gradient", "Checkerboard"};
        int m = (int)params_.bgMode;
        if (ImGui::Combo("Mode##bg", &m, modes, 3)) { params_.bgMode = (BackgroundMode)m; changed = true; }
        if (params_.bgMode == BackgroundMode::SolidColor) {
            changed |= ImGui::ColorEdit3("Colour##bgc", &params_.bgColor.x);
        } else if (params_.bgMode == BackgroundMode::Gradient) {
            changed |= ImGui::ColorEdit3("Top##bgt",    &params_.bgColorTop.x);
            changed |= ImGui::ColorEdit3("Bottom##bgb", &params_.bgColorBot.x);
        }
        ImGui::Separator();
        changed |= ImGui::Checkbox("Reactive Background & Lighting##reactivebg", &params_.reactiveBG);
        if (params_.reactiveBG) {
            changed |= ImGui::SliderFloat("Reaction Speed##reactivesp", &params_.reactiveSpeed, 0.1f, 5.0f, "%.1f");
            ImGui::TextDisabled("Background and lights shift color with head movements.");
        }
    }

    // Grid settings
    if (ImGui::CollapsingHeader("Grid", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox("Show Grid##g", &params_.showGrid);
        if (params_.showGrid) {
            bool gridDirty = false;
            gridDirty |= ImGui::SliderFloat("Spacing##gs", &params_.gridSpacing, 0.05f, 1.0f, "%.2f m");
            gridDirty |= ImGui::SliderFloat("Extent##ge",  &params_.gridExtent,  1.0f,  10.f, "%.1f m");
            if (gridDirty) { rebuildGrid(); changed = true; }
            changed |= ImGui::ColorEdit3("Grid Colour##gc", &params_.gridColor.x);
            changed |= ImGui::SliderFloat("Opacity##ga",    &params_.gridAlpha,   0.f, 1.f, "%.2f");
        }
    }

    // Light settings (from abstract LightingSystem)
    auto& lp = lighting_.params();
    bool lightChanged = false;

    if (ImGui::CollapsingHeader("Ambient Light")) {
        lightChanged |= ImGui::ColorEdit3("Colour##amb",    &lp.ambientColor.x);
        lightChanged |= ImGui::SliderFloat("Intensity##ai", &lp.ambientIntensity, 0.f, 2.f, "%.2f");
    }

    auto lightUI = [&](const char* label, DirectionalLight& L) {
        ImGui::PushID(label);
        if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
            lightChanged |= ImGui::Checkbox("Enabled##le", &L.enabled);
            if (L.enabled) {
                float az = glm::degrees(atan2f(L.direction.x, L.direction.z));
                float el = glm::degrees(asinf(-L.direction.y));
                bool d = false;
                d |= ImGui::SliderFloat("Azimuth##la",    &az, -180.f, 180.f, "%.0f°");
                d |= ImGui::SliderFloat("Elevation##le2", &el,  -90.f,  90.f, "%.0f°");
                if (d) {
                    float azR = glm::radians(az), elR = glm::radians(el);
                    L.direction = glm::normalize(glm::vec3(
                        sinf(azR)*cosf(elR), -sinf(elR), cosf(azR)*cosf(elR)));
                    lightChanged = true;
                }
                lightChanged |= ImGui::ColorEdit3("Colour##lc",     &L.color.x);
                lightChanged |= ImGui::SliderFloat("Intensity##li",  &L.intensity, 0.f, 3.f, "%.2f");
                lightChanged |= ImGui::Checkbox("Cast Shadows##ls", &L.castShadows);
            }
        }
        ImGui::PopID();
    };

    lightUI("Key Light",  lp.keyLight);
    lightUI("Fill Light", lp.fillLight);
    lightUI("Rim Light",  lp.rimLight);

    // Advanced RT & Shadow Settings (future-proofing)
    if (ImGui::CollapsingHeader("Ray Tracing & Shadows (Experimental)")) {
        lightChanged |= ImGui::Checkbox("Enable Ray Tracing##rt", &lp.enableRayTracing);
        lightChanged |= ImGui::Checkbox("Enable Shadows##sd",      &lp.enableShadows);
        if (lp.enableShadows) {
            lightChanged |= ImGui::SliderFloat("Shadow Bias##sb", &lp.shadowBias, 0.0001f, 0.05f, "%.4f");
            lightChanged |= ImGui::Combo("Shadow Map Res##sr", &lp.shadowMapRes, "1024\0" "2048\0" "4096\0");
        }
    }

    if (lightChanged) {
        lighting_.update();
        changed = true;
    }

    return changed;
}

} // namespace vts::renderer
