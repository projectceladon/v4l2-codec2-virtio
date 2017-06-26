LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
        C2VDAComponent.cpp \
        C2VDAAdaptor.cpp   \

LOCAL_C_INCLUDES += \
        $(TOP)/external/libchrome \
        $(TOP)/external/gtest/include \
        $(TOP)/external/v4l2_codec2 \
        $(TOP)/external/v4l2_codec2/vda \
        $(TOP)/frameworks/av/media/libstagefright/codec2/include \
        $(TOP)/frameworks/av/media/libstagefright/codec2/vndk/include \
        $(TOP)/frameworks/av/media/libstagefright/include \

LOCAL_MODULE:= libv4l2_codec2
LOCAL_MODULE_TAGS := optional

LOCAL_SHARED_LIBRARIES := libchrome \
                          liblog \
                          libmedia \
                          libstagefright \
                          libstagefright_codec2 \
                          libstagefright_foundation \
                          libutils \
                          libv4l2_codec2_vda \

# -Wno-unused-parameter is needed for libchrome/base codes
LOCAL_CFLAGS += -Werror -Wall -Wno-unused-parameter
LOCAL_CLANG := true
LOCAL_SANITIZE := unsigned-integer-overflow signed-integer-overflow

LOCAL_LDFLAGS := -Wl,-Bsymbolic

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
