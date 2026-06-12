// src/renderer/StickmanRenderer.cpp
// ─────────────────────────────────────────────────────────────────────────────
//  v4.0: face mesh + hand rendering only.
//  Pose rendering removed from default path; kPoseBones kept commented for
//  easy restoration when full-body mode is re-added.
//
//  Hand world space: wrist-centred, metric, Y-up.  Each hand is rendered
//  wherever its wrist sits in world space — no pose anchor required.
//
//  Face world space: nose-anchored at origin, metric, Y-up.  The Python
//  tracker places the nose at (0,0,0) and builds the rest of the mesh around
//  it using a fixed anatomical scale.
// ─────────────────────────────────────────────────────────────────────────────

#include "renderer/StickmanRenderer.h"

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <filesystem>

using namespace vts::tracking;

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

// ─── Skeleton connectivity ────────────────────────────────────────────────────

static constexpr std::pair<int,int> kHandBones[] = {
    {0,1},{1,2},{2,3},{3,4},        // thumb
    {0,5},{5,6},{6,7},{7,8},        // index
    {0,9},{9,10},{10,11},{11,12},   // middle
    {0,13},{13,14},{14,15},{15,16}, // ring
    {0,17},{17,18},{18,19},{19,20}, // pinky
    {5,9},{9,13},{13,17},           // palm arch
};

// Face contour connectivity — a subset of the 478-point mesh that draws a
// clean outline + eyes + mouth without swamping the view.
static constexpr std::pair<int,int> kFaceContour[] = {
    // Outer silhouette
    {10,338},{338,297},{297,332},{332,284},{284,251},{251,389},{389,356},
    {356,454},{454,323},{323,361},{361,288},{288,397},{397,365},{365,379},
    {379,378},{378,400},{400,377},{377,152},{152,148},{148,176},{176,149},
    {149,150},{150,136},{136,172},{172,58},{58,132},{132,93},{93,234},
    {234,127},{127,162},{162,21},{21,54},{54,103},{103,67},{67,109},{109,10},
    // Right eye
    {33,7},{7,163},{163,144},{144,145},{145,153},{153,154},{154,155},{155,133},
    {33,246},{246,161},{161,160},{160,159},{159,158},{158,157},{157,173},{173,133},
    // Left eye
    {362,382},{382,381},{381,380},{380,374},{374,373},{373,390},{390,249},{249,263},
    {362,466},{466,388},{388,387},{387,386},{386,385},{385,384},{384,398},{398,263},
    // Lips (outer)
    {61,146},{146,91},{91,181},{181,84},{84,17},{17,314},{314,405},{405,321},{321,375},{375,291},
    {61,185},{185,40},{40,39},{39,37},{37,0},{0,267},{267,269},{269,270},{270,409},{409,291},
};

// ─── Shaders ──────────────────────────────────────────────────────────────────

static const char* kVertSrc = R"GLSL(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;

uniform mat4  uView;
uniform mat4  uProj;
uniform float uDepthNear;
uniform float uDepthFar;
uniform float uPointSize;

out vec3  vColor;
out float vDepth;

void main() {
    vec4 viewPos = uView * vec4(aPos, 1.0);
    gl_Position  = uProj * viewPos;
    gl_PointSize = uPointSize;

    float d     = -viewPos.z;
    float range = uDepthFar - uDepthNear;
    vDepth = (range > 1e-4)
           ? clamp((d - uDepthNear) / range, 0.0, 1.0)
           : 0.5;
    vColor = aColor;
}
)GLSL";

static const char* kFragSrc = R"GLSL(
#version 460 core
in  vec3  vColor;
in  float vDepth;
out vec4  fragColor;

uniform int uDepthViz;

void main() {
    if (uDepthViz != 0) {
        float d   = clamp(vDepth, 0.0, 1.0);
        vec3 near = vec3(1.0, 0.2, 0.2);
        vec3 mid  = vec3(0.2, 1.0, 0.2);
        vec3 far  = vec3(0.2, 0.2, 1.0);
        vec3 col  = (d < 0.5)
                  ? mix(near, mid, d * 2.0)
                  : mix(mid,  far, (d - 0.5) * 2.0);
        fragColor = vec4(col, 1.0);
    } else {
        fragColor = vec4(vColor, 1.0);
    }
}
)GLSL";

// ─── Construction & init ──────────────────────────────────────────────────────

StickmanRenderer::StickmanRenderer(rhi::IRHIContext& rhi)
    : rhi_(rhi) {}

