// src/tracking/TrackingReceiver.cpp

#include "tracking/TrackingReceiver.h"

#include <chrono>
#include <thread>

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace vts::tracking {

struct TrackingReceiver::Impl {
    ix::WebSocket ws;
};

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::vector<Landmark> parseLandmarkArray(const json& arr) {
    std::vector<Landmark> out;
    if (!arr.is_array()) return out;
    out.reserve(arr.size());
    for (const auto& lm : arr) {
        Landmark l;
        l.x          = lm.value("x",          0.f);
        l.y          = lm.value("y",          0.f);
        l.z          = lm.value("z",          0.f);
        l.visibility = lm.value("visibility", 1.f);
        out.push_back(l);
    }
    return out;
}

static bool keyPresent(const json& j, const char* key) {
    return j.contains(key) && !j[key].is_null();
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────

TrackingReceiver::TrackingReceiver(TrackingState& state, std::string url)
    : state_(state), url_(std::move(url)), impl_(std::make_unique<Impl>()) {}

TrackingReceiver::~TrackingReceiver() { stop(); }

// ─── Start / Stop ─────────────────────────────────────────────────────────────

void TrackingReceiver::start() {
    if (running_.exchange(true)) return;

    impl_->ws.setUrl(url_);

    impl_->ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) {
            // ── Periodic raw-JSON sample (every 5 s) for diagnostics ──────────
            {
                static auto lastSample = std::chrono::steady_clock::now();
                auto now2 = std::chrono::steady_clock::now();
                if (std::chrono::duration<float>(now2 - lastSample).count() >= 5.f) {
                    lastSample = now2;
                    std::string preview = msg->str.substr(0, 300);
                    spdlog::info("[Tracker] RAW JSON sample: {}", preview);
                }
            }

            TrackingFrame frame;
            if (parseFrame(msg->str, frame)) {
                state_.update(std::move(frame));
                stats_.framesReceived++;

                static auto lastTime = std::chrono::steady_clock::now();
                static uint32_t count = 0;
                count++;
                auto now = std::chrono::steady_clock::now();
                float elapsed = std::chrono::duration<float>(now - lastTime).count();
                if (elapsed >= 1.0f) {
                    stats_.fps.store(count / elapsed);
                    count    = 0;
                    lastTime = now;
                }
            } else {
                stats_.parseErrors++;
            }
        } else if (msg->type == ix::WebSocketMessageType::Open) {
            stats_.connected = true;
            spdlog::info("[Tracker] Connected to {}", url_);
        } else if (msg->type == ix::WebSocketMessageType::Close) {
            stats_.connected = false;
            spdlog::warn("[Tracker] Disconnected — reconnecting…");
        } else if (msg->type == ix::WebSocketMessageType::Error) {
            stats_.connected = false;
            spdlog::error("[Tracker] WebSocket error: {}", msg->errorInfo.reason);
        }
    });

    impl_->ws.enableAutomaticReconnection();
    impl_->ws.setMinWaitBetweenReconnectionRetries(2000);
    impl_->ws.start();
    spdlog::info("[Tracker] Connecting to {}…", url_);
}

void TrackingReceiver::stop() {
    if (!running_.exchange(false)) return;
    impl_->ws.stop();
    spdlog::info("[Tracker] Stopped");
}

// ─── JSON → TrackingFrame ─────────────────────────────────────────────────────

