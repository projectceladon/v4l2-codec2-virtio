// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2AllocatorCrosGralloc"

#include <C2AllocatorCrosGrallocNyc.h>

#include <C2Buffer.h>

#include <media/stagefright/MediaDefs.h>
#include <ui/GraphicBuffer.h>
#include <utils/Log.h>
#include <utils/misc.h>

#include <sys/mman.h>
#include <limits>

namespace android {

/* ================================== CROS GRALLOC ALLOCATION ================================== */

class C2AllocationCrosGralloc : public C2GraphicAllocation {
public:
    virtual ~C2AllocationCrosGralloc();

    virtual c2_status_t map(C2Rect rect, C2MemoryUsage usage, C2Fence* fence,
                            C2PlanarLayout* layout /* nonnull */,
                            uint8_t** addr /* nonnull */) override;
    virtual c2_status_t unmap(uint8_t** addr, C2Rect rect, C2Fence* fence /* nullable */) override;
    virtual C2Allocator::id_t getAllocatorId() const override;
    virtual const C2Handle* handle() const override;
    virtual bool equals(const std::shared_ptr<const C2GraphicAllocation>& other) const override;

    // internal methods
    C2AllocationCrosGralloc(sp<IGraphicBufferAlloc> allocator, uint32_t width, uint32_t height,
                            uint32_t format, uint32_t usage, C2Allocator::id_t allocatorId);
    c2_status_t status() const;

protected:
    class Impl;
    Impl* mImpl;
};

class C2AllocationCrosGralloc::Impl {
public:
    Impl(sp<IGraphicBufferAlloc> allocator, uint32_t width, uint32_t height, uint32_t format,
         uint32_t usage, C2Allocator::id_t allocatorId)
          : mInit(C2_OK), mLocked(false), mAllocatorId(allocatorId) {
        if (format != HAL_PIXEL_FORMAT_YCbCr_420_888) {
            ALOGE("only support format HAL_PIXEL_FORMAT_YCbCr_420_888");
            mInit = C2_BAD_VALUE;
            return;
        }
        status_t error = OK;
        mGraphicBuffer = allocator->createGraphicBuffer(width, height, format, usage, &error);
        if (mGraphicBuffer == nullptr) {
            ALOGE("failed to allocate buffer: %d", error);
            mInit = C2_CORRUPTED;
        }
    }

    ~Impl() {}

    c2_status_t map(C2Rect rect, C2MemoryUsage usage, C2Fence* fence,
                    C2PlanarLayout* layout /* nonnull */, uint8_t** addr /* nonnull */) {
        // TODO
        (void)fence;
        if (mLocked) {
            return C2_DUPLICATE;
        }
        if (!layout || !addr) {
            return C2_BAD_VALUE;
        }
        if (usage.expected != C2MemoryUsage::CPU_READ) {
            return C2_BAD_VALUE;  // always use GRALLOC_USAGE_SW_READ_OFTEN
        }

        // For simplicity, only support mapping with the same size as allocation.
        if (rect != C2Rect(mGraphicBuffer->getWidth(), mGraphicBuffer->getHeight())) {
            ALOGE("Only support the same size mapping.");
            return C2_BAD_VALUE;
        }

        // Resolve the format
        struct android_ycbcr ycbcr;
        memset(&ycbcr, 0, sizeof(ycbcr));
        LOG_ALWAYS_FATAL_IF(mGraphicBuffer->lockYCbCr(GRALLOC_USAGE_SW_READ_OFTEN, &ycbcr));
        addr[C2PlanarLayout::PLANE_Y] = (uint8_t*)ycbcr.y;
        addr[C2PlanarLayout::PLANE_U] = (uint8_t*)ycbcr.cb;
        addr[C2PlanarLayout::PLANE_V] = (uint8_t*)ycbcr.cr;
        ALOGV("Mapped as addr y=%p cb=%p cr=%p, chrome_step=%zu, stride y=%zu c=%zu",
              addr[C2PlanarLayout::PLANE_Y], addr[C2PlanarLayout::PLANE_U],
              addr[C2PlanarLayout::PLANE_V], ycbcr.chroma_step, ycbcr.ystride, ycbcr.cstride);

        LOG_ALWAYS_FATAL_IF(ycbcr.chroma_step != 1 && ycbcr.chroma_step != 2);
        layout->type = C2PlanarLayout::TYPE_YUV;
        layout->numPlanes = 3;
        layout->rootPlanes = 3;
        layout->planes[C2PlanarLayout::PLANE_Y] = {
                C2PlaneInfo::CHANNEL_Y,   // channel
                1,                        // colInc
                (int32_t)ycbcr.ystride,   // rowInc
                1,                        // colSampling
                1,                        // rowSampling
                8,                        // allocatedDepth
                8,                        // bitDepth
                0,                        // valueShift
                C2PlaneInfo::NATIVE,      // endianness
                C2PlanarLayout::PLANE_Y,  // rootIx
                0,                        // offset
        };
        layout->planes[C2PlanarLayout::PLANE_U] = {
                C2PlaneInfo::CHANNEL_CB,     // channel
                (int32_t)ycbcr.chroma_step,  // colInc
                (int32_t)ycbcr.cstride,      // rowInc
                2,                           // colSampling
                2,                           // rowSampling
                8,                           // allocatedDepth
                8,                           // bitDepth
                0,                           // valueShift
                C2PlaneInfo::NATIVE,         // endianness
                C2PlanarLayout::PLANE_U,     // rootIx
                0,                           // offset
        };
        layout->planes[C2PlanarLayout::PLANE_V] = {
                C2PlaneInfo::CHANNEL_CR,     // channel
                (int32_t)ycbcr.chroma_step,  // colInc
                (int32_t)ycbcr.cstride,      // rowInc
                2,                           // colSampling
                2,                           // rowSampling
                8,                           // allocatedDepth
                8,                           // bitDepth
                0,                           // valueShift
                C2PlaneInfo::NATIVE,         // endianness
                C2PlanarLayout::PLANE_V,     // rootIx
                0,                           // offset
        };
        // handle interleaved formats
        intptr_t uvOffset = addr[C2PlanarLayout::PLANE_V] - addr[C2PlanarLayout::PLANE_U];
        if (uvOffset > 0 && uvOffset < (intptr_t)ycbcr.chroma_step) {
            layout->rootPlanes = 2;
            layout->planes[C2PlanarLayout::PLANE_V].rootIx = C2PlanarLayout::PLANE_U;
            layout->planes[C2PlanarLayout::PLANE_V].offset = uvOffset;
        } else if (uvOffset < 0 && uvOffset > -(intptr_t)ycbcr.chroma_step) {
            layout->rootPlanes = 2;
            layout->planes[C2PlanarLayout::PLANE_U].rootIx = C2PlanarLayout::PLANE_V;
            layout->planes[C2PlanarLayout::PLANE_U].offset = -uvOffset;
        }

        LOG_ALWAYS_FATAL_IF(layout->numPlanes > C2PlanarLayout::MAX_NUM_PLANES);
        mLocked = true;
        return C2_OK;
    }

