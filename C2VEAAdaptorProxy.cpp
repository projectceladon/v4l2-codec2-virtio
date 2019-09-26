// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VEAAdaptorProxy"

#include <C2ArcVideoAcceleratorFactory.h>
#include <C2VEAAdaptorProxy.h>

#include <base/bind.h>

#include <arc/MojoProcessSupport.h>
#include <arc/MojoThread.h>
#include <components/arc/video_accelerator/video_pixel_format.h>
#include <mojo/public/cpp/platform/platform_handle.h>
#include <mojo/public/cpp/system/platform_handle.h>

#include <binder/IServiceManager.h>
#include <utils/Log.h>

#include <inttypes.h>

namespace android {
namespace arc {

namespace {

android::VideoEncodeAcceleratorAdaptor::Result convertErrorCode(
        ::arc::mojom::VideoEncodeAccelerator::Error error) {
    switch (error) {
    case ::arc::mojom::VideoEncodeAccelerator::Error::kIllegalStateError:
        return android::VideoEncodeAcceleratorAdaptor::Result::ILLEGAL_STATE;
    case ::arc::mojom::VideoEncodeAccelerator::Error::kInvalidArgumentError:
        return android::VideoEncodeAcceleratorAdaptor::Result::INVALID_ARGUMENT;
    case ::arc::mojom::VideoEncodeAccelerator::Error::kPlatformFailureError:
        return android::VideoEncodeAcceleratorAdaptor::Result::PLATFORM_FAILURE;

    default:
        ALOGE("Unknown error code: %d", static_cast<int>(error));
        return android::VideoEncodeAcceleratorAdaptor::Result::PLATFORM_FAILURE;
    }
}

}  // namespace

C2VEAAdaptorProxy::C2VEAAdaptorProxy()
      : C2VEAAdaptorProxy(::arc::MojoProcessSupport::getLeakyInstance()) {}

C2VEAAdaptorProxy::C2VEAAdaptorProxy(::arc::MojoProcessSupport* mojoProcessSupport)
      : mClient(nullptr),
        mMojoTaskRunner(mojoProcessSupport->mojo_thread().getTaskRunner()),
        mBinding(this),
        mRelay(new ::arc::CancellationRelay()),
        mChannelEstablished(false) {}

C2VEAAdaptorProxy::~C2VEAAdaptorProxy() {
    mRelay->cancel();
    ::arc::Future<void> future;
    ::arc::PostTaskAndSetFutureWithResult(
            mMojoTaskRunner.get(), FROM_HERE,
            ::base::Bind(&C2VEAAdaptorProxy::closeChannelOnMojoThread, ::base::Unretained(this)),
            &future);
    future.get();
}

void C2VEAAdaptorProxy::onConnectionError(const std::string& pipeName) {
    ALOGE("onConnectionError (%s)", pipeName.c_str());
    mRelay->cancel();
    NotifyError(::arc::mojom::VideoEncodeAccelerator::Error::kPlatformFailureError);
}

bool C2VEAAdaptorProxy::establishChannelOnce() {
    if (mChannelEstablished) {
        return true;
    }

    ALOGV("establishChannelOnce");
    auto future = ::arc::Future<bool>::make_shared(mRelay);
    mMojoTaskRunner->PostTask(FROM_HERE,
                              ::base::Bind(&C2VEAAdaptorProxy::establishChannelOnMojoThread,
                                           ::base::Unretained(this), future));
    mChannelEstablished = future->wait() && future->get();
    return mChannelEstablished;
}

void C2VEAAdaptorProxy::establishChannelOnMojoThread(std::shared_ptr<::arc::Future<bool>> future) {
    auto& factory = ::android::GetC2ArcVideoAcceleratorFactory();

    if (!factory.createVideoEncodeAccelerator(mojo::MakeRequest(&mVEAPtr))) {
        future->set(false);
        return;
    }
    mVEAPtr.set_connection_error_handler(::base::Bind(&C2VEAAdaptorProxy::onConnectionError,
                                                      ::base::Unretained(this),
                                                      std::string("mVEAPtr (vea pipe)")));
    mVEAPtr.QueryVersion(::base::Bind(&C2VEAAdaptorProxy::onVersionReady, ::base::Unretained(this),
                                      std::move(future)));
}

void C2VEAAdaptorProxy::onVersionReady(std::shared_ptr<::arc::Future<bool>> future,
                                       uint32_t version) {
    ALOGI("VideoEncodeAccelerator ready (version=%d)", version);

    future->set(true);
}

void C2VEAAdaptorProxy::closeChannelOnMojoThread() {
    if (mBinding.is_bound()) mBinding.Close();
    mVEAPtr.reset();
}

VideoEncodeAcceleratorAdaptor::Result C2VEAAdaptorProxy::getSupportedProfiles(
        std::vector<VideoEncodeProfile>* profiles) {
    ALOGV("getSupportedProfiles");
    profiles->clear();

    if (!establishChannelOnce()) {
        ALOGE("establishChannelOnce failed");
        return VideoEncodeAcceleratorAdaptor::Result::PLATFORM_FAILURE;
    }

    auto future = ::arc::Future<std::vector<VideoEncodeProfile>>::make_shared(mRelay);
    mMojoTaskRunner->PostTask(FROM_HERE,
                              ::base::Bind(&C2VEAAdaptorProxy::getSupportedProfilesOnMojoThread,
                                           ::base::Unretained(this), future));

    if (!future->wait()) {
        ALOGE("getSupportedProfiles failed: Connection lost");
        return VideoEncodeAcceleratorAdaptor::Result::PLATFORM_FAILURE;
    }

    *profiles = future->get();
    if (profiles->empty()) {
        ALOGE("getSupportedProfiles failed: No supported profiles");
        return VideoEncodeAcceleratorAdaptor::Result::PLATFORM_FAILURE;
    }

    return VideoEncodeAcceleratorAdaptor::Result::SUCCESS;
}

void C2VEAAdaptorProxy::getSupportedProfilesOnMojoThread(
        std::shared_ptr<::arc::Future<std::vector<VideoEncodeProfile>>> future) {
    mVEAPtr->GetSupportedProfiles(::base::Bind(&C2VEAAdaptorProxy::onSupportedProfilesReady,
                                               ::base::Unretained(this), std::move(future)));
}

void C2VEAAdaptorProxy::onSupportedProfilesReady(
        std::shared_ptr<::arc::Future<std::vector<VideoEncodeProfile>>> future,
        std::vector<::arc::mojom::VideoEncodeProfilePtr> profiles) {
    ALOGV("NotifySupportedProfilesReady(profile[%zu])", profiles.size());

    std::vector<VideoEncodeProfile> supportedProfiles;
    for (const auto& entry : profiles) {
        supportedProfiles.push_back(
                {static_cast<media::VideoCodecProfile>(entry->profile),
                 media::Size(entry->max_resolution.width(), entry->max_resolution.height()),
                 entry->max_framerate_numerator, entry->max_framerate_denominator});
    }
    future->set(std::move(supportedProfiles));
}

VideoEncodeAcceleratorAdaptor::Result C2VEAAdaptorProxy::initialize(
        const VideoEncoderAcceleratorConfig& config, Client* client) {
    ALOGV("initialize");
    DCHECK(client);
    DCHECK(!mClient);
    mClient = client;

    if (!establishChannelOnce()) {
        ALOGE("establishChannelOnce failed");
        return VideoEncodeAcceleratorAdaptor::Result::PLATFORM_FAILURE;
    }

    auto future = ::arc::Future<bool>::make_shared(mRelay);
    mMojoTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VEAAdaptorProxy::initializeOnMojoThread,
                                                      ::base::Unretained(this), config,
                                                      ::arc::FutureCallback(future)));

