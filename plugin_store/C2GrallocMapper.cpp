// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define LOG_NDEBUG 0
#include <cutils/log.h>
#include <unistd.h>

#include <v4l2_codec2/plugin_store/C2GrallocMapper.h>

C2GrallocMapper::C2GrallocMapper() {
    ALOGV("%s", __func__);
    getGrallocDevice();
}

C2GrallocMapper::~C2GrallocMapper() {
    ALOGV("%s", __func__);
    if (mGralloc) {
        gralloc1_close(mGralloc);
    }
}

int C2GrallocMapper::getGrallocDevice() {
    ALOGV("%s", __func__);

    const hw_module_t* mod = nullptr;

    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &mod) != 0) {
        ALOGE("Failed to load gralloc module");
        return -1;
    }

    if (gralloc1_open(mod, &mGralloc) != 0) {
        ALOGE("Failed to open gralloc1 device");
        return -1;
    }

    if (mGralloc) {
        pfnLock = (GRALLOC1_PFN_LOCK)(mGralloc->getFunction(mGralloc, GRALLOC1_FUNCTION_LOCK));
        pfnUnlock =
                (GRALLOC1_PFN_UNLOCK)(mGralloc->getFunction(mGralloc, GRALLOC1_FUNCTION_UNLOCK));
        pfnGetDimensions = (GRALLOC1_PFN_GET_DIMENSIONS)(mGralloc->getFunction(
                mGralloc, GRALLOC1_FUNCTION_GET_DIMENSIONS));
        pfnGetFormat = (GRALLOC1_PFN_GET_FORMAT)(mGralloc->getFunction(
                mGralloc, GRALLOC1_FUNCTION_GET_FORMAT));
        pfnGetStride = (GRALLOC1_PFN_GET_STRIDE)(mGralloc->getFunction(
                mGralloc, GRALLOC1_FUNCTION_GET_STRIDE));
        pfnImportBuffer = (GRALLOC1_PFN_IMPORT_BUFFER)(mGralloc->getFunction(
                mGralloc, GRALLOC1_FUNCTION_IMPORT_BUFFER));
        pfnRelease =
                (GRALLOC1_PFN_RELEASE)(mGralloc->getFunction(mGralloc, GRALLOC1_FUNCTION_RELEASE));
        pfnGetBackingStore = (GRALLOC1_PFN_GET_BACKING_STORE)(mGralloc->getFunction(
                mGralloc, GRALLOC1_FUNCTION_GET_BACKING_STORE));
    }
    return 0;
}

int C2GrallocMapper::getBufferSize(buffer_handle_t b, uint32_t& w, uint32_t& h) {
    ALOGV("%s", __func__);

    if (!b || !pfnGetDimensions) {
        return -1;
    }

    if (pfnGetDimensions(mGralloc, b, &w, &h) != 0) {
        ALOGE("Failed to getDimensions for buffer %p", b);
        return -1;
    }

    return 0;
}

int C2GrallocMapper::getBufferFormat(buffer_handle_t b, int32_t& f) {
    ALOGV("%s", __func__);

    if (!b || !pfnGetFormat) {
        return -1;
    }

    if (pfnGetFormat(mGralloc, b, &f) != 0) {
        ALOGE("Failed to pfnGetFormat for buffer %p", b);
        return -1;
    }

    return 0;
}

int C2GrallocMapper::getBufferStride(buffer_handle_t b, uint32_t& s) {
    ALOGV("%s", __func__);

    if (!b || !pfnGetStride) {
        return -1;
    }

    if (pfnGetStride(mGralloc, b, &s) != 0) {
        ALOGE("Failed to get buffer %p stride", b);
        return -1;
    }

    return 0;
}

int C2GrallocMapper::lockBuffer(buffer_handle_t b, uint8_t*& data, uint32_t& s) {
    ALOGV("%s", __func__);

    if (!b || !pfnLock) {
        return -1;
    }

    uint32_t w, h;
    if (getBufferSize(b, w, h) < 0) {
        ALOGE("Failed to get buffer size for buffer %p", b);
        return -1;
    }
    if (getBufferStride(b, s) < 0) {
        ALOGE("Failed to get buffer %p stride", b);
        return -1;
    }

    gralloc1_rect_t rect = {0, 0, (int32_t)w, (int32_t)h};
    int fenceFd = -1;
    if (pfnLock(mGralloc, b, 0x0, 0x3, &rect, (void**)&data, fenceFd) != 0) {
        ALOGE("Failed to lock buffer %p", b);
        return -1;
    }
    // ALOGD("lock buffer with w=%d h= %d return addr %p stride=%d", w, h, *data,
    // *s);
    return 0;
}

int C2GrallocMapper::unlockBuffer(buffer_handle_t b) {
    ALOGV("%s", __func__);

    if (!b || !pfnUnlock) {
        return -1;
    }

    int releaseFenceFd = -1;

    if (pfnUnlock(mGralloc, b, &releaseFenceFd) != 0) {
        ALOGE("Failed to unlock buffer %p", b);
        return -1;
    }
    if (releaseFenceFd >= 0) {
        close(releaseFenceFd);
    }
    return 0;
}

int C2GrallocMapper::importBuffer(buffer_handle_t b, buffer_handle_t* bufferHandle) {
    ALOGV("%s", __func__);

    if (!b || !pfnImportBuffer) {
        return -1;
    }
    if (pfnImportBuffer(mGralloc, b, bufferHandle) != 0) {
        return -1;
    }
    return 0;
}

int C2GrallocMapper::release(buffer_handle_t b) {
    ALOGV("%s", __func__);

    if (!b || !pfnRelease) {
        return -1;
    }
    if (pfnRelease(mGralloc, b) != 0) {
        return -1;
    }
    return 0;
}

int C2GrallocMapper::getBackingStore(buffer_handle_t b, uint64_t* id) {
    ALOGV("%s", __func__);

    if (!b || !pfnGetBackingStore) {
        return -1;
    }
    if (pfnGetBackingStore(mGralloc, b, id) != 0) {
        return -1;
    }
    return 0;
}
