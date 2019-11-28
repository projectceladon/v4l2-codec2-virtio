// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// #define LOG_NDEBUG 0
#define LOG_TAG "MediaCodecDecoder"

#include "mediacodec_decoder.h"

#include <assert.h>
#include <inttypes.h>

#include <utility>
#include <vector>

#include <media/NdkMediaFormat.h>
#include <utils/Log.h>

namespace android {
namespace {
// The timeout of AMediaCodec_dequeueOutputBuffer function calls.
constexpr int kTimeoutWaitForOutputUs = 1000;  // 1 millisecond

// The timeout of AMediaCodec_dequeueInputBuffer function calls.
constexpr int kTimeoutWaitForInputUs = 1000;  // 1 millisecond

// The maximal retry times of doDecode routine.
// The maximal tolerable interval between two dequeued outputs will be:
//   kTimeoutWaitForOutputUs * kTimeoutMaxRetries = 500 milliseconds
constexpr size_t kTimeoutMaxRetries = 500;

// The specified framerate for generating input timestamps.
constexpr int32_t kFrameRate = 25;

// Helper function to get possible decoder names from |type|.
std::vector<const char*> GetC2VideoDecoderNames(VideoCodecType type) {
    switch (type) {
    case VideoCodecType::H264:
        return {"c2.vda.avc.decoder", "ARC.h264.decode"};
    case VideoCodecType::VP8:
        return {"c2.vda.vp8.decoder", "ARC.vp8.decode"};
    case VideoCodecType::VP9:
        return {"c2.vda.vp9.decoder", "ARC.vp9.decode"};
    default:  // unknown type
        return {};
    }
}

#if ANDROID_VERSION >= 0x0900
const uint32_t BUFFER_FLAG_CODEC_CONFIG = AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;
const char* FORMAT_KEY_SLICE_HEIGHT = AMEDIAFORMAT_KEY_SLICE_HEIGHT;
#else
// Define non-exported constants of MediaCodec NDK interface here for usage of
// Android Version < Pie.
const uint32_t BUFFER_FLAG_CODEC_CONFIG = 2;
const char* FORMAT_KEY_SLICE_HEIGHT = "slice-height";
#endif

}  // namespace

// static
std::unique_ptr<MediaCodecDecoder> MediaCodecDecoder::Create(const std::string& input_path,
                                                             VideoCodecProfile profile,
                                                             const Size& video_size) {
    if (video_size.IsEmpty()) {
        ALOGE("Size is not valid: %dx%d", video_size.width, video_size.height);
        return nullptr;
    }

    VideoCodecType type = VideoCodecProfileToType(profile);

    std::unique_ptr<EncodedDataHelper> encoded_data_helper(new EncodedDataHelper(input_path, type));
    if (!encoded_data_helper->IsValid()) {
        ALOGE("EncodedDataHelper is not created for file: %s", input_path.c_str());
        return nullptr;
    }

    AMediaCodec* codec = nullptr;
    auto decoder_names = GetC2VideoDecoderNames(type);
    for (const auto& decoder_name : decoder_names) {
        codec = AMediaCodec_createCodecByName(decoder_name);
        if (codec) {
            ALOGD("Created mediacodec decoder by name: %s", decoder_name);
            break;
        }
    }
    if (!codec) {
        ALOGE("Failed to create mediacodec decoder.");
        return nullptr;
    }

    return std::unique_ptr<MediaCodecDecoder>(
            new MediaCodecDecoder(codec, std::move(encoded_data_helper), type, video_size));
}

MediaCodecDecoder::MediaCodecDecoder(AMediaCodec* codec,
                                     std::unique_ptr<EncodedDataHelper> encoded_data_helper,
                                     VideoCodecType type, const Size& size)
      : codec_(codec),
        encoded_data_helper_(std::move(encoded_data_helper)),
        type_(type),
        input_visible_size_(size) {}

MediaCodecDecoder::~MediaCodecDecoder() {
    if (codec_ != nullptr) {
        AMediaCodec_delete(codec_);
    }
}

void MediaCodecDecoder::AddOutputBufferReadyCb(const OutputBufferReadyCb& cb) {
    output_buffer_ready_cbs_.push_back(cb);
}

void MediaCodecDecoder::AddOutputFormatChangedCb(const OutputFormatChangedCb& cb) {
    output_format_changed_cbs_.push_back(cb);
}

void MediaCodecDecoder::Rewind() {
    encoded_data_helper_->Rewind();
    input_fragment_index_ = 0;
}

bool MediaCodecDecoder::Configure() {
    ALOGD("configure: mime=%s, width=%d, height=%d", GetMimeType(type_), input_visible_size_.width,
          input_visible_size_.height);
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, GetMimeType(type_));
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, input_visible_size_.width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, input_visible_size_.height);
    media_status_t ret = AMediaCodec_configure(codec_, format, nullptr /* surface */,
                                               nullptr /* crtpto */, 0 /* flag */);
    AMediaFormat_delete(format);
    if (ret != AMEDIA_OK) {
        ALOGE("Configure return error: %d", ret);
        return false;
    }
    return true;
}

