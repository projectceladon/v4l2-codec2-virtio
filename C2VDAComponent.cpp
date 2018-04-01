// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VDAComponent"

#ifdef V4L2_CODEC2_ARC
#include <C2VDAAdaptorProxy.h>
#else
#include <C2VDAAdaptor.h>
#endif

#define __C2_GENERATE_GLOBAL_VARS__
#include <C2VDAComponent.h>

#ifdef ANDROID_VERSION_NYC
// Get allocators from NYC-specific implementation
#include <C2VDASupport.h>
#else
// Get allocators from framework
#include <C2PlatformSupport.h>
#endif

#include <videodev2.h>

#include <C2ComponentFactory.h>

#include <base/bind.h>
#include <base/bind_helpers.h>

#include <media/stagefright/MediaDefs.h>
#include <utils/Log.h>
#include <utils/misc.h>

#include <inttypes.h>
#include <algorithm>

#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

namespace android {

namespace {

// Helper function to allocate string type parameters.
template <class T>
std::unique_ptr<T> allocUniqueCstr(const char* cstr) {
    size_t len = strlen(cstr) + sizeof(char);  // '\0' in the tail
    std::unique_ptr<T> ptr = T::AllocUnique(len);
    memcpy(ptr->m.value, cstr, len);
    return ptr;
}

template <class T>
std::unique_ptr<C2SettingResult> reportReadOnlyFailure(C2Param* c2Param) {
    T* param = (T*)c2Param;
    return std::unique_ptr<C2SettingResult>(
            new C2SettingResult{C2SettingResult::READ_ONLY,
                                {C2ParamField(param, &T::value), nullptr /* supportedValues */},
                                {} /* conflictedFields */});
}

template <class T>
std::unique_ptr<C2SettingResult> reportReadOnlyFlexFailure(C2Param* c2Param) {
    T* param = (T*)c2Param;
    return std::unique_ptr<C2SettingResult>(
            new C2SettingResult{C2SettingResult::READ_ONLY,
                                {C2ParamField(param, &T::m), nullptr /* supportedValues */},
                                {} /* conflictedFields */});
}

// Helper function to find int32_t value from C2Value::Primitive vector.
bool findInt32FromPrimitiveValues(const int32_t& v, const C2FieldSupportedValues& values) {
    if (values.type == C2FieldSupportedValues::EMPTY) {
        return false;
    }
    if (values.type == C2FieldSupportedValues::FLAGS) {
        ALOGE("Type of field supported values should not be FLAGS.");
        return false;
    }
    if (values.type == C2FieldSupportedValues::RANGE) {
        // Only support min/max/step case only.
        return v >= values.range.min.i32 && v <= values.range.max.i32 &&
               ((v - values.range.min.i32) % values.range.step.i32 == 0);
    }
    // if values.type == C2FieldSupportedValues::VALUES
    return std::any_of(values.values.begin(), values.values.end(),
                       [v = v](const auto& value) { return value.i32 == v; });
}

// Helper function to find uint32_t value from C2Value::Primitive vector.
bool findUint32FromPrimitiveValues(const uint32_t& v, const C2FieldSupportedValues& values) {
    if (values.type == C2FieldSupportedValues::EMPTY) {
        return false;
    }
    if (values.type == C2FieldSupportedValues::FLAGS) {
        ALOGE("Type of field supported values should not be FLAGS.");
        return false;
    }
    if (values.type == C2FieldSupportedValues::RANGE) {
        // Only support min/max/step case only.
        return v >= values.range.min.u32 && v <= values.range.max.u32 &&
               ((v - values.range.min.u32) % values.range.step.u32 == 0);
    }
    // if values.type == C2FieldSupportedValues::VALUES
    return std::any_of(values.values.begin(), values.values.end(),
                       [v = v](const auto& value) { return value.u32 == v; });
}

// Mask against 30 bits to avoid (undefined) wraparound on signed integer.
int32_t frameIndexToBitstreamId(c2_cntr64_t frameIndex) {
    return static_cast<int32_t>(frameIndex.peeku() & 0x3FFFFFFF);
}

const C2String kH264DecoderName = "c2.vda.avc.decoder";
const C2String kVP8DecoderName = "c2.vda.vp8.decoder";
const C2String kVP9DecoderName = "c2.vda.vp9.decoder";

}  // namespace

C2VDAComponentIntf::C2VDAComponentIntf(C2String name, c2_node_id_t id)
      : kName(name),
        kId(id),
        mInitStatus(C2_OK),
        mDomainInfo(C2DomainVideo),
        mInputFormat(0u, C2FormatCompressed),
        mOutputFormat(0u, C2FormatVideo),
        mOutputPortMime(allocUniqueCstr<C2PortMimeConfig::output>(MEDIA_MIMETYPE_VIDEO_RAW)),
        mOutputBlockPools(C2PortBlockPoolsTuning::output::AllocUnique({})) {
    // TODO(johnylin): use factory function to determine whether V4L2 stream or slice API is.
    uint32_t inputFormatFourcc;
    if (name == kH264DecoderName) {
        mInputPortMime = allocUniqueCstr<C2PortMimeConfig::input>(MEDIA_MIMETYPE_VIDEO_AVC);
        inputFormatFourcc = V4L2_PIX_FMT_H264_SLICE;
    } else if (name == kVP8DecoderName) {
        mInputPortMime = allocUniqueCstr<C2PortMimeConfig::input>(MEDIA_MIMETYPE_VIDEO_VP8);
        inputFormatFourcc = V4L2_PIX_FMT_VP8_FRAME;
    } else if (name == kVP9DecoderName) {
        mInputPortMime = allocUniqueCstr<C2PortMimeConfig::input>(MEDIA_MIMETYPE_VIDEO_VP9);
        inputFormatFourcc = V4L2_PIX_FMT_VP9_FRAME;
    } else {
        ALOGE("Invalid component name: %s", name.c_str());
        mInitStatus = C2_BAD_VALUE;
        return;
    }
    // Get supported profiles from VDA.
    // TODO: re-think the suitable method of getting supported profiles for both pure Android and
    //       ARC++.
#ifdef V4L2_CODEC2_ARC
    mSupportedProfiles = arc::C2VDAAdaptorProxy::GetSupportedProfiles(inputFormatFourcc);
#else
    mSupportedProfiles = C2VDAAdaptor::GetSupportedProfiles(inputFormatFourcc);
#endif
    if (mSupportedProfiles.empty()) {
        ALOGE("No supported profile from input format: %u", inputFormatFourcc);
        mInitStatus = C2_BAD_VALUE;
        return;
    }

    // Set default codec profile.
    mInputCodecProfile.value = mSupportedProfiles[0].profile;

    auto minVideoSize = mSupportedProfiles[0].min_resolution;
    auto maxVideoSize = mSupportedProfiles[0].max_resolution;
    // Set default output video size.
    mVideoSize.width = minVideoSize.width();
    mVideoSize.height = minVideoSize.height();
    // Set default max video size.
    mMaxVideoSizeHint.width = maxVideoSize.width();
    mMaxVideoSizeHint.height = maxVideoSize.height();

    for (const auto& supportedProfile : mSupportedProfiles) {
        mSupportedCodecProfiles.push_back(supportedProfile.profile);
        ALOGI("Get supported profile: profile=%d, min_res=%s, max_res=%s", supportedProfile.profile,
              supportedProfile.min_resolution.ToString().c_str(),
              supportedProfile.max_resolution.ToString().c_str());
    }

    auto insertParam = [& params = mParams](C2Param* param) { params[param->index()] = param; };

    insertParam(&mDomainInfo);
    insertParam(&mInputFormat);
    insertParam(&mOutputFormat);
    insertParam(mInputPortMime.get());
    insertParam(mOutputPortMime.get());

    insertParam(&mInputCodecProfile);
    mSupportedValues.emplace(C2ParamField(&mInputCodecProfile, &C2VDAStreamProfileConfig::value),
                             C2FieldSupportedValues(false, mSupportedCodecProfiles));

    // TODO(johnylin): min/max resolution may change by chosen profile, we should dynamically change
    // the supported values in the future.
    insertParam(&mVideoSize);
    mSupportedValues.emplace(
            C2ParamField(&mVideoSize, &C2VideoSizeStreamInfo::width),
            C2FieldSupportedValues(minVideoSize.width(), maxVideoSize.width(), 16));
    mSupportedValues.emplace(
            C2ParamField(&mVideoSize, &C2VideoSizeStreamInfo::height),
            C2FieldSupportedValues(minVideoSize.height(), maxVideoSize.height(), 16));

    insertParam(&mMaxVideoSizeHint);
    mSupportedValues.emplace(
            C2ParamField(&mMaxVideoSizeHint, &C2MaxVideoSizeHintPortSetting::width),
            C2FieldSupportedValues(minVideoSize.width(), maxVideoSize.width(), 16));
    mSupportedValues.emplace(
            C2ParamField(&mMaxVideoSizeHint, &C2MaxVideoSizeHintPortSetting::height),
            C2FieldSupportedValues(minVideoSize.height(), maxVideoSize.height(), 16));

    insertParam(mOutputBlockPools.get());

    mParamDescs.push_back(std::make_shared<C2ParamDescriptor>(true, "_domain", &mDomainInfo));
    mParamDescs.push_back(
            std::make_shared<C2ParamDescriptor>(false, "_input_format", &mInputFormat));
    mParamDescs.push_back(
            std::make_shared<C2ParamDescriptor>(false, "_output_format", &mOutputFormat));
    mParamDescs.push_back(
            std::make_shared<C2ParamDescriptor>(true, "_input_port_mime", mInputPortMime.get()));
    mParamDescs.push_back(
            std::make_shared<C2ParamDescriptor>(true, "_output_port_mime", mOutputPortMime.get()));
    mParamDescs.push_back(std::make_shared<C2ParamDescriptor>(false, "_input_codec_profile",
                                                              &mInputCodecProfile));
    mParamDescs.push_back(std::make_shared<C2ParamDescriptor>(false, "_video_size", &mVideoSize));
    mParamDescs.push_back(
            std::make_shared<C2ParamDescriptor>(false, "_max_video_size_hint", &mMaxVideoSizeHint));
    mParamDescs.push_back(std::make_shared<C2ParamDescriptor>(false, "_output_block_pools",
                                                              mOutputBlockPools.get()));
}

C2String C2VDAComponentIntf::getName() const {
    return kName;
}

c2_node_id_t C2VDAComponentIntf::getId() const {
    return kId;
}

c2_status_t C2VDAComponentIntf::query_vb(
        const std::vector<C2Param*>& stackParams,
        const std::vector<C2Param::Index>& heapParamIndices, c2_blocking_t mayBlock,
        std::vector<std::unique_ptr<C2Param>>* const heapParams) const {
    UNUSED(mayBlock);
    c2_status_t err = C2_OK;
    for (C2Param* const param : stackParams) {
        if (!param || !*param) {
            continue;
        }

        uint32_t index = param->index();
        C2Param* myParam = getParamByIndex(index);
        if (!myParam || (myParam->size() != param->size())) {
            param->invalidate();
            err = C2_BAD_INDEX;
            continue;
        }

        param->updateFrom(*myParam);
    }

    // heapParams should not be nullptr if heapParamIndices is not empty.
    CHECK(heapParamIndices.size() == 0 || heapParams);
    for (const C2Param::Index index : heapParamIndices) {
        C2Param* myParam = getParamByIndex(index);
        if (myParam) {
            heapParams->emplace_back(C2Param::Copy(*myParam));
        } else {
            err = C2_BAD_INDEX;
        }
    }

    return err;
}

c2_status_t C2VDAComponentIntf::config_vb(
        const std::vector<C2Param*>& params, c2_blocking_t mayBlock,
        std::vector<std::unique_ptr<C2SettingResult>>* const failures) {
    UNUSED(mayBlock);
    c2_status_t err = C2_OK;
    for (C2Param* const param : params) {
        uint32_t index = param->index();
        C2Param* myParam = getParamByIndex(index);
        if (!myParam) {
            // C2_BAD_INDEX should be the lowest priority except for C2_OK.
            err = (err == C2_OK) ? C2_BAD_INDEX : err;
            continue;
        }

        if (index == mDomainInfo.index()) {  // read-only
            failures->push_back(reportReadOnlyFailure<decltype(mDomainInfo)>(param));
            err = C2_BAD_VALUE;
            continue;
        } else if (index == mInputFormat.index()) {  // read-only
            failures->push_back(reportReadOnlyFailure<decltype(mInputFormat)>(param));
            err = C2_BAD_VALUE;
            continue;
        } else if (index == mOutputFormat.index()) {  // read-only
            failures->push_back(reportReadOnlyFailure<decltype(mOutputFormat)>(param));
            err = C2_BAD_VALUE;
            continue;
        } else if (index == mInputPortMime->index()) {  // read-only
            failures->push_back(reportReadOnlyFlexFailure<
                                std::remove_pointer<decltype(mInputPortMime.get())>::type>(param));
            err = C2_BAD_VALUE;
            continue;
        } else if (index == mOutputPortMime->index()) {  // read-only
            failures->push_back(reportReadOnlyFlexFailure<
                                std::remove_pointer<decltype(mOutputPortMime.get())>::type>(param));
            err = C2_BAD_VALUE;
            continue;
        } else if (index == mInputCodecProfile.index()) {
            std::unique_ptr<C2SettingResult> result =
                    validateUint32Config<decltype(mInputCodecProfile)>(param);
            if (result) {
                failures->push_back(std::move(result));
                err = C2_BAD_VALUE;
                continue;
            }
        } else if (index == mVideoSize.index()) {
            std::unique_ptr<C2SettingResult> result =
                    validateVideoSizeConfig<decltype(mVideoSize)>(param);
            if (result) {
                failures->push_back(std::move(result));
                err = C2_BAD_VALUE;
                continue;
            }
        } else if (index == mMaxVideoSizeHint.index()) {
            std::unique_ptr<C2SettingResult> result =
                    validateVideoSizeConfig<decltype(mMaxVideoSizeHint)>(param);
            if (result) {
                failures->push_back(std::move(result));
                err = C2_BAD_VALUE;
                continue;
            }
        } else if (index == mOutputBlockPools->index()) {
            // setting output block pools
            // TODO: add support for output-block-pools (this will be done when we move all
            // config to shared ptr)
            mOutputBlockPools.reset(
                    static_cast<C2PortBlockPoolsTuning::output*>(C2Param::Copy(*param).release()));
            continue;
        }
        myParam->updateFrom(*param);
    }
    return err;
}

c2_status_t C2VDAComponentIntf::createTunnel_sm(c2_node_id_t targetComponent) {
    UNUSED(targetComponent);
    return C2_OMITTED;  // Tunneling is not supported by now
}

c2_status_t C2VDAComponentIntf::releaseTunnel_sm(c2_node_id_t targetComponent) {
    UNUSED(targetComponent);
    return C2_OMITTED;  // Tunneling is not supported by now
}

c2_status_t C2VDAComponentIntf::querySupportedParams_nb(
        std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const {
    params->clear();
    params->insert(params->begin(), mParamDescs.begin(), mParamDescs.end());
    return C2_OK;
}

c2_status_t C2VDAComponentIntf::querySupportedValues_vb(
        std::vector<C2FieldSupportedValuesQuery>& fields, c2_blocking_t mayBlock) const {
    UNUSED(mayBlock);
    c2_status_t err = C2_OK;
    for (auto& query : fields) {
        if (mSupportedValues.count(query.field()) == 0) {
            query.status = C2_BAD_INDEX;
            err = C2_BAD_INDEX;
            continue;
        }
        query.status = C2_OK;
        query.values = mSupportedValues.at(query.field());
    }
    return err;
}

c2_status_t C2VDAComponentIntf::status() const {
    return mInitStatus;
}

C2Param* C2VDAComponentIntf::getParamByIndex(uint32_t index) const {
    auto iter = mParams.find(index);
    return (iter != mParams.end()) ? iter->second : nullptr;
}

template <class T>
std::unique_ptr<C2SettingResult> C2VDAComponentIntf::validateVideoSizeConfig(
        C2Param* c2Param) const {
    T* videoSize = (T*)c2Param;

    C2ParamField fieldWidth(videoSize, &T::width);
    const C2FieldSupportedValues& widths = mSupportedValues.at(fieldWidth);
    CHECK_EQ(widths.type, C2FieldSupportedValues::RANGE);
    if (!findInt32FromPrimitiveValues(videoSize->width, widths)) {
        std::unique_ptr<C2SettingResult> result(new C2SettingResult{
                C2SettingResult::BAD_VALUE,
                {fieldWidth, std::make_unique<C2FieldSupportedValues>(
                                     widths.range.min, widths.range.max, widths.range.step)},
                {} /* conflicts */});
        return result;
    }

    C2ParamField fieldHeight(videoSize, &T::height);
    const C2FieldSupportedValues& heights = mSupportedValues.at(fieldHeight);
    CHECK_EQ(heights.type, C2FieldSupportedValues::RANGE);
    if (!findInt32FromPrimitiveValues(videoSize->height, heights)) {
        std::unique_ptr<C2SettingResult> result(new C2SettingResult{
                C2SettingResult::BAD_VALUE,
                {fieldHeight, std::make_unique<C2FieldSupportedValues>(
                                      heights.range.min, heights.range.max, heights.range.step)},
                {} /* conflicts */});
        return result;
    }

    return nullptr;
}

template <class T>
std::unique_ptr<C2SettingResult> C2VDAComponentIntf::validateUint32Config(C2Param* c2Param) const {
    T* config = (T*)c2Param;

    C2ParamField field(config, &T::value);
    const C2FieldSupportedValues& configs = mSupportedValues.at(field);
    if (!findUint32FromPrimitiveValues(config->value, configs)) {
        std::unique_ptr<C2SettingResult> result(new C2SettingResult{
                C2SettingResult::BAD_VALUE, {field, nullptr}, {} /* conflicts */});
        if (configs.type == C2FieldSupportedValues::RANGE) {
            result->field.values.reset(new C2FieldSupportedValues(
                    configs.range.min, configs.range.max, configs.range.step));
        } else if (configs.type == C2FieldSupportedValues::VALUES) {
            result->field.values.reset(new C2FieldSupportedValues(false, configs.values));
        } else {
            return nullptr;
        }
        return result;
    }

    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
#define EXPECT_STATE_OR_RETURN_ON_ERROR(x)                    \
    do {                                                      \
        if (mComponentState == ComponentState::ERROR) return; \
        CHECK_EQ(mComponentState, ComponentState::x);         \
    } while (0)

#define EXPECT_RUNNING_OR_RETURN_ON_ERROR()                       \
    do {                                                          \
        if (mComponentState == ComponentState::ERROR) return;     \
        CHECK_NE(mComponentState, ComponentState::UNINITIALIZED); \
    } while (0)

class C2VDAGraphicBuffer : public C2Buffer {
public:
    C2VDAGraphicBuffer(const std::shared_ptr<C2GraphicBlock>& block,
                       const base::Closure& releaseCB);
    ~C2VDAGraphicBuffer() override;

private:
    base::Closure mReleaseCB;
};

C2VDAGraphicBuffer::C2VDAGraphicBuffer(const std::shared_ptr<C2GraphicBlock>& block,
                                       const base::Closure& releaseCB)
      : C2Buffer({block->share(C2Rect(block->width(), block->height()), C2Fence())}),
        mReleaseCB(releaseCB) {}

C2VDAGraphicBuffer::~C2VDAGraphicBuffer() {
    if (!mReleaseCB.is_null()) {
        mReleaseCB.Run();
    }
}

C2VDAComponent::VideoFormat::VideoFormat(HalPixelFormat pixelFormat, uint32_t minNumBuffers,
                                         media::Size codedSize, media::Rect visibleRect)
      : mPixelFormat(pixelFormat),
        mMinNumBuffers(minNumBuffers),
        mCodedSize(codedSize),
        mVisibleRect(visibleRect) {}

C2VDAComponent::C2VDAComponent(C2String name, c2_node_id_t id)
      : mIntf(std::make_shared<C2VDAComponentIntf>(name, id)),
        mThread("C2VDAComponentThread"),
        mVDAInitResult(VideoDecodeAcceleratorAdaptor::Result::ILLEGAL_STATE),
        mComponentState(ComponentState::UNINITIALIZED),
        mDrainWithEOS(false),
        mColorFormat(0u),
        mLastOutputTimestamp(-1),
        mCodecProfile(media::VIDEO_CODEC_PROFILE_UNKNOWN),
        mState(State::UNLOADED),
        mWeakThisFactory(this) {
    // TODO(johnylin): the client may need to know if init is failed.
    if (mIntf->status() != C2_OK) {
        ALOGE("Component interface init failed (err code = %d)", mIntf->status());
        return;
    }
    if (!mThread.Start()) {
        ALOGE("Component thread failed to start.");
        return;
    }
    mTaskRunner = mThread.task_runner();
    mTaskRunner->PostTask(FROM_HERE, base::Bind(&C2VDAComponent::onCreate, base::Unretained(this)));
    mState.store(State::LOADED);
}

C2VDAComponent::~C2VDAComponent() {
    CHECK_EQ(mState.load(), State::LOADED);

    if (mThread.IsRunning()) {
        mTaskRunner->PostTask(FROM_HERE,
                              base::Bind(&C2VDAComponent::onDestroy, base::Unretained(this)));
        mThread.Stop();
    }
}

void C2VDAComponent::fetchParametersFromIntf() {
    C2VDAStreamProfileConfig::input codecProfile;
    std::vector<C2Param*> stackParams{&codecProfile};
    CHECK_EQ(mIntf->query_vb(stackParams, {}, C2_DONT_BLOCK, nullptr), C2_OK);
    // The value should be guaranteed to be within media::VideoCodecProfile enum range by component
    // interface.
    mCodecProfile = static_cast<media::VideoCodecProfile>(codecProfile.value);
    ALOGI("get parameter: mCodecProfile = %d", static_cast<int>(mCodecProfile));
}

void C2VDAComponent::onCreate() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onCreate");
#ifdef V4L2_CODEC2_ARC
    mVDAAdaptor.reset(new arc::C2VDAAdaptorProxy());
#else
    mVDAAdaptor.reset(new C2VDAAdaptor());
#endif
}

void C2VDAComponent::onDestroy() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDestroy");
    if (mVDAAdaptor.get()) {
        mVDAAdaptor->destroy();
        mVDAAdaptor.reset(nullptr);
    }
}

void C2VDAComponent::onStart(media::VideoCodecProfile profile, base::WaitableEvent* done) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onStart");
    CHECK_EQ(mComponentState, ComponentState::UNINITIALIZED);
    // TODO: Set secureMode value dynamically.
    bool secureMode = false;
    mVDAInitResult = mVDAAdaptor->initialize(profile, secureMode, this);
    if (mVDAInitResult == VideoDecodeAcceleratorAdaptor::Result::SUCCESS) {
        mComponentState = ComponentState::STARTED;
    }

