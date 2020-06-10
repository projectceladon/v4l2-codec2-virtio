// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VEAComponent"

#ifdef V4L2_CODEC2_ARC
#include <C2VEAAdaptorProxy.h>
#endif

#include <C2AllocatorGralloc.h>
#include <C2ArcSupport.h>  // to getParamReflector from arc store
#include <C2ComponentFactory.h>
#include <C2PlatformSupport.h>
#include <C2VEAComponent.h>
#include <C2VEAFormatConverter.h>
#include <SimpleC2Interface.h>
#include <base/bind.h>
#include <base/files/scoped_file.h>
#include <cutils/native_handle.h>
#include <inttypes.h>
#include <system/graphics.h>
#include <ui/GraphicBuffer.h>
#include <utils/Log.h>

#include <algorithm>

#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

namespace android {

namespace {

// The default pixel format of input frames.
const media::VideoPixelFormat kInputPixelFormat = media::VideoPixelFormat::PIXEL_FORMAT_NV12;

// Codec2.0 VEA-based H264 encoder name.
const C2String kH264EncoderName = "c2.vea.avc.encoder";

c2_status_t adaptorResultToC2Status(android::VideoEncodeAcceleratorAdaptor::Result result) {
    switch (result) {
    case android::VideoEncodeAcceleratorAdaptor::Result::SUCCESS:
        return C2_OK;
    case android::VideoEncodeAcceleratorAdaptor::Result::ILLEGAL_STATE:
        ALOGE("Got error: ILLEGAL_STATE");
        return C2_BAD_STATE;
    case android::VideoEncodeAcceleratorAdaptor::Result::INVALID_ARGUMENT:
        ALOGE("Got error: INVALID_ARGUMENT");
        return C2_BAD_VALUE;
    case android::VideoEncodeAcceleratorAdaptor::Result::PLATFORM_FAILURE:
        ALOGE("Got error: PLATFORM_FAILURE");
        return C2_CORRUPTED;
    default:
        ALOGE("Unrecognizable adaptor result (value = %d)...", result);
        return C2_CORRUPTED;
    }
}

media::VideoCodecProfile c2ProfileToVideoCodecProfile(C2Config::profile_t profile) {
    switch (profile) {
    case C2Config::PROFILE_AVC_BASELINE:
        return media::VideoCodecProfile::H264PROFILE_BASELINE;
    case C2Config::PROFILE_AVC_MAIN:
        return media::VideoCodecProfile::H264PROFILE_MAIN;
    case C2Config::PROFILE_AVC_EXTENDED:
        return media::VideoCodecProfile::H264PROFILE_EXTENDED;
    case C2Config::PROFILE_AVC_HIGH:
        return media::VideoCodecProfile::H264PROFILE_HIGH;
    case C2Config::PROFILE_AVC_HIGH_10:
        return media::VideoCodecProfile::H264PROFILE_HIGH10PROFILE;
    case C2Config::PROFILE_AVC_HIGH_422:
        return media::VideoCodecProfile::H264PROFILE_HIGH422PROFILE;
    case C2Config::PROFILE_AVC_HIGH_444_PREDICTIVE:
        return media::VideoCodecProfile::H264PROFILE_HIGH444PREDICTIVEPROFILE;
    case C2Config::PROFILE_AVC_SCALABLE_BASELINE:
        return media::VideoCodecProfile::H264PROFILE_SCALABLEBASELINE;
    case C2Config::PROFILE_AVC_SCALABLE_HIGH:
        return media::VideoCodecProfile::H264PROFILE_SCALABLEHIGH;
    case C2Config::PROFILE_AVC_STEREO_HIGH:
        return media::VideoCodecProfile::H264PROFILE_STEREOHIGH;
    case C2Config::PROFILE_AVC_MULTIVIEW_HIGH:
        return media::VideoCodecProfile::H264PROFILE_MULTIVIEWHIGH;
    default:
        ALOGE("Unrecognizable C2 profile (value = 0x%x)...", profile);
        return media::VideoCodecProfile::VIDEO_CODEC_PROFILE_UNKNOWN;
    }
}

uint8_t c2LevelToLevelIDC(C2Config::level_t level) {
    switch (level) {
    case C2Config::LEVEL_AVC_1:
        return 10;
    case C2Config::LEVEL_AVC_1B:
        return 9;
    case C2Config::LEVEL_AVC_1_1:
        return 11;
    case C2Config::LEVEL_AVC_1_2:
        return 12;
    case C2Config::LEVEL_AVC_1_3:
        return 13;
    case C2Config::LEVEL_AVC_2:
        return 20;
    case C2Config::LEVEL_AVC_2_1:
        return 21;
    case C2Config::LEVEL_AVC_2_2:
        return 22;
    case C2Config::LEVEL_AVC_3:
        return 30;
    case C2Config::LEVEL_AVC_3_1:
        return 31;
    case C2Config::LEVEL_AVC_3_2:
        return 32;
    case C2Config::LEVEL_AVC_4:
        return 40;
    case C2Config::LEVEL_AVC_4_1:
        return 41;
    case C2Config::LEVEL_AVC_4_2:
        return 42;
    case C2Config::LEVEL_AVC_5:
        return 50;
    case C2Config::LEVEL_AVC_5_1:
        return 51;
    case C2Config::LEVEL_AVC_5_2:
        return 52;
    default:
        ALOGE("Unrecognizable C2 level (value = 0x%x)...", level);
        return 0;
    }
}

// Get android_ycbcr by lockYCbCr() from block handle which uses usage without SW_READ/WRITE bits.
android_ycbcr getGraphicBlockInfo(const C2ConstGraphicBlock& block) {
    uint32_t width, height, format, stride, igbp_slot, generation;
    uint64_t usage, igbp_id;
    android::_UnwrapNativeCodec2GrallocMetadata(block.handle(), &width, &height, &format, &usage,
                                                &stride, &generation, &igbp_id, &igbp_slot);
    native_handle_t* grallocHandle = android::UnwrapNativeCodec2GrallocHandle(block.handle());
    sp<GraphicBuffer> buf = new GraphicBuffer(grallocHandle, GraphicBuffer::CLONE_HANDLE, width,
                                              height, format, 1, usage, stride);
    native_handle_delete(grallocHandle);

    android_ycbcr ycbcr = {};
    constexpr uint32_t kNonSWLockUsage = 0;
    int32_t status = buf->lockYCbCr(kNonSWLockUsage, &ycbcr);
    if (status != OK) ALOGE("lockYCbCr is failed: %d", (int)status);
    buf->unlock();
    return ycbcr;
}

// Helper class to parse H264 NAL units from data.
class NalParser {
public:
    NalParser(const uint8_t* data, size_t length) : mCurrNalDataPos(data), mDataEnd(data + length) {
        mNextNalStartCodePos = findNextStartCodePos();
    }

