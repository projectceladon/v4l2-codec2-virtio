// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_V4L2_CODEC2_COMPONENTS_BITSTREAMBUFFER_H
#define ANDROID_V4L2_CODEC2_COMPONENTS_BITSTREAMBUFFER_H

#include <stdint.h>

#include <base/files/scoped_file.h>

namespace android {

struct BitstreamBuffer {
    BitstreamBuffer(const int32_t id, base::ScopedFD dmabuf_fd, const size_t offset,
                    const size_t size)
          : id(id), dmabuf_fd(std::move(dmabuf_fd)), offset(offset), size(size) {}
    ~BitstreamBuffer() = default;

    const int32_t id;
    base::ScopedFD dmabuf_fd;
    const size_t offset;
    const size_t size;
};

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_COMPONENTS_BITSTREAMBUFFER_H
