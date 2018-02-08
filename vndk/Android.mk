LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

# Note: libv4l2_codec2_vndk should be only compiled on ARC-NYC env.
#       For ARC-PIC this makefile should be just skipped.

# define ANDROID_VERSION from PLATFORM_VERSION major number (ex. 7.0.1 -> 7)
ANDROID_VERSION := $(word 1, $(subst ., , $(PLATFORM_VERSION)))

ifeq ($(ANDROID_VERSION),7)  # NYC

LOCAL_SRC_FILES:= \
        C2AllocatorCrosGrallocNyc.cpp \
        C2AllocatorMemDealer.cpp \
        C2VDAStore.cpp \

LOCAL_C_INCLUDES += \
        $(TOP)/external/v4l2_codec2/vndk/include \
        $(TOP)/frameworks/av/media/libstagefright/codec2/include \
        $(TOP)/frameworks/av/media/libstagefright/codec2/vndk/include \
        $(TOP)/frameworks/av/media/libstagefright/include \
        $(TOP)/frameworks/native/include \

LOCAL_MODULE:= libv4l2_codec2_vndk
LOCAL_MODULE_TAGS := optional

LOCAL_SHARED_LIBRARIES := libbinder \
                          libcutils \
                          libgui \
                          liblog \
                          libmedia \
                          libstagefright \
                          libstagefright_codec2 \
                          libstagefright_codec2_vndk \
                          libstagefright_foundation \
                          libui \
                          libutils \

LOCAL_CFLAGS += -Werror -Wall -std=c++14
LOCAL_CLANG := true
LOCAL_SANITIZE := unsigned-integer-overflow signed-integer-overflow

LOCAL_LDFLAGS := -Wl,-Bsymbolic

LOCAL_CFLAGS += -DANDROID_VERSION_NYC

include $(BUILD_SHARED_LIBRARY)

endif  # ifeq ($(ANDROID_VERSION),7)
