// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioAgentsSection.h"

#include "Async/Async.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/MessageDialog.h"
#include "ScenarioAgentClients.h"
#include "ScenarioAgentModule.h"
#include "ScenarioMcpServer.h"
#include "ScenarioSettings.h"
#include "ScenarioTypes.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioAgentsSection"

namespace
{
	FSlateColor Muted()
	{
		return FSlateColor::UseSubduedForeground();
	}

	FText Ago(double Then)
	{
		if (Then <= 0.0)
		{
			return LOCTEXT("Never", "not seen yet");
		}
		const double Seconds = ScenarioNow() - Then;
		if (Seconds < 90.0)
		{
			return LOCTEXT("JustNow", "seen just now");
		}
		if (Seconds < 3600.0)
		{
			return FText::Format(LOCTEXT("Minutes", "seen {0} min ago"), FText::AsNumber(FMath::RoundToInt(Seconds / 60.0)));
		}
		return FText::Format(LOCTEXT("Hours", "seen {0} h ago"), FText::AsNumber(FMath::RoundToInt(Seconds / 3600.0)));
	}

	TSharedPtr<FScenarioMcpServer> Server()
	{
		return FScenarioAgentModule::IsAvailable() ? FScenarioAgentModule::Get().Server() : nullptr;
	}

	FString CurrentUrl()
	{
		const TSharedPtr<FScenarioMcpServer> S = Server();
		return S.IsValid() ? S->Url() : FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), UScenarioEditorSettings::Get().AgentPort);
	}

	FString CurrentToken()
	{
		return UScenarioEditorSettings::Get().bAgentToken && FScenarioAgentModule::IsAvailable() ? FScenarioAgentModule::Get().Token() : FString();
	}
}

