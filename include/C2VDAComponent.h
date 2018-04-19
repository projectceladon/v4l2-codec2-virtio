// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VDA_COMPONENT_H
#define ANDROID_C2_VDA_COMPONENT_H

#include <VideoDecodeAcceleratorAdaptor.h>

#include <rect.h>
#include <size.h>
#include <video_codecs.h>
#include <video_decode_accelerator.h>

#include <C2Component.h>
#include <C2Param.h>

#include <base/macros.h>
#include <base/memory/ref_counted.h>
#include <base/single_thread_task_runner.h>
#include <base/synchronization/waitable_event.h>
#include <base/threading/thread.h>

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <queue>
#include <unordered_map>

namespace android {

C2ENUM(ColorFormat, uint32_t,  // enum for output color format
       kColorFormatYUV420Flexible = 0x7F420888, )

enum C2VDAParamIndexKind : C2Param::type_index_t {
    kParamIndexVDAProfile = kParamIndexParamStart + 1,
};

// Codec profile for VDA VideoCodecProfile (see vda/video_codecs.h) [IN]
// Note: this does not equal to AVC profile index
typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexVDAProfile> C2VDAStreamProfileConfig;

class C2VDAComponentIntf : public C2ComponentInterface {
public:
    C2VDAComponentIntf(C2String name, c2_node_id_t id);
    virtual ~C2VDAComponentIntf() {}