    done->Signal();
}

void C2VDAComponent::onQueueWork(std::unique_ptr<C2Work> work) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onQueueWork: flags=0x%x, index=%llu, timestamp=%llu", work->input.flags,
          work->input.ordinal.frameIndex.peekull(), work->input.ordinal.timestamp.peekull());
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();
    // It is illegal for client to put new works while component is still flushing.
    CHECK_NE(mComponentState, ComponentState::FLUSHING);

    uint32_t drainMode = NO_DRAIN;
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        drainMode = DRAIN_COMPONENT_WITH_EOS;
    }
    mQueue.push({std::move(work), drainMode});
    // TODO(johnylin): set a maximum size of mQueue and check if mQueue is already full.

    mTaskRunner->PostTask(FROM_HERE,
                          base::Bind(&C2VDAComponent::onDequeueWork, base::Unretained(this)));
}

void C2VDAComponent::onDequeueWork() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDequeueWork");
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();
    if (mQueue.empty()) {
        return;
    }
    if (mComponentState == ComponentState::DRAINING) {
        ALOGV("Temporarily stop dequeueing works since component is draining.");
        return;
    }
    if (mComponentState != ComponentState::STARTED) {
        ALOGE("Work queue should be empty if the component is not in STARTED state.");
        return;
    }

    // Dequeue a work from mQueue.
    std::unique_ptr<C2Work> work(std::move(mQueue.front().mWork));
    auto drainMode = mQueue.front().mDrainMode;
    mQueue.pop();

    CHECK_EQ(work->input.buffers.size(), 1u);
    C2ConstLinearBlock linearBlock = work->input.buffers.front()->data().linearBlocks().front();
    // linearBlock.size() == 0 means this is a dummy work. No decode needed.
    if (linearBlock.size() > 0) {
        // Send input buffer to VDA for decode.
        // Use frameIndex as bitstreamId.
        int32_t bitstreamId = frameIndexToBitstreamId(work->input.ordinal.frameIndex);
        sendInputBufferToAccelerator(linearBlock, bitstreamId);
    }

    CHECK_EQ(work->worklets.size(), 1u);
    work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(0);
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;

    if (drainMode != NO_DRAIN) {
        mVDAAdaptor->flush();
        mComponentState = ComponentState::DRAINING;
        mDrainWithEOS = drainMode == DRAIN_COMPONENT_WITH_EOS;
    }

    // Put work to mPendingWorks.
    mPendingWorks.emplace_back(std::move(work));

    if (!mQueue.empty()) {
        mTaskRunner->PostTask(FROM_HERE,
                              base::Bind(&C2VDAComponent::onDequeueWork, base::Unretained(this)));
    }
}

