// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// #define LOG_NDEBUG 0
#define LOG_TAG "C2VEAAdaptorProxy"

#include <C2ArcVideoAcceleratorFactory.h>
#include <C2VEAAdaptorProxy.h>

#include <arc/MojoProcessSupport.h>
#include <arc/MojoThread.h>
#include <base/bind.h>
#include <base/files/scoped_file.h>
#include <mojo/public/cpp/platform/platform_handle.h>
#include <mojo/public/cpp/system/platform_handle.h>

#include <binder/IServiceManager.h>
#include <utils/Log.h>

#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

namespace android {
namespace arc {

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
                                                      std::string("mVEAPtr (vda pipe)")));
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
    UNUSED(config);

    DCHECK(client);
    DCHECK(!mClient);
    mClient = client;
    return VideoEncodeAcceleratorAdaptor::Result::SUCCESS;
}

void C2VEAAdaptorProxy::encode(int frameFd, media::VideoPixelFormat mInputFormat,
                               const std::vector<VideoFramePlane>& planes, int64_t timestamp,
                               bool forceKeyFrame) {
    UNUSED(frameFd);
    UNUSED(mInputFormat);
    UNUSED(planes);
    UNUSED(timestamp);
    UNUSED(forceKeyFrame);
}

void C2VEAAdaptorProxy::useBitstreamBuffer(int shmemFd, uint32_t offset, uint32_t size,
                                           int64_t timestamp) {
    UNUSED(shmemFd);
    UNUSED(offset);
    UNUSED(size);
    UNUSED(timestamp);
}

void C2VEAAdaptorProxy::requestEncodingParametersChange(uint32_t bitrate, uint32_t framerate) {
    UNUSED(bitrate);
    UNUSED(framerate);
}

void C2VEAAdaptorProxy::flush() {}

void C2VEAAdaptorProxy::RequireBitstreamBuffers(uint32_t input_count,
                                                const gfx::Size& input_coded_size,
                                                uint32_t output_buffer_size) {
    UNUSED(input_count);
    UNUSED(input_coded_size);
    UNUSED(output_buffer_size);
}

void C2VEAAdaptorProxy::NotifyError(::arc::mojom::VideoEncodeAccelerator::Error error) {
    UNUSED(error);
}

void C2VEAAdaptorProxy::NotifyVideoFrameDone(int64_t timestamp) {
    UNUSED(timestamp);
}

void C2VEAAdaptorProxy::BitstreamBufferReady(uint32_t payloadSize, bool keyFrame,
                                             int64_t timestamp) {
    UNUSED(payloadSize);
    UNUSED(keyFrame);
    UNUSED(timestamp);
}

void C2VEAAdaptorProxy::NotifyFlushDone(bool complete) {
    UNUSED(complete);
}

}  // namespace arc
}  // namespace android
