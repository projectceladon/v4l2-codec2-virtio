// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_ALLOCATOR_MEM_DEALER_H
#define ANDROID_C2_ALLOCATOR_MEM_DEALER_H

#include <C2Buffer.h>

namespace android {

class C2AllocatorMemDealer : public C2Allocator {
public:
    virtual id_t getId() const override;

    virtual C2String getName() const override;

    virtual std::shared_ptr<const Traits> getTraits() const override;

    virtual c2_status_t newLinearAllocation(
            uint32_t capacity, C2MemoryUsage usage,
            std::shared_ptr<C2LinearAllocation>* allocation) override;

    C2AllocatorMemDealer(id_t id);
    virtual ~C2AllocatorMemDealer();

private:
    std::shared_ptr<const Traits> mTraits;
};

}  // namespace android

#endif  // ANDROID_C2_ALLOCATOR_MEM_DEALER_H
