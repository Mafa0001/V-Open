# V-Open

V-Open is a fully open-source, high-performance C++ and OpenGL-based desktop VTuber application featuring head/upper-body skeletal tracking, spring-bone physics simulation, and reactive lighting.

## Key Features
- **3D Model Formats**: Native support for loading standard VRM, GLB, and glTF models.
- **Stable Physics Simulation**: Verlet-integrated spring-bone physics running in model-local space for stable movement simulation (ideal for hair, ears, and clothing).
- **Fast CPU Morphing**: Sparse morph targets update mechanism on the CPU with zero runtime allocations, running at 60+ FPS.
- **Environment & Reactive Lighting**: Screen-reactive directional/ambient lighting, real-time desktop color sampling, and audio level-driven flash explosion triggers.
- **Click-Through Support**: Fully transparent background support with OS-level mouse click-through for streaming in OBS.
- **Virtual Viewport**: Option to render the avatar to a virtual viewport floating panel, allowing settings customization while outputting the clean avatar.
- **Framerate Control**: Configurable target FPS limits for low GPU overhead.

## Architecture & Integration
V-Open operates via a dual-process architecture:
1. **Frontend (C++)**: Renders the avatar and handles environment, physics, and UI using OpenGL 4.6, GLFW, Glad, and ImGui.
2. **Backend Tracker (Python)**: Uses OpenCV and Google MediaPipe to capture video from your webcam and perform real-time landmark tracking. 
3. **Communication**: Uses a secure local WebSocket connection running on localhost. The C++ application reads the dynamic port from `tracker_port.txt` to automatically establish the receiver interface.

## Prerequisites & Compilation
V-Open uses CMake and `vcpkg` for dependency management.

### Dependencies
- Glad / OpenGL 4.6
- GLFW 3
- GLM
- nlohmann_json
- spdlog
- ImGui (opengl3 + glfw backend)
- IXWebSocket

### Build Steps
1. Clone the repository:
   ```bash
   git clone https://github.com/Mafa0001/V-Open.git
   cd V-Open
   ```
2. Initialize dependencies with `vcpkg` and configure the project:
   ```bash
   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake
   ```
3. Build the executable:
   ```bash
   cmake --build build --config Release
   ```

## License
V-Open is fully open source. Feel free to use, modify, and distribute under the MIT License.
