// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioAgentsSection.h"

#include "Async/Async.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "Misc/MessageDialog.h"
#include "ScenarioAgentClients.h"
#include "ScenarioAgentModule.h"
#include "ScenarioLocalHttpServer.h"
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
	using namespace ScenarioUI;
	auto IsRunning = []() { const TSharedPtr<FScenarioMcpServer> S = Server(); return S.IsValid() && S->IsRunning(); };
	const TSharedRef<SWidget> Toggle = Switch(MakeAttributeLambda(IsRunning), [this](bool bOn)
	{
		if (!FScenarioAgentModule::IsAvailable())
		{
			return;
		}
		FScenarioAgentModule& Module = FScenarioAgentModule::Get();
		UScenarioEditorSettings& Settings = UScenarioEditorSettings::GetMutable();
		if (!bOn)
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
	}, LOCTEXT("ServerTip", "Runs the local Scenario MCP server, so agents on this computer can price, generate and place assets here."));

	ChildSlot
	[
		SNew(SVerticalBox)
		// On and off, with the address agents use.
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Card(
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 12.0f, 0.0f)
				[
					SNew(SBorder)
					.BorderImage(Style().GetBrush("Scenario.Thumb"))
					.Padding(8.0f)
					[
						Icon(TEXT("agent"), 20.0f, MakeAttributeLambda([IsRunning]() { return IsRunning() ? FSlateColor(Palette::Success()) : FSlateColor(Palette::Dim()); }))
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text_Lambda([IsRunning]() { return IsRunning() ? LOCTEXT("OnTitle", "Agents can use Scenario and this project") : LOCTEXT("OffTitle", "Agents cannot reach this editor"); })
						.Font(Font(EText::Title))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 3.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.Font(Font(EText::Meta))
						.ColorAndOpacity(Palette::Dim())
						.Text_Lambda([]()
						{
							const TSharedPtr<FScenarioMcpServer> S = Server();
							if (!S.IsValid() || !S->IsRunning())
							{
								return LOCTEXT("Stopped", "Scenario MCP is off. Turn it on to let an agent price, generate and place assets here.");
							}
							return FText::Format(LOCTEXT("Running", "Scenario MCP on {0}{1}"), FText::FromString(S->Url()),
								S->bRequireToken ? LOCTEXT("WithToken", ", token required") : LOCTEXT("NoToken", ", no token"));
						})
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(Gutter, 0.0f, 0.0f, 0.0f)
				[
					Toggle
				], FMargin(12.0f))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SectionHeader(LOCTEXT("Clients", "Connect a client"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Card(BuildClients(), FMargin(12.0f))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.0f, 6.0f, 2.0f, 0.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Font(Font(EText::Meta))
			.Text_Lambda([this]() { return FText::FromString(Message); })
			.ColorAndOpacity_Lambda([this]() { return bMessageIsError ? FSlateColor(Palette::Error()) : FSlateColor(Palette::Dim()); })
			.Visibility_Lambda([this]() { return Message.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SectionHeader(LOCTEXT("Permissions", "Permissions"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Card(BuildPermissions(), FMargin(12.0f))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SectionHeader(LOCTEXT("EpicMcp", "Epic's Unreal MCP"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Card(BuildEpicMcp(), FMargin(12.0f))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SectionHeader(LOCTEXT("Activity", "Activity"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Card(SAssignNew(ActivityBox, SVerticalBox), FMargin(12.0f))
		]
	];
	if (FScenarioAgentModule::IsAvailable())
	{
		ActivityHandle = FScenarioAgentModule::Get().OnActivity.AddSP(this, &SScenarioAgentsSection::RefreshActivity);
	}
	RefreshActivity();
	RefreshConfigured();
	RefreshEpicMcp();
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
	using namespace ScenarioUI;
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	for (const ScenarioAgentClients::FClient& Client : ScenarioAgentClients::All())
	{
		const FString Id = Client.Id;
		const FString InfoName = Client.ClientInfoName;
		auto IsConfigured = [this, Id]() { const bool* Found = Configured.Find(Id); return Found && *Found; };
		Box->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(Client.Name)
					.Font(Font(EText::Body))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 1.0f, 0.0f, 0.0f)
				[
					Line(MakeAttributeLambda([InfoName]()
					{
						const TSharedPtr<FScenarioMcpServer> S = Server();
						return Ago(S.IsValid() ? S->LastSeen(InfoName) : 0.0);
					}), EText::Meta, true)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(Gutter, 0.0f)
			[
				Pill(MakeAttributeLambda([IsConfigured]() { return IsConfigured() ? LOCTEXT("Connected", "Connected") : LOCTEXT("NotSetUp", "Not set up"); }),
					MakeAttributeLambda([IsConfigured]() { return IsConfigured() ? ETone::Success : ETone::Neutral; }))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.MinDesiredWidth(84.0f)
				[
					SNew(SButton)
					.ButtonStyle(&Style().GetWidgetStyle<FButtonStyle>("Scenario.Button.Outline.Small"))
					.HAlign(HAlign_Center)
					.ToolTipText(FText::Format(LOCTEXT("ConnectTip", "Writes the scenario-unreal entry for this app with the current token (only that entry, after a backup). {0}"), FText::FromString(ScenarioAgentClients::ConfigPath(Id))))
					.IsEnabled_Lambda([this, Id]() { return !Busy.Contains(Id); })
					.OnClicked_Lambda([this, Id]() { Connect(Id); return FReply::Handled(); })
					[
						SNew(STextBlock)
						.Text_Lambda([IsConfigured]() { return IsConfigured() ? LOCTEXT("Reconnect", "Reconnect") : LOCTEXT("SetUp", "Set up"); })
						.Font(Font(EText::Meta))
					]
				]
			]
		];
	}
	Box->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)[Divider()];
	Box->AddSlot().AutoHeight()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			Button(LOCTEXT("CopySetup", "Copy setup"), EButton::Secondary, [this]()
			{
				FPlatformApplicationMisc::ClipboardCopy(*ScenarioAgentClients::SetupText(CurrentUrl(), CurrentToken()));
				Message = TEXT("Setup copied, token included.");
				bMessageIsError = false;
			}, TEXT("copy"), LOCTEXT("CopySetupTip", "Copies the URL, the token and ready-made entries for other apps. The token is a password: share it with nobody."), true)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			Button(LOCTEXT("InstallSkill", "Install the agent skill"), EButton::Secondary, [this]()
			{
				FString Text;
				bMessageIsError = !ScenarioAgentClients::InstallSkill(Text);
				Message = Text;
			}, TEXT("wand"), LOCTEXT("InstallSkillTip", "Copies the scenario-unreal skill (tool order, money rules, Unreal tips) to ~/.claude/skills."), true)
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNullWidget::NullWidget
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SBox)
			.IsEnabled_Lambda([]() { return UScenarioEditorSettings::Get().bAgentToken; })
			[
				Button(LOCTEXT("Rotate", "New token..."), EButton::Ghost, [this]()
				{
					if (FMessageDialog::Open(EAppMsgType::OkCancel, LOCTEXT("RotateConfirm", "Replace the agent token? Connected apps stop working until you press Reconnect.")) == EAppReturnType::Ok)
					{
						FScenarioAgentModule::Get().RotateToken();
						Message = TEXT("New token in place. Press Reconnect for each app you use.");
						bMessageIsError = false;
					}
				}, TEXT("key"), LOCTEXT("RotateTip", "Replaces the token. Every app must be set up again."), true)
			]
		]
	];
	return Box;
}

TSharedRef<SWidget> SScenarioAgentsSection::BuildPermissions()
{
	using namespace ScenarioUI;
	struct FPermission
	{
		FText Label;
		FText Hint;
		FText Tip;
		bool UScenarioEditorSettings::* Field;
	};
	const TArray<FPermission> Permissions = {
		{ LOCTEXT("AllowChanges", "Change this project"), LOCTEXT("AllowChangesHint", "Off: an agent's change arrives as a proposal you apply"), LOCTEXT("AllowChangesTip", "Import and place results, save what the plugin created, remove what it placed. Off: an agent's change arrives here as a proposal you apply."), &UScenarioEditorSettings::bAllowAgentChanges },
		{ LOCTEXT("AllowPython", "Run Unreal Python"), LOCTEXT("AllowPythonHint", "Needs the permission above; one undo step per call"), LOCTEXT("AllowPythonTip", "Unreal Python in this editor, one undo step per call. Needs the setting above and the Python plugin."), &UScenarioEditorSettings::bAllowAgentPython },
		{ LOCTEXT("ApproveSpend", "Approve each spend here"), LOCTEXT("ApproveSpendHint", "Off: agents spend only with a quote they priced first"), LOCTEXT("ApproveSpendTip", "Off by default: agents spend only with a quote they priced first, and each spend shows as a notification. On: each paid call waits for Approve."), &UScenarioEditorSettings::bApproveAgentSpend },
	};
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	for (const FPermission& Permission : Permissions)
	{
		bool UScenarioEditorSettings::* Field = Permission.Field;
		const bool bPython = Field == &UScenarioEditorSettings::bAllowAgentPython;
		Box->AddSlot().AutoHeight().Padding(0.0f, Box->NumSlots() > 0 ? 10.0f : 0.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			.IsEnabled_Lambda([bPython]() { return !bPython || UScenarioEditorSettings::Get().bAllowAgentChanges; })
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				FieldLabel(Permission.Label, Permission.Hint, Permission.Tip)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(Gutter, 0.0f, 0.0f, 0.0f)
			[
				Switch(MakeAttributeLambda([Field]() { return UScenarioEditorSettings::Get().*Field; }), [Field](bool bOn)
				{
					UScenarioEditorSettings& Settings = UScenarioEditorSettings::GetMutable();
					Settings.*Field = bOn;
					if (!Settings.bAllowAgentChanges)
					{
						Settings.bAllowAgentPython = false;
					}
					Settings.SaveConfig();
				}, Permission.Tip)
			]
		];
	}
	return Box;
}

TSharedRef<SWidget> SScenarioAgentsSection::BuildEpicMcp()
{
	using namespace ScenarioUI;
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				Line(MakeAttributeLambda([this]() { return EpicMcpTitle(); }), EText::Body)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Font(Font(EText::Meta))
				.ColorAndOpacity(Palette::Dim())
				.Text_Lambda([this]() { return EpicMcpDetail(); })
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(Gutter, 0.0f)
		[
			Pill(MakeAttributeLambda([this]()
				{
					switch (EpicMcp)
					{
					case EEpicMcp::Running: return LOCTEXT("EpicRunning", "Running");
					case EEpicMcp::Checking: return LOCTEXT("EpicChecking", "Checking");
					case EEpicMcp::Stopped: return LOCTEXT("EpicStopped", "Not running");
					default: return LOCTEXT("EpicOff", "Off");
					}
				}),
				MakeAttributeLambda([this]() { return EpicMcp == EEpicMcp::Running ? ETone::Success : ETone::Neutral; }))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			Button(LOCTEXT("EpicCheck", "Check again"), EButton::Ghost, [this]() { RefreshEpicMcp(); }, NAME_None,
				LOCTEXT("EpicCheckTip", "Reads Epic's Model Context Protocol settings and checks whether its port answers on this computer."), true)
		];
}

void SScenarioAgentsSection::RefreshEpicMcp()
{
	// Epic's plugin is optional and Experimental: read it by name and reflection, never link to it.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ModelContextProtocol"));
	if (!Plugin.IsValid())
	{
		EpicMcp = EEpicMcp::NotInEngine;
		return;
	}
	if (!Plugin->IsEnabled() || !FModuleManager::Get().IsModuleLoaded(TEXT("ModelContextProtocol")))
	{
		EpicMcp = EEpicMcp::Off;
		return;
	}
	int32 Port = 8000;
	FString Path = TEXT("/mcp");
	if (const UClass* Settings = FindObject<UClass>(nullptr, TEXT("/Script/ModelContextProtocolEngine.ModelContextProtocolSettings")))
	{
		const UObject* Defaults = Settings->GetDefaultObject();
		if (const FUInt32Property* PortProperty = FindFProperty<FUInt32Property>(Settings, TEXT("ServerPortNumber")))
		{
			Port = static_cast<int32>(PortProperty->GetPropertyValue_InContainer(Defaults));
		}
		if (const FStrProperty* PathProperty = FindFProperty<FStrProperty>(Settings, TEXT("ServerUrlPath")))
		{
			Path = PathProperty->GetPropertyValue_InContainer(Defaults);
		}
	}
	FParse::Value(FCommandLine::Get(), TEXT("ModelContextProtocolPort="), Port);
	EpicMcpPort = Port;
	EpicMcpPath = Path.StartsWith(TEXT("/")) ? Path : TEXT("/") + Path;
	EpicMcp = EEpicMcp::Checking;
	TWeakPtr<SScenarioAgentsSection> Weak = StaticCastSharedRef<SScenarioAgentsSection>(AsShared());
	Async(EAsyncExecution::ThreadPool, [Weak, Port]()
	{
		const bool bAnswering = FScenarioLocalHttpServer::IsLoopbackPortAnswering(Port);
		AsyncTask(ENamedThreads::GameThread, [Weak, Port, bAnswering]()
		{
			if (const TSharedPtr<SScenarioAgentsSection> This = Weak.Pin())
			{
				if (This->EpicMcpPort == Port)
				{
					This->EpicMcp = bAnswering ? EEpicMcp::Running : EEpicMcp::Stopped;
				}
			}
		});
	});
}

FText SScenarioAgentsSection::EpicMcpTitle() const
{
	switch (EpicMcp)
	{
	case EEpicMcp::NotInEngine:
		return LOCTEXT("EpicNotInEngine", "Not part of this engine (Unreal 5.8 and later)");
	case EEpicMcp::Off:
		return LOCTEXT("EpicPluginOff", "The Model Context Protocol plugin is off");
	case EEpicMcp::Running:
		return FText::Format(LOCTEXT("EpicRunningOn", "Running on http://127.0.0.1:{0}{1}"), FText::AsNumber(EpicMcpPort, &FNumberFormattingOptions::DefaultNoGrouping()), FText::FromString(EpicMcpPath));
	default:
		return FText::Format(LOCTEXT("EpicStoppedOn", "Enabled, not answering on port {0}"), FText::AsNumber(EpicMcpPort, &FNumberFormattingOptions::DefaultNoGrouping()));
	}
}

FText SScenarioAgentsSection::EpicMcpDetail() const
{
	if (EpicMcp == EEpicMcp::NotInEngine || EpicMcp == EEpicMcp::Off)
	{
		return LOCTEXT("EpicNotNeeded", "Scenario MCP above works without it.");
	}
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ScenarioToolsets")) || !FModuleManager::Get().IsModuleLoaded(TEXT("ToolsetRegistry")))
	{
		return LOCTEXT("EpicNoToolset", "Scenario's toolset there needs Epic's Toolset Registry plugin. Epic's server has no token: keep it on this computer.");
	}
	return LOCTEXT("EpicToolset", "Agents connected to it find Scenario as the toolset ScenarioToolsets.ScenarioToolset. Every paid call from there waits for your approval in this editor. Epic's server has no token: keep it on this computer.");
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
			.ColorAndOpacity(ScenarioUI::Palette::Dim())
		];
		return;
	}
	for (int32 Index = Log.Num() - 1; Index >= FMath::Max(0, Log.Num() - 20); --Index)
	{
		const FScenarioAgentActivity& Entry = Log[Index];
		const FDateTime When = FDateTime::FromUnixTimestamp(static_cast<int64>(Entry.Time)) + (FDateTime::Now() - FDateTime::UtcNow());
		FString Detail = Entry.Client.IsEmpty() ? FString(TEXT("agent")) : Entry.Client;
		if (!Entry.Spent.IsEmpty())
		{
			Detail += FString::Printf(TEXT(" · %s CU"), *Entry.Spent);
		}
		if (!Entry.bOk)
		{
			Detail += TEXT(" · ") + Entry.Error;
		}
		ActivityBox->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(When.ToString(TEXT("%H:%M:%S"))))
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				.ColorAndOpacity(ScenarioUI::Palette::Faint())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Entry.Tool))
				.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
				.ColorAndOpacity(Entry.bOk ? FSlateColor(ScenarioUI::Palette::Text()) : FSlateColor(ScenarioUI::Palette::Warning()))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				ScenarioUI::Line(FText::FromString(Detail), ScenarioUI::EText::Meta, true)
			]
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