    // Locates the next NAL after |mNextNalStartCodePos|. If there is one, updates |mCurrNalDataPos|
    // to the first byte of the NAL data (start code is not included), and |mNextNalStartCodePos| to
    // the position of the next start code, and returns true.
    // If there is no more NAL, returns false.
    //
    // Note: This method must be called prior to data() and length().
    bool locateNextNal() {
        if (mNextNalStartCodePos == mDataEnd) return false;
        mCurrNalDataPos = mNextNalStartCodePos + kNalStartCodeLength;  // skip start code.
        mNextNalStartCodePos = findNextStartCodePos();
        return true;
    }

    // Gets current NAL data (start code is not included).
    const uint8_t* data() const { return mCurrNalDataPos; }

    // Gets the byte length of current NAL data (start code is not included).
    size_t length() {
        if (mNextNalStartCodePos == mDataEnd) return mDataEnd - mCurrNalDataPos;
        size_t length = mNextNalStartCodePos - mCurrNalDataPos;
        // The start code could be 3 or 4 bytes, i.e., 0x000001 or 0x00000001.
        return *(mNextNalStartCodePos - 1) == 0x00 ? length - 1 : length;
    }

private:
    // The byte pattern for the start of a H264 NAL unit.
    const uint8_t kNalStartCode[3] = {0x00, 0x00, 0x01};
    // The length in bytes of the NAL-unit start pattern.
    const size_t kNalStartCodeLength = 3;

    const uint8_t* mCurrNalDataPos;
    const uint8_t* mDataEnd;
    const uint8_t* mNextNalStartCodePos;

    const uint8_t* findNextStartCodePos() const {
        return std::search(mCurrNalDataPos, mDataEnd, kNalStartCode,
                           kNalStartCode + kNalStartCodeLength);
    }
};

}  // namespace

C2VEAComponent::IntfImpl::IntfImpl(C2String name, const std::shared_ptr<C2ReflectorHelper>& helper)
      : C2EncoderInterface(helper) {
    std::unique_ptr<VideoEncodeAcceleratorAdaptor> adaptor;
#ifdef V4L2_CODEC2_ARC
    adaptor = std::make_unique<arc::C2VEAAdaptorProxy>();
#endif
    if (!adaptor) {
        mInitStatus = C2_BAD_VALUE;
        return;
    }

    // Query supported profiles in the beginning. Currently only profiles and max resolution are
    // taken into account.
    // TODO(johnylin): regard all other supported values from adaptor.
    std::vector<VideoEncodeProfile> supportedProfiles;
    VideoEncodeAcceleratorAdaptor::Result result =
            adaptor->getSupportedProfiles(&supportedProfiles);
    if (result != VideoEncodeAcceleratorAdaptor::Result::SUCCESS) {
        ALOGE("Failed to get supported profiles from adaptor...");
        mInitStatus = adaptorResultToC2Status(result);
        return;
    }

    Initialize(name, supportedProfiles);
}

base::Optional<media::VideoCodec> C2VEAComponent::IntfImpl::getCodecFromComponentName(
        const std::string& name) const {
    if (name == kH264EncoderName) return media::VideoCodec::kCodecH264;

    ALOGE("Unknown name: %s", name.c_str());
    return base::nullopt;
}

#define RETURN_ON_UNINITIALIZED_OR_ERROR()                    \
    do {                                                      \
        if (mComponentState == ComponentState::ERROR ||       \
            mComponentState == ComponentState::UNINITIALIZED) \
            return;                                           \
    } while (0)

C2VEAComponent::C2VEAComponent(C2String name, c2_node_id_t id,
                               const std::shared_ptr<C2ReflectorHelper>& helper)
      : mThread("C2VEAComponentThread"),
        mVEAInitResult(VideoEncodeAcceleratorAdaptor::Result::ILLEGAL_STATE),
        mComponentState(ComponentState::UNINITIALIZED),
        mState(State::UNLOADED),
        mWeakThisFactory(this) {
    mIntfImpl = std::make_shared<IntfImpl>(name, helper);
    mIntf = std::make_shared<SimpleInterface<IntfImpl>>(name.c_str(), id, mIntfImpl);

    // TODO(johnylin): the client may need to know if init is failed.
    if (mIntfImpl->status() != C2_OK) {
        ALOGE("Component interface init failed (err code = %d)", mIntfImpl->status());
        return;
    }

    if (!mThread.Start()) {
        ALOGE("Component thread failed to start.");
        return;
    }

#ifdef V4L2_CODEC2_ARC
    mVEAAdaptor = std::make_unique<arc::C2VEAAdaptorProxy>();
#endif
    mTaskRunner = mThread.task_runner();
    mState.store(State::LOADED);
}

C2VEAComponent::~C2VEAComponent() {
    if (mThread.IsRunning()) {
        mTaskRunner->PostTask(FROM_HERE,
                              ::base::Bind(&C2VEAComponent::onDestroy, ::base::Unretained(this)));
        mThread.Stop();
    }
}

void C2VEAComponent::onDestroy() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDestroy");
    mVEAAdaptor.reset(nullptr);
}

c2_status_t C2VEAComponent::setListener_vb(const std::shared_ptr<Listener>& listener,
                                           c2_blocking_t mayBlock) {
    UNUSED(mayBlock);
    // TODO(johnylin): API says this method must be supported in all states, however I'm quite not
    //                 sure what is the use case.
    if (mState.load() != State::LOADED) {
        return C2_BAD_STATE;
    }
    mListener = listener;
    return C2_OK;
}

c2_status_t C2VEAComponent::queue_nb(std::list<std::unique_ptr<C2Work>>* const items) {
    if (mState.load() != State::RUNNING) {
        return C2_BAD_STATE;
    }
    while (!items->empty()) {
        mTaskRunner->PostTask(
                FROM_HERE, ::base::BindOnce(&C2VEAComponent::onQueueWork, ::base::Unretained(this),
                                            std::move(items->front())));
        items->pop_front();
    }
    return C2_OK;
}

void C2VEAComponent::onQueueWork(std::unique_ptr<C2Work> work) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onQueueWork: flags=0x%x, index=%llu, timestamp=%llu", work->input.flags,
          work->input.ordinal.frameIndex.peekull(), work->input.ordinal.timestamp.peekull());
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    uint32_t drainMode = NO_DRAIN;
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        drainMode = DRAIN_COMPONENT_WITH_EOS;
    }
    mQueue.push({std::move(work), drainMode});

    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VEAComponent::onDequeueWork, ::base::Unretained(this)));
}

