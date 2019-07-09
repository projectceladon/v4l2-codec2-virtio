// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_VIDEO_ENCODE_ACCELERATOR_ADAPTOR_H
#define ANDROID_VIDEO_ENCODE_ACCELERATOR_ADAPTOR_H

#include <size.h>
#include <video_codecs.h>
#include <video_pixel_format.h>

#include <base/files/scoped_file.h>

#include <vector>

namespace android {

// The offset and stride of a video frame plane.
struct VideoFramePlane {
    uint32_t mOffset;
    uint32_t mStride;
};

// Specification of an encoding profile supported by an encoder.
struct VideoEncodeProfile {
    media::VideoCodecProfile mProfile;
    media::Size mMaxResolution;
    uint32_t mMaxFramerateNumerator;
    uint32_t mMaxFramerateDenominator;
};

// The encoder parameter set.
//  |mInputFormat| is the pixel format of the input frames.
//  |mInputVisibleSize| is the resolution of the input frames.
//  |mOutputProfile| is the codec profile of the encoded output stream.
//  |mInitialBitrate| is the initial bitrate of the encoded output stream, in bits per second.
//  |mInitialFramerate| is the initial requested framerate.
//  |mH264OutputLevel| is H264 level of encoded output stream.
//  |mStorageType| is the storage type of video frame provided on encode().
struct VideoEncoderAcceleratorConfig {
    enum VideoFrameStorageType {
        SHMEM = 0,
        DMABUF = 1,
    };

    media::VideoPixelFormat mInputFormat;
    media::Size mInputVisibleSize;
    media::VideoCodecProfile mOutputProfile;
    uint32_t mInitialBitrate;
    uint32_t mInitialFramerate;
    uint8_t mH264OutputLevel;
    VideoFrameStorageType mStorageType;
};

// Video encoder accelerator adaptor interface.
// The adaptor plays the role of providing unified adaptor API functions and client callback to
// codec component side.
class VideoEncodeAcceleratorAdaptor {
public:
    enum class Result : int32_t {
        SUCCESS = 0,
        ILLEGAL_STATE = 1,
        INVALID_ARGUMENT = 2,
        PLATFORM_FAILURE = 3,
    };

    // The adaptor client interface. This interface should be implemented in the component side.
    class Client {
    public:
        virtual ~Client() {}

        // Callback to tell the client what size of frames and buffers to provide for input and
        // output. The VEA disclaims use or ownership of all previously provided buffers once this
        // callback is called.
        virtual void requireBitstreamBuffers(uint32_t inputCount, const media::Size& inputCodedSize,
                                             uint32_t outputBufferSize) = 0;

        // Callback from encode() to notify the input frame is no more used by VEA. |index| is
        // correspondent to the one passed by encode().
        virtual void notifyVideoFrameDone(uint64_t index) = 0;

        // Callback from useBitstreamBuffer() to deliver encoded bitstream buffers. Ownership of the
        // buffer is transferred back to the client once this callback is called. |index| is
        // correspondent to the one passed by useBitstreamBuffer(). |timestamp| is the same
        // timestamp as the one passed by encode().
        virtual void bitstreamBufferReady(uint64_t index, uint32_t payloadSize, bool keyFrame,
                                          int64_t timestamp) = 0;

        // Callback from flush(). |done| is true if flush() is complete; false if flush() is
        // canceled.
        virtual void notifyFlushDone(bool done) = 0;

        // Callback to notify about errors. Note that errors in initialize() will not be reported
        // here, instead of by its returned value.
        virtual void notifyError(Result error) = 0;
    };

    // Obtains an array of the supported profiles of the video encoder and returns the status. This
    // can be called before initialize().
    virtual Result getSupportedProfiles(
            std::vector<VideoEncodeProfile>* profiles /* nonnull */) = 0;

    // Initializes the video encoder with specific configuration.  Called once per encoder
    // construction.
    virtual Result initialize(const VideoEncoderAcceleratorConfig& config, Client* client) = 0;

    // Encodes the given frame.
    virtual void encode(uint64_t index, ::base::ScopedFD frameFd,
                        media::VideoPixelFormat inputFormat,
                        const std::vector<VideoFramePlane>& planes, int64_t timestamp,
                        bool forceKeyFrame) = 0;

    // Sends a bitstream buffer to the encoder for storing encoded output. The shared memory buffer
    // will be filled with the encoded bitstream.
    virtual void useBitstreamBuffer(uint64_t index, ::base::ScopedFD shmemFd, uint32_t offset,
                                    uint32_t size) = 0;

    // Requests a change to the encoding parameters. This is only a request, fulfilled on a
    // best-effort basis.
    virtual void requestEncodingParametersChange(uint32_t bitrate, uint32_t frameRate) = 0;

    // Flushes the encoder: all pending inputs will be encoded and all bitstreams handed back to the
    // client. The client should not invoke flush() or encode() before the previous flush() is
    // finished.
    virtual void flush() = 0;

    virtual ~VideoEncodeAcceleratorAdaptor() {}
};

}  // namespace android

#endif  // ANDROID_VIDEO_ENCODE_ACCELERATOR_ADAPTOR_H
