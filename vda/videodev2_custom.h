// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Note:
// This header file has extracted some new structs and definitions in videodev2.h from ChromeOS
// which is not upstreamed in Linux mainline. This should be removed once they are upstreamed.
// TODO(johnylin): remove this file once it is upstreamed.

#ifndef VIDEODEV2_CUSTOM_H_
#define VIDEODEV2_CUSTOM_H_

#include "v4l2_controls_custom.h"

#include <linux/videodev2.h>

/* compressed formats */
#define V4L2_PIX_FMT_H264_SLICE v4l2_fourcc('S', '2', '6', '4') /* H264 parsed slices */
#define V4L2_PIX_FMT_VP8_FRAME v4l2_fourcc('V', 'P', '8', 'F') /* VP8 parsed frames */
#define V4L2_PIX_FMT_VP9_FRAME v4l2_fourcc('V', 'P', '9', 'F') /* VP9 parsed frames */

struct v4l2_ext_control_custom {
  __u32 id;
  __u32 size;
  __u32 reserved2[1];
  union {
    __s32 value;
    __s64 value64;
    char __user *string;
    __u8 __user *p_u8;
    __u16 __user *p_u16;
    __u32 __user *p_u32;
    struct v4l2_ctrl_h264_sps __user *p_h264_sps;
    struct v4l2_ctrl_h264_pps __user *p_h264_pps;
    struct v4l2_ctrl_h264_scaling_matrix __user *p_h264_scal_mtrx;
    struct v4l2_ctrl_h264_slice_param __user *p_h264_slice_param;
    struct v4l2_ctrl_h264_decode_param __user *p_h264_decode_param;
    struct v4l2_ctrl_vp8_frame_hdr __user *p_vp8_frame_hdr;
    struct v4l2_ctrl_vp9_frame_hdr __user *p_vp9_frame_hdr;
    struct v4l2_ctrl_vp9_decode_param __user *p_vp9_decode_param;
    struct v4l2_ctrl_vp9_entropy __user *p_vp9_entropy;
    void __user *ptr;
  };
} __attribute__ ((packed));

struct v4l2_ext_controls_custom {
  union {
    __u32 ctrl_class;
    __u32 config_store;
  };
  __u32 count;
  __u32 error_idx;
  __u32 reserved[2];
  struct v4l2_ext_control_custom *controls;
};

/**
 * struct v4l2_buffer - video buffer info
 * @index:	id number of the buffer
 * @type:	enum v4l2_buf_type; buffer type (type == *_MPLANE for
 *		multiplanar buffers);
 * @bytesused:	number of bytes occupied by data in the buffer (payload);
 *		unused (set to 0) for multiplanar buffers
 * @flags:	buffer informational flags
 * @field:	enum v4l2_field; field order of the image in the buffer
 * @timestamp:	frame timestamp
 * @timecode:	frame timecode
 * @sequence:	sequence count of this frame
 * @memory:	enum v4l2_memory; the method, in which the actual video data is
 *		passed
 * @offset:	for non-multiplanar buffers with memory == V4L2_MEMORY_MMAP;
 *		offset from the start of the device memory for this plane,
 *		(or a "cookie" that should be passed to mmap() as offset)
 * @userptr:	for non-multiplanar buffers with memory == V4L2_MEMORY_USERPTR;
 *		a userspace pointer pointing to this buffer
 * @fd:		for non-multiplanar buffers with memory == V4L2_MEMORY_DMABUF;
 *		a userspace file descriptor associated with this buffer
 * @planes:	for multiplanar buffers; userspace pointer to the array of plane
 *		info structs for this buffer
 * @length:	size in bytes of the buffer (NOT its payload) for single-plane
 *		buffers (when type != *_MPLANE); number of elements in the
 *		planes array for multi-plane buffers
 * @config_store: this buffer should use this configuration store
 *
 * Contains data exchanged by application and driver using one of the Streaming
 * I/O methods.
 */
struct v4l2_buffer_custom {
  __u32	index;
  __u32	type;
  __u32	bytesused;
  __u32	flags;
  __u32	field;
  struct timeval timestamp;
  struct v4l2_timecode timecode;
  __u32 sequence;

  /* memory location */
  __u32 memory;
  union {
    __u32 offset;
    unsigned long userptr;
    struct v4l2_plane *planes;
    __s32 fd;
  } m;
  __u32	length;
  __u32	config_store;
  __u32 reserved;
};

#endif  // VIDEODEV2_CUSTOM_H_
