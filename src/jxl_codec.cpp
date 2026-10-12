// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "jxl_codec.hpp"

#include <algorithm>
#include <mutex>
#include <set>
#include <tuple>
#include <stdexcept>
#include <utility>

#if defined(DNGCONV_HAVE_JXL)
#include <jxl/decode.h>
#include <jxl/encode.h>
#endif

namespace dngconv {

#if defined(DNGCONV_HAVE_JXL)

namespace {

[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string("JPEG XL: ") + what); }

// The encoder and decoder objects, released on every way out.
struct Encoder {
    JxlEncoder* p = JxlEncoderCreate(nullptr);
    ~Encoder() {
        if (p) JxlEncoderDestroy(p);
    }
};
struct Decoder {
    JxlDecoder* p = JxlDecoderCreate(nullptr);
    ~Decoder() {
        if (p) JxlDecoderDestroy(p);
    }
};

}  // namespace

bool jxlAvailable() { return true; }

std::string jxlVersion() {
    const uint32_t v = JxlEncoderVersion();  // major * 1000000 + minor * 1000 + patch
    return std::to_string(v / 1000000) + "." + std::to_string(v / 1000 % 1000) + "." + std::to_string(v % 1000);
}

namespace {

std::vector<uint8_t> encode(const uint16_t* samples, uint32_t width, uint32_t height, uint32_t channels,
                            int effort, float distance = 0.0f);
JxlPicture decode(const uint8_t* data, size_t size, size_t maxSamples);

// libjxl builds tables of its own the first time they are needed. Tiles are
// coded on several threads at once, each with its own encoder; so that no
// two of them meet in that first use, the first call for a kind of picture
// codes and decodes a small one alone, while the others wait.
void prepare(uint32_t channels, int effort, bool lossy = false) {
    static std::mutex mutex;
    static std::set<std::tuple<uint32_t, int, bool>> done;
    std::lock_guard<std::mutex> lock(mutex);
    if (!done.insert({channels, effort, lossy}).second) return;
    constexpr uint32_t kSide = 96;
    std::vector<uint16_t> pixels(static_cast<size_t>(kSide) * kSide * channels);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint16_t>((i * 2654435761u) >> 17);
    const std::vector<uint8_t> coded = encode(pixels.data(), kSide, kSide, channels, effort, lossy ? 1.0f : 0.0f);
    (void)decode(coded.data(), coded.size(), pixels.size());
}

}  // namespace

std::vector<uint8_t> encodeJxlLossless(const uint16_t* samples, uint32_t width, uint32_t height,
                                       uint32_t channels, int effort) {
    if (!samples || width == 0 || height == 0 || (channels != 1 && channels != 3))
        throw std::invalid_argument("JPEG XL: picture must have one or three channels and a size");
    effort = std::clamp(effort, 1, 9);
    prepare(channels, effort);
    return encode(samples, width, height, channels, effort);
}

std::vector<uint8_t> encodeJxlLossy(const uint16_t* samples, uint32_t width, uint32_t height, float distance,
                                    int effort) {
    if (!samples || width == 0 || height == 0)
        throw std::invalid_argument("JPEG XL: picture must have a size");
    if (!(distance > 0.0f) || distance > 25.0f) throw std::invalid_argument("JPEG XL: distance must be above 0, at most 25");
    effort = std::clamp(effort, 1, 9);
    prepare(1, effort, true);
    return encode(samples, width, height, 1, effort, distance);
}

