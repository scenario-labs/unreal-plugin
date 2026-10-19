// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ScenarioActions.h"
#include "ScenarioAgentClients.h"
#include "ScenarioFakes.h"
#include "ScenarioJson.h"
#include "ScenarioMcpServer.h"
#include "ScenarioSettings.h"

namespace ScenarioAgentTests
{
	const TCHAR* TestToken = TEXT("test-token-0123456789abcdef0123456789abcdef");

	FScenarioLocalRequest Make(const FString& Method, const FString& Path, const FString& Body, bool bToken = true)
	{
		FScenarioLocalRequest Request;
		Request.Method = Method;
		Request.Path = Path;
		Request.Headers.Add(TEXT("host"), TEXT("127.0.0.1:9877"));
		Request.HostCount = 1;
		if (bToken)
		{
			Request.Headers.Add(TEXT("authorization"), FString(TEXT("Bearer ")) + TestToken);
		}
		const FTCHARToUTF8 Utf8(*Body);
		Request.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Request;
	}

	/** Handles one request; the tested paths all answer synchronously. */
	FScenarioLocalResponse Send(FScenarioMcpServer& Server, const FScenarioLocalRequest& Request)
	{
		FScenarioLocalResponse Out;
		Out.Status = -1;
		Server.Handle(Request, [&Out](const FScenarioLocalResponse& Response) { Out = Response; });
		return Out;
	}

	TSharedPtr<FJsonObject> Rpc(const FScenarioLocalResponse& Response)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Response.Body.GetData()), Response.Body.Num());
		return ScenarioJson::ParseObject(FString(Converted.Length(), Converted.Get()));
	}

	TSharedRef<FScenarioMcpServer> NewServer()
	{
		TSharedRef<FScenarioMcpServer> Server = MakeShared<FScenarioMcpServer>();
		Server->Token = TestToken;
		Server->bRequireToken = true;
		Server->SetPortForTests(9877);
		return Server;
	}
}

using namespace ScenarioAgentTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioAgentToolBudgetTest, "Scenario.Agent.Tools.Budget", SCENARIO_TEST_FLAGS)
bool FScenarioAgentToolBudgetTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FJsonObject> List = FScenarioMcpServer::ToolsList();
	const FString Text = ScenarioJson::Write(List);
	TestTrue(FString::Printf(TEXT("tools/list under 40,000 characters (%d)"), Text.Len()), Text.Len() < 40000);
	const TArray<TSharedPtr<FJsonValue>> Tools = ScenarioJson::GetArray(List, TEXT("tools"));
	TestEqual(TEXT("39 built-in tools and 9 kit tools"), Tools.Num(), 48);
	TSet<FString> Names;
	for (const TSharedPtr<FJsonValue>& Value : Tools)
	{
		const TSharedPtr<FJsonObject> Tool = Value->AsObject();
		const FString Name = ScenarioJson::GetString(Tool, TEXT("name"));
		TestFalse(FString::Printf(TEXT("%s is unique"), *Name), Names.Contains(Name));
		Names.Add(Name);
		TestEqual(FString::Printf(TEXT("%s is snake case"), *Name), Name, Name.ToLower());
		const FString Description = ScenarioJson::GetString(Tool, TEXT("description"));
		TestTrue(FString::Printf(TEXT("%s description under 2,500 characters"), *Name), Description.Len() > 0 && Description.Len() < 2500);
		TestEqual(FString::Printf(TEXT("%s schema is an object"), *Name), ScenarioJson::GetString(ScenarioJson::GetObject(Tool, TEXT("inputSchema")), TEXT("type")), FString(TEXT("object")));
	}
	for (const TCHAR* Spending : { TEXT("generate"), TEXT("run_workflow"), TEXT("approve_prompt") })
	{
		const FScenarioAction* Action = FScenarioActions::Get().Find(Spending);
		TestTrue(FString::Printf(TEXT("%s says it spends first"), Spending), Action && Action->Description.StartsWith(TEXT("SPENDS")));
		TestTrue(FString::Printf(TEXT("%s is marked as costing"), Spending), Action && Action->bCosts);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioAgentServerChecksTest, "Scenario.Agent.Server.Checks", SCENARIO_TEST_FLAGS)
