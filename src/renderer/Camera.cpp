// src/renderer/Camera.cpp
// ─────────────────────────────────────────────────────────────────────────────
//  See Camera.h for design notes.
// ─────────────────────────────────────────────────────────────────────────────

#include "renderer/Camera.h"

#include <algorithm>
#include <cmath>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

namespace vts::renderer {

// ─── Construction ─────────────────────────────────────────────────────────────

Camera::Camera(GLFWwindow* window) : window_(window) {
    // Seed prevMouse so the first frame doesn't produce a huge delta
    glfwGetCursorPos(window_, &prevMouseX_, &prevMouseY_);
}

// ─── Mode switching ───────────────────────────────────────────────────────────

void Camera::setMode(CameraMode m) {
    if (m == mode_) return;
    mode_       = m;
    firstMouse_ = true;   // reset delta so no jump on mode switch

    if (m == CameraMode::Fly) {
        // Initialise fly position from current orbit camera position so the
        // transition is seamless.
        float yRad = glm::radians(orbitYaw_);
        float pRad = glm::radians(orbitPitch_);
        flyPos_ = orbitTarget_ + orbitRadius_ * glm::vec3(
             std::sin(yRad) * std::cos(pRad),
             std::sin(pRad),
            -std::cos(yRad) * std::cos(pRad));
        flyYaw_   = orbitYaw_ + 180.f;   // face the target
        flyPitch_ = -orbitPitch_;
    } else {
        // Transition back to orbit: keep the same focus, recalculate radius.
        orbitRadius_ = glm::length(flyPos_ - orbitTarget_);
        orbitRadius_ = glm::clamp(orbitRadius_, 0.2f, 40.f);
        // Derive yaw/pitch from fly direction so the view doesn't snap.
        orbitYaw_   =  flyYaw_ - 180.f;
        orbitPitch_ = -flyPitch_;
    }
}

// ─── Per-frame update ─────────────────────────────────────────────────────────

void Camera::update(float deltaTime) {
    if (inputEnabled) {
        if (mode_ == CameraMode::Orbit)
            updateOrbit(deltaTime);
        else
            updateFly(deltaTime);
    }

    // Sync prevMouse regardless of mode so switching modes never jumps.
    glfwGetCursorPos(window_, &prevMouseX_, &prevMouseY_);
}

// ── Orbit ─────────────────────────────────────────────────────────────────────

void Camera::updateOrbit(float /*dt*/) {
    double mx, my;
    glfwGetCursorPos(window_, &mx, &my);

    if (firstMouse_) {
        prevMouseX_ = mx;
        prevMouseY_ = my;
        firstMouse_ = false;
    }

    double dx = mx - prevMouseX_;
    double dy = my - prevMouseY_;

    int lmb = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT);
    int rmb = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT);

    if (lmb == GLFW_PRESS) {
        // Rotate
        orbitYaw_   += static_cast<float>(dx) * mouseSens_;
        orbitPitch_ -= static_cast<float>(dy) * mouseSens_;
        orbitPitch_  = glm::clamp(orbitPitch_, -89.f, 89.f);
    }

    if (rmb == GLFW_PRESS) {
        // Pan — move the focus point in the camera's local XY plane.
        // Scale pan speed with orbit radius so it feels consistent.
        float panScale = orbitRadius_ * 0.001f;
        glm::mat4 view = viewMatrix();
        glm::vec3 right = {view[0][0], view[1][0], view[2][0]};  // transpose row
        glm::vec3 up    = {view[0][1], view[1][1], view[2][1]};
        orbitTarget_ -= right * static_cast<float>(dx) * panScale;
        orbitTarget_ += up    * static_cast<float>(dy) * panScale;
    }
}

// ── Fly ───────────────────────────────────────────────────────────────────────