void C2VDAComponent::onInputBufferDone(int32_t bitstreamId) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onInputBufferDone: bitstream id=%d", bitstreamId);
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    C2Work* work = getPendingWorkByBitstreamId(bitstreamId);
    if (!work) {
        reportError(C2_CORRUPTED);
        return;
    }

    // When the work is done, the input buffers vector shall be cleared by component.
    work->input.buffers.clear();

    reportFinishedWorkIfAny();
}

// This is used as callback while output buffer is released by client.
// TODO(johnylin): consider to use C2Buffer::registerOnDestroyNotify instead
void C2VDAComponent::returnOutputBuffer(int32_t pictureBufferId) {
    mTaskRunner->PostTask(FROM_HERE, base::Bind(&C2VDAComponent::onOutputBufferReturned,
                                                base::Unretained(this), pictureBufferId));
}

void C2VDAComponent::onOutputBufferReturned(int32_t pictureBufferId) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onOutputBufferReturned: picture id=%d", pictureBufferId);
    if (mComponentState == ComponentState::UNINITIALIZED) {
        // Output buffer is returned from client after component is stopped. Just let the buffer be
        // released.
        return;
    }

    // TODO(johnylin): when buffer is returned, we should confirm that output format is not changed
    //                 yet. If changed, just let the buffer be released.
    GraphicBlockInfo* info = getGraphicBlockById(pictureBufferId);
    if (!info) {
        reportError(C2_CORRUPTED);
        return;
    }
    CHECK_EQ(info->mState, GraphicBlockInfo::State::OWNED_BY_CLIENT);
    info->mState = GraphicBlockInfo::State::OWNED_BY_COMPONENT;

    if (mPendingOutputFormat) {
        tryChangeOutputFormat();
    } else {
        sendOutputBufferToAccelerator(info);
    }
}

