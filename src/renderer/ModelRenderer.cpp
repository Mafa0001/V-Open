#define GLM_ENABLE_EXPERIMENTAL

#include "renderer/ModelRenderer.h"

#include <spdlog/spdlog.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <unordered_set>

// TinyGLTF for glTF/VRM loading
#define STB_IMAGE_IMPLEMENTATION
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "tiny_gltf.h"

using json = nlohmann::json;

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

// ─── VRM bone name mappings ───────────────────────────────────────────────────
static const std::unordered_map<std::string, VRMBone> kVRMBoneNames = {
    {"hips", VRMBone::Hips},
    {"spine", VRMBone::Spine},
    {"chest", VRMBone::Chest},
    {"upper_chest", VRMBone::UpperChest},
    {"upperChest", VRMBone::UpperChest},
    {"neck", VRMBone::Neck},
    {"head", VRMBone::Head},
    {"leftEye", VRMBone::LeftEye},
    {"rightEye", VRMBone::RightEye},
    {"leftShoulder", VRMBone::LeftShoulder},
    {"leftUpperArm", VRMBone::LeftUpperArm},
    {"leftLowerArm", VRMBone::LeftLowerArm},
    {"leftHand", VRMBone::LeftHand},
    {"rightShoulder", VRMBone::RightShoulder},
    {"rightUpperArm", VRMBone::RightUpperArm},
    {"rightLowerArm", VRMBone::RightLowerArm},
    {"rightHand", VRMBone::RightHand},
    // Fingers — Left
    {"leftThumbProximal",       VRMBone::LeftThumbProximal},
    {"leftThumbIntermediate",   VRMBone::LeftThumbIntermediate},
    {"leftThumbDistal",         VRMBone::LeftThumbDistal},
    {"leftIndexProximal",       VRMBone::LeftIndexProximal},
    {"leftIndexIntermediate",   VRMBone::LeftIndexIntermediate},
    {"leftIndexDistal",         VRMBone::LeftIndexDistal},
    {"leftMiddleProximal",      VRMBone::LeftMiddleProximal},
    {"leftMiddleIntermediate",  VRMBone::LeftMiddleIntermediate},
    {"leftMiddleDistal",        VRMBone::LeftMiddleDistal},
    {"leftRingProximal",        VRMBone::LeftRingProximal},
    {"leftRingIntermediate",    VRMBone::LeftRingIntermediate},
    {"leftRingDistal",          VRMBone::LeftRingDistal},
    {"leftLittleProximal",      VRMBone::LeftLittleProximal},
    {"leftLittleIntermediate",  VRMBone::LeftLittleIntermediate},
    {"leftLittleDistal",        VRMBone::LeftLittleDistal},
    // Fingers — Right
    {"rightThumbProximal",      VRMBone::RightThumbProximal},
    {"rightThumbIntermediate",  VRMBone::RightThumbIntermediate},
    {"rightThumbDistal",        VRMBone::RightThumbDistal},
    {"rightIndexProximal",      VRMBone::RightIndexProximal},
    {"rightIndexIntermediate",  VRMBone::RightIndexIntermediate},
    {"rightIndexDistal",        VRMBone::RightIndexDistal},
    {"rightMiddleProximal",     VRMBone::RightMiddleProximal},
    {"rightMiddleIntermediate", VRMBone::RightMiddleIntermediate},
    {"rightMiddleDistal",       VRMBone::RightMiddleDistal},
    {"rightRingProximal",       VRMBone::RightRingProximal},
    {"rightRingIntermediate",   VRMBone::RightRingIntermediate},
    {"rightRingDistal",         VRMBone::RightRingDistal},
    {"rightLittleProximal",     VRMBone::RightLittleProximal},
    {"rightLittleIntermediate", VRMBone::RightLittleIntermediate},
    {"rightLittleDistal",       VRMBone::RightLittleDistal},
    {"jaw",                     VRMBone::Jaw},
};

// ─── VRM expression name → blend shapes ────────────────────────────────────
static const std::unordered_map<std::string, std::vector<std::string>> kVRMExpressions = {
    {"happy",     {"eye_smile_L", "eye_smile_R", "mouth_smile"}},
    {"angry",     {"brow_down_L", "brow_down_R", "mouth_frown"}},
    {"sad",       {"brow_up_L",   "brow_up_R",   "mouth_frown"}},
    {"surprised", {"eye_wide_open", "mouth_ah"}},
    {"blink",     {"eye_blink_L", "eye_blink_R"}},
    {"blink_left",  {"eye_blink_L"}},
    {"blink_right", {"eye_blink_R"}},
    {"a", {"mouth_aa"}},
    {"i", {"mouth_ii"}},
    {"u", {"mouth_uu"}},
    {"e", {"mouth_ee"}},
    {"o", {"mouth_oh"}},
};

// ─── Default tracker blendshape → VRM mesh morph name mappings ─────────────
static const std::vector<BlendShapeMapping> kDefaultMappings = {
    {"eyeBlinkLeft",      "eye_blink_L"},
    {"eyeBlinkRight",     "eye_blink_R"},
    {"eyeLookDownLeft",   "eye_lookDown_L"},
    {"eyeLookDownRight",  "eye_lookDown_R"},
    {"eyeLookInLeft",     "eye_lookIn_L"},
    {"eyeLookInRight",    "eye_lookIn_R"},
    {"eyeLookOutLeft",    "eye_lookOut_L"},
    {"eyeLookOutRight",   "eye_lookOut_R"},
    {"eyeLookUpLeft",     "eye_lookUp_L"},
    {"eyeLookUpRight",    "eye_lookUp_R"},
    {"eyeSquintLeft",     "eye_squint_L"},
    {"eyeSquintRight",    "eye_squint_R"},
    {"eyeWideLeft",       "eye_wide_L"},
    {"eyeWideRight",      "eye_wide_R"},
    {"jawForward",        "jaw_forward"},
    {"jawLeft",           "jaw_left"},
    {"jawRight",          "jaw_right"},
    {"jawOpen",           "mouth_aa"},
    {"mouthClose",        "mouth_close"},
    {"mouthFunnel",       "mouth_uu"},
    {"mouthPucker",       "mouth_oh"},
    {"mouthSmileLeft",    "mouth_smile_L"},
    {"mouthSmileRight",   "mouth_smile_R"},
    {"mouthFrownLeft",    "mouth_frown_L"},
    {"mouthFrownRight",   "mouth_frown_R"},
    {"mouthDimpleLeft",   "mouth_dimple_L"},
    {"mouthDimpleRight",  "mouth_dimple_R"},
    {"browDownLeft",      "brow_down_L"},
    {"browDownRight",     "brow_down_R"},
    {"browInnerUp",       "brow_up"},
    {"browOuterUpLeft",   "brow_up_L"},
    {"browOuterUpRight",  "brow_up_R"},
    {"cheekPuff",         "cheek_puff"},
    {"tongueOut",         "tongue_out"},
};

// ─── UBO structures matching std140 blocks ───────────────────────────────────
struct ViewProjBlock {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 model;
};

struct MaterialBlock {
    glm::vec4 baseColorFactor;
    int       hasTexture;
    int       _pad[3];
};

// ─── glTF accessor helpers ──────────────────────────────────────────────────
static const float* accessorFloatPtr(const tinygltf::Model& m, int accIdx) {
    if (accIdx < 0) return nullptr;
    const auto& acc = m.accessors[accIdx];
    const auto& bv  = m.bufferViews[acc.bufferView];
    const auto& buf = m.buffers[bv.buffer];
    return reinterpret_cast<const float*>(buf.data.data() + bv.byteOffset + acc.byteOffset);
}

static const uint8_t* accessorRawPtr(const tinygltf::Model& m, int accIdx, size_t& stride, size_t& count) {
    if (accIdx < 0) { stride = 0; count = 0; return nullptr; }
    const auto& acc = m.accessors[accIdx];
    const auto& bv  = m.bufferViews[acc.bufferView];
    const auto& buf = m.buffers[bv.buffer];
    stride = bv.byteStride ? bv.byteStride : tinygltf::GetComponentSizeInBytes(acc.componentType) * tinygltf::GetNumComponentsInType(acc.type);
    count  = acc.count;
    return buf.data.data() + bv.byteOffset + acc.byteOffset;
}

// ─── Impl ───────────────────────────────────────────────────────────────────
struct VRMBlendShapeBind {
    int meshIndex;
    int targetIndex;
    float weightFactor;
};

struct VRMBlendShapeGroup {
    std::string name;
    std::vector<VRMBlendShapeBind> binds;
};

struct PhysicsBoneState {
    int boneIdx = -1;
    glm::vec3 currPosition{0.f};
    glm::vec3 prevPosition{0.f};
    float boneLength = 0.f;
};

struct PhysicsChainState {
    std::string rootName;
    ModelRenderer::PhysicsParams params;
    std::vector<PhysicsBoneState> bones;
};

struct CollisionVertex {
    glm::vec3 pos;
    int joints[4];
    float weights[4];
    int primitiveIdx;
};

struct SparseMorphTarget {
    int targetIndex;
    std::vector<uint32_t> vertexIndices;
    std::vector<glm::vec3> positionDeltas;
};

struct InternalCollider {
    int boneIdx = -1;
    glm::vec3 offset{0.f};
    float radius = 0.15f;
    bool isTorso = false;
};

struct ModelRenderer::Impl {
    tinygltf::Model gltfModel;

    std::vector<ModelNode>        nodes;
    std::vector<MeshPrimitive>    meshes;
    std::vector<BoneTransform>    bones;
    std::vector<glm::mat4>        boneMatrices;        // final skinning matrices for upload
    std::vector<BlendShapeTarget> blendShapes;
    std::unordered_map<std::string, float>     blendShapeWeights;
    std::unordered_map<std::string, glm::mat4> boneOverrides;
    std::unordered_map<VRMBone, int>           vrmBoneIndex;   // VRMBone → bones[] index
    std::unordered_map<std::string, int>       boneByName;     // name → bones[] index
    std::unordered_map<std::string, VRMBlendShapeGroup> vrmBlendShapes;

    // Physics state
    std::vector<PhysicsChainState> physicsChains;
    std::vector<ModelRenderer::PhysicsCollider> colliders;
    float accumWindTime = 0.f;

    // Subsampled vertices for complex mesh-based collision
    std::vector<CollisionVertex> collisionVertices;
    std::vector<glm::vec3>       skinnedCollisionPositions;

    // Tracking persistence: cache last known bone local transform matrices when track is lost
    std::unordered_map<int, glm::mat4> activeBoneLocals;
    std::unordered_map<int, glm::quat> activeBoneRots;

    std::unordered_set<size_t> hadActiveTargets;
    float physicsAccumulator = 0.f;
    std::vector<float> morphedDataBuffer;
    std::vector<std::vector<SparseMorphTarget>> sparseMorphTargets;
    std::vector<std::unordered_map<int, float>> lastAppliedMeshWeights;
    std::vector<InternalCollider> internalColliders;

    // RHI resources
    rhi::PipelineHandle       pipeline;
    rhi::BufferHandle         boneUBO;
    rhi::BufferHandle         viewProjUB;
    rhi::TextureHandle        defaultTexture;
    std::vector<rhi::TextureHandle> textures;          // per-material base colour textures
    std::vector<rhi::BufferHandle>  materialUBs;       // per-material factors UBO

    glm::mat4 modelMatrix{1.f};

    void applyCPUMorphTargets(rhi::IRHIContext& rhi);

    void updateInternalColliders() {
        internalColliders.clear();
        for (const auto& col : colliders) {
            InternalCollider ic;
            auto it = boneByName.find(col.boneName);
            ic.boneIdx = (it != boneByName.end()) ? it->second : -1;
            ic.offset = col.offset;
            ic.radius = col.radius;
            
            std::string nameLower = col.boneName;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            ic.isTorso = (nameLower == "spine" || nameLower == "chest" || nameLower == "upperchest");
            
            internalColliders.push_back(ic);
        }
    }

    void clear() {
        meshes.clear(); nodes.clear(); bones.clear(); boneMatrices.clear();
        sparseMorphTargets.clear();
        lastAppliedMeshWeights.clear();
        internalColliders.clear();
        blendShapes.clear(); blendShapeWeights.clear(); boneOverrides.clear();
        vrmBoneIndex.clear(); boneByName.clear(); vrmBlendShapes.clear();
        physicsChains.clear(); colliders.clear(); accumWindTime = 0.f;
        collisionVertices.clear(); skinnedCollisionPositions.clear();
        activeBoneLocals.clear();
        textures.clear(); materialUBs.clear();
        boneUBO = nullptr;
        viewProjUB = nullptr;
        defaultTexture = nullptr;
        pipeline = nullptr;
    }
};

// ─── Constructor ────────────────────────────────────────────────────────────
ModelRenderer::ModelRenderer(rhi::IRHIContext& rhi) : rhi_(rhi) {
    impl_ = std::make_unique<Impl>();
    blendShapeMappings_ = kDefaultMappings;
    spdlog::info("[ModelRenderer] Initialized");
}

ModelRenderer::~ModelRenderer() {
    if (impl_) {
        impl_->clear();
    }
}

