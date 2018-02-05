// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VDACompIntf_test"

#include <C2VDAComponent.h>

#include <gtest/gtest.h>
#include <utils/Log.h>

#include <stdio.h>
#include <limits>

#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

namespace android {

template <class T>
std::unique_ptr<T> alloc_unique_cstr(const char* cstr) {
    size_t len = strlen(cstr);
    std::unique_ptr<T> ptr = T::alloc_unique(len);
    memcpy(ptr->m.value, cstr, len);
    return ptr;
}

const C2String testCompName = "v4l2.h264.decode";
const c2_node_id_t testCompNodeId = 12345;

const char* MEDIA_MIMETYPE_VIDEO_RAW = "video/raw";
const char* MEDIA_MIMETYPE_VIDEO_AVC = "video/avc";

class C2VDACompIntfTest : public ::testing::Test {
protected:
    C2VDACompIntfTest() {
        mIntf = std::make_shared<C2VDAComponentIntf>(testCompName, testCompNodeId);
    }
    ~C2VDACompIntfTest() override {}

    template <typename T>
    void testReadOnlyParam(const T* expected, T* invalid);

    template <typename T>
    void checkReadOnlyFailureOnConfig(T* param);

    template <typename T>
    void testReadOnlyParamOnStack(const T* expected, T* invalid);

    template <typename T>
    void testReadOnlyParamOnHeap(const T* expected, T* invalid);

    template <typename T>
    void testWritableParam(T* newParam);

    template <typename T>
    void testInvalidWritableParam(T* invalidParam);

    template <typename T>
    void testWritableVideoSizeParam(int32_t widthMin, int32_t widthMax, int32_t widthStep,
                                    int32_t heightMin, int32_t heightMax, int32_t heightStep);

