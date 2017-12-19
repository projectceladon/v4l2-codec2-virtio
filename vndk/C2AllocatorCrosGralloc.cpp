// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2AllocatorCrosGralloc"

#include <C2AllocatorCrosGralloc.h>
#include <C2Buffer.h>

namespace android {

// TODO(johnylin): implement gralloc buffer allocation for ARC++P usage and test on ARC++P device.

/* =================================== CROS GRALLOC ALLOCATOR ================================== */

C2AllocatorCrosGralloc::C2AllocatorCrosGralloc() {}

C2AllocatorCrosGralloc::~C2AllocatorCrosGralloc() {}

C2Allocator::id_t C2AllocatorCrosGralloc::getId() const {
    return 1;  // TODO implement ID
}

C2String C2AllocatorCrosGralloc::getName() const {
    return "vda.allocator.crosgralloc";
}

c2_status_t C2AllocatorCrosGralloc::newGraphicAllocation(
        uint32_t width, uint32_t height, uint32_t format, C2MemoryUsage usage,
        std::shared_ptr<C2GraphicAllocation>* allocation) {
    (void)width;
    (void)height;
    (void)format;
    (void)usage;
    (void)allocation;
    return C2_OMITTED;
}

}  // namespace android
