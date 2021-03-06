// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Note: ported from Chromium commit head: 91175b1
// Note: image processor is not ported.

#define ATRACE_TAG ATRACE_TAG_VIDEO

#include "v4l2_video_decode_accelerator.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <utils/Trace.h>

#include <numeric>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/memory/ptr_util.h"
#include "base/message_loop/message_loop.h"
#include "base/numerics/safe_conversions.h"
#include "base/posix/eintr_wrapper.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"

#include "generic_v4l2_device.h"
#include "macros.h"
#include "rect.h"
#include "shared_memory_region.h"

#define NOTIFY_ERROR(x)                      \
  do {                                       \
    VLOGF(1) << "Setting error state:" << x; \
    SetErrorState(x);                        \
  } while (0)

#define IOCTL_OR_ERROR_RETURN_VALUE(type, arg, value, type_str) \
  do {                                                          \
    if (device_->Ioctl(type, arg) != 0) {                       \
      VPLOGF(1) << "ioctl() failed: " << type_str;              \
      NOTIFY_ERROR(PLATFORM_FAILURE);                           \
      return value;                                             \
    }                                                           \
  } while (0)

#define IOCTL_OR_ERROR_RETURN(type, arg) \
  IOCTL_OR_ERROR_RETURN_VALUE(type, arg, ((void)0), #type)

#define IOCTL_OR_ERROR_RETURN_FALSE(type, arg) \
  IOCTL_OR_ERROR_RETURN_VALUE(type, arg, false, #type)

#define IOCTL_OR_LOG_ERROR(type, arg)           \
  do {                                          \
    if (device_->Ioctl(type, arg) != 0)         \
      VPLOGF(1) << "ioctl() failed: " << #type; \
  } while (0)

namespace media {

namespace {
// Copied from older version of V4L2 device.
VideoPixelFormat V4L2PixFmtToVideoPixelFormat(uint32_t pix_fmt) {
  switch (pix_fmt) {
    case V4L2_PIX_FMT_NV12:
    case V4L2_PIX_FMT_NV12M:
      return PIXEL_FORMAT_NV12;

    case V4L2_PIX_FMT_YUV420:
    case V4L2_PIX_FMT_YUV420M:
      return PIXEL_FORMAT_I420;

    case V4L2_PIX_FMT_YVU420:
      return PIXEL_FORMAT_YV12;

    case V4L2_PIX_FMT_YUV422M:
      return PIXEL_FORMAT_I422;

    case V4L2_PIX_FMT_RGB32:
      return PIXEL_FORMAT_ARGB;

    default:
      DVLOGF(1) << "Add more cases as needed";
      return PIXEL_FORMAT_UNKNOWN;
  }
}
}  // namespace

// static
const uint32_t V4L2VideoDecodeAccelerator::supported_input_fourccs_[] = {
    V4L2_PIX_FMT_H264, V4L2_PIX_FMT_VP8, V4L2_PIX_FMT_VP9,
};

struct V4L2VideoDecodeAccelerator::BitstreamBufferRef {
  BitstreamBufferRef(
      base::WeakPtr<Client>& client,
      scoped_refptr<base::SingleThreadTaskRunner>& client_task_runner,
      BitstreamBuffer buffer,
      int32_t input_id);
  ~BitstreamBufferRef();
  const base::WeakPtr<Client> client;
  const scoped_refptr<base::SingleThreadTaskRunner> client_task_runner;
  base::ScopedFD dmabuf_fd;
  const size_t offset;
  const size_t size;
  const int32_t input_id;
};

V4L2VideoDecodeAccelerator::BitstreamBufferRef::BitstreamBufferRef(
    base::WeakPtr<Client>& client,
    scoped_refptr<base::SingleThreadTaskRunner>& client_task_runner,
    BitstreamBuffer buffer,
    int32_t input_id)
    : client(client),
      client_task_runner(client_task_runner),
      offset(buffer.offset()),
      size(buffer.size()),
      input_id(input_id) {
  base::SharedMemoryHandle handle = buffer.handle();
  // NOTE: BitstreamBuffer and SharedMemoryHandle don't own file descriptor.
  // There is no need of duplicating the file descriptor here.
  // |handle| is invalid only if flush is dummy.
  DCHECK(handle.IsValid() || input_id == kFlushBufferId);
  if (handle.IsValid())
    dmabuf_fd = base::ScopedFD(handle.GetHandle());
}

V4L2VideoDecodeAccelerator::BitstreamBufferRef::~BitstreamBufferRef() {
  if (input_id >= 0) {
    client_task_runner->PostTask(
        FROM_HERE,
        base::Bind(&Client::NotifyEndOfBitstreamBuffer, client, input_id));
  }
}

V4L2VideoDecodeAccelerator::OutputRecord::OutputRecord()
    : state(kFree),
      picture_id(-1),
      cleared(false) {}

V4L2VideoDecodeAccelerator::OutputRecord::~OutputRecord() {}

V4L2VideoDecodeAccelerator::PictureRecord::PictureRecord(bool cleared,
                                                         const Picture& picture)
    : cleared(cleared), picture(picture) {}

V4L2VideoDecodeAccelerator::PictureRecord::~PictureRecord() {}

V4L2VideoDecodeAccelerator::V4L2VideoDecodeAccelerator(
    const scoped_refptr<V4L2Device>& device)
    : child_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      decoder_thread_("V4L2DecoderThread"),
      decoder_state_(kUninitialized),
      output_mode_(Config::OutputMode::ALLOCATE),
      device_(device),
      decoder_delay_bitstream_buffer_id_(-1),
      decoder_decode_buffer_tasks_scheduled_(0),
      decoder_frames_at_client_(0),
      decoder_flushing_(false),
      decoder_cmd_supported_(false),
      flush_awaiting_last_output_buffer_(false),
      reset_pending_(false),
      input_streamon_(false),
      input_buffer_queued_count_(0),
      input_buffer_size_(0),
      output_streamon_(false),
      output_buffer_queued_count_(0),
      output_dpb_size_(0),
      output_planes_count_(0),
      picture_clearing_count_(0),
      device_poll_thread_("V4L2DevicePollThread"),
      video_profile_(VIDEO_CODEC_PROFILE_UNKNOWN),
      input_format_fourcc_(0),
      output_format_fourcc_(0),
      weak_this_factory_(this) {
  weak_this_ = weak_this_factory_.GetWeakPtr();
}

V4L2VideoDecodeAccelerator::~V4L2VideoDecodeAccelerator() {
  DCHECK(!decoder_thread_.IsRunning());
  DCHECK(!device_poll_thread_.IsRunning());
  DVLOGF(2);

  // These maps have members that should be manually destroyed, e.g. file
  // descriptors, mmap() segments, etc.
  DCHECK(input_buffer_map_.empty());
  DCHECK(output_buffer_map_.empty());
}

bool V4L2VideoDecodeAccelerator::Initialize(const Config& config,
                                            Client* client) {
  VLOGF(2) << "profile: " << config.profile
           << ", output_mode=" << static_cast<int>(config.output_mode);
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(decoder_state_, kUninitialized);

  if (config.output_mode != Config::OutputMode::IMPORT) {
    NOTREACHED() << "Only IMPORT OutputModes are supported";
    return false;
  }

  client_ptr_factory_.reset(new base::WeakPtrFactory<Client>(client));
  client_ = client_ptr_factory_->GetWeakPtr();
  // If we haven't been set up to decode on separate thread via
  // TryToSetupDecodeOnSeparateThread(), use the main thread/client for
  // decode tasks.
  if (!decode_task_runner_) {
    decode_task_runner_ = child_task_runner_;
    DCHECK(!decode_client_);
    decode_client_ = client_;
  }

  video_profile_ = config.profile;

  input_format_fourcc_ =
      V4L2Device::VideoCodecProfileToV4L2PixFmt(video_profile_, false);

  if (!device_->Open(V4L2Device::Type::kDecoder, input_format_fourcc_)) {
    VLOGF(1) << "Failed to open device for profile: " << config.profile
             << " fourcc: " << std::hex << "0x" << input_format_fourcc_;
    return false;
  }

  // Capabilities check.
  struct v4l2_capability caps;
  const __u32 kCapsRequired = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_QUERYCAP, &caps);
  if ((caps.capabilities & kCapsRequired) != kCapsRequired) {
    VLOGF(1) << "ioctl() failed: VIDIOC_QUERYCAP"
             << ", caps check failed: 0x" << std::hex << caps.capabilities;
    return false;
  }

  if (!SetupFormats())
    return false;

  if (!decoder_thread_.Start()) {
    VLOGF(1) << "decoder thread failed to start";
    return false;
  }

  decoder_state_ = kInitialized;
  output_mode_ = config.output_mode;

  // InitializeTask will NOTIFY_ERROR on failure.
  decoder_thread_.task_runner()->PostTask(
      FROM_HERE, base::Bind(&V4L2VideoDecodeAccelerator::InitializeTask,
                            base::Unretained(this)));

  return true;
}

void V4L2VideoDecodeAccelerator::InitializeTask() {
  VLOGF(2);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_EQ(decoder_state_, kInitialized);

  // Subscribe to the resolution change event.
  struct v4l2_event_subscription sub;
  memset(&sub, 0, sizeof(sub));
  sub.type = V4L2_EVENT_SOURCE_CHANGE;
  IOCTL_OR_ERROR_RETURN(VIDIOC_SUBSCRIBE_EVENT, &sub);

  if (!CreateInputBuffers()) {
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  decoder_cmd_supported_ = IsDecoderCmdSupported();

  if (!StartDevicePoll())
    return;
}

void V4L2VideoDecodeAccelerator::Decode(
    const BitstreamBuffer& bitstream_buffer) {
  DVLOGF(4) << "input_id=" << bitstream_buffer.id()
            << ", size=" << bitstream_buffer.size();
  DCHECK(decode_task_runner_->BelongsToCurrentThread());

  if (bitstream_buffer.id() < 0) {
    VLOGF(1) << "Invalid bitstream_buffer, id: " << bitstream_buffer.id();
    if (base::SharedMemory::IsHandleValid(bitstream_buffer.handle()))
      base::SharedMemory::CloseHandle(bitstream_buffer.handle());
    NOTIFY_ERROR(INVALID_ARGUMENT);
    return;
  }

  // DecodeTask() will take care of running a DecodeBufferTask().
  decoder_thread_.task_runner()->PostTask(
      FROM_HERE, base::Bind(&V4L2VideoDecodeAccelerator::DecodeTask,
                            base::Unretained(this), bitstream_buffer));
}

void V4L2VideoDecodeAccelerator::AssignPictureBuffers(
    const std::vector<PictureBuffer>& buffers) {
  VLOGF(2) << "buffer_count=" << buffers.size();
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  decoder_thread_.task_runner()->PostTask(
      FROM_HERE,
      base::Bind(&V4L2VideoDecodeAccelerator::AssignPictureBuffersTask,
                 base::Unretained(this), buffers));
}

void V4L2VideoDecodeAccelerator::AssignPictureBuffersTask(
    const std::vector<PictureBuffer>& buffers) {
  VLOGF(2);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_EQ(decoder_state_, kAwaitingPictureBuffers);
  DCHECK(!output_streamon_);

  uint32_t req_buffer_count = output_dpb_size_ + kDpbOutputBufferExtraCount;

  if (buffers.size() < req_buffer_count) {
    VLOGF(1) << "Failed to provide requested picture buffers. (Got "
             << buffers.size() << ", requested " << req_buffer_count << ")";
    NOTIFY_ERROR(INVALID_ARGUMENT);
    return;
  }

  // S_FMT on output queue if frame size allocated by gralloc is different from
  // the frame size given by driver. NOTE: This S_FMT is not needed if memory
  // type in output queue is MMAP because the driver allocates memory.
  const Size& allocated_coded_size = buffers[0].size();
  if (allocated_coded_size != coded_size_) {
    struct v4l2_format format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.width = allocated_coded_size.width();
    format.fmt.pix_mp.height = allocated_coded_size.height();
    format.fmt.pix_mp.pixelformat = output_format_fourcc_;
    format.fmt.pix_mp.num_planes = output_planes_count_;
    IOCTL_OR_ERROR_RETURN(VIDIOC_S_FMT, &format);
    coded_size_.SetSize(format.fmt.pix_mp.width, format.fmt.pix_mp.height);
    const Size& new_visible_size = GetVisibleSize(coded_size_);
    if (new_visible_size != visible_size_) {
      VLOGF(1) << "Visible size is changed by resetting coded_size,"
               << "the previous visible size=" << visible_size_.ToString()
               << "the current visible size=" << new_visible_size.ToString();
      NOTIFY_ERROR(PLATFORM_FAILURE);
      return;
    }
  }

  // Allocate the output buffers.
  struct v4l2_requestbuffers reqbufs;
  memset(&reqbufs, 0, sizeof(reqbufs));
  reqbufs.count = buffers.size();
  reqbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  reqbufs.memory = V4L2_MEMORY_DMABUF;
  IOCTL_OR_ERROR_RETURN(VIDIOC_REQBUFS, &reqbufs);

  if (reqbufs.count < buffers.size()) {
    VLOGF(1) << "Could not allocate enough output buffers";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  DCHECK(free_output_buffers_.empty());
  DCHECK(output_buffer_map_.empty());
  output_buffer_map_.resize(buffers.size());

  // Always use IMPORT output mode for Android solution.
  DCHECK_EQ(output_mode_, Config::OutputMode::IMPORT);

  for (size_t i = 0; i < output_buffer_map_.size(); ++i) {
    OutputRecord& output_record = output_buffer_map_[i];
    DCHECK_EQ(output_record.state, kFree);
    DCHECK_EQ(output_record.picture_id, -1);
    DCHECK_EQ(output_record.cleared, false);

    output_record.picture_id = buffers[i].id();

    // This will remain kAtClient until ImportBufferForPicture is called, either
    // by the client, or by ourselves, if we are allocating.
    output_record.state = kAtClient;

    DVLOGF(3) << "buffer[" << i << "]: picture_id=" << output_record.picture_id;
  }
}

void V4L2VideoDecodeAccelerator::ImportBufferForPicture(
    int32_t picture_buffer_id,
    VideoPixelFormat pixel_format,
    const NativePixmapHandle& native_pixmap_handle) {
  DVLOGF(3) << "picture_buffer_id=" << picture_buffer_id;
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  if (output_mode_ != Config::OutputMode::IMPORT) {
    VLOGF(1) << "Cannot import in non-import mode";
    NOTIFY_ERROR(INVALID_ARGUMENT);
    return;
  }

  if (pixel_format != V4L2PixFmtToVideoPixelFormat(output_format_fourcc_)) {
    VLOGF(1) << "Unsupported import format: " << pixel_format;
    NOTIFY_ERROR(INVALID_ARGUMENT);
    return;
  }

  std::vector<base::ScopedFD> dmabuf_fds;
  std::vector<size_t> offsets;
  for (const auto& plane : native_pixmap_handle.planes)
      offsets.push_back(plane.offset);

  for (const auto& fd : native_pixmap_handle.fds) {
    DCHECK_NE(fd.fd, -1);
    dmabuf_fds.push_back(base::ScopedFD(fd.fd));
  }

  decoder_thread_.task_runner()->PostTask(
      FROM_HERE,
      base::Bind(&V4L2VideoDecodeAccelerator::ImportBufferForPictureTask,
                 base::Unretained(this), picture_buffer_id,
                 std::move(offsets), base::Passed(&dmabuf_fds)));
}

void V4L2VideoDecodeAccelerator::ImportBufferForPictureTask(
    int32_t picture_buffer_id,
    std::vector<size_t> offsets,
    std::vector<base::ScopedFD> dmabuf_fds) {
  DVLOGF(3) << "picture_buffer_id=" << picture_buffer_id
            << ", dmabuf_fds.size()=" << dmabuf_fds.size();
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());

  const auto iter =
      std::find_if(output_buffer_map_.begin(), output_buffer_map_.end(),
                   [picture_buffer_id](const OutputRecord& output_record) {
                     return output_record.picture_id == picture_buffer_id;
                   });
  if (iter == output_buffer_map_.end()) {
    // It's possible that we've already posted a DismissPictureBuffer for this
    // picture, but it has not yet executed when this ImportBufferForPicture was
    // posted to us by the client. In that case just ignore this (we've already
    // dismissed it and accounted for that).
    DVLOGF(3) << "got picture id=" << picture_buffer_id
              << " not in use (anymore?).";
    return;
  }

  if (iter->state != kAtClient) {
    VLOGF(1) << "Cannot import buffer not owned by client";
    NOTIFY_ERROR(INVALID_ARGUMENT);
    return;
  }

  size_t index = iter - output_buffer_map_.begin();
  DCHECK_EQ(std::count(free_output_buffers_.begin(), free_output_buffers_.end(),
                       index),
            0);

  iter->state = kFree;

  DCHECK_LE(output_planes_count_, dmabuf_fds.size());

  iter->output_fds = std::move(dmabuf_fds);
  iter->offsets = std::move(offsets);

  if (decoder_state_ == kAwaitingPictureBuffers)
      decoder_state_ = kDecoding;

  free_output_buffers_.push_back(index);
  if (decoder_state_ != kChangingResolution) {
      Enqueue();
      ScheduleDecodeBufferTaskIfNeeded();
  }
}

void V4L2VideoDecodeAccelerator::ReusePictureBuffer(int32_t picture_buffer_id) {
  DVLOGF(4) << "picture_buffer_id=" << picture_buffer_id;
  // Must be run on child thread, as we'll insert a sync in the EGL context.
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  decoder_thread_.task_runner()->PostTask(
      FROM_HERE, base::Bind(&V4L2VideoDecodeAccelerator::ReusePictureBufferTask,
                            base::Unretained(this), picture_buffer_id));
}

void V4L2VideoDecodeAccelerator::Flush() {
  VLOGF(2);
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  decoder_thread_.task_runner()->PostTask(
      FROM_HERE, base::Bind(&V4L2VideoDecodeAccelerator::FlushTask,
                            base::Unretained(this)));
}

void V4L2VideoDecodeAccelerator::Reset() {
  VLOGF(2);
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  decoder_thread_.task_runner()->PostTask(
      FROM_HERE, base::Bind(&V4L2VideoDecodeAccelerator::ResetTask,
                            base::Unretained(this)));
}

void V4L2VideoDecodeAccelerator::Destroy() {
  VLOGF(2);
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  // We're destroying; cancel all callbacks.
  client_ptr_factory_.reset();
  weak_this_factory_.InvalidateWeakPtrs();

  // If the decoder thread is running, destroy using posted task.
  if (decoder_thread_.IsRunning()) {
    decoder_thread_.task_runner()->PostTask(
        FROM_HERE, base::Bind(&V4L2VideoDecodeAccelerator::DestroyTask,
                              base::Unretained(this)));
    // DestroyTask() will cause the decoder_thread_ to flush all tasks.
    decoder_thread_.Stop();
  } else {
    // Otherwise, call the destroy task directly.
    DestroyTask();
  }

  delete this;
  VLOGF(2) << "Destroyed.";
}

bool V4L2VideoDecodeAccelerator::TryToSetupDecodeOnSeparateThread(
    const base::WeakPtr<Client>& decode_client,
    const scoped_refptr<base::SingleThreadTaskRunner>& decode_task_runner) {
  VLOGF(2);
  decode_client_ = decode_client;
  decode_task_runner_ = decode_task_runner;
  return true;
}

// static
VideoDecodeAccelerator::SupportedProfiles
V4L2VideoDecodeAccelerator::GetSupportedProfiles() {
  scoped_refptr<V4L2Device> device(new GenericV4L2Device());
  if (!device)
    return SupportedProfiles();

  return device->GetSupportedDecodeProfiles(arraysize(supported_input_fourccs_),
                                            supported_input_fourccs_);
}

void V4L2VideoDecodeAccelerator::DecodeTask(
    const BitstreamBuffer& bitstream_buffer) {
  DVLOGF(4) << "input_id=" << bitstream_buffer.id();
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_NE(decoder_state_, kUninitialized);

  // Invalid handle.
  if (!bitstream_buffer.handle().IsValid()) {
    NOTIFY_ERROR(INVALID_ARGUMENT);
    return;
  }

  int bitstream_id = bitstream_buffer.id();
  std::unique_ptr<BitstreamBufferRef> bitstream_record(new BitstreamBufferRef(
      decode_client_, decode_task_runner_,
      std::move(bitstream_buffer), bitstream_id));

  // Skip empty buffer.
  if (bitstream_record->size == 0)
    return;

  if (decoder_state_ == kResetting || decoder_flushing_) {
    // In the case that we're resetting or flushing, we need to delay decoding
    // the BitstreamBuffers that come after the Reset() or Flush() call.  When
    // we're here, we know that this DecodeTask() was scheduled by a Decode()
    // call that came after (in the client thread) the Reset() or Flush() call;
    // thus set up the delay if necessary.
    if (decoder_delay_bitstream_buffer_id_ == -1)
      decoder_delay_bitstream_buffer_id_ = bitstream_record->input_id;
  } else if (decoder_state_ == kError) {
    VLOGF(2) << "early out: kError state";
    return;
  }

  decoder_input_queue_.push(std::move(bitstream_record));
  decoder_decode_buffer_tasks_scheduled_++;
  DecodeBufferTask();
}

void V4L2VideoDecodeAccelerator::DecodeBufferTask() {
  DVLOGF(4);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_NE(decoder_state_, kUninitialized);

  decoder_decode_buffer_tasks_scheduled_--;

  if (decoder_state_ != kInitialized && decoder_state_ != kDecoding) {
    DVLOGF(3) << "early out: state=" << decoder_state_;
    return;
  }

  if (decoder_current_bitstream_buffer_ == NULL) {
    if (decoder_input_queue_.empty()) {
      // We're waiting for a new buffer -- exit without scheduling a new task.
      return;
    }
    const std::unique_ptr<BitstreamBufferRef>& buffer_ref = decoder_input_queue_.front();
    if (decoder_delay_bitstream_buffer_id_ == buffer_ref->input_id) {
      // We're asked to delay decoding on this and subsequent buffers.
      return;
    }

    // Setup to use the next buffer.
    decoder_current_bitstream_buffer_ = std::move(decoder_input_queue_.front());
    decoder_input_queue_.pop();
    const auto& dmabuf_fd = decoder_current_bitstream_buffer_->dmabuf_fd;
    if (dmabuf_fd.is_valid()) {
      DVLOGF(4) << "reading input_id="
                << decoder_current_bitstream_buffer_->input_id
                << ", fd=" << dmabuf_fd.get()
                << ", size=" << decoder_current_bitstream_buffer_->size;
    } else {
      DCHECK_EQ(decoder_current_bitstream_buffer_->input_id, kFlushBufferId);
      DVLOGF(4) << "reading input_id=kFlushBufferId";
    }
  }
  bool schedule_task = false;
  const auto& dmabuf_fd = decoder_current_bitstream_buffer_->dmabuf_fd;
  if (!dmabuf_fd.is_valid()) {
    // This is a dummy buffer, queued to flush the pipe.  Flush.
    DCHECK_EQ(decoder_current_bitstream_buffer_->input_id, kFlushBufferId);
    if (TrySubmitInputFrame()) {
      VLOGF(2) << "enqueued flush buffer";
      schedule_task = true;
    } else {
      // If we failed to enqueue the empty buffer (due to pipeline
      // backpressure), don't advance the bitstream buffer queue, and don't
      // schedule the next task.  This bitstream buffer queue entry will get
      // reprocessed when the pipeline frees up.
      schedule_task = false;
    }
  } else {
    DCHECK_GT(decoder_current_bitstream_buffer_->size, 0u);
    switch (decoder_state_) {
      case kInitialized:
        schedule_task = DecodeBufferInitial();
        break;
      case kDecoding:
        schedule_task = DecodeBufferContinue();
        break;
      default:
        NOTIFY_ERROR(ILLEGAL_STATE);
        return;
    }
  }
  if (decoder_state_ == kError) {
    // Failed during decode.
    return;
  }

  if (schedule_task) {
    ScheduleDecodeBufferTaskIfNeeded();
  }
}

void V4L2VideoDecodeAccelerator::ScheduleDecodeBufferTaskIfNeeded() {
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());

  // If we're behind on tasks, schedule another one.
  int buffers_to_decode = decoder_input_queue_.size();
  if (decoder_current_bitstream_buffer_ != NULL)
    buffers_to_decode++;
  if (decoder_decode_buffer_tasks_scheduled_ < buffers_to_decode) {
    decoder_decode_buffer_tasks_scheduled_++;
    decoder_thread_.task_runner()->PostTask(
        FROM_HERE, base::Bind(&V4L2VideoDecodeAccelerator::DecodeBufferTask,
                              base::Unretained(this)));
  }
}

bool V4L2VideoDecodeAccelerator::DecodeBufferInitial() {
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_EQ(decoder_state_, kInitialized);
  // Initial decode.  We haven't been able to get output stream format info yet.
  // Get it, and start decoding.

  if (!TrySubmitInputFrame())
    return false;

  // Recycle buffers.
  Dequeue();

  // If an initial resolution change event is not done yet, a driver probably
  // needs more stream to decode format.
  // Return true and schedule next buffer without changing status to kDecoding.
  // If the initial resolution change is done and coded size is known, we may
  // still have to wait for AssignPictureBuffers() and output buffers to be
  // allocated.
  if (coded_size_.IsEmpty() || output_buffer_map_.empty()) {
    // Need more stream to decode format, return true and schedule next buffer.
    return true;
  }

  decoder_state_ = kDecoding;
  ScheduleDecodeBufferTaskIfNeeded();
  return true;
}

bool V4L2VideoDecodeAccelerator::DecodeBufferContinue() {
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_EQ(decoder_state_, kDecoding);

  return TrySubmitInputFrame();
}

bool V4L2VideoDecodeAccelerator::TrySubmitInputFrame() {
  DVLOGF(4);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_NE(decoder_state_, kUninitialized);
  DCHECK_NE(decoder_state_, kResetting);
  DCHECK_NE(decoder_state_, kError);
  CHECK(decoder_current_bitstream_buffer_);

  // No free input buffer.
  if (free_input_buffers_.empty())
      return false;

  const int input_buffer_index = free_input_buffers_.back();
  free_input_buffers_.pop_back();
  InputRecord& input_record = input_buffer_map_[input_buffer_index];
  DCHECK(!input_record.bitstream_buffer);

  // Pass the required info to InputRecord.
  input_record.bitstream_buffer = std::move(decoder_current_bitstream_buffer_);
  // Queue it.
  input_ready_queue_.push(input_buffer_index);
  DVLOGF(4) << "submitting input_id=" << input_record.bitstream_buffer->input_id;
  // Enqueue once since there's new available input for it.
  Enqueue();

  return (decoder_state_ != kError);
}

void V4L2VideoDecodeAccelerator::ServiceDeviceTask(bool event_pending) {
  DVLOGF(4);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_NE(decoder_state_, kUninitialized);

  if (decoder_state_ == kResetting) {
    DVLOGF(3) << "early out: kResetting state";
    return;
  } else if (decoder_state_ == kError) {
    DVLOGF(3) << "early out: kError state";
    return;
  } else if (decoder_state_ == kChangingResolution) {
    DVLOGF(3) << "early out: kChangingResolution state";
    return;
  }

  bool resolution_change_pending = false;
  if (event_pending)
    resolution_change_pending = DequeueResolutionChangeEvent();

  if (!resolution_change_pending && coded_size_.IsEmpty()) {
    // Some platforms do not send an initial resolution change event.
    // To work around this, we need to keep checking if the initial resolution
    // is known already by explicitly querying the format after each decode,
    // regardless of whether we received an event.
    // This needs to be done on initial resolution change,
    // i.e. when coded_size_.IsEmpty().

    // Try GetFormatInfo to check if an initial resolution change can be done.
    struct v4l2_format format;
    Size visible_size;
    bool again;
    if (GetFormatInfo(&format, &visible_size, &again) && !again) {
      resolution_change_pending = true;
      DequeueResolutionChangeEvent();
    }
  }

  Dequeue();
  Enqueue();

  // Clear the interrupt fd.
  if (!device_->ClearDevicePollInterrupt()) {
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  bool poll_device = false;
  // Add fd, if we should poll on it.
  // Can be polled as soon as either input or output buffers are queued.
  if (input_buffer_queued_count_ + output_buffer_queued_count_ > 0)
    poll_device = true;

  // ServiceDeviceTask() should only ever be scheduled from DevicePollTask(),
  // so either:
  // * device_poll_thread_ is running normally
  // * device_poll_thread_ scheduled us, but then a ResetTask() or DestroyTask()
  //   shut it down, in which case we're either in kResetting or kError states
  //   respectively, and we should have early-outed already.
  DCHECK(device_poll_thread_.message_loop());
  // Queue the DevicePollTask() now.
  device_poll_thread_.task_runner()->PostTask(
      FROM_HERE, base::Bind(&V4L2VideoDecodeAccelerator::DevicePollTask,
                            base::Unretained(this), poll_device));

  DVLOGF(3) << "ServiceDeviceTask(): buffer counts: DEC["
            << decoder_input_queue_.size() << "->"
            << input_ready_queue_.size() << "] => DEVICE["
            << free_input_buffers_.size() << "+"
            << input_buffer_queued_count_ << "/"
            << input_buffer_map_.size() << "->"
            << free_output_buffers_.size() << "+"
            << output_buffer_queued_count_ << "/"
            << output_buffer_map_.size() << "] => CLIENT["
            << decoder_frames_at_client_ << "]";

  ScheduleDecodeBufferTaskIfNeeded();
  if (resolution_change_pending)
    StartResolutionChange();
}

void V4L2VideoDecodeAccelerator::Enqueue() {
  DVLOGF(4);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_NE(decoder_state_, kUninitialized);

  // Drain the pipe of completed decode buffers.
  const int old_inputs_queued = input_buffer_queued_count_;
  while (!input_ready_queue_.empty()) {
    const int buffer = input_ready_queue_.front();
    InputRecord& input_record = input_buffer_map_[buffer];
    if (input_record.bitstream_buffer->input_id == kFlushBufferId && decoder_cmd_supported_) {
      // Send the flush command after all input buffers are dequeued. This makes
      // sure all previous resolution changes have been handled because the
      // driver must hold the input buffer that triggers resolution change. The
      // driver cannot decode data in it without new output buffers. If we send
      // the flush now and a queued input buffer triggers resolution change
      // later, the driver will send an output buffer that has
      // V4L2_BUF_FLAG_LAST. But some queued input buffer have not been decoded
      // yet. Also, V4L2VDA calls STREAMOFF and STREAMON after resolution
      // change. They implicitly send a V4L2_DEC_CMD_STOP and V4L2_DEC_CMD_START
      // to the decoder.
      if (input_buffer_queued_count_ == 0) {
        if (!SendDecoderCmdStop())
          return;
        input_ready_queue_.pop();
        free_input_buffers_.push_back(buffer);
        input_record.bitstream_buffer.reset();
      } else {
        break;
      }
    } else if (!EnqueueInputRecord())
      return;
  }
  if (old_inputs_queued == 0 && input_buffer_queued_count_ != 0) {
    // We just started up a previously empty queue.
    // Queue state changed; signal interrupt.
    if (!device_->SetDevicePollInterrupt()) {
      VPLOGF(1) << "SetDevicePollInterrupt failed";
      NOTIFY_ERROR(PLATFORM_FAILURE);
      return;
    }
    // Start VIDIOC_STREAMON if we haven't yet.
    if (!input_streamon_) {
      __u32 type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      IOCTL_OR_ERROR_RETURN(VIDIOC_STREAMON, &type);
      input_streamon_ = true;
    }
  }

  // Enqueue all the outputs we can.
  const int old_outputs_queued = output_buffer_queued_count_;
  while (!free_output_buffers_.empty()) {
    if (!EnqueueOutputRecord())
      return;
  }
  if (old_outputs_queued == 0 && output_buffer_queued_count_ != 0) {
    // We just started up a previously empty queue.
    // Queue state changed; signal interrupt.
    if (!device_->SetDevicePollInterrupt()) {
      VPLOGF(1) << "SetDevicePollInterrupt(): failed";
      NOTIFY_ERROR(PLATFORM_FAILURE);
      return;
    }
    // Start VIDIOC_STREAMON if we haven't yet.
    if (!output_streamon_) {
      __u32 type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      IOCTL_OR_ERROR_RETURN(VIDIOC_STREAMON, &type);
      output_streamon_ = true;
    }
  }
}

bool V4L2VideoDecodeAccelerator::DequeueResolutionChangeEvent() {
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_NE(decoder_state_, kUninitialized);
  DVLOGF(3);

  struct v4l2_event ev;
  memset(&ev, 0, sizeof(ev));

  while (device_->Ioctl(VIDIOC_DQEVENT, &ev) == 0) {
    if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
      if (ev.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION) {
        VLOGF(2) << "got resolution change event.";
        return true;
      }
    } else {
      VLOGF(1) << "got an event (" << ev.type << ") we haven't subscribed to.";
    }
  }
  return false;
}

void V4L2VideoDecodeAccelerator::Dequeue() {
  DVLOGF(4);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_NE(decoder_state_, kUninitialized);

  while (input_buffer_queued_count_ > 0) {
    if (!DequeueInputBuffer())
      break;
  }
  while (output_buffer_queued_count_ > 0) {
    if (!DequeueOutputBuffer())
      break;
  }
  NotifyFlushDoneIfNeeded();
}

bool V4L2VideoDecodeAccelerator::DequeueInputBuffer() {
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_GT(input_buffer_queued_count_, 0);
  DCHECK(input_streamon_);

  // Dequeue a completed input (VIDEO_OUTPUT) buffer, and recycle to the free
  // list.
  struct v4l2_buffer dqbuf;
  struct v4l2_plane planes[1];
  memset(&dqbuf, 0, sizeof(dqbuf));
  memset(planes, 0, sizeof(planes));
  dqbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  dqbuf.memory = V4L2_MEMORY_DMABUF;
  dqbuf.m.planes = planes;
  dqbuf.length = 1;
  if (device_->Ioctl(VIDIOC_DQBUF, &dqbuf) != 0) {
    if (errno == EAGAIN) {
      // EAGAIN if we're just out of buffers to dequeue.
      return false;
    }
    VPLOGF(1) << "ioctl() failed: VIDIOC_DQBUF";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return false;
  }
  InputRecord& input_record = input_buffer_map_[dqbuf.index];
  DCHECK(input_record.at_device);
  free_input_buffers_.push_back(dqbuf.index);
  input_record.at_device = false;
  // This will trigger NotifyEndOfBitstreamBuffer().
  input_record.bitstream_buffer.reset();
  input_buffer_queued_count_--;

  return true;
}

bool V4L2VideoDecodeAccelerator::DequeueOutputBuffer() {
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_GT(output_buffer_queued_count_, 0);
  DCHECK(output_streamon_);

  // Dequeue a completed output (VIDEO_CAPTURE) buffer, and queue to the
  // completed queue.
  struct v4l2_buffer dqbuf {};
  struct v4l2_plane dqbuf_planes[VIDEO_MAX_PLANES] = {};
  dqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  dqbuf.memory = V4L2_MEMORY_DMABUF;
  dqbuf.m.planes = dqbuf_planes;
  dqbuf.length = output_planes_count_;
  if (device_->Ioctl(VIDIOC_DQBUF, &dqbuf) != 0) {
    if (errno == EAGAIN) {
      // EAGAIN if we're just out of buffers to dequeue.
      return false;
    } else if (errno == EPIPE) {
      DVLOGF(3) << "Got EPIPE. Last output buffer was already dequeued.";
      return false;
    }
    VPLOGF(1) << "ioctl() failed: VIDIOC_DQBUF";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return false;
  }
  OutputRecord& output_record = output_buffer_map_[dqbuf.index];
  DCHECK_EQ(output_record.state, kAtDevice);
  DCHECK_NE(output_record.picture_id, -1);
  output_buffer_queued_count_--;

  // Zero-bytes buffers are returned as part of a flush and can be dismissed.
  if (dqbuf.m.planes[0].bytesused > 0) {
    int32_t bitstream_buffer_id = dqbuf.timestamp.tv_sec;
    DCHECK_GE(bitstream_buffer_id, 0);
    DVLOGF(4) << "Dequeue output buffer: dqbuf index=" << dqbuf.index
              << " bitstream input_id=" << bitstream_buffer_id;
    output_record.state = kAtClient;
    decoder_frames_at_client_++;

    const Picture picture(output_record.picture_id, bitstream_buffer_id,
                          Rect(visible_size_), false);
    pending_picture_ready_.push(PictureRecord(output_record.cleared, picture));
    SendPictureReady();
    output_record.cleared = true;
  }

  if (dqbuf.flags & V4L2_BUF_FLAG_LAST) {
    DVLOGF(3) << "Got last output buffer. Waiting last buffer="
              << flush_awaiting_last_output_buffer_;
    if (flush_awaiting_last_output_buffer_) {
      flush_awaiting_last_output_buffer_ = false;
      struct v4l2_decoder_cmd cmd;
      memset(&cmd, 0, sizeof(cmd));
      cmd.cmd = V4L2_DEC_CMD_START;
      IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_DECODER_CMD, &cmd);
    }
  }
  return true;
}

bool V4L2VideoDecodeAccelerator::EnqueueInputRecord() {
  DVLOGF(4);
  DCHECK(!input_ready_queue_.empty());

  // Enqueue an input (VIDEO_OUTPUT) buffer.
  const int v4l2_buffer_index = input_ready_queue_.front();
  InputRecord& input_record = input_buffer_map_[v4l2_buffer_index];
  DCHECK(!input_record.at_device);
  struct v4l2_buffer qbuf {};
  struct v4l2_plane qbuf_plane = {};
  qbuf.index = v4l2_buffer_index;
  qbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  qbuf.timestamp.tv_sec = input_record.bitstream_buffer->input_id;
  qbuf.memory = V4L2_MEMORY_DMABUF;
  qbuf.m.planes = &qbuf_plane;
  const std::unique_ptr<BitstreamBufferRef>& buffer = input_record.bitstream_buffer;
  if (!buffer->dmabuf_fd.is_valid()) {
    // This is a flush case. A driver must handle Flush with V4L2_DEC_CMD_STOP.
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return false;
  }
  if (buffer->offset + buffer->size > input_buffer_size_) {
      VLOGF(1) << "offset + size of input buffer is larger than buffer size"
               << ", offset=" << buffer->offset
               << ", size=" << buffer->size
               << ", buffer size=" << input_buffer_size_;
      NOTIFY_ERROR(PLATFORM_FAILURE);
      return false;
  }

  // TODO(crbug.com/901264): The way to pass an offset within a DMA-buf is
  // not defined in V4L2 specification, so we abuse data_offset for now.
  // Fix it when we have the right interface, including any necessary
  // validation and potential alignment.
  qbuf.m.planes[0].m.fd = buffer->dmabuf_fd.get();
  qbuf.m.planes[0].data_offset = buffer->offset;
  qbuf.m.planes[0].bytesused = buffer->offset + buffer->size;
  // Workaround: filling length should not be needed. This is a bug of
  // videobuf2 library.
  qbuf.m.planes[0].length = input_buffer_size_;
  qbuf.length = 1;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_QBUF, &qbuf);
  DVLOGF(4) << "enqueued input_id=" << buffer->input_id;
  input_ready_queue_.pop();

  input_record.at_device = true;
  input_buffer_queued_count_++;

  return true;
}

