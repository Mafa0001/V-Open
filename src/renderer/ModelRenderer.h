// src/renderer/ModelRenderer.h
#pragma once

#include "rhi/RHI.h"
#include "tracking/TrackingData.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace vts::renderer {

// ─── VRM bone enum ─────────────────────────────────────────────────────────────
enum class VRMBone {
    Hips, Spine, Chest, UpperChest, Neck, Head,
    LeftEye, RightEye,
    LeftShoulder, LeftUpperArm, LeftLowerArm, LeftHand,
    RightShoulder, RightUpperArm, RightLowerArm, RightHand,
    // Fingers (left)
    LeftThumbProximal, LeftThumbIntermediate, LeftThumbDistal,
    LeftIndexProximal,  LeftIndexIntermediate,  LeftIndexDistal,
    LeftMiddleProximal, LeftMiddleIntermediate, LeftMiddleDistal,
    LeftRingProximal,   LeftRingIntermediate,   LeftRingDistal,
    LeftLittleProximal, LeftLittleIntermediate, LeftLittleDistal,
    // Fingers (right)
    RightThumbProximal, RightThumbIntermediate, RightThumbDistal,
    RightIndexProximal,  RightIndexIntermediate,  RightIndexDistal,
    RightMiddleProximal, RightMiddleIntermediate, RightMiddleDistal,
    RightRingProximal,   RightRingIntermediate,   RightRingDistal,
    RightLittleProximal, RightLittleIntermediate, RightLittleDistal,
    Jaw,
    Count
};

struct BlendShapeMapping {
    std::string trackerName;  // e.g. "eyeBlinkLeft"
    std::string modelName;    // e.g. "eye_blink_L"
};

struct BoneTransform {
    glm::mat4 local     {1.f};   // current local (tracking-driven, reset each frame)
    glm::mat4 restLocal {1.f};   // rest pose local (never modified after load)
    glm::mat4 world     {1.f};
    glm::mat4 invBind   {1.f};
    int       parentIdx = -1;
    std::string name;
};

struct BlendShapeTarget {
    std::string name;
    std::vector<uint32_t>   indices;    // vertex indices affected
    std::vector<glm::vec3>  deltas;     // position deltas
};

struct ModelNode {
    int parentIndex = -1;
    std::vector<int> children;
    glm::mat4 localTransform{1.f};
    int meshIndex = -1;
    int skinIndex = -1;
    std::string name;
};

struct PrimitiveMorphTarget {
    int targetIndex;
    std::vector<glm::vec3> positionDeltas;
};

struct MeshPrimitive {
    rhi::BufferHandle   vertexBuffer;
    rhi::BufferHandle   indexBuffer;
    rhi::PipelineHandle pipeline;
    uint32_t            indexCount  = 0;
    uint32_t            vertexCount = 0;
    int                 materialIdx = -1;
    int                 skinIdx     = -1;   // index into model.skins
    int                 meshIdx     = -1;   // GLTF mesh index
    std::vector<float>  baseVertexData;
    std::vector<PrimitiveMorphTarget> morphTargets;
};

// ─── ModelRenderer ─────────────────────────────────────────────────────────────
class ModelRenderer {
public:
    explicit ModelRenderer(rhi::IRHIContext& rhi);
    ~ModelRenderer();

    // ── Model loading ─────────────────────────────────────────────────────────
    bool        loadModel(const std::string& filepath);
    void        unloadModel();
    bool        hasModel()     const { return modelLoaded_; }
    std::string getModelName() const { return modelName_; }

    // Load progress (0..1) for UI progress bar
    float getLoadProgress() const { return loadProgress_; }

    // ── Rig inspector ─────────────────────────────────────────────────────────
    const std::vector<BoneTransform>& getBoneList() const;
    glm::mat4 getBoneTransform(const std::string& name) const;
    void      setBoneOverride(const std::string& name, const glm::mat4& localTransform);
    void      clearBoneOverride(const std::string& name);
    void      clearAllBoneOverrides();

