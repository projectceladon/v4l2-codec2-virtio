# Codec2.0 End-to-end Test

## Manually run the test

You need to be running Android with v4l2_codec2 (e.g. ARC++, ARCVM) to run the
tests. Make sure the device under test (DUT) is connected with adb.

1.  Build the e2e test

    ```
    (From the android tree)
    $ mmm external/v4l2_codec2/tests/c2_e2e_test
    ```

2.  Install the test app and grant the necessary permissions

    ```
    (Outside CrOS chroot)
    $ adb install \
      out/target/product/bertha_x86_64/testcases/C2E2ETest/x86_64/C2E2ETest.apk
    $ adb shell pm grant \
      org.chromium.c2.test android.permission.WRITE_EXTERNAL_STORAGE
    $ adb shell pm grant \
      org.chromium.c2.test android.permission.READ_EXTERNAL_STORAGE
    ```

3.  Run E2E tests

    (1) Push the test stream and frame-wise MD5 file to DUT

    ```
    $ adb push test-25fps.h264 /sdcard/Download
    $ adb push test-25fps.h264.frames.md5 /sdcard/Download
    ```

    (2) Run the test binary

    ```
    $ adb shell am start -W -n \
        org.chromium.c2.test/.E2eTestActivity \
        --esa test-args "--test_video_data=/sdcard/Download/test-25fps.h264:320:240:250:258:::1" \
        --es log-file "/sdcard/Download/gtest_logs.txt"'
    ```

    (3) View the test output

    ```
    $ adb shell cat /sdcard/Download/gtest_logs.txt
    ```
