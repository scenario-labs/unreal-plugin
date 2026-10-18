// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioFakes.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScenarioJson.h"

namespace
{
	TArray<uint8> Utf8Bytes(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}
}

FString FScenarioFakeRequest::BodyText() const
{
	const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Body.Num());
	return FString(Text.Length(), Text.Get());
}

TSharedPtr<FJsonObject> FScenarioFakeRequest::BodyJson() const
{
	return ScenarioJson::ParseObject(BodyText());
}

FString FScenarioFakeRequest::RpcMethod() const
{
	return ScenarioJson::GetString(BodyJson(), TEXT("method"));
}

FString FScenarioFakeRequest::ToolName() const
{
	const TSharedPtr<FJsonObject> Json = BodyJson();
	if (ScenarioJson::GetString(Json, TEXT("method")) != TEXT("tools/call"))
	{
		return FString();
	}
	return ScenarioJson::GetString(ScenarioJson::GetObject(Json, TEXT("params")), TEXT("name"));
}

TSharedPtr<FJsonObject> FScenarioFakeRequest::ToolArguments() const
{
	return ScenarioJson::GetObject(ScenarioJson::GetObject(BodyJson(), TEXT("params")), TEXT("arguments"));
}

void FScenarioFakeTransport::Send(const FString& Verb, const FString& Url, const TMap<FString, FString>& Headers, const TArray<uint8>& Body,
	float TimeoutSeconds, FScenarioHttpCallback OnDone, FScenarioProgressCallback OnProgress)
{
	FScenarioFakeRequest Request;
	Request.Verb = Verb;
	Request.Url = Url;
	Request.Headers = Headers;
	Request.Body = Body;
	Request.Timeout = TimeoutSeconds;
	Requests.Add(Request);

	FScenarioHttpResponse Response;
	if (Queue.Num() > 0)
	{
		Response = Queue[0];
		Queue.RemoveAt(0);
	}
	else if (Responder)
	{
		Response = Responder(Request);
	}
	else
	{
		Response.TransportError = EScenarioErrorKind::Transport;
	}
	if (OnProgress)
	{
		OnProgress(Body.Num(), Response.Body.Num());
	}
	OnDone(Response);
}

void FScenarioFakeTransport::Delay(float Seconds, TFunction<void()> OnDone)
{
	Delays.Add(Seconds);
	OnDone();
}

void FScenarioFakeTransport::EnqueueJson(int32 Status, const FString& Body, const TMap<FString, FString>& Headers)
{
	Queue.Add(Json(Status, Body, Headers));
}

void FScenarioFakeTransport::EnqueueSse(int32 Status, const FString& Body, const TMap<FString, FString>& Headers)
{
	FScenarioHttpResponse Response = Json(Status, Body, Headers);
	Response.Headers.Add(TEXT("content-type"), TEXT("text/event-stream"));
	Queue.Add(Response);
}

void FScenarioFakeTransport::EnqueueTransportError(EScenarioErrorKind Kind)
{
	FScenarioHttpResponse Response;
	Response.TransportError = Kind;
	Queue.Add(Response);
}

TArray<FScenarioFakeRequest> FScenarioFakeTransport::ToolCalls() const
{
	return Requests.FilterByPredicate([](const FScenarioFakeRequest& Request) { return !Request.ToolName().IsEmpty(); });
}

FScenarioHttpResponse FScenarioFakeTransport::Json(int32 Status, const FString& Body, const TMap<FString, FString>& Headers)
{
	FScenarioHttpResponse Response;
	Response.Status = Status;
	Response.Body = Utf8Bytes(Body);
	Response.Headers.Add(TEXT("content-type"), TEXT("application/json"));
	for (const TPair<FString, FString>& Header : Headers)
	{
		Response.Headers.Add(Header.Key.ToLower(), Header.Value);
	}
	return Response;
}

FString FScenarioFakeTransport::ToolResult(int64 Id, const FString& StructuredJson, bool bIsError)
{
	return FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"content\":[{\"type\":\"text\",\"text\":%s}],\"structuredContent\":%s,\"isError\":%s}}"),
		Id, *ScenarioJson::EscapeString(StructuredJson), *StructuredJson, bIsError ? TEXT("true") : TEXT("false"));
}

FString FScenarioFakeTransport::InitializeResult(int64 Id)
{
	return FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"protocolVersion\":\"2025-03-26\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"fake\",\"version\":\"0\"}}}"), Id);
}

void FScenarioFakeMcpServer::On(const FString& Tool, TFunction<FString(const TSharedPtr<FJsonObject>& Arguments)> Handler)
{
	Handlers.Add(Tool, [Handler](const TSharedPtr<FJsonObject>& Arguments, int64 RequestId)
	{
		return FScenarioFakeTransport::Json(200, FScenarioFakeTransport::ToolResult(RequestId, Handler(Arguments)));
	});
}