bool FScenarioAgentServerChecksTest::RunTest(const FString& Parameters)
{
	TSharedRef<FScenarioMcpServer> Server = NewServer();
	const FString Ping = TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}");

	FScenarioLocalRequest WrongHost = Make(TEXT("POST"), TEXT("/mcp"), Ping);
	WrongHost.Headers.Add(TEXT("host"), TEXT("evil.test:9877"));
	TestEqual(TEXT("foreign Host refused"), Send(*Server, WrongHost).Status, 403);

	FScenarioLocalRequest TwoHosts = Make(TEXT("POST"), TEXT("/mcp"), Ping);
	TwoHosts.HostCount = 2;
	TestEqual(TEXT("two Host headers refused"), Send(*Server, TwoHosts).Status, 403);

	FScenarioLocalRequest WebPage = Make(TEXT("POST"), TEXT("/mcp"), Ping);
	WebPage.Headers.Add(TEXT("origin"), TEXT("https://evil.test"));
	TestEqual(TEXT("web page Origin refused"), Send(*Server, WebPage).Status, 403);

	TestEqual(TEXT("CORS preflight refused"), Send(*Server, Make(TEXT("OPTIONS"), TEXT("/mcp"), FString())).Status, 403);
	TestEqual(TEXT("health needs no token"), Send(*Server, Make(TEXT("GET"), TEXT("/health"), FString(), false)).Status, 200);
	TestEqual(TEXT("missing token"), Send(*Server, Make(TEXT("POST"), TEXT("/mcp"), Ping, false)).Status, 401);

	FScenarioLocalRequest WrongToken = Make(TEXT("POST"), TEXT("/mcp"), Ping, false);
	WrongToken.Headers.Add(TEXT("authorization"), TEXT("Bearer nope"));
	TestEqual(TEXT("wrong token"), Send(*Server, WrongToken).Status, 401);

	const FScenarioLocalResponse Pong = Send(*Server, Make(TEXT("POST"), TEXT("/mcp"), Ping));
	TestEqual(TEXT("ping answered"), Pong.Status, 200);
	TestTrue(TEXT("ping has a result"), Rpc(Pong).IsValid() && Rpc(Pong)->HasField(TEXT("result")));

	TestEqual(TEXT("GET /mcp has no stream"), Send(*Server, Make(TEXT("GET"), TEXT("/mcp"), FString())).Status, 405);
	TestEqual(TEXT("unknown path"), Send(*Server, Make(TEXT("POST"), TEXT("/other"), Ping)).Status, 404);

	Server->bRequireToken = false;
	TestEqual(TEXT("token off accepts no header"), Send(*Server, Make(TEXT("POST"), TEXT("/mcp"), Ping, false)).Status, 200);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioAgentRpcTest, "Scenario.Agent.Server.JsonRpc", SCENARIO_TEST_FLAGS)
