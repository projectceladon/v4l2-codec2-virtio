// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "VideoFramePool"

#include <v4l2_codec2/components/VideoFramePool.h>

#include <stdint.h>
#include <memory>

#include <android/hardware/graphics/common/1.0/types.h>
#include <base/bind.h>
#include <base/memory/ptr_util.h>
#include <base/time/time.h>
#include <log/log.h>

#include <v4l2_codec2/common/VideoTypes.h>
#include <v4l2_codec2/plugin_store/C2VdaBqBlockPool.h>
#include <v4l2_codec2/plugin_store/C2VdaPooledBlockPool.h>
#include <v4l2_codec2/plugin_store/V4L2AllocatorId.h>

using android::hardware::graphics::common::V1_0::BufferUsage;

namespace android {

// static
std::optional<uint32_t> VideoFramePool::getBufferIdFromGraphicBlock(const C2BlockPool& blockPool,
                                                                    const C2Block2D& block) {
    ALOGV("%s() blockPool.getAllocatorId() = %u", __func__, blockPool.getAllocatorId());

    if (blockPool.getAllocatorId() == android::V4L2AllocatorId::V4L2_BUFFERPOOL) {
        return C2VdaPooledBlockPool::getBufferIdFromGraphicBlock(block);
    } else if (blockPool.getAllocatorId() == C2PlatformAllocatorStore::BUFFERQUEUE) {
        return C2VdaBqBlockPool::getBufferIdFromGraphicBlock(block);
    }

    ALOGE("%s(): unknown allocator ID: %u", __func__, blockPool.getAllocatorId());
    return std::nullopt;
}

// static
c2_status_t VideoFramePool::requestNewBufferSet(C2BlockPool& blockPool, int32_t bufferCount) {
    ALOGV("%s() blockPool.getAllocatorId() = %u", __func__, blockPool.getAllocatorId());

    if (blockPool.getAllocatorId() == android::V4L2AllocatorId::V4L2_BUFFERPOOL) {
        C2VdaPooledBlockPool* bpPool = static_cast<C2VdaPooledBlockPool*>(&blockPool);
        return bpPool->requestNewBufferSet(bufferCount);
    } else if (blockPool.getAllocatorId() == C2PlatformAllocatorStore::BUFFERQUEUE) {
        C2VdaBqBlockPool* bqPool = static_cast<C2VdaBqBlockPool*>(&blockPool);
        return bqPool->requestNewBufferSet(bufferCount);
    }

    ALOGE("%s(): unknown allocator ID: %u", __func__, blockPool.getAllocatorId());
    return C2_BAD_VALUE;
}

// static
std::unique_ptr<VideoFramePool> VideoFramePool::Create(
        std::shared_ptr<C2BlockPool> blockPool, const size_t numBuffers, const media::Size& size,
        HalPixelFormat pixelFormat, bool isSecure,
        scoped_refptr<::base::SequencedTaskRunner> taskRunner) {
    ALOG_ASSERT(blockPool != nullptr);

    if (requestNewBufferSet(*blockPool, numBuffers) != C2_OK) {
        return nullptr;
    }

    std::unique_ptr<VideoFramePool> pool = ::base::WrapUnique(new VideoFramePool(
            std::move(blockPool), size, pixelFormat, isSecure, std::move(taskRunner)));
    if (!pool->initialize()) return nullptr;
    return pool;
}

VideoFramePool::VideoFramePool(std::shared_ptr<C2BlockPool> blockPool, const media::Size& size,
                               HalPixelFormat pixelFormat, bool isSecure,
                               scoped_refptr<::base::SequencedTaskRunner> taskRunner)
      : mBlockPool(std::move(blockPool)),
        mSize(size),
        mPixelFormat(pixelFormat),
        mMemoryUsage(isSecure ? C2MemoryUsage::READ_PROTECTED : C2MemoryUsage::CPU_READ,
                     static_cast<uint64_t>(BufferUsage::VIDEO_DECODER)),
        mClientTaskRunner(std::move(taskRunner)) {
    ALOGV("%s(size=%dx%d)", __func__, size.width(), size.height());
    ALOG_ASSERT(mClientTaskRunner->RunsTasksInCurrentSequence());
    DCHECK(mBlockPool);
    DCHECK(mClientTaskRunner);
}

bool VideoFramePool::initialize() {
    if (!mFetchThread.Start()) {
        ALOGE("Fetch thread failed to start.");
        return false;
    }
    mFetchTaskRunner = mFetchThread.task_runner();

    mClientWeakThis = mClientWeakThisFactory.GetWeakPtr();
    mFetchWeakThis = mFetchWeakThisFactory.GetWeakPtr();

    return true;
}

VideoFramePool::~VideoFramePool() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mClientTaskRunner->RunsTasksInCurrentSequence());

    mClientWeakThisFactory.InvalidateWeakPtrs();
    mCancelGetFrame = true;

    if (mFetchThread.IsRunning()) {
        mFetchTaskRunner->PostTask(FROM_HERE,
                                   ::base::BindOnce(&VideoFramePool::destroyTask, mFetchWeakThis));
        mFetchThread.Stop();
    }
}