    if (!future->wait()) {
        ALOGE("Connection lost");
        return VideoEncodeAcceleratorAdaptor::Result::PLATFORM_FAILURE;
    }

    if (!future->get()) {
        ALOGE("VEA initialize failed");
        return VideoEncodeAcceleratorAdaptor::Result::PLATFORM_FAILURE;
    }

    return VideoEncodeAcceleratorAdaptor::Result::SUCCESS;
}

void C2VEAAdaptorProxy::initializeOnMojoThread(
        const VideoEncoderAcceleratorConfig& config,
        const ::arc::mojom::VideoEncodeAccelerator::InitializeCallback& cb) {
    ::arc::mojom::VideoEncodeAcceleratorConfigPtr arcConfig =
            ::arc::mojom::VideoEncodeAcceleratorConfig::New();
    arcConfig->input_format = static_cast<::arc::VideoPixelFormat>(config.mInputFormat);
    arcConfig->input_visible_size =
            gfx::Size(config.mInputVisibleSize.width(), config.mInputVisibleSize.height());
    arcConfig->output_profile = static_cast<::arc::mojom::VideoCodecProfile>(config.mOutputProfile);
    arcConfig->initial_bitrate = config.mInitialBitrate;
    arcConfig->initial_framerate = config.mInitialFramerate;
    arcConfig->has_initial_framerate = true;
    arcConfig->h264_output_level = config.mH264OutputLevel;
    arcConfig->has_h264_output_level = true;
    arcConfig->storage_type = static_cast<::arc::mojom::VideoFrameStorageType>(config.mStorageType);

    mojo::InterfacePtr<::arc::mojom::VideoEncodeClient> client;
    mBinding.Bind(mojo::MakeRequest(&client));

    mVEAPtr->Initialize(std::move(arcConfig), std::move(client), cb);
}

