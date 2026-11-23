// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioMcpServer.h"

#include "Containers/Ticker.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "ScenarioActions.h"
#include "ScenarioJson.h"
#include "ScenarioTypes.h"

const TCHAR* FScenarioMcpServer::ServerName = TEXT("scenario-unreal");

namespace
{
	const TCHAR* const ProtocolVersions[] = { TEXT("2025-06-18"), TEXT("2025-03-26"), TEXT("2024-11-05") };

	FString PluginVersion()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scenario"));
		return Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("0.0.0");
	}

	const TCHAR* Instructions()
	{
		return TEXT("Scenario for Unreal Engine: generate images, video, 3D, materials, audio and skies with Scenario and land them in this Unreal project. ")
			TEXT("Order: scenario_status, scene_summary, list_models, model_schema, estimate_cost, generate (quote_id and approved_cost from the estimate), ")
			TEXT("wait_for_job, prepare_result_application, apply_result_application, then save_changes. ")
			TEXT("Never resend a request whose job is unknown: use recover_local_job. ")
			TEXT("More tools (the kit's worlds, games and films, Prompt Spark, the Scenario library, the Output Log) are in the catalog: ")
			TEXT("unreal_tools_list or unreal_tools_search, then unreal_tool_execute_<lane> or the tool's own name.");
	}

	/** Same length and same bytes, without an early exit. */
	bool ConstantTimeEquals(const FString& A, const FString& B)
	{
		const FTCHARToUTF8 Left(*A);
		const FTCHARToUTF8 Right(*B);
		if (Left.Length() != Right.Length())
		{
			return false;
		}
		uint8 Diff = 0;
		for (int32 Index = 0; Index < Left.Length(); ++Index)
		{
			Diff |= static_cast<uint8>(Left.Get()[Index] ^ Right.Get()[Index]);
		}
		return Diff == 0;
	}

	TSharedRef<FJsonObject> ToolResult(const FScenarioActionResult& Result)
	{
		TArray<TSharedPtr<FJsonValue>> Content;
		TSharedRef<FJsonObject> Text = MakeShared<FJsonObject>();
		Text->SetStringField(TEXT("type"), TEXT("text"));
		if (Result.bOk)
		{
			Text->SetStringField(TEXT("text"), Result.Data.IsValid() ? ScenarioJson::Write(Result.Data) : TEXT("{}"));
		}
		else
		{
			FString Body = Result.Error;
			if (Result.Data.IsValid())
			{
				Body += TEXT("\n") + ScenarioJson::Write(Result.Data);
			}
			Text->SetStringField(TEXT("text"), Body);
		}
		Content.Add(MakeShared<FJsonValueObject>(Text));
		if (Result.Png.Num() > 0)
		{
			TSharedRef<FJsonObject> Image = MakeShared<FJsonObject>();
			Image->SetStringField(TEXT("type"), TEXT("image"));
			Image->SetStringField(TEXT("mimeType"), TEXT("image/png"));
			Image->SetStringField(TEXT("data"), FBase64::Encode(Result.Png));
			Content.Add(MakeShared<FJsonValueObject>(Image));
		}
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetArrayField(TEXT("content"), Content);
		Json->SetBoolField(TEXT("isError"), !Result.bOk);
		return Json;
	}
}

bool FScenarioMcpServer::Start(int32 FirstPort, FString& OutError)
{
	Stop();
	TWeakPtr<FScenarioMcpServer> Weak = AsShared();
	Listener = MakeShared<FScenarioLocalHttpServer, ESPMode::ThreadSafe>(TEXT("ScenarioMcp"), [Weak](const FScenarioLocalRequest& Request, FScenarioLocalRespond Respond)
	{
		if (const TSharedPtr<FScenarioMcpServer> This = Weak.Pin())
		{
			This->Handle(Request, Respond);
			return;
		}
		Respond(FScenarioLocalResponse::Text(503, TEXT("Server stopping.")));
	});
	if (!Listener->Start(FirstPort, PortTries))
	{
		Listener.Reset();
		OutError = FString::Printf(TEXT("Ports %d to %d are all taken."), FirstPort, FirstPort + PortTries - 1);
		return false;
	}
	Port = Listener->GetPort();
	return true;
}

void FScenarioMcpServer::Stop()
{
	if (Listener.IsValid())
	{
		Listener->Stop();
		Listener.Reset();
	}
	Sessions.Reset();
}