void VideoFramePool::destroyTask() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mFetchTaskRunner->RunsTasksInCurrentSequence());

    mFetchWeakThisFactory.InvalidateWeakPtrs();
}

void VideoFramePool::getVideoFrame(GetVideoFrameCB cb) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mClientTaskRunner->RunsTasksInCurrentSequence());

    ++mNumPendingRequests;
    mFetchTaskRunner->PostTask(FROM_HERE, ::base::BindOnce(&VideoFramePool::getVideoFrameTask,
                                                           mFetchWeakThis, std::move(cb)));
}

bool VideoFramePool::hasPendingRequests() const {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mClientTaskRunner->RunsTasksInCurrentSequence());

    return mNumPendingRequests > 0;
}

void VideoFramePool::getVideoFrameTask(GetVideoFrameCB cb) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mFetchTaskRunner->RunsTasksInCurrentSequence());
    // Initial delay: 64us
    constexpr size_t kFetchRetryDelayInit = 64;
    // Max delay: 16ms (1 frame at 60fps)
    constexpr size_t kFetchRetryDelayMax = 16384;
    std::optional<FrameWithBlockId> frameWithBlockId;

    size_t numRetries = 0;
    size_t delay = kFetchRetryDelayInit;
    while (true) {
        if (mCancelGetFrame) {
            ALOGW("Request to get frame canceled after %zu retries", numRetries);
            break;
        }

        std::shared_ptr<C2GraphicBlock> block;
        c2_status_t err = mBlockPool->fetchGraphicBlock(mSize.width(), mSize.height(),
                                                        static_cast<uint32_t>(mPixelFormat),
                                                        mMemoryUsage, &block);

        if (err == C2_OK) {
            ALOG_ASSERT(block != nullptr);
            std::optional<uint32_t> bufferId = getBufferIdFromGraphicBlock(*mBlockPool, *block);
            std::unique_ptr<VideoFrame> frame = VideoFrame::Create(std::move(block));
            // Only pass the frame + id pair if both have successfully been obtained.
            // Otherwise exit the loop so a nullopt is passed to the client.
            if (bufferId && frame) {
                frameWithBlockId = std::make_pair(std::move(frame), *bufferId);
            }
            break;
        } else if (err != C2_TIMED_OUT && err != C2_BLOCKING) {
            ALOGE("Failed to fetch block, err=%d, retry %zu times", err, numRetries);
            break;
        } else {
            ++numRetries;
            ALOGV("fetchGraphicBlock() timeout, waiting %zuus (%zu retry)", delay, numRetries);
            usleep(delay);
            // Exponential backoff
            delay = std::min(delay * 2, kFetchRetryDelayMax);
        }
    }

    mClientTaskRunner->PostTask(
            FROM_HERE, ::base::BindOnce(&VideoFramePool::onVideoFrameReady, mClientWeakThis,
                                        std::move(cb), std::move(frameWithBlockId)));
}

void VideoFramePool::onVideoFrameReady(GetVideoFrameCB cb,
                                       std::optional<FrameWithBlockId> frameWithBlockId) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mClientTaskRunner->RunsTasksInCurrentSequence());

    --mNumPendingRequests;

    if (!frameWithBlockId) {
        ALOGE("Failed to get GraphicBlock, abandoning all pending requests.");
        mClientWeakThisFactory.InvalidateWeakPtrs();
        mClientWeakThis = mClientWeakThisFactory.GetWeakPtr();

        mNumPendingRequests = 0;
    }

    std::move(cb).Run(std::move(frameWithBlockId));
}

}  // namespace android
