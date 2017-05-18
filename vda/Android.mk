LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_CPP_EXTENSION:= .cc
LOCAL_SRC_FILES:= \
        h264_bit_reader.cc  \
        h264_decoder.cc     \
        h264_dpb.cc         \
        h264_parser.cc      \
        ranges.cc           \

LOCAL_C_INCLUDES += \
        $(TOP)/external/libchrome \

LOCAL_MODULE:= libv4l2_codec2_vda

LOCAL_SHARED_LIBRARIES := libchrome \

# -Wno-unused-parameter is needed for libchrome/base codes
LOCAL_CFLAGS += -Werror -Wall -Wno-unused-parameter
LOCAL_CLANG := true
LOCAL_SANITIZE := unsigned-integer-overflow signed-integer-overflow

LOCAL_LDFLAGS := -Wl,-Bsymbolic

include $(BUILD_SHARED_LIBRARY)