void SScenarioAgentsSection::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("Title", "Agents")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			// Server state
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([]()
					{
						const TSharedPtr<FScenarioMcpServer> S = Server();
						if (!S.IsValid() || !S->IsRunning())
						{
							return LOCTEXT("Stopped", "The Scenario MCP server is off. Agents cannot reach this editor.");
						}
						return FText::Format(LOCTEXT("Running", "Listening on {0} (this computer only){1}."), FText::FromString(S->Url()),
							S->bRequireToken ? LOCTEXT("WithToken", ", token required") : LOCTEXT("NoToken", ", no token"));
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text_Lambda([]() { const TSharedPtr<FScenarioMcpServer> S = Server(); return S.IsValid() && S->IsRunning() ? LOCTEXT("Stop", "Stop") : LOCTEXT("Start", "Start"); })
					.OnClicked_Lambda([this]()
					{
						if (!FScenarioAgentModule::IsAvailable())
						{
							return FReply::Handled();
						}
						FScenarioAgentModule& Module = FScenarioAgentModule::Get();
						UScenarioEditorSettings& Settings = UScenarioEditorSettings::GetMutable();
						const TSharedPtr<FScenarioMcpServer> S = Server();
						if (S.IsValid() && S->IsRunning())
						{
							Module.StopServer();
							Settings.bAgentServer = false;
						}
						else
						{
							FString Error;
							bMessageIsError = !Module.StartServer(Error);
							Message = Error;
							Settings.bAgentServer = !bMessageIsError;
						}
						Settings.SaveConfig();
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				BuildClients()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text_Lambda([this]() { return FText::FromString(Message); })
				.ColorAndOpacity_Lambda([this]() { return bMessageIsError ? FSlateColor(FLinearColor(0.9f, 0.35f, 0.3f)) : Muted(); })
				.Visibility_Lambda([this]() { return Message.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				BuildPermissions()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("Activity", "Activity")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(ActivityBox, SVerticalBox)
			]
		]
	];
	if (FScenarioAgentModule::IsAvailable())
	{
		ActivityHandle = FScenarioAgentModule::Get().OnActivity.AddSP(this, &SScenarioAgentsSection::RefreshActivity);
	}
	RefreshActivity();
	RefreshConfigured();
}

void SScenarioAgentsSection::RefreshConfigured()
{
	const FString Url = CurrentUrl();
	TWeakPtr<SScenarioAgentsSection> Weak = StaticCastSharedRef<SScenarioAgentsSection>(AsShared());
	Async(EAsyncExecution::ThreadPool, [Weak, Url]()
	{
		TMap<FString, bool> Result;
		for (const ScenarioAgentClients::FClient& Client : ScenarioAgentClients::All())
		{
			Result.Add(Client.Id, ScenarioAgentClients::IsConfigured(Client.Id, Url));
		}
		AsyncTask(ENamedThreads::GameThread, [Weak, Result]()
		{
			if (const TSharedPtr<SScenarioAgentsSection> This = Weak.Pin())
			{
				This->Configured = Result;
			}
		});
	});
}

SScenarioAgentsSection::~SScenarioAgentsSection()
{
	if (FScenarioAgentModule::IsAvailable())
	{
		FScenarioAgentModule::Get().OnActivity.Remove(ActivityHandle);
	}
}

TSharedRef<SWidget> SScenarioAgentsSection::BuildClients()
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	for (const ScenarioAgentClients::FClient& Client : ScenarioAgentClients::All())
	{
		const FString Id = Client.Id;
		const FString InfoName = Client.ClientInfoName;
		Box->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(0.35f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Client.Name)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.65f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.ColorAndOpacity(Muted())
				.Text_Lambda([this, Id, InfoName]()
				{
					const bool* Found = Configured.Find(Id);
					const bool bConfigured = Found && *Found;
					const TSharedPtr<FScenarioMcpServer> S = Server();
					const double Seen = S.IsValid() ? S->LastSeen(InfoName) : 0.0;
					return FText::Format(LOCTEXT("ClientState", "{0}, {1}"), bConfigured ? LOCTEXT("Configured", "configured") : LOCTEXT("NotConfigured", "not configured"), Ago(Seen));
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Connect", "Connect"))
				.ToolTipText(FText::Format(LOCTEXT("ConnectTip", "Writes the scenario-unreal entry for this client (only that entry, after a backup). {0}"), FText::FromString(ScenarioAgentClients::ConfigPath(Id))))
				.IsEnabled_Lambda([this, Id]() { return !Busy.Contains(Id); })
				.OnClicked_Lambda([this, Id]() { Connect(Id); return FReply::Handled(); })
			]
		];
	}
	Box->AddSlot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("CopySetup", "Copy setup"))
			.ToolTipText(LOCTEXT("CopySetupTip", "Copies the URL, the token and ready-made entries for other clients. The token is a password: share it with nobody."))
			.OnClicked_Lambda([this]()
			{
				FPlatformApplicationMisc::ClipboardCopy(*ScenarioAgentClients::SetupText(CurrentUrl(), CurrentToken()));
				Message = TEXT("Setup copied, token included.");
				bMessageIsError = false;
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("InstallSkill", "Install the agent skill"))
			.ToolTipText(LOCTEXT("InstallSkillTip", "Copies the scenario-unreal skill (tool order, money rules, Unreal tips) to ~/.claude/skills."))
			.OnClicked_Lambda([this]()
			{
				FString Text;
				bMessageIsError = !ScenarioAgentClients::InstallSkill(Text);
				Message = Text;
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Rotate", "New token"))
			.ToolTipText(LOCTEXT("RotateTip", "Replaces the token. Every client must be connected again."))
			.IsEnabled_Lambda([]() { return UScenarioEditorSettings::Get().bAgentToken; })
			.OnClicked_Lambda([this]()
			{
				if (FMessageDialog::Open(EAppMsgType::OkCancel, LOCTEXT("RotateConfirm", "Replace the agent token? Connected clients stop working until you press Connect again.")) == EAppReturnType::Ok)
				{
					FScenarioAgentModule::Get().RotateToken();
					Message = TEXT("New token in place. Press Connect for each client you use.");
					bMessageIsError = false;
				}
				return FReply::Handled();
			})
		]
	];
	return Box;
}

TSharedRef<SWidget> SScenarioAgentsSection::BuildPermissions()
{
	struct FSwitch
	{
		FText Label;
		FText Tip;
		bool UScenarioEditorSettings::* Field;
	};
	const TArray<FSwitch> Switches = {
		{ LOCTEXT("AllowChanges", "Allow agents to change this project"), LOCTEXT("AllowChangesTip", "Import and place results, save what the plugin created, remove what it placed. Off: an agent's change arrives here as a proposal you apply."), &UScenarioEditorSettings::bAllowAgentChanges },
		{ LOCTEXT("AllowPython", "Allow agents to run Python"), LOCTEXT("AllowPythonTip", "Unreal Python in this editor, one undo step per call. Needs the setting above and the Python plugin."), &UScenarioEditorSettings::bAllowAgentPython },
		{ LOCTEXT("ApproveSpend", "Approve each agent spend here"), LOCTEXT("ApproveSpendTip", "Off by default: agents spend only with a quote they priced first, and each spend shows as a notification. On: each paid call waits for Approve."), &UScenarioEditorSettings::bApproveAgentSpend },
	};
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	for (const FSwitch& Switch : Switches)
	{
		bool UScenarioEditorSettings::* Field = Switch.Field;
		const bool bPython = Field == &UScenarioEditorSettings::bAllowAgentPython;
		Box->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)
		[
			SNew(SCheckBox)
			.ToolTipText(Switch.Tip)
			.IsEnabled_Lambda([bPython]() { return !bPython || UScenarioEditorSettings::Get().bAllowAgentChanges; })
			.IsChecked_Lambda([Field]() { return UScenarioEditorSettings::Get().*Field ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([Field](ECheckBoxState State)
			{
				UScenarioEditorSettings& Settings = UScenarioEditorSettings::GetMutable();
				Settings.*Field = State == ECheckBoxState::Checked;
				if (!Settings.bAllowAgentChanges)
				{
					Settings.bAllowAgentPython = false;
				}
				Settings.SaveConfig();
			})
			[
				SNew(STextBlock).Text(Switch.Label)
			]
		];
	}
	return Box;
}

void SScenarioAgentsSection::RefreshActivity()
{
	if (!ActivityBox.IsValid())
	{
		return;
	}
	ActivityBox->ClearChildren();
	const TArray<FScenarioAgentActivity> Empty;
	const TArray<FScenarioAgentActivity>& Log = FScenarioAgentModule::IsAvailable() ? FScenarioAgentModule::Get().Activity() : Empty;
	if (Log.Num() == 0)
	{
		ActivityBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock).ColorAndOpacity(Muted()).Text(LOCTEXT("NoActivity", "No agent calls yet in this session."))
		];
		return;
	}
	for (int32 Index = Log.Num() - 1; Index >= FMath::Max(0, Log.Num() - 20); --Index)
	{
		const FScenarioAgentActivity& Entry = Log[Index];
		const FDateTime When = FDateTime::FromUnixTimestamp(static_cast<int64>(Entry.Time)) + (FDateTime::Now() - FDateTime::UtcNow());
		FString Line = FString::Printf(TEXT("%s  %s  %s"), *When.ToString(TEXT("%H:%M:%S")), *(Entry.Client.IsEmpty() ? FString(TEXT("agent")) : Entry.Client), *Entry.Tool);
		if (!Entry.Spent.IsEmpty())
		{
			Line += FString::Printf(TEXT("  %s CU"), *Entry.Spent);
		}
		if (!Entry.bOk)
		{
			Line += TEXT("  ") + Entry.Error;
		}
		ActivityBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(Line))
			.ColorAndOpacity(Entry.bOk ? Muted() : FSlateColor(FLinearColor(0.9f, 0.6f, 0.3f)))
		];
	}
}

