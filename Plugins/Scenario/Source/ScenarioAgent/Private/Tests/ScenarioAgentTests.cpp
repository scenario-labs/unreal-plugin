// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
	// scenario-mcp's rules, as scenario-labs/components' mcp-check applies them: what Claude Code shows
	// the model (2,048 characters of each description and of the instructions) holds every rule, each tool
	// has a title, the four hints and a Returns: line, and at most 30 tools are listed besides the catalog.
	const TSharedRef<FJsonObject> Snapshot = FScenarioMcpServer::Snapshot();
	TestTrue(TEXT("instructions within the 2,048-character cut"), ScenarioJson::GetString(Snapshot, TEXT("instructions")).Len() <= 2048);
	const TArray<TSharedPtr<FJsonValue>> Tools = ScenarioJson::GetArray(Snapshot, TEXT("tools"));
	TestEqual(TEXT("38 built-in tools, 9 kit tools and the catalog's 5"), Tools.Num(), 52);

	// Every description string in a schema, instance data (enum, default...) left out.
	TFunction<void(const TSharedPtr<FJsonValue>&, TArray<FString>&)> Descriptions = [&Descriptions](const TSharedPtr<FJsonValue>& Value, TArray<FString>& Out)
	{
		if (Value.IsValid() && Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				Descriptions(Item, Out);
			}
		}
		else if (Value.IsValid() && Value->Type == EJson::Object)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Value->AsObject()->Values)
			{
				if (Field.Key == TEXT("description") && Field.Value->Type == EJson::String)
				{
					Out.Add(Field.Value->AsString());
				}
				else if (Field.Key != TEXT("enum") && Field.Key != TEXT("default") && Field.Key != TEXT("examples") && Field.Key != TEXT("const"))
				{
					Descriptions(Field.Value, Out);
				}
			}
		}
	};

	int32 Listed = 0;
	TSet<FString> Names;
	for (const TSharedPtr<FJsonValue>& Value : Tools)
	{
		const TSharedPtr<FJsonObject> Tool = Value->AsObject();
		const FString Name = ScenarioJson::GetString(Tool, TEXT("name"));
		TestFalse(FString::Printf(TEXT("%s is unique"), *Name), Names.Contains(Name));
		Names.Add(Name);
		TestTrue(FString::Printf(TEXT("%s is snake case, 64 characters at most"), *Name), Name.Len() <= 64 && Name == Name.ToLower() && !Name.Contains(TEXT(" ")));
		TestFalse(FString::Printf(TEXT("%s has a title"), *Name), ScenarioJson::GetString(Tool, TEXT("title")).IsEmpty());
		const TSharedPtr<FJsonObject> Hints = ScenarioJson::GetObject(Tool, TEXT("annotations"));
		bool bHint = false;
		for (const TCHAR* Hint : { TEXT("readOnlyHint"), TEXT("destructiveHint"), TEXT("idempotentHint"), TEXT("openWorldHint") })
		{
			TestTrue(FString::Printf(TEXT("%s sets %s"), *Name, Hint), Hints.IsValid() && Hints->TryGetBoolField(Hint, bHint));
		}
		TestFalse(FString::Printf(TEXT("%s is not read-only and destructive at once"), *Name),
			ScenarioJson::GetBool(Hints, TEXT("readOnlyHint")) && ScenarioJson::GetBool(Hints, TEXT("destructiveHint")));

		const FString Description = ScenarioJson::GetString(Tool, TEXT("description"));
		TestTrue(FString::Printf(TEXT("%s description within the cut (%d)"), *Name, Description.Len()), Description.Len() > 0 && Description.Len() <= 2048);
		TestTrue(FString::Printf(TEXT("%s says what it returns on a Returns: line"), *Name), Description.StartsWith(TEXT("Returns:")) || Description.Contains(TEXT("\nReturns:")));
		TestFalse(FString::Printf(TEXT("%s has no Args: block"), *Name), Description.StartsWith(TEXT("Args:")) || Description.Contains(TEXT("\nArgs:")));

		const TSharedPtr<FJsonObject> Schema = ScenarioJson::GetObject(Tool, TEXT("inputSchema"));
		TestEqual(FString::Printf(TEXT("%s schema is an object"), *Name), ScenarioJson::GetString(Schema, TEXT("type")), FString(TEXT("object")));
		const int32 SchemaBytes = FTCHARToUTF8(*ScenarioJson::Write(Schema)).Length();
		TestTrue(FString::Printf(TEXT("%s schema under 4,500 bytes (%d)"), *Name, SchemaBytes), SchemaBytes <= 4500);
		const TSharedPtr<FJsonObject> Properties = ScenarioJson::GetObject(Schema, TEXT("properties"));
		if (Properties.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Property : Properties->Values)
			{
				TestEqual(FString::Printf(TEXT("%s.%s is snake case"), *Name, *Property.Key), Property.Key, Property.Key.ToLower());
				const TSharedPtr<FJsonObject> Field = Property.Value->AsObject();
				TestFalse(FString::Printf(TEXT("%s.%s is described"), *Name, *Property.Key), ScenarioJson::GetString(Field, TEXT("description")).TrimStartAndEnd().IsEmpty());
			}
		}
		for (const FString& Required : ScenarioJson::GetStringArray(Schema, TEXT("required")))
		{
			TestTrue(FString::Printf(TEXT("%s requires %s, one of its parameters"), *Name, *Required), Properties.IsValid() && Properties->HasField(Required));
		}
		TArray<FString> Texts;
		Descriptions(MakeShared<FJsonValueObject>(Schema), Texts);
		for (const FString& Text : Texts)
		{
			TestTrue(FString::Printf(TEXT("%s parameter descriptions under 1,024 characters"), *Name), Text.Len() <= 1024);
		}
		const bool bCatalogOnly = ScenarioJson::GetString(ScenarioJson::GetObject(Tool, TEXT("_meta")), TEXT("scenario.com/listing")) == TEXT("catalog");
		Listed += !bCatalogOnly && !Name.StartsWith(TEXT("unreal_tool")) ? 1 : 0;
	}
	TestTrue(FString::Printf(TEXT("At most 30 tools listed besides the catalog (%d)"), Listed), Listed <= 30);

	// The sizes docs/ARCHITECTURE.md quotes ("Tool list size").
	const FString Default = ScenarioJson::Write(FScenarioMcpServer::ToolsList());
	const FString Every = ScenarioJson::Write(FScenarioMcpServer::ToolsList(true));
	AddInfo(FString::Printf(TEXT("tools/list: %d tools, %d bytes; with every tool: %d tools, %d bytes"),
		ScenarioJson::GetArray(FScenarioMcpServer::ToolsList(), TEXT("tools")).Num(), FTCHARToUTF8(*Default).Length(),
		ScenarioJson::GetArray(FScenarioMcpServer::ToolsList(true), TEXT("tools")).Num(), FTCHARToUTF8(*Every).Length()));

	for (const TCHAR* Spending : { TEXT("generate"), TEXT("run_workflow"), TEXT("approve_prompt") })
	{
		const FScenarioAction* Action = FScenarioActions::Get().Find(Spending);
		TestTrue(FString::Printf(TEXT("%s says it spends first"), Spending), Action && Action->Description.StartsWith(TEXT("SPENDS")));
		TestTrue(FString::Printf(TEXT("%s is marked as costing"), Spending), Action && Action->bCosts);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioAgentToolSnapshotTest, "Scenario.Agent.Tools.Snapshot", SCENARIO_TEST_FLAGS)
