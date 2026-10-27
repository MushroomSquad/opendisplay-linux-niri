#pragma once

#include "opendisplay/desktop_backend.hpp"
#include "opendisplay/ffmpeg_encoder.hpp"
#include "opendisplay/pipewire_capture.hpp"
#include "opendisplay/socket.hpp"
#include "opendisplay/types.hpp"
#include "opendisplay/vaapi_dmabuf_encoder.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace od {

class Session {
public:
    Session(Options options, std::unique_ptr<DesktopBackend> desktop);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void start(const Endpoint& endpoint);
    /// Called by the Qt main loop. Returns false after disconnection.
    bool tick();
    void stop();

private:
    enum class EventKind { Hello, Touch, Scroll };
    struct Event {
        EventKind kind = EventKind::Touch;
        PhoneInfo phone;
        std::string phase;
        double x = 0;
        double y = 0;
    };

    void receiveLoop();
    void startPipeline(const PhoneInfo& phone);
    void stopPipeline();
    bool send(std::string_view payload);
    void queue(Event event);

    Options options_;
    Socket socket_;
    std::unique_ptr<DesktopBackend> desktop_;
    PipeWireCapture capture_;
    FfmpegEncoder encoder_;
    VaapiDmabufEncoder zeroCopyEncoder_;
    std::atomic_bool zeroCopyActive_ = false;
    std::thread receiver_;
    std::atomic_bool connected_ = false;
    std::mutex sendMutex_;
    std::mutex stateMutex_;
    std::condition_variable helloCondition_;
    std::optional<PhoneInfo> phone_;
    std::optional<PhoneInfo> activePhone_;
    std::deque<Event> events_;
    std::chrono::steady_clock::time_point lastPing_{};
    std::chrono::steady_clock::time_point reconfigureAfter_{};
    bool pipelineRunning_ = false;
    bool reconfigurePending_ = false;
};

}  // namespace od
