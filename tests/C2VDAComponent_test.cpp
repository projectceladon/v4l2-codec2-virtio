// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VDAComponent_test"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include <C2VDAComponent.h>

#include <C2AllocatorCrosGralloc.h>
#include <C2AllocatorMemDealer.h>
#include <C2Buffer.h>
#include <C2BufferPriv.h>
#include <C2Component.h>
#include <C2VDASupport.h>
#include <C2Work.h>

#include <media/IMediaHTTPService.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AUtils.h>

#include <gtest/gtest.h>
#include <utils/Log.h>

using namespace std::chrono_literals;

namespace android {

// Input video data parameters. This could be overwritten by user argument [-i].
// The syntax of each column is:
//  filename:componentName:width:height:numFrames:numFragments
// - |filename| is the file path to mp4 (h264) or webm (VP8/9) video.
// - |componentName| specifies the name of decoder component.
// - |width| and |height| are for video size (in pixels).
// - |numFrames| is the number of picture frames.
// - |numFragments| is the NALU (h264) or frame (VP8/9) count by MediaExtractor.
const char* gTestVideoData = "bear.mp4:v4l2.h264.decode:640:368:82:84";
//const char* gTestVideoData = "bear-vp8.webm:v4l2.vp8.decode:640:368:82:82";

const std::string kH264DecoderName = "v4l2.h264.decode";
const std::string kVP8DecoderName = "v4l2.vp8.decode";

// Magic constants for indicating the timing of flush being called.
enum FlushPoint : int { END_OF_STREAM_FLUSH = -3, MID_STREAM_FLUSH = -2, NO_FLUSH = -1 };

struct TestVideoFile {
    enum class CodecType { UNKNOWN, H264, VP8, VP9 };

    std::string mFilename;
    std::string mComponentName;
    CodecType mCodec = CodecType::UNKNOWN;
    int mWidth = -1;
    int mHeight = -1;
    int mNumFrames = -1;
    int mNumFragments = -1;
    sp<IMediaSource> mData;
};

class C2VDALinearBuffer : public C2Buffer {
public:
    explicit C2VDALinearBuffer(const std::shared_ptr<C2LinearBlock>& block)
          : C2Buffer({block->share(block->offset(), block->size(), C2Fence())}) {}
};

class Listener;

class C2VDAComponentTest : public ::testing::Test {
public:
    void onWorkDone(std::weak_ptr<C2Component> component,
                    std::vector<std::unique_ptr<C2Work>> workItems);
    void onTripped(std::weak_ptr<C2Component> component,
                   std::vector<std::shared_ptr<C2SettingResult>> settingResult);
    void onError(std::weak_ptr<C2Component> component, uint32_t errorCode);

protected:
    C2VDAComponentTest();
    void SetUp() override;

    void parseTestVideoData(const char* testVideoData);

protected:
    using ULock = std::unique_lock<std::mutex>;

    enum {
        kWorkCount = 16,
    };

    std::shared_ptr<Listener> mListener;

    // Allocators
    std::shared_ptr<C2Allocator> mLinearAlloc;
    std::shared_ptr<C2BlockPool> mLinearBlockPool;

    // The array of output video frame counters which will be counted in listenerThread. The array
    // length equals to iteration time of stream play.
    std::vector<int> mOutputFrameCounts;
    // The array of work counters returned from component which will be counted in listenerThread.
    // The array length equals to iteration time of stream play.
    std::vector<int> mFinishedWorkCounts;

    // Mutex for |mWorkQueue| among main and listenerThread.
    std::mutex mQueueLock;
    std::condition_variable mQueueCondition;
    std::list<std::unique_ptr<C2Work>> mWorkQueue;

    // Mutex for |mProcessedWork| among main and listenerThread.
    std::mutex mProcessedLock;
    std::condition_variable mProcessedCondition;
    std::list<std::unique_ptr<C2Work>> mProcessedWork;

    // Mutex for |mFlushDone| among main and listenerThread.
    std::mutex mFlushDoneLock;
    std::condition_variable mFlushDoneCondition;
    bool mFlushDone;

    std::unique_ptr<TestVideoFile> mTestVideoFile;
};

class Listener : public C2Component::Listener {
public:
    explicit Listener(C2VDAComponentTest* thiz) : mThis(thiz) {}
    virtual ~Listener() = default;

    virtual void onWorkDone_nb(std::weak_ptr<C2Component> component,
                               std::vector<std::unique_ptr<C2Work>> workItems) override {
        mThis->onWorkDone(component, std::move(workItems));
    }