void Camera::updateFly(float dt) {
    double mx, my;
    glfwGetCursorPos(window_, &mx, &my);

    if (firstMouse_) {
        prevMouseX_ = mx;
        prevMouseY_ = my;
        firstMouse_ = false;
    }

    int rmb = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT);
    if (rmb == GLFW_PRESS) {
        double dx = mx - prevMouseX_;
        double dy = my - prevMouseY_;
        flyYaw_   += static_cast<float>(dx) * mouseSens_;
        flyPitch_ -= static_cast<float>(dy) * mouseSens_;
        flyPitch_  = glm::clamp(flyPitch_, -89.f, 89.f);
    }

    // Keyboard movement — only when RMB is held so camera doesn't steal keys
    // from ImGui widgets. Use shift for sprint.
    if (rmb == GLFW_PRESS) {
        float speed = flySpeed_;
        if (glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
            glfwGetKey(window_, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS)
            speed *= 3.f;

        glm::vec3 fwd   = flyForward();
        glm::vec3 right = flyRight();

        if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) flyPos_ += fwd   * speed * dt;
        if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) flyPos_ -= fwd   * speed * dt;
        if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) flyPos_ -= right * speed * dt;
        if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) flyPos_ += right * speed * dt;
        if (glfwGetKey(window_, GLFW_KEY_E) == GLFW_PRESS) flyPos_ += glm::vec3(0,1,0) * speed * dt;
        if (glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS) flyPos_ -= glm::vec3(0,1,0) * speed * dt;
    }
}

// ─── Scroll callback (zoom) ───────────────────────────────────────────────────
//
// This must be wired up by the application:
//   glfwSetScrollCallback(window, [](GLFWwindow* w, double, double yoff) {
//       auto* cam = static_cast<Camera*>(glfwGetWindowUserPointer(w));
//       if (cam) cam->onScroll(static_cast<float>(yoff));
//   });
//   glfwSetWindowUserPointer(window, &camera);

void Camera::onScroll(float yOffset) {
    if (mode_ == CameraMode::Orbit) {
        orbitRadius_ -= yOffset * orbitRadius_ * 0.1f;   // proportional zoom
        orbitRadius_  = glm::clamp(orbitRadius_, 0.2f, 40.f);
    } else {
        // In fly mode, scroll adjusts fly speed
        flySpeed_ = glm::clamp(flySpeed_ + yOffset * 0.2f, 0.1f, 20.f);
    }
}

// ─── Matrix output ────────────────────────────────────────────────────────────

glm::mat4 Camera::viewMatrix() const {
    if (mode_ == CameraMode::Orbit) {
        float yRad = glm::radians(orbitYaw_);
        float pRad = glm::radians(orbitPitch_);
        glm::vec3 offset = orbitRadius_ * glm::vec3(
             std::sin(yRad) * std::cos(pRad),
             std::sin(pRad),
            -std::cos(yRad) * std::cos(pRad));
        glm::vec3 eye = orbitTarget_ + offset;
        glm::vec3 up  = glm::vec3(0, 1, 0);
        // If looking straight down/up the up vector would be parallel to gaze;
        // tilt slightly to avoid degenerate lookAt.
        if (std::abs(orbitPitch_) > 88.9f)
            up = glm::vec3(std::cos(yRad), 0.f, std::sin(yRad));
        return glm::lookAt(eye, orbitTarget_, up);
    } else {
        // Fly: build a lookat from position + forward direction.
        return glm::lookAt(flyPos_, flyPos_ + flyForward(), glm::vec3(0,1,0));
    }
}

glm::mat4 Camera::projMatrix(float aspect) const {
    return glm::perspective(glm::radians(fovDeg), aspect, nearPlane, farPlane);
}

// ─── Private helpers ──────────────────────────────────────────────────────────

glm::vec3 Camera::flyForward() const {
    float yRad = glm::radians(flyYaw_);
    float pRad = glm::radians(flyPitch_);
    return glm::normalize(glm::vec3(
         std::sin(yRad) * std::cos(pRad),
         std::sin(pRad),
        -std::cos(yRad) * std::cos(pRad)));
}

glm::vec3 Camera::flyRight() const {
    return glm::normalize(glm::cross(flyForward(), glm::vec3(0,1,0)));
}

} // namespace vts::renderer