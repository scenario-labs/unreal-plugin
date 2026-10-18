// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ScenarioCU.h"
#include "ScenarioErrors.h"
#include "ScenarioFakes.h"
#include "ScenarioFiles.h"
#include "ScenarioHash.h"
#include "ScenarioHttp.h"
#include "ScenarioJson.h"
#include "ScenarioRpc.h"
#include "ScenarioTelemetry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioJsonCanonicalTest, "Scenario.Core.Json.Canonical", SCENARIO_TEST_FLAGS)
bool FScenarioJsonCanonicalTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> A = ScenarioJson::ParseObject(TEXT("{\"b\":1.0,\"a\":{\"y\":[2,\"x\"],\"x\":true}}"));
	const TSharedPtr<FJsonObject> B = ScenarioJson::ParseObject(TEXT("{ \"a\": { \"x\": true, \"y\": [2.0, \"x\"] }, \"b\": 1 }"));
	TestEqual(TEXT("keys sorted, whole numbers as integers"), ScenarioJson::Canonical(A), FString(TEXT("{\"a\":{\"x\":true,\"y\":[2,\"x\"]},\"b\":1}")));
	TestEqual(TEXT("same request, same canonical text"), ScenarioJson::Canonical(A), ScenarioJson::Canonical(B));
	TestEqual(TEXT("11"), ScenarioJson::NumberText(11.0), FString(TEXT("11")));
	TestEqual(TEXT("2.75"), ScenarioJson::NumberText(2.75), FString(TEXT("2.75")));
	TestEqual(TEXT("0.1"), ScenarioJson::NumberText(0.1), FString(TEXT("0.1")));
	TestEqual(TEXT("escape"), ScenarioJson::EscapeString(TEXT("a\"b\n")), FString(TEXT("\"a\\\"b\\n\"")));
	TestTrue(TEXT("truthy yes"), ScenarioJson::Truthy(MakeShared<FJsonValueString>(TEXT("yes"))));
	TestFalse(TEXT("falsy 0"), ScenarioJson::Truthy(MakeShared<FJsonValueNumber>(0.0)));
	TestTrue(TEXT("empty string is empty"), ScenarioJson::IsEmptyValue(MakeShared<FJsonValueString>(TEXT("  "))));
	TestFalse(TEXT("zero is a value"), ScenarioJson::IsEmptyValue(MakeShared<FJsonValueNumber>(0.0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioCUFormatTest, "Scenario.Core.CU.Format", SCENARIO_TEST_FLAGS)
bool FScenarioCUFormatTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("3"), ScenarioCU::Format(3.0), FString(TEXT("3 CU")));
	TestEqual(TEXT("2.75"), ScenarioCU::Format(2.75), FString(TEXT("2.75 CU")));
	TestEqual(TEXT("1,240"), ScenarioCU::Format(1240.0), FString(TEXT("1,240 CU")));
	TestEqual(TEXT("12.3K"), ScenarioCU::Format(12345.0), FString(TEXT("12.3K CU")));
	TestEqual(TEXT("no unit"), ScenarioCU::Format(11.0, false), FString(TEXT("11")));
	TestEqual(TEXT("three decimals max"), ScenarioCU::Format(0.12345), FString(TEXT("0.123 CU")));
	TestEqual(TEXT("exact 11"), ScenarioCU::Exact(11.0), FString(TEXT("11")));
	TestEqual(TEXT("exact 2.75"), ScenarioCU::Exact(2.75), FString(TEXT("2.75")));
	double Parsed = 0.0;
	TestTrue(TEXT("parse"), ScenarioCU::Parse(TEXT("2.75"), Parsed) && Parsed == 2.75);
	TestFalse(TEXT("parse rejects text"), ScenarioCU::Parse(TEXT("about 3"), Parsed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioHashTest, "Scenario.Core.Hash", SCENARIO_TEST_FLAGS)
bool FScenarioHashTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("sha256 empty"), ScenarioHash::Sha256Hex(TEXT("")), FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	TestEqual(TEXT("sha256 abc"), ScenarioHash::Sha256Hex(TEXT("abc")), FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	TestEqual(TEXT("sha256 two blocks"), ScenarioHash::Sha256Hex(TEXT("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
		FString(TEXT("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")));
	TArray<uint8> Bytes = { 0xfb, 0xff, 0x00 };
	TestEqual(TEXT("base64url without padding"), ScenarioHash::Base64Url(Bytes), FString(TEXT("-_8A")));
	const FString TokenA = ScenarioHash::RandomToken(32);
	const FString TokenB = ScenarioHash::RandomToken(32);
	TestEqual(TEXT("token length"), TokenA.Len(), 43);
	TestNotEqual(TEXT("tokens differ"), TokenA, TokenB);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioFilesNamingTest, "Scenario.Core.Files.Naming", SCENARIO_TEST_FLAGS)
bool FScenarioFilesNamingTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("pascal slug"), ScenarioFiles::PascalSlug(TEXT("a weathered pirate chest")), FString(TEXT("WeatheredPirateChest")));
	TestEqual(TEXT("five words max"), ScenarioFiles::PascalSlug(TEXT("one two three four five six seven")), FString(TEXT("OneTwoThreeFourFive")));
	TestEqual(TEXT("ascii only"), ScenarioFiles::PascalSlug(TEXT("caf\u00e9 cr\u00e8me br\u00fbl\u00e9e")).Len() > 0, true);
	TestEqual(TEXT("tail"), ScenarioFiles::Tail(TEXT("asset_abc123def456")), FString(TEXT("def456")));
	const FString Name = ScenarioFiles::AssetName(TEXT("T"), TEXT("a weathered pirate chest"), TEXT("asset_abc123def456"), TEXT("albedo"));
	TestEqual(TEXT("asset name"), Name, FString(TEXT("T_WeatheredPirateChest_def456_BC")));
	const FString Long = ScenarioFiles::AssetName(TEXT("SM"), TEXT("supercalifragilisticexpialidocious antidisestablishmentarianism floccinaucinihilipilification pneumonoultramicroscopic"), TEXT("asset_zzzzzz"));
	TestTrue(TEXT("names stay under 64 characters"), Long.Len() <= 63);
	TestEqual(TEXT("smoothness is roughness"), ScenarioFiles::RoleFor(TEXT("texture-smoothness")), FString(TEXT("roughness")));
	TestEqual(TEXT("unknown role is main"), ScenarioFiles::RoleFor(TEXT("inference-txt2img")), FString(TEXT("main")));
	TestEqual(TEXT("normal suffix"), ScenarioFiles::RoleSuffix(TEXT("normal")), FString(TEXT("N")));
	TestEqual(TEXT("safe object name"), ScenarioFiles::SafeObjectName(TEXT("T_A-b c")), FString(TEXT("T_A_b_c")));

	TArray<uint8> Png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
	TestEqual(TEXT("png"), ScenarioFiles::SniffExtension(Png), FString(TEXT("png")));
	TArray<uint8> Glb = { 'g', 'l', 'T', 'F', 2, 0, 0, 0 };
	TestEqual(TEXT("glb"), ScenarioFiles::SniffExtension(Glb), FString(TEXT("glb")));
	TArray<uint8> Wav = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E' };
	TestEqual(TEXT("wav"), ScenarioFiles::SniffExtension(Wav), FString(TEXT("wav")));
	TestEqual(TEXT("upload kind"), ScenarioFiles::UploadKindForExtension(TEXT("glb")), FString(TEXT("3d")));
	TestEqual(TEXT("mime"), ScenarioFiles::MimeForExtension(TEXT("png")), FString(TEXT("image/png")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioUrlTest, "Scenario.Core.Url", SCENARIO_TEST_FLAGS)
bool FScenarioUrlTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("host"), ScenarioUrl::Host(TEXT("https://user:pw@CDN.Cloud.Scenario.com:443/a/b?x=1")), FString(TEXT("cdn.cloud.scenario.com")));
	TestTrue(TEXT("https"), ScenarioUrl::IsHttps(TEXT("https://mcp.scenario.com/mcp")));
	TestFalse(TEXT("http"), ScenarioUrl::IsHttps(TEXT("http://mcp.scenario.com/mcp")));
	TestTrue(TEXT("loopback"), ScenarioUrl::IsLoopbackHost(TEXT("127.0.0.1")));
	TestFalse(TEXT("not loopback"), ScenarioUrl::IsLoopbackHost(TEXT("127.0.0.1.evil.com")));
	TestEqual(TEXT("encode"), ScenarioUrl::Encode(TEXT("a b&c=d/\u00e9")), FString(TEXT("a%20b%26c%3Dd%2F%C3%A9")));
	const TMap<FString, FString> Query = ScenarioUrl::ParseQuery(TEXT("code=abc%2Fd&state=x+y"));
	TestEqual(TEXT("decoded code"), Query.FindRef(TEXT("code")), FString(TEXT("abc/d")));
	TestEqual(TEXT("plus as space"), Query.FindRef(TEXT("state")), FString(TEXT("x y")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioErrorsTest, "Scenario.Core.Errors", SCENARIO_TEST_FLAGS)
bool FScenarioErrorsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("401"), FScenarioErrors::Classify(TEXT("nope"), 401), EScenarioErrorKind::Auth);
	TestEqual(TEXT("403"), FScenarioErrors::Classify(TEXT("nope"), 403), EScenarioErrorKind::Forbidden);
	TestEqual(TEXT("429"), FScenarioErrors::Classify(TEXT("slow down"), 429), EScenarioErrorKind::RateLimit);
	TestEqual(TEXT("503"), FScenarioErrors::Classify(TEXT("upstream"), 503), EScenarioErrorKind::Http5xx);
	TestEqual(TEXT("422"), FScenarioErrors::Classify(TEXT("bad field"), 422), EScenarioErrorKind::Invalid);
	TestEqual(TEXT("moderation wins over status"), FScenarioErrors::Classify(TEXT("Content flagged by the safety system"), 400), EScenarioErrorKind::Moderated);
	TestEqual(TEXT("quota text in a 200"), FScenarioErrors::Classify(TEXT("Insufficient creative units"), 0), EScenarioErrorKind::Quota);
	TestTrue(TEXT("timeout is ambiguous"), FScenarioErrors::IsAmbiguousKind(EScenarioErrorKind::Timeout));
	TestFalse(TEXT("refusal is not ambiguous"), FScenarioErrors::IsAmbiguousKind(EScenarioErrorKind::Invalid));
	TestFalse(TEXT("moderation is never retried"), FScenarioErrors::IsRetryableKind(EScenarioErrorKind::Moderated));

	const FString Scrubbed = FScenarioErrors::Scrub(TEXT("GET https://cdn.cloud.scenario.com/a.png?X-Amz-Signature=abc&Expires=1 failed, Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload.sig"));
	TestFalse(TEXT("signature removed"), Scrubbed.Contains(TEXT("X-Amz-Signature")));
	TestFalse(TEXT("token removed"), Scrubbed.Contains(TEXT("eyJhbGciOiJIUzI1NiJ9")));
	TestTrue(TEXT("path kept"), Scrubbed.Contains(TEXT("https://cdn.cloud.scenario.com/a.png?...")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioRpcParseTest, "Scenario.Core.Rpc.Parse", SCENARIO_TEST_FLAGS)
bool FScenarioRpcParseTest::RunTest(const FString& Parameters)
{
	const FScenarioReply Json = ScenarioRpc::ParseReply(TEXT("{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"ok\":1}}"), TEXT("application/json"), 3);
	TestTrue(TEXT("json ok"), Json.bOk && Json.Value->HasField(TEXT("result")));

	const FString Stream =
		TEXT("event: message\n")
		TEXT("data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":{}}\n\n")
		TEXT("data: not json\n\n")
		TEXT("data: {\"jsonrpc\":\"2.0\",\"id\":7,\n")
		TEXT("data: \"result\":{\"value\":42}}\n\n")
		TEXT("data: [DONE]\n\n");
	const FScenarioReply Sse = ScenarioRpc::ParseReply(Stream, TEXT("text/event-stream"), 7);
	TestTrue(TEXT("sse ok"), Sse.bOk);
	TestEqual(TEXT("multi-line data joined"), ScenarioJson::GetNumber(ScenarioJson::GetObject(Sse.Value, TEXT("result")), TEXT("value")), 42.0);
	TestEqual(TEXT("bad events skipped"), ScenarioRpc::StreamMessages(Stream).Num(), 2);

	const FScenarioReply Wrong = ScenarioRpc::ParseReply(TEXT("{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{}}"), TEXT("application/json"), 9);
	TestFalse(TEXT("wrong id is an error"), Wrong.bOk);
	const FScenarioReply Notification = ScenarioRpc::ParseReply(FString(), FString(), -1);
	TestTrue(TEXT("empty notification reply"), Notification.bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioTelemetryTest, "Scenario.Core.Telemetry", SCENARIO_TEST_FLAGS)
bool FScenarioTelemetryTest::RunTest(const FString& Parameters)
{
	const TMap<FString, FString> Fields = { { TEXT("lane"), TEXT("image") }, { TEXT("state"), TEXT("success") }, { TEXT("prompt"), TEXT("secret castle") }, { TEXT("model_id"), TEXT("model_x") } };
	const TSharedRef<FJsonObject> Body = ScenarioTelemetry::BuildEvent(TEXT("job_finished"), Fields);
	TestEqual(TEXT("allow-listed field kept"), ScenarioJson::GetString(Body, TEXT("lane")), FString(TEXT("image")));
	TestFalse(TEXT("prompt dropped"), Body->HasField(TEXT("prompt")));
	TestFalse(TEXT("ids dropped"), Body->HasField(TEXT("model_id")));
	TestTrue(TEXT("versions added"), Body->HasField(TEXT("plugin")) && Body->HasField(TEXT("engine")));
	TestTrue(TEXT("no endpoint in this version"), ScenarioTelemetry::Endpoint().IsEmpty());
	return true;
}

#endif
