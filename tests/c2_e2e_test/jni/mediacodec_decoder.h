// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef C2_E2E_TEST_MEDIACODEC_DECODER_H_
#define C2_E2E_TEST_MEDIACODEC_DECODER_H_

#include <memory>
#include <queue>
#include <string>
#include <vector>

#include <media/NdkMediaCodec.h>

#include "common.h"
#include "encoded_data_helper.h"

namespace android {

// Wrapper class to manipulate a MediaCodec video decoder.
class MediaCodecDecoder {
public:
    // Checks the argument and create MediaCodecDecoder instance.
    static std::unique_ptr<MediaCodecDecoder> Create(const std::string& input_path,
                                                     VideoCodecProfile profile,
                                                     const Size& video_size);

    MediaCodecDecoder() = delete;
    ~MediaCodecDecoder();

    // The callback function that is called when output buffer is ready.
    using OutputBufferReadyCb = std::function<void(
            const uint8_t* /* data */, size_t /* buffer_size */, int /* output_index */)>;
    void AddOutputBufferReadyCb(const OutputBufferReadyCb& cb);

    // The callback function that is called when output format is changed.
    using OutputFormatChangedCb =
            std::function<void(const Size& /* coded_size */, const Size& /* visible_size */,
                               int32_t /* color_format */)>;
    void AddOutputFormatChangedCb(const OutputFormatChangedCb& cb);

    // Decoder manipulation methods.

    // Rewind the input stream to the first frame as well as frame index.
    void Rewind();

    // Wrapper of AMediaCodec_configure.
    bool Configure();

    // Wrapper of AMediaCodec_start.
    bool Start();

    // Decode the input stream. After decoding, send EOS request to the decoder.
    // Return true if EOS output buffer is received.
    bool Decode();

    // Wrapper of AMediaCodec_stop.
    bool Stop();

private:
    MediaCodecDecoder(AMediaCodec* codec, std::unique_ptr<EncodedDataHelper> encoded_data_helper,
                      VideoCodecType type, const Size& size);

    // Enum class of the status of dequeueing output buffer.
    enum class DequeueStatus { RETRY, SUCCESS, FAILURE };

    // Fill all available input buffers and enqueue.
    bool EnqueueInputBuffers();

    // Try to dequeue one output buffer and return DequeueStatus.
    DequeueStatus DequeueOutputBuffer();

    // Read the sample data from AMediaExtractor or CSD data and feed into the
    // input buffer.
    // |index| is the index of the target input buffer.
    bool FeedInputBuffer(size_t index);

    // Feed an empty buffer with EOS flag.
    // |index| is the index of the target input buffer.
    bool FeedEOSInputBuffer(size_t index);

    // Receive the output buffer and make mOutputBufferReadyCb callback if given.
    // |index| is the index of the target output buffer.
    // |info| is the buffer info of the target output buffer.
    bool ReceiveOutputBuffer(size_t index, const AMediaCodecBufferInfo& info);

    // Get output format by AMediaCodec_getOutputFormat and make
    // |output_format_changed_cb_| callback if given.
    // Return false if required information is missing, e.g. width, color format.
    bool GetOutputFormat();

    // The target mediacodec decoder.
    AMediaCodec* codec_;

    // The reference of EncodedDataHelper.
    std::unique_ptr<EncodedDataHelper> encoded_data_helper_;

    // The codec type of decoding.
    VideoCodecType type_;
    // The output video visible size.
    Size input_visible_size_;

    // The list of callback functions which are called in order when a output
    // buffer is ready.
    std::vector<OutputBufferReadyCb> output_buffer_ready_cbs_;
    // The list of callback functions that are called in order when output format
    // is changed.
    std::vector<OutputFormatChangedCb> output_format_changed_cbs_;

    // The fragment index that indicates which frame is sent to the decoder at
    // next round.
    int input_fragment_index_ = 0;
    // The total number of received output buffers. Only used for logging.
    int received_outputs_ = 0;

    // The indication of input done.
    bool input_done_ = false;
    // The indication of output done.
    bool output_done_ = false;
};

}  // namespace android

#endif  // C2_E2E_TEST_MEDIACODEC_DECODER_H_
