// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// #define LOG_NDEBUG 0
#define LOG_TAG "VideoFramePool"
#define ATRACE_TAG ATRACE_TAG_VIDEO

#include <v4l2_codec2/components/VideoFramePool.h>

#include <stdint.h>
#include <memory>

#include <android/hardware/graphics/common/1.0/types.h>
#include <base/bind.h>
#include <base/memory/ptr_util.h>
#include <base/time/time.h>
#include <log/log.h>

#include <utils/Trace.h>

#include <v4l2_codec2/common/VideoTypes.h>
#include <v4l2_codec2/plugin_store/C2VdaBqBlockPool.h>
#include <v4l2_codec2/plugin_store/C2VdaPooledBlockPool.h>
#include <v4l2_codec2/plugin_store/V4L2AllocatorId.h>

using android::hardware::graphics::common::V1_0::BufferUsage;

//#define OUT_NV12_TO_RGBA
//#define OUT_RGBA_TO_RGBA
#if defined OUT_NV12_TO_RGBA && defined OUT_RGBA_TO_RGBA
#error "OUT_NV12_TO_RGBA and OUT_RGBA_TO_RGBA cannot defined together"
#endif

namespace android {

// static
std::optional<uint32_t> VideoFramePool::getBufferIdFromGraphicBlock(const C2BlockPool& blockPool,
                                                                    const C2Block2D& block) {
    ALOGV("%s() blockPool.getAllocatorId() = %u", __func__, blockPool.getAllocatorId());

    if (mOutputFormatConverter.get()) {
        return mOutputFormatConverter.get()->getBufferIdFromGraphicBlock(block);
    }

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
bool VideoFramePool::setNotifyBlockAvailableCb(C2BlockPool& blockPool, ::base::OnceClosure cb) {
    ALOGV("%s() blockPool.getAllocatorId() = %u", __func__, blockPool.getAllocatorId());

    if (blockPool.getAllocatorId() == C2PlatformAllocatorStore::BUFFERQUEUE) {
        C2VdaBqBlockPool* bqPool = static_cast<C2VdaBqBlockPool*>(&blockPool);
        return bqPool->setNotifyBlockAvailableCb(std::move(cb));
    }
    return false;
}

// static
std::unique_ptr<VideoFramePool> VideoFramePool::Create(
        std::shared_ptr<C2BlockPool> blockPool, const size_t numBuffers, const media::Size& size,
        HalPixelFormat pixelFormat, bool isSecure,
        scoped_refptr<::base::SequencedTaskRunner> taskRunner) {
    ALOG_ASSERT(blockPool != nullptr);

#if !defined OUT_NV12_TO_RGBA && !defined OUT_RGBA_TO_RGBA
    if (requestNewBufferSet(*blockPool, numBuffers) != C2_OK) {
        return nullptr;
    }
#endif
    std::unique_ptr<VideoFramePool> pool = ::base::WrapUnique(new VideoFramePool(
            std::move(blockPool), size, pixelFormat, isSecure, numBuffers, std::move(taskRunner)));
    if (!pool->initialize()) return nullptr;

    return pool;
}

VideoFramePool::VideoFramePool(std::shared_ptr<C2BlockPool> blockPool, const media::Size& size,
                               HalPixelFormat pixelFormat, bool isSecure, const size_t numBuffers,
                               scoped_refptr<::base::SequencedTaskRunner> taskRunner)
      : mBlockPool(std::move(blockPool)),
        mSize(size),
        mPixelFormat(pixelFormat),
        mMemoryUsage(C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE),
        mClientTaskRunner(std::move(taskRunner)) {
    ALOGV("%s(size=%dx%d)", __func__, size.width(), size.height());
    ALOG_ASSERT(mClientTaskRunner->RunsTasksInCurrentSequence());
    DCHECK(mBlockPool);
    DCHECK(mClientTaskRunner);
#ifdef OUT_NV12_TO_RGBA
    mOutputFormatConverter = OutputFormatConverter::Create(
            media::VideoPixelFormat::PIXEL_FORMAT_NV12, size, numBuffers, size);
    if (!mOutputFormatConverter) ALOGV("%s create OutputFormatConverter failed", __func__);
#endif
#ifdef OUT_RGBA_TO_RGBA
    mOutputFormatConverter = OutputFormatConverter::Create(
            media::VideoPixelFormat::PIXEL_FORMAT_ABGR/* PIXEL_FORMAT_ARGB*/, size, numBuffers, size);
    if (!mOutputFormatConverter) ALOGV("%s create OutputFormatConverter failed", __func__);
#endif
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

bool VideoFramePool::getVideoFrame(GetVideoFrameCB cb) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mClientTaskRunner->RunsTasksInCurrentSequence());
    ATRACE_CALL();

    if (mOutputCb) {
        return false;
    }

    mOutputCb = std::move(cb);
    mFetchTaskRunner->PostTask(
            FROM_HERE, ::base::BindOnce(&VideoFramePool::getVideoFrameTask, mFetchWeakThis));
    return true;
}

// static
void VideoFramePool::getVideoFrameTaskThunk(
        scoped_refptr<::base::SequencedTaskRunner> taskRunner,
        std::optional<::base::WeakPtr<VideoFramePool>> weakPool) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(weakPool);