bool MediaCodecDecoder::Start() {
    media_status_t ret = AMediaCodec_start(codec_);
    if (ret != AMEDIA_OK) {
        ALOGE("Start return error: %d", ret);
        return false;
    }
    return true;
}

bool MediaCodecDecoder::Decode() {
    while (!output_done_) {
        size_t retries = 0;
        bool success = false;

        // It will keep retrying until one output buffer is dequeued successfully.
        // On each retry we would like to enqueue input buffers as fast as possible.
        // The retry loop will break as failure if maxmimum retries are reached or
        // errors returned from enqueue input buffer or dequeue output buffer.
        while (retries < kTimeoutMaxRetries && !success) {
            if (!EnqueueInputBuffers()) return false;

            switch (DequeueOutputBuffer()) {
            case DequeueStatus::RETRY:
                retries++;
                break;
            case DequeueStatus::SUCCESS:
                success = true;
                break;
            case DequeueStatus::FAILURE:
                return false;
            }
        }

        if (retries >= kTimeoutMaxRetries) {
            ALOGE("Decoder did not produce an output buffer after %zu retries", kTimeoutMaxRetries);
        }
        if (!success) return false;
    }
    return true;
}

bool MediaCodecDecoder::EnqueueInputBuffers() {
    ssize_t index;
    while (!input_done_) {
        index = AMediaCodec_dequeueInputBuffer(codec_, kTimeoutWaitForInputUs);
        if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
            return true;  // no available input buffers, try next time

        if (index < 0) {
            ALOGE("Unknown error while dequeueInputBuffer: %zd", index);
            return false;
        }

        if (encoded_data_helper_->ReachEndOfStream()) {
            if (!FeedEOSInputBuffer(index)) return false;
            input_done_ = true;
        } else {
            if (!FeedInputBuffer(index)) return false;
        }
    }
    return true;
}

MediaCodecDecoder::DequeueStatus MediaCodecDecoder::DequeueOutputBuffer() {
    AMediaCodecBufferInfo info;
    ssize_t index = AMediaCodec_dequeueOutputBuffer(codec_, &info, kTimeoutWaitForOutputUs);

    switch (index) {
    case AMEDIACODEC_INFO_TRY_AGAIN_LATER:
        ALOGV("Try again later is reported");
        return DequeueStatus::RETRY;
    case AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED:
        ALOGV("Output buffers changed");
        return DequeueStatus::RETRY;
    case AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED:
        ALOGV("Output format changed");
        if (GetOutputFormat())
            return DequeueStatus::SUCCESS;
        else
            return DequeueStatus::FAILURE;
    default:
        if (index < 0) {
            ALOGE("Unknown error while dequeueOutputBuffer: %zd", index);
            return DequeueStatus::FAILURE;
        }
        break;
    }

    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) output_done_ = true;
    if (!ReceiveOutputBuffer(index, info)) return DequeueStatus::FAILURE;
    return DequeueStatus::SUCCESS;
}

bool MediaCodecDecoder::Stop() {
    return AMediaCodec_stop(codec_) == AMEDIA_OK;
}

bool MediaCodecDecoder::FeedInputBuffer(size_t index) {
    assert(!encoded_data_helper_->ReachEndOfStream());

    size_t buf_size = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec_, index, &buf_size);
    if (!buf) {
        ALOGE("Failed to getInputBuffer: index=%zu", index);
        return false;
    }

    auto fragment = encoded_data_helper_->GetNextFragment();
    assert(fragment);

    if (buf_size < fragment->data.size()) {
        ALOGE("Input buffer size is not enough: buf_size=%zu, data_size=%zu", buf_size,
              fragment->data.size());
        return false;
    }

    memcpy(reinterpret_cast<char*>(buf), fragment->data.data(), fragment->data.size());

    uint32_t input_flag = 0;
    if (fragment->csd_flag) input_flag |= BUFFER_FLAG_CODEC_CONFIG;

    uint64_t timestamp_us = input_fragment_index_ * 1000000 / kFrameRate;

    ALOGV("queueInputBuffer(index=%zu, offset=0, size=%zu, time=%" PRIu64 ", flags=%u) #%d", index,
          fragment->data.size(), timestamp_us, input_flag, input_fragment_index_);
    media_status_t status = AMediaCodec_queueInputBuffer(
            codec_, index, 0 /* offset */, fragment->data.size(), timestamp_us, input_flag);
    if (status != AMEDIA_OK) {
        ALOGE("Failed to queueInputBuffer: %d", status);
        return false;
    }
    ++input_fragment_index_;
    return true;
}