// ─── Load model ─────────────────────────────────────────────────────────────
bool ModelRenderer::loadModel(const std::string& filepath) {
    loadProgress_ = 0.f;
    impl_->clear();

    tinygltf::TinyGLTF loader;
    tinygltf::Model    model;
    std::string err, warn;

    std::string ext = filepath.substr(filepath.find_last_of('.'));
    bool success = (ext == ".glb" || ext == ".vrm")
        ? loader.LoadBinaryFromFile(&model, &err, &warn, filepath)
        : loader.LoadASCIIFromFile(&model, &err, &warn, filepath);

    if (!warn.empty()) spdlog::warn("[ModelRenderer] {}", warn);
    if (!err.empty())  spdlog::error("[ModelRenderer] {}", err);
    if (!success) return false;

    loadProgress_ = 0.1f;

    // ── Extract model name ──────────────────────────────────────────────────
    if (model.extensions.count("VRM")) {
        const auto& vrm = model.extensions.at("VRM");
        if (vrm.Has("meta") && vrm.Get("meta").Has("title")) {
            modelName_ = vrm.Get("meta").Get("title").Get<std::string>();
        }
    }
    if (modelName_.empty())
        modelName_ = filepath.substr(filepath.find_last_of("/\\") + 1);

    spdlog::info("[ModelRenderer] Loading: {}", modelName_);

    // ── Build shader & pipeline ──────────────────────────────────────────────
    rhi::ShaderDesc vsDesc{ rhi::ShaderStage::Vertex, loadShaderFile("ModelRenderer.vert") };
    rhi::ShaderDesc fsDesc{ rhi::ShaderStage::Fragment, loadShaderFile("ModelRenderer.frag") };
    auto vs = rhi_.createShader(vsDesc);
    auto fs = rhi_.createShader(fsDesc);

    rhi::VertexAttribute posAttr{ 0, 0, 0, 64, rhi::PixelFormat::RGB32F };
    rhi::VertexAttribute normAttr{ 1, 0, 12, 64, rhi::PixelFormat::RGB32F };
    rhi::VertexAttribute uvAttr{ 2, 0, 24, 64, rhi::PixelFormat::RG32F };
    rhi::VertexAttribute jointAttr{ 3, 0, 32, 64, rhi::PixelFormat::RGBA32F };
    rhi::VertexAttribute weightAttr{ 4, 0, 48, 64, rhi::PixelFormat::RGBA32F };

    rhi::PipelineDesc pipDesc;
    pipDesc.vertexShader = vs;
    pipDesc.fragmentShader = fs;
    pipDesc.vertexAttributes = { posAttr, normAttr, uvAttr, jointAttr, weightAttr };
    pipDesc.raster.cullMode = rhi::CullMode::None;
    pipDesc.depth.depthTest = true;
    pipDesc.depth.depthWrite = true;
    pipDesc.blend.enabled = false;
    pipDesc.topology = rhi::PrimitiveTopology::TriangleList;
    impl_->pipeline = rhi_.createPipeline(pipDesc);

    // ── Create default 1x1 white texture ─────────────────────────────────────
    uint32_t whitePixel = 0xFFFFFFFF;
    rhi::TextureDesc whiteTexDesc;
    whiteTexDesc.width = 1;
    whiteTexDesc.height = 1;
    whiteTexDesc.format = rhi::PixelFormat::RGBA8_UNORM;
    whiteTexDesc.initialData = &whitePixel;
    impl_->defaultTexture = rhi_.createTexture(whiteTexDesc);

    // ── Upload textures ─────────────────────────────────────────────────────
    impl_->textures.resize(model.materials.size());
    impl_->materialUBs.resize(model.materials.size());

    for (int mi = 0; mi < (int)model.materials.size(); ++mi) {
        const auto& mat = model.materials[mi];

        glm::vec4 baseColor(1.f);
        if (mat.pbrMetallicRoughness.baseColorFactor.size() == 4) {
            const auto& f = mat.pbrMetallicRoughness.baseColorFactor;
            baseColor = { (float)f[0], (float)f[1], (float)f[2], (float)f[3] };
        }

        // Texture
        rhi::TextureHandle texHandle = nullptr;
        int texIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
        if (texIdx >= 0 && texIdx < (int)model.textures.size()) {
            int imgIdx = model.textures[texIdx].source;
            if (imgIdx >= 0 && imgIdx < (int)model.images.size()) {
                const auto& img = model.images[imgIdx];
                rhi::TextureDesc texDesc;
                texDesc.width = img.width;
                texDesc.height = img.height;
                texDesc.format = (img.component == 4) ? rhi::PixelFormat::RGBA8_UNORM : rhi::PixelFormat::RGBA8_UNORM;
                texDesc.mipLevels = 4;
                texDesc.initialData = img.image.data();
                texHandle = rhi_.createTexture(texDesc);
            }
        }
        impl_->textures[mi] = texHandle;

        // Material UB
        MaterialBlock matBlock{};
        matBlock.baseColorFactor = baseColor;
        matBlock.hasTexture = texHandle ? 1 : 0;

        rhi::BufferDesc matUbDesc;
        matUbDesc.size = sizeof(MaterialBlock);
        matUbDesc.usage = rhi::BufferUsage::Uniform;
        matUbDesc.memory = rhi::MemoryType::HostVisible;
        matUbDesc.initialData = &matBlock;
        impl_->materialUBs[mi] = rhi_.createBuffer(matUbDesc);
    }

    // Allocate material UB fallback if model has 0 materials
    if (model.materials.empty()) {
        MaterialBlock matBlock{};
        matBlock.baseColorFactor = glm::vec4(1.f);
        matBlock.hasTexture = 0;
        rhi::BufferDesc matUbDesc;
        matUbDesc.size = sizeof(MaterialBlock);
        matUbDesc.usage = rhi::BufferUsage::Uniform;
        matUbDesc.memory = rhi::MemoryType::HostVisible;
        matUbDesc.initialData = &matBlock;
        impl_->materialUBs.push_back(rhi_.createBuffer(matUbDesc));
    }

    loadProgress_ = 0.3f;

    // ── Process nodes & build skeleton ─────────────────────────────────────
    impl_->nodes.resize(model.nodes.size());
    for (int ni = 0; ni < (int)model.nodes.size(); ++ni) {
        const auto& gn = model.nodes[ni];
        auto& n = impl_->nodes[ni];
        n.name      = gn.name;
        n.meshIndex = gn.mesh;
        n.skinIndex = gn.skin;
        n.children  = gn.children;

        if (!gn.matrix.empty()) {
            glm::mat4 m;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    m[c][r] = (float)gn.matrix[r*4+c];
            n.localTransform = m;
        } else {
            glm::mat4 T(1.f), R(1.f), S(1.f);
            if (!gn.translation.empty()) T = glm::translate(glm::mat4(1.f), {(float)gn.translation[0], (float)gn.translation[1], (float)gn.translation[2]});
            if (!gn.rotation.empty())    R = glm::mat4_cast(glm::quat((float)gn.rotation[3], (float)gn.rotation[0], (float)gn.rotation[1], (float)gn.rotation[2]));
            if (!gn.scale.empty())       S = glm::scale(glm::mat4(1.f), {(float)gn.scale[0], (float)gn.scale[1], (float)gn.scale[2]});
            n.localTransform = T * R * S;
        }
        for (int child : n.children)
            if (child < (int)impl_->nodes.size())
                impl_->nodes[child].parentIndex = ni;
    }

    // ── Build bone list from skins ──────────────────────────────────────────
    std::unordered_map<int, int> nodeToBone;
    for (const auto& skin : model.skins) {
        std::vector<glm::mat4> invBinds(skin.joints.size(), glm::mat4(1.f));
        if (skin.inverseBindMatrices >= 0) {
            const float* ptr = accessorFloatPtr(model, skin.inverseBindMatrices);
            if (ptr) {
                for (int j = 0; j < (int)skin.joints.size(); ++j) {
                    glm::mat4 m;
                    memcpy(glm::value_ptr(m), ptr + j*16, 16*sizeof(float));
                    invBinds[j] = m;
                }
            }
        }

        for (int j = 0; j < (int)skin.joints.size(); ++j) {
            int nodeIdx = skin.joints[j];
            int boneIdx = (int)impl_->bones.size();
            nodeToBone[nodeIdx] = boneIdx;

            BoneTransform bt;
            bt.name      = model.nodes[nodeIdx].name;
            bt.local     = impl_->nodes[nodeIdx].localTransform;
            bt.restLocal = bt.local;   // save rest pose — never touch this again
            bt.invBind   = invBinds[j];
            if (nodeIdx < (int)impl_->nodes.size())
                bt.parentIdx = impl_->nodes[nodeIdx].parentIndex;

            impl_->boneByName[bt.name] = boneIdx;
            impl_->bones.push_back(bt);
        }
    }

    // Resolve parent bone indices
    for (auto& b : impl_->bones) {
        int currParentNodeIdx = b.parentIdx;
        int resolvedBoneParentIdx = -1;
        while (currParentNodeIdx >= 0) {
            auto it = nodeToBone.find(currParentNodeIdx);
            if (it != nodeToBone.end()) {
                resolvedBoneParentIdx = it->second;
                break;
            }
            if (currParentNodeIdx < (int)impl_->nodes.size()) {
                currParentNodeIdx = impl_->nodes[currParentNodeIdx].parentIndex;
            } else {
                break;
            }
        }
        b.parentIdx = resolvedBoneParentIdx;
    }

    // Map VRM bone names
    for (auto& [vrmName, vrmBone] : kVRMBoneNames) {
        auto it = impl_->boneByName.find(vrmName);
        if (it != impl_->boneByName.end())
            impl_->vrmBoneIndex[vrmBone] = it->second;
    }

    // Try to find by VRM 0.0 or VRM 1.0 extension bone map
    if (model.extensions.count("VRM")) {
        const auto& vrm = model.extensions.at("VRM");
        if (vrm.Has("humanoid") && vrm.Get("humanoid").Has("humanBones")) {
            const auto& hb = vrm.Get("humanoid").Get("humanBones");
            if (hb.IsArray()) {
                for (int k = 0; k < (int)hb.ArrayLen(); ++k) {
                    const auto& entry = hb.Get(k);
                    if (!entry.Has("bone") || !entry.Has("node")) continue;
                    std::string boneName = entry.Get("bone").Get<std::string>();
                    int nodeIdx          = entry.Get("node").Get<int>();
                    
                    std::string bNameLower = boneName;
                    std::transform(bNameLower.begin(), bNameLower.end(), bNameLower.begin(), ::tolower);
                    VRMBone vrmBone = VRMBone::Count;
                    for (const auto& [name, bone] : kVRMBoneNames) {
                        std::string targetLower = name;
                        std::transform(targetLower.begin(), targetLower.end(), targetLower.begin(), ::tolower);
                        if (targetLower == bNameLower) {
                            vrmBone = bone;
                            break;
                        }
                    }
                    
                    auto nodeBoneIt = nodeToBone.find(nodeIdx);
                    if (vrmBone != VRMBone::Count && nodeBoneIt != nodeToBone.end())
                        impl_->vrmBoneIndex[vrmBone] = nodeBoneIt->second;
                }
            }
        }
    } else if (model.extensions.count("VRMC_vrm")) {
        const auto& vrm = model.extensions.at("VRMC_vrm");
        if (vrm.Has("humanoid") && vrm.Get("humanoid").Has("humanBones")) {
            const auto& hb = vrm.Get("humanoid").Get("humanBones");
            if (hb.IsObject()) {
                for (const auto& boneName : hb.Keys()) {
                    const auto& entry = hb.Get(boneName);
                    if (entry.Has("node")) {
                        int nodeIdx = entry.Get("node").Get<int>();
                        
                        std::string bNameLower = boneName;
                        std::transform(bNameLower.begin(), bNameLower.end(), bNameLower.begin(), ::tolower);
                        VRMBone vrmBone = VRMBone::Count;
                        for (const auto& [name, bone] : kVRMBoneNames) {
                            std::string targetLower = name;
                            std::transform(targetLower.begin(), targetLower.end(), targetLower.begin(), ::tolower);
                            if (targetLower == bNameLower) {
                                vrmBone = bone;
                                break;
                            }
                        }
                        
                        auto nodeBoneIt = nodeToBone.find(nodeIdx);
                        if (vrmBone != VRMBone::Count && nodeBoneIt != nodeToBone.end())
                            impl_->vrmBoneIndex[vrmBone] = nodeBoneIt->second;
                    }
                }
            }
        }
    }

    // Parse VRM blend shape groups
    if (model.extensions.count("VRM")) {
        const auto& vrm = model.extensions.at("VRM");
        if (vrm.Has("blendShapeMaster") && vrm.Get("blendShapeMaster").Has("blendShapeGroups")) {
            const auto& groups = vrm.Get("blendShapeMaster").Get("blendShapeGroups");
            if (groups.IsArray()) {
                for (int i = 0; i < (int)groups.ArrayLen(); ++i) {
                    const auto& g = groups.Get(i);
                    if (!g.Has("name")) continue;
                    std::string gName = g.Get("name").Get<std::string>();
                    
                    VRMBlendShapeGroup group;
                    group.name = gName;
                    
                    // Case-insensitive key
                    std::string key = gName;
                    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                    
                    // Preset name if present
                    if (g.Has("presetName")) {
                        std::string pName = g.Get("presetName").Get<std::string>();
                        std::transform(pName.begin(), pName.end(), pName.begin(), ::tolower);
                        if (pName != "unknown" && !pName.empty()) {
                            key = pName;
                        }
                    }
                    
                    if (g.Has("binds") && g.Get("binds").IsArray()) {
                        const auto& binds = g.Get("binds");
                        for (int j = 0; j < (int)binds.ArrayLen(); ++j) {
                            const auto& b = binds.Get(j);
                            if (b.Has("mesh") && b.Has("index") && b.Has("weight")) {
                                VRMBlendShapeBind bind;
                                bind.meshIndex = b.Get("mesh").Get<int>();
                                bind.targetIndex = b.Get("index").Get<int>();
                                bind.weightFactor = b.Get("weight").Get<double>() / 100.0f;
                                group.binds.push_back(bind);
                            }
                        }
                    }
                    impl_->vrmBlendShapes[key] = group;
                }
            }
        }
    }

    // ── Fuzzy fallback bone matching ────────────────────────────────────────
    // Tries progressively more aggressive keyword matching.
    // Matching is case-insensitive substring search.
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };
    
    struct FuzzyRule { VRMBone bone; std::vector<std::string> required; std::vector<std::string> forbidden; };
    // required: ALL must match. forbidden: NONE must match.
    static const std::vector<FuzzyRule> kFuzzyRules = {
        // ─ Left arm ────────────────────────────────────────────────────────────────────────────────────────────────
        {VRMBone::LeftShoulder,  {"left","shoulder"},   {"upper","lower","arm","hand"}},
        {VRMBone::LeftShoulder,  {"shoulder","l"},      {"upper","lower","arm","hand","right","r."}},
        {VRMBone::LeftUpperArm,  {"left","upper","arm"}, {"lower","fore"}},
        {VRMBone::LeftUpperArm,  {"upperarm","l"},       {"lower","fore","right","r."}},
        {VRMBone::LeftLowerArm,  {"left","lower","arm"}, {}},
        {VRMBone::LeftLowerArm,  {"left","fore","arm"},  {}},
        {VRMBone::LeftLowerArm,  {"lowerarm","l"},       {"upper","right","r."}},
        {VRMBone::LeftHand,      {"left","hand"},        {"thumb","index","middle","ring","little","pinky","finger"}},
        {VRMBone::LeftHand,      {"hand","l"},           {"thumb","index","middle","ring","little","pinky","finger","right","r."}},
        {VRMBone::LeftHand,      {"l_hand"},             {"thumb","index","middle","ring","little","pinky","finger","right","r."}},
        {VRMBone::LeftHand,      {"hand_l"},             {"thumb","index","middle","ring","little","pinky","finger","right","r."}},
        {VRMBone::LeftHand,      {"hand.l"},             {"thumb","index","middle","ring","little","pinky","finger","right","r."}},
        // ─ Right arm ──────────────────────────────────────────────────────────────────────────────────────
        {VRMBone::RightShoulder, {"right","shoulder"},   {"upper","lower","arm","hand"}},
        {VRMBone::RightShoulder, {"shoulder","r"},      {"upper","lower","arm","hand","left","l."}},
        {VRMBone::RightUpperArm, {"right","upper","arm"},{"lower","fore"}},
        {VRMBone::RightUpperArm, {"upperarm","r"},       {"lower","fore","left","l."}},
        {VRMBone::RightLowerArm, {"right","lower","arm"},{} },
        {VRMBone::RightLowerArm, {"right","fore","arm"}, {}},
        {VRMBone::RightLowerArm, {"lowerarm","r"},       {"upper","left","l."}},
        {VRMBone::RightHand,     {"right","hand"},       {"thumb","index","middle","ring","little","pinky","finger"}},
        {VRMBone::RightHand,     {"hand","r"},           {"thumb","index","middle","ring","little","pinky","finger","left","l."}},
        {VRMBone::RightHand,     {"r_hand"},             {"thumb","index","middle","ring","little","pinky","finger","left","l."}},
        {VRMBone::RightHand,     {"hand_r"},             {"thumb","index","middle","ring","little","pinky","finger","left","l."}},
        {VRMBone::RightHand,     {"hand.r"},             {"thumb","index","middle","ring","little","pinky","finger","left","l."}},
        // ─ Spine / body ───────────────────────────────────────────────────────────────────────────────────
        {VRMBone::Hips,          {"hip"},                {"left","right","l","r"}},
        {VRMBone::Hips,          {"pelvis"},             {}},
        {VRMBone::Spine,         {"spine"},              {"upper","chest","2","3"}},
        {VRMBone::Chest,         {"chest"},              {"upper"}},
        {VRMBone::UpperChest,    {"upperchest"},         {}},
        {VRMBone::UpperChest,    {"upper","chest"},      {}},
        {VRMBone::Neck,          {"neck"},               {}},
        {VRMBone::Head,          {"head"},               {"left","right"}},
        {VRMBone::Jaw,           {"jaw"},                {}},
        {VRMBone::LeftEye,       {"eye","l"},            {"right","r.","brow"}},
        {VRMBone::RightEye,      {"eye","r"},            {"left","l.","brow"}},
        // ─ Left fingers ──────────────────────────────────────────────────────────────────────────────────────
        {VRMBone::LeftThumbProximal,       {"left","thumb","proximal"},     {}},
        {VRMBone::LeftThumbProximal,       {"l","thumb","proximal"},        {"right"}},
        {VRMBone::LeftThumbProximal,       {"l","thumb","1"},               {"right","distal","2","3","tip"}},
        {VRMBone::LeftThumbProximal,       {"l","thumb","a"},               {"right","b","c","2","3","tip"}},
        {VRMBone::LeftThumbIntermediate,   {"left","thumb","intermediate"}, {}},
        {VRMBone::LeftThumbIntermediate,   {"l","thumb","intermediate"},    {"right"}},
        {VRMBone::LeftThumbIntermediate,   {"l","thumb","2"},               {"right","distal","1","3","tip"}},
        {VRMBone::LeftThumbIntermediate,   {"l","thumb","b"},               {"right","a","c","1","3","tip"}},
        {VRMBone::LeftThumbDistal,         {"left","thumb","distal"},       {}},
        {VRMBone::LeftThumbDistal,         {"l","thumb","distal"},          {"right"}},
        {VRMBone::LeftThumbDistal,         {"l","thumb","3"},               {"right","proximal","inter","1","2"}},
        {VRMBone::LeftThumbDistal,         {"l","thumb","c"},               {"right","proximal","inter","a","b"}},

        {VRMBone::LeftIndexProximal,       {"left","index","proximal"},     {}},
        {VRMBone::LeftIndexProximal,       {"l","index","proximal"},        {"right"}},
        {VRMBone::LeftIndexProximal,       {"l","index","1"},               {"right","distal","2","3","tip"}},
        {VRMBone::LeftIndexProximal,       {"l","index","a"},               {"right","b","c","2","3","tip"}},
        {VRMBone::LeftIndexIntermediate,   {"left","index","intermediate"}, {}},
        {VRMBone::LeftIndexIntermediate,   {"l","index","intermediate"},    {"right"}},
        {VRMBone::LeftIndexIntermediate,   {"l","index","2"},               {"right","distal","1","3","tip"}},
        {VRMBone::LeftIndexIntermediate,   {"l","index","b"},               {"right","a","c","1","3","tip"}},
        {VRMBone::LeftIndexDistal,         {"left","index","distal"},       {}},
        {VRMBone::LeftIndexDistal,         {"l","index","distal"},          {"right"}},
        {VRMBone::LeftIndexDistal,         {"l","index","3"},               {"right","proximal","inter","1","2"}},
        {VRMBone::LeftIndexDistal,         {"l","index","c"},               {"right","proximal","inter","a","b"}},

        {VRMBone::LeftMiddleProximal,      {"left","middle","proximal"},    {}},
        {VRMBone::LeftMiddleProximal,      {"l","middle","proximal"},       {"right"}},
        {VRMBone::LeftMiddleProximal,      {"l","middle","1"},              {"right","distal","2","3","tip"}},
        {VRMBone::LeftMiddleProximal,      {"l","middle","a"},              {"right","b","c","2","3","tip"}},
        {VRMBone::LeftMiddleIntermediate,  {"left","middle","intermediate"},{}},
        {VRMBone::LeftMiddleIntermediate,  {"l","middle","intermediate"},   {"right"}},
        {VRMBone::LeftMiddleIntermediate,  {"l","middle","2"},              {"right","distal","1","3","tip"}},
        {VRMBone::LeftMiddleIntermediate,  {"l","middle","b"},              {"right","a","c","1","3","tip"}},
        {VRMBone::LeftMiddleDistal,        {"left","middle","distal"},      {}},
        {VRMBone::LeftMiddleDistal,        {"l","middle","distal"},         {"right"}},
        {VRMBone::LeftMiddleDistal,        {"l","middle","3"},              {"right","proximal","inter","1","2"}},
        {VRMBone::LeftMiddleDistal,        {"l","middle","c"},              {"right","proximal","inter","a","b"}},

        {VRMBone::LeftRingProximal,        {"left","ring","proximal"},      {}},
        {VRMBone::LeftRingProximal,        {"l","ring","proximal"},         {"right"}},
        {VRMBone::LeftRingProximal,        {"l","ring","1"},                {"right","distal","2","3","tip"}},
        {VRMBone::LeftRingProximal,        {"l","ring","a"},                {"right","b","c","2","3","tip"}},
        {VRMBone::LeftRingIntermediate,    {"left","ring","intermediate"},  {}},
        {VRMBone::LeftRingIntermediate,    {"l","ring","intermediate"},     {"right"}},
        {VRMBone::LeftRingIntermediate,    {"l","ring","2"},                {"right","distal","1","3","tip"}},
        {VRMBone::LeftRingIntermediate,    {"l","ring","b"},                {"right","a","c","1","3","tip"}},
        {VRMBone::LeftRingDistal,          {"left","ring","distal"},        {}},
        {VRMBone::LeftRingDistal,          {"l","ring","distal"},           {"right"}},
        {VRMBone::LeftRingDistal,          {"l","ring","3"},                {"right","proximal","inter","1","2"}},
        {VRMBone::LeftRingDistal,          {"l","ring","c"},                {"right","proximal","inter","a","b"}},

        {VRMBone::LeftLittleProximal,      {"left","little","proximal"},    {}},
        {VRMBone::LeftLittleProximal,      {"left","pinky","proximal"},     {}},
        {VRMBone::LeftLittleProximal,      {"l","little","proximal"},       {"right"}},
        {VRMBone::LeftLittleProximal,      {"l","pinky","proximal"},        {"right"}},
        {VRMBone::LeftLittleProximal,      {"l","little","1"},              {"right","distal","2","3","tip"}},
        {VRMBone::LeftLittleProximal,      {"l","pinky","1"},               {"right","distal","2","3","tip"}},
        {VRMBone::LeftLittleProximal,      {"l","little","a"},              {"right","b","c","2","3","tip"}},
        {VRMBone::LeftLittleProximal,      {"l","pinky","a"},               {"right","b","c","2","3","tip"}},
        {VRMBone::LeftLittleIntermediate,  {"left","little","intermediate"},{}},
        {VRMBone::LeftLittleIntermediate,  {"left","pinky","intermediate"}, {}},
        {VRMBone::LeftLittleIntermediate,  {"l","little","intermediate"},   {"right"}},
        {VRMBone::LeftLittleIntermediate,  {"l","pinky","intermediate"},    {"right"}},
        {VRMBone::LeftLittleIntermediate,  {"l","little","2"},              {"right","distal","1","3","tip"}},
        {VRMBone::LeftLittleIntermediate,  {"l","pinky","2"},               {"right","distal","1","3","tip"}},
        {VRMBone::LeftLittleIntermediate,  {"l","little","b"},              {"right","a","c","1","3","tip"}},
        {VRMBone::LeftLittleIntermediate,  {"l","pinky","b"},               {"right","a","c","1","3","tip"}},
        {VRMBone::LeftLittleDistal,        {"left","little","distal"},      {}},
        {VRMBone::LeftLittleDistal,        {"left","pinky","distal"},       {}},
        {VRMBone::LeftLittleDistal,        {"l","little","distal"},         {"right"}},
        {VRMBone::LeftLittleDistal,        {"l","pinky","distal"},          {"right"}},
        {VRMBone::LeftLittleDistal,        {"l","little","3"},              {"right","proximal","inter","1","2"}},
        {VRMBone::LeftLittleDistal,        {"l","pinky","3"},               {"right","proximal","inter","1","2"}},
        {VRMBone::LeftLittleDistal,        {"l","little","c"},              {"right","proximal","inter","a","b"}},
        {VRMBone::LeftLittleDistal,        {"l","pinky","c"},               {"right","proximal","inter","a","b"}},

        // ─ Right fingers ─────────────────────────────────────────────────────────────────────────────────────
        {VRMBone::RightThumbProximal,      {"right","thumb","proximal"},    {}},
        {VRMBone::RightThumbProximal,      {"r","thumb","proximal"},        {"left"}},
        {VRMBone::RightThumbProximal,      {"r","thumb","1"},               {"left","distal","2","3","tip"}},
        {VRMBone::RightThumbProximal,      {"r","thumb","a"},               {"left","b","c","2","3","tip"}},
        {VRMBone::RightThumbIntermediate,  {"right","thumb","intermediate"},{}},
        {VRMBone::RightThumbIntermediate,  {"r","thumb","intermediate"},   {"left"}},
        {VRMBone::RightThumbIntermediate,  {"r","thumb","2"},               {"left","distal","1","3","tip"}},
        {VRMBone::RightThumbIntermediate,  {"r","thumb","b"},               {"left","a","c","1","3","tip"}},
        {VRMBone::RightThumbDistal,        {"right","thumb","distal"},      {}},
        {VRMBone::RightThumbDistal,        {"r","thumb","distal"},          {"left"}},
        {VRMBone::RightThumbDistal,        {"r","thumb","3"},               {"left","proximal","inter","1","2"}},
        {VRMBone::RightThumbDistal,        {"r","thumb","c"},               {"left","proximal","inter","a","b"}},

        {VRMBone::RightIndexProximal,      {"right","index","proximal"},    {}},
        {VRMBone::RightIndexProximal,      {"r","index","proximal"},        {"left"}},
        {VRMBone::RightIndexProximal,      {"r","index","1"},               {"left","distal","2","3","tip"}},
        {VRMBone::RightIndexProximal,      {"r","index","a"},               {"left","b","c","2","3","tip"}},
        {VRMBone::RightIndexIntermediate,  {"right","index","intermediate"},{}},
        {VRMBone::RightIndexIntermediate,  {"r","index","intermediate"},    {"left"}},
        {VRMBone::RightIndexIntermediate,  {"r","index","2"},               {"left","distal","1","3","tip"}},
        {VRMBone::RightIndexIntermediate,  {"r","index","b"},               {"left","a","c","1","3","tip"}},
        {VRMBone::RightIndexDistal,        {"right","index","distal"},      {}},
        {VRMBone::RightIndexDistal,        {"r","index","distal"},          {"left"}},
        {VRMBone::RightIndexDistal,        {"r","index","3"},               {"left","proximal","inter","1","2"}},
        {VRMBone::RightIndexDistal,        {"r","index","c"},               {"left","proximal","inter","a","b"}},

        {VRMBone::RightMiddleProximal,     {"right","middle","proximal"},   {}},
        {VRMBone::RightMiddleProximal,     {"r","middle","proximal"},       {"left"}},
        {VRMBone::RightMiddleProximal,     {"r","middle","1"},              {"left","distal","2","3","tip"}},
        {VRMBone::RightMiddleProximal,     {"r","middle","a"},              {"left","b","c","2","3","tip"}},
        {VRMBone::RightMiddleIntermediate, {"right","middle","intermediate"},{}},
        {VRMBone::RightMiddleIntermediate, {"r","middle","intermediate"},   {"left"}},
        {VRMBone::RightMiddleIntermediate, {"r","middle","2"},              {"left","distal","1","3","tip"}},
        {VRMBone::RightMiddleIntermediate, {"r","middle","b"},              {"left","a","c","1","3","tip"}},
        {VRMBone::RightMiddleDistal,       {"right","middle","distal"},     {}},
        {VRMBone::RightMiddleDistal,       {"r","middle","distal"},         {"left"}},
        {VRMBone::RightMiddleDistal,       {"r","middle","3"},              {"left","proximal","inter","1","2"}},
        {VRMBone::RightMiddleDistal,       {"r","middle","c"},              {"left","proximal","inter","a","b"}},

        {VRMBone::RightRingProximal,       {"right","ring","proximal"},     {}},
        {VRMBone::RightRingProximal,       {"r","ring","proximal"},         {"left"}},
        {VRMBone::RightRingProximal,       {"r","ring","1"},                {"left","distal","2","3","tip"}},
        {VRMBone::RightRingProximal,       {"r","ring","a"},                {"left","b","c","2","3","tip"}},
        {VRMBone::RightRingIntermediate,   {"right","ring","intermediate"}, {}},
        {VRMBone::RightRingIntermediate,   {"r","ring","intermediate"},     {"left"}},
        {VRMBone::RightRingIntermediate,   {"r","ring","2"},                {"left","distal","1","3","tip"}},
        {VRMBone::RightRingIntermediate,   {"r","ring","b"},                {"left","a","c","1","3","tip"}},
        {VRMBone::RightRingDistal,         {"right","ring","distal"},       {}},
        {VRMBone::RightRingDistal,         {"r","ring","distal"},           {"left"}},
        {VRMBone::RightRingDistal,         {"r","ring","3"},                {"left","proximal","inter","1","2"}},
        {VRMBone::RightRingDistal,         {"r","ring","c"},                {"left","proximal","inter","a","b"}},

        {VRMBone::RightLittleProximal,     {"right","little","proximal"},   {}},
        {VRMBone::RightLittleProximal,     {"right","pinky","proximal"},    {}},
        {VRMBone::RightLittleProximal,     {"r","little","proximal"},       {"left"}},
        {VRMBone::RightLittleProximal,     {"r","pinky","proximal"},        {"left"}},
        {VRMBone::RightLittleProximal,     {"r","little","1"},              {"left","distal","2","3","tip"}},
        {VRMBone::RightLittleProximal,     {"r","pinky","1"},               {"left","distal","2","3","tip"}},
        {VRMBone::RightLittleProximal,     {"r","little","a"},              {"left","b","c","2","3","tip"}},
        {VRMBone::RightLittleProximal,     {"r","pinky","a"},               {"left","b","c","2","3","tip"}},
        {VRMBone::RightLittleIntermediate, {"right","little","intermediate"},{}},
        {VRMBone::RightLittleIntermediate, {"right","pinky","intermediate"},{}},
        {VRMBone::RightLittleIntermediate, {"r","little","intermediate"},   {"left"}},
        {VRMBone::RightLittleIntermediate, {"r","pinky","intermediate"},    {"left"}},
        {VRMBone::RightLittleIntermediate, {"r","little","2"},              {"left","distal","1","3","tip"}},
        {VRMBone::RightLittleIntermediate, {"r","pinky","2"},               {"left","distal","1","3","tip"}},
        {VRMBone::RightLittleIntermediate, {"r","little","b"},              {"left","a","c","1","3","tip"}},
        {VRMBone::RightLittleIntermediate, {"r","pinky","b"},               {"left","a","c","1","3","tip"}},
        {VRMBone::RightLittleDistal,       {"right","little","distal"},     {}},
        {VRMBone::RightLittleDistal,       {"right","pinky","distal"},      {}},
        {VRMBone::RightLittleDistal,       {"r","little","distal"},         {"left"}},
        {VRMBone::RightLittleDistal,       {"r","pinky","distal"},          {"left"}},
        {VRMBone::RightLittleDistal,       {"r","little","3"},              {"left","proximal","inter","1","2"}},
        {VRMBone::RightLittleDistal,       {"r","pinky","3"},               {"left","proximal","inter","1","2"}},
        {VRMBone::RightLittleDistal,       {"r","little","c"},              {"left","proximal","inter","a","b"}},
        {VRMBone::RightLittleDistal,       {"r","pinky","c"},               {"left","proximal","inter","a","b"}},
    };

    // Build lowercase bone name list once
    std::vector<std::pair<std::string, int>> boneNamesLower;
    boneNamesLower.reserve(impl_->boneByName.size());
    for (const auto& [name, idx] : impl_->boneByName)
        boneNamesLower.emplace_back(toLower(name), idx);

    // Track which bone indices were already matched to avoid double-assignment
    std::unordered_set<int> usedBoneIndices;
    for (auto& [vb, bi] : impl_->vrmBoneIndex) usedBoneIndices.insert(bi);

    for (const auto& rule : kFuzzyRules) {
        if (impl_->vrmBoneIndex.count(rule.bone)) continue; // already mapped

        for (const auto& [nameLower, boneIdx] : boneNamesLower) {
            if (usedBoneIndices.count(boneIdx)) continue; // already used

            // Check all required keywords
            bool allRequired = true;
            for (const auto& req : rule.required)
                if (nameLower.find(req) == std::string::npos) { allRequired = false; break; }
            if (!allRequired) continue;

            // Check forbidden keywords
            bool anyForbidden = false;
            for (const auto& forb : rule.forbidden)
                if (nameLower.find(forb) != std::string::npos) { anyForbidden = true; break; }
            if (anyForbidden) continue;

            // Match!
            impl_->vrmBoneIndex[rule.bone] = boneIdx;
            usedBoneIndices.insert(boneIdx);
            break;
        }
    }

    // Log summary of mapped bones
    spdlog::info("[ModelRenderer] Mapped {}/{} VRM bones",
        impl_->vrmBoneIndex.size(), (int)VRMBone::Count);

    impl_->boneMatrices.assign(std::max((size_t)1, impl_->bones.size()), glm::mat4(1.f));

    // ── Create UBOs ────────────────────────────────────────────────────────
    rhi::BufferDesc boneUbDesc;
    boneUbDesc.size = 512 * sizeof(glm::mat4);
    boneUbDesc.usage = rhi::BufferUsage::Uniform;
    boneUbDesc.memory = rhi::MemoryType::HostVisible;
    impl_->boneUBO = rhi_.createBuffer(boneUbDesc);

    rhi::BufferDesc vpUbDesc;
    vpUbDesc.size = sizeof(ViewProjBlock);
    vpUbDesc.usage = rhi::BufferUsage::Uniform;
    vpUbDesc.memory = rhi::MemoryType::HostVisible;
    impl_->viewProjUB = rhi_.createBuffer(vpUbDesc);

    loadProgress_ = 0.5f;

    // ── Upload meshes ───────────────────────────────────────────────────────
    constexpr int STRIDE = 16;

    for (int ni = 0; ni < (int)model.nodes.size(); ++ni) {
        int meshIdx = impl_->nodes[ni].meshIndex;
        if (meshIdx < 0) continue;
        const auto& mesh = model.meshes[meshIdx];

        for (int pi = 0; pi < (int)mesh.primitives.size(); ++pi) {
            const auto& prim = mesh.primitives[pi];
            if (prim.mode != TINYGLTF_MODE_TRIANGLES) continue;

            auto getAcc = [&](const char* sem) -> int {
                auto it = prim.attributes.find(sem);
                return (it != prim.attributes.end()) ? it->second : -1;
            };

            int posAcc    = getAcc("POSITION");
            int normAcc   = getAcc("NORMAL");
            int uvAcc     = getAcc("TEXCOORD_0");
            int jointAcc  = getAcc("JOINTS_0");
            int weightAcc = getAcc("WEIGHTS_0");

            if (posAcc < 0) continue;
            size_t count = model.accessors[posAcc].count;

            const float* pos    = accessorFloatPtr(model, posAcc);
            const float* norms  = accessorFloatPtr(model, normAcc);
            const float* uvs    = accessorFloatPtr(model, uvAcc);

            size_t jStride = 0, jCount = 0, wStride = 0, wCount = 0;
            const uint8_t* jointRaw  = accessorRawPtr(model, jointAcc,  jStride, jCount);
            const uint8_t* weightRaw = accessorRawPtr(model, weightAcc, wStride, wCount);
            int jointCompType = (jointAcc >= 0) ? model.accessors[jointAcc].componentType : 0;

            std::vector<float> vdata;
            vdata.reserve(count * STRIDE);

            for (size_t vi = 0; vi < count; ++vi) {
                // Position
                if (pos) { vdata.push_back(pos[vi*3+0]); vdata.push_back(pos[vi*3+1]); vdata.push_back(pos[vi*3+2]); }
                else      { vdata.push_back(0); vdata.push_back(0); vdata.push_back(0); }
                // Normal
                if (norms){ vdata.push_back(norms[vi*3+0]); vdata.push_back(norms[vi*3+1]); vdata.push_back(norms[vi*3+2]); }
                else       { vdata.push_back(0); vdata.push_back(1); vdata.push_back(0); }
                // UV
                if (uvs)  { vdata.push_back(uvs[vi*2+0]); vdata.push_back(uvs[vi*2+1]); }
                else       { vdata.push_back(0); vdata.push_back(0); }
                // Joints
                if (jointRaw && jStride > 0) {
                    const uint8_t* jPtr = jointRaw + vi * jStride;
                    if (jointCompType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        vdata.push_back((float)jPtr[0]); vdata.push_back((float)jPtr[1]);
                        vdata.push_back((float)jPtr[2]); vdata.push_back((float)jPtr[3]);
                    } else {
                        const uint16_t* j16 = reinterpret_cast<const uint16_t*>(jPtr);
                        vdata.push_back((float)j16[0]); vdata.push_back((float)j16[1]);
                        vdata.push_back((float)j16[2]); vdata.push_back((float)j16[3]);
                    }
                } else { vdata.push_back(0); vdata.push_back(0); vdata.push_back(0); vdata.push_back(0); }
                // Weights
                if (weightRaw && wStride > 0) {
                    const float* wPtr = reinterpret_cast<const float*>(weightRaw + vi * wStride);
                    vdata.push_back(wPtr[0]); vdata.push_back(wPtr[1]);
                    vdata.push_back(wPtr[2]); vdata.push_back(wPtr[3]);
                } else { vdata.push_back(1); vdata.push_back(0); vdata.push_back(0); vdata.push_back(0); }
            }

            // Index buffer
            std::vector<uint32_t> indices;
            if (prim.indices >= 0) {
                const auto& idxAcc = model.accessors[prim.indices];
                const auto& idxBV  = model.bufferViews[idxAcc.bufferView];
                const auto& idxBuf = model.buffers[idxBV.buffer];
                const uint8_t* iptr = idxBuf.data.data() + idxBV.byteOffset + idxAcc.byteOffset;
                indices.reserve(idxAcc.count);
                for (size_t ii = 0; ii < idxAcc.count; ++ii) {
                    if (idxAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
                        indices.push_back(*reinterpret_cast<const uint32_t*>(iptr + ii*4));
                    else if (idxAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                        indices.push_back(*reinterpret_cast<const uint16_t*>(iptr + ii*2));
                    else
                        indices.push_back((uint32_t)iptr[ii]);
                }
            }

            // GPU Uploads & Morph Targets Setup
            MeshPrimitive mp{};
            mp.materialIdx  = prim.material;
            mp.vertexCount  = (uint32_t)count;
            mp.indexCount   = (uint32_t)indices.size();
            mp.skinIdx      = impl_->nodes[ni].skinIndex;
            mp.pipeline     = impl_->pipeline;
            mp.meshIdx      = meshIdx;
            mp.baseVertexData = vdata;

            // Load morph target position deltas
            for (int ti = 0; ti < (int)prim.targets.size(); ++ti) {
                int targetPosAcc = -1;
                auto posIt = prim.targets[ti].find("POSITION");
                if (posIt != prim.targets[ti].end()) {
                    targetPosAcc = posIt->second;
                }
                const float* targetPosPtr = accessorFloatPtr(model, targetPosAcc);
                if (targetPosPtr) {
                    PrimitiveMorphTarget pmt;
                    pmt.targetIndex = ti;
                    pmt.positionDeltas.resize(count);
                    for (size_t vi = 0; vi < count; ++vi) {
                        pmt.positionDeltas[vi] = {
                            targetPosPtr[vi*3+0],
                            targetPosPtr[vi*3+1],
                            targetPosPtr[vi*3+2]
                        };
                    }
                    mp.morphTargets.push_back(std::move(pmt));
                }
            }

            rhi::BufferDesc vbDesc;
            vbDesc.size = vdata.size() * sizeof(float);
            vbDesc.usage = rhi::BufferUsage::Vertex;
            // Use HostVisible memory for vertex buffers that will be updated per frame by CPU morphing
            vbDesc.memory = mp.morphTargets.empty() ? rhi::MemoryType::DeviceLocal : rhi::MemoryType::HostVisible;
            vbDesc.initialData = vdata.data();
            mp.vertexBuffer = rhi_.createBuffer(vbDesc);

            if (!indices.empty()) {
                rhi::BufferDesc ibDesc;
                ibDesc.size = indices.size() * sizeof(uint32_t);
                ibDesc.usage = rhi::BufferUsage::Index;
                ibDesc.memory = rhi::MemoryType::DeviceLocal;
                ibDesc.initialData = indices.data();
                mp.indexBuffer = rhi_.createBuffer(ibDesc);
            }

            std::vector<SparseMorphTarget> sparseTargets;
            for (const auto& target : mp.morphTargets) {
                SparseMorphTarget smt;
                smt.targetIndex = target.targetIndex;
                for (size_t vi = 0; vi < mp.vertexCount; ++vi) {
                    const auto& d = target.positionDeltas[vi];
                    if (std::abs(d.x) > 1e-5f || std::abs(d.y) > 1e-5f || std::abs(d.z) > 1e-5f) {
                        smt.vertexIndices.push_back((uint32_t)vi);
                        smt.positionDeltas.push_back(d);
                    }
                }
                sparseTargets.push_back(std::move(smt));
            }
            impl_->sparseMorphTargets.push_back(std::move(sparseTargets));
            impl_->lastAppliedMeshWeights.assign(impl_->meshes.size(), {});

            impl_->meshes.push_back(std::move(mp));
        }
    }

    // Subsample and cache collision vertices once during model loading
    impl_->collisionVertices.clear();
    for (size_t mi = 0; mi < impl_->meshes.size(); ++mi) {
        const auto& mp = impl_->meshes[mi];
        if (mp.baseVertexData.empty()) continue;

        // Subsample body vertices to keep runtime collision checks extremely fast (e.g. check every 32nd vertex)
        constexpr size_t strideFloats = 16;
        size_t step = 32;
        for (size_t vi = 0; vi < mp.vertexCount; vi += step) {
            size_t idx = vi * strideFloats;
            if (idx + 15 >= mp.baseVertexData.size()) continue;

            CollisionVertex cv;
            cv.pos = glm::vec3(mp.baseVertexData[idx + 0], mp.baseVertexData[idx + 1], mp.baseVertexData[idx + 2]);
            cv.joints[0] = (int)mp.baseVertexData[idx + 8];
            cv.joints[1] = (int)mp.baseVertexData[idx + 9];
            cv.joints[2] = (int)mp.baseVertexData[idx + 10];
            cv.joints[3] = (int)mp.baseVertexData[idx + 11];
            cv.weights[0] = mp.baseVertexData[idx + 12];
            cv.weights[1] = mp.baseVertexData[idx + 13];
            cv.weights[2] = mp.baseVertexData[idx + 14];
            cv.weights[3] = mp.baseVertexData[idx + 15];
            cv.primitiveIdx = (int)mi;

            impl_->collisionVertices.push_back(cv);
        }
    }
    spdlog::info("[ModelRenderer] Generated {} collision vertices for complex mesh-based collision", impl_->collisionVertices.size());

    loadProgress_ = 1.0f;
    modelLoaded_  = true;
    impl_->gltfModel = std::move(model);

    spdlog::info("[ModelRenderer] ✓ Loaded: {} ({} meshes, {} bones)",
                 modelName_, impl_->meshes.size(), impl_->bones.size());
    return true;
}

void ModelRenderer::unloadModel() {
    impl_->clear();
    modelLoaded_  = false;
    modelName_.clear();
    loadProgress_ = 0.f;
    spdlog::info("[ModelRenderer] Model unloaded");
}

void ModelRenderer::addPhysicsChain(const std::string& rootName, const PhysicsParams& params) {
    if (!modelLoaded_) return;
    auto it = impl_->boneByName.find(rootName);
    if (it == impl_->boneByName.end()) {
        spdlog::warn("[ModelRenderer] Cannot add physics chain: bone '{}' not found", rootName);
        return;
    }

    PhysicsChainState chain;
    chain.rootName = rootName;
    chain.params = params;

    // Traverse recursively to build the chain
    std::function<void(int)> buildChain = [&](int boneIdx) {
        PhysicsBoneState pbs;
        pbs.boneIdx = boneIdx;
        
        // Find child translation to set rest bone length
        float length = 0.05f; // default fallback length
        for (int i = 0; i < (int)impl_->bones.size(); ++i) {
            if (impl_->bones[i].parentIdx == boneIdx) {
                glm::vec3 trans = glm::vec3(impl_->bones[i].restLocal[3]);
                float d = glm::length(trans);
                if (d > 0.001f) {
                    length = d;
                    break;
                }
            }
        }
        pbs.boneLength = length;
        pbs.currPosition = glm::vec3(0.f);
        pbs.prevPosition = glm::vec3(0.f);
        chain.bones.push_back(pbs);

        // Find child bones in the skeleton
        std::vector<int> children;
        for (int i = 0; i < (int)impl_->bones.size(); ++i) {
            if (impl_->bones[i].parentIdx == boneIdx) {
                children.push_back(i);
            }
        }
        // For physics strands (like hair strands, ears), follow the single child path
        if (!children.empty()) {
            buildChain(children[0]);
        }
    };

    buildChain(it->second);
    
    // Remove duplicate configuration if already exists
    impl_->physicsChains.erase(
        std::remove_if(impl_->physicsChains.begin(), impl_->physicsChains.end(),
            [&](const PhysicsChainState& c) { return c.rootName == rootName; }),
        impl_->physicsChains.end());

    impl_->physicsChains.push_back(chain);
    spdlog::info("[ModelRenderer] Added physics chain for root '{}' with {} bones", rootName, chain.bones.size());
}

void ModelRenderer::clearPhysicsChains() {
    impl_->physicsChains.clear();
    spdlog::info("[ModelRenderer] Cleared all physics chains");
}

std::vector<ModelRenderer::PhysicsBoneConfig> ModelRenderer::getPhysicsChains() const {
    std::vector<PhysicsBoneConfig> configs;
    for (const auto& chain : impl_->physicsChains) {
        configs.push_back({ chain.rootName, chain.params });
    }
    return configs;
}

void ModelRenderer::addCollider(const PhysicsCollider& col) {
    // Remove if duplicate exists
    impl_->colliders.erase(
        std::remove_if(impl_->colliders.begin(), impl_->colliders.end(),
            [&](const PhysicsCollider& c) { return c.boneName == col.boneName && glm::distance(c.offset, col.offset) < 0.001f; }),
        impl_->colliders.end());
    impl_->colliders.push_back(col);
    spdlog::info("[ModelRenderer] Added physics collider sphere on bone '{}' radius={:.3f}", col.boneName, col.radius);
}

void ModelRenderer::clearColliders() {
    impl_->colliders.clear();
    spdlog::info("[ModelRenderer] Cleared all physics colliders");
}

std::vector<ModelRenderer::PhysicsCollider> ModelRenderer::getColliders() const {
    return impl_->colliders;
}

const std::vector<glm::vec3>& ModelRenderer::getSkinnedCollisionPositions() const {
    return impl_->skinnedCollisionPositions;
}

// ─── Rig ────────────────────────────────────────────────────────────────────
const std::vector<BoneTransform>& ModelRenderer::getBoneList() const {
    return impl_->bones;
}

glm::mat4 ModelRenderer::getBoneTransform(const std::string& name) const {
    auto it = impl_->boneByName.find(name);
    if (it == impl_->boneByName.end()) return glm::mat4(1.f);
    return impl_->bones[it->second].world;
}

void ModelRenderer::setBoneOverride(const std::string& name, const glm::mat4& t) {
    impl_->boneOverrides[name] = t;
}

void ModelRenderer::clearBoneOverride(const std::string& name) {
    impl_->boneOverrides.erase(name);
}

void ModelRenderer::clearAllBoneOverrides() {
    impl_->boneOverrides.clear();
}

// ─── Bone matrix computation ─────────────────────────────────────────────────
void ModelRenderer::computeBoneMatrices() {
    auto& bones  = impl_->bones;
    if (bones.empty()) return;

    // Track which bones have already been resolved this frame
    std::vector<bool> resolved(bones.size(), false);

    // Recursive helper to resolve parent before child
    std::function<void(int)> resolveWorldMatrix = [&](int index) {
        if (index < 0 || index >= (int)bones.size() || resolved[index]) return;

        auto& b = bones[index];
        auto ovIt = impl_->boneOverrides.find(b.name);
        glm::mat4 local = (ovIt != impl_->boneOverrides.end()) ? ovIt->second : b.local;

        if (b.parentIdx < 0 || b.parentIdx >= (int)bones.size()) {
            b.world = local;
        } else {
            // Guarantee parent is resolved first
            resolveWorldMatrix(b.parentIdx);
            b.world = bones[b.parentIdx].world * local;
        }
        resolved[index] = true;
    };

    for (int i = 0; i < (int)bones.size(); ++i) {
        resolveWorldMatrix(i);
    }

    impl_->boneMatrices.resize(std::min((int)bones.size(), 512));
    for (int i = 0; i < (int)impl_->boneMatrices.size(); ++i)
        impl_->boneMatrices[i] = bones[i].world * bones[i].invBind;
}

static glm::mat4 applyRotationToLocal(const glm::mat4& originalLocal, const glm::quat& deltaRot) {
    // Extract translation and scale from the rest local matrix
    glm::vec3 translation = glm::vec3(originalLocal[3]);
    glm::vec3 scale;
    scale.x = glm::length(glm::vec3(originalLocal[0]));
    scale.y = glm::length(glm::vec3(originalLocal[1]));
    scale.z = glm::length(glm::vec3(originalLocal[2]));

    // deltaRot is computed in parent space and IS the full intended local rotation.
    // Do NOT multiply with restRot — that double-applies the rest and causes twisting.
    glm::mat4 T = glm::translate(glm::mat4(1.f), translation);
    glm::mat4 R = glm::mat4_cast(deltaRot);
    glm::mat4 S = glm::scale(glm::mat4(1.f), scale);
    return T * R * S;
}

static glm::mat4 applyRotationMatrixToLocal(const glm::mat4& originalLocal, const glm::mat4& rotOnly) {
    glm::vec3 translation = glm::vec3(originalLocal[3]);
    glm::vec3 scale;
    scale.x = glm::length(glm::vec3(originalLocal[0]));
    scale.y = glm::length(glm::vec3(originalLocal[1]));
    scale.z = glm::length(glm::vec3(originalLocal[2]));

    // Extract existing rotation matrix from the rest pose
    glm::mat3 rotMat;
    rotMat[0] = scale.x > 0.0001f ? glm::vec3(originalLocal[0]) / scale.x : glm::vec3(1,0,0);
    rotMat[1] = scale.y > 0.0001f ? glm::vec3(originalLocal[1]) / scale.y : glm::vec3(0,1,0);
    rotMat[2] = scale.z > 0.0001f ? glm::vec3(originalLocal[2]) / scale.z : glm::vec3(0,0,1);
    glm::mat4 restRot = glm::mat4(rotMat);

    // Compose: apply rotOnly in PARENT space (pre-multiply) to correctly align hand bones
    glm::mat4 finalRot = rotOnly * restRot;

    glm::mat4 T = glm::translate(glm::mat4(1.f), translation);
    glm::mat4 S = glm::scale(glm::mat4(1.f), scale);
    return T * finalRot * S;
}

// ─── Update from tracking ────────────────────────────────────────────────────
void ModelRenderer::updateFromTracking(const tracking::TrackingFrame& frame, float dt) {
    if (!modelLoaded_) return;

    // Save previous world transforms for physics delta tracking
    std::vector<glm::mat4> prevWorlds(impl_->bones.size());
    for (size_t i = 0; i < impl_->bones.size(); ++i) {
        prevWorlds[i] = impl_->bones[i].world;
    }

    // Helper: get the current world rotation of a bone's parent
    std::function<glm::quat(int)> getBoneWorldRot = [&](int idx) -> glm::quat {
        if (idx < 0 || idx >= (int)impl_->bones.size()) return glm::quat(1.f, 0.f, 0.f, 0.f);
        glm::mat4 local = impl_->bones[idx].local;
        glm::vec3 scale;
        scale.x = glm::length(glm::vec3(local[0]));
        scale.y = glm::length(glm::vec3(local[1]));
        scale.z = glm::length(glm::vec3(local[2]));

        glm::mat3 rotMat;
        rotMat[0] = scale.x > 0.0001f ? glm::vec3(local[0]) / scale.x : glm::vec3(1,0,0);
        rotMat[1] = scale.y > 0.0001f ? glm::vec3(local[1]) / scale.y : glm::vec3(0,1,0);
        rotMat[2] = scale.z > 0.0001f ? glm::vec3(local[2]) / scale.z : glm::vec3(0,0,1);
        glm::quat localRot = glm::quat_cast(rotMat);

        int parent = impl_->bones[idx].parentIdx;
        if (parent < 0) return localRot;
        return getBoneWorldRot(parent) * localRot;
    };
    auto getParentWorldRot = [&](int bi) -> glm::quat {
        if (bi < 0 || bi >= (int)impl_->bones.size()) return glm::quat(1.f, 0.f, 0.f, 0.f);
        return getBoneWorldRot(impl_->bones[bi].parentIdx);
    };

    // Helper to find parent-to-child rest direction in parent-bone local space.
    auto getBoneRestDir = [&](int bi) -> glm::vec3 {
        // Find the first child bone in the skin hierarchy
        int childIdx = -1;
        for (int i = 0; i < (int)impl_->bones.size(); ++i) {
            if (impl_->bones[i].parentIdx == bi) {
                childIdx = i;
                break;
            }
        }
        if (childIdx >= 0) {
            // childTrans is the child's offset in the PARENT bone's local space — use directly
            glm::vec3 childTrans = glm::vec3(impl_->bones[childIdx].restLocal[3]);
            if (glm::length(childTrans) > 0.001f)
                return glm::normalize(childTrans);
        }
        // Fallback: use the X-axis of the parent's rest local matrix (bone pointing direction)
        glm::vec3 restX = glm::vec3(impl_->bones[bi].restLocal[0]);
        if (glm::length(restX) < 0.001f) restX = glm::vec3(1, 0, 0);
        return glm::normalize(restX);
    };

    // ── Periodic debug log (every ~120 frames) ────────────────────────────────
    static int dbgCounter = 0;
    if (++dbgCounter >= 120) {
        dbgCounter = 0;
        spdlog::info("[Tracking] headEuler={} (Y={:.1f} P={:.1f} R={:.1f})  pose={}  head_mapped={}",
            frame.hasHeadEuler() ? "yes" : "no",
            frame.headEuler.yaw, frame.headEuler.pitch, frame.headEuler.roll,
            frame.hasWorldPose() ? "yes" : "no",
            impl_->vrmBoneIndex.count(VRMBone::Head) ? "yes" : "NO");
        if (frame.hasWorldPose()) {
            spdlog::info("[Tracking] poseWorld size={}, leftShoulder vis={:.2f}, leftElbow vis={:.2f}",
                frame.poseWorld.size(),
                frame.poseWorld.size() > 11 ? frame.poseWorld[11].visibility : -1.f,
                frame.poseWorld.size() > 13 ? frame.poseWorld[13].visibility : -1.f);
        }
    }

    // ── Reset all bone locals to rest pose each frame ────────────────────────
    // This is CRITICAL: prevents rotation accumulation across frames.
    for (auto& b : impl_->bones)
        b.local = b.restLocal;

    // ── Blendshapes ──────────────────────────────────────────────────────────
    if (autoBlendShapes_ && frame.hasBlendshapes()) {
        for (const auto& mapping : blendShapeMappings_) {
            float w = frame.getBlendshape(mapping.trackerName, -1.f);
            if (w >= 0.f) {
                setBlendShapeWeight(mapping.trackerName, w);
                setBlendShapeWeight(mapping.modelName, w);
            }
        }
    }
    if (autoBlendShapes_ && frame.blinkScore > 0.f && !frame.hasBlendshapes()) {
        setBlendShapeWeight("blink", frame.blinkScore);
        setBlendShapeWeight("blink_left", frame.blinkScore);
        setBlendShapeWeight("blink_right", frame.blinkScore);
        setBlendShapeWeight("eyeBlinkLeft", frame.blinkScore);
        setBlendShapeWeight("eyeBlinkRight", frame.blinkScore);
        setBlendShapeWeight("eye_blink_L", frame.blinkScore);
        setBlendShapeWeight("eye_blink_R", frame.blinkScore);
    }

    // ── Head rotation ────────────────────────────────────────────────────────
    if (frame.hasHeadEuler()) {
        auto wrapAngle = [](float angle) {
            while (angle > 180.f) angle -= 360.f;
            while (angle < -180.f) angle += 360.f;
            return angle;
        };

        float yawDeg   = frame.headEuler.yaw;
        float pitchDeg = frame.headEuler.pitch;
        float rollDeg  = frame.headEuler.roll;

        // If yaw/roll are near 180 (flipped), shift them to near 0
        if (std::abs(yawDeg) > 90.f) {
            yawDeg = wrapAngle(yawDeg - 180.f);
        }
        if (std::abs(rollDeg) > 90.f) {
            rollDeg = wrapAngle(rollDeg - 180.f);
        }

        // Physiologically realistic head/neck constraints
        yawDeg   = glm::clamp(yawDeg, -60.f, 60.f);
        pitchDeg = glm::clamp(pitchDeg, -35.f, 30.f);
        rollDeg  = glm::clamp(rollDeg, -40.f, 40.f);

        float yaw   = glm::radians(yawDeg);
        float pitch = glm::radians(pitchDeg);
        float roll  = glm::radians(rollDeg);

        // Unify rotation on Neck (if present) or Head (if Neck not present)
        // to prevent 2x speed differences and desync between different parts of the head/face mesh.
        auto nIt = impl_->vrmBoneIndex.find(VRMBone::Neck);
        auto hIt = impl_->vrmBoneIndex.find(VRMBone::Head);
        glm::quat q = glm::quat(glm::vec3(-pitch, yaw, -roll));

        if (nIt != impl_->vrmBoneIndex.end()) {
            int neckBi = nIt->second;
            impl_->bones[neckBi].local = applyRotationToLocal(impl_->bones[neckBi].restLocal, q);
            if (hIt != impl_->vrmBoneIndex.end()) {
                int headBi = hIt->second;
                impl_->bones[headBi].local = impl_->bones[headBi].restLocal; // Keep rest pose local to Neck
            }
        } else if (hIt != impl_->vrmBoneIndex.end()) {
            int headBi = hIt->second;
            impl_->bones[headBi].local = applyRotationToLocal(impl_->bones[headBi].restLocal, q);
        }

        // Jaw bone fallback driver (driven by jawOpen blendshape, max 15 degrees pitch)
        auto jIt = impl_->vrmBoneIndex.find(VRMBone::Jaw);
        if (jIt != impl_->vrmBoneIndex.end()) {
            int bi = jIt->second;
            float openWeight = frame.getBlendshape("jawOpen", 0.f);
            float jawPitch = glm::radians(openWeight * 15.f); // clamp to 15 degrees max
            glm::quat q = glm::quat(glm::vec3(jawPitch, 0.f, 0.f));
            impl_->bones[bi].local = applyRotationToLocal(impl_->bones[bi].restLocal, q);
        }
    }

    // ── Upper-body pose (shoulders → elbows → wrists) ────────────────────────
    if (armTrackingEnabled_) {
        using namespace tracking;


        // Robust Arm IK Solver using Swing-Twist decomposition to prevent twisting artifacts
        auto applyArmIK = [&](VRMBone upperEnum, VRMBone lowerEnum, glm::vec3 shoulderPos, glm::vec3 elbowPos, glm::vec3 wristPos, glm::vec3 restUp) {
            auto uIt = impl_->vrmBoneIndex.find(upperEnum);
            auto lIt = impl_->vrmBoneIndex.find(lowerEnum);
            if (uIt == impl_->vrmBoneIndex.end() || lIt == impl_->vrmBoneIndex.end()) return;
            int uBi = uIt->second;
            int lBi = lIt->second;

            // 1. Upper Arm
            glm::vec3 upperDir = shoulderPos - elbowPos;
            glm::vec3 lowerDir = elbowPos - wristPos;
            if (glm::length(upperDir) < 0.001f || glm::length(lowerDir) < 0.001f) return;
            upperDir = glm::normalize(upperDir);
            lowerDir = glm::normalize(lowerDir);
            
            static bool loggedParent = false;
            if (!loggedParent) {
                loggedParent = true;
                spdlog::info("[IK Debug] UpperArm: name={}, parentIdx={}, parentName={}",
                    impl_->bones[uBi].name, impl_->bones[uBi].parentIdx,
                    impl_->bones[uBi].parentIdx >= 0 ? impl_->bones[impl_->bones[uBi].parentIdx].name : "None");
                spdlog::info("[IK Debug] LowerArm: name={}, parentIdx={}, parentName={}",
                    impl_->bones[lBi].name, impl_->bones[lBi].parentIdx,
                    impl_->bones[lBi].parentIdx >= 0 ? impl_->bones[impl_->bones[lBi].parentIdx].name : "None");
            }

            // Helper to extract the rest rotation quaternion of a bone from its rest local matrix
            auto getBoneRestRot = [&](int idx) -> glm::quat {
                const glm::mat4& originalLocal = impl_->bones[idx].restLocal;
                glm::vec3 scale;
                scale.x = glm::length(glm::vec3(originalLocal[0]));
                scale.y = glm::length(glm::vec3(originalLocal[1]));
                scale.z = glm::length(glm::vec3(originalLocal[2]));
                glm::mat3 rotMat;
                rotMat[0] = scale.x > 0.0001f ? glm::vec3(originalLocal[0]) / scale.x : glm::vec3(1,0,0);
                rotMat[1] = scale.y > 0.0001f ? glm::vec3(originalLocal[1]) / scale.y : glm::vec3(0,1,0);
                rotMat[2] = scale.z > 0.0001f ? glm::vec3(originalLocal[2]) / scale.z : glm::vec3(0,0,1);
                return glm::quat_cast(rotMat);
            };

            // Upper Arm Rotation
            glm::quat uRestRot = getBoneRestRot(uBi);
            glm::vec3 uRestDirParent = uRestRot * getBoneRestDir(uBi);
            glm::quat uParentWorldRot = getParentWorldRot(uBi);

            // Transform target world-space directions into parent-bone local space
            glm::vec3 targetDirLocal = glm::inverse(uParentWorldRot) * upperDir;

            // Swing: rotate restDir in parent space onto targetDir (both in parent local space)
            glm::quat swing;
            if (glm::dot(uRestDirParent, targetDirLocal) < -0.9999f) {
                glm::vec3 perp = std::abs(uRestDirParent.x) < 0.9f ? glm::vec3(1,0,0) : glm::vec3(0,1,0);
                perp = glm::normalize(glm::cross(uRestDirParent, perp));
                swing = glm::angleAxis(glm::pi<float>(), perp);
            } else {
                swing = glm::rotation(uRestDirParent, targetDirLocal);
            }

            glm::quat uTargetQ = swing * uRestRot;

            // Slerp Upper Arm
            auto uRotIt = impl_->activeBoneRots.find(uBi);
            glm::quat uSmoothedQ = (uRotIt != impl_->activeBoneRots.end()) ?
                                    glm::slerp(uRotIt->second, uTargetQ, glm::clamp(15.0f * dt, 0.0f, 1.0f)) : uTargetQ;
            impl_->activeBoneRots[uBi] = uSmoothedQ;
            impl_->bones[uBi].local = applyRotationToLocal(impl_->bones[uBi].restLocal, uSmoothedQ);
            impl_->activeBoneLocals[uBi] = impl_->bones[uBi].local;
            impl_->boneOverrides[impl_->bones[uBi].name] = impl_->bones[uBi].local;

            // Update upper arm world matrix so lower arm resolves relative to it
            impl_->bones[uBi].world = (impl_->bones[uBi].parentIdx >= 0
                ? impl_->bones[impl_->bones[uBi].parentIdx].world : glm::mat4(1.f))
                * impl_->bones[uBi].local;

            // 2. Lower Arm — 1-DOF hinge around restUp axis (prevents elbow twist)
            glm::vec3 lRestDir = getBoneRestDir(lBi);
            glm::quat lParentWorldRot = getParentWorldRot(lBi);
            glm::vec3 lTargetDirLocal = glm::inverse(lParentWorldRot) * lowerDir;

            // Hinge axis is restUp expressed in the lower arm's parent (upper arm) local space
            glm::vec3 h = glm::normalize(restUp);

            // Signed angle between rest forearm dir and target dir around h
            float cos_theta = glm::dot(lRestDir, lTargetDirLocal);
            float sin_theta = glm::dot(glm::cross(lRestDir, lTargetDirLocal), h);
            float theta = std::atan2(sin_theta, cos_theta);

            // Clamp to anatomical elbow range [0°, 150°]
            theta = glm::clamp(theta, 0.0f, glm::radians(150.0f));

            glm::quat lTargetQ = glm::angleAxis(theta, h);

            // Slerp Lower Arm
            auto lRotIt = impl_->activeBoneRots.find(lBi);
            glm::quat lSmoothedQ = (lRotIt != impl_->activeBoneRots.end()) ?
                                    glm::slerp(lRotIt->second, lTargetQ, glm::clamp(15.0f * dt, 0.0f, 1.0f)) : lTargetQ;
            impl_->activeBoneRots[lBi] = lSmoothedQ;
            impl_->bones[lBi].local = applyRotationToLocal(impl_->bones[lBi].restLocal, lSmoothedQ);
            impl_->activeBoneLocals[lBi] = impl_->bones[lBi].local;
            impl_->boneOverrides[impl_->bones[lBi].name] = impl_->bones[lBi].local;
        };

        auto restoreFallbackBone = [&](VRMBone b) {
            auto it = impl_->vrmBoneIndex.find(b);
            if (it != impl_->vrmBoneIndex.end()) {
                int bi = it->second;
                auto locIt = impl_->activeBoneLocals.find(bi);
                if (locIt != impl_->activeBoneLocals.end()) {
                    impl_->bones[bi].local = locIt->second;
                    impl_->boneOverrides[impl_->bones[bi].name] = locIt->second;
                } else {
                    impl_->boneOverrides.erase(impl_->bones[bi].name);
                }
            }
        };

        if (frame.hasWorldPose()) {
            // Left Arm (Mirrors User's Right Arm)
            if (frame.poseWorld.size() > PoseLM::LEFT_WRIST &&
                frame.poseWorld[PoseLM::LEFT_SHOULDER].isVisible(0.05f) &&
                frame.poseWorld[PoseLM::LEFT_ELBOW].isVisible(0.05f) &&
                frame.poseWorld[PoseLM::LEFT_WRIST].isVisible(0.05f)) {
                applyArmIK(VRMBone::LeftUpperArm, VRMBone::LeftLowerArm,
                           frame.getPosePoint(PoseLM::LEFT_SHOULDER),
                           frame.getPosePoint(PoseLM::LEFT_ELBOW),
                           frame.getPosePoint(PoseLM::LEFT_WRIST),
                           glm::vec3(0.0f, -1.0f, 0.0f)); // Left arm bends forward => -Y normal (pointing down)
            } else {
                restoreFallbackBone(VRMBone::LeftUpperArm);
                restoreFallbackBone(VRMBone::LeftLowerArm);
            }

            // Right Arm (Mirrors User's Left Arm)
            if (frame.poseWorld.size() > PoseLM::RIGHT_WRIST &&
                frame.poseWorld[PoseLM::RIGHT_SHOULDER].isVisible(0.05f) &&
                frame.poseWorld[PoseLM::RIGHT_ELBOW].isVisible(0.05f) &&
                frame.poseWorld[PoseLM::RIGHT_WRIST].isVisible(0.05f)) {
                applyArmIK(VRMBone::RightUpperArm, VRMBone::RightLowerArm,
                           frame.getPosePoint(PoseLM::RIGHT_SHOULDER),
                           frame.getPosePoint(PoseLM::RIGHT_ELBOW),
                           frame.getPosePoint(PoseLM::RIGHT_WRIST),
                           glm::vec3(0.0f, 1.0f, 0.0f)); // Right arm bends forward => +Y normal (pointing up)
            } else {
                restoreFallbackBone(VRMBone::RightUpperArm);
                restoreFallbackBone(VRMBone::RightLowerArm);
            }
        } else {
            // Pose tracking lost entirely: restore all arms
            for (VRMBone b : {VRMBone::LeftUpperArm, VRMBone::RightUpperArm, VRMBone::LeftLowerArm, VRMBone::RightLowerArm}) {
                restoreFallbackBone(b);
            }
        }
    }

    // ── Hand and Finger tracking ─────────────────────────────────────────
    if (handTrackingEnabled_) {
        auto applyDirToFinger = [&](VRMBone boneEnum, const glm::vec3& dir) {
            auto it = impl_->vrmBoneIndex.find(boneEnum);
            if (it == impl_->vrmBoneIndex.end()) return;
            int bi = it->second;

            glm::vec3 restDir = getBoneRestDir(bi);
            glm::quat parentWorldRot = getParentWorldRot(bi);
            glm::vec3 dir_parent = glm::inverse(parentWorldRot) * dir;

            float dot = glm::dot(restDir, dir_parent);
            glm::quat q;
            if (dot < -0.9999f) {
                glm::vec3 perp = std::abs(restDir.x) < 0.9f ? glm::vec3(1,0,0) : glm::vec3(0,1,0);
                perp = glm::normalize(glm::cross(restDir, perp));
                q = glm::angleAxis(glm::pi<float>(), perp);
            } else {
                q = glm::rotation(restDir, dir_parent);
            }
            impl_->bones[bi].local = applyRotationToLocal(impl_->bones[bi].restLocal, q);
            impl_->activeBoneLocals[bi] = impl_->bones[bi].local;
            impl_->boneOverrides[impl_->bones[bi].name] = impl_->bones[bi].local;
        };

        auto restoreFallbackBone = [&](VRMBone b) {
            auto it = impl_->vrmBoneIndex.find(b);
            if (it != impl_->vrmBoneIndex.end()) {
                int bi = it->second;
                auto locIt = impl_->activeBoneLocals.find(bi);
                if (locIt != impl_->activeBoneLocals.end()) {
                    impl_->bones[bi].local = locIt->second;
                    impl_->boneOverrides[impl_->bones[bi].name] = locIt->second;
                } else {
                    impl_->boneOverrides.erase(impl_->bones[bi].name);
                }
            }
        };

        struct FingerBoneMapping { VRMBone bone; int p1; int p2; };

        // ─── Left Hand & Fingers ───
        if (frame.hasLeftHand()) {
            // Build the hand coordinate frame in world space from MediaPipe landmarks.
            // Wrist=0, Middle MCP=9, Index MCP=5.
            glm::vec3 wrist    = frame.getLeftHandPoint(0);
            glm::vec3 midMcp   = frame.getLeftHandPoint(9);
            glm::vec3 idxMcp   = frame.getLeftHandPoint(5);
            glm::vec3 pinkyMcp = frame.getLeftHandPoint(17);

            // fwd  = wrist → middle-MCP  (longitudinal axis of hand)
            glm::vec3 fwd = glm::normalize(midMcp - wrist);
            // Use pinky→index MCP to define the palm plane
            glm::vec3 across = glm::normalize(idxMcp - pinkyMcp);
            // up   = out of palm surface (points UP when palm is down)
            glm::vec3 up    = glm::normalize(glm::cross(across, fwd));
            // right = re-orthogonalise
            glm::vec3 right = glm::normalize(glm::cross(up, fwd));

            // Rotation matrix: columns are the world-space X, Y, Z basis vectors
            // of the hand frame (X=right, Y=up, Z=fwd for left hand)
            glm::mat3 rotOnly(right, up, fwd);

            auto hIt = impl_->vrmBoneIndex.find(VRMBone::LeftHand);
            if (hIt != impl_->vrmBoneIndex.end()) {
                int bi = hIt->second;
                glm::quat parentWorldRot = getParentWorldRot(bi);
                glm::quat targetWorldRot = glm::quat_cast(rotOnly);
                glm::quat targetLocalRot = glm::inverse(parentWorldRot) * targetWorldRot;

                glm::vec3 translation = glm::vec3(impl_->bones[bi].restLocal[3]);
                glm::vec3 scale;
                scale.x = glm::length(glm::vec3(impl_->bones[bi].restLocal[0]));
                scale.y = glm::length(glm::vec3(impl_->bones[bi].restLocal[1]));
                scale.z = glm::length(glm::vec3(impl_->bones[bi].restLocal[2]));

                glm::mat4 T = glm::translate(glm::mat4(1.f), translation);
                glm::mat4 R = glm::mat4_cast(targetLocalRot);
                glm::mat4 S = glm::scale(glm::mat4(1.f), scale);
                impl_->bones[bi].local = T * R * S;
                impl_->activeBoneLocals[bi] = impl_->bones[bi].local;
                impl_->boneOverrides[impl_->bones[bi].name] = impl_->bones[bi].local;
            }

            // Fingers
            std::vector<FingerBoneMapping> leftFingers = {
                {VRMBone::LeftThumbProximal, 1, 2},
                {VRMBone::LeftThumbIntermediate, 2, 3},
                {VRMBone::LeftThumbDistal, 3, 4},
                {VRMBone::LeftIndexProximal, 5, 6},
                {VRMBone::LeftIndexIntermediate, 6, 7},
                {VRMBone::LeftIndexDistal, 7, 8},
                {VRMBone::LeftMiddleProximal, 9, 10},
                {VRMBone::LeftMiddleIntermediate, 10, 11},
                {VRMBone::LeftMiddleDistal, 11, 12},
                {VRMBone::LeftRingProximal, 13, 14},
                {VRMBone::LeftRingIntermediate, 14, 15},
                {VRMBone::LeftRingDistal, 15, 16},
                {VRMBone::LeftLittleProximal, 17, 18},
                {VRMBone::LeftLittleIntermediate, 18, 19},
                {VRMBone::LeftLittleDistal, 19, 20}
            };
            for (const auto& fm : leftFingers) {
                glm::vec3 p1 = frame.getLeftHandPoint(fm.p1);
                glm::vec3 p2 = frame.getLeftHandPoint(fm.p2);
                if (glm::distance(p1, p2) > 0.001f) {
                    glm::vec3 dir = glm::normalize(p2 - p1);
                    applyDirToFinger(fm.bone, dir);
                } else {
                    restoreFallbackBone(fm.bone);
                }
            }
        } else {
            // Left hand tracking lost: restore
            restoreFallbackBone(VRMBone::LeftHand);
            for (int i = (int)VRMBone::LeftThumbProximal; i <= (int)VRMBone::LeftLittleDistal; ++i) {
                restoreFallbackBone((VRMBone)i);
            }
        }

        // ─── Right Hand & Fingers ───
        if (frame.hasRightHand()) {
            glm::vec3 wrist    = frame.getRightHandPoint(0);
            glm::vec3 midMcp   = frame.getRightHandPoint(9);
            glm::vec3 idxMcp   = frame.getRightHandPoint(5);
            glm::vec3 pinkyMcp = frame.getRightHandPoint(17);

            glm::vec3 fwd = glm::normalize(midMcp - wrist);
            // For the right hand, pinky is on the right side so across goes the other way
            glm::vec3 across = glm::normalize(pinkyMcp - idxMcp);
            glm::vec3 up    = glm::normalize(glm::cross(fwd, across));
            glm::vec3 right = glm::normalize(glm::cross(up, fwd));

            // Right hand: mirror X relative to left hand convention
            glm::mat3 rotOnly(-right, up, fwd);

            auto hIt = impl_->vrmBoneIndex.find(VRMBone::RightHand);
            if (hIt != impl_->vrmBoneIndex.end()) {
                int bi = hIt->second;
                glm::quat parentWorldRot = getParentWorldRot(bi);
                glm::quat targetWorldRot = glm::quat_cast(rotOnly);
                glm::quat targetLocalRot = glm::inverse(parentWorldRot) * targetWorldRot;

                glm::vec3 translation = glm::vec3(impl_->bones[bi].restLocal[3]);
                glm::vec3 scale;
                scale.x = glm::length(glm::vec3(impl_->bones[bi].restLocal[0]));
                scale.y = glm::length(glm::vec3(impl_->bones[bi].restLocal[1]));
                scale.z = glm::length(glm::vec3(impl_->bones[bi].restLocal[2]));

                glm::mat4 T = glm::translate(glm::mat4(1.f), translation);
                glm::mat4 R = glm::mat4_cast(targetLocalRot);
                glm::mat4 S = glm::scale(glm::mat4(1.f), scale);
                impl_->bones[bi].local = T * R * S;
                impl_->activeBoneLocals[bi] = impl_->bones[bi].local;
                impl_->boneOverrides[impl_->bones[bi].name] = impl_->bones[bi].local;
            }

            std::vector<FingerBoneMapping> rightFingers = {
                {VRMBone::RightThumbProximal, 1, 2},
                {VRMBone::RightThumbIntermediate, 2, 3},
                {VRMBone::RightThumbDistal, 3, 4},
                {VRMBone::RightIndexProximal, 5, 6},
                {VRMBone::RightIndexIntermediate, 6, 7},
                {VRMBone::RightIndexDistal, 7, 8},
                {VRMBone::RightMiddleProximal, 9, 10},
                {VRMBone::RightMiddleIntermediate, 10, 11},
                {VRMBone::RightMiddleDistal, 11, 12},
                {VRMBone::RightRingProximal, 13, 14},
                {VRMBone::RightRingIntermediate, 14, 15},
                {VRMBone::RightRingDistal, 15, 16},
                {VRMBone::RightLittleProximal, 17, 18},
                {VRMBone::RightLittleIntermediate, 18, 19},
                {VRMBone::RightLittleDistal, 19, 20}
            };
            for (const auto& fm : rightFingers) {
                glm::vec3 p1 = frame.getRightHandPoint(fm.p1);
                glm::vec3 p2 = frame.getRightHandPoint(fm.p2);
                if (glm::distance(p1, p2) > 0.001f) {
                    glm::vec3 dir = glm::normalize(p2 - p1);
                    applyDirToFinger(fm.bone, dir);
                } else {
                    restoreFallbackBone(fm.bone);
                }
            }
        } else {
            // Right hand tracking lost: restore
            restoreFallbackBone(VRMBone::RightHand);
            for (int i = (int)VRMBone::RightThumbProximal; i <= (int)VRMBone::RightLittleDistal; ++i) {
                restoreFallbackBone((VRMBone)i);
            }
        }
    }

    // Spine/Chest lean: use shoulder midpoint relative to hip midpoint for spine/chest tilt.
    // Also combine this with a percentage of the head rotation for smooth, lifelike upper body movement.
    glm::quat bodyAdditionalRot(1.f, 0.f, 0.f, 0.f);
    if (frame.hasHeadEuler()) {
        // Sway hips & spine/chest slightly in direction of head rotation to feel natural (15% of head rotation)
        float yaw   = glm::radians(glm::clamp(frame.headEuler.yaw, -60.f, 60.f));
        float pitch = glm::radians(glm::clamp(frame.headEuler.pitch, -35.f, 30.f));
        float roll  = glm::radians(glm::clamp(frame.headEuler.roll, -40.f, 40.f));
        bodyAdditionalRot = glm::quat(glm::vec3(-pitch * 0.15f, yaw * 0.15f, -roll * 0.15f));
    }

    if (frame.poseWorld.size() > tracking::PoseLM::RIGHT_HIP &&
            frame.poseWorld[tracking::PoseLM::LEFT_SHOULDER].isVisible(0.4f) &&
            frame.poseWorld[tracking::PoseLM::RIGHT_SHOULDER].isVisible(0.4f) &&
            frame.poseWorld[tracking::PoseLM::LEFT_HIP].isVisible(0.4f) &&
            frame.poseWorld[tracking::PoseLM::RIGHT_HIP].isVisible(0.4f)) {

            glm::vec3 shoulderMid = (frame.getPosePoint(tracking::PoseLM::LEFT_SHOULDER) +
                                     frame.getPosePoint(tracking::PoseLM::RIGHT_SHOULDER)) * 0.5f;
            glm::vec3 hipMid      = (frame.getPosePoint(tracking::PoseLM::LEFT_HIP) +
                                     frame.getPosePoint(tracking::PoseLM::RIGHT_HIP)) * 0.5f;
            glm::vec3 spineDir    = glm::normalize(shoulderMid - hipMid);

            // Compute lean angle from vertical
            float yawLean   = std::atan2(spineDir.x, spineDir.y); // lean left/right
            float pitchLean = std::atan2(-spineDir.z, spineDir.y); // lean fwd/back
            glm::quat poseSpineQ = glm::quat(glm::vec3(pitchLean * 0.5f, yawLean * 0.5f, 0.f));

            // Blend 70% of pose tracking lean and 30% of head-driven body sway to make movement highly organic
            glm::quat finalSpineQ = glm::slerp(poseSpineQ, bodyAdditionalRot, 0.3f);

            for (VRMBone b : {VRMBone::Spine, VRMBone::Chest}) {
                auto it = impl_->vrmBoneIndex.find(b);
                if (it != impl_->vrmBoneIndex.end()) {
                    int bi = it->second;
                    impl_->bones[bi].local = applyRotationToLocal(impl_->bones[bi].restLocal, finalSpineQ);
                }
            }

            // Also tilt Hips/Base slightly (e.g. 50% of the movement) to move the whole model with the lean
            auto hipsIt = impl_->vrmBoneIndex.find(VRMBone::Hips);
            if (hipsIt != impl_->vrmBoneIndex.end()) {
                int bi = hipsIt->second;
                glm::quat hipsQ = glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f), finalSpineQ, 0.5f);
                impl_->bones[bi].local = applyRotationToLocal(impl_->bones[bi].restLocal, hipsQ);
            }
        } else {
            // If pose tracking is unavailable or low confidence, drive the body fully with head-driven sway to keep it alive
            for (VRMBone b : {VRMBone::Spine, VRMBone::Chest}) {
                auto it = impl_->vrmBoneIndex.find(b);
                if (it != impl_->vrmBoneIndex.end()) {
                    int bi = it->second;
                    impl_->bones[bi].local = applyRotationToLocal(impl_->bones[bi].restLocal, bodyAdditionalRot);
                }
            }
            // Hips sway
            auto hipsIt = impl_->vrmBoneIndex.find(VRMBone::Hips);
            if (hipsIt != impl_->vrmBoneIndex.end()) {
                int bi = hipsIt->second;
                glm::quat hipsQ = glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f), bodyAdditionalRot, 0.5f);
                impl_->bones[bi].local = applyRotationToLocal(impl_->bones[bi].restLocal, hipsQ);
            }
        }

    // ── Spring-Bone Physics Simulation ───────────────────────────────────────
    if (physicsEnabled_ && !impl_->physicsChains.empty()) {
        // 1. Temporarily calculate current tracking-driven world transforms of all bones
        // so that physics roots can anchor themselves in tracking world-space.
        computeBoneMatrices();
        impl_->updateInternalColliders();

        // Adjust physics bone positions/velocities for parent bone movement from previous frame to current frame
        for (auto& chain : impl_->physicsChains) {
            for (auto& pb : chain.bones) {
                int boneIdx = pb.boneIdx;
                int parentBoneIdx = impl_->bones[boneIdx].parentIdx;
                
                glm::mat4 prevParentMat = (parentBoneIdx >= 0) ? prevWorlds[parentBoneIdx] : glm::mat4(1.f);
                glm::mat4 currParentMat = (parentBoneIdx >= 0) ? impl_->bones[parentBoneIdx].world : glm::mat4(1.f);
                
                glm::vec3 prevParentPos = glm::vec3(prevParentMat[3]);
                glm::vec3 currParentPos = glm::vec3(currParentMat[3]);
                
                auto getRotationOnly = [](const glm::mat4& m) {
                    glm::vec3 sx = glm::length(glm::vec3(m[0])) > 0.0001f ? glm::normalize(glm::vec3(m[0])) : glm::vec3(1,0,0);
                    glm::vec3 sy = glm::length(glm::vec3(m[1])) > 0.0001f ? glm::normalize(glm::vec3(m[1])) : glm::vec3(0,1,0);
                    glm::vec3 sz = glm::length(glm::vec3(m[2])) > 0.0001f ? glm::normalize(glm::vec3(m[2])) : glm::vec3(0,0,1);
                    return glm::quat_cast(glm::mat3(sx, sy, sz));
                };
                
                glm::quat prevParentRot = getRotationOnly(prevParentMat);
                glm::quat currParentRot = getRotationOnly(currParentMat);
                glm::quat deltaRot = currParentRot * glm::inverse(prevParentRot);
                
                if (glm::length(pb.currPosition) > 0.0001f) {
                    pb.currPosition = currParentPos + deltaRot * (pb.currPosition - prevParentPos);
                    pb.prevPosition = currParentPos + deltaRot * (pb.prevPosition - prevParentPos);
                }
            }
        }

        struct BodySegment {
            glm::vec3 a;
            glm::vec3 b;
            int boneIdxA;
            int boneIdxB;
        };
        std::vector<BodySegment> bodySegments;
        impl_->skinnedCollisionPositions.clear();
        {
            std::unordered_set<int> physicsBoneIndices;
            for (const auto& c : impl_->physicsChains) {
                for (const auto& pb : c.bones) {
                    physicsBoneIndices.insert(pb.boneIdx);
                }
            }
            for (int i = 0; i < (int)impl_->bones.size(); ++i) {
                if (physicsBoneIndices.count(i) > 0) continue;
                int parentIdx = impl_->bones[i].parentIdx;
                if (parentIdx >= 0 && physicsBoneIndices.count(parentIdx) == 0) {
                    glm::vec3 a = glm::vec3(impl_->bones[parentIdx].world[3]);
                    glm::vec3 b = glm::vec3(impl_->bones[i].world[3]);
                    if (glm::distance(a, b) > 0.001f) {
                        bodySegments.push_back({a, b, parentIdx, i});
                    }
                }
            }

            // Populate skinnedCollisionPositions with points along the capsules for visualization
            if (showCollisionDebug_) {
                for (const auto& seg : bodySegments) {
                    float len = glm::distance(seg.a, seg.b);
                    int samples = std::max(2, (int)(len / 0.02f));
                    for (int i = 0; i <= samples; ++i) {
                        float t = (float)i / samples;
                        impl_->skinnedCollisionPositions.push_back(glm::mix(seg.a, seg.b, t));
                    }
                }
            }
        }

        // Sub-stepping accumulator (120Hz)
        impl_->physicsAccumulator += dt;
        float sub_dt = 1.0f / 120.0f;
        int max_steps = 8;
        int step = 0;

        while (impl_->physicsAccumulator >= sub_dt && step < max_steps) {
            impl_->physicsAccumulator -= sub_dt;
            step++;

            impl_->accumWindTime += sub_dt;

            for (auto& chain : impl_->physicsChains) {
                if (chain.bones.empty()) continue;

                const auto& params = chain.params;
                
                // Generate simple dynamic wind force
                float windForce = std::sin(impl_->accumWindTime * 3.f) * params.wind;

                // Update chain elements from top to bottom
                for (size_t i = 0; i < chain.bones.size(); ++i) {
                    auto& pb = chain.bones[i];
                    int boneIdx = pb.boneIdx;
                    int parentBoneIdx = impl_->bones[boneIdx].parentIdx;

                    // Physics base/anchor is the parent's local position (model-local space), or origin if root has no parent
                    glm::vec3 parentLocalPos = (parentBoneIdx >= 0) ? glm::vec3(impl_->bones[parentBoneIdx].world[3]) 
                                                                     : glm::vec3(0.f);

                    // Determine the target/rest direction in model-local space.
                    // In rest pose, the bone's orientation relative to its parent points along restDir.
                    glm::mat4 parentLocalMat = (parentBoneIdx >= 0) ? impl_->bones[parentBoneIdx].world 
                                                                    : glm::mat4(1.f);
                    glm::vec3 localRestDir = getBoneRestDir(pb.boneIdx);
                    glm::vec3 restLocalPos = parentLocalPos + glm::vec3(parentLocalMat * glm::vec4(localRestDir * pb.boneLength, 0.f));

                    // Initialize physics state on first frame or reset
                    if (glm::length(pb.currPosition) < 0.0001f) {
                        pb.currPosition = restLocalPos;
                        pb.prevPosition = restLocalPos;
                    }

                    // Verlet Integration (model-local space)
                    glm::vec3 velocity = (pb.currPosition - pb.prevPosition) * (1.f - params.drag);
                    pb.prevPosition = pb.currPosition;

                    // Forces in model-local space: Gravity + Wind + Spring restoring force (scaled by dt*dt)
                    glm::vec3 gravityForce(0.f, -params.gravity * 9.81f * sub_dt * sub_dt, 0.f);
                    glm::vec3 windVec(windForce * sub_dt * sub_dt, 0.f, 0.f);
                    
                    // Spring restoring force pulls the bone back to its rest/neutral orientation (scaled by dt*dt)
                    glm::vec3 springForce = (restLocalPos - pb.currPosition) * (params.stiffness * 50.f) * sub_dt * sub_dt;

                    // Integrate
                    pb.currPosition += velocity + gravityForce + windVec + springForce;

                    // Collision system in model-local space: colliders repel the bone to prevent it from going inside the head or shoulders
                    for (const auto& col : impl_->internalColliders) {
                        if (col.boneIdx >= 0) {
                            glm::mat4 colBoneWorld = impl_->bones[col.boneIdx].world; // model-local
                            // Transform the local offset to model-local position
                            glm::vec3 colLocalPos = glm::vec3(colBoneWorld * glm::vec4(col.offset, 1.0f));

                            // Check collision
                            glm::vec3 toBone = pb.currPosition - colLocalPos;
                            float dist = glm::length(toBone);
                            if (dist < col.radius) {
                                // Collision detected! Push the bone outside the sphere
                                glm::vec3 pushDir = (dist > 0.001f) ? glm::normalize(toBone) : glm::vec3(0.f, 1.f, 0.f);
                                glm::vec3 oldPos = pb.currPosition;
                                pb.currPosition = colLocalPos + pushDir * col.radius;
                                // Offset prevPosition by the push amount to keep velocity clean and jitter-free
                                pb.prevPosition += (pb.currPosition - oldPos);
                            }

                            // Torso/Body approximation: If we are checking the Spine/Chest bone, treat it as a vertical capsule!
                            if (col.isTorso) {
                                // Generate additional capsule verification offset points up/down
                                for (float offsetStep : { -0.12f, -0.06f, 0.06f, 0.12f }) {
                                    glm::vec3 extraLocalPos = glm::vec3(colBoneWorld * glm::vec4(col.offset + glm::vec3(0.f, offsetStep, 0.f), 1.0f));
                                    glm::vec3 toBoneExtra = pb.currPosition - extraLocalPos;
                                    float distExtra = glm::length(toBoneExtra);
                                    if (distExtra < col.radius) {
                                        glm::vec3 pushDir = (distExtra > 0.001f) ? glm::normalize(toBoneExtra) : glm::vec3(0.f, 1.f, 0.f);
                                        glm::vec3 oldPos = pb.currPosition;
                                        pb.currPosition = extraLocalPos + pushDir * col.radius;
                                        pb.prevPosition += (pb.currPosition - oldPos);
                                    }
                                }
                            }
                        }
                    }

                    // Dynamic bone-fitted capsule colliders check! (in model-local space)
                    {
                        float colDistSq = meshCollisionRadius_ * meshCollisionRadius_;
                        glm::vec3 closestVPos(0.f);
                        float minDistSq = colDistSq;
                        bool collided = false;

                        for (const auto& seg : bodySegments) {
                            if (seg.boneIdxA == parentBoneIdx || seg.boneIdxB == parentBoneIdx) continue;
                            if (seg.boneIdxA == boneIdx || seg.boneIdxB == boneIdx) continue;
                            int grandParentIdx = (parentBoneIdx >= 0) ? impl_->bones[parentBoneIdx].parentIdx : -1;
                            if (grandParentIdx >= 0 && (seg.boneIdxA == grandParentIdx || seg.boneIdxB == grandParentIdx)) continue;

                            glm::vec3 ab = seg.b - seg.a;
                            float t = glm::dot(pb.currPosition - seg.a, ab) / glm::dot(ab, ab);
                            t = glm::clamp(t, 0.0f, 1.0f);
                            glm::vec3 pOnSeg = seg.a + t * ab;
                            
                            glm::vec3 toBone = pb.currPosition - pOnSeg;
                            float distSq = glm::dot(toBone, toBone);
                            if (distSq < minDistSq) {
                                minDistSq = distSq;
                                closestVPos = pOnSeg;
                                collided = true;
                            }
                        }

                        if (collided) {
                            float dist = std::sqrt(minDistSq);
                            glm::vec3 toBone = pb.currPosition - closestVPos;
                            glm::vec3 pushDir = (dist > 0.001f) ? toBone / dist : glm::vec3(0.f, 1.f, 0.f);
                            glm::vec3 oldPos = pb.currPosition;
                            pb.currPosition = closestVPos + pushDir * meshCollisionRadius_;
                            // Offset prevPosition to smoothly conserve momentum along the collision surface
                            pb.prevPosition += (pb.currPosition - oldPos);
                        }
                    }

                    // Constraint: Keep bone distance fixed (rigid bone length in model-local space)
                    glm::vec3 dir = pb.currPosition - parentLocalPos;
                    if (glm::length(dir) > 0.0001f) {
                        dir = glm::normalize(dir);
                    } else {
                        dir = glm::normalize(localRestDir);
                    }
                    pb.currPosition = parentLocalPos + dir * pb.boneLength;

                    // 2. Convert the new model-local direction back to parent's local space & apply to local bone transform
                    glm::quat parentLocalRot = (parentBoneIdx >= 0) ? getParentWorldRot(pb.boneIdx) 
                                                                    : glm::quat(1.f, 0.f, 0.f, 0.f);
                    glm::vec3 localPhysicsDir = glm::inverse(parentLocalRot) * dir;

                    float dotVal = glm::dot(localRestDir, localPhysicsDir);
                    glm::quat localRot;
                    if (dotVal < -0.9999f) {
                        glm::vec3 perp = std::abs(localRestDir.x) < 0.9f ? glm::vec3(1,0,0) : glm::vec3(0,1,0);
                        perp = glm::normalize(glm::cross(localRestDir, perp));
                        localRot = glm::angleAxis(glm::pi<float>(), perp);
                    } else {
                        localRot = glm::rotation(localRestDir, localPhysicsDir);
                    }

                    impl_->bones[pb.boneIdx].local = applyRotationToLocal(impl_->bones[pb.boneIdx].restLocal, localRot);

                    // Update the current bone's world transform so descendants resolve correctly
                    impl_->bones[pb.boneIdx].world = parentLocalMat * impl_->bones[pb.boneIdx].local;
                }
            }
        }

        // Avoid accumulator explosion if thread blocks
        if (impl_->physicsAccumulator > 0.25f) {
            impl_->physicsAccumulator = 0.f;
        }
    }

    // Note: computeBoneMatrices() is called inside render(), no need here.
}

