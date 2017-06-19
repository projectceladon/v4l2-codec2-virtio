LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_CPP_EXTENSION:= .cc
LOCAL_SRC_FILES:= \
        bit_reader.cc       \
        bit_reader_core.cc  \
        h264_bit_reader.cc  \
        h264_decoder.cc     \
        h264_dpb.cc         \
        h264_parser.cc      \
        ranges.cc           \
        v4l2_device.cc      \
        video_codecs.cc     \
        video_decode_accelerator.cc \
        vp8_bool_decoder.cc \
        vp8_decoder.cc      \
        vp8_parser.cc       \
        vp8_picture.cc      \
        vp9_bool_decoder.cc \
        vp9_compressed_header_parser.cc \
        vp9_decoder.cc      \
        vp9_parser.cc       \
        vp9_picture.cc      \
        vp9_raw_bits_reader.cc \
        vp9_uncompressed_header_parser.cc \

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