bool FScenarioAgentRpcTest::RunTest(const FString& Parameters)
{
	TSharedRef<FScenarioMcpServer> Server = NewServer();

	const FScenarioLocalResponse Init = Send(*Server, Make(TEXT("POST"), TEXT("/mcp"),
		TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-03-26\",\"clientInfo\":{\"name\":\"claude-code\",\"version\":\"1\"}}}")));
	const TSharedPtr<FJsonObject> InitResult = ScenarioJson::GetObject(Rpc(Init), TEXT("result"));
	TestEqual(TEXT("version echoed when supported"), ScenarioJson::GetString(InitResult, TEXT("protocolVersion")), FString(TEXT("2025-03-26")));
	TestEqual(TEXT("server name"), ScenarioJson::GetString(ScenarioJson::GetObject(InitResult, TEXT("serverInfo")), TEXT("name")), FString(TEXT("scenario-unreal")));
	const FString* Session = Init.Headers.Find(TEXT("Mcp-Session-Id"));
	TestTrue(TEXT("session id returned"), Session && !Session->IsEmpty());
	TestTrue(TEXT("client remembered"), Server->LastSeen(TEXT("claude")) > 0.0);

	FScenarioLocalRequest Notification = Make(TEXT("POST"), TEXT("/mcp"), TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"));
	if (Session)
	{
		Notification.Headers.Add(TEXT("mcp-session-id"), *Session);
	}
	TestEqual(TEXT("notification accepted without reply"), Send(*Server, Notification).Status, 202);

	FScenarioLocalRequest Stale = Make(TEXT("POST"), TEXT("/mcp"), TEXT("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}"));
	Stale.Headers.Add(TEXT("mcp-session-id"), TEXT("from-before-a-restart"));
	TestEqual(TEXT("unknown session asks to initialize again"), Send(*Server, Stale).Status, 404);

	const TSharedPtr<FJsonObject> List = Rpc(Send(*Server, Make(TEXT("POST"), TEXT("/mcp"), TEXT("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}"))));
	TestEqual(TEXT("tools listed"), ScenarioJson::GetArray(ScenarioJson::GetObject(List, TEXT("result")), TEXT("tools")).Num(), 48);

	const TSharedPtr<FJsonObject> Unknown = Rpc(Send(*Server, Make(TEXT("POST"), TEXT("/mcp"), TEXT("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"resources/list\"}"))));
	TestEqual(TEXT("unknown method"), static_cast<int32>(ScenarioJson::GetNumber(ScenarioJson::GetObject(Unknown, TEXT("error")), TEXT("code"))), -32601);

	const TSharedPtr<FJsonObject> Batch = Rpc(Send(*Server, Make(TEXT("POST"), TEXT("/mcp"), TEXT("[{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"ping\"}]"))));
	TestEqual(TEXT("batches refused"), static_cast<int32>(ScenarioJson::GetNumber(ScenarioJson::GetObject(Batch, TEXT("error")), TEXT("code"))), -32600);

	const TSharedPtr<FJsonObject> Garbage = Rpc(Send(*Server, Make(TEXT("POST"), TEXT("/mcp"), TEXT("{not json"))));
	TestEqual(TEXT("parse error"), static_cast<int32>(ScenarioJson::GetNumber(ScenarioJson::GetObject(Garbage, TEXT("error")), TEXT("code"))), -32700);

	const TSharedPtr<FJsonObject> NoTool = Rpc(Send(*Server, Make(TEXT("POST"), TEXT("/mcp"), TEXT("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"format_disk\"}}"))));
	TestEqual(TEXT("unknown tool"), static_cast<int32>(ScenarioJson::GetNumber(ScenarioJson::GetObject(NoTool, TEXT("error")), TEXT("code"))), -32602);

	const TSharedPtr<FJsonObject> Status = Rpc(Send(*Server, Make(TEXT("POST"), TEXT("/mcp"), TEXT("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"scenario_status\",\"arguments\":{}}}"))));
	const TSharedPtr<FJsonObject> StatusResult = ScenarioJson::GetObject(Status, TEXT("result"));
	TestFalse(TEXT("status is not an error"), ScenarioJson::GetBool(StatusResult, TEXT("isError"), true));
	const TArray<TSharedPtr<FJsonValue>> Content = ScenarioJson::GetArray(StatusResult, TEXT("content"));
	TestTrue(TEXT("status carries the plugin version"), Content.Num() > 0 && ScenarioJson::GetString(Content[0]->AsObject(), TEXT("text")).Contains(TEXT("plugin_version")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioAgentPermissionsTest, "Scenario.Agent.Actions.Permissions", SCENARIO_TEST_FLAGS)
bool FScenarioAgentPermissionsTest::RunTest(const FString& Parameters)
{
	FScenarioCaller Agent;
	Agent.Source = TEXT("agent");
	Agent.Client = TEXT("test");
	Agent.bCanChangeProject = false;
	Agent.bCanRunPython = false;

	auto Call = [](const FString& Name, const FScenarioCaller& Caller, const FString& Args)
	{
		FScenarioActionResult Out;
		Out.Error = TEXT("not answered");
		FScenarioActions::Get().Call(Name, ScenarioJson::ParseObject(Args), Caller, [&Out](const FScenarioActionResult& Result) { Out = Result; });
		return Out;
	};

	TestTrue(TEXT("save refused without the change switch"), Call(TEXT("save_changes"), Agent, TEXT("{}")).Error.StartsWith(TEXT("Permission")));
	TestTrue(TEXT("remove refused without the change switch"), Call(TEXT("remove_scenario_items"), Agent, TEXT("{\"actor_ids\":[\"x\"]}")).Error.StartsWith(TEXT("Permission")));
	TestTrue(TEXT("python refused"), Call(TEXT("execute_python"), Agent, TEXT("{\"code\":\"print(1)\"}")).Error.StartsWith(TEXT("Permission")));

	FScenarioCaller Changer = Agent;
	Changer.bCanChangeProject = true;
	TestTrue(TEXT("python needs its own switch"), Call(TEXT("execute_python"), Changer, TEXT("{\"code\":\"print(1)\"}")).Error.StartsWith(TEXT("Permission")));
	TestTrue(TEXT("scripts never run agent python"), Call(TEXT("execute_python"), FScenarioCaller::Script(TEXT("test")), TEXT("{\"code\":\"print(1)\"}")).Error.StartsWith(TEXT("Permission")));

	TestTrue(TEXT("apply reaches its own check (proposal path)"), Call(TEXT("apply_result_application"), Agent, TEXT("{\"application_id\":\"app_missing\"}")).Error.StartsWith(TEXT("NotFound")));
	TestTrue(TEXT("generate needs a quote"), Call(TEXT("generate"), Agent, TEXT("{}")).Error.Len() > 0);
	TestTrue(TEXT("not_sent is the person's call"), Call(TEXT("recover_local_job"), Changer, TEXT("{\"action\":\"not_sent\",\"job_id\":\"x\"}")).Error.Len() > 0);
	TestTrue(TEXT("unknown action"), Call(TEXT("format_disk"), Changer, TEXT("{}")).Error.StartsWith(TEXT("NotFound")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioAgentJsonConfigTest, "Scenario.Agent.Clients.Json", SCENARIO_TEST_FLAGS)
bool FScenarioAgentJsonConfigTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FJsonObject> Entry = ScenarioAgentClients::HttpEntry(TEXT("http://127.0.0.1:9877/mcp"), TEXT("tok"), true);
	FString Out;
	TestTrue(TEXT("existing file updated"), ScenarioAgentClients::UpsertJsonEntry(TEXT("{\"theme\":\"dark\",\"mcpServers\":{\"other\":{\"url\":\"u\"}}}"), TEXT("mcpServers"), TEXT("scenario-unreal"), Entry, Out));
	const TSharedPtr<FJsonObject> Root = ScenarioJson::ParseObject(Out);
	TestEqual(TEXT("other settings kept"), ScenarioJson::GetString(Root, TEXT("theme")), FString(TEXT("dark")));
	const TSharedPtr<FJsonObject> Servers = ScenarioJson::GetObject(Root, TEXT("mcpServers"));
	TestTrue(TEXT("other server kept"), Servers.IsValid() && Servers->HasField(TEXT("other")));
	TestEqual(TEXT("our url"), ScenarioJson::GetString(ScenarioJson::GetObject(Servers, TEXT("scenario-unreal")), TEXT("url")), FString(TEXT("http://127.0.0.1:9877/mcp")));

	TestTrue(TEXT("missing file created"), ScenarioAgentClients::UpsertJsonEntry(FString(), TEXT("mcpServers"), TEXT("scenario-unreal"), Entry, Out));
	TestFalse(TEXT("broken file left alone"), ScenarioAgentClients::UpsertJsonEntry(TEXT("{\"mcpServers\": {"), TEXT("mcpServers"), TEXT("scenario-unreal"), Entry, Out));
	TestFalse(TEXT("unexpected shape left alone"), ScenarioAgentClients::UpsertJsonEntry(TEXT("{\"mcpServers\": []}"), TEXT("mcpServers"), TEXT("scenario-unreal"), Entry, Out));

	const FString Setup = ScenarioAgentClients::SetupText(TEXT("http://127.0.0.1:9877/mcp"), TEXT("tok"));
	TestTrue(TEXT("setup has the claude command"), Setup.Contains(TEXT("claude mcp add --transport http --scope user scenario-unreal")));
	TestTrue(TEXT("setup has the codex table"), Setup.Contains(TEXT("[mcp_servers.scenario-unreal.http_headers]")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioAgentTomlConfigTest, "Scenario.Agent.Clients.Toml", SCENARIO_TEST_FLAGS)
bool FScenarioAgentTomlConfigTest::RunTest(const FString& Parameters)
{
	const FString Table = TEXT("mcp_servers.scenario-unreal");
	const FString Body = ScenarioAgentClients::CodexTable(TEXT("http://127.0.0.1:9878/mcp"), TEXT("new"));
	const FString Before =
		TEXT("model = \"x\"\n\n")
		TEXT("[mcp_servers.scenario-unreal]\nurl = \"http://127.0.0.1:9877/mcp\"\n\n")
		TEXT("[mcp_servers.scenario-unreal.http_headers]\nAuthorization = \"Bearer old\"\n\n")
		TEXT("[mcp_servers.scenario-unreal-staging]\nurl = \"keep\"\n\n")
		TEXT("[profiles.fast]\nmodel = \"y\"\n");
	const FString After = ScenarioAgentClients::UpsertTomlTable(Before, Table, Body);
	TestTrue(TEXT("new url"), After.Contains(TEXT("http://127.0.0.1:9878/mcp")));
	TestFalse(TEXT("old url gone"), After.Contains(TEXT("http://127.0.0.1:9877/mcp")));
	TestFalse(TEXT("old token gone"), After.Contains(TEXT("Bearer old")));
	TestTrue(TEXT("sibling with a longer name kept"), After.Contains(TEXT("[mcp_servers.scenario-unreal-staging]")) && After.Contains(TEXT("url = \"keep\"")));
	TestTrue(TEXT("other tables kept"), After.Contains(TEXT("[profiles.fast]")) && After.Contains(TEXT("model = \"x\"")));
	int32 Count = 0;
	for (int32 At = After.Find(TEXT("[mcp_servers.scenario-unreal]")); At != INDEX_NONE; At = After.Find(TEXT("[mcp_servers.scenario-unreal]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
	{
		++Count;
	}
	TestEqual(TEXT("one table"), Count, 1);

	const FString Appended = ScenarioAgentClients::UpsertTomlTable(TEXT("model = \"x\"\n"), Table, Body);
	TestTrue(TEXT("appended when absent"), Appended.StartsWith(TEXT("model = \"x\"")) && Appended.Contains(TEXT("[mcp_servers.scenario-unreal]")));
	return true;
}

#endif
