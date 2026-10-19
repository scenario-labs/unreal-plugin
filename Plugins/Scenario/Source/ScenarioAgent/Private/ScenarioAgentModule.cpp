// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioAgentModule.h"

#include "ScenarioAgentClients.h"

#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "ScenarioActions.h"
#include "ScenarioCredentialStore.h"
#include "ScenarioEditorModule.h"
#include "ScenarioJson.h"
#include "ScenarioMcpServer.h"
#include "ScenarioWidgets.h"
#include "ScenarioSettings.h"
#include "ScenarioTypes.h"
#include "SScenarioAgentsSection.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioAgent"

IMPLEMENT_MODULE(FScenarioAgentModule, ScenarioAgent)

DEFINE_LOG_CATEGORY_STATIC(LogScenarioAgent, Log, All);

namespace
{
	const TCHAR* TokenAccount = TEXT("agent-token");

	FString NewToken()
	{
		// Two random GUIDs: 244 random bits from the OS generator.
		return FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower() + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	}
}

FScenarioAgentModule& FScenarioAgentModule::Get()
{
	return FModuleManager::LoadModuleChecked<FScenarioAgentModule>(TEXT("ScenarioAgent"));
}

bool FScenarioAgentModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(TEXT("ScenarioAgent"));
}

void FScenarioAgentModule::StartupModule()
{
	ServerPtr = MakeShared<FScenarioMcpServer>();
	CalledHandle = FScenarioActions::Get().OnCalled.AddRaw(this, &FScenarioAgentModule::OnActionCalled);

	FScenarioActions::Get().StatusExtension = [this](const TSharedRef<FJsonObject>& Status)
	{
		TSharedRef<FJsonObject> Server = MakeShared<FJsonObject>();
		Server->SetBoolField(TEXT("running"), ServerPtr.IsValid() && ServerPtr->IsRunning());
		if (ServerPtr.IsValid() && ServerPtr->IsRunning())
		{
			Server->SetStringField(TEXT("url"), ServerPtr->Url());
		}
		Server->SetBoolField(TEXT("token_required"), UScenarioEditorSettings::Get().bAgentToken);
		Status->SetObjectField(TEXT("agent_server"), Server);
	};

	FScenarioEditorModule::RegisterConnectionSection(TEXT("Agents"), []() -> TSharedRef<SWidget>
	{
		return SNew(SScenarioAgentsSection);
	});
	FScenarioEditorModule::SetHeaderStatus([]() -> TSharedRef<SWidget>
	{
		// 2: an agent was active in the last 10 minutes, 1: the server is on, 0: off.
		auto Status = [](FText* OutTooltip) -> int32
		{
			const TSharedPtr<FScenarioMcpServer> Server = IsAvailable() ? Get().Server() : nullptr;
			if (!Server.IsValid() || !Server->IsRunning())
			{
				if (OutTooltip)
				{
					*OutTooltip = LOCTEXT("AgentsOffTip", "Scenario MCP is off: agents cannot reach this editor. Click to turn it on.");
				}
				return 0;
			}
			// Apps, not sessions: one agent opens several sessions (health checks, restarts).
			TSet<FString> Recent;
			for (const TPair<FString, FScenarioMcpServer::FClient>& Pair : Server->Clients())
			{
				if (ScenarioNow() - Pair.Value.LastSeen < 600.0)
				{
					FString Label = Pair.Value.Name;
					for (const ScenarioAgentClients::FClient& Known : ScenarioAgentClients::All())
					{
						if (Pair.Value.Name.Contains(Known.ClientInfoName))
						{
							Label = Known.Name.ToString();
							break;
						}
					}
					Recent.Add(Label);
				}
			}
			if (OutTooltip)
			{
				*OutTooltip = Recent.Num() > 0
					? FText::Format(LOCTEXT("AgentsActiveTip", "{0} active in the last 10 minutes. Click for setup and the agent log."), FText::FromString(FString::Join(Recent.Array(), TEXT(", "))))
					: LOCTEXT("AgentsIdleTip", "Scenario MCP is on, no agent active in the last 10 minutes. Click for setup and the agent log.");
			}
			return Recent.Num() > 0 ? 2 : 1;
		};
		return SNew(SButton)
			.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>("Scenario.Button.Ghost.Small"))
			.ContentPadding(FMargin(6.0f))
			.ToolTipText_Lambda([Status]() { FText Tip; Status(&Tip); return Tip; })
			.OnClicked_Lambda([]() { FScenarioEditorModule::OpenStudio(ScenarioPages::Agent); return FReply::Handled(); })
			[
				SNew(SImage)
				.Image(ScenarioUI::Style().GetBrush("Scenario.Dot"))
				.DesiredSizeOverride(FVector2D(8.0f, 8.0f))
				.ColorAndOpacity_Lambda([Status]()
				{
					switch (Status(nullptr))
					{
					case 2: return FSlateColor(ScenarioUI::Palette::Success());
					case 1: return FSlateColor(ScenarioUI::Palette::Success().CopyWithNewOpacity(0.55f));
					default: return FSlateColor(ScenarioUI::Palette::White(0.22f));
					}
				})
			];
	});

	if (UScenarioEditorSettings::Get().bAgentServer)
	{
		FString Error;
		if (!StartServer(Error))
		{
			UE_LOG(LogScenarioAgent, Warning, TEXT("Scenario MCP server not started: %s"), *Error);
		}
	}
}

