// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VEA_COMPONENT_H
#define ANDROID_C2_VEA_COMPONENT_H

#include <atomic>
#include <map>
#include <memory>

#include <C2Component.h>
#include <base/memory/weak_ptr.h>
#include <base/optional.h>
#include <base/single_thread_task_runner.h>
#include <base/synchronization/waitable_event.h>
#include <base/threading/thread.h>

#include <size.h>
#include <C2EncoderInterface.h>
#include <VideoEncodeAcceleratorAdaptor.h>

class C2ReflectorHelper;

namespace android {

class FormatConverter;

class C2VEAComponent : public C2Component,
                       public VideoEncodeAcceleratorAdaptor::Client,
                       public std::enable_shared_from_this<C2VEAComponent> {
public:
    class IntfImpl : public C2EncoderInterface {
    public:
        IntfImpl(C2String name, const std::shared_ptr<C2ReflectorHelper>& helper);

    protected:
        base::Optional<media::VideoCodec> getCodecFromComponentName(
                const std::string& name) const override;
    };

    C2VEAComponent(C2String name, c2_node_id_t id,
                   const std::shared_ptr<C2ReflectorHelper>& helper);
    virtual ~C2VEAComponent() override;

    // Disable copy and assign.
    C2VEAComponent(const C2VEAComponent&) = delete;
    C2VEAComponent& operator=(const C2VEAComponent&) = delete;

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

    // Implementation of VideEecodeAcceleratorAdaptor::Client interface
    virtual void requireBitstreamBuffers(uint32_t inputCount, const media::Size& inputCodedSize,
                                         uint32_t outputBufferSize) override;
    virtual void notifyVideoFrameDone(uint64_t index) override;
    virtual void bitstreamBufferReady(uint64_t index, uint32_t payloadSize, bool keyFrame,
                                      int64_t timestamp) override;
    virtual void notifyFlushDone(bool done) override;
    virtual void notifyError(VideoEncodeAcceleratorAdaptor::Result error) override;

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
        // This is the initial state until VEA initialization returns successfully.
        UNINITIALIZED,
        // VEA initialization has returned successfully. Component is still waiting for
        // requireBitstreamBuffers callback.
        CONFIGURED,
        // requireBitstreamBuffers callback has received. VEA is ready to make progress.
        STARTED,
        // onDrain() is called. VEA is draining. Component will hold on queueing works until
        // onDrainDone().
        DRAINING,
        // reportError() is called. Component will stay in the error state unless stop() is called.
        ERROR,
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

    // These tasks should be run on the component thread |mThread|.
    void onDestroy();
    void onStart(::base::WaitableEvent* done);
    void onDrain(uint32_t drainMode);
    void onDrainDone(bool done);
    void onFlush(bool reinitAdaptor);
    void onStop(::base::WaitableEvent* done);
    void onRequireBitstreamBuffers(uint32_t inputCount, const media::Size& inputCodedSize,
                                   uint32_t outputBufferSize);
    void onQueueWork(std::unique_ptr<C2Work> work);
    void onDequeueWork();
    void onInputBufferDone(uint64_t index);
    void onOutputBufferDone(uint64_t index, uint32_t payloadSize, bool keyFrame, int64_t timestamp);

    // Initialize VEA with configuration from the component interface.
    VideoEncodeAcceleratorAdaptor::Result initializeVEA();
    // Update |mRequestedBitrate| and |mRequestedFrameRate| from the component interface.
    // Return true if there is any value changed. Component will need to call
    // requestEncodingParametersChange() to VEA for updating new values.
    bool updateEncodingParametersIfChanged();
    // Send input buffer |inputBlock| to accelerator for encode with corresponding |index| and
    // |timestamp|, and set |keyframe| to true on key frame request.
    void sendInputBufferToAccelerator(const C2ConstGraphicBlock& inputBlock, uint64_t index,
                                      int64_t timestamp, bool keyframe);
    // Helper function to find the work iterator in |mPendingWorks| by frame index.
    std::deque<std::unique_ptr<C2Work>>::iterator findPendingWorkByIndex(uint64_t index);
    // Helper function to get the specified work in |mPendingWorks| by frame index.
    C2Work* getPendingWorkByIndex(uint64_t index);
    // Helper function to get the specified work in |mPendingWorks| with the same |timestamp|.
    // Note that EOS and CSD-holder work should be excluded because its timestmap is not meaningful.
    C2Work* getPendingWorkByTimestamp(int64_t timestamp);
    // Helper function to determine if work queue is flushed. This is used to indicate that returned
    // input or output buffer from VEA is no longer needed.
    bool isFlushedState() const;
    // Check if the corresponding work is finished by |index|. If yes, make onWorkDone call to
    // listener and erase the work from |mPendingWorks|.
    void reportWorkIfFinished(uint64_t index);
    // Report the work by onWorkDone call.
    void reportWork(std::unique_ptr<C2Work> work);
    // Helper function to determine if the work is finished.
    bool isWorkDone(const C2Work* work) const;
    // Make onWorkDone call to listener for reporting EOS work in |mPendingWorks|.
    void reportEOSWork();
    // Abandon all works in |mPendingWorks| and |mAbandonedWorks|.
    void reportAbandonedWorks();
    // Make onError call to listener for reporting errors.
    void reportError(c2_status_t error);

    // The pointer of VideoEncodeAcceleratorAdaptor. It should be initialized by the ctor of
    // |mIntfImpl| and used for getSupportProfiles. Then it will be owned by component and utilized
    // on component thread |mThread|.
    // Note: this must be placed before |mIntfImpl| definition due to member variable init order.
    std::unique_ptr<VideoEncodeAcceleratorAdaptor> mVEAAdaptor;

    // The pointer of component interface implementation.
    std::shared_ptr<IntfImpl> mIntfImpl;
    // The pointer of component interface.
    std::shared_ptr<C2ComponentInterface> mIntf;
    // The pointer of component listener.
    std::shared_ptr<Listener> mListener;

    // The main component thread.
    ::base::Thread mThread;
    // The task runner on component thread.
    scoped_refptr<::base::SingleThreadTaskRunner> mTaskRunner;

    // The following members should be utilized on component thread |mThread|.

    // The initialization result retrieved from VEA.
    VideoEncodeAcceleratorAdaptor::Result mVEAInitResult;
    // The done event pointer of start procedure. It should be restored in onStart() and signaled in
    // onRequireBitstreamBuffers().
    ::base::WaitableEvent* mStartDoneEvent;
    // The state machine on component thread.
    ComponentState mComponentState;
    // The work queue. Works are queued along with drain mode from component API queue_nb and
    // dequeued by the encode process of component.
    std::queue<WorkEntry> mQueue;

    // Store the output buffer size specified by VEA on RequireBitstreamBuffers.
    uint32_t mOutputBufferSize = 0;

    // Currently requested bitrate. It would be updated as the value in component interface on
    // updateEncodingParametersIfChanged().
    uint32_t mRequestedBitrate = 0;
    // Currently requested frame rate. It would be updated as the value in component interface on
    // updateEncodingParametersIfChanged().
    uint32_t mRequestedFrameRate = 0;

    // Store the key frame period in frames from the component interface.
    uint32_t mKeyFramePeriod = 0;
    // The counter for determining whether an input frame is key frame. It has range
    // [0, |mKeyFramePeriod|-1] and is circularly increased for each sendInputBufferToAccelerator().
    // The current input will be marked as key frame when |mKeyFrameSerial| is 0.
    uint32_t mKeyFrameSerial = 0;

    // The indicator for extracting CSD info. Set to true once CSD info is extracted and submitted.
    bool mCSDSubmitted = false;
    // The output block pool.
    std::shared_ptr<C2BlockPool> mOutputBlockPool;
    // Store the mapping table for the allocated linear output block with corresponding index. Each
    // newly-allocated output block will be stored here first, and moved to the corresponding work
    // in respect of timestamp when the output buffer is returned from VEA.
    std::map<uint64_t, std::shared_ptr<C2LinearBlock>> mOutputBlockMap;
    // The indicator of draining with EOS. This should be always set along with component going to
    // DRAINING state, and will be unset either after reportEOSWork() (EOS is outputted), or
    // reportAbandonedWorks() (drain is cancelled and works are abandoned).
    bool mPendingOutputEOS = false;
    // Store all pending works. The dequeued works are placed here until they are finished and then
    // sent out by onWorkDone call to listener.
    std::deque<std::unique_ptr<C2Work>> mPendingWorks;
    // If using format converter for input frames, this will be initialized for converting input
    // frames to the default pixel format (onto the additional buffers) to send to VEA.
    std::unique_ptr<FormatConverter> mFormatConverter;

    // The following members should be utilized on parent thread.

    // The state machine on parent thread which should be atomic.
    std::atomic<State> mState;
    // The mutex lock to synchronize start/stop/reset/release calls.
    std::mutex mStartStopLock;

    // The WeakPtrFactory for getting weak pointer of this.
    ::base::WeakPtrFactory<C2VEAComponent> mWeakThisFactory;
};

}  // namespace android

#endif  // ANDROID_C2_VEA_COMPONENT_H
