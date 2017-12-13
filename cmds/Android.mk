LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
        codec2.cpp \

LOCAL_C_INCLUDES += \
        $(TOP)/external/libchrome \
        $(TOP)/external/gtest/include \
        $(TOP)/external/v4l2_codec2 \
        $(TOP)/external/v4l2_codec2/vda \
        $(TOP)/external/v4l2_codec2/vndk/include \
        $(TOP)/frameworks/av/media/libstagefright/codec2/include \
        $(TOP)/frameworks/av/media/libstagefright/codec2/vndk/include \
        $(TOP)/frameworks/av/media/libstagefright/include \
        $(TOP)/frameworks/native/include \

LOCAL_MODULE := v4l2_codec2_testapp
LOCAL_MODULE_TAGS := optional

LOCAL_SHARED_LIBRARIES := libbinder \
                          libchrome \
                          libcutils \
                          libgui \
                          liblog \
                          libmedia \
                          libstagefright \
                          libstagefright_codec2 \
                          libstagefright_foundation \
                          libui \
                          libutils \
                          libv4l2_codec2 \
                          libv4l2_codec2_vda \
                          libv4l2_codec2_vndk \

LOCAL_STATIC_LIBRARIES := libstagefright_codec2_vndk \

# -Wno-unused-parameter is needed for libchrome/base codes
LOCAL_CFLAGS += -Werror -Wall -Wno-unused-parameter
LOCAL_CLANG := true

include $(BUILD_EXECUTABLE)