void C2VDAComponent::onOutputBufferDone(int32_t pictureBufferId, int32_t bitstreamId) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onOutputBufferDone: picture id=%d, bitstream id=%d", pictureBufferId, bitstreamId);
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    C2Work* work = getPendingWorkByBitstreamId(bitstreamId);
    if (!work) {
        reportError(C2_CORRUPTED);
        return;
    }
    GraphicBlockInfo* info = getGraphicBlockById(pictureBufferId);
    if (!info) {
        reportError(C2_CORRUPTED);
        return;
    }
    CHECK_EQ(info->mState, GraphicBlockInfo::State::OWNED_BY_ACCELERATOR);
    // Output buffer will be passed to client soon along with mListener->onWorkDone_nb().
    info->mState = GraphicBlockInfo::State::OWNED_BY_CLIENT;

    // Attach output buffer to the work corresponded to bitstreamId.
    work->worklets.front()->output.buffers.emplace_back(std::make_shared<C2VDAGraphicBuffer>(
            info->mGraphicBlock, base::Bind(&C2VDAComponent::returnOutputBuffer,
                                            mWeakThisFactory.GetWeakPtr(), pictureBufferId)));

    // TODO: this does not work for timestamps as they can wrap around
    int64_t currentTimestamp = base::checked_cast<int64_t>(work->input.ordinal.timestamp.peek());
    CHECK_GE(currentTimestamp, mLastOutputTimestamp);
    mLastOutputTimestamp = currentTimestamp;

    reportFinishedWorkIfAny();
}

