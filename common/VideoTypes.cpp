// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// #define LOG_NDEBUG 0
#define LOG_TAG "VideoTypes"

#include <v4l2_codec2/common/VideoTypes.h>

#include <log/log.h>

namespace android {

const char* VideoCodecToString(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264:
        return "H264";
    case VideoCodec::H265:
        return "H265";
    case VideoCodec::VP8:
        return "VP8";
    case VideoCodec::VP9:
        return "VP9";
    }
}

const char* HalPixelFormatToString(HalPixelFormat format) {
    switch (format) {
    case HalPixelFormat::UNKNOWN:
        return "Unknown";
    case HalPixelFormat::YCBCR_420_888:
        return "YCBCR_420_888";
    case HalPixelFormat::BGRA_8888:
        return "BGRA_8888";
    case HalPixelFormat::RGBA_8888:
        return "RGBA_8888";
    case HalPixelFormat::RGB_888:
        return "RGB_888";
    case HalPixelFormat::YV12:
        return "YV12";
    case HalPixelFormat::NV12:
        return "NV12";
    }
}

}  // namespace android
