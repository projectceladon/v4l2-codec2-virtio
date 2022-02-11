// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define LOG_NDEBUG 0
#define LOG_TAG "VideoFrame"
#define ATRACE_TAG ATRACE_TAG_VIDEO

#include <v4l2_codec2/components/VideoFrame.h>

#include <C2AllocatorGralloc.h>
#include <log/log.h>
#include <utils/Trace.h>

namespace android {

// static
std::unique_ptr<VideoFrame> VideoFrame::Create(std::shared_ptr<C2GraphicBlock> block) {
    if (!block) return nullptr;

    std::vector<int> fds;
    const C2Handle* const handle = block->handle();
    for (int i = 0; i < handle->numFds; i++) {
        fds.emplace_back(handle->data[i]);
    }

    return std::unique_ptr<VideoFrame>(new VideoFrame(std::move(block), std::move(fds)));
}

VideoFrame::VideoFrame(std::shared_ptr<C2GraphicBlock> block, std::vector<int> fds)
      : mGraphicBlock(std::move(block)), mFds(fds) {}

VideoFrame::~VideoFrame() = default;

const std::vector<int>& VideoFrame::getFDs() const {
    return mFds;
}

void VideoFrame::setVisibleRect(const media::Rect& visibleRect) {
    mVisibleRect = visibleRect;
}

const media::Rect& VideoFrame::getVisibleRect() const {
    return mVisibleRect;
}

void VideoFrame::setBitstreamId(int32_t bitstreamId) {
    mBitstreamId = bitstreamId;
}

int32_t VideoFrame::getBitstreamId() const {
    return mBitstreamId;
}

C2ConstGraphicBlock VideoFrame::getGraphicBlock() {
    ALOGV("%s,mVisibleRect:%dx%d ", __func__, mVisibleRect.width(), mVisibleRect.height());
    return mGraphicBlock->share(C2Rect(mVisibleRect.width(), mVisibleRect.height()), ::C2Fence());
}

std::shared_ptr<C2GraphicBlock> VideoFrame::getRawGraphicBlock() {
    return mGraphicBlock;
}

std::shared_ptr<C2GraphicBlock> VideoFrame::setRawGraphicBlock(
        std::shared_ptr<C2GraphicBlock> block) {
    return mGraphicBlock = block;
}
}  // namespace android