// ─── Render ─────────────────────────────────────────────────────────────────
void ModelRenderer::render(const glm::mat4& view, const glm::mat4& proj, const rhi::BufferHandle& lightingUB) {
    if (!modelLoaded_ || impl_->meshes.empty() || !impl_->pipeline) return;

    impl_->applyCPUMorphTargets(rhi_);
    computeBoneMatrices();

    // Model matrix
    impl_->modelMatrix = glm::scale(
        glm::translate(glm::mat4(1.f), modelTranslation_),
        glm::vec3(modelScale_));

    // Upload View/Proj/Model matrices
    ViewProjBlock vp{};
    vp.view = view;
    vp.proj = proj;
    vp.model = impl_->modelMatrix;
    rhi_.updateBuffer(impl_->viewProjUB, &vp, sizeof(ViewProjBlock));

    // Upload Bone matrices
    if (impl_->boneUBO && !impl_->boneMatrices.empty()) {
        size_t sz = std::min(impl_->boneMatrices.size(), (size_t)512) * sizeof(glm::mat4);
        rhi_.updateBuffer(impl_->boneUBO, impl_->boneMatrices.data(), sz);
    }

    // Render primitives
    for (const auto& mp : impl_->meshes) {
        // Descriptor set uniform and texture bindings
        std::pair<uint32_t, rhi::BufferHandle> ubBindings[] = {
            { 1, impl_->boneUBO },
            { 2, lightingUB },
            { 3, impl_->materialUBs[mp.materialIdx >= 0 ? mp.materialIdx : 0] }
        };

        rhi::TextureHandle activeTex = (mp.materialIdx >= 0 && impl_->textures[mp.materialIdx])
            ? impl_->textures[mp.materialIdx] : impl_->defaultTexture;

        std::pair<uint32_t, rhi::TextureHandle> texBindings[] = {
            { 0, activeTex }
        };

        auto ds = rhi_.createDescriptorSet(mp.pipeline, ubBindings, texBindings);

        rhi::DrawCall dc;
        dc.pipeline      = mp.pipeline;
        dc.vertexBuffer  = mp.vertexBuffer;
        dc.indexBuffer   = mp.indexBuffer;
        dc.uniformBuffer = impl_->viewProjUB; // binding 0
        dc.descriptorSet = ds;
        dc.indexCount    = mp.indexCount;
        dc.vertexCount   = mp.vertexCount;
        dc.indexType     = rhi::IndexType::Uint32;
        rhi_.submit(dc);
    }
}