void C2VDAComponent::onDrain(uint32_t drainMode) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDrain: mode = %u", drainMode);
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    if (!mQueue.empty()) {
        // Mark last queued work as "drain-till-here" by setting drainMode. Do not change drainMode
        // if last work already has one.
        if (mQueue.back().mDrainMode == NO_DRAIN) {
            mQueue.back().mDrainMode = drainMode;
        }
    } else if (!mPendingWorks.empty()) {
        // Neglect drain request if component is not in STARTED mode. Otherwise, enters DRAINING
        // mode and signal VDA flush immediately.
        if (mComponentState == ComponentState::STARTED) {
            mVDAAdaptor->flush();
            mComponentState = ComponentState::DRAINING;
            mDrainWithEOS = drainMode == DRAIN_COMPONENT_WITH_EOS;
        } else {
            ALOGV("Neglect drain. Component in state: %d", mComponentState);
        }
    } else {
        // Do nothing.
        ALOGV("No buffers in VDA, drain takes no effect.");
    }
}

void C2VDAComponent::onDrainDone() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDrainDone");
    if (mComponentState == ComponentState::DRAINING) {
        mComponentState = ComponentState::STARTED;
    } else if (mComponentState == ComponentState::STOPPING) {
        // The client signals stop right before VDA notifies drain done. Let stop process goes.
        return;
    } else {
        ALOGE("Unexpected state while onDrainDone(). State=%d", mComponentState);
        reportError(C2_BAD_STATE);
        return;
    }

    if (mDrainWithEOS) {
        // Return EOS work.
        reportEOSWork();
    }
    // mPendingWorks must be empty after draining is finished.
    CHECK(mPendingWorks.empty());

    // Last stream is finished. Reset the timestamp record.
    mLastOutputTimestamp = -1;

    // Work dequeueing was stopped while component draining. Restart it.
    mTaskRunner->PostTask(FROM_HERE,
                          base::Bind(&C2VDAComponent::onDequeueWork, base::Unretained(this)));
}

void C2VDAComponent::onFlush() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onFlush");
    if (mComponentState == ComponentState::FLUSHING) {
        return;  // Ignore other flush request when component is flushing.
    }
    EXPECT_STATE_OR_RETURN_ON_ERROR(STARTED);

    mVDAAdaptor->reset();
    // Pop all works in mQueue and put into mPendingWorks.
    while (!mQueue.empty()) {
        mPendingWorks.emplace_back(std::move(mQueue.front().mWork));
        mQueue.pop();
    }
    mComponentState = ComponentState::FLUSHING;
}

void C2VDAComponent::onStop(base::WaitableEvent* done) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onStop");
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    mVDAAdaptor->reset();
    // Pop all works in mQueue and put into mPendingWorks.
    while (!mQueue.empty()) {
        mPendingWorks.emplace_back(std::move(mQueue.front().mWork));
        mQueue.pop();
    }

    mStopDoneEvent = done;  // restore done event which shoud be signaled in onStopDone().
    mComponentState = ComponentState::STOPPING;
}

void C2VDAComponent::onResetDone() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    if (mComponentState == ComponentState::ERROR) {
        return;
    }
    if (mComponentState == ComponentState::FLUSHING) {
        onFlushDone();
    } else if (mComponentState == ComponentState::STOPPING) {
        onStopDone();
    } else {
        reportError(C2_CORRUPTED);
    }
}

void C2VDAComponent::onFlushDone() {
    ALOGV("onFlushDone");
    reportAbandonedWorks();
    // Reset the timestamp record.
    mLastOutputTimestamp = -1;
    mComponentState = ComponentState::STARTED;
}

void C2VDAComponent::onStopDone() {
    ALOGV("onStopDone");
    CHECK(mStopDoneEvent);

    // Release the graphic block allocator object.
    mOutputBlockPool.reset();

    // TODO(johnylin): At this moment, there may be C2Buffer still owned by client, do we need to
    // do something for them?
    reportAbandonedWorks();
    mPendingOutputFormat.reset();
    mColorFormat = 0u;
    mLastOutputTimestamp = -1;
    if (mVDAAdaptor.get()) {
        mVDAAdaptor->destroy();
        mVDAAdaptor.reset(nullptr);
    }

    mGraphicBlocks.clear();

    mStopDoneEvent->Signal();
    mStopDoneEvent = nullptr;
    mComponentState = ComponentState::UNINITIALIZED;
}

c2_status_t C2VDAComponent::setListener_vb(const std::shared_ptr<C2Component::Listener>& listener,
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

void C2VDAComponent::sendInputBufferToAccelerator(const C2ConstLinearBlock& input,
                                                  int32_t bitstreamId) {
    ALOGV("sendInputBufferToAccelerator");
    int dupFd = dup(input.handle()->data[0]);
    if (dupFd < 0) {
        ALOGE("Failed to dup(%d) input buffer (bitstreamId=%d), errno=%d", input.handle()->data[0],
              bitstreamId, errno);
        reportError(C2_CORRUPTED);
        return;
    }
    ALOGV("Decode bitstream ID: %d, offset: %u size: %u", bitstreamId, input.offset(),
          input.size());
    mVDAAdaptor->decode(bitstreamId, dupFd, input.offset(), input.size());
}

C2Work* C2VDAComponent::getPendingWorkByBitstreamId(int32_t bitstreamId) {
    auto workIter = std::find_if(mPendingWorks.begin(), mPendingWorks.end(),
                                 [bitstreamId](const std::unique_ptr<C2Work>& w) {
                                     return frameIndexToBitstreamId(w->input.ordinal.frameIndex) ==
                                            bitstreamId;
                                 });

    if (workIter == mPendingWorks.end()) {
        ALOGE("Can't find pending work by bitstream ID: %d", bitstreamId);
        return nullptr;
    }
    return workIter->get();
}

C2VDAComponent::GraphicBlockInfo* C2VDAComponent::getGraphicBlockById(int32_t blockId) {
    if (blockId < 0 || blockId >= static_cast<int32_t>(mGraphicBlocks.size())) {
        ALOGE("getGraphicBlockById failed: id=%d", blockId);
        return nullptr;
    }
    return &mGraphicBlocks[blockId];
}

void C2VDAComponent::onOutputFormatChanged(std::unique_ptr<VideoFormat> format) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onOutputFormatChanged");
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    ALOGV("New output format(pixel_format=0x%x, min_num_buffers=%u, coded_size=%s, crop_rect=%s)",
          static_cast<uint32_t>(format->mPixelFormat), format->mMinNumBuffers,
          format->mCodedSize.ToString().c_str(), format->mVisibleRect.ToString().c_str());

    for (auto& info : mGraphicBlocks) {
        if (info.mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR)
            info.mState = GraphicBlockInfo::State::OWNED_BY_COMPONENT;
    }

    CHECK(!mPendingOutputFormat);
    mPendingOutputFormat = std::move(format);
    tryChangeOutputFormat();
}

