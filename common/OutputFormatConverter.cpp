// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// #define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_VIDEO
#define LOG_TAG "OutputFormatConverter"
#include <utils/Trace.h>

#include <v4l2_codec2/common/FormatConverter.h>
#include <v4l2_codec2/common/OutputFormatConverter.h>

#include <inttypes.h>

#include <memory>
#include <string>

#include <C2AllocatorGralloc.h>
#include <C2PlatformSupport.h>
#include <android/hardware/graphics/common/1.0/types.h>
#include <inttypes.h>
#include <libyuv.h>
#include <ui/GraphicBuffer.h>
#include <utils/Log.h>

#include <v4l2_codec2/common/VideoTypes.h>  // for HalPixelFormat

using android::hardware::graphics::common::V1_0::BufferUsage;

#define RGBA_TO_RGBA_WA

namespace android {

namespace {
// The constant expression of mapping the pixel format conversion pair (src, dst) to a unique
// integer.
constexpr int convertMap(media::VideoPixelFormat src, media::VideoPixelFormat dst) {
    return static_cast<int>(src) *
                   (static_cast<int>(media::VideoPixelFormat::PIXEL_FORMAT_MAX) + 1) +
           static_cast<int>(dst);
}
}  // namespace

// static
std::unique_ptr<OutputFormatConverter> OutputFormatConverter::Create(
        media::VideoPixelFormat inFormat, const media::Size& visibleSize, uint32_t inputCount,
        const media::Size& codedSize) {
#ifndef RGBA_TO_RGBA_WA
    if (inFormat != media::VideoPixelFormat::PIXEL_FORMAT_NV12) {
        ALOGE("Unsupported output format: %d", static_cast<int32_t>(inFormat));
        return nullptr;
    }
#endif
    std::unique_ptr<OutputFormatConverter> converter(new OutputFormatConverter);
    if (converter->initialize(inFormat, visibleSize, inputCount, codedSize) != C2_OK) {
        ALOGE("Failed to initialize OutputFormatConverter");
        return nullptr;
    }
    return converter;
}

OutputFormatConverter::~OutputFormatConverter() {
    ALOGV("%s", __func__);
}

c2_status_t OutputFormatConverter::initialize(media::VideoPixelFormat inFormat,
                                              const media::Size& visibleSize, uint32_t inputCount,
                                              const media::Size& codedSize) {
    ALOGV("initialize(out_format=%s, visible_size=%dx%d, input_count=%u, coded_size=%dx%d)",
          media::VideoPixelFormatToString(inFormat).c_str(), visibleSize.width(),
          visibleSize.height(), inputCount, codedSize.width(), codedSize.height());

    mInFormat = inFormat;

    std::shared_ptr<C2BlockPool> pool;
    c2_status_t status = GetCodec2BlockPool(C2BlockPool::BASIC_GRAPHIC, nullptr, &pool);
    if (status != C2_OK) {
        ALOGE("Failed to get basic graphic block pool (err=%d)", status);
        return status;
    }

    HalPixelFormat halFormat;
    media::VideoPixelFormat outFormat = media::VideoPixelFormat::PIXEL_FORMAT_ABGR;
    if (mInFormat == media::VideoPixelFormat::PIXEL_FORMAT_I420) {
        // Android HAL format doesn't have I420, we use YV12 instead and swap U and V data while
        // conversion to perform I420.
        halFormat = HalPixelFormat::YV12;
    } else if (mInFormat == media::VideoPixelFormat::PIXEL_FORMAT_ABGR) {
        halFormat = HalPixelFormat::RGBA_8888;
    } else if (mInFormat == media::VideoPixelFormat::PIXEL_FORMAT_ARGB) {
        halFormat = HalPixelFormat::BGRA_8888;
    } else {
        halFormat = HalPixelFormat::YCBCR_420_888;  // will allocate NV12 by minigbm.
    }

    uint32_t bufferCount = std::max(inputCount, kMinInputBufferCount);
    for (uint32_t i = 0; i < bufferCount; i++) {
        std::shared_ptr<C2GraphicBlock> block;
        status = pool->fetchGraphicBlock(
                codedSize.width(), codedSize.height(), static_cast<uint32_t>(halFormat),
                {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE}, &block);
        if (status != C2_OK) {
            ALOGE("Failed to fetch graphic block (err=%d)", status);
            return status;
        }
        ALOGV("%s, emplace:(%d, %p) to mGraphicBlocks", __func__, i, block.get());
        mGraphicBlocks.emplace_back(new BlockEntry(i, std::move(block)));
        mAvailableQueue.push(mGraphicBlocks.back().get());
    }

    mOutFormat = outFormat;
    mVisibleSize = visibleSize;

    mTempPlaneU = std::unique_ptr<uint8_t[]>(
            new uint8_t[mVisibleSize.width() * mVisibleSize.height() / 4]);
    mTempPlaneV = std::unique_ptr<uint8_t[]>(
            new uint8_t[mVisibleSize.width() * mVisibleSize.height() / 4]);

    return C2_OK;
}

// allocate NV12 buffer for Virtio-Video
c2_status_t OutputFormatConverter::fetchGraphicBlock(std::shared_ptr<C2GraphicBlock>* block) {
    if (!isReady()) {
        ALOGV("There is no available block from OutputFormatConverter Pool");
        return C2_TIMED_OUT;  // This is actually redundant and should not be used.
    }
    // if there isn't any entry available, then how to handle? the same thread will be used to push back block to mAvailableQueue
    // dead lock may happened?
    BlockEntry* entry = mAvailableQueue.front();
    std::shared_ptr<C2GraphicBlock> outputBlock = entry->mBlock;
    *block = outputBlock;
    mAvailableQueue.pop();

    return C2_OK;
}

std::optional<uint32_t> OutputFormatConverter::getBufferIdFromGraphicBlock(const C2Block2D& block) {
    auto iter =
            std::find_if(mGraphicBlocks.begin(), mGraphicBlocks.end(),
                         [&block](const std::unique_ptr<BlockEntry>& be) {
                             ALOGV("%s, find %p, current:%p", __func__, &block, be->mBlock.get());
                             return be->mBlock.get() == &block;
                         });
    if (iter == mGraphicBlocks.end()) {
        ALOGE("Failed to find graphic block by converted/zero-copied frame: %p", &block);
        return std::nullopt;
    }
    if ((*iter)->mAssociatedFrameIndex == kNoFrameAssociated) {
        ALOGE("invalid graphic block index of frame: %p", &block);
        return std::nullopt;
    }

    return (*iter)->mAssociatedFrameIndex;
}

//#define DUMP_SURFACE
std::shared_ptr<C2GraphicBlock> OutputFormatConverter::convertBlock(
        std::shared_ptr<C2GraphicBlock> inputBlock, c2_status_t* status) {
    std::shared_ptr<C2BlockPool> pool;
    GetCodec2BlockPool(C2BlockPool::BASIC_GRAPHIC, nullptr, &pool);

    std::shared_ptr<C2GraphicBlock> outputBlock;
    ALOGV("%s, allocate RGBA8888 of wxh=%dx%d", __func__, mVisibleSize.width(),
          mVisibleSize.height());
#if 1
    *status = pool->fetchGraphicBlock(mVisibleSize.width(), mVisibleSize.height(),
                                      static_cast<uint32_t>(HalPixelFormat::RGBA_8888),
                                      {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE},
                                      &outputBlock);
#else
    *status = pool->fetchGraphicBlock(mVisibleSize.width(), mVisibleSize.height(),
                                      static_cast<uint32_t>(HalPixelFormat::BGRA_8888),
                                      {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE},
                                      &outputBlock);
#endif
    if (*status != C2_OK) {
        ALOGE("Failed to fetch graphic block (err=%d)", *status);
        return NULL;  //inputBlock;
        //return static_cast<C2ConstGraphicBlock> (inputBlock);
    }

    const C2GraphicView& inputView = inputBlock->map().get();
    C2PlanarLayout inputLayout = inputView.layout();

    C2GraphicView outputView = outputBlock->map().get();
    C2PlanarLayout outputLayout = outputView.layout();

    media::VideoPixelFormat inputFormat = media::VideoPixelFormat::PIXEL_FORMAT_UNKNOWN;
    *status = C2_OK;
    if (inputLayout.type == C2PlanarLayout::TYPE_YUV) {
        const uint8_t* srcY = inputView.data()[C2PlanarLayout::PLANE_Y];
        const uint8_t* srcU = inputView.data()[C2PlanarLayout::PLANE_U];
        const uint8_t* srcV = inputView.data()[C2PlanarLayout::PLANE_V];
        const int srcStrideY = inputLayout.planes[C2PlanarLayout::PLANE_Y].rowInc;
        const int srcStrideU = inputLayout.planes[C2PlanarLayout::PLANE_U].rowInc;
        if (inputLayout.rootPlanes == 3) {
            inputFormat = media::VideoPixelFormat::PIXEL_FORMAT_YV12;
        } else if (inputLayout.rootPlanes == 2) {
            inputFormat = (srcV > srcU) ? media::VideoPixelFormat::PIXEL_FORMAT_NV12
                                        : media::VideoPixelFormat::PIXEL_FORMAT_NV21;
        }
        ALOGV("%s inputFormat:%d, mOutFormat:%d", __func__, inputFormat, mOutFormat);

        switch (convertMap(inputFormat, mOutFormat)) {
        case convertMap(media::VideoPixelFormat::PIXEL_FORMAT_NV12,
                        media::VideoPixelFormat::PIXEL_FORMAT_ABGR):
            ALOGV("%s, convert NV12 to RGBA", __func__);
            uint8_t* dstRGB = outputView.data()[C2PlanarLayout::PLANE_R];
            const int dstStrideRGB = outputLayout.planes[C2PlanarLayout::PLANE_R].rowInc;
            ALOGV("srcY:%p, srcStrideY:%d, srcU:%p, srcStrideU:%d,dstRGB:%p, dstStrideRGB:%d, "
                  "wxh:%dx%d ",
                  srcY, srcStrideY, srcU, srcStrideU, dstRGB, dstStrideRGB, mVisibleSize.width(),
                  mVisibleSize.height());
            libyuv::NV12ToABGR(srcY, srcStrideY, srcU, srcStrideU, dstRGB, dstStrideRGB,
                               mVisibleSize.width(), mVisibleSize.height());
#ifdef DUMP_SURFACE
            static FILE* m_f;
            static int count = 0;
            if (count++ < 100) {
                if (!m_f) {
                    m_f = fopen("/data/local/traces/dec.yuv", "w+");
                    ALOGV("/data/local/traces/dec.yuv: created: %p", m_f);
                }
                if (m_f) {
                    fwrite(srcY, mVisibleSize.width() * mVisibleSize.height(), 1, m_f);
                    fwrite(srcU, mVisibleSize.width() * mVisibleSize.height() / 2, 1, m_f);
                }
            } else {
                if (m_f) {
                    fclose(m_f);
                    m_f = NULL;
                }
            }
#endif
            break;
#if 0
        //compiler said it cannot jump to this default, I don't know why
        default:
            ALOGE("Unsupported pixel format conversion from %s to %s",
                  media::VideoPixelFormatToString(inputFormat).c_str(),
                  media::VideoPixelFormatToString(mOutFormat).c_str());
            *status = C2_CORRUPTED;
            return NULL;//inputBlock;  // This is actually redundant and should not be used.
#endif
        }
    } else if (inputLayout.type == C2PlanarLayout::TYPE_RGB) {
        // There is only RGBA_8888 specified in C2AllocationGralloc::map(), no BGRA_8888. Maybe
        // BGRA_8888 is not used now?
        inputFormat = media::VideoPixelFormat::PIXEL_FORMAT_ABGR;

        const uint8_t* srcRGB = inputView.data()[C2PlanarLayout::PLANE_R];
        //const int srcStrideRGB = inputLayout.planes[C2PlanarLayout::PLANE_R].rowInc;

        uint8_t* dstRGB = outputView.data()[C2PlanarLayout::PLANE_R];

        switch (convertMap(inputFormat, mOutFormat)) {
        case convertMap(media::VideoPixelFormat::PIXEL_FORMAT_ABGR,
                        media::VideoPixelFormat::PIXEL_FORMAT_ABGR):
            ALOGV("%s, copy RGBA to RGBA\n", __func__);
            memcpy(dstRGB, srcRGB, 4 * mVisibleSize.width() * mVisibleSize.height());
            break;
        default:
            ALOGE("Unsupported pixel format conversion from %s to %s",
                  media::VideoPixelFormatToString(inputFormat).c_str(),
                  media::VideoPixelFormatToString(mOutFormat).c_str());
            *status = C2_CORRUPTED;
            return NULL;  //inputBlock;  // This is actually redundant and should not be used.
        }
    } else {
        ALOGE("Unsupported input layout type");
        *status = C2_CORRUPTED;
        return NULL;  //inputBlock;  // This is actually redundant and should not be used.
    }

    ALOGV("convertBlock(frame=%p, format=%s)", inputBlock.get(),
          media::VideoPixelFormatToString(inputFormat).c_str());
    returnBlock(inputBlock);

    return outputBlock;
    //return outputBlock->share(C2Rect(mVisibleSize.width(), mVisibleSize.height()), C2Fence());
}

c2_status_t OutputFormatConverter::returnBlock(std::shared_ptr<C2GraphicBlock> block) {
    ALOGV("returnBlock(%p)", block.get());

    auto iter = std::find_if(mGraphicBlocks.begin(), mGraphicBlocks.end(),
                             [block](const std::unique_ptr<BlockEntry>& be) {
                                 ALOGV("%s, return: %p, current:%p", __func__, block.get(),
                                       be->mBlock.get());
                                 return be->mBlock.get() == block.get();
                             });
    if (iter == mGraphicBlocks.end()) {
        ALOGE("Failed to return: %p, not belong to mGraphicBlocks", block.get());
        return C2_BAD_INDEX;
    }

    if ((*iter)->mBlock) {
        // Returned block is format converted.
        mAvailableQueue.push(iter->get());
    } else {
        // Returned block is zero-copied.
        //ALOGV("%s, erase () from ", __func__);
        mGraphicBlocks.erase(iter);
    }
    return C2_OK;
}

}  // namespace android