bool V4L2VideoDecodeAccelerator::EnqueueOutputRecord() {
  DCHECK(!free_output_buffers_.empty());

  // Enqueue an output (VIDEO_CAPTURE) buffer.
  const int buffer = free_output_buffers_.front();
  DVLOGF(4) << "buffer " << buffer;
  OutputRecord& output_record = output_buffer_map_[buffer];
  DCHECK_EQ(output_record.state, kFree);
  DCHECK_NE(output_record.picture_id, -1);
  struct v4l2_buffer qbuf {};
  struct v4l2_plane qbuf_planes[VIDEO_MAX_PLANES] = {};
  qbuf.index = buffer;
  qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  qbuf.memory = V4L2_MEMORY_DMABUF;
  qbuf.m.planes = qbuf_planes;
  qbuf.length = output_planes_count_;
  DVLOGF(4) << "qbuf.index=" << qbuf.index;
  DCHECK_LE(output_planes_count_, output_record.output_fds.size());
  DCHECK_LE(output_planes_count_, output_record.offsets.size());
  // Pass fd and offset info.
  for (size_t i = 0; i < output_planes_count_; i++) {
    // output_record.output_fds is repeatedly used. We will not close the fd of
    // output buffer unless new fds are assigned in ImportBufferForPicture().
    qbuf.m.planes[i].m.fd = output_record.output_fds[i].get();
    qbuf.m.planes[i].data_offset = output_record.offsets[i];
  }
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_QBUF, &qbuf);
  free_output_buffers_.pop_front();
  output_record.state = kAtDevice;
  output_buffer_queued_count_++;
  return true;
}