void C2VDAComponent::tryChangeOutputFormat() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("tryChangeOutputFormat");
    CHECK(mPendingOutputFormat);

    // Change the output format only after all output buffers are returned
    // from clients.
    // TODO(johnylin): don't need to wait for new proposed buffer flow.
    for (const auto& info : mGraphicBlocks) {
        if (info.mState == GraphicBlockInfo::State::OWNED_BY_CLIENT) {
            ALOGV("wait buffer: %d for output format change", info.mBlockId);
            return;
        }
    }

    CHECK_EQ(mPendingOutputFormat->mPixelFormat, HalPixelFormat::YCbCr_420_888);

    mOutputFormat.mPixelFormat = mPendingOutputFormat->mPixelFormat;
    mOutputFormat.mMinNumBuffers = mPendingOutputFormat->mMinNumBuffers;
    mOutputFormat.mCodedSize = mPendingOutputFormat->mCodedSize;

    setOutputFormatCrop(mPendingOutputFormat->mVisibleRect);
    mColorFormat = kColorFormatYUV420Flexible;

    c2_status_t err = allocateBuffersFromBlockAllocator(
            mPendingOutputFormat->mCodedSize,
            static_cast<uint32_t>(mPendingOutputFormat->mPixelFormat));
    if (err != C2_OK) {
        reportError(err);
        return;
    }

    for (auto& info : mGraphicBlocks) {
        sendOutputBufferToAccelerator(&info);
    }
    mPendingOutputFormat.reset();
}

c2_status_t C2VDAComponent::allocateBuffersFromBlockAllocator(const media::Size& size,
                                                              uint32_t pixelFormat) {
    ALOGV("allocateBuffersFromBlockAllocator(%s, 0x%x)", size.ToString().c_str(), pixelFormat);

    size_t bufferCount = mOutputFormat.mMinNumBuffers + kDpbOutputBufferExtraCount;

    // Allocate the output buffers.
    mVDAAdaptor->assignPictureBuffers(bufferCount);

    // TODO: lock access to interface
    C2BlockPool::local_id_t poolId = mIntf->mOutputBlockPools->flexCount()
                                             ? mIntf->mOutputBlockPools->m.values[0]
                                             : C2BlockPool::BASIC_GRAPHIC;
    ALOGI("Using C2BlockPool ID = %" PRIu64 " for allocating output buffers", poolId);
    c2_status_t err;
    if (!mOutputBlockPool || mOutputBlockPool->getLocalId() != poolId) {
        err = GetCodec2BlockPool(poolId, shared_from_this(), &mOutputBlockPool);
        if (err != C2_OK) {
            ALOGE("Graphic block allocator is invalid");
            reportError(err);
            return err;
        }
    }

    mGraphicBlocks.clear();
    for (size_t i = 0; i < bufferCount; ++i) {
        std::shared_ptr<C2GraphicBlock> block;
        C2MemoryUsage usage = {C2MemoryUsage::CPU_READ, 0};
        err = mOutputBlockPool->fetchGraphicBlock(size.width(), size.height(), pixelFormat, usage,
                                                  &block);
        if (err != C2_OK) {
            mGraphicBlocks.clear();
            ALOGE("failed to allocate buffer: %d", err);
            reportError(err);
            return err;
        }
        appendOutputBuffer(std::move(block));
    }
    mOutputFormat.mMinNumBuffers = bufferCount;
    return C2_OK;
}

void C2VDAComponent::appendOutputBuffer(std::shared_ptr<C2GraphicBlock> block) {
    GraphicBlockInfo info;
    info.mBlockId = static_cast<int32_t>(mGraphicBlocks.size());
    info.mGraphicBlock = std::move(block);

    C2ConstGraphicBlock constBlock = info.mGraphicBlock->share(
            C2Rect(info.mGraphicBlock->width(), info.mGraphicBlock->height()), C2Fence());

    const C2GraphicView& view = constBlock.map().get();
    const uint8_t* const* data = view.data();
    CHECK_NE(data, nullptr);
    const C2PlanarLayout& layout = view.layout();

    ALOGV("allocate graphic buffer: %p, id: %d, size: %dx%d", info.mGraphicBlock->handle(),
          info.mBlockId, info.mGraphicBlock->width(), info.mGraphicBlock->height());

    // get offset from data pointers
    uint32_t offsets[C2PlanarLayout::MAX_NUM_PLANES];
    auto baseAddress = reinterpret_cast<intptr_t>(data[0]);
    for (uint32_t i = 0; i < layout.numPlanes; ++i) {
        auto planeAddress = reinterpret_cast<intptr_t>(data[i]);
        offsets[i] = static_cast<uint32_t>(planeAddress - baseAddress);
    }

    bool crcb = false;
    if (layout.numPlanes == 3 &&
        offsets[C2PlanarLayout::PLANE_U] > offsets[C2PlanarLayout::PLANE_V]) {
        // YCrCb format
        std::swap(offsets[C2PlanarLayout::PLANE_U], offsets[C2PlanarLayout::PLANE_V]);
        crcb = true;
    }

    bool semiplanar = false;
    uint32_t passedNumPlanes = layout.numPlanes;
    if (layout.planes[C2PlanarLayout::PLANE_U].colInc == 2) {  // chroma_step
        // Semi-planar format
        passedNumPlanes--;
        semiplanar = true;
    }

    for (uint32_t i = 0; i < passedNumPlanes; ++i) {
        ALOGV("plane %u: stride: %d, offset: %u", i, layout.planes[i].rowInc, offsets[i]);
    }
#ifdef V4L2_CODEC2_ARC
    info.mPixelFormat = arc::C2VDAAdaptorProxy::ResolveBufferFormat(crcb, semiplanar);
#else
    info.mPixelFormat = C2VDAAdaptor::ResolveBufferFormat(crcb, semiplanar);
#endif
    ALOGV("HAL pixel format: 0x%x", static_cast<uint32_t>(info.mPixelFormat));

    base::ScopedFD passedHandle(dup(info.mGraphicBlock->handle()->data[0]));
    if (!passedHandle.is_valid()) {
        ALOGE("Failed to dup(%d), errno=%d", info.mGraphicBlock->handle()->data[0], errno);
        reportError(C2_CORRUPTED);
        return;
    }
    std::vector<VideoFramePlane> passedPlanes;
    for (uint32_t i = 0; i < passedNumPlanes; ++i) {
        CHECK_GT(layout.planes[i].rowInc, 0);
        passedPlanes.push_back({offsets[i], static_cast<uint32_t>(layout.planes[i].rowInc)});
    }
    info.mHandle = std::move(passedHandle);
    info.mPlanes = std::move(passedPlanes);

    mGraphicBlocks.push_back(std::move(info));
}

