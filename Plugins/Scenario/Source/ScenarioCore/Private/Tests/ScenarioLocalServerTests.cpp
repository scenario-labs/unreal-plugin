// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ScenarioFakes.h"
#include "ScenarioLocalHttpServer.h"
#include "ScenarioTransfer.h"

namespace ScenarioServerTests
{
	TArray<uint8> Bytes(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}
}

using namespace ScenarioServerTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioLocalServerChecksTest, "Scenario.Core.LocalServer.Checks", SCENARIO_TEST_FLAGS)
bool FScenarioLocalServerChecksTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("ip with port"), FScenarioLocalHttpServer::IsLoopbackHostHeader(TEXT("127.0.0.1:9877"), 9877));
	TestTrue(TEXT("localhost"), FScenarioLocalHttpServer::IsLoopbackHostHeader(TEXT("localhost:9877"), 9877));
	TestTrue(TEXT("ipv6"), FScenarioLocalHttpServer::IsLoopbackHostHeader(TEXT("[::1]:9877"), 9877));
	TestTrue(TEXT("no port"), FScenarioLocalHttpServer::IsLoopbackHostHeader(TEXT("127.0.0.1"), 9877));
	TestFalse(TEXT("wrong port"), FScenarioLocalHttpServer::IsLoopbackHostHeader(TEXT("127.0.0.1:80"), 9877));
	TestFalse(TEXT("dns rebinding name"), FScenarioLocalHttpServer::IsLoopbackHostHeader(TEXT("evil.test:9877"), 9877));
	TestFalse(TEXT("lookalike"), FScenarioLocalHttpServer::IsLoopbackHostHeader(TEXT("127.0.0.1.evil.test"), 9877));
	TestFalse(TEXT("empty"), FScenarioLocalHttpServer::IsLoopbackHostHeader(TEXT(""), 9877));

	TestTrue(TEXT("no origin (CLI clients)"), FScenarioLocalHttpServer::IsAllowedOrigin(TEXT("")));
	TestTrue(TEXT("loopback origin"), FScenarioLocalHttpServer::IsAllowedOrigin(TEXT("http://localhost:6274")));
	TestFalse(TEXT("web page origin"), FScenarioLocalHttpServer::IsAllowedOrigin(TEXT("https://evil.test")));
	TestFalse(TEXT("null origin"), FScenarioLocalHttpServer::IsAllowedOrigin(TEXT("null")));
	TestFalse(TEXT("lookalike origin"), FScenarioLocalHttpServer::IsAllowedOrigin(TEXT("http://localhost.evil.test")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioLocalServerPortTest, "Scenario.Core.LocalServer.PortInUse", SCENARIO_TEST_FLAGS)
bool FScenarioLocalServerPortTest::RunTest(const FString& Parameters)
{
	// A second server must not share the first one's port, even where the socket allows port reuse.
	auto Handler = [](const FScenarioLocalRequest&, FScenarioLocalRespond Respond) { Respond(FScenarioLocalResponse::Text(200, TEXT("ok"))); };
	TSharedRef<FScenarioLocalHttpServer> First = MakeShared<FScenarioLocalHttpServer>(TEXT("PortTestA"), Handler);
	if (!First->Start(0))
	{
		AddError(TEXT("the first server did not start"));
		return false;
	}
	const int32 Taken = First->GetPort();
	TSharedRef<FScenarioLocalHttpServer> Second = MakeShared<FScenarioLocalHttpServer>(TEXT("PortTestB"), Handler);
	TestTrue(TEXT("second server starts on a later port"), Second->Start(Taken, 20));
	TestNotEqual(TEXT("ports differ"), Second->GetPort(), Taken);
	Second->Stop();
	First->Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioLocalServerParseTest, "Scenario.Core.LocalServer.Parse", SCENARIO_TEST_FLAGS)
bool FScenarioLocalServerParseTest::RunTest(const FString& Parameters)
{
	FScenarioLocalRequest Request;
	FString Error;
	int32 Status = 0;
	const FString Body = TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}");
	const FString Full = FString::Printf(TEXT("POST /mcp?x=1 HTTP/1.1\r\nHost: 127.0.0.1:9877\r\nContent-Type: application/json\r\nAuthorization: Bearer t\r\nContent-Length: %d\r\n\r\n%s"), Bytes(Body).Num(), *Body);
	TestTrue(TEXT("complete request"), FScenarioLocalHttpServer::ParseRequest(Bytes(Full), 1024, Request, Error, Status));
	TestEqual(TEXT("method"), Request.Method, FString(TEXT("POST")));
	TestEqual(TEXT("path"), Request.Path, FString(TEXT("/mcp")));
	TestEqual(TEXT("query"), Request.Query, FString(TEXT("x=1")));
	TestEqual(TEXT("one host"), Request.HostCount, 1);
	TestEqual(TEXT("header case folded"), Request.Header(TEXT("authorization")), FString(TEXT("Bearer t")));
	TestEqual(TEXT("body"), Request.BodyText(), Body);

	FScenarioLocalRequest Partial;
	Error.Reset();
	TestFalse(TEXT("incomplete body waits"), FScenarioLocalHttpServer::ParseRequest(Bytes(Full.LeftChop(5)), 1024, Partial, Error, Status));
	TestTrue(TEXT("no error while waiting"), Error.IsEmpty());

	FScenarioLocalRequest Big;
	const FString TooBig = TEXT("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 999999\r\n\r\n");
	TestFalse(TEXT("too large"), FScenarioLocalHttpServer::ParseRequest(Bytes(TooBig), 1024, Big, Error, Status));
	TestEqual(TEXT("413"), Status, 413);

	FScenarioLocalRequest Chunked;
	Error.Reset();
	const FString ChunkedText = TEXT("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n");
	FScenarioLocalHttpServer::ParseRequest(Bytes(ChunkedText), 1024, Chunked, Error, Status);
	TestFalse(TEXT("chunked refused"), Error.IsEmpty());

	FScenarioLocalRequest TwoHosts;
	Error.Reset();
	const FString TwoHostText = TEXT("GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nHost: evil.test\r\n\r\n");
	FScenarioLocalHttpServer::ParseRequest(Bytes(TwoHostText), 1024, TwoHosts, Error, Status);
	TestEqual(TEXT("both hosts counted"), TwoHosts.HostCount, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioTransferHostsTest, "Scenario.Core.Transfer.Hosts", SCENARIO_TEST_FLAGS)
bool FScenarioTransferHostsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("cdn"), FScenarioDownloader::IsAllowedUrl(TEXT("https://cdn.cloud.scenario.com/assets/a.png?sig=1")));
	TestFalse(TEXT("http refused"), FScenarioDownloader::IsAllowedUrl(TEXT("http://cdn.cloud.scenario.com/assets/a.png")));
	TestFalse(TEXT("other host refused"), FScenarioDownloader::IsAllowedUrl(TEXT("https://evil.test/a.png")));
	TestFalse(TEXT("suffix trick refused"), FScenarioDownloader::IsAllowedUrl(TEXT("https://cdn.cloud.scenario.com.evil.test/a.png")));
	TestTrue(TEXT("presigned part"), FScenarioUploader::IsAllowedPartUrl(TEXT("https://bucket.s3.us-east-1.amazonaws.com/part?X-Amz=1")));
	TestFalse(TEXT("part on another host"), FScenarioUploader::IsAllowedPartUrl(TEXT("https://evil.test/part")));
	return true;
}

#endif
