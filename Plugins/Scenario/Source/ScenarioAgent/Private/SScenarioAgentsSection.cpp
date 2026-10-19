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
#include "ScenarioWidgets.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioAgentsSection"

namespace
{
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
	// On and off, in the section header.
	const TSharedRef<SWidget> Switch = SNew(SCheckBox)
		.ToolTipText(LOCTEXT("ServerTip", "Runs the local Scenario MCP server, so agents on this computer can price, generate and place assets here."))
		.IsChecked_Lambda([]() { const TSharedPtr<FScenarioMcpServer> S = Server(); return S.IsValid() && S->IsRunning() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			if (!FScenarioAgentModule::IsAvailable())
			{
				return;
			}
			FScenarioAgentModule& Module = FScenarioAgentModule::Get();
			UScenarioEditorSettings& Settings = UScenarioEditorSettings::GetMutable();
			if (State != ECheckBoxState::Checked)
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
		})
		[
			SNew(STextBlock)
			.Text(LOCTEXT("On", "On"))
			.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
		];

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			ScenarioUI::SectionHeader(LOCTEXT("Title", "Agents (Scenario MCP)"), Switch)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(ScenarioUI::Gutter, ScenarioUI::Gutter, ScenarioUI::Gutter, ScenarioUI::SectionGap))
		[
			SNew(SVerticalBox)
			// Server state.
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([]()
				{
					const TSharedPtr<FScenarioMcpServer> S = Server();
					if (!S.IsValid() || !S->IsRunning())
					{
						return LOCTEXT("Stopped", "Off. Agents cannot reach this editor.");
					}
					return FText::Format(LOCTEXT("Running", "Agents on this computer can price, generate and place assets here. Listening on {0}{1}."), FText::FromString(S->Url()),
						S->bRequireToken ? LOCTEXT("WithToken", ", token required") : LOCTEXT("NoToken", ", no token"));
				})
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, ScenarioUI::Gutter, 0.0f, 0.0f)
			[
				BuildClients()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, ScenarioUI::RowGap, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				.Text_Lambda([this]() { return FText::FromString(Message); })
				.ColorAndOpacity_Lambda([this]() { return bMessageIsError ? FSlateColor(FStyleColors::Error) : FSlateColor::UseSubduedForeground(); })
				.Visibility_Lambda([this]() { return Message.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, ScenarioUI::SectionGap, 0.0f, ScenarioUI::RowGap)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Permissions", "Permissions"))
				.Font(ScenarioUI::Font(ScenarioUI::EText::Section))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildPermissions()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, ScenarioUI::SectionGap, 0.0f, ScenarioUI::RowGap)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Activity", "Activity"))
				.Font(ScenarioUI::Font(ScenarioUI::EText::Section))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
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
		auto IsConfigured = [this, Id]() { const bool* Found = Configured.Find(Id); return Found && *Found; };
		Box->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, ScenarioUI::RowGap)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(ScenarioUI::LabelWidth)
				[
					SNew(STextBlock)
					.Text(Client.Name)
					.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, ScenarioUI::Gutter, 0.0f)
			[
				ScenarioUI::Chip(
					MakeAttributeLambda([IsConfigured]() { return IsConfigured() ? LOCTEXT("Connected", "Connected") : LOCTEXT("NotSetUp", "Not set up"); }),
					MakeAttributeLambda([IsConfigured]() { return IsConfigured() ? FSlateColor(FStyleColors::AccentGreen) : FSlateColor(FStyleColors::AccentGray); }))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				ScenarioUI::Line(MakeAttributeLambda([InfoName]()
				{
					const TSharedPtr<FScenarioMcpServer> S = Server();
					return Ago(S.IsValid() ? S->LastSeen(InfoName) : 0.0);
				}), ScenarioUI::EText::Meta, true)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text_Lambda([IsConfigured]() { return IsConfigured() ? LOCTEXT("Reconnect", "Reconnect") : LOCTEXT("SetUp", "Set up"); })
				.ToolTipText(FText::Format(LOCTEXT("ConnectTip", "Writes the scenario-unreal entry for this app with the current token (only that entry, after a backup). {0}"), FText::FromString(ScenarioAgentClients::ConfigPath(Id))))
				.IsEnabled_Lambda([this, Id]() { return !Busy.Contains(Id); })
				.OnClicked_Lambda([this, Id]() { Connect(Id); return FReply::Handled(); })
			]
		];
	}
	Box->AddSlot().AutoHeight().Padding(0.0f, ScenarioUI::RowGap, 0.0f, 0.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("CopySetup", "Copy setup for another app"))
			.ToolTipText(LOCTEXT("CopySetupTip", "Copies the URL, the token and ready-made entries for other apps. The token is a password: share it with nobody."))
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
		.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
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
		.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Rotate", "New token..."))
			.ToolTipText(LOCTEXT("RotateTip", "Replaces the token. Every app must be set up again."))
			.IsEnabled_Lambda([]() { return UScenarioEditorSettings::Get().bAgentToken; })
			.OnClicked_Lambda([this]()
			{
				if (FMessageDialog::Open(EAppMsgType::OkCancel, LOCTEXT("RotateConfirm", "Replace the agent token? Connected apps stop working until you press Reconnect.")) == EAppReturnType::Ok)
				{
					FScenarioAgentModule::Get().RotateToken();
					Message = TEXT("New token in place. Press Reconnect for each app you use.");
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
			SNew(STextBlock)
			.Text(LOCTEXT("NoActivity", "No agent calls yet in this session."))
			.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
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
			.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
			.ColorAndOpacity(Entry.bOk ? FSlateColor::UseSubduedForeground() : FSlateColor(FStyleColors::Warning))
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