bool MediaCodecDecoder::FeedEOSInputBuffer(size_t index) {
    // Timestamp of EOS input buffer is undefined, use 0 here to test decoder
    // robustness.
    uint64_t timestamp_us = 0;

    ALOGV("queueInputBuffer(index=%zu) EOS", index);
    media_status_t status =
            AMediaCodec_queueInputBuffer(codec_, index, 0 /* offset */, 0 /* size */, timestamp_us,
                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    if (status != AMEDIA_OK) {
        ALOGE("Failed to queueInputBuffer EOS: %d", status);
        return false;
    }
    return true;
}

bool MediaCodecDecoder::ReceiveOutputBuffer(size_t index, const AMediaCodecBufferInfo& info) {
    size_t out_size;
    uint8_t* buf = AMediaCodec_getOutputBuffer(codec_, index, &out_size);
    if (!buf) {
        ALOGE("Failed to getOutputBuffer(index=%zu)", index);
        return false;
    }

    received_outputs_++;
    ALOGV("ReceiveOutputBuffer(index=%zu, size=%d, time=%" PRId64 ", flags=%u) #%d", index,
          info.size, info.presentationTimeUs, info.flags, received_outputs_);

    // Do not callback for dummy EOS output (info.size == 0)
    if (info.size > 0) {
        for (const auto& callback : output_buffer_ready_cbs_)
            callback(buf, info.size, received_outputs_);
    }

    media_status_t status = AMediaCodec_releaseOutputBuffer(codec_, index, false /* render */);
    if (status != AMEDIA_OK) {
        ALOGE("Failed to releaseOutputBuffer(index=%zu): %d", index, status);
        return false;
    }
    return true;
}

bool MediaCodecDecoder::GetOutputFormat() {
    AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
    bool success = true;

    // Required formats
    int32_t width = 0;
    if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width)) {
        ALOGE("Cannot find width in format.");
        success = false;
    }

    int32_t height = 0;
    if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height)) {
        ALOGE("Cannot find height in format.");
        success = false;
    }

    int32_t color_format = 0;
    if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color_format)) {
        ALOGE("Cannot find color-format in format.");
        success = false;
    }

    // Optional formats
    int32_t crop_left = 0;
    int32_t crop_top = 0;
    int32_t crop_right = width - 1;
    int32_t crop_bottom = height - 1;
#if ANDROID_VERSION >= 0x0900  // Android 9.0 (Pie)
    // Crop info is only avaiable on NDK version >= Pie.
    if (!AMediaFormat_getRect(format, AMEDIAFORMAT_KEY_DISPLAY_CROP, &crop_left, &crop_top,
                              &crop_right, &crop_bottom)) {
        ALOGD("Cannot find crop window in format. Set as large as frame size.");
        crop_left = 0;
        crop_top = 0;
        crop_right = width - 1;
        crop_bottom = height - 1;
    }
#endif
    // Note: For ARC++N, width and height are set as same as the size of crop
    //       window in ArcCodec. So the values above will be still satisfied in
    //       ARC++N.

    // In current exiting ARC video decoder crop origin is always at (0,0)
    if (crop_left != 0 || crop_top != 0) {
        ALOGE("Crop origin is not (0,0): (%d,%d)", crop_left, crop_top);
        success = false;
    }

    int32_t stride = 0;
    if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_STRIDE, &stride)) {
        ALOGD("Cannot find stride in format. Set as frame width.");
        stride = width;
    }

    int32_t slice_height = 0;
    if (!AMediaFormat_getInt32(format, FORMAT_KEY_SLICE_HEIGHT, &slice_height)) {
        ALOGD("Cannot find slice-height in format. Set as frame height.");
        slice_height = height;
    }

    for (const auto& callback : output_format_changed_cbs_) {
        callback(Size(stride, slice_height),
                 Size(crop_right - crop_left + 1, crop_bottom - crop_top + 1), color_format);
    }
    return success;
}

}  // namespace android