void C2VEAComponent::onDequeueWork() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDequeueWork");
    RETURN_ON_UNINITIALIZED_OR_ERROR();
    if (mQueue.empty()) {
        return;
    }
    if (mComponentState == ComponentState::DRAINING) {
        ALOGV("Temporarily stop dequeueing works since component is draining.");
        return;
    }
    if (mComponentState == ComponentState::CONFIGURED) {
        ALOGV("Component is still waiting for onRequireBitstreamBuffers() callback");
        return;
    }

    if (!mQueue.front().mWork->input.buffers.empty() && mFormatConverter &&
        !mFormatConverter->isReady()) {
        ALOGV("There is no available block for conversion currently in format converter");
        return;
    }

    // Update dynamic parameters.
    if (updateEncodingParametersIfChanged()) {
        mVEAAdaptor->requestEncodingParametersChange(mRequestedBitrate, mRequestedFrameRate);
    }

    // Check sync frame request (force_keyframe) from client.
    C2StreamRequestSyncFrameTuning::output requestKeyFrame;
    c2_status_t status = mIntfImpl->query({&requestKeyFrame}, {}, C2_DONT_BLOCK, nullptr);
    if (status != C2_OK) {
        ALOGE("Failed to query request_sync_frame from intf, error: %d", status);
        reportError(status);
        return;
    }

    if (requestKeyFrame.value == C2_TRUE) {
        // Sync keyframe immediately by resetting mKeyFrameSerial.
        mKeyFrameSerial = 0;
        // Unset request.
        requestKeyFrame.value = C2_FALSE;
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        status = mIntfImpl->config({&requestKeyFrame}, C2_MAY_BLOCK, &failures);
        if (status != C2_OK) {
            ALOGE("Failed to config request_sync_frame to intf, error: %d", status);
            reportError(status);
            return;
        }
    }

    // Dequeue a work from mQueue.
    std::unique_ptr<C2Work> work(std::move(mQueue.front().mWork));
    auto drainMode = mQueue.front().mDrainMode;
    mQueue.pop();

    CHECK_LE(work->input.buffers.size(), 1u);
    CHECK_EQ(work->worklets.size(), 1u);

    // Set the default values for the output worklet.
    work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(0);
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;

    uint64_t index = work->input.ordinal.frameIndex.peeku();
    int64_t timestamp = static_cast<int64_t>(work->input.ordinal.timestamp.peeku());
    if (work->input.buffers.empty()) {
        // Emplace a nullptr to unify the check for work done.
        ALOGV("Got a work with no input buffer! Emplace a nullptr inside.");
        work->input.buffers.emplace_back(nullptr);
        if (drainMode == NO_DRAIN) {
            // WORKAROUND from CCodecBufferChannel:
            // A work with no input buffer will be queued for obtaining CSD info because some apps
            // expect CSD available without queueing any input. This is not supported by VEA, we
            // just simply return this work.
            reportWork(std::move(work));
            return;
        }
    } else {
        // If input.buffers is not empty, the buffer should have meaningful content inside.
        C2ConstGraphicBlock inputBlock =
                work->input.buffers.front()->data().graphicBlocks().front();
        bool force_keyframe = (mKeyFrameSerial++ % mKeyFramePeriod) == 0;

        if (mFormatConverter) {
            status = C2_CORRUPTED;
            C2ConstGraphicBlock convertedBlock =
                    mFormatConverter->convertBlock(index, inputBlock, &status);
            if (status != C2_OK) {
                reportError(status);
                return;
            }
            // Send format-converted input buffer to VEA for encode. |convertedBlock| will be the
            // same as |inputBlock| if zero-copy is applied.
            sendInputBufferToAccelerator(convertedBlock, index, timestamp, force_keyframe);
        } else {
            // Send input buffer to VEA for encode.
            sendInputBufferToAccelerator(inputBlock, index, timestamp, force_keyframe);
        }

        if (!mOutputBlockPool) {
            // Get block pool of block pool ID configured from the client.
            C2BlockPool::local_id_t poolId = mIntfImpl->getBlockPoolId();
            ALOGI("Using C2BlockPool ID = %" PRIu64 " for allocating output buffers", poolId);
            status = GetCodec2BlockPool(poolId, shared_from_this(), &mOutputBlockPool);
            if (status != C2_OK || !mOutputBlockPool) {
                ALOGE("Failed to get output block pool, error: %d", status);
                reportError(status);
                return;
            }
        }

        // Allocate a linear buffer from block pool and import to VEA via useBitstreamBuffer call.
        std::shared_ptr<C2LinearBlock> outputBlock;
        status = mOutputBlockPool->fetchLinearBlock(
                mOutputBufferSize, {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE},
                &outputBlock);
        if (status != C2_OK) {
            ALOGE("Failed to fetch linear block, error: %d", status);
            reportError(status);
            return;
        }

        ::base::ScopedFD dupFd(dup(outputBlock->handle()->data[0]));
        if (!dupFd.is_valid()) {
            ALOGE("Failed to dup(%d) output buffer (index=%" PRIu64 "), errno=%d",
                  outputBlock->handle()->data[0], index, errno);
            reportError(C2_CORRUPTED);
            return;
        }

        // Note that |bufferIndex| has different meaning than |index|. It is just an identification
        // for the output block which will be used on BitstreamBufferReady callback. We leverage the
        // value from |index| because |index| is guaranteed to be unique.
        uint64_t bufferIndex = index;
        mVEAAdaptor->useBitstreamBuffer(bufferIndex, std::move(dupFd), outputBlock->offset(),
                                        outputBlock->size());
        if (mOutputBlockMap.find(bufferIndex) != mOutputBlockMap.end()) {
            ALOGE("Buffer index: %" PRIu64 " already exists in output block map", bufferIndex);
            reportError(C2_CORRUPTED);
            return;
        }
        mOutputBlockMap[bufferIndex] = std::move(outputBlock);
    }

    if (drainMode != NO_DRAIN) {
        mVEAAdaptor->flush();
        mComponentState = ComponentState::DRAINING;
        mPendingOutputEOS = drainMode == DRAIN_COMPONENT_WITH_EOS;
    }

    // Put work to mPendingWorks.
    mPendingWorks.emplace_back(std::move(work));

    if (!mQueue.empty()) {
        mTaskRunner->PostTask(
                FROM_HERE, ::base::Bind(&C2VEAComponent::onDequeueWork, ::base::Unretained(this)));
    }
}