    c2_status_t unmap(uint8_t** addr, C2Rect rect, C2Fence* fence /* nullable */) {
        (void)addr;   // TODO
        (void)rect;   // TODO
        (void)fence;  // TODO
        mGraphicBuffer->unlock();
        mLocked = false;
        return C2_OK;
    }

    c2_status_t status() const { return mInit; }

    const C2Handle* handle() const { return mGraphicBuffer->handle; }

    C2Allocator::id_t getAllocatorId() const { return mAllocatorId; }

private:
    c2_status_t mInit;
    sp<GraphicBuffer> mGraphicBuffer;
    bool mLocked;
    C2Allocator::id_t mAllocatorId;
};

C2AllocationCrosGralloc::C2AllocationCrosGralloc(sp<IGraphicBufferAlloc> allocator, uint32_t width,
                                                 uint32_t height, uint32_t format, uint32_t usage,
                                                 C2Allocator::id_t allocatorId)
      : C2GraphicAllocation(width, height),
        mImpl(new Impl(allocator, width, height, format, usage, allocatorId)) {}

C2AllocationCrosGralloc::~C2AllocationCrosGralloc() {
    delete mImpl;
}

c2_status_t C2AllocationCrosGralloc::map(C2Rect rect, C2MemoryUsage usage, C2Fence* fence,
                                         C2PlanarLayout* layout /* nonnull */,
                                         uint8_t** addr /* nonnull */) {
    return mImpl->map(rect, usage, fence, layout, addr);
}

c2_status_t C2AllocationCrosGralloc::unmap(uint8_t** addr, C2Rect rect,
                                           C2Fence* fence /* nullable */) {
    return mImpl->unmap(addr, rect, fence);
}

C2Allocator::id_t C2AllocationCrosGralloc::getAllocatorId() const {
    return mImpl->getAllocatorId();
}

const C2Handle* C2AllocationCrosGralloc::handle() const {
    return mImpl->handle();
}

bool C2AllocationCrosGralloc::equals(
        const std::shared_ptr<const C2GraphicAllocation>& other) const {
    (void)other;
    return false;  // TODO(johnylin)
}

c2_status_t C2AllocationCrosGralloc::status() const {
    return mImpl->status();
}

/* =================================== CROS GRALLOC ALLOCATOR ================================== */

C2AllocatorCrosGralloc::C2AllocatorCrosGralloc(id_t id) {
    mComposer = ComposerService::getComposerService();
    if (mComposer.get() == nullptr) {
        ALOGE("failed to connect to SurfaceComposer");
        return;
    }

    mAllocator = mComposer->createGraphicBufferAlloc();
    if (mAllocator.get() == nullptr) {
        ALOGE("failed to create GraphicBuffer allocator");
    }

    C2MemoryUsage minUsage = {0, 0};
    C2MemoryUsage maxUsage = {std::numeric_limits<uint64_t>::max(),
                              std::numeric_limits<uint64_t>::max()};
    Traits traits = {"vda.allocator.crosgralloc", id, C2Allocator::GRAPHIC, minUsage, maxUsage};
    mTraits = std::make_shared<C2Allocator::Traits>(traits);
}

C2AllocatorCrosGralloc::~C2AllocatorCrosGralloc() {}

C2Allocator::id_t C2AllocatorCrosGralloc::getId() const {
    return mTraits->id;
}

C2String C2AllocatorCrosGralloc::getName() const {
    return mTraits->name;
}

std::shared_ptr<const C2Allocator::Traits> C2AllocatorCrosGralloc::getTraits() const {
    return mTraits;
}

c2_status_t C2AllocatorCrosGralloc::newGraphicAllocation(
        uint32_t width, uint32_t height, uint32_t format, C2MemoryUsage usage,
        std::shared_ptr<C2GraphicAllocation>* allocation) {
    *allocation = nullptr;
    if (usage.expected != C2MemoryUsage::CPU_READ) {
        return C2_BAD_VALUE;  // always use GRALLOC_USAGE_SW_READ_OFTEN
    }

    auto alloc = std::make_shared<C2AllocationCrosGralloc>(
            mAllocator, width, height, format, GRALLOC_USAGE_SW_READ_OFTEN, getId());

    c2_status_t ret = alloc->status();
    if (ret == C2_OK) {
        *allocation = alloc;
    }
    return ret;
}

}  // namespace android
