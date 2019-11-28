// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// #define LOG_NDEBUG 0
#define LOG_TAG "E2E_JNI"

#include <jni.h>
#include <pthread.h>
#include <sstream>

#include <android/native_window_jni.h>
#include <utils/Log.h>

#include "e2e_test_jni.h"
#include "mediacodec_decoder.h"

extern "C" {

JNIEXPORT jint JNICALL Java_org_chromium_c2_test_E2eTestActivity_c2VideoTest(
        JNIEnv* env, jobject thiz, jboolean encode, jobjectArray test_args, int test_args_count,
        jstring tmp_file_path) {
    const char* log_path = env->GetStringUTFChars(tmp_file_path, nullptr);
    if (freopen(log_path, "a+", stdout) == NULL) {
        env->ReleaseStringUTFChars(tmp_file_path, log_path);
        ALOGE("Failed to redirect stream to file: %s: %s\n", log_path, strerror(errno));
        return JNI_ERR;
    }
    ALOGI("Saving gtest output to %s\n", log_path);
    env->ReleaseStringUTFChars(tmp_file_path, log_path);

    char** args = new char*[test_args_count];
    for (int i = 0; i < test_args_count; i++) {
        jstring string = (jstring)env->GetObjectArrayElement(test_args, i);
        const char* c_str = env->GetStringUTFChars(string, nullptr);
        int len = env->GetStringUTFLength(string);

        args[i] = new char[len + 1];
        memcpy(args[i], c_str, len);
        args[i][len] = '\0';

        env->ReleaseStringUTFChars(string, c_str);
    }

    char** final_args = new char*[test_args_count + 1];
    final_args[0] = "e2e_test_jni";
    memcpy(final_args + 1, args, sizeof(args[0]) * test_args_count);

    int res;
    if (encode) {
        ALOGE("Encoder e2e tests not yet supported");
        res = JNI_ERR;
    } else {
        res = RunDecoderTests(final_args, test_args_count + 1);
    }
    delete[] final_args;

    for (int i = 0; i < test_args_count; i++) {
        delete[] args[i];
    }
    delete[] args;

    fflush(stdout);
    fclose(stdout);

    return res;
}
}