void C2VEAComponent::sendInputBufferToAccelerator(const C2ConstGraphicBlock& inputBlock,
                                                  uint64_t index, int64_t timestamp,
                                                  bool keyframe) {
    ALOGV("sendInputBufferToAccelerator: blockSize:%dx%d, index=%" PRIu64 ", ts=%" PRId64
          ", keyframe=%d",
          inputBlock.width(), inputBlock.height(), index, timestamp, keyframe);

    // TODO(johnylin): find the way not to map input block every time for acquiring pixel format.
    C2PlanarLayout layout;
    {
        const C2GraphicView& view = inputBlock.map().get();
        layout = view.layout();
        // Release |view| to unmap |inputBlock| here, then we could perform lockYCbCr (or lock)
        // later to get offset and stride information.
    }

    // The above layout() cannot fill layout information and memset 0 instead if the input format is
    // IMPLEMENTATION_DEFINED and its backed format is RGB. We fill the layout by using
    // ImplDefinedToRGBXMap in the case.
    if (static_cast<uint32_t>(layout.type) == 0u) {
        std::unique_ptr<ImplDefinedToRGBXMap> idMap = ImplDefinedToRGBXMap::Create(inputBlock);
        if (idMap == nullptr) {
            ALOGE("Unable to parse RGBX_8888 from IMPLEMENTATION_DEFINED");
            reportError(C2_CORRUPTED);
            return;
        }
        layout.type = C2PlanarLayout::TYPE_RGB;
        // These parameters would be used in TYPE_GRB case below.
        layout.numPlanes = 3;   // same value as in C2AllocationGralloc::map()
        layout.rootPlanes = 1;  // same value as in C2AllocationGralloc::map()
        layout.planes[C2PlanarLayout::PLANE_R].offset = idMap->offset();
        layout.planes[C2PlanarLayout::PLANE_R].rowInc = idMap->rowInc();
    }

    std::vector<uint32_t> offsets(layout.numPlanes, 0u);
    std::vector<uint32_t> strides(layout.numPlanes, 0u);
    media::VideoPixelFormat format = media::VideoPixelFormat::PIXEL_FORMAT_UNKNOWN;
    if (layout.type == C2PlanarLayout::TYPE_YUV) {
        // lockYCbCr() stores offsets into the pointers if given usage does not contain
        // SW_READ/WRITE bits.
        auto ycbcr = getGraphicBlockInfo(inputBlock);
        offsets[C2PlanarLayout::PLANE_Y] =
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ycbcr.y));
        offsets[C2PlanarLayout::PLANE_U] =
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ycbcr.cb));
        offsets[C2PlanarLayout::PLANE_V] =
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ycbcr.cr));
        strides[C2PlanarLayout::PLANE_Y] = static_cast<uint32_t>(ycbcr.ystride);
        strides[C2PlanarLayout::PLANE_U] = static_cast<uint32_t>(ycbcr.cstride);
        strides[C2PlanarLayout::PLANE_V] = static_cast<uint32_t>(ycbcr.cstride);

        bool crcb = false;
        if (offsets[C2PlanarLayout::PLANE_U] > offsets[C2PlanarLayout::PLANE_V]) {
            std::swap(offsets[C2PlanarLayout::PLANE_U], offsets[C2PlanarLayout::PLANE_V]);
            crcb = true;
        }

        bool semiplanar = false;
        if (ycbcr.chroma_step >
            offsets[C2PlanarLayout::PLANE_V] - offsets[C2PlanarLayout::PLANE_U]) {
            semiplanar = true;
        }

        if (!crcb && !semiplanar) {
            format = media::VideoPixelFormat::PIXEL_FORMAT_I420;
        } else if (!crcb && semiplanar) {
            format = media::VideoPixelFormat::PIXEL_FORMAT_NV12;
        } else if (crcb && !semiplanar) {
            // HACK: pretend YV12 is I420 now since VEA only accepts I420. (YV12 will be used
            //       for input byte-buffer mode).
            // TODO(johnylin): revisit this after VEA finishes format conversion.
            //format = media::VideoPixelFormat::PIXEL_FORMAT_YV12;
            format = media::VideoPixelFormat::PIXEL_FORMAT_I420;
        } else {
            format = media::VideoPixelFormat::PIXEL_FORMAT_NV21;
        }
    } else if (layout.type == C2PlanarLayout::TYPE_RGB) {
        offsets[C2PlanarLayout::PLANE_R] = layout.planes[C2PlanarLayout::PLANE_R].offset;
        strides[C2PlanarLayout::PLANE_R] =
                static_cast<uint32_t>(layout.planes[C2PlanarLayout::PLANE_R].rowInc);
        // TODO(johnylin): is PIXEL_FORMAT_ABGR valid?
        format = media::VideoPixelFormat::PIXEL_FORMAT_ARGB;
    }

    if (format == media::VideoPixelFormat::PIXEL_FORMAT_UNKNOWN) {
        ALOGE("Failed to parse input pixel format.");
        reportError(C2_CORRUPTED);
        return;
    }

    if (keyframe) {
        // Print format logs only for keyframes in order to avoid excessive verbosity.
        for (uint32_t i = 0; i < layout.rootPlanes; ++i) {
            ALOGV("plane %u: stride: %d, offset: %u", i, strides[i], offsets[i]);
        }
        ALOGV("HAL pixel format: %s", media::VideoPixelFormatToString(format).c_str());
    }

    std::vector<VideoFramePlane> passedPlanes;
    for (uint32_t i = 0; i < layout.rootPlanes; ++i) {
        passedPlanes.push_back({offsets[i], strides[i]});
    }

    ::base::ScopedFD dupFd(dup(inputBlock.handle()->data[0]));
    if (!dupFd.is_valid()) {
        ALOGE("Failed to dup(%d) input buffer (index=%" PRIu64 "), errno=%d",
              inputBlock.handle()->data[0], index, errno);
        reportError(C2_CORRUPTED);
        return;
    }

    mVEAAdaptor->encode(index, std::move(dupFd), format, std::move(passedPlanes), timestamp,
                        keyframe);
}

bool C2VEAComponent::isFlushedState() const {
    // There are two situations for encoder to perform flush:
    // 1. Flush by stop: while stop() is called, all pending works should be flushed and VEAAdaptor
    //                   should be released. After onStop() is finished, the component state will
    //                   be UNINITIALIZED until next start() call.
    // 2. Flush by flush: while flush() is called, all pending works should be flushed. VEAAdaptor
    //                    should be re-created and re-initialized, which means the component state
    //                    will be CONFIGURED until RequireBitstreamBuffers callback.
    return mComponentState == ComponentState::UNINITIALIZED ||
           mComponentState == ComponentState::CONFIGURED;
}