void FScenarioAgentModule::ShutdownModule()
{
	StopServer();
	if (FModuleManager::Get().IsModuleLoaded(TEXT("ScenarioEditor")))
	{
		FScenarioEditorModule::UnregisterConnectionSection(TEXT("Agents"));
		FScenarioEditorModule::SetHeaderStatus(nullptr);
	}
	FScenarioActions::Get().OnCalled.Remove(CalledHandle);
	FScenarioActions::Get().StatusExtension = nullptr;
	ServerPtr.Reset();
}

bool FScenarioAgentModule::StartServer(FString& OutError)
{
	const UScenarioEditorSettings& Settings = UScenarioEditorSettings::Get();
	ServerPtr->bRequireToken = Settings.bAgentToken;
	ServerPtr->Token = Settings.bAgentToken ? Token() : FString();
	if (Settings.bAgentToken && ServerPtr->Token.IsEmpty())
	{
		OutError = TEXT("The token could not be stored in the credential store.");
		return false;
	}
	if (!ServerPtr->Start(Settings.AgentPort, OutError))
	{
		return false;
	}
	UE_LOG(LogScenarioAgent, Log, TEXT("Scenario MCP server listening on %s (token %s)."), *ServerPtr->Url(), Settings.bAgentToken ? TEXT("required") : TEXT("off"));
	return true;
}

void FScenarioAgentModule::StopServer()
{
	if (ServerPtr.IsValid())
	{
		ServerPtr->Stop();
	}
}

FString FScenarioAgentModule::Token()
{
	if (!CachedToken.IsEmpty())
	{
		return CachedToken;
	}
	const TSharedRef<IScenarioCredentialStore> Store = ScenarioCredentials::CreatePlatformStore();
	FString Value;
	if (!Store->Read(TokenAccount, Value) || Value.Len() < 32)
	{
		Value = NewToken();
		if (!Store->Write(TokenAccount, Value))
		{
			return FString();
		}
	}
	CachedToken = Value;
	return CachedToken;
}

void FScenarioAgentModule::RotateToken()
{
	const TSharedRef<IScenarioCredentialStore> Store = ScenarioCredentials::CreatePlatformStore();
	const FString Value = NewToken();
	if (Store->Write(TokenAccount, Value))
	{
		CachedToken = Value;
		if (ServerPtr.IsValid())
		{
			ServerPtr->Token = Value;
		}
	}
}

void FScenarioAgentModule::OnActionCalled(const FString& Name, const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, const FScenarioActionResult& Result, double Seconds)
{
	if (Caller.Source != TEXT("agent"))
	{
		return;
	}
	FScenarioAgentActivity Entry;
	Entry.Time = ScenarioNow();
	Entry.Client = Caller.Client;
	Entry.Tool = Name;
	Entry.bOk = Result.bOk;
	Entry.Error = Result.Error.Left(300);
	Entry.Spent = Result.bOk ? Result.SpentExact : FString();
	Entry.Seconds = Seconds;
	Log.Add(Entry);
	if (Log.Num() > 200)
	{
		Log.RemoveAt(0, Log.Num() - 200);
	}
	if (!Entry.Spent.IsEmpty())
	{
		const FString Model = ScenarioJson::GetString(Result.Data, TEXT("model_id"), ScenarioJson::GetString(Result.Data, TEXT("workflow_id")));
		FScenarioEditorModule::Toast(
			FText::Format(LOCTEXT("AgentSpent", "{0} spent {1} CU"), FText::FromString(Caller.Client.IsEmpty() ? TEXT("An agent") : Caller.Client), FText::FromString(Entry.Spent)),
			FText::FromString(FString::Printf(TEXT("%s %s"), *Name, *Model)), true);
	}
	OnActivity.Broadcast();
}

#undef LOCTEXT_NAMESPACE
