// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VDAComponent"

#include <algorithm>

#include "C2VDAAdaptor.h"
#define __C2_GENERATE_GLOBAL_VARS__
#include "C2VDAComponent.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "videodev2.h"

#include <media/stagefright/MediaDefs.h>
#include <utils/Log.h>
#include <utils/misc.h>

#define UNUSED(expr) do { (void)(expr); } while (0)

namespace android {

namespace {

// Get index from C2param object. Use index to identify the type of the parameter.
// Currently there is no wise way to get index from a parameter because index is private.
uint32_t restoreIndex(const C2Param* param) {
    return (param->forStream() ? (0x02000000 | ((param->stream() << 17) & 0x01FE0000)) : 0)
            | param->type();
}


// Helper function to allocate string type parameters.
template <class T>
std::unique_ptr<T> alloc_unique_cstr(const char* cstr) {
    size_t len = strlen(cstr);
    std::unique_ptr<T> ptr = T::alloc_unique(len);
    memcpy(ptr->m.mValue, cstr, len);
    return ptr;
}

template <class T>
std::unique_ptr<C2SettingResult> reportReadOnlyFailure(C2Param* c2Param) {
    T* param = (T*)c2Param;
    return std::unique_ptr<C2SettingResult>(
            new C2SettingResult { C2SettingResult::READ_ONLY,
                                  { C2ParamField(param, &T::mValue), nullptr /* supportedValues */ },
                                  {} /* conflictedFields */ });
}

template <class T>
std::unique_ptr<C2SettingResult> reportReadOnlyFlexFailure(C2Param* c2Param) {
    T* param = (T*)c2Param;
    return std::unique_ptr<C2SettingResult>(
            new C2SettingResult { C2SettingResult::READ_ONLY,
                                  { C2ParamField(param, &T::m), nullptr /* supportedValues */ },
                                  {} /* conflictedFields */ });
}

// Helper function to find int32_t value from C2Value::Primitive vector.
bool findInt32FromPrimitiveValues(const int32_t& v, const C2FieldSupportedValues& values) {
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
    for (const auto& value : values.values) {
        if (value.i32 == v) {
            return true;
        }
    }
    return false;
}

// Helper function to find uint32_t value from C2Value::Primitive vector.
bool findUint32FromPrimitiveValues(const uint32_t& v, const C2FieldSupportedValues& values) {
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
    for (const auto& value : values.values) {
        if (value.u32 == v) {
            return true;
        }
    }
    return false;
}

}  // namespace


// Neglect flexible flag while matching parameter indices.
#define CASE(paramType) \
    case paramType::coreIndex: \
        return std::unique_ptr<C2StructDescriptor>(new C2StructDescriptor{ \
                paramType::coreIndex, paramType::fieldList, })

class C2VDAComponentStore::ParamReflector : public C2ParamReflector {
public:
    virtual std::unique_ptr<C2StructDescriptor> describe(C2Param::BaseIndex coreIndex) override {
        switch (coreIndex.coreIndex()) {
            //CASE(C2ComponentDomainInfo);  //TODO: known codec2 framework bug
            CASE(C2StreamFormatConfig);
            CASE(C2VideoSizeStreamInfo);
            CASE(C2PortMimeConfig);
            CASE(C2MaxVideoSizeHintPortSetting);
        }
        return nullptr;
    }
};

#undef CASE

// static
const uint32_t C2VDAComponentIntf::kInputFormatFourcc = V4L2_PIX_FMT_H264_SLICE;

C2VDAComponentIntf::C2VDAComponentIntf(C2String name, c2_node_id_t id)
    : kName(name),
      kId(id),
      mDomainInfo(C2DomainVideo),
      mOutputColorFormat(0u, kColorFormatYUV420Flexible),
      // Support H264 only for now.
      mInputPortMime(alloc_unique_cstr<C2PortMimeConfig::input>(MEDIA_MIMETYPE_VIDEO_AVC)),
      mOutputPortMime(alloc_unique_cstr<C2PortMimeConfig::output>(MEDIA_MIMETYPE_VIDEO_RAW)),
      mSupportedProfiles(C2VDAAdaptor::GetSupportedProfiles(kInputFormatFourcc)) {
    // Get supported profiles from VDA.
    CHECK_GT(mSupportedProfiles.size(), 0u);

    // Set default codec profile.
    mInputCodecProfile.mValue = mSupportedProfiles[0].profile;

    mMinVideoSize = mSupportedProfiles[0].min_resolution;
    mMaxVideoSize = mSupportedProfiles[0].max_resolution;
    // Set default output video size.
    mVideoSize.mWidth = mMinVideoSize.width();
    mVideoSize.mHeight = mMinVideoSize.height();
    // Set default max video size.
    mMaxVideoSizeHint.mWidth = mMaxVideoSize.width();
    mMaxVideoSizeHint.mHeight = mMaxVideoSize.height();

    for (const auto& supportedProfile : mSupportedProfiles) {
        mSupportedCodecProfiles.push_back(supportedProfile.profile);
        ALOGI("Get supported profile: profile=%d, min_res=%s, max_res=%s",
              supportedProfile.profile, supportedProfile.min_resolution.ToString().c_str(),
              supportedProfile.max_resolution.ToString().c_str());
    }

    auto insertParam = [&params = mParams] (C2Param* param) {
        params[restoreIndex(param)] = param;
    };

    insertParam(&mDomainInfo);
    insertParam(&mOutputColorFormat);
    insertParam(mInputPortMime.get());
    insertParam(mOutputPortMime.get());

    insertParam(&mInputCodecProfile);
    // TODO(johnylin): C2FieldSupportedValues for mInputCodecProfile should be VALUES type. But
    // right now VALUES type only accepts initializer_list argument, we cannot pass a vector.
    mSupportedValues.emplace(
            C2ParamField(&mInputCodecProfile, &C2StreamFormatConfig::mValue),
            C2FieldSupportedValues(
                    mSupportedCodecProfiles.front(), mSupportedCodecProfiles.back(), 1u));

    // TODO(johnylin): min/max resolution may change by chosen profile, we should dynamically change
    // the supported values in the future.
    insertParam(&mVideoSize);
    mSupportedValues.emplace(
            C2ParamField(&mVideoSize, &C2VideoSizeStreamInfo::mWidth),
            C2FieldSupportedValues(mMinVideoSize.width(), mMaxVideoSize.width(), 16));
    mSupportedValues.emplace(
            C2ParamField(&mVideoSize, &C2VideoSizeStreamInfo::mHeight),
            C2FieldSupportedValues(mMinVideoSize.height(), mMaxVideoSize.height(), 16));

    insertParam(&mMaxVideoSizeHint);
    mSupportedValues.emplace(
            C2ParamField(&mMaxVideoSizeHint, &C2MaxVideoSizeHintPortSetting::mWidth),
            C2FieldSupportedValues(mMinVideoSize.width(), mMaxVideoSize.width(), 16));
    mSupportedValues.emplace(
            C2ParamField(&mMaxVideoSizeHint, &C2MaxVideoSizeHintPortSetting::mHeight),
            C2FieldSupportedValues(mMinVideoSize.height(), mMaxVideoSize.height(), 16));

    mParamDescs.push_back(std::make_shared<C2ParamDescriptor>(
            true, "_domain", &mDomainInfo));
    mParamDescs.push_back(std::make_shared<C2ParamDescriptor>(
            false, "_output_color_format", &mOutputColorFormat));
    mParamDescs.push_back(std::make_shared<C2ParamDescriptor>(
            true, "_input_port_mime", mInputPortMime.get()));
    mParamDescs.push_back(std::make_shared<C2ParamDescriptor>(
            true, "_output_port_mime", mOutputPortMime.get()));
    mParamDescs.push_back(std::make_shared<C2ParamDescriptor>(
            false, "_input_codec_profile", &mInputCodecProfile));
    mParamDescs.push_back(std::make_shared<C2ParamDescriptor>(
            false, "_video_size", &mVideoSize));
    mParamDescs.push_back(std::make_shared<C2ParamDescriptor>(
            false, "_max_video_size_hint", &mMaxVideoSizeHint));
}

C2String C2VDAComponentIntf::getName() const {
    return kName;
}

c2_node_id_t C2VDAComponentIntf::getId() const {
    return kId;
}

c2_status_t C2VDAComponentIntf::query_nb(
        const std::vector<C2Param* const>& stackParams,
        const std::vector<C2Param::Index>& heapParamIndices,
        std::vector<std::unique_ptr<C2Param>>* const heapParams) const {
    c2_status_t err = C2_OK;
    for (C2Param* const param : stackParams) {
        if (!param || !*param) {
            continue;
        }

        uint32_t index = restoreIndex(param);
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

c2_status_t C2VDAComponentIntf::config_nb(
        const std::vector<C2Param* const> &params,
        std::vector<std::unique_ptr<C2SettingResult>>* const failures) {
    c2_status_t err = C2_OK;
    for (C2Param* const param : params) {
        uint32_t index = restoreIndex(param);
        C2Param* myParam = getParamByIndex(index);
        if (!myParam) {
            // C2_BAD_INDEX should be the lowest priority except for C2_OK.
            err = (err == C2_OK) ? C2_BAD_INDEX : err;
            continue;
        }

        if (index == restoreIndex(&mDomainInfo)) {  // read-only
            failures->push_back(reportReadOnlyFailure<decltype(mDomainInfo)>(param));
            err = C2_BAD_VALUE;
            continue;
        } else if (index == restoreIndex(&mOutputColorFormat)) {  // read-only
            failures->push_back(reportReadOnlyFailure<decltype(mOutputColorFormat)>(param));
            err = C2_BAD_VALUE;
            continue;
        } else if (index == restoreIndex(mInputPortMime.get())) {  // read-only
            failures->push_back(
                    reportReadOnlyFlexFailure<std::remove_pointer<decltype(mInputPortMime.get())>::type>(param));
            err = C2_BAD_VALUE;
            continue;
        } else if (index == restoreIndex(mOutputPortMime.get())) {  // read-only
            failures->push_back(
                    reportReadOnlyFlexFailure<std::remove_pointer<decltype(mOutputPortMime.get())>::type>(param));
            err = C2_BAD_VALUE;
            continue;
        } else if (index == restoreIndex(&mInputCodecProfile)) {
            std::unique_ptr<C2SettingResult> result = validateUint32Config<decltype(mInputCodecProfile)>(param);
            if (result) {
                failures->push_back(std::move(result));
                err = C2_BAD_VALUE;
                continue;
            }
        } else if (index == restoreIndex(&mVideoSize)) {
            std::unique_ptr<C2SettingResult> result = validateVideoSizeConfig<decltype(mVideoSize)>(param);
            if (result) {
                failures->push_back(std::move(result));
                err = C2_BAD_VALUE;
                continue;
            }
        } else if (index == restoreIndex(&mMaxVideoSizeHint)) {
            std::unique_ptr<C2SettingResult> result = validateVideoSizeConfig<decltype(mMaxVideoSizeHint)>(param);
            if (result) {
                failures->push_back(std::move(result));
                err = C2_BAD_VALUE;
                continue;
            }
        }
        myParam->updateFrom(*param);
    }
    return err;
}

c2_status_t C2VDAComponentIntf::commit_sm(
        const std::vector<C2Param* const>& params,
        std::vector<std::unique_ptr<C2SettingResult>>* const failures) {
    UNUSED(params);
    UNUSED(failures);
    return C2_OMITTED;
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
    params->insert(params->begin(), mParamDescs.begin(), mParamDescs.end());
    return C2_OK;
}

c2_status_t C2VDAComponentIntf::querySupportedValues_nb(
        std::vector<C2FieldSupportedValuesQuery>& fields) const {
    c2_status_t err = C2_OK;
    for (auto& query : fields) {
        if (mSupportedValues.count(query.field) == 0) {
            query.status = C2_BAD_INDEX;
            err = C2_BAD_INDEX;
            continue;
        }
        query.status = C2_OK;
        query.values = mSupportedValues.at(query.field);
    }
    return err;
}

C2Param* C2VDAComponentIntf::getParamByIndex(uint32_t index) const {
    auto iter = mParams.find(index);
    return (iter != mParams.end()) ? iter->second : nullptr;
}

template<class T>
std::unique_ptr<C2SettingResult> C2VDAComponentIntf::validateVideoSizeConfig(
        C2Param* c2Param) const {
    T* videoSize = (T*)c2Param;

    C2ParamField fieldWidth(videoSize, &T::mWidth);
    const C2FieldSupportedValues &widths = mSupportedValues.at(fieldWidth);
    CHECK_EQ(widths.type, C2FieldSupportedValues::RANGE);
    if (!findInt32FromPrimitiveValues(videoSize->mWidth, widths)) {
        std::unique_ptr<C2SettingResult> result(
                new C2SettingResult { C2SettingResult::BAD_VALUE,
                                      { fieldWidth, nullptr /* supportedValues */ },
                                      {} /* conflictinfFields */ });
        result->field.values.reset(
                new C2FieldSupportedValues(widths.range.min,
                                           widths.range.max,
                                           widths.range.step));
        return result;
    }

    C2ParamField fieldHeight(videoSize, &T::mHeight);
    const C2FieldSupportedValues &heights = mSupportedValues.at(fieldHeight);
    CHECK_EQ(heights.type, C2FieldSupportedValues::RANGE);
    if (!findInt32FromPrimitiveValues(videoSize->mHeight, heights)) {
        std::unique_ptr<C2SettingResult> result(
                new C2SettingResult { C2SettingResult::BAD_VALUE,
                                      { fieldHeight, nullptr /* supportedValues */ },
                                      {} /* conflictinfFields */ });
        result->field.values.reset(
                new C2FieldSupportedValues(heights.range.min,
                                           heights.range.max,
                                           heights.range.step));
        return result;
    }

    return nullptr;
}

template<class T>
std::unique_ptr<C2SettingResult> C2VDAComponentIntf::validateUint32Config(
        C2Param* c2Param) const {
    T* config = (T*)c2Param;

    C2ParamField field(config, &T::mValue);
    const C2FieldSupportedValues &configs = mSupportedValues.at(field);
    CHECK_EQ(configs.type, C2FieldSupportedValues::RANGE);
    if (!findUint32FromPrimitiveValues(config->mValue, configs)) {
        std::unique_ptr<C2SettingResult> result(
                new C2SettingResult { C2SettingResult::BAD_VALUE,
                                      { field, nullptr /* supportedValues */ },
                                      {} /* conflictinfFields */ });
        result->field.values.reset(
                new C2FieldSupportedValues(configs.range.min,
                                           configs.range.max,
                                           configs.range.step));
        return result;
    }

    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////

C2VDAComponent::C2VDAComponent(C2String name,
                               c2_node_id_t id)
    : mIntf(std::make_shared<C2VDAComponentIntf>(name, id)),
      mThread("C2VDAComponentThread"),
      mVDAInitResult(VideoDecodeAcceleratorAdaptor::Result::ILLEGAL_STATE),
      mComponentState(ComponentState::UNINITIALIZED),
      mCodecProfile(media::VIDEO_CODEC_PROFILE_UNKNOWN),
      mState(State::UNLOADED) {
    if (!mThread.Start()) {
        ALOGE("Component thread failed to start");
        return;
    }
    mTaskRunner = mThread.task_runner();
    mTaskRunner->PostTask(FROM_HERE, base::Bind(&C2VDAComponent::onCreate, base::Unretained(this)));
    mState = State::LOADED;
}

C2VDAComponent::~C2VDAComponent() {
    CHECK_EQ(mState, State::LOADED);

    if (mThread.IsRunning()) {
        mTaskRunner->PostTask(
                FROM_HERE, base::Bind(&C2VDAComponent::onDestroy, base::Unretained(this)));
        mThread.Stop();
    }
    CHECK(!mThread.IsRunning());
}

void C2VDAComponent::getParameters() {
    C2StreamFormatConfig::input codecProfile;
    std::vector<C2Param* const> stackParams{ &codecProfile };
    CHECK_EQ(mIntf->query_nb(stackParams, {}, nullptr), C2_OK);
    // The value should be guaranteed to be within media::VideoCodecProfile enum range by component
    // interface.
    mCodecProfile = static_cast<media::VideoCodecProfile>(codecProfile.mValue);
    ALOGI("get parameter: mCodecProfile = %d", static_cast<int>(mCodecProfile));
}

void C2VDAComponent::onCreate() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onCreate");
    mVDAAdaptor.reset(new C2VDAAdaptor());
}

void C2VDAComponent::onDestroy() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDestroy");
    mVDAAdaptor.reset(nullptr);
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

void C2VDAComponent::onStop(base::WaitableEvent* done) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onStop");
    CHECK(mComponentState != ComponentState::UNINITIALIZED);
    mVDAAdaptor->reset();
    mStopDoneEvent = done;  // restore done event which shoud be signaled in onStopDone().
    mComponentState = ComponentState::STOPPING;
}

void C2VDAComponent::onStopDone() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onStopDone");
    CHECK(mComponentState == ComponentState::STOPPING);
    CHECK(mStopDoneEvent);
    mVDAAdaptor->destroy();
    mStopDoneEvent->Signal();
    mStopDoneEvent = nullptr;
    mComponentState = ComponentState::UNINITIALIZED;
}

c2_status_t C2VDAComponent::setListener_sm(const std::shared_ptr<C2Component::Listener> &listener) {
    mListener = listener;
    return C2_OMITTED;
}

c2_status_t C2VDAComponent::queue_nb(std::list<std::unique_ptr<C2Work>>* const items) {
    UNUSED(items);
    return C2_OMITTED;
}

c2_status_t C2VDAComponent::announce_nb(const std::vector<C2WorkOutline>& items) {
    UNUSED(items);
    return C2_OMITTED;  // Tunneling is not supported by now
}

c2_status_t C2VDAComponent::flush_sm(
        flush_mode_t mode, std::list<std::unique_ptr<C2Work>>* const flushedWork) {
    if (mode != FLUSH_COMPONENT)
        return C2_OMITTED;  // Tunneling is not supported by now
    UNUSED(flushedWork);
    return C2_OMITTED;
}

c2_status_t C2VDAComponent::drain_nb(drain_mode_t mode) {
    if (mode != DRAIN_COMPONENT)
        return C2_OMITTED;  // Tunneling is not supported by now
    return C2_OMITTED;
}

c2_status_t C2VDAComponent::start() {
    if (mState != State::LOADED) {
        return C2_BAD_STATE;  // start() is only supported when component is in LOADED state.
    }

    getParameters();
    base::WaitableEvent done(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                             base::WaitableEvent::InitialState::NOT_SIGNALED);
    mTaskRunner->PostTask(
            FROM_HERE,
            base::Bind(&C2VDAComponent::onStart, base::Unretained(this), mCodecProfile, &done));
    done.Wait();
    if (mVDAInitResult != VideoDecodeAcceleratorAdaptor::Result::SUCCESS) {
        ALOGE("Failed to start component due to VDA error: %d", static_cast<int>(mVDAInitResult));
        return C2_CORRUPTED;
    }
    mState = State::RUNNING;
    return C2_OK;
}

c2_status_t C2VDAComponent::stop() {
    if (!(mState == State::RUNNING || mState == State::ERROR)) {
        return C2_BAD_STATE;  // component is already in stopped state.
    }

    base::WaitableEvent done(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                             base::WaitableEvent::InitialState::NOT_SIGNALED);
    mTaskRunner->PostTask(FROM_HERE,
                          base::Bind(&C2VDAComponent::onStop, base::Unretained(this), &done));
    done.Wait();
    mState = State::LOADED;
    return C2_OK;
}

void C2VDAComponent::reset() {
    stop();
    // TODO(johnylin): what is the use case for calling reset() instead of stop()?
}

void C2VDAComponent::release() {
    // TODO(johnylin): what should we do for release?
}

std::shared_ptr<C2ComponentInterface> C2VDAComponent::intf() {
    return mIntf;
}

void C2VDAComponent::providePictureBuffers(
        uint32_t pixelFormat, uint32_t minNumBuffers, const media::Size& codedSize) {
    UNUSED(pixelFormat);
    UNUSED(minNumBuffers);
    UNUSED(codedSize);
}

void C2VDAComponent::dismissPictureBuffer(int32_t pictureBufferId) {
    UNUSED(pictureBufferId);
}

void C2VDAComponent::pictureReady(
        int32_t pictureBufferId, int32_t bitstreamId, const media::Rect& cropRect) {
    UNUSED(pictureBufferId);
    UNUSED(bitstreamId);
    UNUSED(cropRect);
}

void C2VDAComponent::notifyEndOfBitstreamBuffer(int32_t bitstreamId) {
    UNUSED(bitstreamId);
}

void C2VDAComponent::notifyFlushDone() {
}

void C2VDAComponent::notifyResetDone() {
    mTaskRunner->PostTask(FROM_HERE,
                          base::Bind(&C2VDAComponent::onStopDone, base::Unretained(this)));
}

void C2VDAComponent::notifyError(VideoDecodeAcceleratorAdaptor::Result error) {
    UNUSED(error);
}

////////////////////////////////////////////////////////////////////////////////
C2VDAComponentStore::C2VDAComponentStore()
    : mParamReflector(std::make_shared<ParamReflector>()) {
}

C2String C2VDAComponentStore::getName() const {
    return "android.componentStore.v4l2";
}

c2_status_t C2VDAComponentStore::createComponent(
        C2String name, std::shared_ptr<C2Component>* const component) {
    UNUSED(name);
    UNUSED(component);
    return C2_OMITTED;
}

c2_status_t C2VDAComponentStore::createInterface(
        C2String name, std::shared_ptr<C2ComponentInterface>* const interface) {
    interface->reset(new C2VDAComponentIntf(name, 12345));
    return C2_OK;
}

std::vector<std::shared_ptr<const C2Component::Traits>>
C2VDAComponentStore::listComponents_sm() const {
    return std::vector<std::shared_ptr<const C2Component::Traits>>();
}

c2_status_t C2VDAComponentStore::copyBuffer(
        std::shared_ptr<C2GraphicBuffer> src, std::shared_ptr<C2GraphicBuffer> dst) {
    UNUSED(src);
    UNUSED(dst);
    return C2_OMITTED;
}

std::shared_ptr<C2ParamReflector> C2VDAComponentStore::getParamReflector() const {
    return mParamReflector;
}

c2_status_t C2VDAComponentStore::querySupportedParams_nb(
            std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const {
    UNUSED(params);
    return C2_OMITTED;
}

c2_status_t C2VDAComponentStore::querySupportedValues_nb(
            std::vector<C2FieldSupportedValuesQuery>& fields) const {
    UNUSED(fields);
    return C2_OMITTED;
}

c2_status_t C2VDAComponentStore::query_sm(
        const std::vector<C2Param* const>& stackParams,
        const std::vector<C2Param::Index>& heapParamIndices,
        std::vector<std::unique_ptr<C2Param>>* const heapParams) const {
    UNUSED(stackParams);
    UNUSED(heapParamIndices);
    UNUSED(heapParams);
    return C2_OMITTED;
}

c2_status_t C2VDAComponentStore::config_sm(
        const std::vector<C2Param* const> &params,
        std::vector<std::unique_ptr<C2SettingResult>>* const failures) {
    UNUSED(params);
    UNUSED(failures);
    return C2_OMITTED;
}

c2_status_t C2VDAComponentStore::commit_sm(
        const std::vector<C2Param* const> &params,
        std::vector<std::unique_ptr<C2SettingResult>>* const failures) {
    UNUSED(params);
    UNUSED(failures);
    return C2_OMITTED;
}

}  // namespace android

// ---------------------- Factory Functions Interface ----------------

using namespace android;

extern "C" C2ComponentStore* create_store() {
    return new C2VDAComponentStore();
}

extern "C" void destroy_store(C2ComponentStore* store) {
    delete store;
}
