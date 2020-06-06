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
#include <base/time/time.h>
#include <log/log.h>

#include <v4l2_codec2/components/VideoTypes.h>

using android::hardware::graphics::common::V1_0::BufferUsage;

namespace android {
namespace {
constexpr size_t kAllocateBufferMaxRetries = 32;
constexpr size_t kFetchRetryDelayUs = 500;
}  // namespace

VideoFramePool::VideoFramePool(std::shared_ptr<C2BlockPool> blockPool, const media::Size& size,
                               HalPixelFormat pixelFormat, bool isSecure,
                               scoped_refptr<::base::SequencedTaskRunner> taskRunner)
      : mBlockPool(std::move(blockPool)),
        mSize(size),
        mPixelFormat(pixelFormat),
        mMemoryUsage(isSecure ? C2MemoryUsage::READ_PROTECTED : C2MemoryUsage::CPU_READ,
                     static_cast<uint64_t>(BufferUsage::VIDEO_DECODER)),
        mTaskRunner(std::move(taskRunner)) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    DCHECK(mBlockPool);
    DCHECK(mTaskRunner);

    mWeakThis = mWeakThisFactory.GetWeakPtr();
}

VideoFramePool::~VideoFramePool() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    mWeakThisFactory.InvalidateWeakPtrs();
}

void VideoFramePool::getVideoFrame(GetVideoFrameCB cb) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    bool isRunning = !mCbQueue.empty();
    mCbQueue.push(std::move(cb));
    if (!isRunning) tryFetchGraphicBlock();
}

bool VideoFramePool::hasPendingRequests() const {
    return !mCbQueue.empty();
}

void VideoFramePool::tryFetchGraphicBlock() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mCbQueue.empty()) return;

    std::shared_ptr<C2GraphicBlock> block;
    auto err = mBlockPool->fetchGraphicBlock(mSize.width(), mSize.height(),
                                             static_cast<uint32_t>(mPixelFormat), mMemoryUsage,
                                             &block);

    if ((err == C2_TIMED_OUT || err == C2_BLOCKING) && mNumRetries++ < kAllocateBufferMaxRetries) {
        ALOGD("fetchGraphicBlock() timeout. retry %zu times", mNumRetries);
        mTaskRunner->PostDelayedTask(
                FROM_HERE, ::base::BindOnce(&VideoFramePool::tryFetchGraphicBlock, mWeakThis),
                ::base::TimeDelta::FromMicroseconds(kFetchRetryDelayUs));
    } else if (err != C2_OK) {
        ALOGE("Failed to fetch block, err=%d, retry %zu times", err, mNumRetries);
        sendVideoFrame(nullptr);
    } else {
        mNumRetries = 0;
        sendVideoFrame(VideoFrame::Create(std::move(block)));

        if (!mCbQueue.empty()) {
            mTaskRunner->PostTask(
                    FROM_HERE, ::base::BindOnce(&VideoFramePool::tryFetchGraphicBlock, mWeakThis));
        }
    }
}

void VideoFramePool::sendVideoFrame(std::unique_ptr<VideoFrame> frame) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    ALOG_ASSERT(!mCbQueue.empty());

    auto cb = std::move(mCbQueue.front());
    mCbQueue.pop();
    std::move(cb).Run(std::move(frame));
}

}  // namespace android
