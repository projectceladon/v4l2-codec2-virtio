/*
 * Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

package org.chromium.c2.test;

import android.app.Activity;
import android.os.AsyncTask;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/** Activity responsible for running the native Codec2.0 E2E tests. */
public class E2eTestActivity extends Activity {

    public final String TAG = "E2eTestActivity";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        System.loadLibrary("codectest");

        boolean encode = getIntent().getBooleanExtra("do-encode", false);
        String[] testArgs =
                getIntent().getStringArrayExtra("test-args") != null
                        ? getIntent().getStringArrayExtra("test-args")
                        : new String[0];
        String logFile = getIntent().getStringExtra("log-file");

        AsyncTask.execute(
                new Runnable() {
                    @Override
                    public void run() {
                        int res = c2VideoTest(encode, testArgs, testArgs.length, logFile);
                        Log.i(TAG, "Test returned result code " + res);

                        new Handler(Looper.getMainLooper())
                                .post(
                                        new Runnable() {
                                            @Override
                                            public void run() {
                                                finish();
                                            }
                                        });
                    }
                });
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // gtest can't reuse a process
        System.exit(0);
    }

    public native int c2VideoTest(
            boolean encode, String[] testArgs, int testArgsCount, String tmpFile);
}