// ─── Blend shape control ─────────────────────────────────────────────────────
void ModelRenderer::setBlendShapeWeight(const std::string& name, float weight) {
    impl_->blendShapeWeights[name] = glm::clamp(weight, 0.f, 1.f);
}

float ModelRenderer::getBlendShapeWeight(const std::string& name) const {
    auto it = impl_->blendShapeWeights.find(name);
    return (it != impl_->blendShapeWeights.end()) ? it->second : 0.f;
}

void ModelRenderer::resetBlendShapes() {
    impl_->blendShapeWeights.clear();
}

const std::unordered_map<std::string, float>& ModelRenderer::getAllBlendShapeWeights() const {
    return impl_->blendShapeWeights;
}

void ModelRenderer::setExpression(const std::string& expressionName, float weight) {
    // Try to set expression via VRM blendshape group first
    std::string key = expressionName;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    auto groupIt = impl_->vrmBlendShapes.find(key);
    if (groupIt != impl_->vrmBlendShapes.end()) {
        setBlendShapeWeight(expressionName, weight);
        return;
    }

    // Fallback to hardcoded list
    auto it = kVRMExpressions.find(expressionName);
    if (it != kVRMExpressions.end())
        for (const auto& bs : it->second)
            setBlendShapeWeight(bs, weight);
}

