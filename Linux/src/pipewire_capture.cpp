#include "opendisplay/pipewire_capture.hpp"

#include "opendisplay/log.hpp"
#include "opendisplay/pipewire_format.hpp"

#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/utils/result.h>

#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace od {
namespace {

// DMA-BUF mappings of VRAM are write-combined: ordinary loads are uncached
// and take ~25 ns each. Non-temporal 16-byte loads fill WC line buffers
// instead, which is an order of magnitude faster for this read-once copy.
__attribute__((target("sse4.1")))
void copyFromWriteCombined(char* target, const char* source, std::size_t size) {
    std::size_t offset = 0;
    if ((reinterpret_cast<std::uintptr_t>(source) & 15U) == 0) {
        for (; offset + 64 <= size; offset += 64) {
            auto* from = reinterpret_cast<__m128i*>(const_cast<char*>(source + offset));
            const __m128i a = _mm_stream_load_si128(from);
            const __m128i b = _mm_stream_load_si128(from + 1);
            const __m128i c = _mm_stream_load_si128(from + 2);
            const __m128i d = _mm_stream_load_si128(from + 3);
            auto* to = reinterpret_cast<__m128i*>(target + offset);
            _mm_storeu_si128(to, a);
            _mm_storeu_si128(to + 1, b);
            _mm_storeu_si128(to + 2, c);
            _mm_storeu_si128(to + 3, d);
        }
    }
    std::memcpy(target + offset, source + offset, size - offset);
}

const pw_stream_events streamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = PipeWireCapture::stateChanged;
    events.param_changed = PipeWireCapture::parameterChanged;
    events.process = PipeWireCapture::process;
    return events;
}();

std::optional<VideoFormat::PixelFormat> pixelFormat(const spa_video_format format) {
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRA: return VideoFormat::PixelFormat::Bgra;
    case SPA_VIDEO_FORMAT_BGRx: return VideoFormat::PixelFormat::Bgrx;
    case SPA_VIDEO_FORMAT_RGBA: return VideoFormat::PixelFormat::Rgba;
    case SPA_VIDEO_FORMAT_RGBx: return VideoFormat::PixelFormat::Rgbx;
    default: return std::nullopt;
    }
}

const char* pixelFormatName(const spa_video_format format) {
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRA: return "BGRA";
    case SPA_VIDEO_FORMAT_BGRx: return "BGRx";
    case SPA_VIDEO_FORMAT_RGBA: return "RGBA";
    case SPA_VIDEO_FORMAT_RGBx: return "RGBx";
    default: return "unknown";
    }
}

}  // namespace

PipeWireCapture::~PipeWireCapture() { stop(); }

void PipeWireCapture::start(const int remoteFd, const std::uint32_t nodeId, const int width,
                            const int height, const int fps, FrameCallback callback,
                            DmabufCallback dmabufCallback) {
    stop();
    {
        std::lock_guard lock(stateMutex_);
        state_ = PW_STREAM_STATE_UNCONNECTED;
        error_.clear();
    }
    static std::once_flag initialized;
    std::call_once(initialized, [] { pw_init(nullptr, nullptr); });

    callback_ = std::move(callback);
    dmabufCallback_ = std::move(dmabufCallback);
    loop_ = pw_thread_loop_new("opendisplay-capture", nullptr);
    if (loop_ == nullptr) {
        ::close(remoteFd);
        throw std::runtime_error("cannot create PipeWire thread loop");
    }
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (context_ == nullptr) {
        ::close(remoteFd);
        stop();
        throw std::runtime_error("cannot create PipeWire context");
    }
    core_ = remoteFd < 0 ? pw_context_connect(context_, nullptr, 0)
                         : pw_context_connect_fd(context_, remoteFd, nullptr, 0);
    if (core_ == nullptr) {
        ::close(remoteFd);
        stop();
        throw std::runtime_error(remoteFd < 0 ? "cannot connect to the default PipeWire socket"
                                             : "cannot connect to the portal PipeWire remote");
    }

    auto* properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_APP_NAME, "OpenDisplay",
        nullptr);
    stream_ = pw_stream_new(core_, "OpenDisplay KDE capture", properties);
    if (stream_ == nullptr) {
        stop();
        throw std::runtime_error("cannot create PipeWire capture stream");
    }
    pw_stream_add_listener(stream_, &listener_, &streamEvents, this);

    std::uint8_t storage[2048]{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const spa_pod* parameters[2];
    parameters[0] = buildPipeWireFormatOffer(builder, width, height, fps);
    parameters[1] = buildPipeWireLinearDmabufOffer(builder, width, height, fps);

    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT
        | PW_STREAM_FLAG_MAP_BUFFERS);
    const int result = pw_stream_connect(stream_, PW_DIRECTION_INPUT, nodeId, flags,
                                         parameters, 2);
    if (result < 0) {
        stop();
        throw std::runtime_error(std::string("cannot connect PipeWire stream: ")
                                 + spa_strerror(result));
    }
    if (pw_thread_loop_start(loop_) < 0) {
        stop();
        throw std::runtime_error("cannot start PipeWire thread loop");
    }

    std::unique_lock lock(stateMutex_);
    const bool configured = stateCondition_.wait_for(lock, std::chrono::seconds(10), [this] {
        return state_ == PW_STREAM_STATE_PAUSED || state_ == PW_STREAM_STATE_STREAMING
            || state_ == PW_STREAM_STATE_ERROR || !error_.empty();
    });
    if (!configured || state_ == PW_STREAM_STATE_ERROR || !error_.empty()) {
        const std::string failure = !configured
            ? "timed out while negotiating a video format"
            : (error_.empty() ? "unknown stream error" : error_);
        lock.unlock();
        stop();
        throw std::runtime_error("PipeWire capture failed: " + failure);
    }
}

