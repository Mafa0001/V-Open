#pragma once
// include/rhi/RHI.h
// ─────────────────────────────────────────────────────────────────────────────
//  Render Hardware Interface  — backend-agnostic GPU abstraction
//  Backends: Vulkan (primary), OpenGL 4.6 (fallback/debug)
//
//  Design goals
//  ─────────────
//  • Thin enough to map directly to both Vulkan and OpenGL without impedance
//  • Handles opaque; creation always goes through the RHIContext factory
//  • No virtual dispatch in hot paths — command buffers record plain structs,
//    dispatched by the backend in one flush call
//  • Future-ready for VRM / VSeeFace model rendering (skinning, blend shapes)
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace vts::rhi {

// ─── Forward declarations ──────────────────────────────────────────────────────
struct Buffer;
struct Texture {
    uint32_t id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t internalFmt = 0;
};
struct Shader;
struct Pipeline;
struct RenderPass;
struct Framebuffer;
struct DescriptorSet;
struct CommandBuffer;

using BufferHandle      = std::shared_ptr<Buffer>;
using TextureHandle     = std::shared_ptr<Texture>;
using ShaderHandle      = std::shared_ptr<Shader>;
using PipelineHandle    = std::shared_ptr<Pipeline>;
using RenderPassHandle  = std::shared_ptr<RenderPass>;
using FramebufferHandle = std::shared_ptr<Framebuffer>;
using DescriptorSetHandle = std::shared_ptr<DescriptorSet>;
using CommandBufferHandle = std::shared_ptr<CommandBuffer>;

// ─── Enumerations ──────────────────────────────────────────────────────────────

enum class BackendType { Vulkan, OpenGL };

enum class BufferUsage : uint32_t {
    Vertex        = 1 << 0,
    Index         = 1 << 1,
    Uniform       = 1 << 2,
    Storage       = 1 << 3,
    TransferSrc   = 1 << 4,
    TransferDst   = 1 << 5,
};
inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(uint32_t(a) | uint32_t(b));
}

enum class MemoryType { DeviceLocal, HostVisible, HostCoherent };

enum class PixelFormat {
    RGBA8_UNORM, BGRA8_UNORM, RGBA16F, RGBA32F, RGB32F, RG32F,
    Depth32F, Depth24_Stencil8
};

enum class ShaderStage { Vertex, Fragment, Geometry, Compute };

enum class PrimitiveTopology { TriangleList, TriangleStrip, LineList, LineStrip, PointList };

enum class CullMode  { None, Front, Back };
enum class FrontFace { CCW, CW };
enum class PolygonMode { Fill, Line, Point };

enum class BlendFactor {
    Zero, One, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha
};

enum class CompareOp { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };

enum class IndexType { Uint16, Uint32 };

// ─── Descriptor types (for future model/material binding) ─────────────────────
enum class DescriptorType { UniformBuffer, StorageBuffer, CombinedImageSampler, StorageImage };

// ─── Creation descriptors ──────────────────────────────────────────────────────

struct BufferDesc {
    uint64_t     size       = 0;
    BufferUsage  usage      = BufferUsage::Vertex;
    MemoryType   memory     = MemoryType::HostVisible;
    const void*  initialData = nullptr;
    std::string  debugName;
};

struct TextureDesc {
    uint32_t    width  = 1, height = 1, depth = 1, mipLevels = 1, arrayLayers = 1;
    PixelFormat format = PixelFormat::RGBA8_UNORM;
    bool        isRenderTarget = false;
    bool        isDepthStencil = false;
    const void* initialData = nullptr;
    std::string debugName;
};

struct ShaderDesc {
    ShaderStage       stage;
    std::string       glslSource;   // always provide GLSL; backend compiles as needed
    std::string       entryPoint = "main";
    std::string       debugName;
};

struct VertexAttribute {
    uint32_t    location;
    uint32_t    binding  = 0;
    uint32_t    offset   = 0;
    uint32_t    stride   = 0;       // 0 = tightly packed (computed by backend)
    PixelFormat format;             // repurposed: R32G32B32_FLOAT → glm::vec3 etc.
};

struct RasterState {
    CullMode      cullMode   = CullMode::Back;
    FrontFace     frontFace  = FrontFace::CCW;
    PolygonMode   polyMode   = PolygonMode::Fill;
    float         lineWidth  = 1.0f;
};

struct DepthState {
    bool      depthTest  = true;
    bool      depthWrite = true;
    CompareOp compareOp  = CompareOp::Less;
};