void V4L2VideoDecodeAccelerator::ReusePictureBufferTask(int32_t picture_buffer_id) {
  DVLOGF(4) << "picture_buffer_id=" << picture_buffer_id;
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());

  // We run ReusePictureBufferTask even if we're in kResetting.
  if (decoder_state_ == kError) {
    DVLOGF(4) << "early out: kError state";
    return;
  }

  if (decoder_state_ == kChangingResolution) {
    DVLOGF(4) << "early out: kChangingResolution";
    return;
  }

  size_t index;
  for (index = 0; index < output_buffer_map_.size(); ++index)
    if (output_buffer_map_[index].picture_id == picture_buffer_id)
      break;

  if (index >= output_buffer_map_.size()) {
    // It's possible that we've already posted a DismissPictureBuffer for this
    // picture, but it has not yet executed when this ReusePictureBuffer was
    // posted to us by the client. In that case just ignore this (we've already
    // dismissed it and accounted for that) and let the sync object get
    // destroyed.
    DVLOGF(3) << "got picture id= " << picture_buffer_id
              << " not in use (anymore?).";
    return;
  }

  OutputRecord& output_record = output_buffer_map_[index];
  if (output_record.state != kAtClient) {
    VLOGF(1) << "picture_buffer_id not reusable";
    NOTIFY_ERROR(INVALID_ARGUMENT);
    return;
  }

  output_record.state = kFree;
  free_output_buffers_.push_back(index);
  decoder_frames_at_client_--;
  // We got a buffer back, so enqueue it back.
  Enqueue();
}