    // ── Animation from tracking ───────────────────────────────────────────────
    void updateFromTracking(const tracking::TrackingFrame& frame, float deltaTime);
    void render(const glm::mat4& view, const glm::mat4& proj, const rhi::BufferHandle& lightingUB);

    // ── Blend shape control ───────────────────────────────────────────────────
    void  setBlendShapeWeight(const std::string& name, float weight);
    float getBlendShapeWeight(const std::string& name) const;
    void  resetBlendShapes();
    const std::unordered_map<std::string, float>& getAllBlendShapeWeights() const;

    // VRM named expressions (map to one or more blend shapes)
    void setExpression(const std::string& expressionName, float weight);

    // Auto-apply tracking blendshapes every update
    void setAutoBlendShapes(bool v) { autoBlendShapes_ = v; }
    bool getAutoBlendShapes()  const { return autoBlendShapes_; }

    // ── Spring-bone physics system ──────────────────────────────────────────
    struct PhysicsParams {
        float stiffness = 2.0f; // Spring resistance factor
        float gravity = 1.2f;    // Gravity multiplier pulling down
        float drag = 0.4f;       // Air resistance damping (0 to 1)
        float wind = 0.0f;       // Simulated wind/sway amplitude
    };

    struct PhysicsBoneConfig {
        std::string rootBoneName;
        PhysicsParams params;
    };

    struct PhysicsCollider {
        std::string boneName;  // Bone this sphere is attached to (e.g. "head")
        glm::vec3 offset{0.f}; // Local offset from the bone center
        float radius = 0.15f;   // Radius of collider sphere
    };

    void setPhysicsEnabled(bool enabled) { physicsEnabled_ = enabled; }
    bool isPhysicsEnabled() const { return physicsEnabled_; }

    void  setMeshCollisionRadius(float r) { meshCollisionRadius_ = r; }
    float getMeshCollisionRadius() const { return meshCollisionRadius_; }

    // Arm and Hand tracking toggles
    void setArmTrackingEnabled(bool enabled) { armTrackingEnabled_ = enabled; }
    bool isArmTrackingEnabled() const { return armTrackingEnabled_; }
    void setHandTrackingEnabled(bool enabled) { handTrackingEnabled_ = enabled; }
    bool isHandTrackingEnabled() const { return handTrackingEnabled_; }

    void setShowCollisionDebug(bool v) { showCollisionDebug_ = v; }
    bool getShowCollisionDebug() const { return showCollisionDebug_; }

    // Configure spring-bone chains manually (e.g. hair strands, ears)
    void addPhysicsChain(const std::string& rootName, const PhysicsParams& params);
    void clearPhysicsChains();
    std::vector<PhysicsBoneConfig> getPhysicsChains() const;

    // Colliders setup (colliders repel physics bones to prevent intersecting the head/shoulders)
    void addCollider(const PhysicsCollider& col);
    void clearColliders();
    std::vector<PhysicsCollider> getColliders() const;

    // Get live skinned vertices for complex collision visualization
    const std::vector<glm::vec3>& getSkinnedCollisionPositions() const;

    // Model-space scale / transform
    void setModelScale(float s)           { modelScale_ = s; }
    void setModelTranslation(glm::vec3 t) { modelTranslation_ = t; }
    float       getModelScale()       const { return modelScale_; }
    glm::vec3   getModelTranslation() const { return modelTranslation_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    rhi::IRHIContext& rhi_;
    bool        modelLoaded_     = false;
    std::string modelName_;
    bool        autoBlendShapes_ = true;
    bool        physicsEnabled_  = true;
    bool        armTrackingEnabled_  = true;
    bool        handTrackingEnabled_ = true;
    bool        showCollisionDebug_  = false;
    float       meshCollisionRadius_ = 0.05f;
    float       loadProgress_    = 0.f;
    float       modelScale_      = 1.f;
    glm::vec3   modelTranslation_{0.f};

    std::vector<BlendShapeMapping> blendShapeMappings_;

    void computeBoneMatrices();
};

} // namespace vts::renderer