void C2VDAComponent::sendOutputBufferToAccelerator(GraphicBlockInfo* info) {
    ALOGV("sendOutputBufferToAccelerator index=%d", info->mBlockId);
    CHECK_EQ(info->mState, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
    info->mState = GraphicBlockInfo::State::OWNED_BY_ACCELERATOR;

    // is_valid() is true for the first time the buffer is passed to VDA. In that case, VDA needs to
    // import the buffer first.
    if (info->mHandle.is_valid()) {
        mVDAAdaptor->importBufferForPicture(info->mBlockId, info->mPixelFormat,
                                            info->mHandle.release(), info->mPlanes);
    } else {
        mVDAAdaptor->reusePictureBuffer(info->mBlockId);
    }
}

void C2VDAComponent::onVisibleRectChanged(const media::Rect& cropRect) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onVisibleRectChanged");
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    // We should make sure there is no pending output format change. That is, the input cropRect is
    // corresponding to current output format.
    CHECK(mPendingOutputFormat == nullptr);
    setOutputFormatCrop(cropRect);
}

void C2VDAComponent::setOutputFormatCrop(const media::Rect& cropRect) {
    ALOGV("setOutputFormatCrop(%dx%d)", cropRect.width(), cropRect.height());
    mOutputFormat.mVisibleRect = cropRect;
    // TODO(johnylin): what else do we need to do? crop rect could be an info requested from
    // framework by requestedInfos in worklets.
}

c2_status_t C2VDAComponent::queue_nb(std::list<std::unique_ptr<C2Work>>* const items) {
    if (mState.load() != State::RUNNING) {
        return C2_BAD_STATE;
    }
    while (!items->empty()) {
        mTaskRunner->PostTask(FROM_HERE,
                              base::Bind(&C2VDAComponent::onQueueWork, base::Unretained(this),
                                         base::Passed(&items->front())));
        items->pop_front();
    }
    return C2_OK;
}

c2_status_t C2VDAComponent::announce_nb(const std::vector<C2WorkOutline>& items) {
    UNUSED(items);
    return C2_OMITTED;  // Tunneling is not supported by now
}

c2_status_t C2VDAComponent::flush_sm(flush_mode_t mode,
                                     std::list<std::unique_ptr<C2Work>>* const flushedWork) {
    if (mode != FLUSH_COMPONENT) {
        return C2_OMITTED;  // Tunneling is not supported by now
    }
    if (mState.load() != State::RUNNING) {
        return C2_BAD_STATE;
    }
    mTaskRunner->PostTask(FROM_HERE, base::Bind(&C2VDAComponent::onFlush, base::Unretained(this)));
    // Instead of |flushedWork|, abandoned works will be returned via onWorkDone_nb() callback.
    return C2_OK;
}

c2_status_t C2VDAComponent::drain_nb(drain_mode_t mode) {
    if (mode != DRAIN_COMPONENT_WITH_EOS && mode != DRAIN_COMPONENT_NO_EOS) {
        return C2_OMITTED;  // Tunneling is not supported by now
    }
    if (mState.load() != State::RUNNING) {
        return C2_BAD_STATE;
    }
    mTaskRunner->PostTask(FROM_HERE, base::Bind(&C2VDAComponent::onDrain, base::Unretained(this),
                                                static_cast<uint32_t>(mode)));
    return C2_OK;
}

c2_status_t C2VDAComponent::start() {
    // Use mStartStopLock to block other asynchronously start/stop calls.
    std::lock_guard<std::mutex> lock(mStartStopLock);

    if (mState.load() != State::LOADED) {
        return C2_BAD_STATE;  // start() is only supported when component is in LOADED state.
    }

    fetchParametersFromIntf();
    base::WaitableEvent done(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                             base::WaitableEvent::InitialState::NOT_SIGNALED);
    mTaskRunner->PostTask(FROM_HERE, base::Bind(&C2VDAComponent::onStart, base::Unretained(this),
                                                mCodecProfile, &done));
    done.Wait();
    if (mVDAInitResult != VideoDecodeAcceleratorAdaptor::Result::SUCCESS) {
        ALOGE("Failed to start component due to VDA error: %d", static_cast<int>(mVDAInitResult));
        return C2_CORRUPTED;
    }
    mState.store(State::RUNNING);
    return C2_OK;
}

c2_status_t C2VDAComponent::stop() {
    // Use mStartStopLock to block other asynchronously start/stop calls.
    std::lock_guard<std::mutex> lock(mStartStopLock);

    auto state = mState.load();
    if (!(state == State::RUNNING || state == State::ERROR)) {
        return C2_OK;  // Component is already in stopped state.
    }

    base::WaitableEvent done(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                             base::WaitableEvent::InitialState::NOT_SIGNALED);
    mTaskRunner->PostTask(FROM_HERE,
                          base::Bind(&C2VDAComponent::onStop, base::Unretained(this), &done));
    done.Wait();
    mState.store(State::LOADED);
    return C2_OK;
}

c2_status_t C2VDAComponent::reset() {
    return stop();
    // TODO(johnylin): reset is different than stop that it could be called in any state.
    // TODO(johnylin): when reset is called, set ComponentInterface to default values.
}

c2_status_t C2VDAComponent::release() {
    return reset();
}

std::shared_ptr<C2ComponentInterface> C2VDAComponent::intf() {
    return mIntf;
}

void C2VDAComponent::providePictureBuffers(uint32_t minNumBuffers, const media::Size& codedSize) {
    // Always use fexible pixel 420 format YCbCr_420_888 in Android.
    // Uses coded size for crop rect while it is not available.
    auto format = std::make_unique<VideoFormat>(HalPixelFormat::YCbCr_420_888, minNumBuffers,
                                                codedSize, media::Rect(codedSize));

    // Set mRequestedVisibleRect to default.
    mRequestedVisibleRect = media::Rect();

    mTaskRunner->PostTask(FROM_HERE, base::Bind(&C2VDAComponent::onOutputFormatChanged,
                                                base::Unretained(this), base::Passed(&format)));
}

void C2VDAComponent::dismissPictureBuffer(int32_t pictureBufferId) {
    UNUSED(pictureBufferId);
    // no ops
}

void C2VDAComponent::pictureReady(int32_t pictureBufferId, int32_t bitstreamId,
                                  const media::Rect& cropRect) {
    UNUSED(pictureBufferId);
    UNUSED(bitstreamId);

    if (mRequestedVisibleRect != cropRect) {
        mRequestedVisibleRect = cropRect;
        mTaskRunner->PostTask(FROM_HERE, base::Bind(&C2VDAComponent::onVisibleRectChanged,
                                                    base::Unretained(this), cropRect));
    }

    mTaskRunner->PostTask(FROM_HERE,
                          base::Bind(&C2VDAComponent::onOutputBufferDone, base::Unretained(this),
                                     pictureBufferId, bitstreamId));
}