void V4L2VideoDecodeAccelerator::FlushTask() {
  VLOGF(2);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());

  if (decoder_state_ == kError) {
    VLOGF(2) << "early out: kError state";
    return;
  }

  // We don't support stacked flushing.
  DCHECK(!decoder_flushing_);

  // Queue up an empty buffer -- this triggers the flush.
  // BitstreamBufferRef::dmabuf_fd becomes invalid.
  decoder_input_queue_.push(std::make_unique<BitstreamBufferRef>(
      decode_client_, decode_task_runner_, BitstreamBuffer(), kFlushBufferId));
  decoder_flushing_ = true;
  SendPictureReady();  // Send all pending PictureReady.

  ScheduleDecodeBufferTaskIfNeeded();
}

void V4L2VideoDecodeAccelerator::NotifyFlushDoneIfNeeded() {
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  if (!decoder_flushing_)
    return;

  // Pipeline is empty when:
  // * Decoder input queue is empty of non-delayed buffers.
  // * There is no currently filling input buffer.
  // * Input holding queue is empty.
  // * All input (VIDEO_OUTPUT) buffers are returned.
  // * All image processor buffers are returned.
  if (!decoder_input_queue_.empty()) {
    if (decoder_input_queue_.front()->input_id !=
        decoder_delay_bitstream_buffer_id_) {
      DVLOGF(3) << "Some input bitstream buffers are not queued.";
      return;
    }
  }

  if ((input_ready_queue_.size() + input_buffer_queued_count_) != 0) {
    DVLOGF(3) << "Some input buffers are not dequeued.";
    return;
  }
  if (flush_awaiting_last_output_buffer_) {
    DVLOGF(3) << "Waiting for last output buffer.";
    return;
  }

  // TODO(posciak): https://crbug.com/270039. Exynos requires a
  // streamoff-streamon sequence after flush to continue, even if we are not
  // resetting. This would make sense, because we don't really want to resume
  // from a non-resume point (e.g. not from an IDR) if we are flushed.
  // MSE player however triggers a Flush() on chunk end, but never Reset(). One
  // could argue either way, or even say that Flush() is not needed/harmful when
  // transitioning to next chunk.
  // For now, do the streamoff-streamon cycle to satisfy Exynos and not freeze
  // when doing MSE. This should be harmless otherwise.
  if (!(StopDevicePoll() && StopOutputStream() && StopInputStream()))
    return;

  if (!StartDevicePoll())
    return;

  decoder_delay_bitstream_buffer_id_ = -1;
  decoder_flushing_ = false;
  VLOGF(2) << "returning flush";
  child_task_runner_->PostTask(FROM_HERE,
                               base::Bind(&Client::NotifyFlushDone, client_));

  // While we were flushing, we early-outed DecodeBufferTask()s.
  ScheduleDecodeBufferTaskIfNeeded();
}

