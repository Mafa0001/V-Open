// src/rhi/opengl/OpenGLContext.cpp
// ─────────────────────────────────────────────────────────────────────────────
//  OpenGL 4.6 RHI backend
//  Uses glad for function loading and GLFW for the window handle.
//  ImGui uses the imgui_impl_opengl3 + imgui_impl_glfw backends.
// ─────────────────────────────────────────────────────────────────────────────

#include "rhi/OpenGLContext.h"

#include <cassert>
#include <stdexcept>
#include <vector>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <spdlog/spdlog.h>
#include <glm/gtc/type_ptr.hpp>

namespace vts::rhi {

// ─── Internal GPU resource wrappers ───────────────────────────────────────────

struct Buffer {
    GLuint  id    = 0;
    GLenum  target = GL_ARRAY_BUFFER;
    uint64_t size  = 0;
};

struct Shader {
    GLuint program = 0;   // linked program for this stage pair
    ShaderStage stage;
    // For OpenGL we store the compiled shader object here; pipeline links them
    GLuint shader = 0;
};

struct Pipeline {
    GLuint      program = 0;
    RasterState raster;
    DepthState  depth;
    BlendState  blend;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    GLuint      vao = 0;
    std::vector<VertexAttribute> attribs;
};

struct RenderPass {
    RenderPassDesc desc;
};

struct Framebuffer {
    GLuint fbo = 0;   // 0 = default (swapchain)
};

struct DescriptorSet {
    std::vector<std::pair<uint32_t, BufferHandle>>  uniformBindings;
    std::vector<std::pair<uint32_t, TextureHandle>> textureBindings;
};

// ─── Helpers ──────────────────────────────────────────────────────────────────

static GLenum toGLTopology(PrimitiveTopology t) {
    switch (t) {
        case PrimitiveTopology::TriangleList:  return GL_TRIANGLES;
        case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
        case PrimitiveTopology::LineList:      return GL_LINES;
        case PrimitiveTopology::LineStrip:     return GL_LINE_STRIP;
        case PrimitiveTopology::PointList:     return GL_POINTS;
    }
    return GL_TRIANGLES;
}

static GLenum toGLCompare(CompareOp op) {
    switch (op) {
        case CompareOp::Less:         return GL_LESS;
        case CompareOp::LessEqual:    return GL_LEQUAL;
        case CompareOp::Greater:      return GL_GREATER;
        case CompareOp::GreaterEqual: return GL_GEQUAL;
        case CompareOp::Equal:        return GL_EQUAL;
        case CompareOp::NotEqual:     return GL_NOTEQUAL;
        case CompareOp::Always:       return GL_ALWAYS;
        case CompareOp::Never:        return GL_NEVER;
    }
    return GL_LESS;
}

static GLenum toGLBlend(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero:              return GL_ZERO;
        case BlendFactor::One:               return GL_ONE;
        case BlendFactor::SrcAlpha:          return GL_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha:  return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha:          return GL_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha:  return GL_ONE_MINUS_DST_ALPHA;
    }
    return GL_ONE;
}

// Compile a single GLSL shader
static GLuint compileShader(GLenum type, const std::string& src) {
    GLuint s = glCreateShader(type);
    const char* ptr = src.c_str();
    glShaderSource(s, 1, &ptr, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetShaderInfoLog(s, len, nullptr, log.data());
        glDeleteShader(s);
        throw std::runtime_error("Shader compile error:\n" + log);
    }
    return s;
}

// ─── Init / Shutdown ──────────────────────────────────────────────────────────