void SScenarioAgentsSection::Connect(const FString& ClientId)
{
	if (!FScenarioAgentModule::IsAvailable())
	{
		return;
	}
	const TSharedPtr<FScenarioMcpServer> S = Server();
	if (!S.IsValid() || !S->IsRunning())
	{
		Message = TEXT("Start the server first.");
		bMessageIsError = true;
		return;
	}
	const FString Url = S->Url();
	const FString Token = CurrentToken();
	Busy.Add(ClientId);
	Message = TEXT("Connecting...");
	bMessageIsError = false;
	TWeakPtr<SScenarioAgentsSection> Weak = StaticCastSharedRef<SScenarioAgentsSection>(AsShared());
	// The CLI and file writes block: off the game thread.
	Async(EAsyncExecution::ThreadPool, [Weak, ClientId, Url, Token]()
	{
		FString Text;
		const bool bOk = ScenarioAgentClients::Connect(ClientId, Url, Token, Text);
		AsyncTask(ENamedThreads::GameThread, [Weak, ClientId, bOk, Text]()
		{
			if (const TSharedPtr<SScenarioAgentsSection> This = Weak.Pin())
			{
				This->Busy.Remove(ClientId);
				This->Message = Text;
				This->bMessageIsError = !bOk;
				This->RefreshConfigured();
			}
		});
	});
}

#undef LOCTEXT_NAMESPACE
