// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef VIDEO_DECODE_ACCELERATOR_H_
#define VIDEO_DECODE_ACCELERATOR_H_

#include <vector>

#include "size.h"
#include "video_codecs.h"

namespace media {

// Video decoder interface.
// This interface is extended by the various components that ultimately
// implement the backend of PPB_VideoDecoder_Dev.
class VideoDecodeAccelerator {
 public:
  // Specification of a decoding profile supported by an decoder.
  // |max_resolution| and |min_resolution| are inclusive.
  struct SupportedProfile {
    SupportedProfile();
    ~SupportedProfile();
    VideoCodecProfile profile;
    Size max_resolution;
    Size min_resolution;
    bool encrypted_only;
  };
  using SupportedProfiles = std::vector<SupportedProfile>;
};

}  // namespace media

#endif  // VIDEO_DECODE_ACCELERATOR_H_
