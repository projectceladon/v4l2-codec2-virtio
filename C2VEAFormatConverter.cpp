// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VEAFormatConverter"

#include <C2VDACommon.h>  // for HalPixelFormat
#include <C2VEAFormatConverter.h>

#include <C2AllocatorGralloc.h>
#include <C2PlatformSupport.h>

#include <utils/Log.h>

#include <inttypes.h>
#include <libyuv.h>

#include <memory>
#include <string>

namespace android {

namespace {
// The helper function to copy a plane pixel by pixel. It assumes bytesPerPixel is 1.
void copyPlaneByPixel(const uint8_t* src, int srcStride, int srcColInc, uint8_t* dst, int dstStride,
                      int dstColInc, int width, int height) {
    for (int row = 0; row < height; row++) {
        const uint8_t* srcRow = src;
        uint8_t* dstRow = dst;
        for (int col = 0; col < width; col++) {
            memcpy(dstRow, srcRow, 1);
            srcRow += srcColInc;
            dstRow += dstColInc;
        }
        src += srcStride;
        dst += dstStride;
    }
}

}  // namespace

// static
std::unique_ptr<C2VEAFormatConverter> C2VEAFormatConverter::Create(
        media::VideoPixelFormat outFormat, const media::Size& visibleSize, uint32_t inputCount,
        const media::Size& codedSize) {
    if (outFormat != media::VideoPixelFormat::PIXEL_FORMAT_I420 &&
        outFormat != media::VideoPixelFormat::PIXEL_FORMAT_NV12) {
        ALOGE("Unsupported output format: %d", static_cast<int32_t>(outFormat));
        return nullptr;
    }

    std::unique_ptr<C2VEAFormatConverter> converter(new C2VEAFormatConverter);
    if (converter->initialize(outFormat, visibleSize, inputCount, codedSize) != C2_OK) {
        ALOGE("Failed to initialize C2VEAFormatConverter");
        return nullptr;
    }
    return converter;
}

c2_status_t C2VEAFormatConverter::initialize(media::VideoPixelFormat outFormat,
                                             const media::Size& visibleSize, uint32_t inputCount,
                                             const media::Size& codedSize) {
    ALOGV("initialize(out_format=%s, visible_size=%dx%d, input_count=%u, coded_size=%dx%d)",
          media::VideoPixelFormatToString(outFormat).c_str(), visibleSize.width(),
          visibleSize.height(), inputCount, codedSize.width(), codedSize.height());

    std::shared_ptr<C2BlockPool> pool;
    c2_status_t status = GetCodec2BlockPool(C2BlockPool::BASIC_GRAPHIC, nullptr, &pool);
    if (status != C2_OK) {
        ALOGE("Failed to get basic graphic block pool (err=%d)", status);
        return status;
    }

    HalPixelFormat halFormat;
    if (outFormat == media::VideoPixelFormat::PIXEL_FORMAT_I420) {
        // Android HAL format doesn't have I420, we use YV12 instead and swap U and V data while
        // conversion to perform I420.
        halFormat = HalPixelFormat::YV12;
    } else {
        halFormat = HalPixelFormat::YCbCr_420_888;  // will allocate NV12 by minigbm.
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
        mGraphicBlocks.emplace_back(new BlockEntry(std::move(block)));
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

C2ConstGraphicBlock C2VEAFormatConverter::convertBlock(uint64_t frameIndex,
                                                       const C2ConstGraphicBlock& inputBlock,
                                                       c2_status_t* status) {
    if (!isReady()) {
        ALOGV("There is no available block for conversion");
        *status = C2_NO_MEMORY;
        return inputBlock;  // This is actually redundant and should not be used.
    }

    BlockEntry* entry = mAvailableQueue.front();
    mAvailableQueue.pop();
    std::shared_ptr<C2GraphicBlock> outputBlock = entry->mBlock;

    const C2GraphicView& inputView = inputBlock.map().get();
    C2PlanarLayout inputLayout = inputView.layout();

    C2GraphicView outputView = outputBlock->map().get();
    C2PlanarLayout outputLayout = outputView.layout();
    uint8_t* dstY = outputView.data()[C2PlanarLayout::PLANE_Y];
    uint8_t* dstU = outputView.data()[C2PlanarLayout::PLANE_V];   // only for I420
    uint8_t* dstV = outputView.data()[C2PlanarLayout::PLANE_U];   // only for I420
    uint8_t* dstUV = outputView.data()[C2PlanarLayout::PLANE_U];  // only for NV12
    const int dstStrideY = outputLayout.planes[C2PlanarLayout::PLANE_Y].rowInc;
    const int dstStrideU = outputLayout.planes[C2PlanarLayout::PLANE_V].rowInc;   // only for I420
    const int dstStrideV = outputLayout.planes[C2PlanarLayout::PLANE_U].rowInc;   // only for I420
    const int dstStrideUV = outputLayout.planes[C2PlanarLayout::PLANE_U].rowInc;  // only for NV12

    media::VideoPixelFormat inputFormat = media::VideoPixelFormat::PIXEL_FORMAT_UNKNOWN;
    if (inputLayout.type == C2PlanarLayout::TYPE_YUV) {
        const uint8_t* srcY = inputView.data()[C2PlanarLayout::PLANE_Y];
        const uint8_t* srcU = inputView.data()[C2PlanarLayout::PLANE_U];
        const uint8_t* srcV = inputView.data()[C2PlanarLayout::PLANE_V];
        const int srcStrideY = inputLayout.planes[C2PlanarLayout::PLANE_Y].rowInc;
        const int srcStrideU = inputLayout.planes[C2PlanarLayout::PLANE_U].rowInc;
        const int srcStrideV = inputLayout.planes[C2PlanarLayout::PLANE_V].rowInc;
        if (inputLayout.rootPlanes == 3) {
            inputFormat = media::VideoPixelFormat::PIXEL_FORMAT_YV12;
            if (mOutFormat == media::VideoPixelFormat::PIXEL_FORMAT_I420) {
                libyuv::I420Copy(srcY, srcStrideY, srcU, srcStrideU, srcV, srcStrideV, dstY,
                                 dstStrideY, dstU, dstStrideU, dstV, dstStrideV,
                                 mVisibleSize.width(), mVisibleSize.height());
            } else {  // media::VideoPixelFormat::PIXEL_FORMAT_NV12
                libyuv::I420ToNV12(srcY, srcStrideY, srcU, srcStrideU, srcV, srcStrideV, dstY,
                                   dstStrideY, dstUV, dstStrideUV, mVisibleSize.width(),
                                   mVisibleSize.height());
            }
        } else if (inputLayout.rootPlanes == 2) {
            if (srcV > srcU) {
                inputFormat = media::VideoPixelFormat::PIXEL_FORMAT_NV12;
                if (mOutFormat == media::VideoPixelFormat::PIXEL_FORMAT_I420) {
                    libyuv::NV12ToI420(srcY, srcStrideY, srcU, srcStrideU, dstY, dstStrideY, dstU,
                                       dstStrideU, dstV, dstStrideV, mVisibleSize.width(),
                                       mVisibleSize.height());
                } else {  // media::VideoPixelFormat::PIXEL_FORMAT_NV12
                    // TODO(johnylin): remove this copy in the future for zero-copy, we would use a
                    //                 specified c2_status_t to indicate caller there is no need to
                    //                 convert. Moreover, we need to manage returnBlock() wisely.
                    libyuv::CopyPlane(srcY, srcStrideY, dstY, dstStrideY, mVisibleSize.width(),
                                      mVisibleSize.height());
                    libyuv::CopyPlane(srcU, srcStrideU, dstUV, dstStrideUV, mVisibleSize.width(),
                                      mVisibleSize.height() / 2);
                }
            } else {
                inputFormat = media::VideoPixelFormat::PIXEL_FORMAT_NV21;
                if (mOutFormat == media::VideoPixelFormat::PIXEL_FORMAT_I420) {
                    libyuv::NV21ToI420(srcY, srcStrideY, srcV, srcStrideV, dstY, dstStrideY, dstU,
                                       dstStrideU, dstV, dstStrideV, mVisibleSize.width(),
                                       mVisibleSize.height());
                } else {  // media::VideoPixelFormat::PIXEL_FORMAT_NV12
                    libyuv::CopyPlane(srcY, srcStrideY, dstY, dstStrideY, mVisibleSize.width(),
                                      mVisibleSize.height());
                    copyPlaneByPixel(srcU, srcStrideU, 2, dstUV, dstStrideUV, 2,
                                     mVisibleSize.width() / 2, mVisibleSize.height() / 2);
                    copyPlaneByPixel(srcV, srcStrideV, 2, dstUV + 1, dstStrideUV, 2,
                                     mVisibleSize.width() / 2, mVisibleSize.height() / 2);
                }
            }
        }
    } else if (inputLayout.type == C2PlanarLayout::TYPE_RGB) {
        // There is only RGBA_8888 specified in C2AllocationGralloc::map(), no BGRA_8888. Maybe
        // BGRA_8888 is not used now?
        inputFormat = media::VideoPixelFormat::PIXEL_FORMAT_ABGR;
        const uint8_t* srcRGB = inputView.data()[C2PlanarLayout::PLANE_R];
        const int srcStrideRGB = inputLayout.planes[C2PlanarLayout::PLANE_R].rowInc;
        if (mOutFormat == media::VideoPixelFormat::PIXEL_FORMAT_I420) {
            libyuv::ABGRToI420(srcRGB, srcStrideRGB, dstY, dstStrideY, dstU, dstStrideU, dstV,
                               dstStrideV, mVisibleSize.width(), mVisibleSize.height());
        } else {  // media::VideoPixelFormat::PIXEL_FORMAT_NV12
            // There is no libyuv function to convert ABGR to NV12. Therefore, we first convert to
            // I420 on dst-Y plane and temporary U/V plane. Then we copy U and V pixels from
            // temporary planes to dst-UV interleavedly.
            const int tempStride = mVisibleSize.width() / 2;
            libyuv::ABGRToI420(srcRGB, srcStrideRGB, dstY, dstStrideY, mTempPlaneU.get(),
                               tempStride, mTempPlaneV.get(), tempStride, mVisibleSize.width(),
                               mVisibleSize.height());
            libyuv::MergeUVPlane(mTempPlaneU.get(), tempStride, mTempPlaneV.get(), tempStride,
                                 dstUV, dstStrideUV, mVisibleSize.width() / 2,
                                 mVisibleSize.height() / 2);
        }
    }

    if (inputFormat == media::VideoPixelFormat::PIXEL_FORMAT_UNKNOWN) {
        ALOGE("Failed to parse input pixel format");
        *status = C2_CORRUPTED;
        return inputBlock;  // This is actually redundant and should not be used.
    }

    ALOGV("convertBlock(frame_index=%" PRIu64 ", format=%s)", frameIndex,
          media::VideoPixelFormatToString(inputFormat).c_str());
    entry->mConvertedFrameIndex = frameIndex;
    *status = C2_OK;
    return outputBlock->share(C2Rect(mVisibleSize.width(), mVisibleSize.height()), C2Fence());
}

c2_status_t C2VEAFormatConverter::returnBlock(uint64_t frameIndex) {
    ALOGV("returnBlock(frame_index=%" PRIu64 ")", frameIndex);

    auto iter = std::find_if(
            mGraphicBlocks.begin(), mGraphicBlocks.end(),
            [frameIndex](const std::unique_ptr<BlockEntry>& be) {
                    return be->mConvertedFrameIndex == frameIndex; });
    if (iter == mGraphicBlocks.end()) {
        ALOGE("Failed to find graphic block by converted frame index: %" PRIu64 "", frameIndex);
        return C2_BAD_INDEX;
    }

    (*iter)->mConvertedFrameIndex = kNoFrameConverted;
    mAvailableQueue.push(iter->get());
    return C2_OK;
}

}  // namespace android