bool V4L2VideoDecodeAccelerator::IsDecoderCmdSupported() {
  // CMD_STOP should always succeed. If the decoder is started, the command can
  // flush it. If the decoder is stopped, the command does nothing. We use this
  // to know if a driver supports V4L2_DEC_CMD_STOP to flush.
  struct v4l2_decoder_cmd cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.cmd = V4L2_DEC_CMD_STOP;
  if (device_->Ioctl(VIDIOC_TRY_DECODER_CMD, &cmd) != 0) {
    VLOGF(2) << "V4L2_DEC_CMD_STOP is not supported.";
    return false;
  }

  return true;
}

bool V4L2VideoDecodeAccelerator::SendDecoderCmdStop() {
  VLOGF(2);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK(!flush_awaiting_last_output_buffer_);

  struct v4l2_decoder_cmd cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.cmd = V4L2_DEC_CMD_STOP;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_DECODER_CMD, &cmd);
  flush_awaiting_last_output_buffer_ = true;

  return true;
}

void V4L2VideoDecodeAccelerator::ResetTask() {
  VLOGF(2);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());

  if (decoder_state_ == kError) {
    VLOGF(2) << "early out: kError state";
    return;
  }
  decoder_current_bitstream_buffer_.reset();
  while (!decoder_input_queue_.empty())
    decoder_input_queue_.pop();

  // If we are in the middle of switching resolutions or awaiting picture
  // buffers, postpone reset until it's done. We don't have to worry about
  // timing of this wrt to decoding, because output pipe is already
  // stopped if we are changing resolution. We will come back here after
  // we are done.
  DCHECK(!reset_pending_);
  if (decoder_state_ == kChangingResolution ||
      decoder_state_ == kAwaitingPictureBuffers) {
    reset_pending_ = true;
    return;
  }
  FinishReset();
}

