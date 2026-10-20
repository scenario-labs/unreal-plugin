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
			TEXT("Never resend a request whose job is unknown: use recover_local_job.");
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

TSharedRef<FJsonObject> FScenarioMcpServer::ToolJson(const FScenarioAction& Action)
{
	TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
	Tool->SetStringField(TEXT("name"), Action.Name);
	Tool->SetStringField(TEXT("description"), Action.Description);
	Tool->SetObjectField(TEXT("inputSchema"), Action.InputSchema.IsValid() ? Action.InputSchema.ToSharedRef() : MakeShared<FJsonObject>());
	TSharedRef<FJsonObject> Annotations = MakeShared<FJsonObject>();
	const bool bReadOnly = Action.Kind == EScenarioActionKind::Read;
	Annotations->SetBoolField(TEXT("readOnlyHint"), bReadOnly);
	if (!bReadOnly)
	{
		Annotations->SetBoolField(TEXT("destructiveHint"), Action.bDestructive);
	}
	if (Action.Kind == EScenarioActionKind::Spend)
	{
		Annotations->SetBoolField(TEXT("openWorldHint"), true);
	}
	Tool->SetObjectField(TEXT("annotations"), Annotations);
	return Tool;
}

TSharedRef<FJsonObject> FScenarioMcpServer::ToolsList()
{
	TArray<TSharedPtr<FJsonValue>> Tools;
	for (const FScenarioAction& Action : FScenarioActions::Get().All())
	{
		Tools.Add(MakeShared<FJsonValueObject>(ToolJson(Action)));
	}
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetArrayField(TEXT("tools"), Tools);
	return Json;
}

namespace
{
	const TCHAR* FindTools = TEXT("find_tools");
	const TCHAR* DescribeTools = TEXT("describe_tools");
	const TCHAR* CallTool = TEXT("call_tool");

	TSharedRef<FJsonObject> MetaTool(const TCHAR* Name, const TCHAR* Description, const TSharedRef<FJsonObject>& Properties, const TArray<FString>& Required, bool bReadOnly)
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
		Tool->SetStringField(TEXT("description"), Description);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		TSharedRef<FJsonObject> Annotations = MakeShared<FJsonObject>();
		Annotations->SetBoolField(TEXT("readOnlyHint"), bReadOnly);
		if (!bReadOnly)
		{
			// call_tool reaches every tool, paid and destructive ones included.
			Annotations->SetBoolField(TEXT("destructiveHint"), true);
			Annotations->SetBoolField(TEXT("openWorldHint"), true);
		}
		Tool->SetObjectField(TEXT("annotations"), Annotations);
		return Tool;
	}

	TSharedRef<FJsonObject> Prop(const TCHAR* Type, const TCHAR* Description)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("type"), Type);
		Json->SetStringField(TEXT("description"), Description);
		return Json;
	}

	const TCHAR* KindName(EScenarioActionKind Kind)
	{
		switch (Kind)
		{
		case EScenarioActionKind::Read: return TEXT("read");
		case EScenarioActionKind::Editor: return TEXT("editor");
		case EScenarioActionKind::Spend: return TEXT("spend");
		case EScenarioActionKind::Change: return TEXT("change");
		default: return TEXT("python");
		}
	}

	/** find_tools: names and descriptions, matched on every word of the query. */
	TSharedRef<FJsonObject> FoundTools(const TSharedPtr<FJsonObject>& Args)
	{
		TArray<FString> Words;
		ScenarioJson::GetString(Args, TEXT("query")).ToLower().ParseIntoArrayWS(Words);
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FScenarioAction& Action : FScenarioActions::Get().All())
		{
			const FString Haystack = (Action.Name + TEXT(" ") + Action.Description).ToLower();
			bool bMatch = true;
			for (const FString& Word : Words)
			{
				bMatch &= Haystack.Contains(Word);
			}
			if (!bMatch)
			{
				continue;
			}
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Action.Name);
			Row->SetStringField(TEXT("kind"), KindName(Action.Kind));
			Row->SetStringField(TEXT("description"), Action.Description);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetArrayField(TEXT("tools"), Rows);
		Json->SetStringField(TEXT("next"), TEXT("describe_tools for the input schemas, then call_tool (or the tool by its own name)."));
		return Json;
	}

	/** describe_tools: the full tools/list entries for the names asked. */
	FScenarioActionResult DescribedTools(const TSharedPtr<FJsonObject>& Args)
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		TArray<FString> Unknown;
		for (const FString& Name : ScenarioJson::GetStringArray(Args, TEXT("names")))
		{
			if (const FScenarioAction* Action = FScenarioActions::Get().Find(Name))
			{
				Tools.Add(MakeShared<FJsonValueObject>(FScenarioMcpServer::ToolJson(*Action)));
			}
			else
			{
				Unknown.Add(Name.Left(64));
			}
		}
		if (Tools.Num() == 0)
		{
			return FScenarioActionResult::Fail(TEXT("NotFound"), TEXT("No tool with these names: find_tools lists them."));
		}
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetArrayField(TEXT("tools"), Tools);
		if (Unknown.Num() > 0)
		{
			Json->SetField(TEXT("unknown"), ScenarioJson::StringArrayValue(Unknown));
		}
		return FScenarioActionResult::Ok(Json);
	}
}

