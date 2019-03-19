// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VEAComponent"

#include <C2VEAAdaptorProxy.h>
#include <C2VEAComponent.h>

#include <video_codecs.h>

#include <C2ComponentFactory.h>
#include <C2PlatformSupport.h>

#include <base/bind.h>
#include <base/bind_helpers.h>

#include <media/stagefright/MediaDefs.h>
#include <utils/Log.h>

#include <inttypes.h>
#include <string.h>
#include <algorithm>
#include <string>

#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

namespace android {

namespace {

// Use basic linear block pool/allocator as default.
const C2BlockPool::local_id_t kDefaultOutputBlockPool = C2BlockPool::BASIC_LINEAR;
// The default output framerate in frames per second.
// TODO: increase to 60 fps in the future.
const float kDefaultFrameRate = 30.0;
// The default output bitrate in bits per second. Use the max bitrate of AVC Level1.0 as default.
const uint32_t kDefaultBitrate = 64000;
// The maximal output bitrate in bits per second. It's the max bitrate of AVC Level4.1.
// TODO: increase this in the future for supporting higher level/resolution encoding.
const uint32_t kMaxBitrate = 50000000;

// The frame size of 1080p video.
const uint32_t kFrameSize1080P = 1920 * 1080;
// Codec2.0 VEA-based H264 encoder name.
const C2String kH264EncoderName = "c2.vea.avc.encoder";

}  // namespace

static c2_status_t adaptorResultToC2Status(VideoEncodeAcceleratorAdaptor::Result result) {
    switch (result) {
    case VideoEncodeAcceleratorAdaptor::Result::SUCCESS:
        return C2_OK;
    case VideoEncodeAcceleratorAdaptor::Result::ILLEGAL_STATE:
        ALOGE("Got error: ILLEGAL_STATE");
        return C2_BAD_STATE;
    case VideoEncodeAcceleratorAdaptor::Result::INVALID_ARGUMENT:
        ALOGE("Got error: INVALID_ARGUMENT");
        return C2_BAD_VALUE;
    case VideoEncodeAcceleratorAdaptor::Result::PLATFORM_FAILURE:
        ALOGE("Got error: PLATFORM_FAILURE");
        return C2_CORRUPTED;
    default:
        ALOGE("Unrecognizable adaptor result (value = %d)...", result);
        return C2_CORRUPTED;
    }
}

static C2Config::profile_t videoCodecProfileToC2Profile(media::VideoCodecProfile profile) {
    switch (profile) {
    case media::VideoCodecProfile::H264PROFILE_BASELINE:
        return PROFILE_AVC_BASELINE;
    case media::VideoCodecProfile::H264PROFILE_MAIN:
        return PROFILE_AVC_MAIN;
    case media::VideoCodecProfile::H264PROFILE_EXTENDED:
        return PROFILE_AVC_EXTENDED;
    case media::VideoCodecProfile::H264PROFILE_HIGH:
        return PROFILE_AVC_HIGH;
    case media::VideoCodecProfile::H264PROFILE_HIGH10PROFILE:
        return PROFILE_AVC_HIGH_10;
    case media::VideoCodecProfile::H264PROFILE_HIGH422PROFILE:
        return PROFILE_AVC_HIGH_422;
    case media::VideoCodecProfile::H264PROFILE_HIGH444PREDICTIVEPROFILE:
        return PROFILE_AVC_HIGH_444_PREDICTIVE;
    case media::VideoCodecProfile::H264PROFILE_SCALABLEBASELINE:
        return PROFILE_AVC_SCALABLE_BASELINE;
    case media::VideoCodecProfile::H264PROFILE_SCALABLEHIGH:
        return PROFILE_AVC_SCALABLE_HIGH;
    case media::VideoCodecProfile::H264PROFILE_STEREOHIGH:
        return PROFILE_AVC_STEREO_HIGH;
    case media::VideoCodecProfile::H264PROFILE_MULTIVIEWHIGH:
        return PROFILE_AVC_MULTIVIEW_HIGH;
    default:
        ALOGE("Unrecognizable profile (value = %d)...", profile);
        return PROFILE_UNUSED;
    }
}

// static
C2R C2VEAComponent::IntfImpl::ProfileLevelSetter(
        bool mayBlock, C2P<C2StreamProfileLevelInfo::output>& info,
        const C2P<C2StreamPictureSizeInfo::input>& videoSize,
        const C2P<C2StreamFrameRateInfo::output>& frameRate,
        const C2P<C2StreamBitrateInfo::output>& bitrate) {
    (void)mayBlock;

    // Use at least PROFILE_AVC_MAIN as default for 1080p input video and up.
    // TODO (b/114332827): Find root cause of bad quality of Baseline encoding.
    C2Config::profile_t defaultMinProfile = PROFILE_AVC_BASELINE;
    if (videoSize.v.width * videoSize.v.height >= kFrameSize1080P) {
        defaultMinProfile = PROFILE_AVC_MAIN;
    }

    // Adopt default minimal profile instead if the requested profile is not supported, or lower
    // than the default minimal one.
    if (!info.F(info.v.profile).supportsAtAll(info.v.profile) ||
        info.v.profile < defaultMinProfile) {
        if (info.F(info.v.profile).supportsAtAll(defaultMinProfile)) {
            ALOGV("Set profile to default (%u) instead.", defaultMinProfile);
            info.set().profile = defaultMinProfile;
        } else {
            ALOGE("Unable to set either requested profile (%u) or default profile (%u).",
                  info.v.profile, defaultMinProfile);
            return C2R(C2SettingResultBuilder::BadValue(info.F(info.v.profile)));
        }
    }

    // Table A-1 in spec
    struct LevelLimits {
        C2Config::level_t level;
        float maxMBPS;   // max macroblock processing rate in macroblocks per second
        uint64_t maxFS;  // max frame size in macroblocks
        uint32_t maxBR;  // max video bitrate in bits per second
    };
    constexpr LevelLimits kLimits[] = {
            {LEVEL_AVC_1, 1485, 99, 64000},          {LEVEL_AVC_1B, 1485, 99, 128000},
            {LEVEL_AVC_1_1, 3000, 396, 192000},      {LEVEL_AVC_1_2, 6000, 396, 384000},
            {LEVEL_AVC_1_3, 11880, 396, 768000},     {LEVEL_AVC_2, 11880, 396, 2000000},
            {LEVEL_AVC_2_1, 19800, 792, 4000000},    {LEVEL_AVC_2_2, 20250, 1620, 4000000},
            {LEVEL_AVC_3, 40500, 1620, 10000000},    {LEVEL_AVC_3_1, 108000, 3600, 14000000},
            {LEVEL_AVC_3_2, 216000, 5120, 20000000}, {LEVEL_AVC_4, 245760, 8192, 20000000},
            {LEVEL_AVC_4_1, 245760, 8192, 50000000}, {LEVEL_AVC_4_2, 522240, 8704, 50000000},
            {LEVEL_AVC_5, 589824, 22080, 135000000},
    };

    uint64_t targetFS =
            static_cast<uint64_t>((videoSize.v.width + 15) / 16) * ((videoSize.v.height + 15) / 16);
    float targetMBPS = static_cast<float>(targetFS) * frameRate.v.value;

    // Check if the supplied level meets the requirements. If not, update the level with the lowest
    // level meeting the requirements.

    bool found = false;
    bool needsUpdate = !info.F(info.v.level).supportsAtAll(info.v.level);
    for (const LevelLimits& limit : kLimits) {
        if (!info.F(info.v.level).supportsAtAll(limit.level)) {
            continue;
        }

        // Table A-2 in spec
        // The maximum bit rate for High Profile is 1.25 times that of the Base/Extended/Main
        // Profiles, 3 times for Hi10P, and 4 times for Hi422P/Hi444PP.
        uint32_t maxBR = limit.maxBR;
        if (info.v.profile >= PROFILE_AVC_HIGH_422) {
            maxBR *= 4;
        } else if (info.v.profile >= PROFILE_AVC_HIGH_10) {
            maxBR *= 3;
        } else if (info.v.profile >= PROFILE_AVC_HIGH) {
            maxBR = maxBR * 5.0 / 4.0;
        }

        if (targetFS <= limit.maxFS && targetMBPS <= limit.maxMBPS && bitrate.v.value <= maxBR) {
            // This is the lowest level that meets the requirements, and if
            // we haven't seen the supplied level yet, that means we don't
            // need the update.
            if (needsUpdate) {
                ALOGD("Given level %u does not cover current configuration: "
                      "adjusting to %u",
                      info.v.level, limit.level);
                info.set().level = limit.level;
            }
            found = true;
            break;
        }
        if (info.v.level <= limit.level) {
            // We break out of the loop when the lowest feasible level is found. The fact that we're
            // here means that our level doesn't meet the requirement and needs to be updated.
            needsUpdate = true;
        }
    }
    if (!found) {
        ALOGE("Unable to find proper level with current config, requested level (%u).",
              info.v.level);
        return C2R(C2SettingResultBuilder::BadValue(info.F(info.v.level)));
    }

    return C2R::Ok();
}

// static
C2R C2VEAComponent::IntfImpl::SizeSetter(bool mayBlock,
                                         C2P<C2StreamPictureSizeInfo::input>& videoSize) {
    (void)mayBlock;
    // TODO: maybe apply block limit?
    return videoSize.F(videoSize.v.width)
            .validatePossible(videoSize.v.width)
            .plus(videoSize.F(videoSize.v.height).validatePossible(videoSize.v.height));
}

// static
C2R C2VEAComponent::IntfImpl::IntraRefreshPeriodSetter(
        bool mayBlock, C2P<C2StreamIntraRefreshTuning::output>& period) {
    (void)mayBlock;
    if (period.v.period < 1) {
        period.set().mode = C2Config::INTRA_REFRESH_DISABLED;
        period.set().period = 0;
    } else {
        // Only support arbitrary mode (cyclic in our case).
        period.set().mode = C2Config::INTRA_REFRESH_ARBITRARY;
    }
    return C2R::Ok();
}

C2VEAComponent::IntfImpl::IntfImpl(C2String name, const std::shared_ptr<C2ReflectorHelper>& helper,
                                   std::unique_ptr<VideoEncodeAcceleratorAdaptor>* const adaptor)
      : C2InterfaceHelper(helper), mInitStatus(C2_OK) {
    setDerivedInstance(this);

    // Create new VEAAdaptor.
    adaptor->reset(new arc::C2VEAAdaptorProxy());

    // Query supported profiles in the beginning. Currently only profiles and max resolution are
    // taken into account.
    // TODO(johnylin): regard all other supported values from adaptor.
    std::vector<VideoEncodeProfile> supportedProfiles;
    VideoEncodeAcceleratorAdaptor::Result result =
            (*adaptor)->getSupportedProfiles(&supportedProfiles);
    if (result != VideoEncodeAcceleratorAdaptor::Result::SUCCESS) {
        ALOGE("Failed to get supported profiles from adaptor...");
        mInitStatus = adaptorResultToC2Status(result);
        return;
    }

    // Use type=unsigned int here, otherwise it will cause compile error in
    // C2F(mProfileLevel, profile).oneOf(profiles) since std::vector<C2Config::profile_t> cannot
    // convert to std::vector<unsigned int>.
    std::vector<unsigned int> profiles;
    media::Size maxSize;
    for (const auto& supportedProfile : supportedProfiles) {
        C2Config::profile_t profile = videoCodecProfileToC2Profile(supportedProfile.mProfile);
        if (profile == PROFILE_UNUSED) {
            continue;  // neglect unrecognizable profile
        }
        ALOGV("Queried c2_profile = 0x%x : max_size = %d x %d", profile,
              supportedProfile.mMaxResolution.width(), supportedProfile.mMaxResolution.height());
        profiles.push_back(static_cast<unsigned int>(profile));
        maxSize.set_width(std::max(maxSize.width(), supportedProfile.mMaxResolution.width()));
        maxSize.set_height(std::max(maxSize.height(), supportedProfile.mMaxResolution.height()));
    }
    C2Config::profile_t minProfile =
            static_cast<C2Config::profile_t>(*std::min_element(profiles.begin(), profiles.end()));

    // Special note: the order of addParameter matters if your setters are dependent on other
    //               parameters. Please make sure the dependent parameters are added prior to the
    //               one needs the setter dependency.

    addParameter(DefineParam(mInputVisibleSize, C2_PARAMKEY_STREAM_PICTURE_SIZE)
                         .withDefault(new C2VideoSizeStreamTuning::input(0u, 320, 240))
                         .withFields({
                                 C2F(mInputVisibleSize, width).inRange(2, maxSize.width(), 2),
                                 C2F(mInputVisibleSize, height).inRange(2, maxSize.height(), 2),
                         })
                         .withSetter(SizeSetter)
                         .build());

    addParameter(DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
                         .withDefault(new C2StreamFrameRateInfo::output(0u, kDefaultFrameRate))
                         // TODO: More restriction?
                         .withFields({C2F(mFrameRate, value).greaterThan(0.)})
                         .withSetter(Setter<decltype(*mFrameRate)>::StrictValueWithNoDeps)
                         .build());

    addParameter(DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
                         .withDefault(new C2StreamBitrateInfo::output(0u, kDefaultBitrate))
                         .withFields({C2F(mBitrate, value).inRange(0, kMaxBitrate)})
                         .withSetter(Setter<decltype(*mBitrate)>::StrictValueWithNoDeps)
                         .build());

    char outputMime[128];
    if (name == kH264EncoderName) {
        strcpy(outputMime, MEDIA_MIMETYPE_VIDEO_AVC);
        addParameter(
                DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                        .withDefault(new C2StreamProfileLevelInfo::output(0u, minProfile,
                                                                          C2Config::LEVEL_AVC_4_1))
                        .withFields(
                                {C2F(mProfileLevel, profile).oneOf(profiles),
                                 C2F(mProfileLevel, level)
                                         // TODO: query supported levels from adaptor.
                                         .oneOf({C2Config::LEVEL_AVC_1, C2Config::LEVEL_AVC_1B,
                                                 C2Config::LEVEL_AVC_1_1, C2Config::LEVEL_AVC_1_2,
                                                 C2Config::LEVEL_AVC_1_3, C2Config::LEVEL_AVC_2,
                                                 C2Config::LEVEL_AVC_2_1, C2Config::LEVEL_AVC_2_2,
                                                 C2Config::LEVEL_AVC_3, C2Config::LEVEL_AVC_3_1,
                                                 C2Config::LEVEL_AVC_3_2, C2Config::LEVEL_AVC_4,
                                                 C2Config::LEVEL_AVC_4_1})})
                        .withSetter(ProfileLevelSetter, mInputVisibleSize, mFrameRate, mBitrate)
                        .build());
    } else {
        // TODO(johnylin): implement VP8/VP9 encoder in the future.
        ALOGE("Unsupported component name: %s", name.c_str());
        mInitStatus = C2_BAD_VALUE;
        return;
    }

    addParameter(DefineParam(mInputFormat, C2_PARAMKEY_INPUT_STREAM_BUFFER_TYPE)
                         .withConstValue(new C2StreamFormatConfig::input(0u, C2FormatVideo))
                         .build());

    addParameter(DefineParam(mOutputFormat, C2_PARAMKEY_OUTPUT_STREAM_BUFFER_TYPE)
                         .withConstValue(new C2StreamFormatConfig::output(0u, C2FormatCompressed))
                         .build());

    addParameter(DefineParam(mInputMediaType, C2_PARAMKEY_INPUT_MEDIA_TYPE)
                         .withConstValue(AllocSharedString<C2PortMimeConfig::input>(
                                 MEDIA_MIMETYPE_VIDEO_RAW))
                         .build());

    addParameter(DefineParam(mOutputMediaType, C2_PARAMKEY_OUTPUT_MEDIA_TYPE)
                         .withConstValue(AllocSharedString<C2PortMimeConfig::output>(outputMime))
                         .build());

    addParameter(DefineParam(mIntraRefreshPeriod, C2_PARAMKEY_INTRA_REFRESH)
                         .withDefault(new C2StreamIntraRefreshTuning::output(
                                 0u, C2Config::INTRA_REFRESH_DISABLED, 0.))
                         .withFields({C2F(mIntraRefreshPeriod, mode)
                                              .oneOf({C2Config::INTRA_REFRESH_DISABLED,
                                                      C2Config::INTRA_REFRESH_ARBITRARY}),
                                      C2F(mIntraRefreshPeriod, period).any()})
                         .withSetter(IntraRefreshPeriodSetter)
                         .build());

    addParameter(DefineParam(mRequestKeyFrame, C2_PARAMKEY_REQUEST_SYNC_FRAME)
                         .withDefault(new C2StreamRequestSyncFrameTuning::output(0u, C2_FALSE))
                         .withFields({C2F(mRequestKeyFrame, value).oneOf({C2_FALSE, C2_TRUE})})
                         .withSetter(Setter<decltype(*mRequestKeyFrame)>::NonStrictValueWithNoDeps)
                         .build());

    addParameter(DefineParam(mKeyFramePeriodUs, C2_PARAMKEY_SYNC_FRAME_INTERVAL)
                         .withDefault(new C2StreamSyncFrameIntervalTuning::output(0u, 1000000))
                         .withFields({C2F(mKeyFramePeriodUs, value).any()})
                         .withSetter(Setter<decltype(*mKeyFramePeriodUs)>::StrictValueWithNoDeps)
                         .build());

    C2Allocator::id_t inputAllocators[] = {C2PlatformAllocatorStore::GRALLOC};

    C2Allocator::id_t outputAllocators[] = {C2PlatformAllocatorStore::ION};

    addParameter(
            DefineParam(mInputAllocatorIds, C2_PARAMKEY_INPUT_ALLOCATORS)
                    .withConstValue(C2PortAllocatorsTuning::input::AllocShared(inputAllocators))
                    .build());

    addParameter(
            DefineParam(mOutputAllocatorIds, C2_PARAMKEY_OUTPUT_ALLOCATORS)
                    .withConstValue(C2PortAllocatorsTuning::output::AllocShared(outputAllocators))
                    .build());

    C2BlockPool::local_id_t outputBlockPools[] = {kDefaultOutputBlockPool};

    addParameter(
            DefineParam(mOutputBlockPoolIds, C2_PARAMKEY_OUTPUT_BLOCK_POOLS)
                    .withDefault(C2PortBlockPoolsTuning::output::AllocShared(outputBlockPools))
                    .withFields({C2F(mOutputBlockPoolIds, m.values[0]).any(),
                                 C2F(mOutputBlockPoolIds, m.values).inRange(0, 1)})
                    .withSetter(Setter<C2PortBlockPoolsTuning::output>::NonStrictValuesWithNoDeps)
                    .build());
}

uint32_t C2VEAComponent::IntfImpl::getKeyFramePeriod() const {
    if (mKeyFramePeriodUs->value < 0 || mKeyFramePeriodUs->value == INT64_MAX) {
        return 0;
    }
    double period = mKeyFramePeriodUs->value / 1e6 * mFrameRate->value;
    return static_cast<uint32_t>(std::max(std::min(std::round(period), double(UINT32_MAX)), 1.));
}

C2VEAComponent::C2VEAComponent(C2String name, c2_node_id_t id,
                               const std::shared_ptr<C2ReflectorHelper>& helper)
      : mIntfImpl(std::make_shared<IntfImpl>(name, helper, &mVEAAdaptor)),
        mIntf(std::make_shared<SimpleInterface<IntfImpl>>(name.c_str(), id, mIntfImpl)),
        mThread("C2VEAComponentThread"),
        mState(State::UNLOADED),
        mWeakThisFactory(this) {
    // TODO(johnylin): the client may need to know if init is failed.
    if (mIntfImpl->status() != C2_OK) {
        ALOGE("Component interface init failed (err code = %d)", mIntfImpl->status());
        return;
    }

    if (!mThread.Start()) {
        ALOGE("Component thread failed to start.");
        return;
    }
    mTaskRunner = mThread.task_runner();
    mState.store(State::LOADED);
}

C2VEAComponent::~C2VEAComponent() {
    CHECK_EQ(mState.load(), State::LOADED);

    if (mThread.IsRunning()) {
        mThread.Stop();
    }
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
    UNUSED(items);
    return C2_OMITTED;
}

c2_status_t C2VEAComponent::announce_nb(const std::vector<C2WorkOutline>& items) {
    UNUSED(items);
    return C2_OMITTED;
}

c2_status_t C2VEAComponent::flush_sm(flush_mode_t mode,
                                     std::list<std::unique_ptr<C2Work>>* const flushedWork) {
    UNUSED(mode);
    UNUSED(flushedWork);
    return C2_OMITTED;
}

c2_status_t C2VEAComponent::drain_nb(drain_mode_t mode) {
    UNUSED(mode);
    return C2_OMITTED;
}

c2_status_t C2VEAComponent::start() {
    return C2_OMITTED;
}

c2_status_t C2VEAComponent::stop() {
    return C2_OMITTED;
}

c2_status_t C2VEAComponent::reset() {
    return C2_OMITTED;
}

c2_status_t C2VEAComponent::release() {
    return C2_OMITTED;
}

std::shared_ptr<C2ComponentInterface> C2VEAComponent::intf() {
    return mIntf;
}

void C2VEAComponent::requireBitstreamBuffers(uint32_t inputCount, const media::Size& inputCodedSize,
                                             uint32_t outputBufferSize) {
    UNUSED(inputCount);
    UNUSED(inputCodedSize);
    UNUSED(outputBufferSize);
}

void C2VEAComponent::notifyVideoFrameDone(int64_t timestamp) {
    UNUSED(timestamp);
}

void C2VEAComponent::bitstreamBufferReady(uint32_t payloadSize, bool keyFrame, int64_t timestamp) {
    UNUSED(payloadSize);
    UNUSED(keyFrame);
    UNUSED(timestamp);
}

void C2VEAComponent::notifyFlushDone(bool done) {
    UNUSED(done);
}

void C2VEAComponent::notifyError(VideoEncodeAcceleratorAdaptor::Result error) {
    UNUSED(error);
}

class C2VEAComponentFactory : public C2ComponentFactory {
public:
    C2VEAComponentFactory(C2String encoderName)
          : mEncoderName(encoderName),
            // TODO: should get reflector from C2VEAComponentStore.
            mReflector(std::make_shared<C2ReflectorHelper>()){};

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
        std::unique_ptr<VideoEncodeAcceleratorAdaptor> adaptor;
        *interface =
                std::shared_ptr<C2ComponentInterface>(new SimpleInterface<C2VEAComponent::IntfImpl>(
                        mEncoderName.c_str(), id,
                        std::make_shared<C2VEAComponent::IntfImpl>(mEncoderName, mReflector,
                                                                   &adaptor)));
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
