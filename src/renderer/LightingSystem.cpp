// src/renderer/LightingSystem.cpp
#include "renderer/LightingSystem.h"
#include <spdlog/spdlog.h>

namespace vts::renderer {

struct DirLightGPU {
    glm::vec4 direction;
    glm::vec4 color;
};

struct LightingUBO {
    glm::vec4 ambient;
    DirLightGPU keyLight;
    DirLightGPU fillLight;
    DirLightGPU rimLight;
    // Future-proofing fields for shadows/RT
    int       enableRayTracing;
    int       enableShadows;
    float     shadowBias;
    int       _pad;
    // Explosion point light source
    glm::vec4 explosionPos;
    glm::vec4 explosionColor;
    // Character bone positions for ray-traced shadows
    glm::vec4 shadowHipPos;
    glm::vec4 shadowChestPos;
    glm::vec4 shadowHeadPos;
};

LightingSystem::LightingSystem(rhi::IRHIContext& rhi) : rhi_(rhi) {}
LightingSystem::~LightingSystem() = default;

void LightingSystem::init() {
    rhi::BufferDesc lightingUbDesc;
    lightingUbDesc.size = sizeof(LightingUBO);
    lightingUbDesc.usage = rhi::BufferUsage::Uniform;
    lightingUbDesc.memory = rhi::MemoryType::HostVisible;
    lightingUB_ = rhi_.createBuffer(lightingUbDesc);
    
    update();
    spdlog::info("[LightingSystem] Initialized UBO and options");
}

void LightingSystem::update() {
    if (!lightingUB_) return;

    LightingUBO ubo{};
    ubo.ambient = glm::vec4(params_.ambientColor * params_.ambientIntensity, 1.f);

    auto fillDirLight = [&](DirLightGPU& dest, const DirectionalLight& src) {
        dest.direction = glm::vec4(src.direction, 0.f);
        dest.color     = glm::vec4(src.enabled ? (src.color * src.intensity) : glm::vec3(0.f), 1.f);
    };
    fillDirLight(ubo.keyLight,  params_.keyLight);
    fillDirLight(ubo.fillLight, params_.fillLight);
    fillDirLight(ubo.rimLight,  params_.rimLight);

    ubo.enableRayTracing = params_.enableRayTracing ? 1 : 0;
    ubo.enableShadows    = params_.enableShadows    ? 1 : 0;
    ubo.shadowBias       = params_.shadowBias;

    ubo.explosionPos   = params_.explosionPos;
    ubo.explosionColor = params_.explosionColor;

    ubo.shadowHipPos   = glm::vec4(params_.shadowHipPos, params_.shadowHipRadius);
    ubo.shadowChestPos = glm::vec4(params_.shadowChestPos, params_.shadowChestRadius);
    ubo.shadowHeadPos  = glm::vec4(params_.shadowHeadPos, params_.shadowHeadRadius);

    rhi_.updateBuffer(lightingUB_, &ubo, sizeof(LightingUBO));
}

} // namespace vts::renderer
