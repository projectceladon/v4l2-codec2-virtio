LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
        C2VDAComponent.cpp \
        C2VDAAdaptor.cpp   \

LOCAL_C_INCLUDES += \
        $(TOP)/external/libchrome \
        $(TOP)/external/gtest/include \
        $(TOP)/external/v4l2_codec2/include \
        $(TOP)/external/v4l2_codec2/vda \
        $(TOP)/external/v4l2_codec2/vndk/include \
        $(TOP)/frameworks/av/media/libstagefright/codec2/include \
        $(TOP)/frameworks/av/media/libstagefright/codec2/vndk/include \
        $(TOP)/frameworks/av/media/libstagefright/include \

LOCAL_MODULE:= libv4l2_codec2
LOCAL_MODULE_TAGS := optional

LOCAL_SHARED_LIBRARIES := libbinder \
                          libchrome \
                          liblog \
                          libmedia \
                          libstagefright \
                          libstagefright_codec2 \
                          libstagefright_codec2_vndk \
                          libstagefright_foundation \
                          libutils \
                          libv4l2_codec2_vda \
                          libv4l2_codec2_vndk \

# -Wno-unused-parameter is needed for libchrome/base codes
LOCAL_CFLAGS += -Werror -Wall -Wno-unused-parameter -std=c++14
LOCAL_CFLAGS += -Wno-unused-lambda-capture -Wno-unknown-warning-option
LOCAL_CLANG := true
LOCAL_SANITIZE := unsigned-integer-overflow signed-integer-overflow

LOCAL_LDFLAGS := -Wl,-Bsymbolic

# Build C2VDAAdaptorProxy only for ARC++ case.
ifneq (,$(findstring cheets_,$(TARGET_PRODUCT)))

LOCAL_CFLAGS += -DV4L2_CODEC2_ARC
LOCAL_SRC_FILES += C2VDAAdaptorProxy.cpp
LOCAL_SRC_FILES := $(filter-out C2VDAAdaptor.cpp, $(LOCAL_SRC_FILES))
LOCAL_SHARED_LIBRARIES += libarcbridge \
                          libarcbridgeservice \
                          libarcvideobridge \
                          libmojo \

ANDROID_VERSION := $(word 1, $(subst ., , $(PLATFORM_VERSION)))
ifeq ($(ANDROID_VERSION),7)  # NYC
LOCAL_CFLAGS += -DANDROID_VERSION_NYC
endif

endif

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