bool FScenarioAgentToolSnapshotTest::RunTest(const FString& Parameters)
{
	FString Text = ScenarioJson::Write(FScenarioMcpServer::Snapshot(), true).Replace(TEXT("\r\n"), TEXT("\n")) + TEXT("\n");
	const FString Fresh = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Scenario"), TEXT("Tests"), TEXT("mcp-tools.json"));
	FFileHelper::SaveStringToFile(Text, *Fresh, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scenario"));
	FString Committed;
	const bool bRead = Plugin.IsValid() && FFileHelper::LoadFileToString(Committed, *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tests"), TEXT("mcp-tools.json")));
	TestTrue(FString::Printf(TEXT("Plugins/Scenario/Tests/mcp-tools.json is current; if the change is intended, copy %s over it"), *FPaths::ConvertRelativePathToFull(Fresh)),
		bRead && Committed.Replace(TEXT("\r\n"), TEXT("\n")) == Text);
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
	TestEqual(TEXT("29 core tools and the catalog's 5 listed"), ScenarioJson::GetArray(ScenarioJson::GetObject(List, TEXT("result")), TEXT("tools")).Num(), 34);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioAgentCatalogTest, "Scenario.Agent.Server.Catalog", SCENARIO_TEST_FLAGS)
bool FScenarioAgentCatalogTest::RunTest(const FString& Parameters)
{
	TSharedRef<FScenarioMcpServer> Server = NewServer();
	auto Call = [&Server](int32 Id, const FString& Name, const FString& Arguments)
	{
		const FString Body = FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\",\"params\":{\"name\":\"%s\",\"arguments\":%s}}"), Id, *Name, *Arguments);
		return ScenarioJson::GetObject(Rpc(Send(*Server, Make(TEXT("POST"), TEXT("/mcp"), Body))), TEXT("result"));
	};
	auto Text = [](const TSharedPtr<FJsonObject>& Result)
	{
		const TArray<TSharedPtr<FJsonValue>> Content = ScenarioJson::GetArray(Result, TEXT("content"));
		return Content.Num() > 0 ? ScenarioJson::GetString(Content[0]->AsObject(), TEXT("text")) : FString();
	};
	auto ListedNames = [&Server]()
	{
		const FString ListBody = TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");
		TArray<FString> Names;
		for (const TSharedPtr<FJsonValue>& Value : ScenarioJson::GetArray(ScenarioJson::GetObject(Rpc(Send(*Server, Make(TEXT("POST"), TEXT("/mcp"), ListBody))), TEXT("result")), TEXT("tools")))
		{
			Names.Add(ScenarioJson::GetString(Value->AsObject(), TEXT("name")));
		}
		return Names;
	};

	int32 CatalogOnly = 0;
	for (const FScenarioAction& Action : FScenarioActions::Get().All())
	{
		CatalogOnly += Action.bCatalogOnly ? 1 : 0;
	}
	const TArray<FString> Listed = ListedNames();
	TestEqual(TEXT("tools/list: the core tools and the catalog's five"), Listed.Num(), FScenarioActions::Get().All().Num() - CatalogOnly + 5);
	TestTrue(TEXT("The catalog is listed"), Listed.Contains(TEXT("unreal_tools_list")) && Listed.Contains(TEXT("unreal_tools_search")) && Listed.Contains(TEXT("unreal_tool_execute_delete")));
	TestTrue(TEXT("The core loop is listed"), Listed.Contains(TEXT("generate")) && Listed.Contains(TEXT("apply_result_application")));
	TestFalse(TEXT("Kit tools are in the catalog only"), Listed.Contains(TEXT("make_film")));
	Server->bListEveryTool = true;
	TestEqual(TEXT("With every tool listed"), ListedNames().Num(), FScenarioActions::Get().All().Num() + 5);
	Server->bListEveryTool = false;

	const TArray<TSharedPtr<FJsonValue>> Rows = ScenarioJson::GetArray(ScenarioJson::ParseObject(Text(Call(2, TEXT("unreal_tools_list"), TEXT("{}")))), TEXT("tools"));
	TestEqual(TEXT("unreal_tools_list has every tool"), Rows.Num(), FScenarioActions::Get().All().Num());
	for (const TSharedPtr<FJsonValue>& Row : Rows)
	{
		if (ScenarioJson::GetString(Row->AsObject(), TEXT("name")) == TEXT("make_film"))
		{
			TestFalse(TEXT("make_film is not listed"), ScenarioJson::GetBool(Row->AsObject(), TEXT("listed"), true));
			TestEqual(TEXT("make_film runs in the delete lane"), ScenarioJson::GetString(Row->AsObject(), TEXT("lane")), FString(TEXT("delete")));
		}
	}

	const TArray<TSharedPtr<FJsonValue>> Found = ScenarioJson::GetArray(ScenarioJson::ParseObject(Text(Call(3, TEXT("unreal_tools_search"), TEXT("{\"query\":\"film\"}")))), TEXT("results"));
	bool bFilm = false;
	for (const TSharedPtr<FJsonValue>& Row : Found)
	{
		bFilm |= ScenarioJson::GetString(Row->AsObject(), TEXT("name")) == TEXT("make_film") && ScenarioJson::GetObject(Row->AsObject(), TEXT("inputSchema")).IsValid();
	}
	TestTrue(TEXT("unreal_tools_search finds make_film with its schema"), bFilm);
	TestTrue(TEXT("unreal_tools_search needs a query"), ScenarioJson::GetBool(Call(4, TEXT("unreal_tools_search"), TEXT("{}")), TEXT("isError")));

	const TSharedPtr<FJsonObject> Status = Call(5, TEXT("unreal_tool_execute_read"), TEXT("{\"name\":\"scenario_status\",\"parameters\":{}}"));
	TestFalse(TEXT("The read lane runs a read tool"), ScenarioJson::GetBool(Status, TEXT("isError"), true));
	TestTrue(TEXT("It returns the tool's answer"), Text(Status).Contains(TEXT("plugin_version")));
	const TSharedPtr<FJsonObject> Wrong = Call(6, TEXT("unreal_tool_execute_read"), TEXT("{\"name\":\"generate\",\"parameters\":{}}"));
	TestTrue(TEXT("The read lane refuses a paid tool, naming the write lane"), ScenarioJson::GetBool(Wrong, TEXT("isError")) && Text(Wrong).Contains(TEXT("unreal_tool_execute_write")));
	TestTrue(TEXT("Unknown tools are refused"), ScenarioJson::GetBool(Call(7, TEXT("unreal_tool_execute_read"), TEXT("{\"name\":\"format_disk\"}")), TEXT("isError")));

	// Paid tools keep their rules through the catalog: no valid quote, nothing sent. With "Approve each spend
	// here" on, the call would wait for the person and answer after Send returns: keep it off for this check.
	TGuardValue<bool> NoSpendApproval(UScenarioEditorSettings::GetMutable().bApproveAgentSpend, false);
	const TSharedPtr<FJsonObject> Paid = Call(8, TEXT("unreal_tool_execute_write"), TEXT("{\"name\":\"generate\",\"parameters\":{\"quote_id\":\"q_none\",\"approved_cost\":\"1\"}}"));
	TestTrue(TEXT("The write lane keeps the money rules"), ScenarioJson::GetBool(Paid, TEXT("isError")));

	TestFalse(TEXT("Catalog-only tools stay callable by name"), ScenarioJson::GetBool(Call(9, TEXT("output_log"), TEXT("{}")), TEXT("isError"), true));
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
	{
		// Synchronous only while spends do not wait for the person's approval.
		TGuardValue<bool> NoSpendApproval(UScenarioEditorSettings::GetMutable().bApproveAgentSpend, false);
		TestTrue(TEXT("generate needs a quote"), Call(TEXT("generate"), Agent, TEXT("{}")).Error.Len() > 0);
	}
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
