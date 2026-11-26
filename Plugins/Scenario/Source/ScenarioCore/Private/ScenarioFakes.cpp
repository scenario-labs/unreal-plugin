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

FString FScenarioFakeRequest::Path() const
{
	FString Left, Right;
	FString Base = Url.Split(TEXT("?"), &Left, &Right) ? Left : Url;
	const int32 Scheme = Base.Find(TEXT("://"));
	if (Scheme != INDEX_NONE)
	{
		const int32 Slash = Base.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Scheme + 3);
		Base = Slash == INDEX_NONE ? TEXT("/") : Base.Mid(Slash);
	}
	return Base;
}

FString FScenarioFakeRequest::Query(const FString& Name) const
{
	FString Left, Right;
	if (!Url.Split(TEXT("?"), &Left, &Right))
	{
		return FString();
	}
	return ScenarioUrl::ParseQuery(Right).FindRef(Name);
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

void FScenarioFakeTransport::EnqueueTransportError(EScenarioErrorKind Kind)
{
	FScenarioHttpResponse Response;
	Response.TransportError = Kind;
	Queue.Add(Response);
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

bool FScenarioFakeBackend::Matches(const FString& Route, const FScenarioRequest& Request)
{
	FString Verb, Path;
	if (!Route.Split(TEXT(" "), &Verb, &Path) || Verb != Request.Verb)
	{
		return false;
	}
	TArray<FString> Want, Have;
	Path.ParseIntoArray(Want, TEXT("/"));
	Request.Path.ParseIntoArray(Have, TEXT("/"));
	if (Want.Num() != Have.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Want.Num(); ++Index)
	{
		if (Want[Index] != TEXT("*") && Want[Index] != ScenarioUrl::Decode(Have[Index]))
		{
			return false;
		}
	}
	return true;
}

void FScenarioFakeBackend::Send(const FScenarioRequest& Request, FScenarioReplyCallback OnDone)
{
	FScenarioRequest Seen = Request;
	Seen.Body = Request.Body.IsValid() ? ScenarioJson::CloneObject(Request.Body) : nullptr;
	Calls.Add(Seen);
	if (!bSignedIn)
	{
		FScenarioReply Reply = FScenarioReply::Fail(EScenarioErrorKind::Auth, TEXT("Sign in to Scenario first."));
		Reply.Error.bNotSent = true;
		OnDone(Reply);
		return;
	}
	const FString Exact = Request.Verb + TEXT(" ") + Request.Path;
	if (const FHandler* Handler = Handlers.Find(Exact))
	{
		OnDone((*Handler)(Request));
		return;
	}
	for (const TPair<FString, FHandler>& Pair : Handlers)
	{
		if (Matches(Pair.Key, Request))
		{
			OnDone(Pair.Value(Request));
			return;
		}
	}
	OnDone(FScenarioReply::Fail(EScenarioErrorKind::NotFound, FString::Printf(TEXT("The fake backend has no handler for %s."), *Exact)));
}

void FScenarioFakeBackend::OnJson(const FString& Route, const FString& Json)
{
	On(Route, [Json](const FScenarioRequest&)
	{
		const TSharedPtr<FJsonObject> Value = ScenarioJson::ParseObject(Json);
		return Value.IsValid() ? FScenarioReply::Ok(Value) : FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Bad fixture JSON."));
	});
}

int32 FScenarioFakeBackend::CountOf(const FString& Route) const
{
	int32 Count = 0;
	for (const FScenarioRequest& Call : Calls)
	{
		Count += Matches(Route, Call) ? 1 : 0;
	}
	return Count;
}

const FScenarioRequest* FScenarioFakeBackend::Last(const FString& Route) const
{
	for (int32 Index = Calls.Num() - 1; Index >= 0; --Index)
	{
		if (Matches(Route, Calls[Index]))
		{
			return &Calls[Index];
		}
	}
	return nullptr;
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