void V4L2VideoDecodeAccelerator::FinishReset() {
  VLOGF(2);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());

  reset_pending_ = false;
  // After the output stream is stopped, the codec should not post any
  // resolution change events. So we dequeue the resolution change event
  // afterwards. The event could be posted before or while stopping the output
  // stream. The codec will expect the buffer of new size after the seek, so
  // we need to handle the resolution change event first.
  if (!(StopDevicePoll() && StopOutputStream()))
    return;

  if (DequeueResolutionChangeEvent()) {
    reset_pending_ = true;
    StartResolutionChange();
    return;
  }

  if (!StopInputStream())
    return;

  // If we were flushing, we'll never return any more BitstreamBuffers or
  // PictureBuffers; they have all been dropped and returned by now.
  NotifyFlushDoneIfNeeded();

  // Mark that we're resetting, then enqueue a ResetDoneTask().  All intervening
  // jobs will early-out in the kResetting state.
  decoder_state_ = kResetting;
  SendPictureReady();  // Send all pending PictureReady.
  decoder_thread_.task_runner()->PostTask(
      FROM_HERE, base::Bind(&V4L2VideoDecodeAccelerator::ResetDoneTask,
                            base::Unretained(this)));
}

void V4L2VideoDecodeAccelerator::ResetDoneTask() {
  VLOGF(2);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());

  if (decoder_state_ == kError) {
    VLOGF(2) << "early out: kError state";
    return;
  }

  // Start poll thread if NotifyFlushDoneIfNeeded has not already.
  if (!device_poll_thread_.IsRunning()) {
    if (!StartDevicePoll())
      return;
  }

  // Jobs drained, we're finished resetting.
  DCHECK_EQ(decoder_state_, kResetting);
  decoder_state_ = kInitialized;

  decoder_delay_bitstream_buffer_id_ = -1;
  child_task_runner_->PostTask(FROM_HERE,
                               base::Bind(&Client::NotifyResetDone, client_));

  // While we were resetting, we early-outed DecodeBufferTask()s.
  ScheduleDecodeBufferTaskIfNeeded();
}

void V4L2VideoDecodeAccelerator::DestroyTask() {
  VLOGF(2);

  // DestroyTask() should run regardless of decoder_state_.

  StopDevicePoll();
  StopOutputStream();
  StopInputStream();

  decoder_current_bitstream_buffer_.reset();
  decoder_decode_buffer_tasks_scheduled_ = 0;
  decoder_frames_at_client_ = 0;
  while (!decoder_input_queue_.empty())
    decoder_input_queue_.pop();
  decoder_flushing_ = false;

  // Set our state to kError.  Just in case.
  decoder_state_ = kError;

  DestroyInputBuffers();
  DestroyOutputBuffers();
}

bool V4L2VideoDecodeAccelerator::StartDevicePoll() {
  DVLOGF(3);
  DCHECK(!device_poll_thread_.IsRunning());
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());

  // Start up the device poll thread and schedule its first DevicePollTask().
  if (!device_poll_thread_.Start()) {
    VLOGF(1) << "Device thread failed to start";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return false;
  }
  device_poll_thread_.task_runner()->PostTask(
      FROM_HERE, base::Bind(&V4L2VideoDecodeAccelerator::DevicePollTask,
                            base::Unretained(this), 0));

  return true;
}

bool V4L2VideoDecodeAccelerator::StopDevicePoll() {
  DVLOGF(3);

  if (!device_poll_thread_.IsRunning())
    return true;

  if (decoder_thread_.IsRunning())
    DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());

  // Signal the DevicePollTask() to stop, and stop the device poll thread.
  if (!device_->SetDevicePollInterrupt()) {
    VPLOGF(1) << "SetDevicePollInterrupt(): failed";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return false;
  }
  device_poll_thread_.Stop();
  // Clear the interrupt now, to be sure.
  if (!device_->ClearDevicePollInterrupt()) {
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return false;
  }
  DVLOGF(3) << "device poll stopped";
  return true;
}

bool V4L2VideoDecodeAccelerator::StopOutputStream() {
  VLOGF(2);
  if (!output_streamon_)
    return true;

  __u32 type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_STREAMOFF, &type);
  output_streamon_ = false;

  // Output stream is stopped. No need to wait for the buffer anymore.
  flush_awaiting_last_output_buffer_ = false;

  for (size_t i = 0; i < output_buffer_map_.size(); ++i) {
    // After streamoff, the device drops ownership of all buffers, even if we
    // don't dequeue them explicitly. Some of them may still be owned by the
    // client however. Reuse only those that aren't.
    OutputRecord& output_record = output_buffer_map_[i];
    if (output_record.state == kAtDevice) {
      output_record.state = kFree;
      free_output_buffers_.push_back(i);
    }
  }
  output_buffer_queued_count_ = 0;
  return true;
}

