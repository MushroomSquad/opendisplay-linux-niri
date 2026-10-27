#include "opendisplay/vaapi_dmabuf_encoder.hpp"

#include "opendisplay/log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/opt.h>
}

#include <drm_fourcc.h>
#include <va/va.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace od {
namespace {

std::string averror(const int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void check(const int result, const std::string& what) {
    if (result < 0) {
        throw std::runtime_error(what + ": " + averror(result));
    }
}

// PipeWire names formats by byte order, DRM by little-endian word order.
std::uint32_t drmFourcc(const VideoFormat::PixelFormat format) {
    switch (format) {
    case VideoFormat::PixelFormat::Bgra: return DRM_FORMAT_ARGB8888;
    case VideoFormat::PixelFormat::Bgrx: return DRM_FORMAT_XRGB8888;
    case VideoFormat::PixelFormat::Rgba: return DRM_FORMAT_ABGR8888;
    case VideoFormat::PixelFormat::Rgbx: return DRM_FORMAT_XBGR8888;
    }
    return DRM_FORMAT_XRGB8888;
}

AVPixelFormat softwareFormat(const VideoFormat::PixelFormat format) {
    switch (format) {
    case VideoFormat::PixelFormat::Bgra: return AV_PIX_FMT_BGRA;
    case VideoFormat::PixelFormat::Bgrx: return AV_PIX_FMT_BGR0;
    case VideoFormat::PixelFormat::Rgba: return AV_PIX_FMT_RGBA;
    case VideoFormat::PixelFormat::Rgbx: return AV_PIX_FMT_RGB0;
    }
    return AV_PIX_FMT_BGR0;
}

void freeDescriptor(void*, std::uint8_t* data) {
    delete reinterpret_cast<AVDRMFrameDescriptor*>(data);
}

}  // namespace

VaapiDmabufEncoder::~VaapiDmabufEncoder() { stop(); }

void VaapiDmabufEncoder::start(EncoderConfig config, FrameCallback callback) {
    stop();
    config_ = std::move(config);
    callback_ = std::move(callback);
    check(av_hwdevice_ctx_create(&drmDevice_, AV_HWDEVICE_TYPE_DRM,
                                 config_.vaapiDevice.c_str(), nullptr, 0),
          "cannot open DRM device " + config_.vaapiDevice);
}

void VaapiDmabufEncoder::stop() {
    releasePipeline();
    av_buffer_unref(&drmDevice_);
    callback_ = {};
}

void VaapiDmabufEncoder::releasePipeline() {
    av_bsf_free(&bitstreamFilter_);
    avcodec_free_context(&codec_);
    avfilter_graph_free(&graph_);
    source_ = nullptr;
    sink_ = nullptr;
    av_buffer_unref(&drmFrames_);
    av_frame_free(&scaled_);
    av_packet_free(&packet_);
    inputFormat_ = {};
    pts_ = 0;
}

void VaapiDmabufEncoder::configure(const DmabufFrame& frame) {
    releasePipeline();
    const auto& format = frame.format;

    drmFrames_ = av_hwframe_ctx_alloc(drmDevice_);
    if (drmFrames_ == nullptr) throw std::runtime_error("cannot allocate DRM frames context");
    auto* frames = reinterpret_cast<AVHWFramesContext*>(drmFrames_->data);
    frames->format = AV_PIX_FMT_DRM_PRIME;
    frames->sw_format = softwareFormat(format.pixelFormat);
    frames->width = format.width;
    frames->height = format.height;
    check(av_hwframe_ctx_init(drmFrames_), "cannot initialize DRM frames context");

    graph_ = avfilter_graph_alloc();
    if (graph_ == nullptr) throw std::runtime_error("cannot allocate filter graph");
    // A hardware-format source must receive its frames context before init.
    source_ = avfilter_graph_alloc_filter(graph_, avfilter_get_by_name("buffer"), "in");
    if (source_ == nullptr) throw std::runtime_error("cannot create filter source");
    auto* parameters = av_buffersrc_parameters_alloc();
    parameters->format = AV_PIX_FMT_DRM_PRIME;
    parameters->width = format.width;
    parameters->height = format.height;
    parameters->time_base = AVRational{1, 1000};
    parameters->sample_aspect_ratio = AVRational{1, 1};
    parameters->hw_frames_ctx = drmFrames_;
    const int result = av_buffersrc_parameters_set(source_, parameters);
    av_free(parameters);
    check(result, "cannot attach DRM frames to filter source");
    check(avfilter_init_str(source_, nullptr), "cannot initialize filter source");
    check(avfilter_graph_create_filter(&sink_, avfilter_get_by_name("buffersink"), "out",
                                       nullptr, nullptr, graph_),
          "cannot create filter sink");

    const int outputWidth = config_.outputWidth > 0 ? config_.outputWidth : format.width;
    const int outputHeight = config_.outputHeight > 0 ? config_.outputHeight : format.height;
    const std::string description = "hwmap=derive_device=vaapi:mode=read,scale_vaapi=w="
        + std::to_string(outputWidth) + ":h=" + std::to_string(outputHeight) + ":format=nv12";
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = source_;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = sink_;
    const int parsed = avfilter_graph_parse_ptr(graph_, description.c_str(), &inputs, &outputs,
                                                nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    check(parsed, "cannot parse VA-API filter graph");
    for (unsigned index = 0; index < graph_->nb_filters; ++index) {
        graph_->filters[index]->hw_device_ctx = av_buffer_ref(drmDevice_);
    }
    check(avfilter_graph_config(graph_, nullptr), "cannot configure VA-API filter graph");

    const AVCodec* codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (codec == nullptr) throw std::runtime_error("FFmpeg does not provide h264_vaapi");
    codec_ = avcodec_alloc_context3(codec);
    codec_->width = outputWidth;
    codec_->height = outputHeight;
    codec_->time_base = AVRational{1, 1000};
    codec_->framerate = AVRational{std::max(1, config_.fps), 1};
    codec_->pix_fmt = AV_PIX_FMT_VAAPI;
    codec_->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(sink_));
    codec_->max_b_frames = 0;
    codec_->gop_size = std::max(config_.fps * 60, config_.fps);
    codec_->bit_rate = config_.bitrate;
    codec_->rc_max_rate = config_.bitrate;
    codec_->rc_buffer_size = std::max(config_.bitrate / 2, 1);
    // VBR: CBR pads static frames with filler NALs; async_depth 1: no queueing.
    av_opt_set(codec_->priv_data, "rc_mode", "VBR", 0);
    av_opt_set_int(codec_->priv_data, "async_depth", 1, 0);
    check(avcodec_open2(codec_, codec, nullptr), "cannot open h264_vaapi");

    check(av_bsf_list_parse_str("h264_metadata=aud=insert,dump_extra=freq=keyframe",
                                &bitstreamFilter_),
          "cannot create H.264 bitstream filters");
    check(avcodec_parameters_from_context(bitstreamFilter_->par_in, codec_),
          "cannot configure H.264 bitstream filters");
    bitstreamFilter_->time_base_in = codec_->time_base;
    check(av_bsf_init(bitstreamFilter_), "cannot initialize H.264 bitstream filters");

    scaled_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    inputFormat_ = format;
    log("Zero-copy VA-API encoder: " + std::to_string(format.width) + "x"
        + std::to_string(format.height) + " DMA-BUF -> " + std::to_string(outputWidth) + "x"
        + std::to_string(outputHeight) + " h264_vaapi");
}

void VaapiDmabufEncoder::encode(const DmabufFrame& frame) {
    if (drmDevice_ == nullptr) throw std::runtime_error("encoder is not started");
    if (codec_ == nullptr || frame.format.width != inputFormat_.width
        || frame.format.height != inputFormat_.height
        || frame.format.pixelFormat != inputFormat_.pixelFormat) {
        configure(frame);
    }

    auto* descriptor = new AVDRMFrameDescriptor{};
    descriptor->nb_objects = 1;
    descriptor->objects[0].fd = frame.fd;
    descriptor->objects[0].size = frame.size;
    descriptor->objects[0].format_modifier = frame.modifier;
    descriptor->nb_layers = 1;
    descriptor->layers[0].format = drmFourcc(frame.format.pixelFormat);
    descriptor->layers[0].nb_planes = 1;
    descriptor->layers[0].planes[0].object_index = 0;
    descriptor->layers[0].planes[0].offset = frame.offset;
    descriptor->layers[0].planes[0].pitch = frame.format.stride;

    AVFrame* input = av_frame_alloc();
    input->format = AV_PIX_FMT_DRM_PRIME;
    input->width = frame.format.width;
    input->height = frame.format.height;
    input->data[0] = reinterpret_cast<std::uint8_t*>(descriptor);
    input->buf[0] = av_buffer_create(reinterpret_cast<std::uint8_t*>(descriptor),
                                     sizeof(*descriptor), freeDescriptor, nullptr, 0);
    input->hw_frames_ctx = av_buffer_ref(drmFrames_);
    // Capture time doubles as the pts (time base 1/1000), kept strictly
    // increasing, so the packet carries it back out for the wire telemetry.
    pts_ = std::max(frame.capturedAtMs, pts_ + 1);
    input->pts = pts_;

    const int pushed = av_buffersrc_add_frame_flags(source_, input,
                                                    AV_BUFFERSRC_FLAG_KEEP_REF);
    av_frame_free(&input);
    check(pushed, "cannot import DMA-BUF into VA-API");

    for (;;) {
        const int pulled = av_buffersink_get_frame(sink_, scaled_);
        if (pulled == AVERROR(EAGAIN) || pulled == AVERROR_EOF) break;
        check(pulled, "VA-API scaling failed");
        // Make sure the GPU has finished reading the capture buffer before
        // PipeWire hands it back to the compositor.
        const auto* device = reinterpret_cast<AVHWDeviceContext*>(
            reinterpret_cast<AVHWFramesContext*>(scaled_->hw_frames_ctx->data)->device_ref->data);
        const auto* vaapi = static_cast<AVVAAPIDeviceContext*>(device->hwctx);
        vaSyncSurface(vaapi->display,
                      static_cast<VASurfaceID>(reinterpret_cast<std::uintptr_t>(scaled_->data[3])));
        if (keyframeRequested_.exchange(false)) {
            scaled_->pict_type = AV_PICTURE_TYPE_I;
            scaled_->flags |= AV_FRAME_FLAG_KEY;
        } else {
            scaled_->pict_type = AV_PICTURE_TYPE_NONE;
        }
        const int sent = avcodec_send_frame(codec_, scaled_);
        av_frame_unref(scaled_);
        check(sent, "h264_vaapi rejected a frame");
        drainPackets();
    }
}

void VaapiDmabufEncoder::drainPackets() {
    for (;;) {
        const int received = avcodec_receive_packet(codec_, packet_);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return;
        check(received, "h264_vaapi encode failed");
        check(av_bsf_send_packet(bitstreamFilter_, packet_), "H.264 bitstream filter failed");
        while (av_bsf_receive_packet(bitstreamFilter_, packet_) == 0) {
            emitPacket(*packet_);
            av_packet_unref(packet_);
        }
    }
}

void VaapiDmabufEncoder::emitPacket(const AVPacket& packet) {
    // The iOS receiver only recognizes four-byte start codes.
    std::string annexB;
    annexB.reserve(static_cast<std::size_t>(packet.size) + 64);
    const auto* data = reinterpret_cast<const char*>(packet.data);
    const auto size = static_cast<std::size_t>(packet.size);
    for (std::size_t index = 0; index < size;) {
        if (index + 3 <= size && data[index] == 0 && data[index + 1] == 0
            && data[index + 2] == 1) {
            annexB.append("\0\0\0\1", 4);
            index += 3;
        } else if (index + 4 <= size && data[index] == 0 && data[index + 1] == 0
                   && data[index + 2] == 0 && data[index + 3] == 1) {
            annexB.append("\0\0\0\1", 4);
            index += 4;
        } else {
            annexB.push_back(data[index]);
            ++index;
        }
    }
    if (callback_) {
        callback_(EncodedFrame{.capturedAtMs = packet.pts,
                               .keyframe = (packet.flags & AV_PKT_FLAG_KEY) != 0,
                               .annexB = std::move(annexB)});
    }
}

}  // namespace od