void ModelRenderer::Impl::applyCPUMorphTargets(rhi::IRHIContext& rhi) {
    if (meshes.empty()) return;

    // 1. Accumulate morph target weights for each mesh primitive
    std::unordered_map<int, std::unordered_map<int, float>> accumulatedWeights;

    for (const auto& [bsName, bsWeight] : blendShapeWeights) {
        if (bsWeight <= 0.001f) continue;
        
        std::string key = bsName;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        
        auto it = vrmBlendShapes.find(key);
        if (it == vrmBlendShapes.end()) {
            // Preset fallbacks for standard VRM models
            if (key == "eyeblinkleft") it = vrmBlendShapes.find("blink_left");
            else if (key == "eyeblinkright") it = vrmBlendShapes.find("blink_right");
            else if (key == "jawopen") it = vrmBlendShapes.find("a");
            else if (key == "mouthfunnel") it = vrmBlendShapes.find("u");
            else if (key == "mouthpucker") it = vrmBlendShapes.find("o");
            
            // If blink_left/right not found, try main blink
            if (it == vrmBlendShapes.end() && (key == "eyeblinkleft" || key == "eyeblinkright")) {
                it = vrmBlendShapes.find("blink");
            }
        }

        if (it != vrmBlendShapes.end()) {
            for (const auto& bind : it->second.binds) {
                accumulatedWeights[bind.meshIndex][bind.targetIndex] += bsWeight * bind.weightFactor;
            }
        }
    }

    // 2. Apply deltas on CPU and update vertex buffers
    constexpr int STRIDE = 16;
    for (size_t pi = 0; pi < meshes.size(); ++pi) {
        auto& mp = meshes[pi];
        if (mp.morphTargets.empty() || mp.baseVertexData.empty() || !mp.vertexBuffer) continue;

        auto meshIt = accumulatedWeights.find(mp.meshIdx);
        bool hasActiveTargets = (meshIt != accumulatedWeights.end());

        if (!hasActiveTargets) {
            if (hadActiveTargets.count(pi) > 0) {
                rhi.updateBuffer(mp.vertexBuffer, mp.baseVertexData.data(), mp.baseVertexData.size() * sizeof(float));
                hadActiveTargets.erase(pi);
                lastAppliedMeshWeights[pi].clear();
            }
            continue;
        }

        const auto& targetWeights = meshIt->second;

        // Skip computation and GPU upload if blendshape weights haven't changed
        if (lastAppliedMeshWeights[pi] == targetWeights) {
            continue;
        }

        lastAppliedMeshWeights[pi] = targetWeights;
        hadActiveTargets.insert(pi);

        // Re-use cache buffer to avoid heap allocation
        auto& morphedData = morphedDataBuffer;
        morphedData.assign(mp.baseVertexData.begin(), mp.baseVertexData.end());
        
        const auto& sparseTargets = sparseMorphTargets[pi];
        for (const auto& target : sparseTargets) {
            auto wIt = targetWeights.find(target.targetIndex);
            if (wIt == targetWeights.end()) continue;
            
            float weight = glm::clamp(wIt->second, 0.f, 1.f);
            if (weight <= 0.001f) continue;
            
            for (size_t i = 0; i < target.vertexIndices.size(); ++i) {
                uint32_t vi = target.vertexIndices[i];
                const auto& delta = target.positionDeltas[i];
                morphedData[vi * STRIDE + 0] += delta.x * weight;
                morphedData[vi * STRIDE + 1] += delta.y * weight;
                morphedData[vi * STRIDE + 2] += delta.z * weight;
            }
        }

        rhi.updateBuffer(mp.vertexBuffer, morphedData.data(), morphedData.size() * sizeof(float));
    }
}

} // namespace vts::renderer