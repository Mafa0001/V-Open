#pragma once
// include/renderer/Camera.h
// ─────────────────────────────────────────────────────────────────────────────
//  Two-mode camera for the tracking visualiser:
//
//  ORBIT  — arcball around a focus point. Left-drag rotates, right-drag pans,
//           scroll zooms. Good for inspecting a pose from any angle.
//
//  FLY    — WASD/QE + mouse-look (hold RMB). Good for flying around a scene.
//
//  Both modes share view/proj matrices and expose the same interface so the
//  renderer does not need to know which mode is active.
//
//  FREEZE — when frozen_, incoming TrackingFrame data is ignored by the
//           renderer; the camera still moves so you can orbit/inspect freely.
// ─────────────────────────────────────────────────────────────────────────────

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct GLFWwindow;

namespace vts::renderer {

enum class CameraMode { Orbit, Fly };

class Camera {
public:
    // ── Construction ──────────────────────────────────────────────────────────
    explicit Camera(GLFWwindow* window);

    // Call once per frame before rendering.
    // deltaTime: seconds since last frame.
    void update(float deltaTime);

    // ── Output ────────────────────────────────────────────────────────────────
    glm::mat4 viewMatrix()  const;
    glm::mat4 projMatrix(float aspect) const;

    // ── Mode & freeze ─────────────────────────────────────────────────────────
    CameraMode mode()    const { return mode_; }
    bool       frozen()  const { return frozen_; }

    void setMode(CameraMode m);
    void toggleMode()   { setMode(mode_ == CameraMode::Orbit ? CameraMode::Fly : CameraMode::Orbit); }
    void toggleFreeze() { frozen_ = !frozen_; }
    void setFrozen(bool f) { frozen_ = f; }

    // Re-centre orbit focus on a world-space point (e.g. skeleton mid-hip).
    void setOrbitTarget(const glm::vec3& t) { orbitTarget_ = t; }

    // Called from the GLFW scroll callback.
    void onScroll(float yOffset);

    // ── Tweakables (ImGui-friendly public fields) ─────────────────────────────
    float fovDeg      = 45.f;
    float nearPlane   = 0.01f;
    float farPlane    = 50.f;
    float flySpeed_   = 2.0f;   // m/s (base; shift multiplies by 3)
    float mouseSens_  = 0.15f;  // degrees per pixel
    bool  inputEnabled = true;

private:
    // ── Orbit state ───────────────────────────────────────────────────────────
    glm::vec3 orbitTarget_  = {0.f, 0.f, 0.f};
    float     orbitRadius_  = 3.5f;
    float     orbitYaw_     = 0.f;    // degrees, around Y
    float     orbitPitch_   = 15.f;   // degrees, above horizon

    // ── Fly state ─────────────────────────────────────────────────────────────
    glm::vec3 flyPos_   = {0.f, 0.f, 4.f};
    float     flyYaw_   = 180.f;   // looking toward -Z initially
    float     flyPitch_ = 0.f;

    // ── Shared ────────────────────────────────────────────────────────────────
    CameraMode mode_   = CameraMode::Orbit;
    bool       frozen_ = false;

    GLFWwindow* window_ = nullptr;

    // Previous-frame mouse position (for delta computation)
    double prevMouseX_ = 0.0, prevMouseY_ = 0.0;
    bool   firstMouse_ = true;

    // GLFW callbacks store state here via glfwGetWindowUserPointer trick;
    // instead we poll every frame to stay simple.
    void updateOrbit(float dt);
    void updateFly(float dt);

    glm::vec3 flyForward() const;
    glm::vec3 flyRight()   const;
};

} // namespace vts::renderer