struct BlendState {
    bool        enabled    = false;
    BlendFactor srcFactor  = BlendFactor::SrcAlpha;
    BlendFactor dstFactor  = BlendFactor::OneMinusSrcAlpha;
};

struct PipelineDesc {
    ShaderHandle            vertexShader;
    ShaderHandle            fragmentShader;
    std::vector<VertexAttribute> vertexAttributes;
    PrimitiveTopology       topology   = PrimitiveTopology::TriangleList;
    RasterState             raster;
    DepthState              depth;
    BlendState              blend;
    RenderPassHandle        renderPass;   // null = default swapchain pass
    std::string             debugName;
};

struct RenderPassDesc {
    PixelFormat  colorFormat   = PixelFormat::BGRA8_UNORM;
    PixelFormat  depthFormat   = PixelFormat::Depth32F;
    bool         clearColor    = true;
    bool         clearDepth    = true;
    glm::vec4    clearColorVal = {0.08f, 0.08f, 0.12f, 1.0f};
};

// ─── Per-frame draw call ───────────────────────────────────────────────────────

struct DrawCall {
    PipelineHandle      pipeline;
    BufferHandle        vertexBuffer;
    BufferHandle        indexBuffer;        // null = non-indexed
    BufferHandle        uniformBuffer;      // null = no per-draw UBO
    DescriptorSetHandle descriptorSet;      // null = no descriptors
    IndexType           indexType   = IndexType::Uint16;
    uint32_t            vertexCount = 0;
    uint32_t            indexCount  = 0;
    uint32_t            firstVertex = 0;
    uint32_t            firstIndex  = 0;
    uint32_t            instanceCount = 1;
    // Push constants (small per-draw data, avoids UBO updates for transforms)
    glm::mat4           modelMatrix  = glm::mat4(1.0f);
};

// ─── Viewport / scissor ───────────────────────────────────────────────────────
struct Viewport {
    float x = 0, y = 0, width = 800, height = 600, minDepth = 0, maxDepth = 1;
};
struct Scissor {
    int32_t x = 0, y = 0;
    uint32_t width = 800, height = 600;
};

// ─── RHI Context — one per window/GPU ─────────────────────────────────────────

class IRHIContext {
public:
    virtual ~IRHIContext() = default;

    // Lifecycle
    virtual void init(void* nativeWindowHandle, uint32_t w, uint32_t h) = 0;
    virtual void shutdown() = 0;
    virtual void resize(uint32_t w, uint32_t h) = 0;

    // Resource creation
    virtual BufferHandle      createBuffer     (const BufferDesc&)      = 0;
    virtual TextureHandle     createTexture    (const TextureDesc&)     = 0;
    virtual ShaderHandle      createShader     (const ShaderDesc&)      = 0;
    virtual PipelineHandle    createPipeline   (const PipelineDesc&)    = 0;
    virtual RenderPassHandle  createRenderPass (const RenderPassDesc&)  = 0;
    virtual FramebufferHandle createFramebuffer(const TextureHandle& color,
                                                const TextureHandle& depth) = 0;
    virtual DescriptorSetHandle createDescriptorSet(
        const PipelineHandle&,
        std::span<std::pair<uint32_t, BufferHandle>> uniformBindings,
        std::span<std::pair<uint32_t, TextureHandle>> textureBindings) = 0;

    // Resource updates
    virtual void updateBuffer(const BufferHandle&, const void* data,
                               uint64_t size, uint64_t offset = 0) = 0;

    // Frame
    virtual bool    beginFrame()  = 0;   // returns false if swapchain needs recreate
    virtual void    beginRenderPass(const RenderPassHandle&,
                                    const FramebufferHandle& = nullptr) = 0;
    virtual void    setViewport(const Viewport&) = 0;
    virtual void    setScissor (const Scissor&)  = 0;
    virtual void    submit(const DrawCall&) = 0;
    virtual void    endRenderPass() = 0;
    virtual void    endFrame()  = 0;    // present

    // ImGui integration
    virtual void imguiNewFrame()  = 0;
    virtual void imguiRender()    = 0;

    // Info
    virtual BackendType backendType() const = 0;
    virtual std::string backendName() const = 0;
};

// ─── Factory ──────────────────────────────────────────────────────────────────

std::unique_ptr<IRHIContext> createRHIContext(BackendType type);

// Picks the best available backend based on compile flags + runtime detection
std::unique_ptr<IRHIContext> createDefaultRHIContext();

} // namespace vts::rhi