void StickmanRenderer::init() {
    rhi::ShaderDesc vd; vd.stage = rhi::ShaderStage::Vertex;   vd.glslSource = loadShaderFile("StickmanRenderer.vert");
    rhi::ShaderDesc fd; fd.stage = rhi::ShaderStage::Fragment;  fd.glslSource = loadShaderFile("StickmanRenderer.frag");
    auto vs = rhi_.createShader(vd);
    auto fs = rhi_.createShader(fd);

    rhi::VertexAttribute posAttr;
    posAttr.location = 0;
    posAttr.format   = rhi::PixelFormat::RGB32F;
    posAttr.stride   = sizeof(Vertex);
    posAttr.offset   = offsetof(Vertex, pos);

    rhi::VertexAttribute colAttr;
    colAttr.location = 1;
    colAttr.format   = rhi::PixelFormat::RGB32F;
    colAttr.stride   = sizeof(Vertex);
    colAttr.offset   = offsetof(Vertex, color);

    rhi::PipelineDesc baseDesc;
    baseDesc.vertexShader     = vs;
    baseDesc.fragmentShader   = fs;
    baseDesc.vertexAttributes = {posAttr, colAttr};
    baseDesc.raster.cullMode  = rhi::CullMode::None;
    baseDesc.depth.depthTest  = true;
    baseDesc.depth.depthWrite = true;
    baseDesc.blend.enabled    = false;

    rhi::PipelineDesc pointDesc = baseDesc;
    pointDesc.topology          = rhi::PrimitiveTopology::PointList;
    pointDesc.debugName         = "stickman_points";
    pointPipeline_ = rhi_.createPipeline(pointDesc);

    rhi::PipelineDesc lineDesc  = baseDesc;
    lineDesc.topology           = rhi::PrimitiveTopology::LineList;
    lineDesc.raster.lineWidth   = lineWidth_;
    lineDesc.debugName          = "stickman_lines";
    linePipeline_ = rhi_.createPipeline(lineDesc);

    rhi::BufferDesc vbDesc;
    vbDesc.size   = kMaxVertices * sizeof(Vertex);
    vbDesc.usage  = rhi::BufferUsage::Vertex;
    vbDesc.memory = rhi::MemoryType::HostVisible;
    vertexBuffer_ = rhi_.createBuffer(vbDesc);

    rhi::BufferDesc ubDesc;
    ubDesc.size   = sizeof(UBOData);
    ubDesc.usage  = rhi::BufferUsage::Uniform;
    ubDesc.memory = rhi::MemoryType::HostVisible;
    uniformBuffer_ = rhi_.createBuffer(ubDesc);

    spdlog::info("[StickmanRenderer] Initialized — face+hand mode");
}

// ─── Geometry helpers ─────────────────────────────────────────────────────────

void StickmanRenderer::addVertex(const glm::vec3& pos, const glm::vec3& color) {
    vertices_.push_back({pos, color});
}

void StickmanRenderer::addLine(const glm::vec3& p1, const glm::vec3& p2,
                                const glm::vec3& color) {
    addVertex(p1, color);
    addVertex(p2, color);
}

// ─── Depth range ──────────────────────────────────────────────────────────────

std::pair<float,float> StickmanRenderer::depthRange(const std::vector<Vertex>& verts,
                                                      const glm::mat4& view) {
    float nearD =  1e9f;
    float farD  = -1e9f;
    for (const auto& v : verts) {
        float d = -(view * glm::vec4(v.pos, 1.f)).z;
        nearD = std::min(nearD, d);
        farD  = std::max(farD,  d);
    }
    constexpr float kMinSpan = 0.05f;
    if (farD - nearD < kMinSpan) {
        float c = (nearD + farD) * 0.5f;
        nearD = c - kMinSpan * 0.5f;
        farD  = c + kMinSpan * 0.5f;
    }
    float pad = (farD - nearD) * 0.05f;
    return { nearD - pad, farD + pad };
}

// ─── Submit ───────────────────────────────────────────────────────────────────