bool FScenarioMcpServer::IsRunning() const
{
	return Listener.IsValid() && Listener->IsRunning();
}

FString FScenarioMcpServer::Url() const
{
	return FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), Port > 0 ? Port : DefaultPort);
}

double FScenarioMcpServer::LastSeen(const FString& Fragment) const
{
	double Latest = 0.0;
	for (const TPair<FString, FClient>& Pair : Sessions)
	{
		if (Pair.Value.Name.Contains(Fragment, ESearchCase::IgnoreCase))
		{
			Latest = FMath::Max(Latest, Pair.Value.LastSeen);
		}
	}
	return Latest;
}

bool FScenarioMcpServer::TokenMatches(const FString& Authorization) const
{
	FString Value = Authorization.TrimStartAndEnd();
	if (!Value.StartsWith(TEXT("Bearer "), ESearchCase::IgnoreCase) || Token.IsEmpty())
	{
		return false;
	}
	return ConstantTimeEquals(Value.RightChop(7).TrimStartAndEnd(), Token);
}

namespace
{
	// The catalog, named as scenario-mcp names its own (scenario_tools_list...): every tool stays callable by name.
	const TCHAR* const ToolsListName = TEXT("unreal_tools_list");
	const TCHAR* const ToolsSearchName = TEXT("unreal_tools_search");
	const TCHAR* const ExecutePrefix = TEXT("unreal_tool_execute_");
	/** The marker scenario-mcp puts on tools served only through its catalog. */
	const TCHAR* const ListingKey = TEXT("scenario.com/listing");

