// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ScenarioApi.h"
#include "ScenarioFakes.h"
#include "ScenarioJson.h"
#include "ScenarioMcpClient.h"

namespace ScenarioMcpTests
{
	/** Auth provider whose token changes on refresh. */
	class FTestAuth : public IScenarioAuthProvider
	{
	public:
		explicit FTestAuth(bool bInWorkspace, bool bInRefreshWorks = true)
			: bWorkspace(bInWorkspace)
			, bRefreshWorks(bInRefreshWorks)
		{
		}
		virtual bool HasCredentials() const override { return true; }
		virtual FString AuthorizationHeader() const override { return TEXT("Bearer ") + Token; }
		virtual bool NeedsWorkspaceArguments() const override { return bWorkspace; }
		virtual void Refresh(TFunction<void(bool)> OnDone) override
		{
			++Refreshes;
			if (bRefreshWorks)
			{
				Token = TEXT("fresh-token");
			}
			OnDone(bRefreshWorks);
		}
		bool bWorkspace = false;
		bool bRefreshWorks = true;
		int32 Refreshes = 0;
		FString Token = TEXT("old-token");
	};

	struct FRig
	{
		TSharedRef<FScenarioFakeTransport> Transport = MakeShared<FScenarioFakeTransport>();
		TSharedRef<FScenarioFakeMcpServer> Server = MakeShared<FScenarioFakeMcpServer>();
		TSharedRef<FScenarioMcpClient> Client = MakeShared<FScenarioMcpClient>(Transport, TEXT("https://mcp.example.test/mcp"));
		TSharedRef<FTestAuth> Auth;

		explicit FRig(bool bWorkspace = false)
			: Auth(MakeShared<FTestAuth>(bWorkspace))
		{
			Client->SetAuth(Auth);
			Client->bSpendingDisabled = false;
			Server->Attach(*Transport);
		}

		FScenarioReply Call(const FString& Tool, const FString& ArgumentsJson = TEXT("{}"))
		{
			FScenarioReply Out = FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("no callback"));
			bool bCalled = false;
			Client->CallTool(Tool, ScenarioJson::ParseObject(ArgumentsJson), [&Out, &bCalled](const FScenarioReply& Reply)
			{
				Out = Reply;
				bCalled = true;
			});
			check(bCalled);
			return Out;
		}

		int32 Count(const FString& Method) const
		{
			int32 N = 0;
			for (const FScenarioFakeRequest& Request : Transport->Requests)
			{
				N += Request.RpcMethod() == Method ? 1 : 0;
			}
			return N;
		}
	};

	FScenarioHttpResponse Status(int32 Code, const FString& Body = TEXT("{}"), const TMap<FString, FString>& Headers = {})
	{
		return FScenarioFakeTransport::Json(Code, Body, Headers);
	}
}