void C2VDAComponent::notifyEndOfBitstreamBuffer(int32_t bitstreamId) {
    mTaskRunner->PostTask(FROM_HERE, base::Bind(&C2VDAComponent::onInputBufferDone,
                                                base::Unretained(this), bitstreamId));
}

void C2VDAComponent::notifyFlushDone() {
    mTaskRunner->PostTask(FROM_HERE,
                          base::Bind(&C2VDAComponent::onDrainDone, base::Unretained(this)));
}

void C2VDAComponent::notifyResetDone() {
    mTaskRunner->PostTask(FROM_HERE,
                          base::Bind(&C2VDAComponent::onResetDone, base::Unretained(this)));
}

void C2VDAComponent::notifyError(VideoDecodeAcceleratorAdaptor::Result error) {
    ALOGE("Got notifyError from VDA error=%d", error);
    c2_status_t err;
    switch (error) {
    case VideoDecodeAcceleratorAdaptor::Result::ILLEGAL_STATE:
        err = C2_BAD_STATE;
        break;
    case VideoDecodeAcceleratorAdaptor::Result::INVALID_ARGUMENT:
    case VideoDecodeAcceleratorAdaptor::Result::UNREADABLE_INPUT:
        err = C2_BAD_VALUE;
        break;
    case VideoDecodeAcceleratorAdaptor::Result::PLATFORM_FAILURE:
        err = C2_CORRUPTED;
        break;
    case VideoDecodeAcceleratorAdaptor::Result::INSUFFICIENT_RESOURCES:
        err = C2_NO_MEMORY;
        break;
    case VideoDecodeAcceleratorAdaptor::Result::SUCCESS:
        ALOGE("Shouldn't get SUCCESS err code in NotifyError(). Skip it...");
        return;
    }
    reportError(err);
}

void C2VDAComponent::reportFinishedWorkIfAny() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    std::list<std::unique_ptr<C2Work>> finishedWorks;

    // Work should be reported as done if both input and output buffer are returned by VDA.

    // Note that not every input buffer has matched output (ex. CSD header for H.264).
    // However, the timestamp is guaranteed to be monotonic increasing for buffers in display order.
    // That is, since VDA output is in display order, if we get a returned output with timestamp T,
    // it implies all works with timestamp <= T are done.
    // EOS work will not be reported here. reportEOSWork() does it.
    auto iter = mPendingWorks.begin();
    while (iter != mPendingWorks.end()) {
        if (isWorkDone(iter->get())) {
            iter->get()->result = C2_OK;
            iter->get()->workletsProcessed = static_cast<uint32_t>(iter->get()->worklets.size());
            finishedWorks.emplace_back(std::move(*iter));
            iter = mPendingWorks.erase(iter);
        } else {
            ++iter;
        }
    }

    if (!finishedWorks.empty()) {
        mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
    }
}

bool C2VDAComponent::isWorkDone(const C2Work* work) const {
    if (!work->input.buffers.empty()) {
        // Input buffer is still owned by VDA.
        // This condition could also recognize dummy EOS work since it won't get
        // onInputBufferDone(), input.buffers won't be cleared until reportEOSWork().
        return false;
    }
    if (mComponentState == ComponentState::DRAINING && mDrainWithEOS &&
        mPendingWorks.size() == 1u) {
        // If component is in DRAINING state and mDrainWithEOS is true. The last returned work
        // should be marked EOS flag and returned by reportEOSWork() instead.
        return false;
    }
    if (mLastOutputTimestamp < 0) {
        return false;  // No output buffer is returned yet.
    }
    if (work->input.ordinal.timestamp > static_cast<uint64_t>(mLastOutputTimestamp)) {
        return false;  // Output buffer is not returned by VDA yet.
    }
    return true;  // Output buffer is returned, or it has no related output buffer.
}

void C2VDAComponent::reportEOSWork() {
    ALOGV("reportEOSWork");
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    // In this moment all works prior to EOS work should be done and returned to listener.
    if (mPendingWorks.size() != 1u) {  // only EOS work left
        ALOGE("It shouldn't have remaining works in mPendingWorks except EOS work.");
        reportError(C2_CORRUPTED);
        return;
    }

    std::unique_ptr<C2Work> eosWork(std::move(mPendingWorks.front()));
    mPendingWorks.pop_front();
    eosWork->input.buffers.clear();
    eosWork->result = C2_OK;
    eosWork->workletsProcessed = static_cast<uint32_t>(eosWork->worklets.size());
    eosWork->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;

    std::list<std::unique_ptr<C2Work>> finishedWorks;
    finishedWorks.emplace_back(std::move(eosWork));
    mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
}

void C2VDAComponent::reportAbandonedWorks() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    std::list<std::unique_ptr<C2Work>> abandonedWorks;

    while (!mPendingWorks.empty()) {
        std::unique_ptr<C2Work> work(std::move(mPendingWorks.front()));
        mPendingWorks.pop_front();

        // TODO: correlate the definition of flushed work result to framework.
        work->result = C2_NOT_FOUND;
        // When the work is abandoned, the input buffers vector shall be cleared by component.
        work->input.buffers.clear();
        abandonedWorks.emplace_back(std::move(work));
    }

    if (!abandonedWorks.empty()) {
        mListener->onWorkDone_nb(shared_from_this(), std::move(abandonedWorks));
    }
}

void C2VDAComponent::reportError(c2_status_t error) {
    uint32_t reported_error = -error;
    // Why onError_nb takes uint32_t while c2_status_t is mostly negative numbers?
    mListener->onError_nb(shared_from_this(), reported_error);
}

class C2VDAComponentFactory : public C2ComponentFactory {
public:
    C2VDAComponentFactory(C2String decoderName) : mDecoderName(decoderName){};

    c2_status_t createComponent(c2_node_id_t id, std::shared_ptr<C2Component>* const component,
                                ComponentDeleter deleter) override {
        UNUSED(deleter);
        *component = std::shared_ptr<C2Component>(new C2VDAComponent(mDecoderName, id));
        return C2_OK;
    }
    c2_status_t createInterface(c2_node_id_t id,
                                std::shared_ptr<C2ComponentInterface>* const interface,
                                InterfaceDeleter deleter) override {
        UNUSED(deleter);
        *interface =
                std::shared_ptr<C2ComponentInterface>(new C2VDAComponentIntf(mDecoderName, id));
        return C2_OK;
    }
    ~C2VDAComponentFactory() override = default;

private:
    const C2String mDecoderName;
};
}  // namespace android

extern "C" ::C2ComponentFactory* CreateC2VDAH264Factory() {
    ALOGV("in %s", __func__);
    return new ::android::C2VDAComponentFactory(android::kH264DecoderName);
}

extern "C" void DestroyC2VDAH264Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}

extern "C" ::C2ComponentFactory* CreateC2VDAVP8Factory() {
    ALOGV("in %s", __func__);
    return new ::android::C2VDAComponentFactory(android::kVP8DecoderName);
}

extern "C" void DestroyC2VDAVP8Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}

extern "C" ::C2ComponentFactory* CreateC2VDAVP9Factory() {
    ALOGV("in %s", __func__);
    return new ::android::C2VDAComponentFactory(android::kVP9DecoderName);
}

extern "C" void DestroyC2VDAVP9Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}
