// src/renderer/LightingSystem.h
#pragma once
#include "rhi/RHI.h"
#include <glm/glm.hpp>

namespace vts::renderer {

/// Describes a single directional light source.
struct DirectionalLight {
    glm::vec3 direction = glm::normalize(glm::vec3(0.5f, -1.0f, -0.8f));
    glm::vec3 color     = {1.0f, 0.97f, 0.90f};   // warm white
    float     intensity = 1.0f;
    bool      enabled   = true;
    bool      castShadows = false;                // future-proofing for shadows
};

/// All lighting system parameters.
struct LightingParams {
    // Ambient
    glm::vec3 ambientColor     = {0.18f, 0.18f, 0.22f};
    float     ambientIntensity = 1.0f;

    // Directional lights
    DirectionalLight keyLight;
    DirectionalLight fillLight = {
        glm::normalize(glm::vec3(-0.6f, -0.5f, 0.8f)),
        {0.55f, 0.65f, 0.90f},   // cool blue fill
        0.45f, true, false
    };
    DirectionalLight rimLight = {
        glm::normalize(glm::vec3(0.0f, -0.3f, 1.0f)),
        {1.0f, 0.90f, 0.75f},    // warm rim
        0.35f, false, false
    };

    // Future-proofing for shadows & ray tracing
    bool  enableRayTracing = false;
    bool  enableShadows    = true;
    float shadowBias       = 0.005f;
    int   shadowMapRes     = 2048;

    // Explosion point light source
    glm::vec4 explosionPos   = {0.f, 0.f, 0.f, 1.5f};   // xyz = position, w = radius
    glm::vec4 explosionColor = {1.f, 0.4f, 0.05f, 0.f}; // rgb = color, w = intensity/fade factor

    // Character bone positions for ray-traced shadows
    glm::vec3 shadowHipPos   = {0.f, 0.f, 0.f};
    float     shadowHipRadius = 0.25f;
    glm::vec3 shadowChestPos = {0.f, 0.f, 0.f};
    float     shadowChestRadius = 0.22f;
    glm::vec3 shadowHeadPos  = {0.f, 0.f, 0.f};
    float     shadowHeadRadius = 0.18f;
};

/// LightingSystem class encapsulates UBO creation, updates, and lighting configs.
class LightingSystem {
public:
    explicit LightingSystem(rhi::IRHIContext& rhi);
    ~LightingSystem();

    void init();
    
    /// Re-uploads the lighting UBO parameters to the GPU.
    void update();

    rhi::BufferHandle     getLightingUB() const { return lightingUB_; }
    const LightingParams& params()        const { return params_; }
    LightingParams&       params()              { return params_; }

private:
    rhi::IRHIContext& rhi_;
    LightingParams    params_;
    rhi::BufferHandle lightingUB_;
};

} // namespace vts::renderer
