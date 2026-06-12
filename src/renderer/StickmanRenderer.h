#pragma once
// include/renderer/StickmanRenderer.h
// ─────────────────────────────────────────────────────────────────────────────
//  Default mode: renders face mesh + hands only.
//  Pose rendering is compiled out in v4.0; the kPoseBones table is kept as a
//  comment so re-adding full-body mode later requires minimal effort.
// ─────────────────────────────────────────────────────────────────────────────

#include <vector>
#include <glm/glm.hpp>
#include "rhi/RHI.h"
#include "tracking/TrackingData.h"

namespace vts::renderer {

    class StickmanRenderer {
    public:
        explicit StickmanRenderer(rhi::IRHIContext& rhi);
        ~StickmanRenderer() = default;

        void init();

        void render(const tracking::TrackingFrame& frame,
                    const glm::mat4& view,
                    const glm::mat4& proj);

        // ── Visualisation knobs (ImGui-friendly public fields) ────────────────
        bool  depthViz_      = false;
        bool  drawBones_     = true;    // skeleton / finger lines
        bool  drawPoints_    = true;    // landmark dots
        bool  drawFaceMesh_  = true;    // face contour lines
        float pointSize_     = 6.0f;
        float lineWidth_     = 2.0f;
        // Visibility threshold only applies when full-body pose is active;
        // face and hand landmarks are always fully visible.
        float visThreshold_  = 0.5f;

        // ── Freeze ────────────────────────────────────────────────────────────
        bool frozen_ = false;

    private:
        struct Vertex {
            glm::vec3 pos;
            glm::vec3 color;
        };

        struct UBOData {
            glm::mat4 view;
            glm::mat4 proj;
            float     depthNear  = 0.f;
            float     depthFar   = 10.f;
            float     pointSize  = 6.f;
            float     visThresh  = 0.5f;
            int       depthViz   = 0;
            float     _pad[3]    = {};
        };

        void buildGeometry(const tracking::TrackingFrame& frame);
        void addVertex(const glm::vec3& pos, const glm::vec3& color);
        void addLine  (const glm::vec3& p1,  const glm::vec3& p2,
                       const glm::vec3& color);

        void submitDraw(const glm::mat4& view, const glm::mat4& proj,
                        rhi::PrimitiveTopology topo, uint32_t count);

        static std::pair<float,float> depthRange(const std::vector<Vertex>& verts,
                                                  const glm::mat4& view);

        rhi::IRHIContext& rhi_;

        rhi::PipelineHandle pointPipeline_;
        rhi::PipelineHandle linePipeline_;

        rhi::BufferHandle   vertexBuffer_;
        rhi::BufferHandle   uniformBuffer_;

        std::vector<Vertex>   vertices_;

        tracking::TrackingFrame frozenFrame_;
        bool                    hasFrozenFrame_ = false;

        static constexpr size_t kMaxVertices = 20000;
    };

} // namespace vts::renderer