void StickmanRenderer::submitDraw(const glm::mat4& view, const glm::mat4& proj,
                                   rhi::PrimitiveTopology topo, uint32_t count) {
    if (count == 0 || vertices_.size() < count) return;

    rhi_.updateBuffer(vertexBuffer_, vertices_.data(), count * sizeof(Vertex));

    auto [nearD, farD] = depthRange(vertices_, view);

    UBOData ubo;
    ubo.view      = view;
    ubo.proj      = proj;
    ubo.depthNear = nearD;
    ubo.depthFar  = farD;
    ubo.pointSize = pointSize_;
    ubo.visThresh = visThreshold_;
    ubo.depthViz  = depthViz_ ? 1 : 0;
    rhi_.updateBuffer(uniformBuffer_, &ubo, sizeof(ubo));

    rhi::DrawCall dc;
    dc.pipeline      = (topo == rhi::PrimitiveTopology::PointList) ? pointPipeline_ : linePipeline_;
    dc.vertexBuffer  = vertexBuffer_;
    dc.uniformBuffer = uniformBuffer_;
    dc.vertexCount   = count;
    rhi_.submit(dc);
}

// ─── Main render ──────────────────────────────────────────────────────────────

void StickmanRenderer::render(const tracking::TrackingFrame& frame,
                               const glm::mat4& view,
                               const glm::mat4& proj) {
    // ── Freeze ────────────────────────────────────────────────────────────────
    if (frozen_) {
        if (!hasFrozenFrame_) {
            frozenFrame_    = frame;
            hasFrozenFrame_ = true;
            spdlog::info("[StickmanRenderer] Frame frozen");
        }
    } else {
        hasFrozenFrame_ = false;
    }
    const tracking::TrackingFrame& src = (frozen_ && hasFrozenFrame_) ? frozenFrame_ : frame;

    // ── Colour palette ────────────────────────────────────────────────────────
    const glm::vec3 facePointColor  (0.9f, 0.85f, 0.3f);   // warm yellow
    const glm::vec3 faceLineColor   (0.6f, 0.55f, 0.2f);   // darker yellow
    const glm::vec3 leftHandColor   (0.0f, 0.6f,  1.0f);   // blue
    const glm::vec3 rightHandColor  (1.0f, 0.4f,  0.0f);   // orange
    const glm::vec3 lHandBone       (0.0f, 0.45f, 0.8f);
    const glm::vec3 rHandBone       (0.8f, 0.3f,  0.0f);

    const bool hasFace = src.hasFaceWorld();
    const bool hasLH   = src.hasLeftHand();
    const bool hasRH   = src.hasRightHand();

    if (!hasFace && !hasLH && !hasRH) return;   // nothing to draw

    // ─────────────────────────────────────────────────────────────────────────
    //  PASS 1 — LINES
    // ─────────────────────────────────────────────────────────────────────────
    if (drawBones_ || drawFaceMesh_) {
        vertices_.clear();

        // Face contour lines
        if (drawFaceMesh_ && hasFace) {
            for (auto [a, b] : kFaceContour) {
                if (a >= (int)src.faceWorld.size() || b >= (int)src.faceWorld.size()) continue;
                addLine(src.getFaceWorldPoint(a), src.getFaceWorldPoint(b), faceLineColor);
            }
        }

        // Hand finger/palm lines
        if (drawBones_) {
            if (hasLH) {
                for (auto [a, b] : kHandBones) {
                    if (a >= (int)src.leftHand.size() || b >= (int)src.leftHand.size()) continue;
                    addLine(src.getLeftHandPoint(a), src.getLeftHandPoint(b), lHandBone);
                }
            }
            if (hasRH) {
                for (auto [a, b] : kHandBones) {
                    if (a >= (int)src.rightHand.size() || b >= (int)src.rightHand.size()) continue;
                    addLine(src.getRightHandPoint(a), src.getRightHandPoint(b), rHandBone);
                }
            }
        }

        if (!vertices_.empty())
            submitDraw(view, proj, rhi::PrimitiveTopology::LineList,
                       static_cast<uint32_t>(vertices_.size()));
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  PASS 2 — POINTS
    // ─────────────────────────────────────────────────────────────────────────
    if (drawPoints_) {
        vertices_.clear();

        if (hasFace) {
            for (int i = 0; i < (int)src.faceWorld.size(); ++i)
                addVertex(src.getFaceWorldPoint(i), facePointColor);
        }

        if (hasLH) {
            for (int i = 0; i < (int)src.leftHand.size(); ++i)
                addVertex(src.getLeftHandPoint(i), leftHandColor);
        }

        if (hasRH) {
            for (int i = 0; i < (int)src.rightHand.size(); ++i)
                addVertex(src.getRightHandPoint(i), rightHandColor);
        }

        if (!vertices_.empty())
            submitDraw(view, proj, rhi::PrimitiveTopology::PointList,
                       static_cast<uint32_t>(vertices_.size()));
    }
}

} // namespace vts::renderer