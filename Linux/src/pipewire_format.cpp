#include "opendisplay/pipewire_format.hpp"

#include <spa/param/format-utils.h>
#include <spa/param/video/raw.h>

#include <algorithm>
#include <cstdint>

namespace od {

const spa_pod* buildPipeWireFormatOffer(spa_pod_builder& builder, const int width,
                                        const int height, const int fps) {
    const auto requestedWidth = static_cast<std::uint32_t>(std::max(1, width));
    const auto requestedHeight = static_cast<std::uint32_t>(std::max(1, height));
    const auto requestedFps = static_cast<std::uint32_t>(std::max(1, fps));
    const spa_rectangle requestedSize = SPA_RECTANGLE(requestedWidth, requestedHeight);
    const spa_rectangle minimumSize = SPA_RECTANGLE(1, 1);
    const spa_rectangle maximumSize = SPA_RECTANGLE(16384, 16384);
    const spa_fraction requestedRate = SPA_FRACTION(requestedFps, 1);
    const spa_fraction minimumRate = SPA_FRACTION(0, 1);
    const spa_fraction maximumRate = SPA_FRACTION(std::max(requestedFps, 240U), 1);

    return static_cast<spa_pod*>(spa_pod_builder_add_object(
        &builder,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        // KWin commonly exposes BGRx, while other producers use BGRA/RGBx/RGBA.
        // Every advertised layout is four bytes per pixel and is supported by
        // the FFmpeg input mapping.
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(
            5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
        // The portal decides the actual virtual-output mode. The encoder scales
        // that mode to the receiver's requested output dimensions.
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&requestedSize, &minimumSize, &maximumSize),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&requestedRate, &minimumRate, &maximumRate)));
}

const spa_pod* buildPipeWireLinearDmabufOffer(spa_pod_builder& builder, const int width,
                                              const int height, const int fps) {
    const auto requestedWidth = static_cast<std::uint32_t>(std::max(1, width));
    const auto requestedHeight = static_cast<std::uint32_t>(std::max(1, height));
    const auto requestedFps = static_cast<std::uint32_t>(std::max(1, fps));
    const spa_rectangle requestedSize = SPA_RECTANGLE(requestedWidth, requestedHeight);
    const spa_rectangle minimumSize = SPA_RECTANGLE(1, 1);
    const spa_rectangle maximumSize = SPA_RECTANGLE(16384, 16384);
    const spa_fraction requestedRate = SPA_FRACTION(requestedFps, 1);
    const spa_fraction minimumRate = SPA_FRACTION(0, 1);
    const spa_fraction maximumRate = SPA_FRACTION(std::max(requestedFps, 240U), 1);

    // niri (and Mutter) only advertise DMA-BUF formats with a mandatory
    // modifier. A LINEAR buffer can be mmapped and read like shared memory.
    spa_pod_frame frame{};
    spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format,
                                SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&builder,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(
            5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&requestedSize, &minimumSize, &maximumSize),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&requestedRate, &minimumRate, &maximumRate),
        0);
    spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
    spa_pod_builder_long(&builder, 0);  // DRM_FORMAT_MOD_LINEAR
    return static_cast<spa_pod*>(spa_pod_builder_pop(&builder, &frame));
}

}  // namespace od
