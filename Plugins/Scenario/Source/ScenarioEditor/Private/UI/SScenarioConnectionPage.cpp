// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "ScenarioCatalog.h"
#include "ScenarioCU.h"
#include "ScenarioEditorModule.h"
#include "ScenarioSession.h"
#include "ScenarioSettings.h"
#include "ScenarioStyle.h"
#include "ScenarioSubsystem.h"
#include "SScenarioPages.h"
#include "ScenarioWidgets.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioConnection"

namespace
{
	FString AuthText(const FScenarioSession& Session)
	{
		switch (Session.GetAuthMode())
		{
		case EScenarioAuthMode::OAuth:
			return TEXT("Signed in with your Scenario account.");
		case EScenarioAuthMode::ApiKey:
			return Session.GetApiKeySource() == TEXT("environment")
				? TEXT("Using the API key from the SCENARIO_API_KEY environment variable.")
				: TEXT("Using a saved API key.");
		default:
			return TEXT("Signed out.");
		}
	}
}

FString SScenarioConnectionPage::Diagnostics()
{
	TArray<FString> Lines;
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scenario"));
	Lines.Add(FString::Printf(TEXT("Scenario for Unreal Engine %s"), Plugin.IsValid() ? *Plugin->GetDescriptor().VersionName : TEXT("?")));
	Lines.Add(FString::Printf(TEXT("Unreal Engine %s"), *FEngineVersion::Current().ToString(EVersionComponent::Patch)));
	Lines.Add(FString::Printf(TEXT("Platform %s %s"), ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()), *FPlatformMisc::GetOSVersion()));
	FScenarioSession* Session = ScenarioUI::Session();
	if (!Session)
	{
		Lines.Add(TEXT("Session: not started"));
		return FString::Join(Lines, TEXT("\n"));
	}
	static const TCHAR* Connections[] = { TEXT("signed out"), TEXT("connecting"), TEXT("needs a project"), TEXT("ready") };
	Lines.Add(FString::Printf(TEXT("Connection: %s"), Connections[FMath::Clamp(static_cast<int32>(Session->GetConnection()), 0, 3)]));
	Lines.Add(FString::Printf(TEXT("Sign-in: %s"), *AuthText(*Session)));
	Lines.Add(FString::Printf(TEXT("Credentials: %s%s"), *Session->DescribeCredentialStore(), Session->IsCredentialStoreSecure() ? TEXT("") : TEXT(" (not encrypted)")));
	Lines.Add(FString::Printf(TEXT("Endpoint: %s"), Session->GetConfig().Endpoint.IsEmpty() ? TEXT("default") : *Session->GetConfig().Endpoint));
	Lines.Add(FString::Printf(TEXT("Team/project ids: %s / %s"), *Session->GetWorkspace().TeamId, *Session->GetWorkspace().ProjectId));
	Lines.Add(FString::Printf(TEXT("Spending: %s"), Session->IsSpendingDisabled() ? TEXT("off (SCENARIO_NO_SPEND=1)") : TEXT("on")));
	Lines.Add(FString::Printf(TEXT("Automatic price checks: %s"), Session->IsEstimateAlarmRaised() ? TEXT("stopped (a price check returned a job)") : TEXT("on")));
	const FScenarioCatalog& Catalog = Session->Catalog();
	Lines.Add(FString::Printf(TEXT("Catalog: %d models, fetched %s"), Catalog.All().Num(),
		Catalog.FetchedAt() > 0.0 ? *FDateTime::FromUnixTimestamp(static_cast<int64>(Catalog.FetchedAt())).ToIso8601() : TEXT("never")));
	int32 Active = 0;
	int32 Unresolved = 0;
	for (const FScenarioJobRow& Row : Session->Ledger().Rows())
	{
		Active += Row.IsActive() ? 1 : 0;
		Unresolved += Row.IsUnresolved() ? 1 : 0;
	}
	Lines.Add(FString::Printf(TEXT("Jobs: %d in the list, %d running, %d unresolved"), Session->Ledger().Rows().Num(), Active, Unresolved));
	Lines.Add(FString::Printf(TEXT("State folder: %s"), *FPaths::ConvertRelativePathToFull(UScenarioSubsystem::StateDir())));
	if (Session->GetLastError().Kind != EScenarioErrorKind::None)
	{
		Lines.Add(FString::Printf(TEXT("Last error: %s"), *Session->GetLastError().ToString()));
	}
	return FString::Join(Lines, TEXT("\n"));
}

void SScenarioConnectionPage::Construct(const FArguments& InArgs)
{
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		bWasSignedIn = Session->IsSignedIn();
		const TWeakPtr<SScenarioConnectionPage> Weak = SharedThis(this);
		SessionHandle = Session->OnChanged.AddLambda([Weak]()
		{
			const TSharedPtr<SScenarioConnectionPage> This = Weak.Pin();
			FScenarioSession* S = ScenarioUI::Session();
			if (This.IsValid() && S && S->IsSignedIn() != This->bWasSignedIn)
			{
				This->bWasSignedIn = S->IsSignedIn();
				This->Rebuild();
			}
		});
	}
	ChildSlot
	[
		SAssignNew(Content, SBox)
	];
	Rebuild();
}

SScenarioConnectionPage::~SScenarioConnectionPage()
{
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		Session->OnChanged.Remove(SessionHandle);
	}
}

