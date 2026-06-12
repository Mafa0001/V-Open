cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_TOOLCHAIN_FILE AND EXISTS "$ENV{USERPROFILE}/.vcpkg-clion/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{USERPROFILE}/.vcpkg-clion/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

project(V-Open VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Options ───────────────────────────────────────────────────────────────────
option(VTUBER_RHI_VULKAN  "Build Vulkan backend"  OFF)
option(VTUBER_RHI_OPENGL  "Build OpenGL backend"  ON)
option(VTUBER_RHI_DEFAULT "Default RHI (vulkan|opengl)" "opengl")

# ─── vcpkg / find_package bootstrap ───────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ─── Dependencies ──────────────────────────────────────────────────────────────
find_package(glfw3      CONFIG REQUIRED)
find_package(glm        CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog     CONFIG REQUIRED)
find_package(imgui      CONFIG REQUIRED)

if(VTUBER_RHI_VULKAN)
    find_package(Vulkan REQUIRED)
    find_package(unofficial-shaderc CONFIG REQUIRED)
endif()

if(VTUBER_RHI_OPENGL)
    find_package(glad CONFIG REQUIRED)
endif()

find_package(ixwebsocket CONFIG REQUIRED)

# ─── Compile definitions ───────────────────────────────────────────────────────
set(VTUBER_DEFINES "")
if(VTUBER_RHI_VULKAN)
    list(APPEND VTUBER_DEFINES VTUBER_RHI_VULKAN_ENABLED)
endif()
if(VTUBER_RHI_OPENGL)
    list(APPEND VTUBER_DEFINES VTUBER_RHI_OPENGL_ENABLED)
endif()
if("${VTUBER_RHI_DEFAULT}" STREQUAL "vulkan")
    list(APPEND VTUBER_DEFINES VTUBER_RHI_DEFAULT_VULKAN)
else()
    list(APPEND VTUBER_DEFINES VTUBER_RHI_DEFAULT_OPENGL)
endif()

# ─── Sources ───────────────────────────────────────────────────────────────────
file(GLOB_RECURSE VTUBER_SOURCES
        "src/*.cpp"
        "src/*.cc"
)

# IMPORTANT: Create the executable FIRST
add_executable(VStudio main.cpp ${VTUBER_SOURCES}
        src/rhi/RHI.h
        src/tracking/TrackingData.h
        src/tracking/TrackingReceiver.h
        src/rhi/OpenGLContext.h
        src/renderer/StickmanRenderer.h
        src/renderer/Camera.h
        src/renderer/ModelRenderer.h
        src/renderer/LightingSystem.h)

# ─── Python Tracker Integration (AFTER add_executable) ─────────────────────────
# Only copy the main.py file (not the entire directory)
set(TRACKER_SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/Tracker_VideoProcessing/main.py)
set(TRACKER_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/Tracker_VideoProcessing)
set(TRACKER_BUILD_FILE ${TRACKER_BUILD_DIR}/main.py)

# Create directory and copy only main.py
if(EXISTS ${TRACKER_SOURCE_FILE})
    add_custom_command(
            TARGET VStudio PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Copying Python tracker..."
            COMMAND ${CMAKE_COMMAND} -E make_directory ${TRACKER_BUILD_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${TRACKER_SOURCE_FILE}
            ${TRACKER_BUILD_FILE}
            COMMENT "Copying main.py to build directory"
    )
else()
    message(WARNING "Python tracker not found at: ${TRACKER_SOURCE_FILE}")
endif()

target_include_directories(VStudio PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/include/nlohmann"
        ${Stb_INCLUDE_DIR}
)

target_compile_definitions(VStudio PRIVATE ${VTUBER_DEFINES})

target_link_libraries(VStudio PRIVATE
        glfw
        glm::glm
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        imgui::imgui
        ixwebsocket::ixwebsocket
)

if(WIN32)
    target_link_libraries(VStudio PRIVATE bcrypt)
endif()

if(VTUBER_RHI_VULKAN)
    target_link_libraries(VStudio PRIVATE
            Vulkan::Vulkan
            unofficial::shaderc::shaderc
    )
endif()

if(VTUBER_RHI_OPENGL)
    target_link_libraries(VStudio PRIVATE glad::glad)
endif()

# ─── Shader compilation (GLSL → SPIR-V at build time) ─────────────────────────
if(VTUBER_RHI_VULKAN)
    find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin")
    if(GLSLC)
        file(GLOB SHADER_SOURCES "shaders/*.vert" "shaders/*.frag")
        foreach(SHADER ${SHADER_SOURCES})
            get_filename_component(SHADER_NAME ${SHADER} NAME)
            set(SPIRV_OUT "${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.spv")
            add_custom_command(
                    OUTPUT  ${SPIRV_OUT}
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders"
                    COMMAND ${GLSLC} ${SHADER} -o ${SPIRV_OUT}
                    DEPENDS ${SHADER}
                    COMMENT "Compiling shader ${SHADER_NAME}"
            )
            list(APPEND SPIRV_OUTPUTS ${SPIRV_OUT})
        endforeach()
        add_custom_target(Shaders ALL DEPENDS ${SPIRV_OUTPUTS})
        add_dependencies(VStudio Shaders)
    endif()
endif()

# Copy shaders next to the executable if the shaders directory exists
if(EXISTS "${CMAKE_SOURCE_DIR}/shaders")
    add_custom_command(TARGET VStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/shaders"
            "$<TARGET_FILE_DIR:VStudio>/shaders"
    )
endif()