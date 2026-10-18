// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioTelemetry.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "ScenarioCoreModule.h"
#include "ScenarioJson.h"

#include <atomic>

namespace
{
	std::atomic<bool> GEnabled{ false };

	/** The only fields an event may carry. Values are short enums or version strings. */
	const TCHAR* const AllowedFields[] = { TEXT("lane"), TEXT("kind"), TEXT("state"), TEXT("source"), TEXT("client") };
}

void ScenarioTelemetry::SetEnabled(bool bEnabled)
{
	GEnabled = bEnabled;
}

bool ScenarioTelemetry::IsEnabled()
{
	return GEnabled;
}

FString ScenarioTelemetry::Endpoint()
{
	// No collection endpoint exists yet. Before one is set here, docs/PRIVACY.md lists every field.
	return FString();
}

TSharedRef<FJsonObject> ScenarioTelemetry::BuildEvent(const FString& Name, const TMap<FString, FString>& Fields)
{
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("event"), Name.Left(64));
	Body->SetStringField(TEXT("plugin"), ScenarioVersion::PluginVersion());
	Body->SetStringField(TEXT("engine"), ScenarioVersion::EngineVersion());
	Body->SetStringField(TEXT("os"), ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()));
	for (const TCHAR* Key : AllowedFields)
	{
		if (const FString* Value = Fields.Find(Key))
		{
			Body->SetStringField(Key, Value->Left(64));
		}
	}
	return Body;
}

void ScenarioTelemetry::Event(const FString& Name, const TMap<FString, FString>& Fields)
{
	const FString Url = Endpoint();
	if (!IsEnabled() || Url.IsEmpty() || !FHttpModule::Get().IsHttpEnabled())
	{
		return;
	}
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("User-Agent"), ScenarioVersion::UserAgent());
	Request->SetContentAsString(ScenarioJson::Write(BuildEvent(Name, Fields)));
	Request->SetTimeout(10.0f);
	Request->ProcessRequest();
}