using namespace ScenarioMcpTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioMcpHandshakeTest, "Scenario.Core.Mcp.Handshake", SCENARIO_TEST_FLAGS)
bool FScenarioMcpHandshakeTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Server->On(TEXT("models_list"), [](const TSharedPtr<FJsonObject>&) { return FString(TEXT("{\"models\":[]}")); });
	TestTrue(TEXT("first call"), Rig.Call(TEXT("models_list")).bOk);
	TestTrue(TEXT("second call"), Rig.Call(TEXT("models_list")).bOk);
	TestEqual(TEXT("one initialize"), Rig.Count(TEXT("initialize")), 1);
	TestEqual(TEXT("one initialized notification"), Rig.Count(TEXT("notifications/initialized")), 1);
	TestEqual(TEXT("two tool calls"), Rig.Count(TEXT("tools/call")), 2);
	const FScenarioFakeRequest& Last = Rig.Transport->Requests.Last();
	TestEqual(TEXT("session header"), Last.Headers.FindRef(TEXT("Mcp-Session-Id")), FString(TEXT("fake-session")));
	TestEqual(TEXT("protocol header"), Last.Headers.FindRef(TEXT("MCP-Protocol-Version")), FString(TEXT("2025-03-26")));
	TestTrue(TEXT("accepts SSE"), Last.Headers.FindRef(TEXT("Accept")).Contains(TEXT("text/event-stream")));
	TestTrue(TEXT("user agent names the plugin"), Last.Headers.FindRef(TEXT("User-Agent")).StartsWith(TEXT("ScenarioUnreal/")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioMcpSessionExpiryTest, "Scenario.Core.Mcp.SessionExpiry", SCENARIO_TEST_FLAGS)
bool FScenarioMcpSessionExpiryTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	int32 Calls = 0;
	Rig.Server->OnRaw(TEXT("model_run"), [&Calls](const TSharedPtr<FJsonObject>&, int64 Id)
	{
		// The first paid call hits an expired session: refused before dispatch, so it is safe to send again.
		if (++Calls == 1)
		{
			return Status(404, TEXT("{\"error\":\"No valid session ID provided\"}"));
		}
		return FScenarioFakeTransport::Json(200, FScenarioFakeTransport::ToolResult(Id, TEXT("{\"job\":{\"jobId\":\"job_1\",\"status\":\"queued\"}}")));
	});
	const FScenarioReply Reply = Rig.Call(TEXT("model_run"), TEXT("{\"model_id\":\"model_x\",\"parameters\":{}}"));
	TestTrue(TEXT("resent after renewal"), Reply.bOk);
	TestEqual(TEXT("two initializes"), Rig.Count(TEXT("initialize")), 2);
	TestEqual(TEXT("the paid call reached the server twice, once refused"), Calls, 2);
	const TArray<FScenarioFakeRequest> ToolCalls = Rig.Transport->ToolCalls();
	if (!TestEqual(TEXT("two tool calls"), ToolCalls.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("same JSON-RPC id on resend"), ScenarioJson::GetNumber(ToolCalls[0].BodyJson(), TEXT("id")), ScenarioJson::GetNumber(ToolCalls[1].BodyJson(), TEXT("id")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioMcpPaidNeverRetriedTest, "Scenario.Core.Mcp.PaidNeverRetried", SCENARIO_TEST_FLAGS)
bool FScenarioMcpPaidNeverRetriedTest::RunTest(const FString& Parameters)
{
	{
		FRig Rig;
		Rig.Server->OnRaw(TEXT("model_run"), [](const TSharedPtr<FJsonObject>&, int64) { return Status(503, TEXT("upstream error")); });
		const FScenarioReply Reply = Rig.Call(TEXT("model_run"), TEXT("{\"model_id\":\"model_x\"}"));
		TestFalse(TEXT("503 fails"), Reply.bOk);
		TestTrue(TEXT("503 on a paid call is ambiguous"), Reply.Error.bAmbiguous);
		TestEqual(TEXT("sent once"), Rig.Server->CallCounts.FindRef(TEXT("model_run")), 1);
		TestEqual(TEXT("no back-off"), Rig.Transport->Delays.Num(), 0);
	}
	{
		FRig Rig;
		Rig.Server->OnRaw(TEXT("model_run"), [](const TSharedPtr<FJsonObject>&, int64)
		{
			FScenarioHttpResponse Lost;
			Lost.TransportError = EScenarioErrorKind::Timeout;
			return Lost;
		});
		const FScenarioReply Reply = Rig.Call(TEXT("model_run"), TEXT("{\"model_id\":\"model_x\"}"));
		TestTrue(TEXT("timeout on a paid call is ambiguous"), Reply.Error.bAmbiguous);
		TestEqual(TEXT("timeout kind"), Reply.Error.Kind, EScenarioErrorKind::Timeout);
		TestEqual(TEXT("timeout sent once"), Rig.Server->CallCounts.FindRef(TEXT("model_run")), 1);
	}
	{
		FRig Rig;
		Rig.Server->OnRaw(TEXT("model_run"), [](const TSharedPtr<FJsonObject>&, int64) { return Status(429, TEXT("Too many requests"), { { TEXT("Retry-After"), TEXT("3") } }); });
		const FScenarioReply Reply = Rig.Call(TEXT("model_run"), TEXT("{\"model_id\":\"model_x\"}"));
		TestEqual(TEXT("429 kind"), Reply.Error.Kind, EScenarioErrorKind::RateLimit);
		TestTrue(TEXT("429 was not sent"), Reply.Error.bNotSent);
		TestFalse(TEXT("429 is not ambiguous"), Reply.Error.bAmbiguous);
		TestEqual(TEXT("retry-after read"), Reply.Error.RetryAfterSeconds, 3.0);
		TestEqual(TEXT("429 paid call not retried"), Rig.Server->CallCounts.FindRef(TEXT("model_run")), 1);
	}
	{
		FRig Rig;
		Rig.Server->OnRaw(TEXT("brand_new_tool"), [](const TSharedPtr<FJsonObject>&, int64) { return Status(502, TEXT("bad gateway")); });
		const FScenarioReply Reply = Rig.Call(TEXT("brand_new_tool"));
		TestTrue(TEXT("unknown tools are treated as paid"), Reply.Error.bAmbiguous);
		TestEqual(TEXT("unknown tool sent once"), Rig.Server->CallCounts.FindRef(TEXT("brand_new_tool")), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioMcpReadRetryTest, "Scenario.Core.Mcp.ReadRetry", SCENARIO_TEST_FLAGS)
bool FScenarioMcpReadRetryTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	int32 Calls = 0;
	Rig.Server->OnRaw(TEXT("job_get"), [&Calls](const TSharedPtr<FJsonObject>&, int64 Id)
	{
		if (++Calls < 3)
		{
			return Status(503, TEXT("busy"));
		}
		return FScenarioFakeTransport::Json(200, FScenarioFakeTransport::ToolResult(Id, TEXT("{\"job\":{\"jobId\":\"job_1\",\"status\":\"success\"}}")));
	});
	TestTrue(TEXT("read succeeds on the third try"), Rig.Call(TEXT("job_get"), TEXT("{\"job_id\":\"job_1\"}")).bOk);
	TestEqual(TEXT("three attempts"), Calls, 3);
	TestEqual(TEXT("two back-offs"), Rig.Transport->Delays.Num(), 2);

	FRig Long;
	Long.Server->OnRaw(TEXT("job_get"), [](const TSharedPtr<FJsonObject>&, int64) { return Status(429, TEXT("slow"), { { TEXT("Retry-After"), TEXT("120") } }); });
	TestFalse(TEXT("long Retry-After stops"), Long.Call(TEXT("job_get"), TEXT("{\"job_id\":\"job_1\"}")).bOk);
	TestEqual(TEXT("no retry past the cap"), Long.Server->CallCounts.FindRef(TEXT("job_get")), 1);

	FRig DryRun;
	int32 Prices = 0;
	DryRun.Server->OnRaw(TEXT("model_run"), [&Prices](const TSharedPtr<FJsonObject>&, int64 Id)
	{
		if (++Prices == 1)
		{
			return Status(500, TEXT("oops"));
		}
		return FScenarioFakeTransport::Json(200, FScenarioFakeTransport::ToolResult(Id, TEXT("{\"creativeUnitsCost\":11}")));
	});
	TestTrue(TEXT("a price check is a read and is retried"), DryRun.Call(TEXT("model_run"), TEXT("{\"model_id\":\"model_x\",\"dry_run\":true}")).bOk);
	TestEqual(TEXT("two price attempts"), Prices, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioMcpEstimateAlarmTest, "Scenario.Core.Mcp.EstimateAlarm", SCENARIO_TEST_FLAGS)
bool FScenarioMcpEstimateAlarmTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Server->On(TEXT("model_run"), [](const TSharedPtr<FJsonObject>&) { return FString(TEXT("{\"job\":{\"jobId\":\"job_surprise\"}}")); });
	const FScenarioReply Reply = Rig.Call(TEXT("model_run"), TEXT("{\"model_id\":\"model_x\",\"dry_run\":true}"));
	TestFalse(TEXT("price check with a job fails"), Reply.bOk);
	TestEqual(TEXT("alarm kind"), Reply.Error.Kind, EScenarioErrorKind::EstimateJob);
	TestEqual(TEXT("job id surfaced"), Reply.Error.JobId, FString(TEXT("job_surprise")));
	TestTrue(TEXT("alarm raised for the session"), Rig.Client->IsEstimateAlarmRaised());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioMcpRefreshTest, "Scenario.Core.Mcp.Refresh", SCENARIO_TEST_FLAGS)
bool FScenarioMcpRefreshTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Server->On(TEXT("model_run"), [](const TSharedPtr<FJsonObject>&) { return FString(TEXT("{\"job\":{\"jobId\":\"job_2\",\"status\":\"queued\"}}")); });
	const TSharedRef<FScenarioFakeMcpServer> Server = Rig.Server;
	Rig.Transport->Responder = [Server](const FScenarioFakeRequest& Request)
	{
		if (Request.Headers.FindRef(TEXT("Authorization")) != TEXT("Bearer fresh-token"))
		{
			return Status(401, TEXT("{\"error\":\"invalid_token\"}"));
		}
		return Server->Handle(Request);
	};
	TestTrue(TEXT("refreshed then sent"), Rig.Call(TEXT("model_run"), TEXT("{\"model_id\":\"model_x\"}")).bOk);
	TestEqual(TEXT("one refresh"), Rig.Auth->Refreshes, 1);
	TestEqual(TEXT("paid call ran once"), Server->CallCounts.FindRef(TEXT("model_run")), 1);

	FRig Dead;
	Dead.Auth->bRefreshWorks = false;
	Dead.Transport->Responder = [](const FScenarioFakeRequest&) { return Status(401, TEXT("{\"error\":\"invalid_token\"}")); };
	const FScenarioReply Reply = Dead.Call(TEXT("model_run"), TEXT("{\"model_id\":\"model_x\"}"));
	TestEqual(TEXT("auth error"), Reply.Error.Kind, EScenarioErrorKind::Auth);
	TestTrue(TEXT("refused request was not sent"), Reply.Error.bNotSent);
	TestFalse(TEXT("refused request is not ambiguous"), Reply.Error.bAmbiguous);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioMcpNoSpendTest, "Scenario.Core.Mcp.NoSpend", SCENARIO_TEST_FLAGS)
bool FScenarioMcpNoSpendTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Client->bSpendingDisabled = true;
	Rig.Server->On(TEXT("job_cancel"), [](const TSharedPtr<FJsonObject>&) { return FString(TEXT("{\"ok\":true}")); });
	Rig.Server->On(TEXT("model_run"), [](const TSharedPtr<FJsonObject>&) { return FString(TEXT("{\"creativeUnitsCost\":3}")); });
	const FScenarioReply Paid = Rig.Call(TEXT("model_run"), TEXT("{\"model_id\":\"model_x\"}"));
	TestTrue(TEXT("paid call refused locally"), !Paid.bOk && Paid.Error.bNotSent);
	TestEqual(TEXT("nothing sent for the paid call"), Rig.Server->CallCounts.FindRef(TEXT("model_run")), 0);
	TestTrue(TEXT("price checks still work"), Rig.Call(TEXT("model_run"), TEXT("{\"model_id\":\"model_x\",\"dry_run\":true}")).bOk);
	TestTrue(TEXT("cancel still works"), Rig.Call(TEXT("job_cancel"), TEXT("{\"job_id\":\"job_1\"}")).bOk);
	TestFalse(TEXT("uploads refused"), Rig.Call(TEXT("upload_asset"), TEXT("{}")).bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioMcpWorkspaceArgsTest, "Scenario.Core.Mcp.WorkspaceArguments", SCENARIO_TEST_FLAGS)
bool FScenarioMcpWorkspaceArgsTest::RunTest(const FString& Parameters)
{
	FRig Rig(true);
	Rig.Client->SetWorkspace(TEXT("team_a"), TEXT("proj_b"));
	for (const TCHAR* Tool : { TEXT("teams_list"), TEXT("projects_list"), TEXT("models_list") })
	{
		Rig.Server->On(Tool, [](const TSharedPtr<FJsonObject>&) { return FString(TEXT("{\"items\":[]}")); });
		Rig.Call(Tool);
	}
	const TArray<FScenarioFakeRequest> Calls = Rig.Transport->ToolCalls();
	if (!TestEqual(TEXT("three tool calls"), Calls.Num(), 3))
	{
		return false;
	}
	TestFalse(TEXT("teams_list has no team"), Calls[0].ToolArguments()->HasField(TEXT("team_id")));
	TestEqual(TEXT("projects_list has the team"), ScenarioJson::GetString(Calls[1].ToolArguments(), TEXT("team_id")), FString(TEXT("team_a")));
	TestFalse(TEXT("projects_list has no project"), Calls[1].ToolArguments()->HasField(TEXT("project_id")));
	TestEqual(TEXT("models_list has the project"), ScenarioJson::GetString(Calls[2].ToolArguments(), TEXT("project_id")), FString(TEXT("proj_b")));

	FRig Key(false);
	Key.Client->SetWorkspace(TEXT("team_a"), TEXT("proj_b"));
	Key.Server->On(TEXT("models_list"), [](const TSharedPtr<FJsonObject>&) { return FString(TEXT("{\"models\":[]}")); });
	Key.Call(TEXT("models_list"));
	const TArray<FScenarioFakeRequest> KeyCalls = Key.Transport->ToolCalls();
	TestTrue(TEXT("API keys send no workspace"), KeyCalls.Num() == 1 && !KeyCalls[0].ToolArguments()->HasField(TEXT("project_id")));

	// A catalog read carries the workspace inside the parameters of the tool it runs.
	FRig Catalog(true);
	Catalog.Client->SetWorkspace(TEXT("team_a"), TEXT("proj_b"));
	Catalog.Server->On(TEXT("scenario_tool_execute_read"), [](const TSharedPtr<FJsonObject>&) { return FString(TEXT("{\"assets\":[]}")); });
	Catalog.Call(TEXT("scenario_tool_execute_read"), TEXT("{\"name\":\"assets_list\",\"parameters\":{\"limit\":5}}"));
	const TArray<FScenarioFakeRequest> CatalogCalls = Catalog.Transport->ToolCalls();
	if (TestEqual(TEXT("one catalog read"), CatalogCalls.Num(), 1))
	{
		const TSharedPtr<FJsonObject> Inner = ScenarioJson::GetObject(CatalogCalls[0].ToolArguments(), TEXT("parameters"));
		TestFalse(TEXT("no workspace at the top"), CatalogCalls[0].ToolArguments()->HasField(TEXT("project_id")));
		TestEqual(TEXT("team in parameters"), ScenarioJson::GetString(Inner, TEXT("team_id")), FString(TEXT("team_a")));
		TestEqual(TEXT("project in parameters"), ScenarioJson::GetString(Inner, TEXT("project_id")), FString(TEXT("proj_b")));
		TestEqual(TEXT("parameters kept"), ScenarioJson::GetNumber(Inner, TEXT("limit")), 5.0);
	}
	TestTrue(TEXT("a catalog read is retried"), FScenarioMcpClient::IsRetryable(TEXT("scenario_tool_execute_read"), MakeShared<FJsonObject>()));
	TestFalse(TEXT("a catalog read never spends"), FScenarioMcpClient::IsSubmission(TEXT("scenario_tool_execute_read"), MakeShared<FJsonObject>()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioMcpToolErrorsTest, "Scenario.Core.Mcp.ToolErrors", SCENARIO_TEST_FLAGS)
bool FScenarioMcpToolErrorsTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Server->OnRaw(TEXT("model_run"), [](const TSharedPtr<FJsonObject>&, int64 Id)
	{
		const FString Body = FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"Prompt flagged by content moderation\"}],\"isError\":true}}"), Id);
		return FScenarioFakeTransport::Json(200, Body);
	});
	const FScenarioReply Moderated = Rig.Call(TEXT("model_run"), TEXT("{\"model_id\":\"model_x\"}"));
	TestEqual(TEXT("moderation"), Moderated.Error.Kind, EScenarioErrorKind::Moderated);
	TestFalse(TEXT("a clear refusal is not ambiguous"), Moderated.Error.bAmbiguous);

	// usage answers with JSON in a text block only (checked 2026-10-18).
	Rig.Server->OnRaw(TEXT("usage"), [](const TSharedPtr<FJsonObject>&, int64 Id)
	{
		const FString Body = FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"{\\\"total\\\":12.5}\"}]}}"), Id);
		return FScenarioFakeTransport::Json(200, Body);
	});
	const FScenarioReply Usage = Rig.Call(TEXT("usage"), TEXT("{}"));
	TestTrue(TEXT("text-only JSON parsed"), Usage.bOk && ScenarioJson::GetNumber(Usage.Value, TEXT("total")) == 12.5);

	// An SSE reply with a progress notification before the result.
	Rig.Server->OnRaw(TEXT("job_get"), [](const TSharedPtr<FJsonObject>&, int64 Id)
	{
		FScenarioHttpResponse Response = FScenarioFakeTransport::Json(200,
			FString(TEXT("data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n\n")) +
			TEXT("data: ") + FScenarioFakeTransport::ToolResult(Id, TEXT("{\"job\":{\"jobId\":\"job_3\",\"status\":\"in-progress\"}}")) + TEXT("\n\n"));
		Response.Headers.Add(TEXT("content-type"), TEXT("text/event-stream"));
		return Response;
	});
	TestTrue(TEXT("sse reply"), Rig.Call(TEXT("job_get"), TEXT("{\"job_id\":\"job_3\"}")).bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioApiEstimateTest, "Scenario.Core.Api.Estimate", SCENARIO_TEST_FLAGS)
bool FScenarioApiEstimateTest::RunTest(const FString& Parameters)
{
	FScenarioEstimate Estimate;
	TestTrue(TEXT("top level"), FScenarioApi::FindEstimate(ScenarioJson::ParseObject(TEXT("{\"creativeUnitsCost\":11}")), Estimate) && Estimate.CU == 11.0);
	TestEqual(TEXT("exact text"), Estimate.Exact, FString(TEXT("11")));
	TestTrue(TEXT("nested"), FScenarioApi::FindEstimate(ScenarioJson::ParseObject(TEXT("{\"estimate\":{\"cuCost\":2.75,\"creativeUnitsDiscount\":0.25}}")), Estimate) && Estimate.CU == 2.75);
	TestFalse(TEXT("no price"), FScenarioApi::FindEstimate(ScenarioJson::ParseObject(TEXT("{\"job\":{}}")), Estimate));

	TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
	Backend->OnJson(TEXT("model_run"), TEXT("{\"creativeUnitsCost\":12}"));
	FScenarioApi Api(Backend);
	bool bPriced = false;
	Api.Estimate(TEXT("model_x"), ScenarioJson::ParseObject(TEXT("{\"prompt\":\"a chest\"}")), [&bPriced](const TScenarioResult<FScenarioEstimate>& Result)
	{
		bPriced = Result.bOk && Result.Value.CU == 12.0;
	});
	TestTrue(TEXT("api estimate"), bPriced);
	TestTrue(TEXT("estimate sends dry_run"), ScenarioJson::GetBool(Backend->Calls.Last().Value, TEXT("dry_run")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioApiListAssetsTest, "Scenario.Core.Api.ListAssets", SCENARIO_TEST_FLAGS)
bool FScenarioApiListAssetsTest::RunTest(const FString& Parameters)
{
	TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
	Backend->OnJson(TEXT("scenario_tool_execute_read"), TEXT("{\"assets\":[")
		TEXT("{\"id\":\"asset_mesh\",\"kind\":\"3d\",\"modelId\":\"model_x\",\"prompt\":\"asset_text\",\"url\":\"https://cdn.example.test/m\"},")
		TEXT("{\"id\":\"asset_image\",\"kind\":\"image\",\"prompt\":\"a mossy arch\"}")
		TEXT("],\"nextPaginationToken\":\"next_1\"}"));
	FScenarioApi Api(Backend);
	FScenarioAssetPage Page;
	bool bListed = false;
	Api.ListAssets(TEXT("3d"), 20, TEXT("page_0"), [&Page, &bListed](const TScenarioResult<FScenarioAssetPage>& Result)
	{
		bListed = Result.bOk;
		Page = Result.Value;
	});
	if (!TestTrue(TEXT("listed"), bListed) || !TestEqual(TEXT("two assets"), Page.Assets.Num(), 2))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Sent = Backend->Calls.Last().Value;
	const TSharedPtr<FJsonObject> Inner = ScenarioJson::GetObject(Sent, TEXT("parameters"));
	TestEqual(TEXT("runs assets_list"), ScenarioJson::GetString(Sent, TEXT("name")), FString(TEXT("assets_list")));
	TestEqual(TEXT("passes the page token"), ScenarioJson::GetString(Inner, TEXT("page_token")), FString(TEXT("page_0")));
	TestEqual(TEXT("filters 3D types"), ScenarioJson::GetArray(ScenarioJson::GetObject(Inner, TEXT("filters")), TEXT("types")).Num(), 4);
	TestEqual(TEXT("next token"), Page.NextPageToken, FString(TEXT("next_1")));
	TestEqual(TEXT("flattened model id"), Page.Assets[0].ModelId, FString(TEXT("model_x")));
	TestTrue(TEXT("a prompt kept as an asset id is no title"), Page.Assets[0].Prompt.IsEmpty());
	TestEqual(TEXT("flattened prompt"), Page.Assets[1].Prompt, FString(TEXT("a mossy arch")));
	return true;
}

#endif