namespace {

std::vector<uint8_t> encode(const uint16_t* samples, uint32_t width, uint32_t height, uint32_t channels,
                            int effort, float distance) {
    const bool lossless = distance == 0.0f;
    Encoder enc;
    if (!enc.p) fail("the encoder could not be created");

    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = width;
    info.ysize = height;
    info.bits_per_sample = 16;
    info.exponent_bits_per_sample = 0;
    info.num_color_channels = channels;
    info.num_extra_channels = 0;
    info.alpha_bits = 0;
    // Lossless: stored as given, not converted to the codec's own colour
    // space, the condition for getting the same numbers back. Lossy: in the
    // codec's own space (XYB), where its notion of visible error lives.
    info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
    if (JxlEncoderSetBasicInfo(enc.p, &info) != JXL_ENC_SUCCESS) fail("the encoder refused the picture's size");

    // Lossless: sensor values are linear, and the colour space is a label
    // only; nothing is converted on the way in or out. Lossy: the values have
    // been put through a curve that spaces the tones about evenly to the eye,
    // which the sRGB transfer curve stands for here (Adobe's SDK does the
    // same); the decoder hands the values back in that encoding.
    JxlColorEncoding colour;
    if (lossless)
        JxlColorEncodingSetToLinearSRGB(&colour, channels == 1 ? JXL_TRUE : JXL_FALSE);
    else
        JxlColorEncodingSetToSRGB(&colour, channels == 1 ? JXL_TRUE : JXL_FALSE);
    if (JxlEncoderSetColorEncoding(enc.p, &colour) != JXL_ENC_SUCCESS) fail("the encoder refused the colour encoding");

    JxlEncoderFrameSettings* frame = JxlEncoderFrameSettingsCreate(enc.p, nullptr);
    if (!frame) fail("the encoder gave no frame settings");
    if (lossless) {
        if (JxlEncoderSetFrameDistance(frame, 0.0f) != JXL_ENC_SUCCESS ||
            JxlEncoderSetFrameLossless(frame, JXL_TRUE) != JXL_ENC_SUCCESS)
            fail("lossless mode is not available");
    } else if (JxlEncoderSetFrameDistance(frame, distance) != JXL_ENC_SUCCESS) {
        fail("the encoder refused the distance");
    }
    if (JxlEncoderFrameSettingsSetOption(frame, JXL_ENC_FRAME_SETTING_EFFORT, std::clamp(effort, 1, 9)) !=
        JXL_ENC_SUCCESS)
        fail("the encoder refused the effort setting");

    const JxlPixelFormat format = {channels, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
    const size_t bytes = static_cast<size_t>(width) * height * channels * sizeof(uint16_t);
    if (JxlEncoderAddImageFrame(frame, &format, samples, bytes) != JXL_ENC_SUCCESS) fail("the encoder refused the pixels");
    JxlEncoderCloseInput(enc.p);

    std::vector<uint8_t> out(std::max<size_t>(bytes / 4, 4096));
    size_t used = 0;
    for (;;) {
        uint8_t* next = out.data() + used;
        size_t available = out.size() - used;
        const JxlEncoderStatus status = JxlEncoderProcessOutput(enc.p, &next, &available);
        used = static_cast<size_t>(next - out.data());
        if (status == JXL_ENC_SUCCESS) break;
        if (status != JXL_ENC_NEED_MORE_OUTPUT) fail("encoding failed");
        out.resize(out.size() * 2);
    }
    out.resize(used);
    return out;
}

JxlPicture decode(const uint8_t* data, size_t size, size_t maxSamples) {
    Decoder dec;
    if (!dec.p) fail("the decoder could not be created");
    if (JxlDecoderSubscribeEvents(dec.p, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS)
        fail("the decoder could not be set up");
    if (JxlDecoderSetInput(dec.p, data, size) != JXL_DEC_SUCCESS) fail("the decoder refused the data");
    JxlDecoderCloseInput(dec.p);

    JxlPicture picture;
    bool complete = false;
    for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec.p);
        if (status == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info;
            if (JxlDecoderGetBasicInfo(dec.p, &info) != JXL_DEC_SUCCESS) fail("the picture's header is unreadable");
            if (info.num_color_channels != 1 && info.num_color_channels != 3) fail("not one or three channels");
            if (info.exponent_bits_per_sample != 0) fail("floating-point samples are not supported");
            if (info.xsize == 0 || info.ysize == 0 ||
                static_cast<uint64_t>(info.xsize) * info.ysize > maxSamples / info.num_color_channels)
                fail("the picture is larger than expected");
            picture.width = info.xsize;
            picture.height = info.ysize;
            picture.channels = info.num_color_channels;
            picture.bitsPerSample = info.bits_per_sample;
            picture.originalValues = info.uses_original_profile != JXL_FALSE;
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            if (picture.channels == 0) fail("pixels before the header");
            // Extra channels (alpha) are left out: only the picture itself is asked for.
            const JxlPixelFormat format = {picture.channels, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
            size_t bytes = 0;
            if (JxlDecoderImageOutBufferSize(dec.p, &format, &bytes) != JXL_DEC_SUCCESS) fail("the decoder gave no size");
            const size_t wanted = static_cast<size_t>(picture.width) * picture.height * picture.channels;
            if (bytes != wanted * sizeof(uint16_t)) fail("the decoder's size does not match the header");
            picture.samples.resize(wanted);
            if (JxlDecoderSetImageOutBuffer(dec.p, &format, picture.samples.data(), bytes) != JXL_DEC_SUCCESS)
                fail("the decoder refused the output buffer");
        } else if (status == JXL_DEC_FULL_IMAGE) {
            complete = true;  // the first frame is the picture; anything after it is not read
            break;
        } else if (status == JXL_DEC_SUCCESS) {
            break;
        } else if (status == JXL_DEC_NEED_MORE_INPUT) {
            fail("the data is cut short");
        } else {
            fail("the data does not decode");
        }
    }
    if (!complete || picture.samples.empty()) fail("the data holds no picture");
    return picture;
}

}  // namespace

JxlPicture decodeJxl(const uint8_t* data, size_t size, size_t maxSamples) {
    if (!data || size == 0) fail("no data");
    // Decoding in several threads is preceded by the same first use; a file
    // being read was not necessarily written by this run.
    prepare(1, 3);
    prepare(1, 3, true);
    return decode(data, size, maxSamples);
}

#else  // no libjxl

bool jxlAvailable() { return false; }

std::string jxlVersion() { return {}; }

std::vector<uint8_t> encodeJxlLossless(const uint16_t*, uint32_t, uint32_t, uint32_t, int) {
    throw std::runtime_error("this build of dngconv has no JPEG XL support (it was built without libjxl)");
}

std::vector<uint8_t> encodeJxlLossy(const uint16_t*, uint32_t, uint32_t, float, int) {
    throw std::runtime_error("this build of dngconv has no JPEG XL support (it was built without libjxl)");
}

JxlPicture decodeJxl(const uint8_t*, size_t, size_t) {
    throw std::runtime_error("this build of dngconv has no JPEG XL support (it was built without libjxl)");
}

#endif

}  // namespace dngconv
