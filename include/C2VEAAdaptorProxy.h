// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VEA_ADAPTOR_PROXY_H
#define ANDROID_C2_VEA_ADAPTOR_PROXY_H

#include <VideoEncodeAcceleratorAdaptor.h>

#include <base/files/scoped_file.h>

#include <arc/Future.h>
#include <mojo/public/cpp/bindings/binding.h>

#include <components/arc/common/video_encode_accelerator.mojom.h>

#include <memory>

namespace arc {
class MojoProcessSupport;
}  // namespace arc

namespace android {
namespace arc {

class C2VEAAdaptorProxy : public VideoEncodeAcceleratorAdaptor,
                          public ::arc::mojom::VideoEncodeClient {
public:
    C2VEAAdaptorProxy();
    explicit C2VEAAdaptorProxy(::arc::MojoProcessSupport* MojomProcessSupport);
    ~C2VEAAdaptorProxy() override;

    // Implementation of the VideoEncodeAcceleratorAdaptor interface.
    Result getSupportedProfiles(std::vector<VideoEncodeProfile>* profiles /* nonnull */) override;
    Result initialize(const VideoEncoderAcceleratorConfig& config, Client* client) override;
    void encode(uint64_t index, ::base::ScopedFD frameFd, media::VideoPixelFormat inputFormat,
                const std::vector<VideoFramePlane>& planes, int64_t timestamp,
                bool forceKeyFrame) override;
    void useBitstreamBuffer(uint64_t index, ::base::ScopedFD shmemFd, uint32_t offset,
                            uint32_t size) override;
    void requestEncodingParametersChange(uint32_t bitrate, uint32_t frameRate) override;
    void flush() override;

    // ::arc::mojom::VideoEncodeClient implementations.
    void RequireBitstreamBuffers(uint32_t input_count, const gfx::Size& input_coded_size,
                                 uint32_t output_buffer_size) override;
    void NotifyError(::arc::mojom::VideoEncodeAccelerator::Error error) override;

    // The following functions are called as callbacks.
    void NotifyVideoFrameDone(uint64_t index);
    void BitstreamBufferReady(uint64_t index, uint32_t payloadSize, bool keyFrame,
                              int64_t timestamp);
    void NotifyFlushDone(bool complete);

private:
    // Establishes ipc channel for video acceleration. Returns true if channel
    // connected successfully.
    // This must be called before the first time client calls either getSupportedProfiles() or
    // initialize(), and channel should be only established once.
    bool establishChannelOnce();

    void onConnectionError(const std::string& pipeName);
    void establishChannelOnMojoThread(std::shared_ptr<::arc::Future<bool>> future);
    void onVersionReady(std::shared_ptr<::arc::Future<bool>> future, uint32_t version);

    // Closes ipc channel for video acceleration.
    // This must be called before deleting this object.
    void closeChannelOnMojoThread();

    void getSupportedProfilesOnMojoThread(
            std::shared_ptr<::arc::Future<std::vector<VideoEncodeProfile>>> future);
    void onSupportedProfilesReady(
            std::shared_ptr<::arc::Future<std::vector<VideoEncodeProfile>>> future,
            std::vector<::arc::mojom::VideoEncodeProfilePtr> profiles);

    void initializeOnMojoThread(const VideoEncoderAcceleratorConfig& config,
                                const ::arc::mojom::VideoEncodeAccelerator::InitializeCallback& cb);
    void encodeOnMojoThread(uint64_t index, ::base::ScopedFD frameFd,
                            media::VideoPixelFormat inputFormat,
                            const std::vector<VideoFramePlane>& planes, int64_t timestamp,
                            bool forceKeyFrame);
    void useBitstreamBufferOnMojoThread(uint64_t index, ::base::ScopedFD shmemFd, uint32_t offset,
                                        uint32_t size);
    void requestEncodingParametersChangeOnMojoThread(uint32_t bitrate, uint32_t frameRate);
    void flushOnMojoThread();

    VideoEncodeAcceleratorAdaptor::Client* mClient;

    // Task runner for mojom functions.
    const scoped_refptr<::base::SingleThreadTaskRunner> mMojoTaskRunner;

    // |mVEAPtr| and |mBinding| should only be called on |mMojoTaskRunner| after bound.
    ::arc::mojom::VideoEncodeAcceleratorPtr mVEAPtr;
    mojo::Binding<::arc::mojom::VideoEncodeClient> mBinding;

    // Used to cancel the wait on arc::Future.
    sp<::arc::CancellationRelay> mRelay;

    // The flag of ipc channel is established. This is used to ensure channel is only established
    // once.
    bool mChannelEstablished;

    DISALLOW_COPY_AND_ASSIGN(C2VEAAdaptorProxy);
};

}  // namespace arc
}  // namespace android

#endif  // ANDROID_VIDEO_ENCODE_ACCELERATOR_ADAPTOR_H