	TSharedRef<FJsonObject> Annotations(const FString& Title, bool bReadOnly, bool bDestructive, bool bIdempotent, bool bOpenWorld)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Title.IsEmpty())
		{
			Json->SetStringField(TEXT("title"), Title);
		}
		Json->SetBoolField(TEXT("readOnlyHint"), bReadOnly);
		Json->SetBoolField(TEXT("destructiveHint"), bDestructive);
		Json->SetBoolField(TEXT("idempotentHint"), bIdempotent);
		Json->SetBoolField(TEXT("openWorldHint"), bOpenWorld);
		return Json;
	}

	/** The catalog executor that runs a tool, from its hints: read, write or delete. */
	FString LaneOf(const FScenarioAction& Action)
	{
		if (Action.Kind == EScenarioActionKind::Read)
		{
			return TEXT("read");
		}
		return Action.bDestructive ? TEXT("delete") : TEXT("write");
	}

	TSharedRef<FJsonObject> Prop(const TCHAR* Type, const TCHAR* Description)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("type"), Type);
		Json->SetStringField(TEXT("description"), Description);
		return Json;
	}

	TSharedRef<FJsonObject> CatalogTool(const FString& Name, const TCHAR* Title, const TCHAR* Description, const TSharedRef<FJsonObject>& Properties,
		const TArray<FString>& Required, const TSharedRef<FJsonObject>& Hints)
	{
		TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		if (Required.Num() > 0)
		{
			Schema->SetField(TEXT("required"), ScenarioJson::StringArrayValue(Required));
		}
		TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), Name);
		Tool->SetStringField(TEXT("title"), Title);
		Tool->SetStringField(TEXT("description"), Description);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tool->SetObjectField(TEXT("annotations"), Hints);
		return Tool;
	}

	/** unreal_tools_list, unreal_tools_search and the three executors. */
	TArray<TSharedPtr<FJsonValue>> CatalogTools()
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		Tools.Add(MakeShared<FJsonValueObject>(CatalogTool(ToolsListName, TEXT("List Unreal Tools"),
			TEXT("Every tool of this editor's Scenario server, including those not in tools/list: the kit's worlds, games and films, Prompt Spark, the Scenario library, the Output Log. Free.\n")
			TEXT("Returns: { tools: [{ name, title, summary, lane (read, write or delete), listed }] }. listed tools can be called directly; get another tool's input schema from unreal_tools_search, then run it with unreal_tool_execute_<lane> or by its name."),
			MakeShared<FJsonObject>(), {}, Annotations(TEXT("List Unreal Tools"), true, false, true, false))));

		TSharedRef<FJsonObject> SearchProps = MakeShared<FJsonObject>();
		SearchProps->SetObjectField(TEXT("query"), Prop(TEXT("string"), TEXT("Words matched against tool names, titles and descriptions, for example film or prompt.")));
		SearchProps->SetObjectField(TEXT("limit"), Prop(TEXT("integer"), TEXT("Most tools to return, 1 to 20. Default 8.")));
		Tools.Add(MakeShared<FJsonValueObject>(CatalogTool(ToolsSearchName, TEXT("Search Unreal Tools"),
			TEXT("Finds tools of this editor's Scenario server by keyword and gives their input schemas. Searches tools only: for models use list_models, for project assets find_assets. Free.\n")
			TEXT("Returns: { results: [{ name, title, lane, listed, description, inputSchema, annotations }] }; run a result with unreal_tool_execute_<lane> or by its name."),
			SearchProps, { TEXT("query") }, Annotations(TEXT("Search Unreal Tools"), true, false, true, false))));

		struct FLane { const TCHAR* Lane; const TCHAR* Title; const TCHAR* Description; bool bDestructive; };
		const FLane Lanes[] = {
			{ TEXT("read"), TEXT("Run Read Tool"),
				TEXT("Runs one read-only tool from unreal_tools_search by name, with the same permissions as calling it directly. Tools of the write and delete lanes are refused here, with the executor to use.\n")
				TEXT("Returns: the tool's own result."), false },
			{ TEXT("write"), TEXT("Run Write Tool"),
				TEXT("Runs one tool of the write lane by name: it changes the project, or SPENDS Creative Units, with the same permissions and money rules as calling it directly (price first, quote_id and approved_cost; paid calls are sent once). Read and delete tools are refused here.\n")
				TEXT("Returns: the tool's own result."), false },
			{ TEXT("delete"), TEXT("Run Delete Tool"),
				TEXT("Runs one destructive tool by name, such as remove_scenario_items or the kit's world, game and film tools, with the same permissions as calling it directly. Confirm with the person first. Read and write tools are refused here.\n")
				TEXT("Returns: the tool's own result."), true },
		};
		for (const FLane& Lane : Lanes)
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			Props->SetObjectField(TEXT("name"), Prop(TEXT("string"), TEXT("Tool name, as unreal_tools_list or unreal_tools_search gives it.")));
			Props->SetObjectField(TEXT("parameters"), Prop(TEXT("object"), TEXT("The tool's arguments, matching its inputSchema.")));
			const bool bRead = FCString::Strcmp(Lane.Lane, TEXT("read")) == 0;
			Tools.Add(MakeShared<FJsonValueObject>(CatalogTool(FString(ExecutePrefix) + Lane.Lane, Lane.Title, Lane.Description, Props, { TEXT("name") },
				Annotations(Lane.Title, bRead, Lane.bDestructive, bRead, !bRead))));
		}
		return Tools;
	}

	/** Up to the first sentence of a description, for unreal_tools_list. */
	FString Summary(const FString& Description)
	{
		FString First = Description;
		int32 Cut = INDEX_NONE;
		if (First.FindChar(TEXT('\n'), Cut))
		{
			First.LeftInline(Cut);
		}
		Cut = First.Find(TEXT(". "));
		return Cut == INDEX_NONE ? First : First.Left(Cut + 1);
	}

	TSharedRef<FJsonObject> ListedTools()
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FScenarioAction& Action : FScenarioActions::Get().All())
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Action.Name);
			Row->SetStringField(TEXT("title"), Action.Title);
			Row->SetStringField(TEXT("summary"), Summary(Action.Description));
			Row->SetStringField(TEXT("lane"), LaneOf(Action));
			Row->SetBoolField(TEXT("listed"), !Action.bCatalogOnly);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetArrayField(TEXT("tools"), Rows);
		return Json;
	}

	FScenarioActionResult SearchedTools(const TSharedPtr<FJsonObject>& Args)
	{
		TArray<FString> Words;
		ScenarioJson::GetString(Args, TEXT("query")).ToLower().ParseIntoArrayWS(Words);
		if (Words.Num() == 0)
		{
			return FScenarioActionResult::Fail(TEXT("BadRequest"), TEXT("query is empty: unreal_tools_list lists every tool."));
		}
		const int32 Limit = FMath::Clamp(static_cast<int32>(ScenarioJson::GetNumber(Args, TEXT("limit"), 8.0)), 1, 20);
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FScenarioAction& Action : FScenarioActions::Get().All())
		{
			const FString Haystack = (Action.Name + TEXT(" ") + Action.Title + TEXT(" ") + Action.Description).ToLower();
			bool bMatch = true;
			for (const FString& Word : Words)
			{
				bMatch &= Haystack.Contains(Word);
			}
			if (!bMatch)
			{
				continue;
			}
			TSharedRef<FJsonObject> Row = FScenarioMcpServer::ToolJson(Action);
			Row->SetStringField(TEXT("lane"), LaneOf(Action));
			Row->SetBoolField(TEXT("listed"), !Action.bCatalogOnly);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			if (Rows.Num() >= Limit)
			{
				break;
			}
		}
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("query"), ScenarioJson::GetString(Args, TEXT("query")));
		Json->SetArrayField(TEXT("results"), Rows);
		return FScenarioActionResult::Ok(Json);
	}
}

