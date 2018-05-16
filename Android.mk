# Build only if hardware/google/av is visible; otherwise, don't build any
# target under this repository.
ifneq (,$(findstring hardware/google/av,$(PRODUCT_SOONG_NAMESPACES)))

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
        $(TOP)/frameworks/av/media/libstagefright/include \
        $(TOP)/hardware/google/av/codec2/include \
        $(TOP)/hardware/google/av/codec2/vndk/include \

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

# -Wno-unused-parameter is needed for libchrome/base codes
LOCAL_CFLAGS += -Werror -Wall -Wno-unused-parameter -std=c++14
LOCAL_CFLAGS += -Wno-unused-lambda-capture -Wno-unknown-warning-option
LOCAL_CLANG := true
LOCAL_SANITIZE := unsigned-integer-overflow signed-integer-overflow

LOCAL_LDFLAGS := -Wl,-Bsymbolic

# Build C2VDAAdaptorProxy only for ARC++ case.
ifneq (,$(findstring cheets_,$(TARGET_PRODUCT)))

LOCAL_CFLAGS += -DV4L2_CODEC2_ARC
LOCAL_SRC_FILES += \
                   C2ArcVideoAcceleratorFactory.cpp \
                   C2VDAAdaptorProxy.cpp \

LOCAL_SRC_FILES := $(filter-out C2VDAAdaptor.cpp, $(LOCAL_SRC_FILES))
LOCAL_SHARED_LIBRARIES += libarcbridge \
                          libarcbridgeservice \
                          libarcvideobridge \
                          libmojo \

endif

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))

endif
