// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef ANDROID_VERSION_NYC
#include <C2AllocatorCrosGrallocNyc.h>
#else
#include <C2AllocatorCrosGralloc.h>
#endif
#include <C2AllocatorMemDealer.h>

#include <C2BufferPriv.h>
#include <C2Component.h>
#include <C2VDASupport.h>

#include <map>
#include <memory>
#include <mutex>

namespace android {

class C2VDAAllocatorStore : public C2AllocatorStore {
public:
    enum : id_t {
        MEM_DEALER = VENDOR_START,
        CROS_GRALLOC,
    };

    C2VDAAllocatorStore() = default;

    c2_status_t fetchAllocator(id_t id, std::shared_ptr<C2Allocator>* const allocator) override;

    std::vector<std::shared_ptr<const C2Allocator::Traits>> listAllocators_nb() const override {
        return std::vector<std::shared_ptr<const C2Allocator::Traits>>();  /// TODO
    }

    C2String getName() const override { return "vda.allocator-store"; }

private:
    // returns a shared-singleton memory dealer allocator
    std::shared_ptr<C2Allocator> fetchMemDealerAllocator();

    // returns a shared-singleton CrOS gralloc allocator
    std::shared_ptr<C2Allocator> fetchCrosGrallocAllocator();

    static std::weak_ptr<C2Allocator> mMemDealerAllocator;
    static std::weak_ptr<C2Allocator> mCrosGrallocAllocator;
};

// Instantiate
std::weak_ptr<C2Allocator> C2VDAAllocatorStore::mMemDealerAllocator;
std::weak_ptr<C2Allocator> C2VDAAllocatorStore::mCrosGrallocAllocator;

c2_status_t C2VDAAllocatorStore::fetchAllocator(id_t id,
                                                std::shared_ptr<C2Allocator>* const allocator) {
    allocator->reset();
    switch (id) {
    case C2VDAAllocatorStore::MEM_DEALER:
    case C2AllocatorStore::DEFAULT_LINEAR:
        *allocator = fetchMemDealerAllocator();
        break;

    case C2VDAAllocatorStore::CROS_GRALLOC:
    case C2AllocatorStore::DEFAULT_GRAPHIC:
        *allocator = fetchCrosGrallocAllocator();
        break;

    default:
        return C2_NOT_FOUND;
    }
    if (*allocator == nullptr) {
        return C2_NO_MEMORY;
    }
    return C2_OK;
}

std::shared_ptr<C2Allocator> C2VDAAllocatorStore::fetchMemDealerAllocator() {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    auto allocator = mMemDealerAllocator.lock();
    if (!allocator) {
        allocator = std::make_shared<C2AllocatorMemDealer>();
        mMemDealerAllocator = allocator;
    }
    return allocator;
}

std::shared_ptr<C2Allocator> C2VDAAllocatorStore::fetchCrosGrallocAllocator() {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    auto allocator = mCrosGrallocAllocator.lock();
    if (!allocator) {
        allocator = std::make_shared<C2AllocatorCrosGralloc>();
        mCrosGrallocAllocator = allocator;
    }
    return allocator;
}

std::shared_ptr<C2AllocatorStore> getCodec2VDAAllocatorStore() {
    return std::make_shared<C2VDAAllocatorStore>();
}

c2_status_t getCodec2BlockPool(C2BlockPool::local_id_t id,
                               std::shared_ptr<const C2Component> component,
                               std::shared_ptr<C2BlockPool>* pool) {
    pool->reset();
    if (!component) {
        return C2_BAD_VALUE;
    }
    std::shared_ptr<C2AllocatorStore> allocatorStore = getCodec2VDAAllocatorStore();
    std::shared_ptr<C2Allocator> allocator;
    c2_status_t res = C2_NOT_FOUND;

    switch (id) {
    case C2BlockPool::BASIC_LINEAR:
        res = allocatorStore->fetchAllocator(C2AllocatorStore::DEFAULT_LINEAR, &allocator);
        if (res == C2_OK) {
            *pool = std::make_shared<C2BasicLinearBlockPool>(allocator);
        }
        break;
    case C2BlockPool::BASIC_GRAPHIC:
        res = allocatorStore->fetchAllocator(C2AllocatorStore::DEFAULT_GRAPHIC, &allocator);
        if (res == C2_OK) {
            *pool = std::make_shared<C2BasicGraphicBlockPool>(allocator);
        }
        break;
    default:
        break;
    }
    return res;
}

}  // namespace android