TSharedRef<FJsonObject> FScenarioMcpServer::ToolJson(const FScenarioAction& Action)
{
	TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
	Tool->SetStringField(TEXT("name"), Action.Name);
	if (!Action.Title.IsEmpty())
	{
		Tool->SetStringField(TEXT("title"), Action.Title);
	}
	Tool->SetStringField(TEXT("description"), Action.Description);
	Tool->SetObjectField(TEXT("inputSchema"), Action.InputSchema.IsValid() ? Action.InputSchema.ToSharedRef() : MakeShared<FJsonObject>());
	const bool bReadOnly = Action.Kind == EScenarioActionKind::Read;
	Tool->SetObjectField(TEXT("annotations"), Annotations(Action.Title, bReadOnly, !bReadOnly && Action.bDestructive, bReadOnly, Action.bOpenWorld));
	return Tool;
}

TSharedRef<FJsonObject> FScenarioMcpServer::ToolsList(bool bEveryTool)
{
	TArray<TSharedPtr<FJsonValue>> Tools;
	for (const FScenarioAction& Action : FScenarioActions::Get().All())
	{
		if (bEveryTool || !Action.bCatalogOnly)
		{
			Tools.Add(MakeShared<FJsonValueObject>(ToolJson(Action)));
		}
	}
	Tools.Append(CatalogTools());
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetArrayField(TEXT("tools"), Tools);
	return Json;
}

TSharedRef<FJsonObject> FScenarioMcpServer::Snapshot()
{
	TArray<TSharedPtr<FJsonValue>> Tools;
	for (const FScenarioAction& Action : FScenarioActions::Get().All())
	{
		TSharedRef<FJsonObject> Tool = ToolJson(Action);
		if (Action.bCatalogOnly)
		{
			TSharedRef<FJsonObject> Meta = MakeShared<FJsonObject>();
			Meta->SetStringField(ListingKey, TEXT("catalog"));
			Tool->SetObjectField(TEXT("_meta"), Meta);
		}
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}
	Tools.Append(CatalogTools());
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("instructions"), Instructions());
	Json->SetArrayField(TEXT("tools"), Tools);
	return Json;
}

FScenarioLocalResponse FScenarioMcpServer::RpcResult(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Result, const FString& SessionHeader)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Json->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
	Json->SetObjectField(TEXT("result"), Result);
	FScenarioLocalResponse Response = FScenarioLocalResponse::Json(200, ScenarioJson::Write(Json));
	if (!SessionHeader.IsEmpty())
	{
		Response.Headers.Add(TEXT("Mcp-Session-Id"), SessionHeader);
	}
	return Response;
}

FScenarioLocalResponse FScenarioMcpServer::RpcError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
{
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetNumberField(TEXT("code"), Code);
	Error->SetStringField(TEXT("message"), Message);
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Json->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
	Json->SetObjectField(TEXT("error"), Error);
	return FScenarioLocalResponse::Json(200, ScenarioJson::Write(Json));
}

