# Build the unit tests.
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)
LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk

LOCAL_MODULE := C2VDAComponent_test

LOCAL_MODULE_TAGS := tests

LOCAL_SRC_FILES := \
  C2VDAComponent_test.cpp \

LOCAL_SHARED_LIBRARIES := \
  libchrome \
  libcutils \
  liblog \
  libutils \
  libv4l2_codec2 \
  libv4l2_codec2_vda \

LOCAL_C_INCLUDES += \
  $(TOP)/external/v4l2_codec2 \
  $(TOP)/external/v4l2_codec2/vda \
  $(TOP)/frameworks/av/media/libstagefright/codec2/include \

LOCAL_CFLAGS += -Werror -Wall -std=c++14
LOCAL_CLANG := true

LOCAL_LDFLAGS := -Wl,-Bsymbolic

include $(BUILD_NATIVE_TEST)
