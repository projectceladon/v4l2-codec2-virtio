// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2AllocatorCrosGralloc"

#include <sys/mman.h>

#include <media/stagefright/MediaDefs.h>
#include <ui/GraphicBuffer.h>
#include <utils/Log.h>
#include <utils/misc.h>

#include <C2AllocatorCrosGrallocNyc.h>
#include <C2Buffer.h>

namespace android {

/* ================================== CROS GRALLOC ALLOCATION ================================== */

class C2AllocationCrosGralloc : public C2GraphicAllocation {
public:
    virtual ~C2AllocationCrosGralloc();

    virtual c2_status_t map(
            C2Rect rect, C2MemoryUsage usage, int *fenceFd,
            C2PlaneLayout *layout /* nonnull */, uint8_t **addr /* nonnull */) override;
    virtual c2_status_t unmap(C2Fence *fenceFd /* nullable */) override;
    virtual bool isValid() const override;
    virtual const C2Handle *handle() const override;
    virtual bool equals(const std::shared_ptr<const C2GraphicAllocation> &other) const override;

    // internal methods
    C2AllocationCrosGralloc(sp<IGraphicBufferAlloc> allocator, uint32_t width, uint32_t height,
                            uint32_t format, uint32_t usage);
    c2_status_t status() const;

protected:
    class Impl;
    Impl* mImpl;
};

class C2AllocationCrosGralloc::Impl {
public:
    Impl(sp<IGraphicBufferAlloc> allocator, uint32_t width, uint32_t height, uint32_t format,
         uint32_t usage) : mInit(C2_OK), mLocked(false) {
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

    c2_status_t map(C2Rect rect, C2MemoryUsage usage, int *fenceFd,
                    C2PlaneLayout *layout /* nonnull */, uint8_t **addr /* nonnull */) {
        // TODO
        (void) fenceFd;
        if (mLocked) {
            return C2_DUPLICATE;
        }
        if (!layout || !addr) {
            return C2_BAD_VALUE;
        }
        if (usage.mConsumer != C2MemoryUsage::kSoftwareRead) {
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
        LOG_ALWAYS_FATAL_IF(mGraphicBuffer->lockYCbCr(C2MemoryUsage::kSoftwareRead, &ycbcr));
        addr[C2PlaneLayout::Y] = (uint8_t*)ycbcr.y;
        addr[C2PlaneLayout::U] = (uint8_t*)ycbcr.cb;
        addr[C2PlaneLayout::V] = (uint8_t*)ycbcr.cr;
        if (addr[C2PlaneLayout::U] > addr[C2PlaneLayout::V]) {
            // YCrCb format
            std::swap(addr[C2PlaneLayout::U], addr[C2PlaneLayout::V]);
        }
        ALOGV("Mapped as addr y=%p cb=%p cr=%p, chrome_step=%zu, stride y=%zu c=%zu",
              addr[C2PlaneLayout::Y], addr[C2PlaneLayout::U], addr[C2PlaneLayout::V],
              ycbcr.chroma_step, ycbcr.ystride, ycbcr.cstride);

        LOG_ALWAYS_FATAL_IF(ycbcr.chroma_step != 1 && ycbcr.chroma_step != 2);
        layout->mType = C2PlaneLayout::MEDIA_IMAGE_TYPE_YUV;
        layout->mPlanes[C2PlaneLayout::Y] = {
            C2PlaneInfo::Y,           // mChannel
            1,                        // mColInc
            (int32_t)ycbcr.ystride,   // mRowInc
            1,                        // mHorizSubsampling
            1,                        // mVertSubsampling
            8,                        // mBitDepth
            8,                        // mAllocatedDepth
        };

        if (ycbcr.chroma_step == 2) {
            // Semi-planar format
            layout->mNumPlanes = 2;
            layout->mPlanes[C2PlaneLayout::U] = {
                C2PlaneInfo::Cb,            // mChannel
                (int32_t)ycbcr.chroma_step, // mColInc
                (int32_t)ycbcr.cstride,     // mRowInc
                1,                          // mHorizSubsampling
                2,                          // mVertSubsampling
                8,                          // mBitDepth
                8,                          // mAllocatedDepth
            };
            addr[C2PlaneLayout::V] = nullptr;
        } else {
            layout->mNumPlanes = 3;
            layout->mPlanes[C2PlaneLayout::U] = {
                C2PlaneInfo::Cb,            // mChannel
                (int32_t)ycbcr.chroma_step, // mColInc
                (int32_t)ycbcr.cstride,     // mRowInc
                2,                          // mHorizSubsampling
                2,                          // mVertSubsampling
                8,                          // mBitDepth
                8,                          // mAllocatedDepth
            };
            layout->mPlanes[C2PlaneLayout::V] = {
                C2PlaneInfo::Cr,            // mChannel
                (int32_t)ycbcr.chroma_step, // mColInc
                (int32_t)ycbcr.cstride,     // mRowInc
                2,                          // mHorizSubsampling
                2,                          // mVertSubsampling
                8,                          // mBitDepth
                8,                          // mAllocatedDepth
            };
        }
        LOG_ALWAYS_FATAL_IF(layout->mNumPlanes > C2PlaneLayout::MAX_NUM_PLANES);
        mLocked = true;
        return C2_OK;
    }

    c2_status_t unmap(C2Fence *fenceFd /* nullable */) {
        (void) fenceFd;  // TODO
        mGraphicBuffer->unlock();
        mLocked = false;
        return C2_OK;
    }

    c2_status_t status() const {
        return mInit;
    }

    const C2Handle* handle() const {
        return mGraphicBuffer->handle;
    }

private:
    c2_status_t mInit;
    sp<GraphicBuffer> mGraphicBuffer;
    bool mLocked;
};

C2AllocationCrosGralloc::C2AllocationCrosGralloc(
        sp<IGraphicBufferAlloc> allocator, uint32_t width, uint32_t height,
        uint32_t format, uint32_t usage)
    : C2GraphicAllocation(width, height),
      mImpl(new Impl(allocator, width, height, format, usage)) {}

C2AllocationCrosGralloc::~C2AllocationCrosGralloc() {
    delete mImpl;
}

c2_status_t C2AllocationCrosGralloc::map(
        C2Rect rect, C2MemoryUsage usage, int *fenceFd,
        C2PlaneLayout *layout /* nonnull */, uint8_t **addr /* nonnull */) {
    return mImpl->map(rect, usage, fenceFd, layout, addr);
}

c2_status_t C2AllocationCrosGralloc::unmap(C2Fence *fenceFd /* nullable */) {
    return mImpl->unmap(fenceFd);
}

bool C2AllocationCrosGralloc::isValid() const {
    return mImpl->status() == C2_OK;
}

const C2Handle* C2AllocationCrosGralloc::handle() const {
    return mImpl->handle();
}

bool C2AllocationCrosGralloc::equals(
        const std::shared_ptr<const C2GraphicAllocation> &other) const {
    (void) other;
    return false;  // TODO(johnylin)
}

c2_status_t C2AllocationCrosGralloc::status() const {
    return mImpl->status();
}

/* =================================== CROS GRALLOC ALLOCATOR ================================== */

C2AllocatorCrosGralloc::C2AllocatorCrosGralloc() {
    mComposer = ComposerService::getComposerService();
    if (mComposer.get() == nullptr) {
        ALOGE("failed to connect to SurfaceComposer");
        return;
    }

    mAllocator = mComposer->createGraphicBufferAlloc();
    if (mAllocator.get() == nullptr) {
        ALOGE("failed to create GraphicBuffer allocator");
    }
}

C2AllocatorCrosGralloc::~C2AllocatorCrosGralloc() {}

C2Allocator::id_t C2AllocatorCrosGralloc::getId() const {
    return 1; // TODO implement ID
}

C2String C2AllocatorCrosGralloc::getName() const {
    return "vda.allocator.crosgralloc";
}

c2_status_t C2AllocatorCrosGralloc::newGraphicAllocation(
        uint32_t width, uint32_t height, uint32_t format, C2MemoryUsage usage,
        std::shared_ptr<C2GraphicAllocation> *allocation) {
    *allocation = nullptr;
    if (usage.mConsumer != C2MemoryUsage::kSoftwareRead) {
        return C2_BAD_VALUE;  // always use GRALLOC_USAGE_SW_READ_OFTEN
    }

    auto alloc = std::make_shared<C2AllocationCrosGralloc>(mAllocator, width, height, format,
                                                           C2MemoryUsage::kSoftwareRead);

    c2_status_t ret = alloc->status();
    if (ret == C2_OK) {
        *allocation = alloc;
    }
    return ret;
}

}  // namespace android