void OpenGLContext::init(void* nativeWindowHandle, uint32_t w, uint32_t h) {
    window_ = static_cast<GLFWwindow*>(nativeWindowHandle);
    width_  = w;
    height_ = h;

    glfwMakeContextCurrent(window_);
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
        throw std::runtime_error("glad failed to load OpenGL");

    glfwSwapInterval(1); // vsync

    spdlog::info("[GL] Vendor:   {}", reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
    spdlog::info("[GL] Renderer: {}", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    spdlog::info("[GL] Version:  {}", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_PROGRAM_POINT_SIZE); // allow gl_PointSize in vertex shader
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 460");
}

void OpenGLContext::shutdown() {
    static bool isShutdown = false;
    if (isShutdown) return;
    isShutdown = true;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void OpenGLContext::resize(uint32_t w, uint32_t h) {
    width_ = w; height_ = h;
    glViewport(0, 0, w, h);
}

// ─── Buffer ───────────────────────────────────────────────────────────────────

BufferHandle OpenGLContext::createBuffer(const BufferDesc& desc) {
    auto buf = std::make_shared<Buffer>();
    buf->size = desc.size;

    GLenum target = GL_ARRAY_BUFFER;
    if (uint32_t(desc.usage) & uint32_t(BufferUsage::Index))   target = GL_ELEMENT_ARRAY_BUFFER;
    if (uint32_t(desc.usage) & uint32_t(BufferUsage::Uniform)) target = GL_UNIFORM_BUFFER;
    buf->target = target;

    GLenum usage = (desc.memory == MemoryType::DeviceLocal) ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW;

    glGenBuffers(1, &buf->id);
    glBindBuffer(target, buf->id);
    glBufferData(target, desc.size, desc.initialData, usage);
    glBindBuffer(target, 0);
    return buf;
}

void OpenGLContext::updateBuffer(const BufferHandle& h, const void* data,
                                  uint64_t size, uint64_t offset) {
    auto& buf = *h;
    glBindBuffer(buf.target, buf.id);
    glBufferSubData(buf.target, static_cast<GLintptr>(offset),
                    static_cast<GLsizeiptr>(size), data);
    glBindBuffer(buf.target, 0);
}

// ─── Texture ──────────────────────────────────────────────────────────────────

TextureHandle OpenGLContext::createTexture(const TextureDesc& desc) {
    auto tex = std::make_shared<Texture>();
    tex->width  = desc.width;
    tex->height = desc.height;

    GLenum intFmt = GL_RGBA8, fmt = GL_RGBA, type = GL_UNSIGNED_BYTE;
    switch (desc.format) {
        case PixelFormat::RGBA16F:          intFmt = GL_RGBA16F;  fmt = GL_RGBA; type = GL_FLOAT; break;
        case PixelFormat::RG32F:            intFmt = GL_RG32F;    fmt = GL_RG;   type = GL_FLOAT; break;
        case PixelFormat::Depth32F:         intFmt = GL_DEPTH_COMPONENT32F; fmt = GL_DEPTH_COMPONENT; type = GL_FLOAT; break;
        case PixelFormat::Depth24_Stencil8: intFmt = GL_DEPTH24_STENCIL8; fmt = GL_DEPTH_STENCIL; type = GL_UNSIGNED_INT_24_8; break;
        default: break;
    }
    tex->internalFmt = intFmt;

    glGenTextures(1, &tex->id);
    glBindTexture(GL_TEXTURE_2D, tex->id);
    glTexImage2D(GL_TEXTURE_2D, 0, intFmt, desc.width, desc.height, 0, fmt, type, desc.initialData);
    if (desc.initialData && desc.mipLevels > 1) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// ─── Shader ───────────────────────────────────────────────────────────────────

ShaderHandle OpenGLContext::createShader(const ShaderDesc& desc) {
    auto sh = std::make_shared<Shader>();
    sh->stage = desc.stage;
    GLenum type = (desc.stage == ShaderStage::Vertex) ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
    sh->shader = compileShader(type, desc.glslSource);
    return sh;
}

// ─── Pipeline ─────────────────────────────────────────────────────────────────

PipelineHandle OpenGLContext::createPipeline(const PipelineDesc& desc) {
    auto pip = std::make_shared<Pipeline>();
    pip->raster   = desc.raster;
    pip->depth    = desc.depth;
    pip->blend    = desc.blend;
    pip->topology = desc.topology;
    pip->attribs  = desc.vertexAttributes;

    // Link program
    pip->program = glCreateProgram();
    glAttachShader(pip->program, desc.vertexShader->shader);
    glAttachShader(pip->program, desc.fragmentShader->shader);
    glLinkProgram(pip->program);

    GLint ok = 0;
    glGetProgramiv(pip->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(pip->program, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetProgramInfoLog(pip->program, len, nullptr, log.data());
        throw std::runtime_error("Program link error:\n" + log);
    }

    // VAO
    glGenVertexArrays(1, &pip->vao);
    return pip;
}

// ─── RenderPass / Framebuffer ─────────────────────────────────────────────────

RenderPassHandle OpenGLContext::createRenderPass(const RenderPassDesc& desc) {
    auto rp = std::make_shared<RenderPass>();
    rp->desc = desc;
    return rp;
}

FramebufferHandle OpenGLContext::createFramebuffer(const TextureHandle& color,
                                                    const TextureHandle& depth) {
    auto fb = std::make_shared<Framebuffer>();
    glGenFramebuffers(1, &fb->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    if (color) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color->id, 0);
    if (depth) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_TEXTURE_2D, depth->id, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return fb;
}

DescriptorSetHandle OpenGLContext::createDescriptorSet(
    const PipelineHandle&,
    std::span<std::pair<uint32_t, BufferHandle>>  ub,
    std::span<std::pair<uint32_t, TextureHandle>> tb)
{
    auto ds = std::make_shared<DescriptorSet>();
    ds->uniformBindings  = {ub.begin(),  ub.end()};
    ds->textureBindings  = {tb.begin(),  tb.end()};
    return ds;
}

// ─── Frame ────────────────────────────────────────────────────────────────────

bool OpenGLContext::beginFrame() { return true; }

void OpenGLContext::beginRenderPass(const RenderPassHandle& rp,
                                    const FramebufferHandle& fb) {
    inPass_ = true;
    GLuint fbo = (fb && fb->fbo) ? fb->fbo : 0;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width_, height_);

    const auto& desc = rp ? rp->desc : RenderPassDesc{};
    activePass_ = { desc.clearColor, desc.clearDepth };

    // Force write masks enabled for clear
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);

    GLbitfield mask = 0;
    if (desc.clearColor) {
        glClearColor(desc.clearColorVal.r, desc.clearColorVal.g,
                     desc.clearColorVal.b, desc.clearColorVal.a);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (desc.clearDepth) {
        glClearDepth(1.0);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (mask) glClear(mask);
}

void OpenGLContext::setViewport(const Viewport& v) {
    glViewport(static_cast<GLint>(v.x), static_cast<GLint>(v.y),
               static_cast<GLsizei>(v.width), static_cast<GLsizei>(v.height));
    glDepthRangef(v.minDepth, v.maxDepth);
}

void OpenGLContext::setScissor(const Scissor& s) {
    glEnable(GL_SCISSOR_TEST);
    // OpenGL scissor Y is bottom-left; flip from top-left
    glScissor(s.x, height_ - s.y - s.height, s.width, s.height);
}

void OpenGLContext::submit(const DrawCall& dc) {
    if (!dc.pipeline) return;
    auto& pip = *dc.pipeline;

    glUseProgram(pip.program);
    glBindVertexArray(pip.vao);

    // Raster state
    switch (pip.raster.cullMode) {
        case CullMode::None:  glDisable(GL_CULL_FACE); break;
        case CullMode::Front: glEnable(GL_CULL_FACE); glCullFace(GL_FRONT); break;
        case CullMode::Back:  glEnable(GL_CULL_FACE); glCullFace(GL_BACK);  break;
    }
    glFrontFace(pip.raster.frontFace == FrontFace::CCW ? GL_CCW : GL_CW);
    glLineWidth(pip.raster.lineWidth);
    glPolygonMode(GL_FRONT_AND_BACK,
                  pip.raster.polyMode == PolygonMode::Line ? GL_LINE : GL_FILL);

    // Depth state
    pip.depth.depthTest  ? glEnable(GL_DEPTH_TEST)  : glDisable(GL_DEPTH_TEST);
    pip.depth.depthWrite ? glDepthMask(GL_TRUE)      : glDepthMask(GL_FALSE);
    glDepthFunc(toGLCompare(pip.depth.compareOp));

    // Blend state
    if (pip.blend.enabled) {
        glEnable(GL_BLEND);
        glBlendFunc(toGLBlend(pip.blend.srcFactor), toGLBlend(pip.blend.dstFactor));
    } else {
        glDisable(GL_BLEND);
    }

    // Upload model matrix as plain uniform
    GLint loc = glGetUniformLocation(pip.program, "uModel");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(dc.modelMatrix));

    // Uniform buffer — try binding as a UBO block first; if the shader uses plain
    // uniforms (uView / uProj), read the data back and upload via glUniform*.
    if (dc.uniformBuffer) {
        // Always bind as UBO for shaders that use layout(std140) blocks
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, dc.uniformBuffer->id);

        // Also upload as plain mat4 uniforms for shaders that don't use UBO blocks
        // (e.g. StickmanRenderer's kVertSrc which uses "uniform mat4 uView/uProj")
        GLint viewLoc      = glGetUniformLocation(pip.program, "uView");
        GLint projLoc      = glGetUniformLocation(pip.program, "uProj");
        GLint depthVizLoc  = glGetUniformLocation(pip.program, "uDepthViz");
        GLint depthNearLoc = glGetUniformLocation(pip.program, "uDepthNear");
        GLint depthFarLoc  = glGetUniformLocation(pip.program, "uDepthFar");
        GLint pointSizeLoc = glGetUniformLocation(pip.program, "uPointSize");
        if (viewLoc >= 0 || projLoc >= 0 || depthVizLoc >= 0 || depthNearLoc >= 0 || depthFarLoc >= 0 || pointSizeLoc >= 0) {
            struct { glm::mat4 view; glm::mat4 proj; int depthViz; float depthNear; float depthFar; float pointSize; float visThresh; float _pad[3]; } uboData{};
            glGetBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(uboData), &uboData);
            if (viewLoc      >= 0) glUniformMatrix4fv(viewLoc,  1, GL_FALSE, glm::value_ptr(uboData.view));
            if (projLoc      >= 0) glUniformMatrix4fv(projLoc,  1, GL_FALSE, glm::value_ptr(uboData.proj));
            if (depthVizLoc  >= 0) glUniform1i(depthVizLoc,  uboData.depthViz);
            if (depthNearLoc >= 0) glUniform1f(depthNearLoc, uboData.depthNear);
            if (depthFarLoc  >= 0) glUniform1f(depthFarLoc,  uboData.depthFar);
            if (pointSizeLoc >= 0) glUniform1f(pointSizeLoc, uboData.pointSize);
        }
    }

    // Descriptor set
    if (dc.descriptorSet) {
        for (auto& [binding, buf] : dc.descriptorSet->uniformBindings)
            glBindBufferBase(GL_UNIFORM_BUFFER, binding, buf->id);
        for (auto& [unit, tex] : dc.descriptorSet->textureBindings) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, tex->id);
        }
    }

    // Vertex buffer + attribs
    if (dc.vertexBuffer) {
        glBindBuffer(GL_ARRAY_BUFFER, dc.vertexBuffer->id);
        for (const auto& a : pip.attribs) {
            glEnableVertexAttribArray(a.location);
            uint32_t compCount = 3; // default vec3
            switch (a.format) {
                case PixelFormat::RGBA32F:     compCount = 4; break;
                case PixelFormat::RGBA16F:     compCount = 4; break;
                case PixelFormat::RGBA8_UNORM: compCount = 4; break;
                case PixelFormat::RGB32F:      compCount = 3; break;
                case PixelFormat::RG32F:       compCount = 2; break;
                default: compCount = 3; break;
            }
            uint32_t stride = a.stride ? a.stride : compCount * sizeof(float);
            glVertexAttribPointer(a.location, compCount, GL_FLOAT, GL_FALSE,
                                  stride, reinterpret_cast<void*>(a.offset));
        }
    }

    GLenum topo = toGLTopology(pip.topology);

    if (dc.indexBuffer && dc.indexCount > 0) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dc.indexBuffer->id);
        GLenum idxType = (dc.indexType == IndexType::Uint32) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
        glDrawElements(topo, dc.indexCount, idxType,
                       reinterpret_cast<void*>(dc.firstIndex * (dc.indexType == IndexType::Uint32 ? 4 : 2)));
    } else {
        glDrawArrays(topo, dc.firstVertex, dc.vertexCount);
    }

    glBindVertexArray(0);
}

void OpenGLContext::endRenderPass() {
    inPass_ = false;
    glDisable(GL_SCISSOR_TEST);
}

void OpenGLContext::endFrame() {
    glfwSwapBuffers(window_);
}

// ─── ImGui ────────────────────────────────────────────────────────────────────

void OpenGLContext::imguiNewFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void OpenGLContext::imguiRender() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

OpenGLContext::~OpenGLContext() { shutdown(); }

} // namespace vts::rhi