// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_ALLOCATOR_CROS_GRALLOC_H
#define ANDROID_C2_ALLOCATOR_CROS_GRALLOC_H

#include <C2Buffer.h>

namespace android {

class C2AllocatorCrosGralloc : public C2Allocator {
public:
    virtual id_t getId() const override;

    virtual C2String getName() const override;

    virtual std::shared_ptr<const Traits> getTraits() const override {
        return nullptr;  // TODO
    }

    virtual c2_status_t newGraphicAllocation(
            uint32_t width, uint32_t height, uint32_t format, C2MemoryUsage usage,
            std::shared_ptr<C2GraphicAllocation>* allocation) override;

    C2AllocatorCrosGralloc();
    virtual ~C2AllocatorCrosGralloc();
};

}  // namespace android

#endif  // ANDROID_C2_ALLOCATOR_CROS_GRALLOC_H