bool V4L2VideoDecodeAccelerator::StopInputStream() {
  VLOGF(2);
  if (!input_streamon_)
    return true;

  __u32 type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_STREAMOFF, &type);
  input_streamon_ = false;

  // Reset accounting info for input.
  while (!input_ready_queue_.empty())
    input_ready_queue_.pop();
  free_input_buffers_.clear();
  for (size_t i = 0; i < input_buffer_map_.size(); ++i) {
    free_input_buffers_.push_back(i);
    input_buffer_map_[i].at_device = false;
    input_buffer_map_[i].bitstream_buffer.reset();
  }
  input_buffer_queued_count_ = 0;

  return true;
}

void V4L2VideoDecodeAccelerator::StartResolutionChange() {
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_NE(decoder_state_, kUninitialized);
  DCHECK_NE(decoder_state_, kResetting);

  VLOGF(2) << "Initiate resolution change";

  if (!(StopDevicePoll() && StopOutputStream()))
    return;

  decoder_state_ = kChangingResolution;
  SendPictureReady();  // Send all pending PictureReady.

  if (!DestroyOutputBuffers()) {
    VLOGF(1) << "Failed destroying output buffers.";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  FinishResolutionChange();
}

void V4L2VideoDecodeAccelerator::FinishResolutionChange() {
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_EQ(decoder_state_, kChangingResolution);
  VLOGF(2);

  if (decoder_state_ == kError) {
    VLOGF(2) << "early out: kError state";
    return;
  }

  struct v4l2_format format;
  bool again;
  Size visible_size;
  bool ret = GetFormatInfo(&format, &visible_size, &again);
  if (!ret || again) {
    VLOGF(1) << "Couldn't get format information after resolution change";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  if (!CreateBuffersForFormat(format, visible_size)) {
    VLOGF(1) << "Couldn't reallocate buffers after resolution change";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  if (!StartDevicePoll())
    return;
}

void V4L2VideoDecodeAccelerator::DevicePollTask(bool poll_device) {
  DVLOGF(4);
  DCHECK(device_poll_thread_.task_runner()->BelongsToCurrentThread());

  bool event_pending = false;

  if (!device_->Poll(poll_device, &event_pending)) {
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  // All processing should happen on ServiceDeviceTask(), since we shouldn't
  // touch decoder state from this thread.
  decoder_thread_.task_runner()->PostTask(
      FROM_HERE, base::Bind(&V4L2VideoDecodeAccelerator::ServiceDeviceTask,
                            base::Unretained(this), event_pending));
}

void V4L2VideoDecodeAccelerator::NotifyError(Error error) {
  VLOGF(1);

  if (!child_task_runner_->BelongsToCurrentThread()) {
    child_task_runner_->PostTask(
        FROM_HERE, base::Bind(&V4L2VideoDecodeAccelerator::NotifyError,
                              weak_this_, error));
    return;
  }

  if (client_) {
    client_->NotifyError(error);
    client_ptr_factory_.reset();
  }
}

void V4L2VideoDecodeAccelerator::SetErrorState(Error error) {
  // We can touch decoder_state_ only if this is the decoder thread or the
  // decoder thread isn't running.
  if (decoder_thread_.task_runner() &&
      !decoder_thread_.task_runner()->BelongsToCurrentThread()) {
    decoder_thread_.task_runner()->PostTask(
        FROM_HERE, base::Bind(&V4L2VideoDecodeAccelerator::SetErrorState,
                              base::Unretained(this), error));
    return;
  }

  // Post NotifyError only if we are already initialized, as the API does
  // not allow doing so before that.
  if (decoder_state_ != kError && decoder_state_ != kUninitialized)
    NotifyError(error);

  decoder_state_ = kError;
}

bool V4L2VideoDecodeAccelerator::GetFormatInfo(struct v4l2_format* format,
                                               Size* visible_size,
                                               bool* again) {
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());

  *again = false;
  memset(format, 0, sizeof(*format));
  format->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (device_->Ioctl(VIDIOC_G_FMT, format) != 0) {
    if (errno == EINVAL) {
      // EINVAL means we haven't seen sufficient stream to decode the format.
      *again = true;
      return true;
    } else {
      VPLOGF(1) << "ioctl() failed: VIDIOC_G_FMT";
      NOTIFY_ERROR(PLATFORM_FAILURE);
      return false;
    }
  }

  // Make sure we are still getting the format we set on initialization.
  if (format->fmt.pix_mp.pixelformat != output_format_fourcc_) {
    VLOGF(1) << "Unexpected format from G_FMT on output";
    return false;
  }

  Size coded_size(format->fmt.pix_mp.width, format->fmt.pix_mp.height);
  if (visible_size != nullptr)
    *visible_size = GetVisibleSize(coded_size);

  return true;
}

bool V4L2VideoDecodeAccelerator::CreateBuffersForFormat(
    const struct v4l2_format& format,
    const Size& visible_size) {
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  output_planes_count_ = format.fmt.pix_mp.num_planes;
  coded_size_.SetSize(format.fmt.pix_mp.width, format.fmt.pix_mp.height);
  visible_size_ = visible_size;

  VLOGF(2) << "new resolution: " << coded_size_.ToString()
           << ", visible size: " << visible_size_.ToString()
           << ", decoder output planes count: " << output_planes_count_;

  return CreateOutputBuffers();
}

Size V4L2VideoDecodeAccelerator::GetVisibleSize(
    const Size& coded_size) {
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());

  struct v4l2_rect* visible_rect;
  struct v4l2_selection selection_arg;
  memset(&selection_arg, 0, sizeof(selection_arg));
  selection_arg.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  selection_arg.target = V4L2_SEL_TGT_COMPOSE;

  if (device_->Ioctl(VIDIOC_G_SELECTION, &selection_arg) == 0) {
    VLOGF(2) << "VIDIOC_G_SELECTION is supported";
    visible_rect = &selection_arg.r;
  } else {
    VLOGF(2) << "Fallback to VIDIOC_G_CROP";
    struct v4l2_crop crop_arg;
    memset(&crop_arg, 0, sizeof(crop_arg));
    crop_arg.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (device_->Ioctl(VIDIOC_G_CROP, &crop_arg) != 0) {
      VPLOGF(1) << "ioctl() VIDIOC_G_CROP failed";
      return coded_size;
    }
    visible_rect = &crop_arg.c;
  }

  Rect rect(visible_rect->left, visible_rect->top, visible_rect->width,
            visible_rect->height);
  VLOGF(2) << "visible rectangle is " << rect.ToString();
  if (!Rect(coded_size).Contains(rect)) {
    DVLOGF(3) << "visible rectangle " << rect.ToString()
              << " is not inside coded size " << coded_size.ToString();
    return coded_size;
  }
  if (rect.IsEmpty()) {
    VLOGF(1) << "visible size is empty";
    return coded_size;
  }

  // Chrome assume picture frame is coded at (0, 0).
  if (rect.x() != 0 || rect.y() != 0) {
    VLOGF(1) << "Unexpected visible rectangle " << rect.ToString()
             << ", top-left is not origin";
    return coded_size;
  }

  return rect.size();
}

bool V4L2VideoDecodeAccelerator::CreateInputBuffers() {
  VLOGF(2);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  // We always run this as we prepare to initialize.
  DCHECK_EQ(decoder_state_, kInitialized);
  DCHECK(!input_streamon_);
  DCHECK(input_buffer_map_.empty());

  struct v4l2_requestbuffers reqbufs;
  memset(&reqbufs, 0, sizeof(reqbufs));
  reqbufs.count = kInputBufferCount;
  reqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  reqbufs.memory = V4L2_MEMORY_DMABUF;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_REQBUFS, &reqbufs);
  if (reqbufs.count < kInputBufferCount) {
    VLOGF(1) << "Could not allocate enough output buffers";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return false;
  }
  input_buffer_map_.resize(reqbufs.count);
  free_input_buffers_.resize(reqbufs.count);
  std::iota(free_input_buffers_.begin(), free_input_buffers_.end(), 0);
  return true;
}

static bool IsSupportedOutputFormat(uint32_t v4l2_format) {
  // Only support V4L2_PIX_FMT_NV12 output format for now.
  // TODO(johnylin): add more supported format if necessary.
  uint32_t kSupportedOutputFmtFourcc[] = { V4L2_PIX_FMT_NV12 };
  return std::find(
      kSupportedOutputFmtFourcc,
      kSupportedOutputFmtFourcc + arraysize(kSupportedOutputFmtFourcc),
      v4l2_format) !=
          kSupportedOutputFmtFourcc + arraysize(kSupportedOutputFmtFourcc);
}

bool V4L2VideoDecodeAccelerator::SetupFormats() {
  // We always run this as we prepare to initialize.
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(decoder_state_, kUninitialized);
  DCHECK(!input_streamon_);
  DCHECK(!output_streamon_);

  size_t input_size;
  Size max_resolution, min_resolution;
  device_->GetSupportedResolution(input_format_fourcc_, &min_resolution,
                                  &max_resolution);
  if (max_resolution.width() > 1920 && max_resolution.height() > 1088)
    input_size = kInputBufferMaxSizeFor4k;
  else
    input_size = kInputBufferMaxSizeFor1080p;

  struct v4l2_fmtdesc fmtdesc;
  memset(&fmtdesc, 0, sizeof(fmtdesc));
  fmtdesc.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  bool is_format_supported = false;
  while (device_->Ioctl(VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    if (fmtdesc.pixelformat == input_format_fourcc_) {
      is_format_supported = true;
      break;
    }
    ++fmtdesc.index;
  }

  if (!is_format_supported) {
    VLOGF(1) << "Input fourcc " << input_format_fourcc_
             << " not supported by device.";
    return false;
  }

  struct v4l2_format format;
  memset(&format, 0, sizeof(format));
  format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  format.fmt.pix_mp.pixelformat = input_format_fourcc_;
  format.fmt.pix_mp.plane_fmt[0].sizeimage = input_size;
  format.fmt.pix_mp.num_planes = 1;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_S_FMT, &format);
  // V4L2 driver adjusts input size that the driver may access. Store the size
  // in order to specify it in QBUF later.
  input_buffer_size_ = format.fmt.pix_mp.plane_fmt[0].sizeimage;


  // We have to set up the format for output, because the driver may not allow
  // changing it once we start streaming; whether it can support our chosen
  // output format or not may depend on the input format.
  memset(&fmtdesc, 0, sizeof(fmtdesc));
  fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  while (device_->Ioctl(VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    if (IsSupportedOutputFormat(fmtdesc.pixelformat)) {
      output_format_fourcc_ = fmtdesc.pixelformat;
      break;
    }
    ++fmtdesc.index;
  }

  if (output_format_fourcc_ == 0) {
    VLOGF(2) << "Image processor not available";
    return false;
  }
  VLOGF(2) << "Output format=" << output_format_fourcc_;

  // Just set the fourcc for output; resolution, etc., will come from the
  // driver once it extracts it from the stream.
  memset(&format, 0, sizeof(format));
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  format.fmt.pix_mp.pixelformat = output_format_fourcc_;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_S_FMT, &format);

  return true;
}

bool V4L2VideoDecodeAccelerator::CreateOutputBuffers() {
  VLOGF(2);
  DCHECK(decoder_state_ == kInitialized ||
         decoder_state_ == kChangingResolution);
  DCHECK(!output_streamon_);
  DCHECK(output_buffer_map_.empty());
  DCHECK_EQ(output_mode_, Config::OutputMode::IMPORT);

  // Number of output buffers we need.
  struct v4l2_control ctrl;
  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_G_CTRL, &ctrl);
  output_dpb_size_ = ctrl.value;

  // Output format setup in Initialize().

  uint32_t buffer_count = output_dpb_size_ + kDpbOutputBufferExtraCount;

  VideoPixelFormat pixel_format =
      V4L2PixFmtToVideoPixelFormat(output_format_fourcc_);

  child_task_runner_->PostTask(
      FROM_HERE, base::Bind(&Client::ProvidePictureBuffers, client_,
                            buffer_count, pixel_format, coded_size_));


  // Go into kAwaitingPictureBuffers to prevent us from doing any more decoding
  // or event handling while we are waiting for AssignPictureBuffers(). Not
  // having Pictures available would not have prevented us from making decoding
  // progress entirely e.g. in the case of H.264 where we could further decode
  // non-slice NALUs and could even get another resolution change before we were
  // done with this one. After we get the buffers, we'll go back into kIdle and
  // kick off further event processing, and eventually go back into kDecoding
  // once no more events are pending (if any).
  decoder_state_ = kAwaitingPictureBuffers;

  return true;
}