    // Impementation of C2ComponentInterface interface
    virtual C2String getName() const override;
    virtual c2_node_id_t getId() const override;
    virtual c2_status_t query_vb(
            const std::vector<C2Param*>& stackParams,
            const std::vector<C2Param::Index>& heapParamIndices, c2_blocking_t mayBlock,
            std::vector<std::unique_ptr<C2Param>>* const heapParams) const override;
    virtual c2_status_t config_vb(
            const std::vector<C2Param*>& params, c2_blocking_t mayBlock,
            std::vector<std::unique_ptr<C2SettingResult>>* const failures) override;
    virtual c2_status_t createTunnel_sm(c2_node_id_t targetComponent) override;
    virtual c2_status_t releaseTunnel_sm(c2_node_id_t targetComponent) override;
    virtual c2_status_t querySupportedParams_nb(
            std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const override;
    virtual c2_status_t querySupportedValues_vb(std::vector<C2FieldSupportedValuesQuery>& fields,
                                                c2_blocking_t mayBlock) const override;

    c2_status_t status() const;

private:
    friend class C2VDAComponent;

    const C2String kName;
    const c2_node_id_t kId;

    C2Param* getParamByIndex(uint32_t index) const;
    template <class T>
    std::unique_ptr<C2SettingResult> validateVideoSizeConfig(C2Param* c2Param) const;
    template <class T>
    std::unique_ptr<C2SettingResult> validateUint32Config(C2Param* c2Param) const;

    c2_status_t mInitStatus;

    // The following parameters are read-only.

    // The component domain; should be C2DomainVideo.
    C2ComponentDomainInfo mDomainInfo;
    // The input format kind; should be C2FormatCompressed.
    C2StreamFormatConfig::input mInputFormat;
    // The output format kind; should be C2FormatVideo.
    C2StreamFormatConfig::output mOutputFormat;
    // The MIME type of input port.
    std::unique_ptr<C2PortMimeConfig::input> mInputPortMime;
    // The MIME type of output port; should be MEDIA_MIMETYPE_VIDEO_RAW.
    std::unique_ptr<C2PortMimeConfig::output> mOutputPortMime;

    // The following parameters are also writable.

    // The input video codec profile.
    C2VDAStreamProfileConfig::input mInputCodecProfile;
    // Decoded video size for output.
    C2VideoSizeStreamInfo::output mVideoSize;
    // Max video size for video decoder.
    C2MaxVideoSizeHintPortSetting::input mMaxVideoSizeHint;
    // The directive of output block pool usage
    std::unique_ptr<C2PortBlockPoolsTuning::output> mOutputBlockPools;

    std::unordered_map<uint32_t, C2Param*> mParams;
    // C2ParamField is LessThanComparable
    std::map<C2ParamField, C2FieldSupportedValues> mSupportedValues;
    std::vector<std::shared_ptr<C2ParamDescriptor>> mParamDescs;

    media::VideoDecodeAccelerator::SupportedProfiles mSupportedProfiles;
    std::vector<uint32_t> mSupportedCodecProfiles;
};

class C2VDAComponent : public C2Component,
                       public VideoDecodeAcceleratorAdaptor::Client,
                       public std::enable_shared_from_this<C2VDAComponent> {
public:
    C2VDAComponent(C2String name, c2_node_id_t id);
    virtual ~C2VDAComponent() override;

    // Implementation of C2Component interface
    virtual c2_status_t setListener_vb(const std::shared_ptr<Listener>& listener,
                                       c2_blocking_t mayBlock) override;
    virtual c2_status_t queue_nb(std::list<std::unique_ptr<C2Work>>* const items) override;
    virtual c2_status_t announce_nb(const std::vector<C2WorkOutline>& items) override;
    virtual c2_status_t flush_sm(flush_mode_t mode,
                                 std::list<std::unique_ptr<C2Work>>* const flushedWork) override;
    virtual c2_status_t drain_nb(drain_mode_t mode) override;
    virtual c2_status_t start() override;
    virtual c2_status_t stop() override;
    virtual c2_status_t reset() override;
    virtual c2_status_t release() override;
    virtual std::shared_ptr<C2ComponentInterface> intf() override;

    // Implementation of VideDecodeAcceleratorAdaptor::Client interface
    virtual void providePictureBuffers(uint32_t minNumBuffers,
                                       const media::Size& codedSize) override;
    virtual void dismissPictureBuffer(int32_t pictureBufferId) override;
    virtual void pictureReady(int32_t pictureBufferId, int32_t bitstreamId,
                              const media::Rect& cropRect) override;
    virtual void notifyEndOfBitstreamBuffer(int32_t bitstreamId) override;
    virtual void notifyFlushDone() override;
    virtual void notifyResetDone() override;
    virtual void notifyError(VideoDecodeAcceleratorAdaptor::Result error) override;

private:
    // The state machine enumeration on parent thread.
    enum class State : int32_t {
        // The initial state of component. State will change to LOADED after the component is
        // created.
        UNLOADED,
        // The component is stopped. State will change to RUNNING when start() is called by
        // framework.
        LOADED,
        // The component is running, State will change to LOADED when stop() or reset() is called by
        // framework.
        RUNNING,
        // The component is in error state.
        ERROR,
    };
    // The state machine enumeration on component thread.
    enum class ComponentState : int32_t {
        // This is the initial state until VDA initialization returns successfully.
        UNINITIALIZED,
        // VDA initialization returns successfully. VDA is ready to make progress.
        STARTED,
        // onDrain() is called. VDA is draining. Component will hold on queueing works until
        // onDrainDone().
        DRAINING,
        // onFlush() is called. VDA is flushing. State will change to STARTED after onFlushDone().
        FLUSHING,
        // onStop() is called. VDA is shutting down. State will change to UNINITIALIZED after
        // onStopDone().
        STOPPING,
        // onError() is called.
        ERROR,
    };

    enum {
        kDpbOutputBufferExtraCount = 3,  // Use the same number as ACodec.
    };

    // This constant is used to tell apart from drain_mode_t enumerations in C2Component.h, which
    // means no drain request.
    // Note: this value must be different than all enumerations in drain_mode_t.
    static constexpr uint32_t NO_DRAIN = ~0u;

    // Internal struct for work queue.
    struct WorkEntry {
        std::unique_ptr<C2Work> mWork;
        uint32_t mDrainMode = NO_DRAIN;
    };

    // Internal struct to keep the information of a specific graphic block.
    struct GraphicBlockInfo {
        enum class State {
            OWNED_BY_COMPONENT,    // Owned by this component.
            OWNED_BY_ACCELERATOR,  // Owned by video decode accelerator.
            OWNED_BY_CLIENT,       // Owned by client.
        };

        int32_t mBlockId = -1;
        State mState = State::OWNED_BY_COMPONENT;
        // Graphic block buffer allocated from allocator. This should be reused.
        std::shared_ptr<C2GraphicBlock> mGraphicBlock;
        // HAL pixel format used while importing to VDA.
        HalPixelFormat mPixelFormat;
        // The handle dupped from graphic block for importing to VDA.
        base::ScopedFD mHandle;
        // VideoFramePlane information for importing to VDA.
        std::vector<VideoFramePlane> mPlanes;
    };

    struct VideoFormat {
        HalPixelFormat mPixelFormat = HalPixelFormat::UNKNOWN;
        uint32_t mMinNumBuffers = 0;
        media::Size mCodedSize;
        media::Rect mVisibleRect;

        VideoFormat() {}
        VideoFormat(HalPixelFormat pixelFormat, uint32_t minNumBuffers, media::Size codedSize,
                    media::Rect visibleRect);
    };

    // Get configured parameters from component interface. This should be called once framework
    // wants to start the component.
    void fetchParametersFromIntf();
    // Used as the release callback for C2VDAGraphicBuffer to get back the output buffer.
    void returnOutputBuffer(int32_t pictureBufferId);

    // These tasks should be run on the component thread |mThread|.
    void onDestroy();
    void onStart(media::VideoCodecProfile profile, base::WaitableEvent* done);
    void onQueueWork(std::unique_ptr<C2Work> work);
    void onDequeueWork();
    void onInputBufferDone(int32_t bitstreamId);
    void onOutputBufferDone(int32_t pictureBufferId, int32_t bitstreamId);
    void onDrain(uint32_t drainMode);
    void onDrainDone();
    void onFlush();
    void onStop(base::WaitableEvent* done);
    void onResetDone();
    void onFlushDone();
    void onStopDone();
    void onOutputFormatChanged(std::unique_ptr<VideoFormat> format);
    void onVisibleRectChanged(const media::Rect& cropRect);
    void onOutputBufferReturned(int32_t pictureBufferId);

    // Send input buffer to accelerator with specified bitstream id.
    void sendInputBufferToAccelerator(const C2ConstLinearBlock& input, int32_t bitstreamId);
    // Send output buffer to accelerator.
    void sendOutputBufferToAccelerator(GraphicBlockInfo* info);
    // Set crop rectangle infomation to output format.
    void setOutputFormatCrop(const media::Rect& cropRect);
    // Helper function to get the specified GraphicBlockInfo object by its id.
    GraphicBlockInfo* getGraphicBlockById(int32_t blockId);
    // Helper function to get the specified work in mPendingWorks by bitstream id.
    C2Work* getPendingWorkByBitstreamId(int32_t bitstreamId);
    // Try to apply the output format change.
    void tryChangeOutputFormat();
    // Allocate output buffers (graphic blocks) from block allocator.
    c2_status_t allocateBuffersFromBlockAllocator(const media::Size& size, uint32_t pixelFormat);
    // Append allocated buffer (graphic block) to mGraphicBlocks.
    void appendOutputBuffer(std::shared_ptr<C2GraphicBlock> block);

    // Check for finished works in mPendingWorks. If any, make onWorkDone call to listener.
    void reportFinishedWorkIfAny();
    // Make onWorkDone call to listener for reporting EOS work in mPendingWorks.
    void reportEOSWork();
    // Abandon all works in mPendingWorks.
    void reportAbandonedWorks();
    // Make onError call to listener for reporting errors.
    void reportError(c2_status_t error);
    // Helper function to determine if the work is finished.
    bool isWorkDone(const C2Work* work) const;

    // The pointer of component interface.
    const std::shared_ptr<C2VDAComponentIntf> mIntf;
    // The pointer of component listener.
    std::shared_ptr<Listener> mListener;

    // The main component thread.
    base::Thread mThread;
    // The task runner on component thread.
    scoped_refptr<base::SingleThreadTaskRunner> mTaskRunner;

    // The following members should be utilized on component thread |mThread|.

    // The initialization result retrieved from VDA.
    VideoDecodeAcceleratorAdaptor::Result mVDAInitResult;
    // The pointer of VideoDecodeAcceleratorAdaptor.
    std::unique_ptr<VideoDecodeAcceleratorAdaptor> mVDAAdaptor;
    // The done event pointer of stop procedure. It should be restored in onStop() and signaled in
    // onStopDone().
    base::WaitableEvent* mStopDoneEvent;
    // The state machine on component thread.
    ComponentState mComponentState;
    // The indicator of drain mode (true for draining with EOS). This should be always set along
    // with component going to DRAINING state, and only regarded under DRAINING state.
    bool mDrainWithEOS;
    // The vector of storing allocated output graphic block information.
    std::vector<GraphicBlockInfo> mGraphicBlocks;
    // The work queue. Works are queued along with drain mode from component API queue_nb and
    // dequeued by the decode process of component.
    std::queue<WorkEntry> mQueue;
    // Store all pending works. The dequeued works are placed here until they are finished and then
    // sent out by onWorkDone call to listener.
    std::deque<std::unique_ptr<C2Work>> mPendingWorks;
    // Store the visible rect provided from VDA. If this is changed, component should issue a
    // visible size change event.
    media::Rect mRequestedVisibleRect;
    // The current output format.
    VideoFormat mOutputFormat;
    // The pending output format. We need to wait until all buffers are returned back to apply the
    // format change.
    std::unique_ptr<VideoFormat> mPendingOutputFormat;
    // The current color format.
    uint32_t mColorFormat;
    // Record the timestamp of the last output buffer. This is used to determine if the work is
    // finished.
    int64_t mLastOutputTimestamp;
    // The pointer of output block pool.
    std::shared_ptr<C2BlockPool> mOutputBlockPool;

    // The following members should be utilized on parent thread.

    // The input codec profile which is configured in component interface.
    media::VideoCodecProfile mCodecProfile;
    // The state machine on parent thread which should be atomic.
    std::atomic<State> mState;
    // The mutex lock to synchronize start/stop/reset/release calls.
    std::mutex mStartStopLock;

    // The WeakPtrFactory for getting weak pointer of this.
    base::WeakPtrFactory<C2VDAComponent> mWeakThisFactory;

    DISALLOW_COPY_AND_ASSIGN(C2VDAComponent);
};

}  // namespace android

#endif  // ANDROID_C2_VDA_COMPONENT_H