TSharedRef<FJsonObject> FScenarioMcpServer::ToolSearchList()
{
	TArray<TSharedPtr<FJsonValue>> Tools;
	if (const FScenarioAction* Status = FScenarioActions::Get().Find(TEXT("scenario_status")))
	{
		Tools.Add(MakeShared<FJsonValueObject>(ToolJson(*Status)));
	}

	TSharedRef<FJsonObject> FindProps = MakeShared<FJsonObject>();
	FindProps->SetObjectField(TEXT("query"), Prop(TEXT("string"), TEXT("Words matched against tool names and descriptions, for example material or job. Empty lists every tool.")));
	Tools.Add(MakeShared<FJsonValueObject>(MetaTool(FindTools,
		TEXT("Finds Scenario tools: name, kind (read, editor, spend, change, python) and description. Free."), FindProps, {}, true)));

	TSharedRef<FJsonObject> NamesProp = Prop(TEXT("array"), TEXT("Tool names from find_tools."));
	TSharedRef<FJsonObject> Items = MakeShared<FJsonObject>();
	Items->SetStringField(TEXT("type"), TEXT("string"));
	NamesProp->SetObjectField(TEXT("items"), Items);
	TSharedRef<FJsonObject> DescribeProps = MakeShared<FJsonObject>();
	DescribeProps->SetObjectField(TEXT("names"), NamesProp);
	Tools.Add(MakeShared<FJsonValueObject>(MetaTool(DescribeTools,
		TEXT("Input schemas and annotations of Scenario tools, as tools/list gives them. Free."), DescribeProps, { TEXT("names") }, true)));

	TSharedRef<FJsonObject> CallProps = MakeShared<FJsonObject>();
	CallProps->SetObjectField(TEXT("name"), Prop(TEXT("string"), TEXT("Tool name.")));
	CallProps->SetObjectField(TEXT("arguments"), Prop(TEXT("object"), TEXT("The tool's arguments.")));
	Tools.Add(MakeShared<FJsonValueObject>(MetaTool(CallTool,
		TEXT("Runs one Scenario tool with the same permissions and money rules as calling it by name. Tools of kind spend SPEND Creative Units: price first, pass quote_id and approved_cost."),
		CallProps, { TEXT("name") }, false)));

	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
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
		Respond(RpcResult(Id, bToolSearch ? ToolSearchList() : ToolsList()));
		return;
	}
	if (Method != TEXT("tools/call"))
	{
		Respond(RpcError(Id, -32601, FString::Printf(TEXT("Method not found: %s."), *Method.Left(64))));
		return;
	}

	FString Name = ScenarioJson::GetString(Params, TEXT("name"));
	TSharedPtr<FJsonObject> Arguments = ScenarioJson::GetObject(Params, TEXT("arguments"));
	// The search tools answer in every mode, so a client keeps working when the setting changes.
	if (Name == FindTools)
	{
		Respond(RpcResult(Id, ToolResult(FScenarioActionResult::Ok(FoundTools(Arguments)))));
		return;
	}
	if (Name == DescribeTools)
	{
		Respond(RpcResult(Id, ToolResult(DescribedTools(Arguments))));
		return;
	}
	if (Name == CallTool)
	{
		Name = ScenarioJson::GetString(Arguments, TEXT("name"));
		Arguments = ScenarioJson::GetObject(Arguments, TEXT("arguments"));
		if (!FScenarioActions::Get().Find(Name))
		{
			Respond(RpcResult(Id, ToolResult(FScenarioActionResult::Fail(TEXT("NotFound"), FString::Printf(TEXT("No tool named %s: find_tools lists them."), *Name.Left(64))))));
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
