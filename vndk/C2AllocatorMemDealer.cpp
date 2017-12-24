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

#include <sys/mman.h>

namespace android {

/* ================================= MEMORY DEALER ALLOCATION ================================== */

class C2AllocationMemDealer : public C2LinearAllocation {
public:
    virtual ~C2AllocationMemDealer();

    virtual c2_status_t map(size_t offset, size_t size, C2MemoryUsage usage, int* fence,
                            void** addr /* nonnull */) override;
    virtual c2_status_t unmap(void* addr, size_t size, int* fenceFd) override;
    virtual bool isValid() const override;
    virtual const C2Handle* handle() const override;
    virtual bool equals(const std::shared_ptr<C2LinearAllocation>& other) const override;

    // internal methods
    C2AllocationMemDealer(uint32_t capacity);
    c2_status_t status() const;

protected:
    class Impl;
    Impl* mImpl;
};

class C2AllocationMemDealer::Impl {
public:
    // NOTE: using constructor here instead of a factory method as we will need the
    // error value and this simplifies the error handling by the wrapper.
    Impl(uint32_t capacity) : mInit(C2_OK), mHandle(nullptr), mMapSize(0u) {
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

    c2_status_t map(size_t offset, size_t size, C2MemoryUsage usage, int* fenceFd, void** addr) {
        (void)fenceFd;  // TODO: wait for fence
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

    c2_status_t unmap(void* addr, size_t size, int* fenceFd) {
        if (addr != mMemory->pointer() || size != mMapSize) {
            return C2_BAD_VALUE;
        }
        mMapSize = 0u;
        if (fenceFd) {
            *fenceFd = -1;
        }
        return C2_OK;
    }

    ~Impl() { native_handle_delete(mHandle); }

    c2_status_t status() const { return mInit; }

    const C2Handle* handle() const { return mHandle; }

private:
    c2_status_t mInit;
    C2Handle* mHandle;
    sp<IMemory> mMemory;
    size_t mMapSize;
};

c2_status_t C2AllocationMemDealer::map(size_t offset, size_t size, C2MemoryUsage usage,
                                       int* fenceFd, void** addr) {
    return mImpl->map(offset, size, usage, fenceFd, addr);
}

c2_status_t C2AllocationMemDealer::unmap(void* addr, size_t size, int* fenceFd) {
    return mImpl->unmap(addr, size, fenceFd);
}

bool C2AllocationMemDealer::isValid() const {
    return mImpl->status() == C2_OK;
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

C2AllocationMemDealer::C2AllocationMemDealer(uint32_t capacity)
      : C2LinearAllocation(capacity), mImpl(new Impl(capacity)) {}

/* ================================ MEMORY DEALER ALLOCATOR ==================================== */

C2AllocatorMemDealer::C2AllocatorMemDealer() {}

C2AllocatorMemDealer::~C2AllocatorMemDealer() {}

C2Allocator::id_t C2AllocatorMemDealer::getId() const {
    return 0;  // TODO implement ID
}

C2String C2AllocatorMemDealer::getName() const {
    return "vda.allocator.memdealer";
}

c2_status_t C2AllocatorMemDealer::newLinearAllocation(
        uint32_t capacity, C2MemoryUsage usage, std::shared_ptr<C2LinearAllocation>* allocation) {
    (void)usage;  // is usage needed?
    *allocation = nullptr;

    auto alloc = std::make_shared<C2AllocationMemDealer>(capacity);

    c2_status_t ret = alloc->status();
    if (ret == C2_OK) {
        *allocation = alloc;
    }
    return ret;
}

}  // namespace android