void FScenarioMcpServer::Handle(const FScenarioLocalRequest& Request, FScenarioLocalRespond Respond)
{
	// DNS rebinding and web pages: exactly one loopback Host, and no foreign Origin.
	if (Request.HostCount != 1 || !FScenarioLocalHttpServer::IsLoopbackHostHeader(Request.Header(TEXT("host")), Port))
	{
		Respond(FScenarioLocalResponse::Text(403, TEXT("Forbidden: the Host header must name 127.0.0.1 or localhost.")));
		return;
	}
	if (!FScenarioLocalHttpServer::IsAllowedOrigin(Request.Header(TEXT("origin"))))
	{
		Respond(FScenarioLocalResponse::Text(403, TEXT("Forbidden: requests from web pages are refused.")));
		return;
	}
	if (Request.Method == TEXT("OPTIONS"))
	{
		Respond(FScenarioLocalResponse::Text(403, TEXT("Forbidden: cross-origin requests are refused.")));
		return;
	}
	if (Request.Path == TEXT("/health"))
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("status"), TEXT("ok"));
		Json->SetStringField(TEXT("server"), ServerName);
		Json->SetStringField(TEXT("version"), PluginVersion());
		Respond(FScenarioLocalResponse::Json(200, ScenarioJson::Write(Json)));
		return;
	}
	if (Request.Path != TEXT("/mcp"))
	{
		Respond(FScenarioLocalResponse::Text(404, TEXT("Not found. The MCP endpoint is /mcp.")));
		return;
	}
	if (bRequireToken && !TokenMatches(Request.Header(TEXT("authorization"))))
	{
		FScenarioLocalResponse Response = FScenarioLocalResponse::Json(401, TEXT("{\"error\":\"Missing or wrong bearer token. Use Connect in Scenario > Connection > Agents.\"}"));
		Response.Headers.Add(TEXT("WWW-Authenticate"), TEXT("Bearer"));
		Respond(Response);
		return;
	}
	const FString SessionId = Request.Header(TEXT("mcp-session-id"));
	if (Request.Method == TEXT("DELETE"))
	{
		Sessions.Remove(SessionId);
		OnClientsChanged.Broadcast();
		Respond(FScenarioLocalResponse::Text(204, FString()));
		return;
	}
	if (Request.Method != TEXT("POST"))
	{
		// No server-sent event stream: replies are plain JSON.
		Respond(FScenarioLocalResponse::Text(405, TEXT("Method not allowed. Send JSON-RPC with POST.")));
		return;
	}
	const TSharedPtr<FJsonValue> Parsed = ScenarioJson::ParseValue(Request.BodyText());
	if (!Parsed.IsValid())
	{
		Respond(RpcError(nullptr, -32700, TEXT("Parse error.")));
		return;
	}
	if (Parsed->Type != EJson::Object)
	{
		Respond(RpcError(nullptr, -32600, TEXT("Send one JSON-RPC message per request (batches are not supported).")));
		return;
	}
	const TSharedPtr<FJsonObject> Message = Parsed->AsObject();
	const FString Method = ScenarioJson::GetString(Message, TEXT("method"));
	if (!SessionId.IsEmpty() && Method != TEXT("initialize") && !Sessions.Contains(SessionId))
	{
		// The editor restarted: the client must initialize again.
		Respond(FScenarioLocalResponse::Text(404, TEXT("Unknown session. Initialize again.")));
		return;
	}
	if (FClient* Client = Sessions.Find(SessionId))
	{
		Client->LastSeen = ScenarioNow();
	}
	HandleRpc(Message, SessionId, Respond);
}

