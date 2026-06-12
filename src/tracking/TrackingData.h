#pragma once
// src/tracking/TrackingData.h
// ─────────────────────────────────────────────────────────────────────────────
//  v4.0: face + upper-body tracking.
//  New fields: headEuler, blendshapes.
//  Pose fields populated from upper-body landmarks (shoulders, elbows, wrists).
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <string>
#include <glm/glm.hpp>

namespace vts::tracking {

struct Landmark {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float visibility = 1.f;

    // Convert normalized image-space landmark to world space.
    // Only used as a fallback — prefer world landmarks whenever available.
    glm::vec3 toWorld(float scale = 2.0f) const {
        return glm::vec3(
            (x - 0.5f) *  scale,
            (0.5f - y) *  scale,
            -z         *  scale
        );
    }

    // Return as metric world-space coords.
    glm::vec3 asWorld() const {
        return glm::vec3(x, y, z);
    }

    bool isVisible(float threshold = 0.5f) const {
        return visibility >= threshold;
    }
};

// ── Pose landmark indices ──────────────────────────────────────────────────────
namespace PoseLM {
    constexpr int NOSE           = 0;
    constexpr int LEFT_SHOULDER  = 11;
    constexpr int RIGHT_SHOULDER = 12;
    constexpr int LEFT_ELBOW     = 13;
    constexpr int RIGHT_ELBOW    = 14;
    constexpr int LEFT_WRIST     = 15;
    constexpr int RIGHT_WRIST    = 16;
    constexpr int LEFT_HIP       = 23;
    constexpr int RIGHT_HIP      = 24;
    constexpr int LEFT_KNEE      = 25;
    constexpr int RIGHT_KNEE     = 26;
    constexpr int LEFT_ANKLE     = 27;
    constexpr int RIGHT_ANKLE    = 28;
}

namespace HandLM {
    constexpr int WRIST      = 0;
    constexpr int THUMB_TIP  = 4;
    constexpr int INDEX_TIP  = 8;
    constexpr int MIDDLE_TIP = 12;
    constexpr int RING_TIP   = 16;
    constexpr int PINKY_TIP  = 20;
    constexpr int NUM_LANDMARKS = 21;
}

// ── Face landmark indices used by the renderer ────────────────────────────────
namespace FaceLM {
    constexpr int NOSE_TIP   = 4;
    constexpr int RIGHT_EYE  = 133;
    constexpr int LEFT_EYE   = 362;
    constexpr int CHIN       = 152;
    constexpr int FOREHEAD   = 10;
}

// ── Head Euler angles (degrees, Y-up right-hand, VRM convention) ──────────────
struct HeadEuler {
    float yaw   = 0.f;   // left/right  (+right)
    float pitch = 0.f;   // up/down     (+up)
    float roll  = 0.f;   // tilt        (+right-tilt)
    bool  valid = false;

    glm::vec3 toVec3() const { return glm::vec3(yaw, pitch, roll); }
};

struct TrackingFrame {
    double timestamp  = 0.0;
    float  trackerFPS = 0.f;
    float  blinkScore = 0.f;

    // ── Head rotation ────────────────────────────────────────────────────────
    HeadEuler headEuler;

    // ── Face blendshapes (ARKit 52 + MediaPipe extras) ────────────────────────
    std::unordered_map<std::string, float> blendshapes;

    // ── Active in v4.0 (face + hand + upper-body mode) ────────────────────────
    std::vector<Landmark> faceWorld;  // metric, nose-anchored, Y-up world space
    std::vector<Landmark> leftHand;   // metric, wrist-centred, Y-up world space
    std::vector<Landmark> rightHand;  // metric, wrist-centred, Y-up world space

    // ── Upper-body pose (shoulders, elbows, wrists — visibility-gated) ────────
    std::vector<Landmark> pose;       // normalized image-space
    std::vector<Landmark> poseWorld;  // metric world-space, Y-up

    // ── Legacy / unused ──────────────────────────────────────────────────────
    std::vector<Landmark> face;       // image-space face mesh

    // ── Body proportions ──────────────────────────────────────────────────────
    float torsoLength = 0.f;
    float armSpan     = 0.f;
    float height      = 0.f;

    // ── Availability queries ───────────────────────────────────────────────────