void C2VEAComponent::onInputBufferDone(uint64_t index) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onInputBufferDone: index=%" PRIu64 "", index);
    if (mComponentState == ComponentState::ERROR) {
        return;
    }
    if (isFlushedState()) {
        ALOGV("Work is already flushed, just neglect this input.");
        return;
    }

    C2Work* work = getPendingWorkByIndex(index);
    if (!work) {
        reportError(C2_CORRUPTED);
        return;
    }

    // When the work is done, the input buffer shall be reset by component.
    work->input.buffers.front().reset();

    reportWorkIfFinished(index);

    if (!mFormatConverter) {
        return;
    }

    bool previouslyOutOfBlock = !mFormatConverter->isReady();
    c2_status_t status = mFormatConverter->returnBlock(index);
    if (status != C2_OK) {
        reportError(status);
        return;
    }

    // Work dequeueing was temporarily blocked due to no available block for conversion in
    // |mFormatConverter| until this function is called (one will be returned). Restart to dequeue
    // work if there is still work queued.
    if (previouslyOutOfBlock && !mQueue.empty()) {
        mTaskRunner->PostTask(
                FROM_HERE, ::base::Bind(&C2VEAComponent::onDequeueWork, ::base::Unretained(this)));
    }
}

void C2VEAComponent::onOutputBufferDone(uint64_t index, uint32_t payloadSize, bool keyFrame,
                                        int64_t timestamp) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onOutputBufferDone: index=%" PRIu64 ", payload=%u, key_frame=%d, timestamp=%" PRId64 "",
          index, payloadSize, keyFrame, timestamp);
    if (mComponentState == ComponentState::ERROR) {
        return;
    }
    if (isFlushedState()) {
        ALOGV("Work is already flushed, just neglect this output.");
        return;
    }

    auto blockIter = mOutputBlockMap.find(index);
    if (blockIter == mOutputBlockMap.end()) {
        ALOGE("Cannot find corresponding output block by buffer index: %" PRIu64 "", index);
        reportError(C2_CORRUPTED);
        return;
    }

    C2ConstLinearBlock constBlock =
            blockIter->second->share(blockIter->second->offset(), payloadSize, C2Fence());

    // Get the work with corresponding timestamp of returned output buffer.
    C2Work* work = getPendingWorkByTimestamp(timestamp);
    if (!work) {
        reportError(C2_CORRUPTED);
        return;
    }

    if (!mCSDSubmitted) {
        // Extract CSD info and put into the corresponding work.
        std::unique_ptr<C2StreamInitDataInfo::output> csd;
        C2ReadView view = constBlock.map().get();
        extractCSDInfo(&csd, view.data(), view.capacity());
        if (!csd) {
            reportError(C2_CORRUPTED);
            return;
        }
        work->worklets.front()->output.configUpdate.push_back(std::move(csd));
        mCSDSubmitted = true;
    }

    std::shared_ptr<C2Buffer> buffer = C2Buffer::CreateLinearBuffer(std::move(constBlock));
    if (keyFrame) {
        buffer->setInfo(
                std::make_shared<C2StreamPictureTypeMaskInfo::output>(0u, C2Config::SYNC_FRAME));
    }
    work->worklets.front()->output.buffers.emplace_back(buffer);

    mOutputBlockMap.erase(blockIter);

    reportWorkIfFinished(work->input.ordinal.frameIndex.peeku());
}

std::deque<std::unique_ptr<C2Work>>::iterator C2VEAComponent::findPendingWorkByIndex(
        uint64_t index) {
    return std::find_if(mPendingWorks.begin(), mPendingWorks.end(),
                        [index](const std::unique_ptr<C2Work>& w) {
                            return w->input.ordinal.frameIndex.peeku() == index;
                        });
}

C2Work* C2VEAComponent::getPendingWorkByIndex(uint64_t index) {
    auto workIter = findPendingWorkByIndex(index);
    if (workIter == mPendingWorks.end()) {
        ALOGE("Can't find pending work by index: %" PRIu64 "", index);
        return nullptr;
    }
    return workIter->get();
}

C2Work* C2VEAComponent::getPendingWorkByTimestamp(int64_t timestamp) {
    if (timestamp < 0) {
        ALOGE("Invalid timestamp: %" PRId64 "", timestamp);
        return nullptr;
    }
    auto workIter = std::find_if(mPendingWorks.begin(), mPendingWorks.end(),
                                 [timestamp](const std::unique_ptr<C2Work>& w) {
                                     return !(w->input.flags & C2FrameData::FLAG_END_OF_STREAM) &&
                                            w->input.ordinal.timestamp.peeku() ==
                                                    static_cast<uint64_t>(timestamp);
                                 });
    if (workIter == mPendingWorks.end()) {
        ALOGE("Can't find pending work by timestmap: %" PRId64 "", timestamp);
        return nullptr;
    }
    return workIter->get();
}

void C2VEAComponent::extractCSDInfo(std::unique_ptr<C2StreamInitDataInfo::output>* const csd,
                                    const uint8_t* data, size_t length) {
    constexpr uint8_t kTypeSeqParamSet = 7;
    constexpr uint8_t kTypePicParamSet = 8;

    // Android frameworks needs 4 bytes start code.
    constexpr uint8_t kStartCode[] = {0x00, 0x00, 0x00, 0x01};
    constexpr int kStartCodeLength = 4;

    csd->reset();

    // Temporarily allocate a byte array to copy codec config data. This should be freed after
    // codec config data extraction is done.
    auto tmpConfigData = std::make_unique<uint8_t[]>(length);
    uint8_t* tmpOutput = tmpConfigData.get();
    uint8_t* tmpConfigDataEnd = tmpOutput + length;

    NalParser parser(data, length);
    while (parser.locateNextNal()) {
        if (parser.length() == 0) continue;
        uint8_t nalType = *parser.data() & 0x1f;
        ALOGV("find next NAL: type=%d, length=%zu", nalType, parser.length());
        if (nalType != kTypeSeqParamSet && nalType != kTypePicParamSet) continue;

        if (tmpOutput + kStartCodeLength + parser.length() > tmpConfigDataEnd) {
            ALOGE("Buffer overflow on extracting codec config data (length=%zu)", length);
            return;
        }
        std::memcpy(tmpOutput, kStartCode, kStartCodeLength);
        tmpOutput += kStartCodeLength;
        std::memcpy(tmpOutput, parser.data(), parser.length());
        tmpOutput += parser.length();
    }

    size_t configDataLength = tmpOutput - tmpConfigData.get();
    ALOGV("Extracted codec config data: length=%zu", configDataLength);
    *csd = C2StreamInitDataInfo::output::AllocUnique(configDataLength, 0u);
    std::memcpy((*csd)->m.value, tmpConfigData.get(), configDataLength);
}