void FScenarioMcpServer::HandleRpc(const TSharedPtr<FJsonObject>& Message, const FString& SessionId, FScenarioLocalRespond Respond)
{
	const FString Method = ScenarioJson::GetString(Message, TEXT("method"));
	const TSharedPtr<FJsonValue> Id = Message->TryGetField(TEXT("id"));
	const TSharedPtr<FJsonObject> Params = ScenarioJson::GetObject(Message, TEXT("params"));

	if (!Id.IsValid() || Id->IsNull())
	{
		// Notifications (initialized, cancelled...) get no JSON-RPC reply.
		Respond(FScenarioLocalResponse::Text(202, FString()));
		return;
	}

	if (Method == TEXT("initialize"))
	{
		const FString Asked = ScenarioJson::GetString(Params, TEXT("protocolVersion"));
		FString Version = ProtocolVersions[0];
		for (const TCHAR* Supported : ProtocolVersions)
		{
			if (Asked == Supported)
			{
				Version = Supported;
			}
		}
		const TSharedPtr<FJsonObject> Info = ScenarioJson::GetObject(Params, TEXT("clientInfo"));
		FClient Client;
		Client.Name = ScenarioJson::GetString(Info, TEXT("name"), TEXT("agent")).Left(64);
		Client.Version = ScenarioJson::GetString(Info, TEXT("version")).Left(32);
		Client.LastSeen = ScenarioNow();
		const FString NewSession = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
		Sessions.Add(NewSession, Client);
		OnClientsChanged.Broadcast();

		TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Tools = MakeShared<FJsonObject>();
		Tools->SetBoolField(TEXT("listChanged"), false);
		Capabilities->SetObjectField(TEXT("tools"), Tools);
		TSharedRef<FJsonObject> Server = MakeShared<FJsonObject>();
		Server->SetStringField(TEXT("name"), ServerName);
		Server->SetStringField(TEXT("title"), TEXT("Scenario for Unreal Engine"));
		Server->SetStringField(TEXT("version"), PluginVersion());
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("protocolVersion"), Version);
		Result->SetObjectField(TEXT("capabilities"), Capabilities);
		Result->SetObjectField(TEXT("serverInfo"), Server);
		Result->SetStringField(TEXT("instructions"), Instructions());
		Respond(RpcResult(Id, Result, NewSession));
		return;
	}
	if (Method == TEXT("ping"))
	{
		Respond(RpcResult(Id, MakeShared<FJsonObject>()));
		return;
	}
	if (Method == TEXT("tools/list"))
	{
		Respond(RpcResult(Id, ToolsList(bListEveryTool)));
		return;
	}
	if (Method != TEXT("tools/call"))
	{
		Respond(RpcError(Id, -32601, FString::Printf(TEXT("Method not found: %s."), *Method.Left(64))));
		return;
	}

	FString Name = ScenarioJson::GetString(Params, TEXT("name"));
	TSharedPtr<FJsonObject> Arguments = ScenarioJson::GetObject(Params, TEXT("arguments"));
	// The catalog answers whatever tools/list shows, so a client keeps working when the setting changes.
	if (Name == ToolsListName)
	{
		Respond(RpcResult(Id, ToolResult(FScenarioActionResult::Ok(ListedTools()))));
		return;
	}
	if (Name == ToolsSearchName)
	{
		Respond(RpcResult(Id, ToolResult(SearchedTools(Arguments))));
		return;
	}
	if (Name.StartsWith(ExecutePrefix))
	{
		const FString Lane = Name.RightChop(FCString::Strlen(ExecutePrefix));
		Name = ScenarioJson::GetString(Arguments, TEXT("name"));
		Arguments = ScenarioJson::GetObject(Arguments, TEXT("parameters"));
		const FScenarioAction* Target = FScenarioActions::Get().Find(Name);
		if (!Target)
		{
			Respond(RpcResult(Id, ToolResult(FScenarioActionResult::Fail(TEXT("NotFound"), FString::Printf(TEXT("No tool named %s: unreal_tools_search finds them."), *Name.Left(64))))));
			return;
		}
		if (LaneOf(*Target) != Lane)
		{
			Respond(RpcResult(Id, ToolResult(FScenarioActionResult::Fail(TEXT("WrongLane"),
				FString::Printf(TEXT("%s is a %s tool: run it with unreal_tool_execute_%s."), *Name, *LaneOf(*Target), *LaneOf(*Target))))));
			return;
		}
	}
	const FScenarioAction* Action = FScenarioActions::Get().Find(Name);
	if (!Action)
	{
		Respond(RpcError(Id, -32602, FString::Printf(TEXT("Unknown tool: %s."), *Name.Left(64))));
		return;
	}
	const FClient* Client = Sessions.Find(SessionId);
	const FScenarioCaller Caller = FScenarioCaller::Agent(Client ? Client->Name : TEXT("agent"));

	// One reply: the action's, or the time limit's.
	TSharedRef<bool> bReplied = MakeShared<bool>(false);
	TSharedRef<FTSTicker::FDelegateHandle> Timer = MakeShared<FTSTicker::FDelegateHandle>();
	const double Limit = Action->TimeLimit;
	*Timer = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([bReplied, Id, Name, Limit, Respond](float)
	{
		if (!*bReplied)
		{
			*bReplied = true;
			Respond(RpcResult(Id, ToolResult(FScenarioActionResult::Fail(TEXT("Timeout"),
				FString::Printf(TEXT("%s took longer than %.0f s. It may still finish: check job_status or list_local_jobs before trying again."), *Name, Limit)))));
		}
		return false;
	}), static_cast<float>(Limit));

	FScenarioActions::Get().Call(Name, Arguments, Caller, [bReplied, Timer, Id, Respond](const FScenarioActionResult& Result)
	{
		FTSTicker::GetCoreTicker().RemoveTicker(*Timer);
		if (*bReplied)
		{
			return;
		}
		*bReplied = true;
		Respond(RpcResult(Id, ToolResult(Result)));
	});
}