bool TrackingReceiver::parseFrame(const std::string& raw, TrackingFrame& out) const {
    try {
        auto j = json::parse(raw);

        out.timestamp   = j.value("timestamp", 0.0);
        out.trackerFPS  = j.value("fps",        0.f);
        out.blinkScore  = j.value("blink",      0.f);

        // ── Face world landmarks ──────────────────────────────────────────────
        if (keyPresent(j, "face_world")) {
            out.faceWorld = parseLandmarkArray(j["face_world"]);
            for (auto& lm : out.faceWorld) {
                lm.x = -lm.x;
                lm.y = -lm.y;
                lm.z = -lm.z;
            }
        }

        // ── Hand landmarks ────────────────────────────────────────────────────
        if (keyPresent(j, "left_hand")) {
            out.rightHand = parseLandmarkArray(j["left_hand"]);
            for (auto& lm : out.rightHand) {
                lm.x = -lm.x;
                lm.y = -lm.y;
                lm.z = -lm.z;
            }
        }
        if (keyPresent(j, "right_hand")) {
            out.leftHand = parseLandmarkArray(j["right_hand"]);
            for (auto& lm : out.leftHand) {
                lm.x = -lm.x;
                lm.y = -lm.y;
                lm.z = -lm.z;
            }
        }

        // ── Pose landmarks (upper body, visibility-gated) ─────────────────────
        if (keyPresent(j, "pose")) {
            out.pose = parseLandmarkArray(j["pose"]);
            for (auto& lm : out.pose) lm.x = 1.0f - lm.x;
            if (out.pose.size() >= 33) {
                std::swap(out.pose[11], out.pose[12]);
                std::swap(out.pose[13], out.pose[14]);
                std::swap(out.pose[15], out.pose[16]);
                std::swap(out.pose[23], out.pose[24]);
            }
        }
        if (keyPresent(j, "pose_world")) {
            out.poseWorld = parseLandmarkArray(j["pose_world"]);
            for (auto& lm : out.poseWorld) {
                lm.x = -lm.x;
                lm.y = -lm.y;
                lm.z = -lm.z;
            }
            if (out.poseWorld.size() >= 33) {
                std::swap(out.poseWorld[11], out.poseWorld[12]);
                std::swap(out.poseWorld[13], out.poseWorld[14]);
                std::swap(out.poseWorld[15], out.poseWorld[16]);
                std::swap(out.poseWorld[23], out.poseWorld[24]);
            }
        }
        if (keyPresent(j, "face")) {
            out.face = parseLandmarkArray(j["face"]);
            for (auto& lm : out.face) lm.x = 1.0f - lm.x;
        }

        // ── Head Euler angles (degrees) ───────────────────────────────────────
        if (keyPresent(j, "head_euler")) {
            const auto& he = j["head_euler"];
            out.headEuler.yaw   = -he.value("yaw",   0.f);
            out.headEuler.pitch = he.value("pitch",  0.f);
            out.headEuler.roll  = -he.value("roll",   0.f);
            out.headEuler.valid = true;
        } else {
            out.headEuler.valid = false;
        }

        // ── Full blendshapes dict ─────────────────────────────────────────────
        if (keyPresent(j, "blendshapes") && j["blendshapes"].is_object()) {
            out.blendshapes.clear();
            for (auto& [key, val] : j["blendshapes"].items()) {
                if (val.is_number()) {
                    std::string mappedKey = key;
                    if (key == "eyeBlinkLeft") mappedKey = "eyeBlinkRight";
                    else if (key == "eyeBlinkRight") mappedKey = "eyeBlinkLeft";
                    else if (key == "eyeLookDownLeft") mappedKey = "eyeLookDownRight";
                    else if (key == "eyeLookDownRight") mappedKey = "eyeLookDownLeft";
                    else if (key == "eyeLookInLeft") mappedKey = "eyeLookInRight";
                    else if (key == "eyeLookInRight") mappedKey = "eyeLookInLeft";
                    else if (key == "eyeLookOutLeft") mappedKey = "eyeLookOutRight";
                    else if (key == "eyeLookOutRight") mappedKey = "eyeLookOutLeft";
                    else if (key == "eyeLookUpLeft") mappedKey = "eyeLookUpRight";
                    else if (key == "eyeLookUpRight") mappedKey = "eyeLookUpLeft";
                    else if (key == "eyeSquintLeft") mappedKey = "eyeSquintRight";
                    else if (key == "eyeSquintRight") mappedKey = "eyeSquintLeft";
                    else if (key == "eyeWideLeft") mappedKey = "eyeWideRight";
                    else if (key == "eyeWideRight") mappedKey = "eyeWideLeft";
                    else if (key == "mouthSmileLeft") mappedKey = "mouthSmileRight";
                    else if (key == "mouthSmileRight") mappedKey = "mouthSmileLeft";
                    else if (key == "mouthFrownLeft") mappedKey = "mouthFrownRight";
                    else if (key == "mouthFrownRight") mappedKey = "mouthFrownLeft";
                    else if (key == "mouthDimpleLeft") mappedKey = "mouthDimpleRight";
                    else if (key == "mouthDimpleRight") mappedKey = "mouthDimpleLeft";
                    else if (key == "browOuterUpLeft") mappedKey = "browOuterUpRight";
                    else if (key == "browOuterUpRight") mappedKey = "browOuterUpLeft";
                    
                    out.blendshapes[mappedKey] = val.get<float>();
                }
            }
        }

        return true;
    } catch (const std::exception& e) {
        spdlog::debug("[Tracker] Parse error: {}", e.what());
        return false;
    }
}

} // namespace vts::tracking