    std::shared_ptr<C2ComponentInterface> mIntf;
};

template <typename T>
void C2VDACompIntfTest::testReadOnlyParam(const T* expected, T* invalid) {
    testReadOnlyParamOnStack(expected, invalid);
    testReadOnlyParamOnHeap(expected, invalid);
}

template <typename T>
void C2VDACompIntfTest::checkReadOnlyFailureOnConfig(T* param) {
    std::vector<C2Param*> params{param};
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    ASSERT_EQ(C2_BAD_VALUE, mIntf->config_vb(params, C2_DONT_BLOCK, &failures));
    ASSERT_EQ(1u, failures.size());
    EXPECT_EQ(C2SettingResult::READ_ONLY, failures[0]->failure);
}

template <typename T>
void C2VDACompIntfTest::testReadOnlyParamOnStack(const T* expected, T* invalid) {
    T param;
    std::vector<C2Param*> stackParams{&param};
    ASSERT_EQ(C2_OK, mIntf->query_vb(stackParams, {}, C2_DONT_BLOCK, nullptr));
    EXPECT_EQ(*expected, param);

    checkReadOnlyFailureOnConfig(&param);
    checkReadOnlyFailureOnConfig(invalid);

    // The param must not change after failed config.
    ASSERT_EQ(C2_OK, mIntf->query_vb(stackParams, {}, C2_DONT_BLOCK, nullptr));
    EXPECT_EQ(*expected, param);
}

template <typename T>
void C2VDACompIntfTest::testReadOnlyParamOnHeap(const T* expected, T* invalid) {
    std::vector<std::unique_ptr<C2Param>> heapParams;

    uint32_t index = expected->index();

    ASSERT_EQ(C2_OK, mIntf->query_vb({}, {index}, C2_DONT_BLOCK, &heapParams));
    ASSERT_EQ(1u, heapParams.size());
    EXPECT_EQ(*expected, *heapParams[0]);

    checkReadOnlyFailureOnConfig(heapParams[0].get());
    checkReadOnlyFailureOnConfig(invalid);

    // The param must not change after failed config.
    heapParams.clear();
    ASSERT_EQ(C2_OK, mIntf->query_vb({}, {index}, C2_DONT_BLOCK, &heapParams));
    ASSERT_EQ(1u, heapParams.size());
    EXPECT_EQ(*expected, *heapParams[0]);
}

template <typename T>
void C2VDACompIntfTest::testWritableParam(T* newParam) {
    std::vector<C2Param*> params{newParam};
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    ASSERT_EQ(C2_OK, mIntf->config_vb(params, C2_DONT_BLOCK, &failures));
    EXPECT_EQ(0u, failures.size());

    // The param must change to newParam
    // Check like param on stack
    T param;
    std::vector<C2Param*> stackParams{&param};
    ASSERT_EQ(C2_OK, mIntf->query_vb(stackParams, {}, C2_DONT_BLOCK, nullptr));
    EXPECT_EQ(*newParam, param);

    // Check also like param on heap
    std::vector<std::unique_ptr<C2Param>> heapParams;
    ASSERT_EQ(C2_OK, mIntf->query_vb({}, {newParam->index()}, C2_DONT_BLOCK, &heapParams));
    ASSERT_EQ(1u, heapParams.size());
    EXPECT_EQ(*newParam, *heapParams[0]);
}

template <typename T>
void C2VDACompIntfTest::testInvalidWritableParam(T* invalidParam) {
    // Get the current parameter info
    T preParam;
    std::vector<C2Param*> stackParams{&preParam};
    ASSERT_EQ(C2_OK, mIntf->query_vb(stackParams, {}, C2_DONT_BLOCK, nullptr));

    // Config invalid value. The failure is expected
    std::vector<C2Param*> params{invalidParam};
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    ASSERT_EQ(C2_BAD_VALUE, mIntf->config_vb(params, C2_DONT_BLOCK, &failures));
    EXPECT_EQ(1u, failures.size());

    //The param must not change after config failed
    T param;
    std::vector<C2Param*> stackParams2{&param};
    ASSERT_EQ(C2_OK, mIntf->query_vb(stackParams2, {}, C2_DONT_BLOCK, nullptr));
    EXPECT_EQ(preParam, param);

    // Check also like param on heap
    std::vector<std::unique_ptr<C2Param>> heapParams;
    ASSERT_EQ(C2_OK, mIntf->query_vb({}, {invalidParam->index()}, C2_DONT_BLOCK, &heapParams));
    ASSERT_EQ(1u, heapParams.size());
    EXPECT_EQ(preParam, *heapParams[0]);
}

bool isUnderflowSubstract(int32_t a, int32_t b) {
    return a < 0 && b > a - std::numeric_limits<int32_t>::min();
}

bool isOverflowAdd(int32_t a, int32_t b) {
    return a > 0 && b > std::numeric_limits<int32_t>::max() - a;
}

template <typename T>
void C2VDACompIntfTest::testWritableVideoSizeParam(int32_t widthMin, int32_t widthMax,
                                                   int32_t widthStep, int32_t heightMin,
                                                   int32_t heightMax, int32_t heightStep) {
    // Test supported values of video size
    T valid;
    for (int32_t h = heightMin; h <= heightMax; h += heightStep) {
        for (int32_t w = widthMin; w <= widthMax; w += widthStep) {
            valid.width = w;
            valid.height = h;
            {
                SCOPED_TRACE("testWritableParam");
                testWritableParam(&valid);
                if (HasFailure()) {
                    printf("Failed while config width = %d, height = %d\n", valid.width,
                           valid.height);
                }
                if (HasFatalFailure()) return;
            }
        }
    }

    // Test invalid values video size
    T invalid;
    // Width or height is smaller than min values
    if (!isUnderflowSubstract(widthMin, widthStep)) {
        invalid.width = widthMin - widthStep;
        invalid.height = heightMin;
        testInvalidWritableParam(&invalid);
    }
    if (!isUnderflowSubstract(heightMin, heightStep)) {
        invalid.width = widthMin;
        invalid.height = heightMin - heightStep;
        testInvalidWritableParam(&invalid);
    }

    // Width or height is bigger than max values
    if (!isOverflowAdd(widthMax, widthStep)) {
        invalid.width = widthMax + widthStep;
        invalid.height = heightMax;
        testInvalidWritableParam(&invalid);
    }
    if (!isOverflowAdd(heightMax, heightStep)) {
        invalid.width = widthMax;
        invalid.height = heightMax + heightStep;
        testInvalidWritableParam(&invalid);
    }

    // Invalid width/height within the range
    if (widthStep != 1) {
        invalid.width = widthMin + 1;
        invalid.height = heightMin;
        testInvalidWritableParam(&invalid);
    }
    if (heightStep != 1) {
        invalid.width = widthMin;
        invalid.height = heightMin + 1;
        testInvalidWritableParam(&invalid);
    }
}

#define TRACED_FAILURE(func)                            \
    do {                                                \
        SCOPED_TRACE(#func);                            \
        func;                                           \
        if (::testing::Test::HasFatalFailure()) return; \
    } while (false)

TEST_F(C2VDACompIntfTest, CreateInstance) {
    auto name = mIntf->getName();
    auto id = mIntf->getId();
    printf("name = %s\n", name.c_str());
    printf("node_id = %u\n", id);
    EXPECT_STREQ(name.c_str(), testCompName.c_str());
    EXPECT_EQ(id, testCompNodeId);
}

TEST_F(C2VDACompIntfTest, TestDomainInfo) {
    C2ComponentDomainInfo expected(C2DomainVideo);
    C2ComponentDomainInfo invalid(C2DomainAudio);
    TRACED_FAILURE(testReadOnlyParam(&expected, &invalid));
}

TEST_F(C2VDACompIntfTest, TestInputFormat) {
    C2StreamFormatConfig::input expected(0u, C2FormatCompressed);
    expected.setStream(0);  // only support single stream
    C2StreamFormatConfig::input invalid(0u, C2FormatVideo);
    invalid.setStream(0);  // only support single stream
    TRACED_FAILURE(testReadOnlyParam(&expected, &invalid));
}

TEST_F(C2VDACompIntfTest, TestOutputFormat) {
    C2StreamFormatConfig::output expected(0u, C2FormatVideo);
    expected.setStream(0);  // only support single stream
    C2StreamFormatConfig::output invalid(0u, C2FormatCompressed);
    invalid.setStream(0);  // only support single stream
    TRACED_FAILURE(testReadOnlyParam(&expected, &invalid));
}

TEST_F(C2VDACompIntfTest, TestInputPortMime) {
    std::unique_ptr<C2PortMimeConfig::input> expected(
            alloc_unique_cstr<C2PortMimeConfig::input>(MEDIA_MIMETYPE_VIDEO_AVC));
    std::unique_ptr<C2PortMimeConfig::input> invalid(
            alloc_unique_cstr<C2PortMimeConfig::input>(MEDIA_MIMETYPE_VIDEO_RAW));
    TRACED_FAILURE(testReadOnlyParamOnHeap(expected.get(), invalid.get()));
}

TEST_F(C2VDACompIntfTest, TestOutputPortMime) {
    std::unique_ptr<C2PortMimeConfig::output> expected(
            alloc_unique_cstr<C2PortMimeConfig::output>(MEDIA_MIMETYPE_VIDEO_RAW));
    std::unique_ptr<C2PortMimeConfig::output> invalid(
            alloc_unique_cstr<C2PortMimeConfig::output>(MEDIA_MIMETYPE_VIDEO_AVC));
    TRACED_FAILURE(testReadOnlyParamOnHeap(expected.get(), invalid.get()));
}

TEST_F(C2VDACompIntfTest, TestVideoSize) {
    C2VideoSizeStreamInfo::output videoSize;
    videoSize.setStream(0);  // only support single stream
    std::vector<C2FieldSupportedValuesQuery> widthC2FSV = {
            {C2ParamField(&videoSize, &C2VideoSizeStreamInfo::width),
             C2FieldSupportedValuesQuery::CURRENT},
    };
    ASSERT_EQ(C2_OK, mIntf->querySupportedValues_vb(widthC2FSV, C2_DONT_BLOCK));
    std::vector<C2FieldSupportedValuesQuery> heightC2FSV = {
            {C2ParamField(&videoSize, &C2VideoSizeStreamInfo::height),
             C2FieldSupportedValuesQuery::CURRENT},
    };
    ASSERT_EQ(C2_OK, mIntf->querySupportedValues_vb(heightC2FSV, C2_DONT_BLOCK));
    ASSERT_EQ(1u, widthC2FSV.size());
    ASSERT_EQ(C2_OK, widthC2FSV[0].status);
    ASSERT_EQ(C2FieldSupportedValues::RANGE, widthC2FSV[0].values.type);
    auto& widthFSVRange = widthC2FSV[0].values.range;
    int32_t widthMin = widthFSVRange.min.i32;
    int32_t widthMax = widthFSVRange.max.i32;
    int32_t widthStep = widthFSVRange.step.i32;

    ASSERT_EQ(1u, heightC2FSV.size());
    ASSERT_EQ(C2_OK, heightC2FSV[0].status);
    ASSERT_EQ(C2FieldSupportedValues::RANGE, heightC2FSV[0].values.type);
    auto& heightFSVRange = heightC2FSV[0].values.range;
    int32_t heightMin = heightFSVRange.min.i32;
    int32_t heightMax = heightFSVRange.max.i32;
    int32_t heightStep = heightFSVRange.step.i32;

    // test updating invalid values
    TRACED_FAILURE(testWritableVideoSizeParam<C2VideoSizeStreamInfo::output>(
            widthMin, widthMax, widthStep, heightMin, heightMax, heightStep));
}

TEST_F(C2VDACompIntfTest, TestMaxVideoSizeHint) {
    C2MaxVideoSizeHintPortSetting::input maxVideoSizeHint;
    std::vector<C2FieldSupportedValuesQuery> widthC2FSV = {
            {C2ParamField(&maxVideoSizeHint, &C2MaxVideoSizeHintPortSetting::width),
             C2FieldSupportedValuesQuery::CURRENT},
    };
    mIntf->querySupportedValues_vb(widthC2FSV, C2_DONT_BLOCK);
    std::vector<C2FieldSupportedValuesQuery> heightC2FSV = {
            {C2ParamField(&maxVideoSizeHint, &C2MaxVideoSizeHintPortSetting::height),
             C2FieldSupportedValuesQuery::CURRENT},
    };
    mIntf->querySupportedValues_vb(heightC2FSV, C2_DONT_BLOCK);

    ASSERT_EQ(1u, widthC2FSV.size());
    ASSERT_EQ(C2_OK, widthC2FSV[0].status);
    ASSERT_EQ(C2FieldSupportedValues::RANGE, widthC2FSV[0].values.type);
    auto& widthFSVRange = widthC2FSV[0].values.range;
    int32_t widthMin = widthFSVRange.min.i32;
    int32_t widthMax = widthFSVRange.max.i32;
    int32_t widthStep = widthFSVRange.step.i32;

    ASSERT_EQ(1u, heightC2FSV.size());
    ASSERT_EQ(C2_OK, heightC2FSV[0].status);
    ASSERT_EQ(C2FieldSupportedValues::RANGE, heightC2FSV[0].values.type);
    auto& heightFSVRange = heightC2FSV[0].values.range;
    int32_t heightMin = heightFSVRange.min.i32;
    int32_t heightMax = heightFSVRange.max.i32;
    int32_t heightStep = heightFSVRange.step.i32;

    TRACED_FAILURE(testWritableVideoSizeParam<C2MaxVideoSizeHintPortSetting::input>(
            widthMin, widthMax, widthStep, heightMin, heightMax, heightStep));
}

TEST_F(C2VDACompIntfTest, TestInputCodecProfile) {
    C2VDAStreamProfileConfig::input codecProfile;
    codecProfile.setStream(0);  // only support single stream
    std::vector<C2FieldSupportedValuesQuery> profileValues = {
            {C2ParamField(&codecProfile, &C2VDAStreamProfileConfig::value),
             C2FieldSupportedValuesQuery::CURRENT},
    };
    ASSERT_EQ(C2_OK, mIntf->querySupportedValues_vb(profileValues, C2_DONT_BLOCK));
    ASSERT_EQ(1u, profileValues.size());
    ASSERT_EQ(C2_OK, profileValues[0].status);

    for (const auto& profile : profileValues[0].values.values) {
        codecProfile.value = profile.u32;
        TRACED_FAILURE(testWritableParam(&codecProfile));
    }
    codecProfile.value = 999;  // hard-coded invalid profile number
    TRACED_FAILURE(testInvalidWritableParam(&codecProfile));
}

TEST_F(C2VDACompIntfTest, TestUnsupportedParam) {
    C2ComponentTemporalInfo unsupportedParam;
    std::vector<C2Param*> stackParams{&unsupportedParam};
    ASSERT_EQ(C2_BAD_INDEX, mIntf->query_vb(stackParams, {}, C2_DONT_BLOCK, nullptr));
    EXPECT_EQ(0u, unsupportedParam.size());  // invalidated
}

void dumpType(const C2FieldDescriptor::type_t type) {
    switch (type) {
    case C2FieldDescriptor::INT32:
        printf("int32_t");
        break;
    case C2FieldDescriptor::UINT32:
        printf("uint32_t");
        break;
    case C2FieldDescriptor::INT64:
        printf("int64_t");
        break;
    case C2FieldDescriptor::UINT64:
        printf("uint64_t");
        break;
    case C2FieldDescriptor::FLOAT:
        printf("float");
        break;
    default:
        printf("<flex>");
        break;
    }
}

void dumpStruct(const C2StructDescriptor& sd) {
    printf("  struct: { ");
    for (const C2FieldDescriptor& f : sd) {
        printf("%s:", f.name());
        dumpType(f.type());
        printf(", ");
    }
    printf("}\n");
}

// TODO: move this to some component store test
// TEST_F(C2VDACompIntfTest, ParamReflector) {
//     std::shared_ptr<C2ComponentStore> store(new C2VDAComponentStore());

//     std::vector<std::shared_ptr<C2ParamDescriptor>> params;

//     ASSERT_EQ(mIntf->querySupportedParams_nb(&params), C2_OK);
//     for (const auto& paramDesc : params) {
//         printf("name: %s\n", paramDesc->name().c_str());
//         printf("  required: %s\n", paramDesc->isRequired() ? "yes" : "no");
//         printf("  type: %x\n", paramDesc->index().type());
//         std::unique_ptr<C2StructDescriptor> desc{
//                 store->getParamReflector()->describe(paramDesc->index().type())};
//         if (desc.get()) dumpStruct(*desc);
//     }
// }
}  // namespace android
