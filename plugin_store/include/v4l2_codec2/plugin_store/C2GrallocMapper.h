// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_V4L2_CODEC2_PLUGIN_STORE_C2_GRALLOC_MAPPER_H
#define ANDROID_V4L2_CODEC2_PLUGIN_STORE_C2_GRALLOC_MAPPER_H

#include <hardware/gralloc1.h>
#include <system/graphics.h>

class C2GrallocMapper {
public:
    ~C2GrallocMapper();

    static C2GrallocMapper& getMapper() {
        static C2GrallocMapper sInst;
        return sInst;
    }

    int getBufferSize(buffer_handle_t b, uint32_t& w, uint32_t& h);
    int getBufferFormat(buffer_handle_t b, int32_t& f);
    int getBufferStride(buffer_handle_t b, uint32_t& s);
    int lockBuffer(buffer_handle_t b, uint8_t*& data, uint32_t& s);
    int unlockBuffer(buffer_handle_t b);
    int importBuffer(buffer_handle_t b, buffer_handle_t* bufferHandle);
    int release(buffer_handle_t b);
    int getBackingStore(buffer_handle_t b, uint64_t* id);

private:
    C2GrallocMapper();
    int getGrallocDevice();

private:
    gralloc1_device_t* mGralloc = nullptr;
    GRALLOC1_PFN_LOCK pfnLock = nullptr;
    GRALLOC1_PFN_UNLOCK pfnUnlock = nullptr;
    GRALLOC1_PFN_GET_DIMENSIONS pfnGetDimensions = nullptr;
    GRALLOC1_PFN_GET_FORMAT pfnGetFormat = nullptr;
    GRALLOC1_PFN_GET_STRIDE pfnGetStride = nullptr;
    GRALLOC1_PFN_IMPORT_BUFFER pfnImportBuffer = nullptr;
    GRALLOC1_PFN_RELEASE pfnRelease = nullptr;
    GRALLOC1_PFN_GET_BACKING_STORE pfnGetBackingStore = nullptr;
};
#endif
