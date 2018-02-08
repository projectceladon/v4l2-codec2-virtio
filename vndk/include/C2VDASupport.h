// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VDA_SUPPORT_H
#define ANDROID_C2_VDA_SUPPORT_H

#include <C2Component.h>

namespace android {

/**
 * Returns the VDA allocator store.
 * \retval nullptr if the VDA allocator store could not be obtained
 */
std::shared_ptr<C2AllocatorStore> GetCodec2VDAAllocatorStore();

/**
 * Retrieves a block pool for a component.
 *
 * \param id        the local ID of the block pool
 * \param component the component using the block pool (must be non-null)
 * \param pool      pointer to where the obtained block pool shall be stored on success. nullptr
 *                  will be stored here on failure
 *
 * \retval C2_OK        the operation was successful
 * \retval C2_BAD_VALUE the component is null
 * \retval C2_NOT_FOUND if the block pool does not exist
 * \retval C2_NO_MEMORY not enough memory to fetch the block pool (this return value is only
 *                      possible for basic pools)
 * \retval C2_TIMED_OUT the operation timed out (this return value is only possible for basic pools)
 * \retval C2_REFUSED   no permission to complete any required allocation (this return value is only
 *                      possible for basic pools)
 * \retval C2_CORRUPTED some unknown, unrecoverable error occured during operation (unexpected,
 *                      this return value is only possible for basic pools)
 */
c2_status_t GetCodec2BlockPool(C2BlockPool::local_id_t id,
                               std::shared_ptr<const C2Component> component,
                               std::shared_ptr<C2BlockPool>* pool);

}  // namespace android

#endif  // ANDROID_C2_VDA_SUPPORT_H