void C2VEAAdaptorProxy::encode(uint64_t index, ::base::ScopedFD frameFd,
                               media::VideoPixelFormat inputFormat,
                               const std::vector<VideoFramePlane>& planes, int64_t timestamp,
                               bool forceKeyFrame) {
    ALOGV("encode(frame_index=%" PRIu64 ", timestamp=%" PRId64 ")", index, timestamp);
    mMojoTaskRunner->PostTask(FROM_HERE,
                              ::base::BindOnce(&C2VEAAdaptorProxy::encodeOnMojoThread,
                                               ::base::Unretained(this), index, std::move(frameFd),
                                               inputFormat, planes, timestamp, forceKeyFrame));
}

void C2VEAAdaptorProxy::encodeOnMojoThread(uint64_t index, ::base::ScopedFD frameFd,
                                           media::VideoPixelFormat inputFormat,
                                           const std::vector<VideoFramePlane>& planes,
                                           int64_t timestamp, bool forceKeyFrame) {
    mojo::ScopedHandle wrappedHandle =
            mojo::WrapPlatformHandle(mojo::PlatformHandle(std::move(frameFd)));
    if (!wrappedHandle.is_valid()) {
        ALOGE("encodeOnMojoThread: failed to wrap handle");
        NotifyError(::arc::mojom::VideoEncodeAccelerator::Error::kPlatformFailureError);
        return;
    }

    std::vector<::arc::VideoFramePlane> arcPlanes;
    for (const auto& plane : planes) {
        arcPlanes.push_back(::arc::VideoFramePlane{static_cast<int32_t>(plane.mOffset),
                                                   static_cast<int32_t>(plane.mStride)});
    }

    mVEAPtr->Encode(static_cast<::arc::VideoPixelFormat>(inputFormat), std::move(wrappedHandle),
                    std::move(arcPlanes), timestamp, forceKeyFrame,
                    ::base::Bind(&C2VEAAdaptorProxy::NotifyVideoFrameDone, ::base::Unretained(this),
                                 index));
}