    bool hasFaceWorld()  const { return faceWorld.size()  >= 468; }
    bool hasLeftHand()   const { return leftHand.size()   >= HandLM::NUM_LANDMARKS; }
    bool hasRightHand()  const { return rightHand.size()  >= HandLM::NUM_LANDMARKS; }
    bool hasPose()       const { return poseWorld.size()  >= 33 || pose.size() >= 33; }
    bool hasWorldPose()  const { return poseWorld.size()  >= 33; }
    bool hasHeadEuler()  const { return headEuler.valid; }
    bool hasBlendshapes()const { return !blendshapes.empty(); }

    float getBlendshape(const std::string& name, float defaultVal = 0.f) const {
        auto it = blendshapes.find(name);
        return (it != blendshapes.end()) ? it->second : defaultVal;
    }

    // ── Accessors ─────────────────────────────────────────────────────────────

    glm::vec3 getFaceWorldPoint(int idx) const {
        if (idx < 0 || idx >= (int)faceWorld.size()) return glm::vec3(0.f);
        return faceWorld[idx].asWorld();
    }

    glm::vec3 getFaceOrigin() const {
        return getFaceWorldPoint(FaceLM::NOSE_TIP);
    }

    glm::vec3 getLeftHandPoint(int idx) const {
        if (idx < 0 || idx >= (int)leftHand.size()) return glm::vec3(0.f);
        return leftHand[idx].asWorld();
    }

    glm::vec3 getRightHandPoint(int idx) const {
        if (idx < 0 || idx >= (int)rightHand.size()) return glm::vec3(0.f);
        return rightHand[idx].asWorld();
    }

    glm::vec3 getPosePoint(int idx, float fallbackScale = 2.0f) const {
        if (idx < 0) return glm::vec3(0.f);
        if (idx < (int)poseWorld.size()) return poseWorld[idx].asWorld();
        if (idx < (int)pose.size())      return pose[idx].toWorld(fallbackScale);
        return glm::vec3(0.f);
    }

    // Camera re-centre target.
    glm::vec3 getRootPosition() const {
        if (hasFaceWorld()) return getFaceOrigin();
        if (hasPose()) {
            return (getPosePoint(PoseLM::LEFT_SHOULDER) +
                    getPosePoint(PoseLM::RIGHT_SHOULDER)) * 0.5f;
        }
        return glm::vec3(0.f);
    }

    // Mid-shoulder for upper body anchor
    glm::vec3 getShoulderCenter() const {
        if (!hasWorldPose()) return glm::vec3(0.f);
        bool lv = poseWorld[PoseLM::LEFT_SHOULDER].isVisible(0.5f);
        bool rv = poseWorld[PoseLM::RIGHT_SHOULDER].isVisible(0.5f);
        if (lv && rv)
            return (getPosePoint(PoseLM::LEFT_SHOULDER) + getPosePoint(PoseLM::RIGHT_SHOULDER)) * 0.5f;
        if (lv) return getPosePoint(PoseLM::LEFT_SHOULDER);
        if (rv) return getPosePoint(PoseLM::RIGHT_SHOULDER);
        return glm::vec3(0.f);
    }

    void updateProportions() {
        if (!hasPose()) return;
        glm::vec3 ls  = getPosePoint(PoseLM::LEFT_SHOULDER);
        glm::vec3 rs  = getPosePoint(PoseLM::RIGHT_SHOULDER);
        glm::vec3 lh  = getPosePoint(PoseLM::LEFT_HIP);
        glm::vec3 rh  = getPosePoint(PoseLM::RIGHT_HIP);
        torsoLength   = glm::distance((ls + rs) * 0.5f, (lh + rh) * 0.5f);
        armSpan       = glm::distance(getPosePoint(PoseLM::LEFT_WRIST),
                                      getPosePoint(PoseLM::RIGHT_WRIST));
    }
};

class TrackingState {
public:
    void update(TrackingFrame frame) {
        frame.updateProportions();
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = std::move(frame);
        frameCount_++;
    }

    TrackingFrame get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_;
    }

    uint64_t frameCount() const { return frameCount_.load(); }

private:
    mutable std::mutex mutex_;
    TrackingFrame      latest_;
    std::atomic<uint64_t> frameCount_{0};
};

} // namespace vts::tracking