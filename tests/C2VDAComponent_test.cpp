// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VDAComponent_test"

#include <limits>
#include <stdio.h>

#include <C2VDAComponent.h>

#include <gtest/gtest.h>
#include <utils/Log.h>

#define UNUSED(expr) do { (void)(expr); } while (0)

namespace android {

template <class T>
std::unique_ptr<T> alloc_unique_cstr(const char* cstr) {
    size_t len = strlen(cstr);
    std::unique_ptr<T> ptr = T::alloc_unique(len);
    memcpy(ptr->m.mValue, cstr, len);
    return ptr;
}

// TODO(johnylin): should be fixed by ag/1930596, remove this then.
C2ComponentListener::~C2ComponentListener() {}

class TestListener: public C2ComponentListener {
public:
    ~TestListener() override {}
    void onWorkDone(std::weak_ptr<C2Component> component,
                    std::vector<std::unique_ptr<C2Work>> workItems) override {
        UNUSED(workItems);
        auto comp = component.lock();
        printf("TestListener::onWorkDone from component %s\n",
               comp->intf()->getName().c_str());
    }

    void onTripped(std::weak_ptr<C2Component> component,
                   std::vector<std::shared_ptr<C2SettingResult>> settingResult) override {
        UNUSED(settingResult);
        auto comp = component.lock();
        printf("TestListener::onTripped from component %s\n",
               comp->intf()->getName().c_str());
    }

    void onError(std::weak_ptr<C2Component> component,
                 uint32_t errorCode) override {
        auto comp = component.lock();
        printf("TestListener::onError Errno = %u from component %s\n",
               errorCode, comp->intf()->getName().c_str());
    }
};

const C2String testCompName = "v4l2.decoder";
const node_id testCompNodeId = 12345;

const char* MEDIA_MIMETYPE_VIDEO_RAW = "video/raw";
const char* MEDIA_MIMETYPE_VIDEO_AVC = "video/avc";

class C2VDAComponentTest : public ::testing::Test {
protected:
    C2VDAComponentTest() {
        mListener = std::make_shared<TestListener>();
        mComponent = std::make_shared<C2VDAComponent>(testCompName, testCompNodeId, mListener);
    }
    ~C2VDAComponentTest() override {}

    void SetUp() override {
        mIntf = mComponent->intf();
    }

    template <typename T>
    void testReadOnlyParam(const T* expected, const T* invalid);

    template <typename T>
    void testReadOnlyParamOnStack(const T* expected, const T* invalid);

    template <typename T>
    void testReadOnlyParamOnHeap(const T* expected, const T* invalid);

    template <typename T>
    void testReadOnlyFlexParam(
            const std::unique_ptr<T>& expected, const std::unique_ptr<T>& invalid);

    template <typename T>
    void testWritableParam(const T* const newParam);

    template <typename T>
    void testInvalidWritableParam(const T* const invalidParam);

    template <typename T>
    void testWritableVideoSizeParam(int32_t widthMin, int32_t widthMax, int32_t widthStep,
                                    int32_t heightMin, int32_t heightMax, int32_t heightStep);