    taskRunner->PostTask(FROM_HERE,
                         ::base::BindOnce(&VideoFramePool::getVideoFrameTask, *weakPool));
}

void VideoFramePool::getVideoFrameTaskFromConverterPool() {}

void VideoFramePool::convertFrame(std::shared_ptr<C2GraphicBlock> from,
                                  std::shared_ptr<C2GraphicBlock>* to) {
    c2_status_t status;
    if (mOutputFormatConverter) {
        *to = mOutputFormatConverter->convertBlock(from, &status);
        if (status != C2_OK) ALOGE("%s(), convertBlock failed:%d", __func__, status);
    } else {
        *to = from;
    }
}

void VideoFramePool::retrunFrame(std::shared_ptr<C2GraphicBlock> block) {
    c2_status_t status;
    if (mOutputFormatConverter) {
        status = mOutputFormatConverter->returnBlock(block);
        if (status != C2_OK) ALOGE("%s(), convertBlock failed:%d", __func__, status);
    }
}

void VideoFramePool::getVideoFrameTask() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mFetchTaskRunner->RunsTasksInCurrentSequence());
    ATRACE_CALL();

    // Variables used to exponential backoff retry when buffer fetching times out.
    constexpr size_t kFetchRetryDelayInit = 16384;    // Initial delay: 16ms
    constexpr size_t kFetchRetryDelayMax = 16384;  // Max delay: 16ms (1 frame at 60fps)
    static size_t sNumRetries = 0;
    static size_t sDelay = kFetchRetryDelayInit;

    std::shared_ptr<C2GraphicBlock> block;
    c2_status_t err;
    if (mOutputFormatConverter) {
        err = mOutputFormatConverter->fetchGraphicBlock(&block);
    } else {
        err = mBlockPool->fetchGraphicBlock(mSize.width(), mSize.height(),
                                            static_cast<uint32_t>(mPixelFormat), mMemoryUsage,
                                            &block);
    }
    if (err == C2_TIMED_OUT || err == C2_BLOCKING) {
        if (!mOutputFormatConverter &&
            setNotifyBlockAvailableCb(*mBlockPool,
                                      ::base::BindOnce(&VideoFramePool::getVideoFrameTaskThunk,
                                                       mFetchTaskRunner, mFetchWeakThis))) {
            ALOGV("%s(): fetchGraphicBlock() timeout, waiting for block available.", __func__);
        } else {
            ALOGV("%s(): fetchGraphicBlock() timeout, waiting %zuus (%zu retry)", __func__, sDelay,
                  sNumRetries + 1);
            mFetchTaskRunner->PostDelayedTask(
                    FROM_HERE, ::base::BindOnce(&VideoFramePool::getVideoFrameTask, mFetchWeakThis),
                    ::base::TimeDelta::FromMicroseconds(sDelay));

            sDelay = std::min(sDelay * 2, kFetchRetryDelayMax);  // Exponential backoff
            sNumRetries++;
        }

        return;
    }

    // Reset to the default value.
    sNumRetries = 0;
    sDelay = kFetchRetryDelayInit;

    std::optional<FrameWithBlockId> frameWithBlockId;
    if (err == C2_OK) {
        ALOG_ASSERT(block != nullptr);
        std::optional<uint32_t> bufferId = getBufferIdFromGraphicBlock(*mBlockPool, *block);
        std::unique_ptr<VideoFrame> frame = VideoFrame::Create(std::move(block));
        // Only pass the frame + id pair if both have successfully been obtained.
        // Otherwise exit the loop so a nullopt is passed to the client.
        if (ATRACE_ENABLED())
            ATRACE_INT("bufferId", *bufferId);
        if (bufferId && frame) {
            frameWithBlockId = std::make_pair(std::move(frame), *bufferId);
        } else {
            ALOGE("%s(): Failed to generate VideoFrame or get the buffer id.", __func__);
        }
    } else {
        ALOGE("%s(): Failed to fetch block, err=%d", __func__, err);
    }

    mClientTaskRunner->PostTask(
            FROM_HERE, ::base::BindOnce(&VideoFramePool::onVideoFrameReady, mClientWeakThis,
                                        std::move(frameWithBlockId)));
}

void VideoFramePool::onVideoFrameReady(std::optional<FrameWithBlockId> frameWithBlockId) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mClientTaskRunner->RunsTasksInCurrentSequence());
    ATRACE_CALL();

    if (!frameWithBlockId) {
        ALOGE("Failed to get GraphicBlock, abandoning all pending requests.");
        mClientWeakThisFactory.InvalidateWeakPtrs();
        mClientWeakThis = mClientWeakThisFactory.GetWeakPtr();
    }

    ALOG_ASSERT(mOutputCb);
    std::move(mOutputCb).Run(std::move(frameWithBlockId));
}

}  // namespace android