    virtual void onTripped_nb(
            std::weak_ptr<C2Component> component,
            std::vector<std::shared_ptr<C2SettingResult>> settingResult) override {
        mThis->onTripped(component, settingResult);
    }

    virtual void onError_nb(std::weak_ptr<C2Component> component, uint32_t errorCode) override {
        mThis->onError(component, errorCode);
    }

private:
    C2VDAComponentTest* const mThis;
};

C2VDAComponentTest::C2VDAComponentTest() : mListener(new Listener(this)) {
    std::shared_ptr<C2AllocatorStore> store = getCodec2VDAAllocatorStore();
    CHECK_EQ(store->fetchAllocator(C2AllocatorStore::DEFAULT_LINEAR, &mLinearAlloc), C2_OK);

    mLinearBlockPool = std::make_shared<C2BasicLinearBlockPool>(mLinearAlloc);
}

void C2VDAComponentTest::onWorkDone(std::weak_ptr<C2Component> component,
                                    std::vector<std::unique_ptr<C2Work>> workItems) {
    (void)component;
    ULock l(mProcessedLock);
    for (auto& item : workItems) {
        mProcessedWork.emplace_back(std::move(item));
    }
    mProcessedCondition.notify_all();
}

void C2VDAComponentTest::onTripped(std::weak_ptr<C2Component> component,
                                   std::vector<std::shared_ptr<C2SettingResult>> settingResult) {
    (void)component;
    (void)settingResult;
    // no-ops
}

void C2VDAComponentTest::onError(std::weak_ptr<C2Component> component, uint32_t errorCode) {
    (void)component;
    // fail the test
    FAIL() << "Get error code from component: " << errorCode;
}

void C2VDAComponentTest::SetUp() {
    parseTestVideoData(gTestVideoData);

    mWorkQueue.clear();
    for (int i = 0; i < kWorkCount; ++i) {
        mWorkQueue.emplace_back(new C2Work);
    }
    mProcessedWork.clear();
    mFlushDone = false;
}

static bool getMediaSourceFromFile(const std::string& filename,
                                   const TestVideoFile::CodecType codec, sp<IMediaSource>* source) {
    source->clear();

    sp<DataSource> dataSource =
            DataSource::CreateFromURI(nullptr /* httpService */, filename.c_str());

    if (dataSource == nullptr) {
        fprintf(stderr, "Unable to create data source.\n");
        return false;
    }

    sp<IMediaExtractor> extractor = MediaExtractor::Create(dataSource);
    if (extractor == nullptr) {
        fprintf(stderr, "could not create extractor.\n");
        return false;
    }

    std::string expectedMime;
    if (codec == TestVideoFile::CodecType::H264) {
        expectedMime = "video/avc";
    } else if (codec == TestVideoFile::CodecType::VP8) {
        expectedMime = "video/x-vnd.on2.vp8";
    } else {
        fprintf(stderr, "unsupported codec type.\n");
        return false;
    }

    for (size_t i = 0, numTracks = extractor->countTracks(); i < numTracks; ++i) {
        sp<MetaData> meta =
                extractor->getTrackMetaData(i, MediaExtractor::kIncludeExtensiveMetaData);
        if (meta == nullptr) {
            continue;
        }
        const char* mime;
        meta->findCString(kKeyMIMEType, &mime);
        if (!strcasecmp(mime, expectedMime.c_str())) {
            *source = extractor->getTrack(i);
            if (*source == nullptr) {
                fprintf(stderr, "It's NULL track for track %zu.\n", i);
                return false;
            }
            return true;
        }
    }
    fprintf(stderr, "No track found.\n");
    return false;
}

void C2VDAComponentTest::parseTestVideoData(const char* testVideoData) {
    ALOGV("videoDataStr: %s", testVideoData);
    mTestVideoFile = std::make_unique<TestVideoFile>();

    auto splitString = [](const std::string& input, const char delim) {
        std::vector<std::string> splits;
        auto beg = input.begin();
        while (beg != input.end()) {
            auto pos = std::find(beg, input.end(), delim);
            splits.emplace_back(beg, pos);
            beg = pos != input.end() ? pos + 1 : pos;
        }
        return splits;
    };
    auto tokens = splitString(testVideoData, ':');
    LOG_ASSERT(tokens.size() == 6u);
    mTestVideoFile->mFilename = tokens[0];
    LOG_ASSERT(mTestVideoFile->mFilename.length() > 0);

    mTestVideoFile->mComponentName = tokens[1];
    if (mTestVideoFile->mComponentName == kH264DecoderName) {
        mTestVideoFile->mCodec = TestVideoFile::CodecType::H264;
    } else if (mTestVideoFile->mComponentName == kVP8DecoderName) {
        mTestVideoFile->mCodec = TestVideoFile::CodecType::VP8;
    }
    LOG_ASSERT(mTestVideoFile->mCodec != TestVideoFile::CodecType::UNKNOWN);

    mTestVideoFile->mWidth = std::stoi(tokens[2]);
    mTestVideoFile->mHeight = std::stoi(tokens[3]);
    mTestVideoFile->mNumFrames = std::stoi(tokens[4]);
    mTestVideoFile->mNumFragments = std::stoi(tokens[5]);

    ALOGV("mTestVideoFile: %s, %s, %d, %d, %d, %d", mTestVideoFile->mFilename.c_str(),
          mTestVideoFile->mComponentName.c_str(), mTestVideoFile->mWidth, mTestVideoFile->mHeight,
          mTestVideoFile->mNumFrames, mTestVideoFile->mNumFragments);
}

// Test parameters:
// - Flush after work index. If this value is not negative, test will signal flush to component
//   after queueing the work frame index equals to this value in the first iteration. Negative
//   values may be magic constants, please refer to FlushPoint enum.
// - Number of play through. This value specifies the iteration time for playing entire video. If
//   |mFlushAfterWorkIndex| is not negative, the first iteration will perform flush, then repeat
//   times as this value for playing entire video.
class C2VDAComponentParamTest : public C2VDAComponentTest,
                                public ::testing::WithParamInterface<std::tuple<int, uint32_t>> {
protected:
    int mFlushAfterWorkIndex;
    uint32_t mNumberOfPlaythrough;
};

TEST_P(C2VDAComponentParamTest, SimpleDecodeTest) {
    mFlushAfterWorkIndex = std::get<0>(GetParam());
    if (mFlushAfterWorkIndex == FlushPoint::MID_STREAM_FLUSH) {
        mFlushAfterWorkIndex = mTestVideoFile->mNumFragments / 2;
    } else if (mFlushAfterWorkIndex == FlushPoint::END_OF_STREAM_FLUSH) {
        mFlushAfterWorkIndex = mTestVideoFile->mNumFragments - 1;
    }
    ASSERT_LT(mFlushAfterWorkIndex, mTestVideoFile->mNumFragments);
    mNumberOfPlaythrough = std::get<1>(GetParam());

    if (mFlushAfterWorkIndex >= 0) {
        mNumberOfPlaythrough++;  // add the first iteration for perform mid-stream flushing.
    }

    // Reset counters and determine the expected answers for all iterations.
    mOutputFrameCounts.resize(mNumberOfPlaythrough, 0);
    mFinishedWorkCounts.resize(mNumberOfPlaythrough, 0);
    std::vector<int> expectedOutputFrameCounts(mNumberOfPlaythrough, mTestVideoFile->mNumFrames);
    std::vector<int> expectedFinishedWorkCounts(mNumberOfPlaythrough,
                                                mTestVideoFile->mNumFragments);
    if (mFlushAfterWorkIndex >= 0) {
        // First iteration performs the mid-stream flushing.
        expectedOutputFrameCounts[0] = mFlushAfterWorkIndex + 1;
        expectedFinishedWorkCounts[0] = mFlushAfterWorkIndex + 1;
    }

    std::shared_ptr<C2Component> component(
            std::make_shared<C2VDAComponent>(mTestVideoFile->mComponentName, 0));
    ASSERT_EQ(component->setListener_vb(mListener, C2_DONT_BLOCK), C2_OK);
    std::unique_ptr<C2PortBlockPoolsTuning::output> pools =
            C2PortBlockPoolsTuning::output::alloc_unique(
                    {static_cast<uint64_t>(C2BlockPool::BASIC_GRAPHIC)});
    std::vector<std::unique_ptr<C2SettingResult>> result;
    ASSERT_EQ(component->intf()->config_vb({pools.get()}, C2_DONT_BLOCK, &result), C2_OK);
    ASSERT_EQ(result.size(), 0u);
    ASSERT_EQ(component->start(), C2_OK);

    std::atomic_bool running(true);
    std::thread listenerThread([this, &running]() {
        uint32_t iteration = 0;
        while (running) {
            std::unique_ptr<C2Work> work;
            {
                ULock l(mProcessedLock);
                if (mProcessedWork.empty()) {
                    mProcessedCondition.wait_for(l, 100ms);
                    if (mProcessedWork.empty()) {
                        continue;
                    }
                }
                work = std::move(mProcessedWork.front());
                mProcessedWork.pop_front();
            }
            mFinishedWorkCounts[iteration]++;
            ALOGV("Output: frame index: %" PRIu64 " result: %d outputs: %zu",
                  work->input.ordinal.frame_index, work->result,
                  work->worklets.front()->output.buffers.size());

            if (work->worklets_processed == 1u) {
                ASSERT_EQ(work->worklets.size(), 1u);
                ASSERT_EQ(work->worklets.front()->output.buffers.size(), 1u);
                std::shared_ptr<C2Buffer> output = work->worklets.front()->output.buffers[0];
                C2ConstGraphicBlock graphic_block = output->data().graphicBlocks().front();
                ASSERT_EQ(mTestVideoFile->mWidth, static_cast<int>(graphic_block.width()));
                ASSERT_EQ(mTestVideoFile->mHeight, static_cast<int>(graphic_block.height()));
                work->worklets.front()->output.buffers.clear();
                mOutputFrameCounts[iteration]++;
            }

            // input buffers should be cleared in component side.
            ASSERT_TRUE(work->input.buffers.empty());
            work->worklets.clear();
            work->worklets_processed = 0;

            bool iteration_end = work->input.flags & C2BufferPack::FLAG_END_OF_STREAM;
            if (iteration == 0 &&
                work->input.ordinal.frame_index == static_cast<uint64_t>(mFlushAfterWorkIndex)) {
                ULock l(mFlushDoneLock);
                mFlushDone = true;
                mFlushDoneCondition.notify_all();
                iteration_end = true;
            }

            ULock l(mQueueLock);
            mWorkQueue.emplace_back(std::move(work));
            mQueueCondition.notify_all();

            if (iteration_end) {
                iteration++;
                if (iteration == mNumberOfPlaythrough) {
                    running.store(false);  // stop the thread
                }
            }
        }
    });

    for (uint32_t iteration = 0; iteration < mNumberOfPlaythrough; ++iteration) {
        ASSERT_TRUE(getMediaSourceFromFile(mTestVideoFile->mFilename, mTestVideoFile->mCodec,
                                           &mTestVideoFile->mData));

        std::deque<sp<ABuffer>> csds;
        if (mTestVideoFile->mCodec == TestVideoFile::CodecType::H264) {
            // Get csd buffers for h264.
            sp<AMessage> format;
            (void)convertMetaDataToMessage(mTestVideoFile->mData->getFormat(), &format);
            csds.resize(2);
            format->findBuffer("csd-0", &csds[0]);
            format->findBuffer("csd-1", &csds[1]);
            ASSERT_TRUE(csds[0] != nullptr && csds[1] != nullptr);
        }

        ASSERT_EQ(mTestVideoFile->mData->start(), OK);

        int numWorks = 0;
        while (true) {
            size_t size = 0u;
            void* data = nullptr;
            int64_t timestamp = 0u;
            MediaBuffer* buffer = nullptr;
            sp<ABuffer> csd;
            if (!csds.empty()) {
                csd = std::move(csds.front());
                csds.pop_front();
                size = csd->size();
                data = csd->data();
            } else {
                if (mTestVideoFile->mData->read(&buffer) != OK) {
                    ASSERT_TRUE(buffer == nullptr);
                    ALOGV("Meet end of stream. Now drain the component.");
                    ASSERT_EQ(component->drain_nb(C2Component::DRAIN_COMPONENT_WITH_EOS), C2_OK);
                    break;
                }
                sp<MetaData> meta = buffer->meta_data();
                ASSERT_TRUE(meta->findInt64(kKeyTime, &timestamp));
                size = buffer->size();
                data = buffer->data();
            }

            std::unique_ptr<C2Work> work;
            while (!work) {
                ULock l(mQueueLock);
                if (!mWorkQueue.empty()) {
                    work = std::move(mWorkQueue.front());
                    mWorkQueue.pop_front();
                } else {
                    mQueueCondition.wait_for(l, 100ms);
                }
            }
            work->input.flags = static_cast<C2BufferPack::flags_t>(0);
            work->input.ordinal.timestamp = static_cast<uint64_t>(timestamp);
            work->input.ordinal.frame_index = static_cast<uint64_t>(numWorks);

            // Allocate input buffer.
            std::shared_ptr<C2LinearBlock> block;
            mLinearBlockPool->fetchLinearBlock(
                    size, {C2MemoryUsage::kSoftwareRead, C2MemoryUsage::kSoftwareWrite}, &block);
            C2WriteView view = block->map().get();
            ASSERT_EQ(view.error(), C2_OK);
            memcpy(view.base(), data, size);

            work->input.buffers.clear();
            work->input.buffers.emplace_back(new C2VDALinearBuffer(std::move(block)));
            work->worklets.clear();
            work->worklets.emplace_back(new C2Worklet);
            ALOGV("Input: bitstream id: %" PRIu64 " timestamp: %" PRIu64 " size: %zu",
                  work->input.ordinal.frame_index, work->input.ordinal.timestamp, size);

            std::list<std::unique_ptr<C2Work>> items;
            items.push_back(std::move(work));

            // Queue the work.
            ASSERT_EQ(component->queue_nb(&items), C2_OK);
            numWorks++;

            if (buffer) {
                buffer->release();
            }

            if (iteration == 0 && numWorks == mFlushAfterWorkIndex + 1) {
                // Perform flush.
                // Note: C2VDAComponent does not return work via |flushedWork|.
                ASSERT_EQ(component->flush_sm(C2Component::FLUSH_COMPONENT,
                                              nullptr /* flushedWork */),
                          C2_OK);
                break;
            }
        }

        if (iteration == 0 && mFlushAfterWorkIndex >= 0) {
            // Wait here until client get all flushed works.
            while (true) {
                ULock l(mFlushDoneLock);
                if (mFlushDone) {
                    break;
                }
                mFlushDoneCondition.wait_for(l, 100ms);
            }
            ALOGV("Got flush done signal");
            EXPECT_EQ(numWorks, mFlushAfterWorkIndex + 1);
        } else {
            EXPECT_EQ(numWorks, mTestVideoFile->mNumFragments);
        }
        ASSERT_EQ(mTestVideoFile->mData->stop(), OK);
    }

    listenerThread.join();
    ASSERT_EQ(running, false);
    ASSERT_EQ(component->stop(), C2_OK);

    // Finally check the decoding want as expected.
    for (uint32_t i = 0; i < mNumberOfPlaythrough; ++i) {
        if (mFlushAfterWorkIndex >= 0 && i == 0) {
            EXPECT_LE(mOutputFrameCounts[i], expectedOutputFrameCounts[i]) << "At iteration: " << i;
        } else {
            EXPECT_EQ(mOutputFrameCounts[i], expectedOutputFrameCounts[i]) << "At iteration: " << i;
        }
        EXPECT_EQ(mFinishedWorkCounts[i], expectedFinishedWorkCounts[i]) << "At iteration: " << i;
    }
}

// Play input video once.
INSTANTIATE_TEST_CASE_P(SinglePlaythroughTest, C2VDAComponentParamTest,
                        ::testing::Values(std::make_tuple(static_cast<int>(FlushPoint::NO_FLUSH),
                                                          1u)));

// Play 5 times of input video.
INSTANTIATE_TEST_CASE_P(MultiplePlaythroughTest, C2VDAComponentParamTest,
                        ::testing::Values(std::make_tuple(static_cast<int>(FlushPoint::NO_FLUSH),
                                                          5u)));

// Test mid-stream flush then play once entirely.
INSTANTIATE_TEST_CASE_P(FlushPlaythroughTest, C2VDAComponentParamTest,
                        ::testing::Values(std::make_tuple(40, 1u)));

// Test mid-stream flush then stop.
INSTANTIATE_TEST_CASE_P(
        FlushStopTest, C2VDAComponentParamTest,
        ::testing::Values(std::make_tuple(static_cast<int>(FlushPoint::MID_STREAM_FLUSH), 0u)));

// Test early flush (after a few works) then stop.
INSTANTIATE_TEST_CASE_P(EarlyFlushStopTest, C2VDAComponentParamTest,
                        ::testing::Values(std::make_tuple(0, 0u), std::make_tuple(1, 0u),
                                          std::make_tuple(2, 0u), std::make_tuple(3, 0u)));

// Test end-of-stream flush then stop.
INSTANTIATE_TEST_CASE_P(
        EndOfStreamFlushStopTest, C2VDAComponentParamTest,
        ::testing::Values(std::make_tuple(static_cast<int>(FlushPoint::END_OF_STREAM_FLUSH), 0u)));

}  // namespace android

static void usage(const char* me) {
    fprintf(stderr, "usage: %s [-h] [-i test_video_data] [gtest options]\n", me);
}

int main(int argc, char** argv) {
    extern int opterr;
    opterr = 0;  // avoid printing error because we want to ignore that and pass to gtest

    int res;
    bool escape = false;
    while (!escape && (res = getopt(argc, argv, "i:h")) >= 0) {
        switch (res) {
        case 'i': {
            android::gTestVideoData = optarg;
            break;
        }
        case 'h': {
            usage(argv[0]);
            exit(1);
            break;
        }
        default: {
            escape = true;
            optind--;  // go back to last argv
            break;
        }
        }
    }
    argc -= optind;
    argv += optind;

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