    std::shared_ptr<C2Component> mComponent;
    std::shared_ptr<C2ComponentListener> mListener;
    std::shared_ptr<C2ComponentInterface> mIntf;
};

template <typename T>
void C2VDAComponentTest::testReadOnlyParam(const T* expected, const T* invalid) {
    testReadOnlyParamOnStack(expected, invalid);
    testReadOnlyParamOnHeap(expected, invalid);
}

template <typename T>
void C2VDAComponentTest::testReadOnlyParamOnStack(const T* expected, const T* invalid) {
    T param;
    std::vector<C2Param* const> stackParams{ &param };
    ASSERT_EQ(C2_OK, mIntf->query_nb(stackParams, {}, nullptr));
    EXPECT_EQ(*expected, param);

    std::vector<C2Param* const> params{ (C2Param* const)invalid };
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    ASSERT_EQ(C2_BAD_VALUE, mIntf->config_nb(params, &failures));
    ASSERT_EQ(1u, failures.size());
    EXPECT_EQ(C2SettingResult::READ_ONLY, failures[0]->failure);

    // The param must not change after failed config.
    ASSERT_EQ(C2_OK, mIntf->query_nb(stackParams, {}, nullptr));
    EXPECT_EQ(*expected, param);
}

template <typename T>
void C2VDAComponentTest::testReadOnlyParamOnHeap(const T* expected, const T* invalid) {
    std::vector<std::unique_ptr<C2Param>> heapParams;

    uint32_t index = expected->type();
    if (expected->forStream()) {
        index |= ((expected->stream() << 17) & 0x01FE0000) | 0x02000000;
    }

    ASSERT_EQ(C2_OK, mIntf->query_nb({}, {index}, &heapParams));
    ASSERT_EQ(1u, heapParams.size());
    EXPECT_EQ(*expected, *heapParams[0]);

    std::vector<C2Param* const> params{ (C2Param* const)invalid };
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    ASSERT_EQ(C2_BAD_VALUE, mIntf->config_nb(params, &failures));
    ASSERT_EQ(1u, failures.size());
    EXPECT_EQ(C2SettingResult::READ_ONLY, failures[0]->failure);

    // The param must not change after failed config.
    heapParams.clear();
    ASSERT_EQ(C2_OK, mIntf->query_nb({}, {index}, &heapParams));
    ASSERT_EQ(1u, heapParams.size());
    EXPECT_EQ(*expected, *heapParams[0]);
}

template <typename T>
void C2VDAComponentTest::testReadOnlyFlexParam(
        const std::unique_ptr<T>& expected, const std::unique_ptr<T>& invalid) {
    std::vector<std::unique_ptr<C2Param>> heapParams;

    uint32_t index = expected->type();
    if (expected->forStream()) {
        index |= ((expected->stream() << 17) & 0x01FE0000) | 0x02000000;
    }

    ASSERT_EQ(C2_OK, mIntf->query_nb({}, {index}, &heapParams));
    ASSERT_EQ(1u, heapParams.size());
    EXPECT_EQ(*expected, *heapParams[0]);

    std::vector<C2Param* const> params{ invalid.get() };
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    ASSERT_EQ(C2_BAD_VALUE, mIntf->config_nb(params, &failures));
    ASSERT_EQ(1u, failures.size());
    EXPECT_EQ(C2SettingResult::READ_ONLY, failures[0]->failure);

    // The param must not change after failed config.
    heapParams.clear();
    ASSERT_EQ(C2_OK, mIntf->query_nb({}, {index}, &heapParams));
    ASSERT_EQ(1u, heapParams.size());
    EXPECT_EQ(*expected, *heapParams[0]);
}

template <typename T>
void C2VDAComponentTest::testWritableParam(const T* const newParam) {
    std::vector<C2Param* const> params{ (C2Param* const)newParam };
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    ASSERT_EQ(C2_OK, mIntf->config_nb(params, &failures));
    EXPECT_EQ(0u, failures.size());

    // The param must change to newParam
    // Check like param on stack
    T param;
    std::vector<C2Param* const> stackParams{ &param };
    ASSERT_EQ(C2_OK, mIntf->query_nb(stackParams, {}, nullptr));
    EXPECT_EQ(*newParam, param);

    // Check also like param on heap
    std::vector<std::unique_ptr<C2Param>> heapParams;
    uint32_t index = newParam->type();
    if (newParam->forStream()) {
        index |= ((newParam->stream() << 17) & 0x01FE0000) | 0x02000000;
    }
    ASSERT_EQ(C2_OK, mIntf->query_nb({}, {index}, &heapParams));
    ASSERT_EQ(1u, heapParams.size());
    EXPECT_EQ(*newParam, *heapParams[0]);
}

template <typename T>
void C2VDAComponentTest::testInvalidWritableParam(const T* const invalidParam) {
    // Get the current parameter info
    T preParam;
    std::vector<C2Param* const> stackParams { &preParam };
    ASSERT_EQ(C2_OK, mIntf->query_nb(stackParams, {}, nullptr));

    // Config invalid value. The failure is expected
    std::vector<C2Param* const> params{ (C2Param* const)invalidParam };
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    ASSERT_EQ(C2_BAD_VALUE, mIntf->config_nb(params, &failures));
    EXPECT_EQ(1u, failures.size());

    //The param must not change after config failed
    T param;
    std::vector<C2Param* const> stackParams2 { &param };
    ASSERT_EQ(C2_OK, mIntf->query_nb(stackParams2, {}, nullptr));
    EXPECT_EQ(preParam, param);

    // Check also like param on heap
    std::vector<std::unique_ptr<C2Param>> heapParams;
    uint32_t index = invalidParam->type();
    if (invalidParam->forStream()) {
        index |= ((invalidParam->stream() << 17) & 0x01FE0000) | 0x02000000;
    }
    ASSERT_EQ(C2_OK, mIntf->query_nb({}, {index}, &heapParams));
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
void C2VDAComponentTest::testWritableVideoSizeParam(
        int32_t widthMin, int32_t widthMax, int32_t widthStep,
        int32_t heightMin, int32_t heightMax, int32_t heightStep) {
    // Test supported values of video size
    T valid;
    for (int32_t h = heightMin; h <= heightMax; h += heightStep) {
        for (int32_t w = widthMin; w <= widthMax; w += widthStep) {
            valid.mWidth = w;
            valid.mHeight = h;
            {
                SCOPED_TRACE("testWritableParam");
                testWritableParam(&valid);
                if (HasFailure()) {
                    printf("Failed while config width = %d, height = %d\n",
                           valid.mWidth, valid.mHeight);
                }
                if (HasFatalFailure()) return;
            }
        }
    }

    // Test invalid values video size
    T invalid;
    // Width or height is smaller than min values
    if (!isUnderflowSubstract(widthMin, widthStep)) {
        invalid.mWidth = widthMin - widthStep;
        invalid.mHeight = heightMin;
        testInvalidWritableParam(&invalid);
    }
    if (!isUnderflowSubstract(heightMin, heightStep)) {
        invalid.mWidth = widthMin;
        invalid.mHeight = heightMin - heightStep;
        testInvalidWritableParam(&invalid);
    }

    // Width or height is bigger than max values
    if (!isOverflowAdd(widthMax, widthStep)) {
        invalid.mWidth = widthMax + widthStep;
        invalid.mHeight = heightMax;
        testInvalidWritableParam(&invalid);
    }
    if (!isOverflowAdd(heightMax, heightStep)) {
        invalid.mWidth = widthMax;
        invalid.mHeight = heightMax + heightStep;
        testInvalidWritableParam(&invalid);
    }

    // Invalid width/height within the range
    if (widthStep != 1) {
        invalid.mWidth = widthMin + 1;
        invalid.mHeight = heightMin;
        testInvalidWritableParam(&invalid);
    }
    if (heightStep != 1) {
        invalid.mWidth = widthMin;
        invalid.mHeight = heightMin + 1;
        testInvalidWritableParam(&invalid);
    }
}

#define TRACED_FAILURE(func) do { \
    SCOPED_TRACE(#func); \
    func; \
    if (::testing::Test::HasFatalFailure()) return; \
} while (false)

TEST_F(C2VDAComponentTest, CreateInstance) {
    auto name = mIntf->getName();
    auto id = mIntf->getId();
    printf("name = %s\n", name.c_str());
    printf("node_id = %u\n", id);
    EXPECT_STREQ(name.c_str(), testCompName.c_str());
    EXPECT_EQ(id, testCompNodeId);
}

TEST_F(C2VDAComponentTest, TestDomainInfo) {
    C2ComponentDomainInfo expected(C2DomainVideo);
    C2ComponentDomainInfo invalid(C2DomainAudio);
    TRACED_FAILURE(testReadOnlyParam(&expected, &invalid));
}

TEST_F(C2VDAComponentTest, TestOutputColorFormat) {
    C2StreamFormatConfig::output expected(0u, kColorFormatYUV420Flexible);
    C2StreamFormatConfig::output invalid(0u, 0xdeadbeef);
    TRACED_FAILURE(testReadOnlyParam(&expected, &invalid));
}

TEST_F(C2VDAComponentTest, TestInputPortMime) {
    std::unique_ptr<C2PortMimeConfig::input> expected(
            alloc_unique_cstr<C2PortMimeConfig::input>(MEDIA_MIMETYPE_VIDEO_AVC));
    std::unique_ptr<C2PortMimeConfig::input> invalid(
            alloc_unique_cstr<C2PortMimeConfig::input>(MEDIA_MIMETYPE_VIDEO_RAW));
    TRACED_FAILURE(testReadOnlyFlexParam(expected, invalid));
}

TEST_F(C2VDAComponentTest, TestOutputPortMime) {
    std::unique_ptr<C2PortMimeConfig::output> expected(
            alloc_unique_cstr<C2PortMimeConfig::output>(MEDIA_MIMETYPE_VIDEO_RAW));
    std::unique_ptr<C2PortMimeConfig::output> invalid(
            alloc_unique_cstr<C2PortMimeConfig::output>(MEDIA_MIMETYPE_VIDEO_AVC));
    TRACED_FAILURE(testReadOnlyFlexParam(expected, invalid));
}

TEST_F(C2VDAComponentTest, TestVideoSize) {
    C2VideoSizeStreamInfo::output videoSize;
    std::vector<C2FieldSupportedValues> widthC2FSV;
    ASSERT_EQ(
        C2_OK,
        mIntf->getSupportedValues({ C2ParamField(&videoSize, &C2VideoSizeStreamInfo::mWidth) },
                                  &widthC2FSV));
    std::vector<C2FieldSupportedValues> heightC2FSV;
    ASSERT_EQ(
        C2_OK,
        mIntf->getSupportedValues({ C2ParamField(&videoSize, &C2VideoSizeStreamInfo::mHeight) },
                                  &heightC2FSV));
    ASSERT_EQ(1u, widthC2FSV.size());
    auto& widthFSVRange = widthC2FSV[0].range;
    int32_t widthMin = widthFSVRange.min.i32;
    int32_t widthMax = widthFSVRange.max.i32;
    int32_t widthStep = widthFSVRange.step.i32;

    ASSERT_EQ(1u, heightC2FSV.size());
    auto& heightFSVRange = heightC2FSV[0].range;
    int32_t heightMin = heightFSVRange.min.i32;
    int32_t heightMax = heightFSVRange.max.i32;
    int32_t heightStep = heightFSVRange.step.i32;

    // test updating invalid values
    TRACED_FAILURE(testWritableVideoSizeParam<C2VideoSizeStreamInfo::output>(
            widthMin, widthMax, widthStep, heightMin, heightMax, heightStep));
}

TEST_F(C2VDAComponentTest, TestMaxVideoSizeHint) {
    C2MaxVideoSizeHintPortSetting::input maxVideoSizeHint;
    std::vector<C2FieldSupportedValues> widthC2FSV;
    mIntf->getSupportedValues({ C2ParamField(
                                &maxVideoSizeHint,
                                &C2MaxVideoSizeHintPortSetting::mWidth) }, &widthC2FSV);
    std::vector<C2FieldSupportedValues> heightC2FSV;
    mIntf->getSupportedValues({ C2ParamField(
                                &maxVideoSizeHint,
                                &C2MaxVideoSizeHintPortSetting::mHeight) }, &heightC2FSV);

    ASSERT_EQ(1u, widthC2FSV.size());
    auto &widthFSVRange = widthC2FSV[0].range;
    int32_t widthMin = widthFSVRange.min.i32;
    int32_t widthMax = widthFSVRange.max.i32;
    int32_t widthStep = widthFSVRange.step.i32;

    ASSERT_EQ(1u, heightC2FSV.size());
    auto &heightFSVRange = heightC2FSV[0].range;
    int32_t heightMin = heightFSVRange.min.i32;
    int32_t heightMax = heightFSVRange.max.i32;
    int32_t heightStep = heightFSVRange.step.i32;

    TRACED_FAILURE(testWritableVideoSizeParam<C2MaxVideoSizeHintPortSetting::input>(
            widthMin, widthMax, widthStep, heightMin, heightMax, heightStep));
}

TEST_F(C2VDAComponentTest, TestInputCodecProfile) {
    C2StreamFormatConfig::input codecProfile;
    std::vector<C2FieldSupportedValues> profileValues;
    ASSERT_EQ(
        C2_OK,
        mIntf->getSupportedValues({ C2ParamField(&codecProfile, &C2StreamFormatConfig::mValue) },
                                  &profileValues));
    ASSERT_EQ(1u, profileValues.size());

    // TODO(johnylin): refactor this after supporting VALUES type for profile values.
    uint32_t profileMin =   profileValues[0].range.min.u32;
    uint32_t profileMax =   profileValues[0].range.max.u32;
    //uint32_t profileStep =  profileValues[0].range.step.u32;
    printf("Supported codec profile = [ %u, %u]\n", profileMin, profileMax);

    codecProfile.mValue = profileMin;
    TRACED_FAILURE(testWritableParam(&codecProfile));
    codecProfile.mValue = profileMax;
    TRACED_FAILURE(testWritableParam(&codecProfile));
    codecProfile.mValue = profileMax + 1;
    TRACED_FAILURE(testInvalidWritableParam(&codecProfile));
}

TEST_F(C2VDAComponentTest, TestUnsupportedParam) {
    C2ComponentTemporalInfo unsupportedParam;
    std::vector<C2Param* const> stackParams{ &unsupportedParam };
    ASSERT_EQ(C2_BAD_INDEX, mIntf->query_nb(stackParams, {}, nullptr));
    EXPECT_EQ(0u, unsupportedParam.size());  // invalidated
}

void dumpType(const C2FieldDescriptor::Type type) {
    switch (type) {
    case C2FieldDescriptor::INT32: printf("int32_t"); break;
    case C2FieldDescriptor::UINT32: printf("uint32_t"); break;
    case C2FieldDescriptor::INT64: printf("int64_t"); break;
    case C2FieldDescriptor::UINT64: printf("uint64_t"); break;
    case C2FieldDescriptor::FLOAT: printf("float"); break;
    default: printf("<flex>"); break;
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

TEST_F(C2VDAComponentTest, ParamReflector) {
    std::vector<std::shared_ptr<C2ParamDescriptor>> params;

    ASSERT_EQ(mIntf->getSupportedParams(&params), C2_OK);
    for (const auto& paramDesc : params) {
        printf("name: %s\n", paramDesc->name().c_str());
        printf("  required: %s\n", paramDesc->isRequired() ? "yes" : "no");
        printf("  type: %x\n", paramDesc->type().type());
        std::unique_ptr<C2StructDescriptor> desc{
                mIntf->getParamReflector()->describe(paramDesc->type().type())};
        if (desc.get())
            dumpStruct(*desc);
    }
}

TEST_F(C2VDAComponentTest, InitializeVDA) {
    C2StreamFormatConfig::input codecProfile;
    std::vector<C2FieldSupportedValues> profileValues;
    ASSERT_EQ(
        C2_OK,
        mIntf->getSupportedValues({ C2ParamField(&codecProfile, &C2StreamFormatConfig::mValue) },
                                  &profileValues));
    ASSERT_EQ(1u, profileValues.size());

    // TODO(johnylin): refactor this after supporting VALUES type for profile values.
    uint32_t profileMin = profileValues[0].range.min.u32;
    uint32_t profileMax = profileValues[0].range.max.u32;
    uint32_t profileStep =  profileValues[0].range.step.u32;
    for (uint32_t p = profileMin; p <= profileMax; p += profileStep) {
        printf("Configure profile = %u\n", p);
        codecProfile.mValue = p;
        std::vector<C2Param * const> params{ &codecProfile };
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        ASSERT_EQ(C2_OK, mIntf->config_nb(params, &failures));
        EXPECT_EQ(0u, failures.size());

        ASSERT_EQ(mComponent->start(), C2_OK);
        ASSERT_EQ(mComponent->start(), C2_BAD_STATE);  // already started
        ASSERT_EQ(mComponent->stop(), C2_OK);
        ASSERT_EQ(mComponent->stop(), C2_BAD_STATE);  // already stopped
    }
}

}  // namespace android
