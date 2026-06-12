#pragma once
// include/tracking/TrackingReceiver.h

#include <atomic>
#include <functional>
#include <string>

#include <memory>
#include "TrackingData.h"

namespace vts::tracking {

    struct ReceiverStats {
        std::atomic<float>    fps{0.f};
        std::atomic<uint64_t> framesReceived{0};
        std::atomic<uint64_t> parseErrors{0};
        std::atomic<bool>     connected{false};
    };

    class TrackingReceiver {
    public:
        explicit TrackingReceiver(TrackingState& state, std::string url = "ws://localhost:8765");
        ~TrackingReceiver();

        void start();
        void stop();

        bool        isConnected() const { return stats_.connected.load(); }
        const ReceiverStats& stats()  const { return stats_; }

    private:
        void runLoop();
        bool parseFrame(const std::string& json, TrackingFrame& out) const;

        TrackingState&      state_;
        std::string         url_;
        std::atomic<bool>   running_{false};
        ReceiverStats       stats_;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace vts::tracking