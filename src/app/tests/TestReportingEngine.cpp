/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *      This file implements unit tests for CHIP Interaction Model Reporting Engine
 *
 */

#include <app/InteractionModelEngine.h>
#include <app/reporting/Engine.h>
#include <core/CHIPCore.h>
#include <core/CHIPTLV.h>
#include <core/CHIPTLVDebug.hpp>
#include <core/CHIPTLVUtilities.hpp>
#include <messaging/ExchangeContext.h>
#include <messaging/ExchangeMgr.h>
#include <messaging/Flags.h>
#include <platform/CHIPDeviceLayer.h>
#include <protocols/secure_channel/MessageCounterManager.h>
#include <protocols/secure_channel/PASESession.h>
#include <stack/Stack.h>
#include <support/ErrorStr.h>
#include <support/UnitTestRegistration.h>
#include <system/SystemPacketBuffer.h>
#include <system/TLVPacketBufferBackingStore.h>
#include <transport/SecureSessionMgr.h>
#include <transport/raw/UDP.h>

#include <nlunit-test.h>

namespace chip {
static Stack<> gStack(chip::kTestDeviceNodeId);
constexpr ClusterId kTestClusterId    = 6;
constexpr EndpointId kTestEndpointId  = 1;
constexpr chip::FieldId kTestFieldId1 = 1;
constexpr chip::FieldId kTestFieldId2 = 2;
constexpr uint8_t kTestFieldValue1    = 1;
constexpr uint8_t kTestFieldValue2    = 2;

namespace app {
CHIP_ERROR ReadSingleClusterData(AttributePathParams & aAttributePathParams, TLV::TLVWriter & aWriter)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    VerifyOrExit(aAttributePathParams.mClusterId == kTestClusterId && aAttributePathParams.mEndpointId == kTestEndpointId,
                 err = CHIP_ERROR_INVALID_ARGUMENT);

    if (aAttributePathParams.mFieldId == kRootFieldId || aAttributePathParams.mFieldId == kTestFieldId1)
    {
        err = aWriter.Put(TLV::ContextTag(kTestFieldId1), kTestFieldValue1);
        SuccessOrExit(err);
    }
    if (aAttributePathParams.mFieldId == kRootFieldId || aAttributePathParams.mFieldId == kTestFieldId2)
    {
        err = aWriter.Put(TLV::ContextTag(kTestFieldId2), kTestFieldValue2);
        SuccessOrExit(err);
    }

exit:
    ChipLogFunctError(err);
    return err;
}

namespace reporting {
class TestReportingEngine
{
public:
    static void TestBuildAndSendSingleReportData(nlTestSuite * apSuite, void * apContext);
};

class TestExchangeDelegate : public Messaging::ExchangeDelegate
{
    void OnMessageReceived(Messaging::ExchangeContext * ec, const PacketHeader & packetHeader, const PayloadHeader & payloadHeader,
                           System::PacketBufferHandle && payload) override
    {}

    void OnResponseTimeout(Messaging::ExchangeContext * ec) override {}
};

void TestReportingEngine::TestBuildAndSendSingleReportData(nlTestSuite * apSuite, void * apContext)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    app::ReadHandler readHandler;
    Engine reportingEngine;
    System::PacketBufferTLVWriter writer;
    System::PacketBufferHandle readRequestbuf = System::PacketBufferHandle::New(System::PacketBuffer::kMaxSize);
    ReadRequest::Builder readRequestBuilder;
    AttributePathList::Builder attributePathListBuilder;
    AttributePath::Builder attributePathBuilder;

    err = InteractionModelEngine::GetInstance()->Init(&gStack.GetExchangeManager(), nullptr);
    NL_TEST_ASSERT(apSuite, err == CHIP_NO_ERROR);
    Messaging::ExchangeContext * exchangeCtx = gStack.GetExchangeManager().NewContext({ 0, 0, 0 }, nullptr);
    TestExchangeDelegate delegate;
    exchangeCtx->SetDelegate(&delegate);

    writer.Init(std::move(readRequestbuf));
    err = readRequestBuilder.Init(&writer);
    NL_TEST_ASSERT(apSuite, err == CHIP_NO_ERROR);
    attributePathListBuilder = readRequestBuilder.CreateAttributePathListBuilder();
    NL_TEST_ASSERT(apSuite, readRequestBuilder.GetError() == CHIP_NO_ERROR);
    attributePathBuilder = attributePathListBuilder.CreateAttributePathBuilder();
    NL_TEST_ASSERT(apSuite, attributePathListBuilder.GetError() == CHIP_NO_ERROR);
    attributePathBuilder =
        attributePathBuilder.NodeId(1).EndpointId(kTestEndpointId).ClusterId(kTestClusterId).FieldId(0).EndOfAttributePath();
    NL_TEST_ASSERT(apSuite, attributePathBuilder.GetError() == CHIP_NO_ERROR);
    attributePathListBuilder.EndOfAttributePathList();
    readRequestBuilder.EventNumber(1);
    NL_TEST_ASSERT(apSuite, readRequestBuilder.GetError() == CHIP_NO_ERROR);
    readRequestBuilder.EndOfReadRequest();
    NL_TEST_ASSERT(apSuite, readRequestBuilder.GetError() == CHIP_NO_ERROR);
    err = writer.Finalize(&readRequestbuf);
    NL_TEST_ASSERT(apSuite, err == CHIP_NO_ERROR);

    readHandler.OnReadRequest(exchangeCtx, std::move(readRequestbuf));
    reportingEngine.Init();
    err = reportingEngine.BuildAndSendSingleReportData(&readHandler);
    NL_TEST_ASSERT(apSuite, err == CHIP_ERROR_NOT_CONNECTED);
}
} // namespace reporting
} // namespace app
} // namespace chip

namespace {

// clang-format off
const nlTest sTests[] =
        {
                NL_TEST_DEF("CheckBuildAndSendSingleReportData", chip::app::reporting::TestReportingEngine::TestBuildAndSendSingleReportData),
                NL_TEST_SENTINEL()
        };
// clang-format on
} // namespace

int TestReportingEngine()
{
    // clang-format off
    nlTestSuite theSuite =
	{
        "TestReportingEngine",
        &sTests[0],
        nullptr,
        nullptr
    };
    // clang-format on

    NL_TEST_ASSERT(&theSuite, chip::gStack.Init(chip::StackParameters()) == CHIP_NO_ERROR);
    nlTestRunner(&theSuite, nullptr);
    int result = nlTestRunnerStats(&theSuite);
    NL_TEST_ASSERT(&theSuite, chip::gStack.Shutdown() == CHIP_NO_ERROR);

    return result;
}

CHIP_REGISTER_TEST_SUITE(TestReportingEngine)