void SScenarioConnectionPage::Rebuild()
{
	FScenarioSession* Session = ScenarioUI::Session();
	if (!Session || !Session->IsSignedIn())
	{
		Content->SetContent(SNew(SScenarioSignIn));
		return;
	}

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	// Account
	Box->AddSlot().AutoHeight()
	[
		ScenarioUI::Section(LOCTEXT("Account", "Account"),
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([]() { FScenarioSession* S = ScenarioUI::Session(); return S ? FText::FromString(AuthText(*S)) : FText::GetEmpty(); })
					.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
					.AutoWrapText(true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("SignOut", "Sign out"))
					.ToolTipText(LOCTEXT("SignOutTip", "Removes the saved sign-in from this machine. Jobs and imported assets stay."))
					.OnClicked_Lambda([]()
					{
						if (FScenarioSession* S = ScenarioUI::Session())
						{
							S->SignOut();
						}
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, ScenarioUI::RowGap, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([]()
				{
					FScenarioSession* S = ScenarioUI::Session();
					if (!S)
					{
						return FText::GetEmpty();
					}
					return S->IsCredentialStoreSecure()
						? FText::Format(LOCTEXT("Stored", "Sign-in kept in {0}, never in project files."), FText::FromString(S->DescribeCredentialStore()))
						: FText::Format(LOCTEXT("Insecure", "Sign-in kept in {0}. This store is not encrypted on this system."), FText::FromString(S->DescribeCredentialStore()));
				})
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				.ColorAndOpacity_Lambda([]() { FScenarioSession* S = ScenarioUI::Session(); return S && !S->IsCredentialStoreSecure() ? FSlateColor(FStyleColors::Warning) : FSlateColor::UseSubduedForeground(); })
				.AutoWrapText(true)
			])
	];

	// Team and project, and what the project used.
	Box->AddSlot().AutoHeight()
	[
		ScenarioUI::Section(LOCTEXT("Workspace", "Team and project"),
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				Session->GetAuthMode() == EScenarioAuthMode::OAuth
					? StaticCastSharedRef<SWidget>(SNew(SScenarioWorkspacePicker))
					: StaticCastSharedRef<SWidget>(SNew(STextBlock)
						.Text_Lambda([]()
						{
							FScenarioSession* S = ScenarioUI::Session();
							const FText Current = S && S->GetWorkspace().IsSet()
								? FText::Format(LOCTEXT("Current", "Generating in {0}."), FText::FromString(S->GetWorkspace().Label()))
								: LOCTEXT("NoWorkspace", "No project chosen yet.");
							return FText::Format(LOCTEXT("KeyScope", "{0} An API key belongs to one project: use another key to switch."), Current);
						})
						.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
						.AutoWrapText(true))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, ScenarioUI::Gutter, 0.0f, 0.0f)
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
						.Text(LOCTEXT("UsageLabel", "Usage"))
						.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([]()
					{
						FScenarioSession* S = ScenarioUI::Session();
						const double Used = S ? S->GetUsageThisMonth() : -1.0;
						return Used < 0.0
							? LOCTEXT("UsageUnknown", "Usage unavailable")
							: FText::Format(LOCTEXT("UsageValue", "{0} this month, all members and tools"), FText::FromString(ScenarioCU::Format(Used)));
					})
					.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
					.AutoWrapText(true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					ScenarioUI::IconButton(TEXT("Icons.Refresh"), LOCTEXT("RefreshUsage", "Refresh this month's usage. Free."),
						FSimpleDelegate::CreateLambda([]()
						{
							if (FScenarioSession* S = ScenarioUI::Session())
							{
								S->RefreshUsage();
							}
						}))
				]
			])
	];

	// Sections other modules add (agents).
	for (const TPair<FName, FScenarioEditorModule::FSectionFactory>& Section : FScenarioEditorModule::ConnectionSections())
	{
		Box->AddSlot().AutoHeight()[Section.Value()];
	}

	// Diagnostics
	Box->AddSlot().AutoHeight()
	[
		ScenarioUI::Section(LOCTEXT("Diagnostics", "Diagnostics"),
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text_Lambda([]() { return FText::FromString(SScenarioConnectionPage::Diagnostics()); })
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, ScenarioUI::Gutter, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, ScenarioUI::Gutter, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Copy", "Copy diagnostics"))
					.ToolTipText(LOCTEXT("CopyTip", "Copies these lines for support. They hold no key, token or prompt."))
					.OnClicked_Lambda([]()
					{
						FPlatformApplicationMisc::ClipboardCopy(*SScenarioConnectionPage::Diagnostics());
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, ScenarioUI::Gutter, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Folder", "Open state folder"))
					.ToolTipText(LOCTEXT("FolderTip", "Opens Saved/Scenario: job list, downloads, captures."))
					.OnClicked_Lambda([]()
					{
						FPlatformProcess::ExploreFolder(*FPaths::ConvertRelativePathToFull(UScenarioSubsystem::StateDir()));
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Settings", "Editor Preferences"))
					.ToolTipText(LOCTEXT("SettingsTip", "Opens Editor Preferences > Plugins > Scenario."))
					.OnClicked_Lambda([]()
					{
						FScenarioEditorModule::OpenSettings(false);
						return FReply::Handled();
					})
				]
			])
	];

	Content->SetContent(
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			Box
		]);
}

#undef LOCTEXT_NAMESPACE
