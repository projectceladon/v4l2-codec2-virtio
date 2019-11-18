# Copyright 2019 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_C_INCLUDES := \
	system/core/include \

LOCAL_MULTILIB := both

LOCAL_SRC_FILES := \
	video_decoder_e2e_test.cpp \
	e2e_test_jni.cpp \
	common.cpp \
	encoded_data_helper.cpp \
	video_frame.cpp \
	md5.cpp \
	mediacodec_decoder.cpp \

# TODO(stevensd): Port encoder tests

LOCAL_MODULE_TAGS := optional

LOCAL_SHARED_LIBRARIES := \
	liblog \
	libmediandk \
	libandroid \

LOCAL_SDK_VERSION := 21
LOCAL_NDK_STL_VARIANT := c++_static

LOCAL_STATIC_LIBRARIES := libgtest_ndk_c++ libgtest_main_ndk_c++

LOCAL_MODULE := libcodectest

# TODO(stevensd): Fix and reenable warnings
LOCAL_CFLAGS += -Wno-everything


include $(BUILD_SHARED_LIBRARY)
