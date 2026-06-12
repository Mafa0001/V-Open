#pragma once
// include/rhi/OpenGLContext.h

#include "RHI.h"
#include <unordered_map>

struct GLFWwindow;

namespace vts::rhi {

class OpenGLContext final : public IRHIContext {
public:
    OpenGLContext() = default;
    ~OpenGLContext() override;

    void init(void* nativeWindowHandle, uint32_t w, uint32_t h) override;
    void shutdown() override;
    void resize(uint32_t w, uint32_t h) override;

    BufferHandle      createBuffer     (const BufferDesc&)      override;
    TextureHandle     createTexture    (const TextureDesc&)     override;
    ShaderHandle      createShader     (const ShaderDesc&)      override;
    PipelineHandle    createPipeline   (const PipelineDesc&)    override;
    RenderPassHandle  createRenderPass (const RenderPassDesc&)  override;
    FramebufferHandle createFramebuffer(const TextureHandle&,
                                        const TextureHandle&)   override;
    DescriptorSetHandle createDescriptorSet(
        const PipelineHandle&,
        std::span<std::pair<uint32_t, BufferHandle>>,
        std::span<std::pair<uint32_t, TextureHandle>>) override;

    void updateBuffer(const BufferHandle&, const void*, uint64_t, uint64_t) override;

    bool beginFrame()  override;
    void beginRenderPass(const RenderPassHandle&, const FramebufferHandle&) override;
    void setViewport(const Viewport&) override;
    void setScissor (const Scissor&)  override;
    void submit(const DrawCall&)      override;
    void endRenderPass()              override;
    void endFrame()    override;

    void imguiNewFrame() override;
    void imguiRender()   override;

    BackendType backendType() const override { return BackendType::OpenGL; }
    std::string backendName() const override { return "OpenGL 4.6"; }

private:
    GLFWwindow* window_  = nullptr;
    uint32_t    width_   = 0;
    uint32_t    height_  = 0;

    glm::vec4   clearColor_ = {0.08f, 0.08f, 0.12f, 1.0f};

    // Current render pass state
    bool inPass_ = false;
    struct ActivePass { bool clearColor = true; bool clearDepth = true; };
    ActivePass activePass_;
};

} // namespace vts::rhi