void V4L2VideoDecodeAccelerator::DestroyInputBuffers() {
  VLOGF(2);
  DCHECK(!decoder_thread_.IsRunning() ||
         decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK(!input_streamon_);

  if (input_buffer_map_.empty())
    return;

  struct v4l2_requestbuffers reqbufs;
  memset(&reqbufs, 0, sizeof(reqbufs));
  reqbufs.count = 0;
  reqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  reqbufs.memory = V4L2_MEMORY_DMABUF;
  IOCTL_OR_LOG_ERROR(VIDIOC_REQBUFS, &reqbufs);

  input_buffer_map_.clear();
  free_input_buffers_.clear();
}

bool V4L2VideoDecodeAccelerator::DestroyOutputBuffers() {
  VLOGF(2);
  DCHECK(!decoder_thread_.IsRunning() ||
         decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK(!output_streamon_);
  bool success = true;

  if (output_buffer_map_.empty())
    return true;

  for (size_t i = 0; i < output_buffer_map_.size(); ++i) {
    OutputRecord& output_record = output_buffer_map_[i];

    DVLOGF(3) << "dismissing PictureBuffer id=" << output_record.picture_id;
    child_task_runner_->PostTask(
        FROM_HERE, base::Bind(&Client::DismissPictureBuffer, client_,
                              output_record.picture_id));
  }

  struct v4l2_requestbuffers reqbufs;
  memset(&reqbufs, 0, sizeof(reqbufs));
  reqbufs.count = 0;
  reqbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  reqbufs.memory = V4L2_MEMORY_DMABUF;
  if (device_->Ioctl(VIDIOC_REQBUFS, &reqbufs) != 0) {
    VPLOGF(1) << "ioctl() failed: VIDIOC_REQBUFS";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    success = false;
  }

  output_buffer_map_.clear();
  while (!free_output_buffers_.empty())
    free_output_buffers_.pop_front();
  output_buffer_queued_count_ = 0;
  // The client may still hold some buffers. The texture holds a reference to
  // the buffer. It is OK to free the buffer and destroy EGLImage here.
  decoder_frames_at_client_ = 0;

  return success;
}

void V4L2VideoDecodeAccelerator::SendPictureReady() {
  DVLOGF(4);
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  bool send_now = (decoder_state_ == kChangingResolution ||
                   decoder_state_ == kResetting || decoder_flushing_);
  while (pending_picture_ready_.size() > 0) {
    bool cleared = pending_picture_ready_.front().cleared;
    const Picture& picture = pending_picture_ready_.front().picture;
    if (cleared && picture_clearing_count_ == 0) {
      // This picture is cleared. It can be posted to a thread different than
      // the main GPU thread to reduce latency. This should be the case after
      // all pictures are cleared at the beginning.
      decode_task_runner_->PostTask(
          FROM_HERE,
          base::Bind(&Client::PictureReady, decode_client_, picture));
      pending_picture_ready_.pop();
    } else if (!cleared || send_now) {
      DVLOGF(4) << "cleared=" << pending_picture_ready_.front().cleared
                << ", decoder_state_=" << decoder_state_
                << ", decoder_flushing_=" << decoder_flushing_
                << ", picture_clearing_count_=" << picture_clearing_count_;
      // If the picture is not cleared, post it to the child thread because it
      // has to be cleared in the child thread. A picture only needs to be
      // cleared once. If the decoder is changing resolution, resetting or
      // flushing, send all pictures to ensure PictureReady arrive before
      // ProvidePictureBuffers, NotifyResetDone, or NotifyFlushDone.
      child_task_runner_->PostTaskAndReply(
          FROM_HERE, base::Bind(&Client::PictureReady, client_, picture),
          // Unretained is safe. If Client::PictureReady gets to run, |this| is
          // alive. Destroy() will wait the decode thread to finish.
          base::Bind(&V4L2VideoDecodeAccelerator::PictureCleared,
                     base::Unretained(this)));
      picture_clearing_count_++;
      pending_picture_ready_.pop();
    } else {
      // This picture is cleared. But some pictures are about to be cleared on
      // the child thread. To preserve the order, do not send this until those
      // pictures are cleared.
      break;
    }
  }
}

void V4L2VideoDecodeAccelerator::PictureCleared() {
  DVLOGF(4) << "clearing count=" << picture_clearing_count_;
  DCHECK(decoder_thread_.task_runner()->BelongsToCurrentThread());
  DCHECK_GT(picture_clearing_count_, 0);
  picture_clearing_count_--;
  SendPictureReady();
}

}  // namespace media
