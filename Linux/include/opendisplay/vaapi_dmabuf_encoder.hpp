#pragma once

#include "opendisplay/ffmpeg_encoder.hpp"
#include "opendisplay/types.hpp"

#include <atomic>
#include <functional>

struct AVBSFContext;
struct AVBufferRef;
struct AVCodecContext;
struct AVFilterContext;
struct AVFilterGraph;
struct AVFrame;
struct AVPacket;

namespace od {

/// Zero-copy H.264 encoder for DMA-BUF capture. The frame is imported into
/// VA-API, scaled and converted to NV12 on the GPU, and encoded in-process, so
/// the CPU never reads the (write-combined, VRAM-resident) capture buffer.
class VaapiDmabufEncoder {
public:
    using FrameCallback = std::function<void(EncodedFrame)>;

    VaapiDmabufEncoder() = default;
    ~VaapiDmabufEncoder();
    VaapiDmabufEncoder(const VaapiDmabufEncoder&) = delete;
    VaapiDmabufEncoder& operator=(const VaapiDmabufEncoder&) = delete;

    void start(EncoderConfig config, FrameCallback callback);
    /// Encodes synchronously; the GPU has finished reading the buffer on return.
    /// Throws if the frame cannot be imported or encoded.
    void encode(const DmabufFrame& frame);
    void requestKeyframe() { keyframeRequested_.store(true); }
    void stop();

private:
    void configure(const DmabufFrame& frame);
    void releasePipeline();
    void drainPackets();
    void emitPacket(const AVPacket& packet);

    EncoderConfig config_;
    FrameCallback callback_;
    std::atomic_bool keyframeRequested_ = false;
    VideoFormat inputFormat_;
    AVBufferRef* drmDevice_ = nullptr;
    AVBufferRef* drmFrames_ = nullptr;
    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* source_ = nullptr;
    AVFilterContext* sink_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVBSFContext* bitstreamFilter_ = nullptr;
    AVFrame* scaled_ = nullptr;
    AVPacket* packet_ = nullptr;
    std::int64_t pts_ = 0;
};

}  // namespace od