c2_status_t C2VEAComponent::announce_nb(const std::vector<C2WorkOutline>& items) {
    UNUSED(items);
    return C2_OMITTED;  // Tunneling is not supported by now
}

c2_status_t C2VEAComponent::flush_sm(flush_mode_t mode,
                                     std::list<std::unique_ptr<C2Work>>* const flushedWork) {
    if (mode != FLUSH_COMPONENT) {
        return C2_OMITTED;  // Tunneling is not supported by now
    }
    if (mState.load() != State::RUNNING) {
        return C2_BAD_STATE;
    }
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VEAComponent::onFlush, ::base::Unretained(this),
                                       true /* reinitAdaptor */));
    // Instead of |flushedWork|, abandoned works will be returned via onWorkDone_nb() callback.
    return C2_OK;
}

void C2VEAComponent::onFlush(bool reinitAdaptor) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onFlush: reinitAdaptor = %d", reinitAdaptor);
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    mVEAAdaptor.reset(nullptr);
    // Pop all works in mQueue and put into mPendingWorks.
    while (!mQueue.empty()) {
        mPendingWorks.emplace_back(std::move(mQueue.front().mWork));
        mQueue.pop();
    }

    reportAbandonedWorks();

    mFormatConverter = nullptr;

    if (reinitAdaptor) {
        VideoEncodeAcceleratorAdaptor::Result result = initializeVEA();
        if (result != VideoEncodeAcceleratorAdaptor::Result::SUCCESS) {
            ALOGE("Failed to re-initialize VEA, init_result = %d", result);
            reportError(adaptorResultToC2Status(result));
        }
    }
}

c2_status_t C2VEAComponent::drain_nb(drain_mode_t mode) {
    if (mode != DRAIN_COMPONENT_WITH_EOS && mode != DRAIN_COMPONENT_NO_EOS) {
        return C2_OMITTED;  // Tunneling is not supported by now
    }
    if (mState.load() != State::RUNNING) {
        return C2_BAD_STATE;
    }
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VEAComponent::onDrain, ::base::Unretained(this),
                                       static_cast<uint32_t>(mode)));
    return C2_OK;
}

void C2VEAComponent::onDrain(uint32_t drainMode) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDrain: mode = %u", drainMode);
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    if (!mQueue.empty()) {
        // Mark last queued work as "drain-till-here" by setting drainMode. Do not change drainMode
        // if last work already has one.
        if (mQueue.back().mDrainMode == NO_DRAIN) {
            mQueue.back().mDrainMode = drainMode;
        }
    } else if (!mPendingWorks.empty()) {
        // Neglect drain request if component is not in STARTED mode. Otherwise, enters DRAINING
        // mode and signal VEA flush immediately.
        if (mComponentState == ComponentState::STARTED) {
            mVEAAdaptor->flush();
            mComponentState = ComponentState::DRAINING;
            mPendingOutputEOS = drainMode == DRAIN_COMPONENT_WITH_EOS;
        } else {
            ALOGV("Neglect drain. Component in state: %d", mComponentState);
        }
    } else {
        // Do nothing.
        ALOGV("No buffers in VEA, drain takes no effect.");
    }
}

void C2VEAComponent::onDrainDone(bool done) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDrainDone");
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    if (!done) {
        ALOGE("VEA flush (draining) is aborted...");
        reportError(C2_CORRUPTED);
        return;
    }

    if (mComponentState == ComponentState::DRAINING) {
        mComponentState = ComponentState::STARTED;
    }

    if (mPendingOutputEOS) {
        // Return EOS work.
        reportEOSWork();
    }

    // Work dequeueing was stopped while component draining. Restart it if there is queued work.
    if (!mQueue.empty()) {
        mTaskRunner->PostTask(
                FROM_HERE, ::base::Bind(&C2VEAComponent::onDequeueWork, ::base::Unretained(this)));
    }
}

c2_status_t C2VEAComponent::start() {
    // Use mStartStopLock to block other asynchronously start/stop calls.
    std::lock_guard<std::mutex> lock(mStartStopLock);

    if (mState.load() != State::LOADED) {
        return C2_BAD_STATE;  // start() is only supported when component is in LOADED state.
    }

    ::base::WaitableEvent done(::base::WaitableEvent::ResetPolicy::AUTOMATIC,
                               ::base::WaitableEvent::InitialState::NOT_SIGNALED);
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VEAComponent::onStart, ::base::Unretained(this), &done));
    done.Wait();
    c2_status_t c2Status = adaptorResultToC2Status(mVEAInitResult);
    if (c2Status != C2_OK) {
        ALOGE("Failed to start component due to VEA error...");
        return c2Status;
    }
    mState.store(State::RUNNING);
    return C2_OK;
}

void C2VEAComponent::onStart(::base::WaitableEvent* done) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onStart");
    CHECK_EQ(mComponentState, ComponentState::UNINITIALIZED);

    mVEAInitResult = initializeVEA();
    if (mVEAInitResult != VideoEncodeAcceleratorAdaptor::Result::SUCCESS) {
        done->Signal();  // Signal now for VEA initialization error.
        return;
    }

    // Event will be signaled after onRequireBitstreamBuffers().
    mStartDoneEvent = done;
}