void FScenarioFakeMcpServer::OnRaw(const FString& Tool, FToolHandler Handler)
{
	Handlers.Add(Tool, MoveTemp(Handler));
}

FScenarioHttpResponse FScenarioFakeMcpServer::Handle(const FScenarioFakeRequest& Request)
{
	const TSharedPtr<FJsonObject> Json = Request.BodyJson();
	const FString Method = ScenarioJson::GetString(Json, TEXT("method"));
	const int64 Id = static_cast<int64>(ScenarioJson::GetNumber(Json, TEXT("id"), -1.0));
	if (Method == TEXT("initialize"))
	{
		return FScenarioFakeTransport::Json(200, FScenarioFakeTransport::InitializeResult(Id), { { TEXT("mcp-session-id"), SessionId } });
	}
	if (Method == TEXT("notifications/initialized"))
	{
		FScenarioHttpResponse Accepted;
		Accepted.Status = 202;
		return Accepted;
	}
	if (Method != TEXT("tools/call"))
	{
		return FScenarioFakeTransport::Json(200, FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"id\":%lld,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}"), Id));
	}
	const FString Tool = Request.ToolName();
	CallCounts.FindOrAdd(Tool)++;
	if (const FToolHandler* Handler = Handlers.Find(Tool))
	{
		return (*Handler)(Request.ToolArguments(), Id);
	}
	return FScenarioFakeTransport::Json(200, FScenarioFakeTransport::ToolResult(Id, FString::Printf(TEXT("{\"error\":\"Unknown tool %s\"}"), *Tool), true));
}

void FScenarioFakeMcpServer::Attach(FScenarioFakeTransport& Transport)
{
	TWeakPtr<FScenarioFakeMcpServer> WeakSelf = AsShared();
	Transport.Responder = [WeakSelf](const FScenarioFakeRequest& Request)
	{
		if (const TSharedPtr<FScenarioFakeMcpServer> Self = WeakSelf.Pin())
		{
			return Self->Handle(Request);
		}
		FScenarioHttpResponse Gone;
		Gone.TransportError = EScenarioErrorKind::Transport;
		return Gone;
	};
}

void FScenarioFakeBackend::CallTool(const FString& Tool, const TSharedPtr<FJsonObject>& Arguments, FScenarioReplyCallback OnDone, float TimeoutSeconds)
{
	Calls.Add({ Tool, Arguments.IsValid() ? ScenarioJson::CloneObject(Arguments) : MakeShared<FJsonObject>() });
	if (!bSignedIn)
	{
		FScenarioReply Reply = FScenarioReply::Fail(EScenarioErrorKind::Auth, TEXT("Sign in to Scenario first."));
		Reply.Error.bNotSent = true;
		OnDone(Reply);
		return;
	}
	if (const FHandler* Handler = Handlers.Find(Tool))
	{
		OnDone((*Handler)(Arguments));
		return;
	}
	OnDone(FScenarioReply::Fail(EScenarioErrorKind::NotFound, FString::Printf(TEXT("The fake backend has no handler for %s."), *Tool)));
}

void FScenarioFakeBackend::OnJson(const FString& Tool, const FString& Json)
{
	On(Tool, [Json](const TSharedPtr<FJsonObject>&)
	{
		const TSharedPtr<FJsonObject> Value = ScenarioJson::ParseObject(Json);
		return Value.IsValid() ? FScenarioReply::Ok(Value) : FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Bad fixture JSON."));
	});
}

int32 FScenarioFakeBackend::CountOf(const FString& Tool) const
{
	int32 Count = 0;
	for (const TPair<FString, TSharedPtr<FJsonObject>>& Call : Calls)
	{
		Count += Call.Key == Tool ? 1 : 0;
	}
	return Count;
}

FString ScenarioFixtures::Dir()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scenario"));
	if (!Plugin.IsValid())
	{
		return FString();
	}
	const FString Folder = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tests"), TEXT("Fixtures"));
	return FPaths::DirectoryExists(Folder) ? Folder : FString();
}

bool ScenarioFixtures::Load(const FString& Name, FString& OutText)
{
	const FString Folder = Dir();
	return !Folder.IsEmpty() && FFileHelper::LoadFileToString(OutText, *FPaths::Combine(Folder, Name));
}

TSharedPtr<FJsonObject> ScenarioFixtures::LoadJson(const FString& Name)
{
	FString Text;
	return Load(Name, Text) ? ScenarioJson::ParseObject(Text) : nullptr;
}