void PipeWireCapture::stop() {
    if (loop_ != nullptr) {
        pw_thread_loop_stop(loop_);
    }
    if (stream_ != nullptr) {
        spa_hook_remove(&listener_);
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }
    if (core_ != nullptr) {
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    if (context_ != nullptr) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    if (loop_ != nullptr) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
    callback_ = {};
    dmabufCallback_ = {};
    format_ = {};
}

std::optional<std::string> PipeWireCapture::error() const {
    std::lock_guard lock(stateMutex_);
    if (error_.empty()) {
        return std::nullopt;
    }
    return error_;
}

void PipeWireCapture::stateChanged(void* data, pw_stream_state, const pw_stream_state state,
                                   const char* error) {
    auto& self = *static_cast<PipeWireCapture*>(data);
    {
        std::lock_guard lock(self.stateMutex_);
        self.state_ = state;
        if (state == PW_STREAM_STATE_ERROR) {
            self.error_ = error != nullptr ? error : "unknown stream error";
        }
    }
    self.stateCondition_.notify_all();
    if (state == PW_STREAM_STATE_ERROR) {
        log(std::string("PipeWire stream error: ") + (error != nullptr ? error : "unknown"));
    } else if (state == PW_STREAM_STATE_STREAMING) {
        log("PipeWire capture is streaming");
    }
}

void PipeWireCapture::parameterChanged(void* data, const std::uint32_t id,
                                       const spa_pod* parameter) {
    auto& self = *static_cast<PipeWireCapture*>(data);
    if (parameter == nullptr || id != SPA_PARAM_Format) {
        return;
    }
    spa_video_info_raw negotiated{};
    if (spa_format_video_raw_parse(parameter, &negotiated) < 0
        || !pixelFormat(negotiated.format)) {
        constexpr auto message = "PipeWire returned an unsupported video format";
        log(message);
        self.format_ = {};
        {
            std::lock_guard lock(self.stateMutex_);
            self.error_ = message;
        }
        self.stateCondition_.notify_all();
        return;
    }
    self.format_ = negotiated;
    self.modifier_ = negotiated.modifier;

    const bool dmabuf = spa_pod_find_prop(parameter, nullptr, SPA_FORMAT_VIDEO_modifier)
        != nullptr;
    const int dataTypes = dmabuf ? (1 << SPA_DATA_DmaBuf)
                                 : ((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd));
    std::uint8_t storage[256]{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const spa_pod* buffers[1];
    buffers[0] = static_cast<spa_pod*>(spa_pod_builder_add_object(
        &builder,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(dataTypes)));
    pw_stream_update_params(self.stream_, buffers, 1);
    log("PipeWire format: " + std::to_string(self.format_.size.width) + "x"
        + std::to_string(self.format_.size.height) + " "
        + pixelFormatName(self.format_.format) + " @ "
        + std::to_string(self.format_.framerate.num) + "/"
        + std::to_string(self.format_.framerate.denom) + " fps"
        + (dmabuf ? " (LINEAR DMA-BUF)" : ""));
}

void PipeWireCapture::process(void* data) {
    static_cast<PipeWireCapture*>(data)->handleProcess();
}

void PipeWireCapture::handleProcess() {
    pw_buffer* pipewireBuffer = pw_stream_dequeue_buffer(stream_);
    if (pipewireBuffer == nullptr) {
        return;
    }
    auto* buffer = pipewireBuffer->buffer;
    if (buffer->n_datas == 0 || buffer->datas[0].chunk == nullptr || format_.size.width == 0) {
        pw_stream_queue_buffer(stream_, pipewireBuffer);
        return;
    }

    const auto copyStart = std::chrono::steady_clock::now();
    const auto& data = buffer->datas[0];
    const auto* chunk = data.chunk;
    if (dmabufCallback_ && data.type == SPA_DATA_DmaBuf && data.fd >= 0) {
        const auto width = static_cast<int>(format_.size.width);
        DmabufFrame frame{
            .format = VideoFormat{
                .width = width,
                .height = static_cast<int>(format_.size.height),
                .stride = chunk->stride > 0 ? chunk->stride : width * 4,
                .fps = format_.framerate.denom > 0
                    ? static_cast<int>(format_.framerate.num / format_.framerate.denom)
                    : 60,
                .pixelFormat = *pixelFormat(format_.format),
            },
            .fd = static_cast<int>(data.fd),
            .offset = chunk->offset,
            .size = data.maxsize > 0 ? data.maxsize
                : static_cast<std::uint64_t>(::lseek(static_cast<int>(data.fd), 0, SEEK_END)),
            .modifier = modifier_,
            .capturedAtMs = wallClockMs(),
        };
        if (dmabufCallback_(frame)) {
            pw_stream_queue_buffer(stream_, pipewireBuffer);
            return;
        }
    }
    // PipeWire does not map DMA-BUF planes itself; map LINEAR buffers here and
    // bracket the CPU read with DMA_BUF_IOCTL_SYNC so the GPU write is visible.
    const void* mapped = data.data;
    void* ownMapping = nullptr;
    std::size_t mappingSize = 0;
    std::uint64_t maxSize = data.maxsize;
    const bool dmabuf = data.type == SPA_DATA_DmaBuf && data.fd >= 0;
    if (mapped == nullptr && dmabuf) {
        const auto end = ::lseek(static_cast<int>(data.fd), 0, SEEK_END);
        mappingSize = end > 0 ? static_cast<std::size_t>(end)
                              : static_cast<std::size_t>(data.mapoffset + data.maxsize);
        ownMapping = ::mmap(nullptr, mappingSize, PROT_READ, MAP_SHARED,
                            static_cast<int>(data.fd), 0);
        if (ownMapping == MAP_FAILED) {
            ownMapping = nullptr;
        } else {
            mapped = ownMapping;
            maxSize = mappingSize;
            dma_buf_sync sync{.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
            ::ioctl(static_cast<int>(data.fd), DMA_BUF_IOCTL_SYNC, &sync);
        }
    }
    const auto release = [&] {
        if (ownMapping != nullptr) {
            dma_buf_sync sync{.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
            ::ioctl(static_cast<int>(data.fd), DMA_BUF_IOCTL_SYNC, &sync);
            ::munmap(ownMapping, mappingSize);
        }
        pw_stream_queue_buffer(stream_, pipewireBuffer);
    };
    if (mapped == nullptr) {
        release();
        return;
    }
    const auto width = static_cast<int>(format_.size.width);
    const auto height = static_cast<int>(format_.size.height);
    const int sourceStride = chunk->stride > 0 ? chunk->stride : width * 4;
    const int targetStride = width * 4;
    const auto required = static_cast<std::uint64_t>(chunk->offset)
        + static_cast<std::uint64_t>(sourceStride) * static_cast<std::uint64_t>(height - 1)
        + static_cast<std::uint64_t>(targetStride);
    if (sourceStride < targetStride || required > maxSize) {
        release();
        return;
    }
    const auto* source = static_cast<const char*>(mapped) + chunk->offset;

    CapturedFrame frame;
    frame.format = VideoFormat{
        .width = width,
        .height = height,
        .stride = targetStride,
        .fps = format_.framerate.denom > 0
            ? static_cast<int>(format_.framerate.num / format_.framerate.denom)
            : 60,
        .pixelFormat = *pixelFormat(format_.format),
    };
    frame.capturedAtMs = wallClockMs();
    frame.sequence = sequence_.fetch_add(1);
    frame.bytes.resize(static_cast<std::size_t>(targetStride * height));
    for (int row = 0; row < height; ++row) {
        auto* to = frame.bytes.data() + static_cast<std::size_t>(row * targetStride);
        const auto* from = source + static_cast<std::size_t>(row * sourceStride);
        if (dmabuf) {
            copyFromWriteCombined(to, from, static_cast<std::size_t>(targetStride));
        } else {
            std::memcpy(to, from, static_cast<std::size_t>(targetStride));
        }
    }
    release();
    if (verboseLogging) {
        static int frames = 0;
        static double copyMs = 0;
        static auto windowStart = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        copyMs += std::chrono::duration<double, std::milli>(now - copyStart).count();
        ++frames;
        if (now - windowStart >= std::chrono::seconds(1)) {
            debug("capture: " + std::to_string(frames) + " fps, copy avg "
                  + std::to_string(copyMs / frames) + " ms");
            frames = 0;
            copyMs = 0;
            windowStart = now;
        }
    }
    if (callback_) {
        callback_(std::move(frame));
    }
}

}  // namespace od