VideoEncodeAcceleratorAdaptor::Result C2VEAComponent::initializeVEA() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());

    media::Size visibleSize = mIntfImpl->getInputVisibleSize();
    media::VideoCodecProfile profile = c2ProfileToVideoCodecProfile(mIntfImpl->getOutputProfile());
    uint8_t level = c2LevelToLevelIDC(mIntfImpl->getOutputLevel());

    updateEncodingParametersIfChanged();

    VideoEncoderAcceleratorConfig config;
    config.mInputFormat = kInputPixelFormat;
    config.mInputVisibleSize = visibleSize;
    config.mOutputProfile = profile;
    config.mInitialBitrate = mRequestedBitrate;
    config.mInitialFramerate = mRequestedFrameRate;
    config.mH264OutputLevel = level;
    config.mStorageType = VideoEncoderAcceleratorConfig::DMABUF;

    ALOGI("Initialize VEA by config{ format=%d, inputVisibleSize=%dx%d, profile=%d, level=%u, "
          "bitrate=%u, frameRate=%u, storageType=%d }",
          kInputPixelFormat, visibleSize.width(), visibleSize.height(), profile, level,
          mRequestedBitrate, mRequestedFrameRate, config.mStorageType);

    // Re-create mVEAAdaptor if necessary. mVEAAdaptor will be created and established channel by
    // mIntfImpl if this is the first time component starts.
    if (!mVEAAdaptor) {
#ifdef V4L2_CODEC2_ARC
        mVEAAdaptor.reset(new arc::C2VEAAdaptorProxy());
#endif
    }

    VideoEncodeAcceleratorAdaptor::Result result = mVEAAdaptor->initialize(config, this);
    if (result != VideoEncodeAcceleratorAdaptor::Result::SUCCESS) {
        return result;
    }

    mComponentState = ComponentState::CONFIGURED;

    mKeyFramePeriod = mIntfImpl->getKeyFramePeriod();
    ALOGI("Set keyframe period = %u", mKeyFramePeriod);
    mKeyFrameSerial = 0;
    mCSDSubmitted = false;

    return VideoEncodeAcceleratorAdaptor::Result::SUCCESS;
}

void C2VEAComponent::onRequireBitstreamBuffers(uint32_t inputCount,
                                               const media::Size& inputCodedSize,
                                               uint32_t outputBufferSize) {
    // There are two situations for component to execute onRequireBitstreamBuffers():
    // 1. If |mStartDoneEvent|, component is on start procedure. |mStartDoneEvent| has to be
    //    signaled no matter when there is any error.
    // 2. If |mStartDoneEvent| is null, component is recovering VEA after flush.

    DCHECK(mTaskRunner->BelongsToCurrentThread());
    if (!mStartDoneEvent) {
        // VEA may be released (component stopped) or get errors after flush. In such case we don't
        // care about RequireBitstreamBuffers callback anymore.
        RETURN_ON_UNINITIALIZED_OR_ERROR();
    }

    ALOGV("onRequireBitstreamBuffers(inputCount=%u, inputCodedSize=%dx%d, outBufferSize=%u)",
          inputCount, inputCodedSize.width(), inputCodedSize.height(), outputBufferSize);
    CHECK_EQ(mComponentState, ComponentState::CONFIGURED);

    // Check if inputCodedSize is aligned to 2 and not smaller than visible size.
    media::Size visibleSize = mIntfImpl->getInputVisibleSize();
    if ((inputCodedSize.width() & 1) || (inputCodedSize.height() & 1) ||
        (inputCodedSize.width() < visibleSize.width()) ||
        (inputCodedSize.height() < visibleSize.height())) {
        ALOGE("Invalid coded size: %dx%d", inputCodedSize.width(), inputCodedSize.height());
        if (mStartDoneEvent) {
            mVEAInitResult = VideoEncodeAcceleratorAdaptor::Result::PLATFORM_FAILURE;
            mStartDoneEvent->Signal();
            mStartDoneEvent = nullptr;
        } else {
            reportError(C2_CORRUPTED);
        }
        return;
    }

    mOutputBufferSize = outputBufferSize;

    mComponentState = ComponentState::STARTED;

#ifdef USE_VEA_FORMAT_CONVERTER
    // Note: OnRequireBitstreamBuffers() must not be called twice.
    CHECK(!mFormatConverter);
    mFormatConverter = C2VEAFormatConverter::Create(kInputPixelFormat, visibleSize, inputCount,
                                                    inputCodedSize);
    if (!mFormatConverter) {
        if (mStartDoneEvent) {
            mVEAInitResult = VideoEncodeAcceleratorAdaptor::Result::PLATFORM_FAILURE;
            mStartDoneEvent->Signal();
            mStartDoneEvent = nullptr;
        } else {
            reportError(C2_CORRUPTED);
        }
        return;
    }
#endif

    if (mStartDoneEvent) {
        mStartDoneEvent->Signal();
        mStartDoneEvent = nullptr;
        return;
    }

    // Starts to process queued works if any.
    if (!mQueue.empty()) {
        mTaskRunner->PostTask(
                FROM_HERE, ::base::Bind(&C2VEAComponent::onDequeueWork, ::base::Unretained(this)));
    }
}

bool C2VEAComponent::updateEncodingParametersIfChanged() {
    C2StreamBitrateInfo::output bitrate;
    C2StreamFrameRateInfo::output frameRate;
    c2_status_t status = mIntfImpl->query({&bitrate, &frameRate}, {}, C2_DONT_BLOCK, nullptr);
    if (status != C2_OK) {
        ALOGE("Failed to query encoding parameters from intf, error: %d", status);
        reportError(status);
        return false;
    }

    uint32_t bitrateValue = bitrate.value;
    uint32_t frameRateValue = static_cast<uint32_t>(std::round(frameRate.value));
    if (mRequestedBitrate != bitrateValue || mRequestedFrameRate != frameRateValue) {
        mRequestedBitrate = bitrate.value;
        mRequestedFrameRate = frameRate.value;
        return true;
    }
    return false;
}

c2_status_t C2VEAComponent::stop() {
    // Use mStartStopLock to block other asynchronously start/stop calls.
    std::lock_guard<std::mutex> lock(mStartStopLock);

    auto state = mState.load();
    if (!(state == State::RUNNING || state == State::ERROR)) {
        return C2_OK;  // Component is already in stopped state.
    }

    ::base::WaitableEvent done(::base::WaitableEvent::ResetPolicy::AUTOMATIC,
                               ::base::WaitableEvent::InitialState::NOT_SIGNALED);
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VEAComponent::onStop, ::base::Unretained(this), &done));
    done.Wait();
    mState.store(State::LOADED);
    return C2_OK;
}

void C2VEAComponent::onStop(::base::WaitableEvent* done) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onStop");
    // Stop call should be processed even if component is in error state.
    CHECK_NE(mComponentState, ComponentState::UNINITIALIZED);

    onFlush(false /* reinitAdaptor */);

    mOutputBlockPool.reset();

    mComponentState = ComponentState::UNINITIALIZED;
    done->Signal();
}

c2_status_t C2VEAComponent::reset() {
    return stop();
    // TODO(johnylin): reset is different than stop that it could be called in any state.
    // TODO(johnylin): when reset is called, set ComponentInterface to default values.
}

c2_status_t C2VEAComponent::release() {
    return reset();
}

std::shared_ptr<C2ComponentInterface> C2VEAComponent::intf() {
    return mIntf;
}