void C2VEAAdaptorProxy::useBitstreamBuffer(uint64_t index, ::base::ScopedFD shmemFd,
                                           uint32_t offset, uint32_t size) {
    ALOGV("useBitstreamBuffer");
    mMojoTaskRunner->PostTask(
            FROM_HERE,
            ::base::BindOnce(&C2VEAAdaptorProxy::useBitstreamBufferOnMojoThread,
                             ::base::Unretained(this), index, std::move(shmemFd), offset, size));
}

void C2VEAAdaptorProxy::useBitstreamBufferOnMojoThread(uint64_t index, ::base::ScopedFD shmemFd,
                                                       uint32_t offset, uint32_t size) {
    mojo::ScopedHandle wrappedHandle =
            mojo::WrapPlatformHandle(mojo::PlatformHandle(std::move(shmemFd)));
    if (!wrappedHandle.is_valid()) {
        ALOGE("useBitstreamBufferOnMojoThread: failed to wrap handle");
        NotifyError(::arc::mojom::VideoEncodeAccelerator::Error::kPlatformFailureError);
        return;
    }

    mVEAPtr->UseBitstreamBuffer(std::move(wrappedHandle), offset, size,
                                ::base::Bind(&C2VEAAdaptorProxy::BitstreamBufferReady,
                                             ::base::Unretained(this), index));
}

void C2VEAAdaptorProxy::requestEncodingParametersChange(uint32_t bitrate, uint32_t frameRate) {
    ALOGV("requestEncodingParametersChange(bitrate=%u, frameRate=%u)", bitrate, frameRate);
    mMojoTaskRunner->PostTask(
            FROM_HERE, ::base::Bind(&C2VEAAdaptorProxy::requestEncodingParametersChangeOnMojoThread,
                                    ::base::Unretained(this), bitrate, frameRate));
}

void C2VEAAdaptorProxy::requestEncodingParametersChangeOnMojoThread(uint32_t bitrate,
                                                                    uint32_t frameRate) {
    mVEAPtr->RequestEncodingParametersChange(bitrate, frameRate);
}

void C2VEAAdaptorProxy::flush() {
    ALOGV("flush");
    mMojoTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VEAAdaptorProxy::flushOnMojoThread,
                                                      ::base::Unretained(this)));
}

void C2VEAAdaptorProxy::flushOnMojoThread() {
    mVEAPtr->Flush(::base::Bind(&C2VEAAdaptorProxy::NotifyFlushDone, ::base::Unretained(this)));
}

void C2VEAAdaptorProxy::RequireBitstreamBuffers(uint32_t input_count,
                                                const gfx::Size& input_coded_size,
                                                uint32_t output_buffer_size) {
    ALOGV("RequireBitstreamBuffers");
    mClient->requireBitstreamBuffers(
            input_count, media::Size(input_coded_size.width(), input_coded_size.height()),
            output_buffer_size);
}

void C2VEAAdaptorProxy::NotifyError(::arc::mojom::VideoEncodeAccelerator::Error error) {
    ALOGE("NotifyError %d", static_cast<int>(error));
    mClient->notifyError(convertErrorCode(error));
}

void C2VEAAdaptorProxy::NotifyVideoFrameDone(uint64_t index) {
    ALOGV("NotifyVideoFrameDone(frame_index=%" PRIu64 ")", index);
    mClient->notifyVideoFrameDone(index);
}

void C2VEAAdaptorProxy::BitstreamBufferReady(uint64_t index, uint32_t payloadSize, bool keyFrame,
                                             int64_t timestamp) {
    ALOGV("BitstreamBufferReady(index=%" PRIu64 ", timestamp=%" PRId64 ")", index, timestamp);
    mClient->bitstreamBufferReady(index, payloadSize, keyFrame, timestamp);
}

void C2VEAAdaptorProxy::NotifyFlushDone(bool complete) {
    ALOGV("NotifyFlushDone: %s", complete ? "complete" : "abort");
    mClient->notifyFlushDone(complete);
}

}  // namespace arc
}  // namespace android
