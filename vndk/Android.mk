LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
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
                          libstagefright_foundation \
                          libui \
                          libutils \

LOCAL_STATIC_LIBRARIES := libstagefright_codec2_vndk \

LOCAL_CFLAGS += -Werror -Wall -std=c++14
LOCAL_CLANG := true
LOCAL_SANITIZE := unsigned-integer-overflow signed-integer-overflow

LOCAL_LDFLAGS := -Wl,-Bsymbolic

# define ANDROID_VERSION from PLATFORM_VERSION major number (ex. 7.0.1 -> 7)
ANDROID_VERSION := $(word 1, $(subst ., , $(PLATFORM_VERSION)))

ifeq ($(ANDROID_VERSION),7)  # NYC
LOCAL_SRC_FILES += C2AllocatorCrosGrallocNyc.cpp

LOCAL_CFLAGS += -DANDROID_VERSION_NYC

else
LOCAL_SRC_FILES += C2AllocatorCrosGralloc.cpp

endif

include $(BUILD_SHARED_LIBRARY)