void C2VEAComponent::reportWorkIfFinished(uint64_t index) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());

    auto workIter = findPendingWorkByIndex(index);
    if (workIter == mPendingWorks.end()) {
        reportError(C2_CORRUPTED);
        return;
    }

    // EOS work will not be reported here. reportEOSWork() does it.
    if (isWorkDone(workIter->get())) {
        reportWork(std::move(*workIter));
        mPendingWorks.erase(workIter);
    }
}

void C2VEAComponent::reportWork(std::unique_ptr<C2Work> work) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("Reported finished work index=%llu", work->input.ordinal.frameIndex.peekull());

    work->result = C2_OK;
    work->workletsProcessed = static_cast<uint32_t>(work->worklets.size());

    std::list<std::unique_ptr<C2Work>> finishedWorks;
    finishedWorks.emplace_back(std::move(work));
    mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
}

bool C2VEAComponent::isWorkDone(const C2Work* work) const {
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        // This is EOS work and should be processed by reportEOSWork().
        return false;
    }
    if (work->input.buffers.front()) {
        // Input buffer is still owned by VEA.
        return false;
    }
    if (mPendingOutputEOS && mPendingWorks.size() == 1u) {
        // If mPendingOutputEOS is true, the last returned work should be marked EOS flag and
        // returned by reportEOSWork() instead.
        return false;
    }
    if (work->worklets.front()->output.buffers.empty()) {
        // Output buffer is not returned from VEA yet.
        return false;
    }
    return true;  // This work is done.
}

void C2VEAComponent::reportEOSWork() {
    ALOGV("reportEOSWork");
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    // In this moment all works prior to EOS work should be done and returned to listener.
    if (mPendingWorks.size() != 1u) {  // only EOS work left
        ALOGE("It shouldn't have remaining works in mPendingWorks except EOS work.");
        reportError(C2_CORRUPTED);
        return;
    }

    mPendingOutputEOS = false;

    std::unique_ptr<C2Work> eosWork(std::move(mPendingWorks.front()));
    mPendingWorks.pop_front();
    if (!eosWork->input.buffers.empty()) {
        eosWork->input.buffers.front().reset();
    }
    eosWork->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;
    reportWork(std::move(eosWork));
}

void C2VEAComponent::reportAbandonedWorks() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    std::list<std::unique_ptr<C2Work>> abandonedWorks;

    // Discard all pending output buffers (will not be returned from VEA after VEA reset).
    mOutputBlockMap.clear();

    while (!mPendingWorks.empty()) {
        std::unique_ptr<C2Work> work(std::move(mPendingWorks.front()));
        mPendingWorks.pop_front();

        // TODO: correlate the definition of flushed work result to framework.
        work->result = C2_NOT_FOUND;
        // When the work is abandoned, buffer in input.buffers shall reset by component.
        if (!work->input.buffers.empty()) {
            work->input.buffers.front().reset();
        }
        abandonedWorks.emplace_back(std::move(work));
    }

    // Pending EOS work will be abandoned here due to component flush if any.
    mPendingOutputEOS = false;

    if (!abandonedWorks.empty()) {
        mListener->onWorkDone_nb(shared_from_this(), std::move(abandonedWorks));
    }
}

void C2VEAComponent::reportError(c2_status_t error) {
    mListener->onError_nb(shared_from_this(), static_cast<uint32_t>(error));
    mComponentState = ComponentState::ERROR;
    mState.store(State::ERROR);
}

void C2VEAComponent::requireBitstreamBuffers(uint32_t inputCount, const media::Size& inputCodedSize,
                                             uint32_t outputBufferSize) {
    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VEAComponent::onRequireBitstreamBuffers,
                                                  ::base::Unretained(this), inputCount,
                                                  inputCodedSize, outputBufferSize));
}

void C2VEAComponent::notifyVideoFrameDone(uint64_t index) {
    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VEAComponent::onInputBufferDone,
                                                  ::base::Unretained(this), index));
}

void C2VEAComponent::bitstreamBufferReady(uint64_t index, uint32_t payloadSize, bool keyFrame,
                                          int64_t timestamp) {
    mTaskRunner->PostTask(
            FROM_HERE, ::base::Bind(&C2VEAComponent::onOutputBufferDone, ::base::Unretained(this),
                                    index, payloadSize, keyFrame, timestamp));
}

void C2VEAComponent::notifyFlushDone(bool done) {
    mTaskRunner->PostTask(
            FROM_HERE, ::base::Bind(&C2VEAComponent::onDrainDone, ::base::Unretained(this), done));
}

void C2VEAComponent::notifyError(VideoEncodeAcceleratorAdaptor::Result error) {
    ALOGE("Got notifyError from VEA...");
    c2_status_t err = adaptorResultToC2Status(error);
    if (err == C2_OK) {
        ALOGW("Shouldn't get SUCCESS err code in NotifyError(). Skip it...");
        return;
    }
    mTaskRunner->PostTask(
            FROM_HERE, ::base::Bind(&C2VEAComponent::reportError, ::base::Unretained(this), err));
}

class C2VEAComponentFactory : public C2ComponentFactory {
public:
    C2VEAComponentFactory(C2String encoderName)
          : mEncoderName(encoderName),
            mReflector(std::static_pointer_cast<C2ReflectorHelper>(
                    GetCodec2ArcComponentStore()->getParamReflector())){};

    c2_status_t createComponent(c2_node_id_t id, std::shared_ptr<C2Component>* const component,
                                ComponentDeleter deleter) override {
        UNUSED(deleter);
        *component = std::shared_ptr<C2Component>(new C2VEAComponent(mEncoderName, id, mReflector));
        return C2_OK;
    }

    c2_status_t createInterface(c2_node_id_t id,
                                std::shared_ptr<C2ComponentInterface>* const interface,
                                InterfaceDeleter deleter) override {
        UNUSED(deleter);

        *interface =
                std::shared_ptr<C2ComponentInterface>(new SimpleInterface<C2VEAComponent::IntfImpl>(
                        mEncoderName.c_str(), id,
                        std::make_shared<C2VEAComponent::IntfImpl>(mEncoderName, mReflector)));
        return C2_OK;
    }

    ~C2VEAComponentFactory() override = default;

private:
    const C2String mEncoderName;
    std::shared_ptr<C2ReflectorHelper> mReflector;
};
}  // namespace android

extern "C" ::C2ComponentFactory* CreateC2VEAH264Factory() {
    ALOGV("in %s", __func__);
    return new ::android::C2VEAComponentFactory(android::kH264EncoderName);
}

extern "C" void DestroyC2VEAH264Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}
