// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VEA_COMPONENT_H
#define ANDROID_C2_VEA_COMPONENT_H

#include <VideoEncodeAcceleratorAdaptor.h>

#include <size.h>

#include <C2Component.h>
#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>

#include <base/macros.h>
#include <base/memory/ref_counted.h>
#include <base/single_thread_task_runner.h>
#include <base/synchronization/waitable_event.h>
#include <base/threading/thread.h>

#include <atomic>
#include <memory>

namespace android {

class C2VEAComponent : public C2Component,
                       public VideoEncodeAcceleratorAdaptor::Client,
                       public std::enable_shared_from_this<C2VEAComponent> {
public:
    class IntfImpl : public C2InterfaceHelper {
    public:
        IntfImpl(C2String name, const std::shared_ptr<C2ReflectorHelper>& helper,
                 std::unique_ptr<VideoEncodeAcceleratorAdaptor>* const adaptor /* nonnull */);

        // Interfaces for C2VEAComponent
        // Note: these getters are not thread-safe. For dynamic parameters, component should use
        // formal query API for C2ComponentInterface instead.
        c2_status_t status() const { return mInitStatus; }
        C2Config::profile_t getOutputProfile() const { return mProfileLevel->profile; }
        C2Config::level_t getOutputLevel() const { return mProfileLevel->level; }
        const media::Size getInputVisibleSize() const {
            return media::Size(mInputVisibleSize->width, mInputVisibleSize->height);
        }
        C2BlockPool::local_id_t getBlockPoolId() const { return mOutputBlockPoolIds->m.values[0]; }
        // Get sync key-frame period in frames.
        uint32_t getKeyFramePeriod() const;

    private:
        // Configurable parameter setters.
        static C2R ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::output>& info,
                                      const C2P<C2StreamPictureSizeInfo::input>& videosize,
                                      const C2P<C2StreamFrameRateInfo::output>& frameRate,
                                      const C2P<C2StreamBitrateInfo::output>& bitrate);

        static C2R SizeSetter(bool mayBlock, C2P<C2StreamPictureSizeInfo::input>& videoSize);

        static C2R IntraRefreshPeriodSetter(bool mayBlock,
                                            C2P<C2StreamIntraRefreshTuning::output>& period);

        // Constant parameters

        // The input format kind; should be C2FormatVideo.
        std::shared_ptr<C2StreamBufferTypeSetting::input> mInputFormat;
        // The output format kind; should be C2FormatCompressed.
        std::shared_ptr<C2StreamBufferTypeSetting::output> mOutputFormat;
        // The MIME type of input port; should be MEDIA_MIMETYPE_VIDEO_RAW.
        std::shared_ptr<C2PortMediaTypeSetting::input> mInputMediaType;
        // The MIME type of output port.
        std::shared_ptr<C2PortMediaTypeSetting::output> mOutputMediaType;

        // The suggested usage of input buffer allocator ID.
        std::shared_ptr<C2PortAllocatorsTuning::input> mInputAllocatorIds;
        // The suggested usage of output buffer allocator ID.
        std::shared_ptr<C2PortAllocatorsTuning::output> mOutputAllocatorIds;

        // Initialization parameters

        // The visible size for input raw video.
        std::shared_ptr<C2StreamPictureSizeInfo::input> mInputVisibleSize;
        // The output codec profile and level.
        std::shared_ptr<C2StreamProfileLevelInfo::output> mProfileLevel;
        // The expected period for key frames in microseconds.
        std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> mKeyFramePeriodUs;

        // Compnent uses this ID to fetch corresponding output block pool from platform.
        std::shared_ptr<C2PortBlockPoolsTuning::output> mOutputBlockPoolIds;

        // Dynamic parameters

        // The requested bitrate of the encoded output stream, in bits per second.
        std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
        // The requested framerate, in frames per second.
        std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
        // The switch-type parameter that will be set to true while client requests keyframe. It
        // will be reset once encoder gets the request.
        std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestKeyFrame;
        // The intra-frame refresh period. This is unused for the component now.
        // TODO: adapt intra refresh period to encoder.
        std::shared_ptr<C2StreamIntraRefreshTuning::output> mIntraRefreshPeriod;

        c2_status_t mInitStatus;
    };

    C2VEAComponent(C2String name, c2_node_id_t id,
                   const std::shared_ptr<C2ReflectorHelper>& helper);
    virtual ~C2VEAComponent() override;

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
    virtual void notifyVideoFrameDone(int64_t timestamp) override;
    virtual void bitstreamBufferReady(uint32_t payloadSize, bool keyFrame,
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
        // VDA initialization returns successfully. VEA is ready to make progress.
        STARTED,
        // onDrain() is called. VEA is draining. Component will hold on queueing works until
        // onDrainDone().
        DRAINING,
        // onFlush() is called, VEA performs flush and state will change to FLUSHED on leave.
        FLUSHED,
        // onError() is called.
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

    // The pointer of VideoEncodeAcceleratorAdaptor.
    std::unique_ptr<VideoEncodeAcceleratorAdaptor> mVEAAdaptor;

    // The pointer of component interface implementation.
    std::shared_ptr<IntfImpl> mIntfImpl;
    // The pointer of component interface.
    const std::shared_ptr<C2ComponentInterface> mIntf;
    // The pointer of component listener.
    std::shared_ptr<Listener> mListener;

    // The main component thread.
    ::base::Thread mThread;
    // The task runner on component thread.
    scoped_refptr<::base::SingleThreadTaskRunner> mTaskRunner;

    // The following members should be utilized on component thread |mThread|.

    // The state machine on component thread.
    //ComponentState mComponentState;

    // The following members should be utilized on parent thread.

    // The state machine on parent thread which should be atomic.
    std::atomic<State> mState;

    // The WeakPtrFactory for getting weak pointer of this.
    ::base::WeakPtrFactory<C2VEAComponent> mWeakThisFactory;

    DISALLOW_COPY_AND_ASSIGN(C2VEAComponent);
};

}  // namespace android

#endif  // ANDROID_C2_VEA_COMPONENT_H
