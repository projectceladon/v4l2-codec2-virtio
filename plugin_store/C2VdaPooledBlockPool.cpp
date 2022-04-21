// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define LOG_NDEBUG 0
#define LOG_TAG "C2VdaPooledBlockPool"
#define ATRACE_TAG ATRACE_TAG_VIDEO

#include <v4l2_codec2/plugin_store/C2VdaPooledBlockPool.h>
#include <v4l2_codec2/plugin_store/C2GrallocMapper.h>

#include <time.h>

#include <C2AllocatorGralloc.h>
#include <C2BlockInternal.h>
#include <bufferpool/BufferPoolTypes.h>
#include <log/log.h>

#include <utils/Trace.h>

namespace android {
namespace {
// The wait time for another try to fetch a buffer from bufferpool.
const int64_t kFetchRetryDelayUs = 10 * 1000;

int64_t GetNowUs() {
    struct timespec t;
    t.tv_sec = 0;
    t.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t nsecs = static_cast<int64_t>(t.tv_sec) * 1000000000LL + t.tv_nsec;
    return nsecs / 1000ll;
}
}  // namespace

using android::hardware::media::bufferpool::BufferPoolData;

// static
std::optional<uint32_t> C2VdaPooledBlockPool::getBufferIdFromGraphicBlock(const C2Block2D& block) {
    native_handle_t* grallocHandle = android::UnwrapNativeCodec2GrallocHandle(block.handle());

    auto& mapper = C2GrallocMapper::getMapper();

    uint64_t id;
    mapper.getBackingStore(grallocHandle, &id);
    ALOGV("backingstoreid:%d", (int)id);
    native_handle_delete(grallocHandle);
    return (uint32_t)id;
}

// Tries to fetch a buffer from bufferpool. When the size of |mBufferIds| is smaller than
// |mBufferCount|, pass the obtained buffer to caller and record its ID in BufferPoolData to
// |mBufferIds|. When the size of |mBufferIds| is equal to |mBufferCount|, pass the obtained
// buffer only if its ID is included in |mBufferIds|. Otherwise, discard the buffer and
// return C2_TIMED_OUT.
c2_status_t C2VdaPooledBlockPool::fetchGraphicBlock(uint32_t width, uint32_t height,
                                                    uint32_t format, C2MemoryUsage usage,
                                                    std::shared_ptr<C2GraphicBlock>* block) {
    ALOG_ASSERT(block != nullptr);
    std::lock_guard<std::mutex> lock(mMutex);

    if (mNextFetchTimeUs != 0) {
        int delayUs = GetNowUs() - mNextFetchTimeUs;
        if (delayUs > 0) {
            ::usleep(delayUs);
        }
        mNextFetchTimeUs = 0;
    }

    std::shared_ptr<C2GraphicBlock> fetchBlock;
    c2_status_t err =
            C2PooledBlockPool::fetchGraphicBlock(width, height, format, usage, &fetchBlock);

    if (err != C2_OK) {
        ALOGE("Failed at C2PooledBlockPool::fetchGraphicBlock: %d", err);
        return err;
    }

    std::optional<uint32_t> bufferId = getBufferIdFromGraphicBlock(*fetchBlock);
    if (!bufferId) {
        ALOGE("Failed to getBufferIdFromGraphicBlock");
        return C2_CORRUPTED;
    }

    if (mBufferIds.size() < mBufferCount) {
        mBufferIds.insert(*bufferId);
    }

    if (mBufferIds.find(*bufferId) != mBufferIds.end()) {
        ALOGV("Returned buffer id = %u", *bufferId);
        *block = std::move(fetchBlock);
        //ALOGV("fetchGraphicBlock, backingstoreid:%d", (int)id);
        return C2_OK;
    }
    //mBufferBlocks.insert(fetchBlock);
    ALOGV("dyang23, cache *bufferId:%d", *bufferId);
    ALOGV("No buffer could be recycled now, wait for another try...");
    mNextFetchTimeUs = GetNowUs() + kFetchRetryDelayUs;
    return C2_TIMED_OUT;
}

c2_status_t C2VdaPooledBlockPool::requestNewBufferSet(int32_t bufferCount) {
    if (bufferCount <= 0) {
        ALOGE("Invalid requested buffer count = %d", bufferCount);
        return C2_BAD_VALUE;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    mBufferIds.clear();
    mBufferCount = bufferCount;
    return C2_OK;
}

}  // namespace android
