# Copyright 2019 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SDK_VERSION := current

LOCAL_SRC_FILES := \
    $(call all-java-files-under, src) \

LOCAL_RESOURCE_DIR := \
    $(LOCAL_PATH)/res \

LOCAL_MULTILIB := both
LOCAL_PACKAGE_NAME := C2E2ETest
LOCAL_JNI_SHARED_LIBRARIES := libcodectest
LOCAL_MODULE_TAGS := tests

LOCAL_CXX_STL := libc++_static

include $(BUILD_PACKAGE)

include $(call all-makefiles-under,$(LOCAL_PATH))
