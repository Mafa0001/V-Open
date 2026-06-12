// src/rhi/RHIFactory.cpp

#include "rhi/RHI.h"
#include <stdexcept>

#ifdef VTUBER_RHI_OPENGL_ENABLED
#include "rhi/OpenGLContext.h"
#endif

#ifdef VTUBER_RHI_VULKAN_ENABLED
// #include "rhi/VulkanContext.h"   // TODO: Vulkan backend
#endif

namespace vts::rhi {

    std::unique_ptr<IRHIContext> createRHIContext(BackendType type) {
        switch (type) {
#ifdef VTUBER_RHI_OPENGL_ENABLED
            case BackendType::OpenGL:
                return std::make_unique<OpenGLContext>();
#endif
#ifdef VTUBER_RHI_VULKAN_ENABLED
            case BackendType::Vulkan:
                // return std::make_unique<VulkanContext>();
                throw std::runtime_error("Vulkan backend not yet implemented — use OpenGL");
#endif
            default:
                throw std::runtime_error("Requested RHI backend not compiled in");
        }
    }

    std::unique_ptr<IRHIContext> createDefaultRHIContext() {
#if defined(VTUBER_RHI_DEFAULT_VULKAN) && defined(VTUBER_RHI_VULKAN_ENABLED)
        return createRHIContext(BackendType::Vulkan);
#elif defined(VTUBER_RHI_OPENGL_ENABLED)
        return createRHIContext(BackendType::OpenGL);
#else
        throw std::runtime_error("No RHI backend compiled in");
#endif
    }

} // namespace vts::rhi