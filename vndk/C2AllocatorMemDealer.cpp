// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2AllocatorMemDealer"

#include <C2AllocatorMemDealer.h>

#include <C2Buffer.h>

#include <binder/IMemory.h>
#include <binder/MemoryDealer.h>
#include <media/stagefright/MediaDefs.h>
#include <utils/Log.h>
#include <utils/misc.h>

#include <limits>
#include <sys/mman.h>
#include <limits>

namespace android {

/* ================================= MEMORY DEALER ALLOCATION ================================== */

class C2AllocationMemDealer : public C2LinearAllocation {
public:
    virtual ~C2AllocationMemDealer();

    virtual c2_status_t map(size_t offset, size_t size, C2MemoryUsage usage, C2Fence* fence,
                            void** addr /* nonnull */) override;
    virtual c2_status_t unmap(void* addr, size_t size, C2Fence* fence) override;
    virtual C2Allocator::id_t getAllocatorId() const override;
    virtual const C2Handle* handle() const override;
    virtual bool equals(const std::shared_ptr<C2LinearAllocation>& other) const override;

    // internal methods
    C2AllocationMemDealer(uint32_t capacity, C2Allocator::id_t id);
    c2_status_t status() const;

protected:
    class Impl;
    Impl* mImpl;
};

class C2AllocationMemDealer::Impl {
public:
    // NOTE: using constructor here instead of a factory method as we will need the
    // error value and this simplifies the error handling by the wrapper.
    Impl(uint32_t capacity, C2Allocator::id_t id)
          : mInit(C2_OK), mHandle(nullptr), mMapSize(0u), mAllocatorId(id) {
        sp<MemoryDealer> dealer = new MemoryDealer(capacity, "C2AllocationMemDealer");
        mMemory = dealer->allocate(capacity);
        if (mMemory == nullptr || mMemory->pointer() == nullptr) {
            ALOGE("fail to allocate memory: %u", capacity);
            mInit = C2_NO_MEMORY;
            return;
        }

        // Create a handle for IMemory.
        sp<IMemoryHeap> heap = mMemory->getMemory();
        mHandle = native_handle_create(1 /* numFds */, 0 /* numInts */);
        if (mHandle == nullptr) {
            ALOGE("fail to create native handle.");
            mInit = C2_NO_MEMORY;
            return;
        }
        mHandle->data[0] = heap->getHeapID();
    }

    c2_status_t map(size_t offset, size_t size, C2MemoryUsage usage, C2Fence* fence, void** addr) {
        (void)fence;  // TODO: wait for fence
        (void)usage;
        *addr = nullptr;
        // For simplicity, only support offset = 0 mapping for now.
        if (offset != 0) {
            ALOGE("Only support offset=0 mapping now.");
            return C2_OMITTED;
        }
        if (size > mMemory->size()) {
            ALOGE("Mapping size(%zu) exceeds allocation size(%zu).", size, mMemory->size());
            return C2_NO_MEMORY;
        }

        *addr = mMemory->pointer();
        mMapSize = size;
        return C2_OK;
    }

    c2_status_t unmap(void* addr, size_t size, C2Fence* fence) {
        if (addr != mMemory->pointer() || size != mMapSize) {
            return C2_BAD_VALUE;
        }
        mMapSize = 0u;
        if (fence) {
            *fence = C2Fence();
        }
        return C2_OK;
    }

    ~Impl() { native_handle_delete(mHandle); }

    c2_status_t status() const { return mInit; }

    const C2Handle* handle() const { return mHandle; }

    C2Allocator::id_t getAllocatorId() const { return mAllocatorId; }

private:
    c2_status_t mInit;
    C2Handle* mHandle;
    sp<IMemory> mMemory;
    size_t mMapSize;
    C2Allocator::id_t mAllocatorId;
};

c2_status_t C2AllocationMemDealer::map(size_t offset, size_t size, C2MemoryUsage usage,
                                       C2Fence* fence, void** addr) {
    return mImpl->map(offset, size, usage, fence, addr);
}

c2_status_t C2AllocationMemDealer::unmap(void* addr, size_t size, C2Fence* fence) {
    return mImpl->unmap(addr, size, fence);
}

C2Allocator::id_t C2AllocationMemDealer::getAllocatorId() const {
    return mImpl->getAllocatorId();
}

c2_status_t C2AllocationMemDealer::status() const {
    return mImpl->status();
}

bool C2AllocationMemDealer::equals(const std::shared_ptr<C2LinearAllocation>& other) const {
    (void)other;
    return false;  // TODO(johnylin)
}

const C2Handle* C2AllocationMemDealer::handle() const {
    return mImpl->handle();
}

C2AllocationMemDealer::~C2AllocationMemDealer() {
    delete mImpl;
}

C2AllocationMemDealer::C2AllocationMemDealer(uint32_t capacity, C2Allocator::id_t id)
      : C2LinearAllocation(capacity), mImpl(new Impl(capacity, id)) {}

/* ================================ MEMORY DEALER ALLOCATOR ==================================== */

C2AllocatorMemDealer::C2AllocatorMemDealer(id_t id) {
    C2MemoryUsage minUsage = {0, 0};
    C2MemoryUsage maxUsage = {std::numeric_limits<uint64_t>::max(),
                              std::numeric_limits<uint64_t>::max()};
    Traits traits = {"vda.allocator.memdealer", id, C2Allocator::LINEAR, minUsage, maxUsage};
    mTraits = std::make_shared<C2Allocator::Traits>(traits);
}

C2AllocatorMemDealer::~C2AllocatorMemDealer() {}

C2Allocator::id_t C2AllocatorMemDealer::getId() const {
    return mTraits->id;
}

C2String C2AllocatorMemDealer::getName() const {
    return mTraits->name;
}

std::shared_ptr<const C2Allocator::Traits> C2AllocatorMemDealer::getTraits() const {
    return mTraits;
}

c2_status_t C2AllocatorMemDealer::newLinearAllocation(
        uint32_t capacity, C2MemoryUsage usage, std::shared_ptr<C2LinearAllocation>* allocation) {
    (void)usage;  // is usage needed?
    *allocation = nullptr;

    auto alloc = std::make_shared<C2AllocationMemDealer>(capacity, getId());

    c2_status_t ret = alloc->status();
    if (ret == C2_OK) {
        *allocation = alloc;
    }
    return ret;
}

}  // namespace android
