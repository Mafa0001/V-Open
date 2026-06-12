// src/renderer/StudioEnvironment.h
#pragma once
#include "rhi/RHI.h"
#include "renderer/LightingSystem.h"
#include <glm/glm.hpp>

namespace vts::renderer {

/// Background rendering mode.
enum class BackgroundMode {
    SolidColor,
    Gradient,
    Checkerboard,
};

/// All studio scene parameters.
struct StudioParams {
    // Background
    BackgroundMode bgMode      = BackgroundMode::Gradient;
    glm::vec4      bgColor     = {0.08f, 0.08f, 0.10f, 1.0f};   // dark near-black
    glm::vec4      bgColorTop  = {0.06f, 0.06f, 0.14f, 1.0f};   // deep navy
    glm::vec4      bgColorBot  = {0.04f, 0.04f, 0.06f, 1.0f};   // near black

    // Grid / ground plane
    bool  showGrid      = true;
    float gridSpacing   = 0.25f;
    float gridExtent    = 4.0f;
    glm::vec3 gridColor = {0.25f, 0.25f, 0.28f};
    float gridAlpha     = 0.6f;

    // Reactive Background and Lighting
    bool  reactiveBG    = false;
    float reactiveSpeed = 1.5f;
};

/// Studio environment: background, grid, lighting configuration.
class StudioEnvironment {
public:
    explicit StudioEnvironment(rhi::IRHIContext& rhi, LightingSystem& lighting);
    ~StudioEnvironment();

    void init();

    /// Render background (call before any scene geometry).
    void renderBackground(uint32_t viewportW, uint32_t viewportH);

    /// Render the grid overlay (call after scene, before ImGui).
    void renderGrid(const glm::mat4& view, const glm::mat4& proj);

    /// Draw the ImGui settings panel. Returns true if params changed.
    bool renderUI();

    /// Rebuild grid geometry from current parameters.
    void rebuildGrid();

    const StudioParams& params() const { return params_; }
    StudioParams&       params()       { return params_; }

private:
    rhi::IRHIContext& rhi_;
    LightingSystem&   lighting_;
    StudioParams      params_;

    // Background quad
    rhi::PipelineHandle bgPipeline_;
    rhi::BufferHandle   bgVB_;
    rhi::BufferHandle   bgUB_;

    // Grid lines
    rhi::PipelineHandle gridPipeline_;
    rhi::BufferHandle   gridVB_;
    rhi::BufferHandle   gridUB_;
    int                 gridVertexCount_ = 0;
};

} // namespace vts::renderer
