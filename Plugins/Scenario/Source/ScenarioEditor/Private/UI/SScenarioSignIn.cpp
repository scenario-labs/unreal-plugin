// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "ScenarioCreateModel.h"
#include "ScenarioOAuth.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "ScenarioSubsystem.h"
#include "SScenarioPages.h"
#include "ScenarioWidgets.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

#define LOCTEXT_NAMESPACE "ScenarioSignIn"

namespace ScenarioUI
{
	FScenarioSession* Session()
	{
		UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
		return Subsystem ? &Subsystem->Session() : nullptr;
	}

	TSharedRef<SWidget> Section(const FText& Title, const TSharedRef<SWidget>& Body)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SectionHeader(Title)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(Gutter, 2.0f, Gutter, SectionGap))
			[
				Body
			];
	}

	double TodayCU()
	{
		double Total = 0.0;
		FScenarioSession* S = Session();
		if (!S)
		{
			return Total;
		}
		const FDateTime Today = FDateTime::Now().GetDate();
		const FTimespan LocalOffset = FDateTime::Now() - FDateTime::UtcNow();
		for (const FScenarioJobRow& Row : S->Ledger().Rows())
		{
			if ((FDateTime::FromUnixTimestamp(static_cast<int64>(Row.CreatedAt)) + LocalOffset).GetDate() != Today)
			{
				continue;
			}
			if (Row.ChargedCU >= 0.0)
			{
				Total += Row.ChargedCU;
			}
			else if (Row.State != EScenarioJobState::Rejected && Row.State != EScenarioJobState::Canceled && Row.State != EScenarioJobState::Failed)
			{
				Total += Row.QuotedCU;
			}
		}
		return Total;
	}
}

void SScenarioSignIn::Construct(const FArguments& InArgs)
{
	auto Signing = []() { FScenarioSession* S = ScenarioUI::Session(); return S && S->IsSigningIn(); };
	// Sign-in callbacks can arrive after the Studio replaced this view: hold it weakly.
	const TWeakPtr<SScenarioSignIn> Weak = SharedThis(this);
	auto Done = [Weak](const FScenarioReply& Reply)
	{
		if (const TSharedPtr<SScenarioSignIn> This = Weak.Pin())
		{
			This->Status.Reset();
			This->DeviceCode.Reset();
			This->SignInUrl.Reset();
			This->Error = Reply.bOk ? FString() : Reply.Error.Message;
		}
	};

	ChildSlot
	.HAlign(HAlign_Center)
	.VAlign(VAlign_Center)
	[
		SNew(SBox)
		.MaxDesiredWidth(520.0f)
		.Padding(24.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SNew(SImage).Image(FScenarioStyle::Get().GetBrush("Scenario.Mark.Large"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.0f, 12.0f, 0.0f, 20.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Tagline", "Every Scenario model, inside Unreal."))
				.Font(ScenarioUI::Font(ScenarioUI::EText::Display))
			]
			// Browser sign-in
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SNew(SBox)
				.MinDesiredWidth(280.0f)
				.HeightOverride(32.0f)
				.Visibility_Lambda([Signing]() { return Signing() ? EVisibility::Collapsed : EVisibility::Visible; })
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.ToolTipText(LOCTEXT("BrowserTip", "Opens your browser to sign in to Scenario. Nothing is typed into Unreal."))
					.OnClicked_Lambda([this, Done, Weak]()
					{
						if (FScenarioSession* S = ScenarioUI::Session())
						{
							Error.Reset();
							Status = LOCTEXT("Waiting", "Finish signing in in your browser, then come back here.").ToString();
							S->SignInWithBrowser([Weak](const FString& Url)
							{
								if (const TSharedPtr<SScenarioSignIn> This = Weak.Pin())
								{
									This->SignInUrl = Url;
								}
								FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
							}, Done);
						}
						return FReply::Handled();
					})
					[
						SNew(STextBlock)
						.Text(LOCTEXT("SignIn", "Sign in with Scenario"))
						.Font(ScenarioUI::Font(ScenarioUI::EText::Title))
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("OpensBrowser", "Opens your browser."))
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				.ColorAndOpacity(FSlateColor(ScenarioUI::Palette::Dim()))
				.Visibility_Lambda([Signing]() { return Signing() ? EVisibility::Collapsed : EVisibility::Visible; })
			]
			// In progress
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.0f, 8.0f)
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([Signing]() { return Signing() ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FText::FromString(Status); })
					.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(0.0f, 8.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FText::FromString(DeviceCode); })
					.Font(ScenarioUI::Font(ScenarioUI::EText::Display))
					.Visibility_Lambda([this]() { return DeviceCode.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("CopyLink", "Copy link"))
						.ToolTipText(LOCTEXT("CopyLinkTip", "Copies the sign-in link, for a browser on another profile."))
						.Visibility_Lambda([this]() { return SignInUrl.IsEmpty() && DeviceUrl.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
						.OnClicked_Lambda([this]()
						{
							FPlatformApplicationMisc::ClipboardCopy(*(DeviceCode.IsEmpty() ? SignInUrl : DeviceUrl));
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("Cancel", "Cancel"))
						.OnClicked_Lambda([this]()
						{
							if (FScenarioSession* S = ScenarioUI::Session())
							{
								S->CancelSignIn();
							}
							Status.Reset();
							DeviceCode.Reset();
							return FReply::Handled();
						})
					]
				]
			]
			// Other ways
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.0f, 16.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([Signing]() { return Signing() ? EVisibility::Collapsed : EVisibility::Visible; })
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SHyperlink)
					.Text(LOCTEXT("UseKey", "Use an API key instead"))
					.ToolTipText(LOCTEXT("UseKeyTip", "For build machines and shared accounts. The key is stored in the system keychain."))
					.OnNavigate_Lambda([this]() { bShowKey = !bShowKey; })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(16.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SHyperlink)
					.Text(LOCTEXT("UseCode", "Sign in with a code"))
					.ToolTipText(LOCTEXT("UseCodeTip", "Shows a code to enter on scenario.com, for when the browser cannot reach this editor."))
					.OnNavigate_Lambda([this, Done, Weak]()
					{
						if (FScenarioSession* S = ScenarioUI::Session())
						{
							Error.Reset();
							Status = LOCTEXT("EnterCode", "Enter this code in the page that opened:").ToString();
							S->SignInWithDeviceCode([Weak](const FScenarioDeviceCode& Code)
							{
								const FString Url = Code.VerificationUriComplete.IsEmpty() ? Code.VerificationUri : Code.VerificationUriComplete;
								if (const TSharedPtr<SScenarioSignIn> This = Weak.Pin())
								{
									This->DeviceCode = Code.UserCode;
									This->DeviceUrl = Url;
								}
								FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
							}, Done);
						}
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(16.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SHyperlink)
					.Text(LOCTEXT("Create", "Create an account"))
					.ToolTipText(LOCTEXT("CreateTip", "Opens scenario.com."))
					.OnNavigate_Lambda([]() { FPlatformProcess::LaunchURL(TEXT("https://www.scenario.com"), nullptr, nullptr); })
				]
			]
			// API key form
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([this, Signing]() { return bShowKey && !Signing() ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SNew(SEditableTextBox)
					.HintText(LOCTEXT("KeyHint", "API key"))
					.OnTextChanged_Lambda([this](const FText& Text) { Key = Text.ToString().TrimStartAndEnd(); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SNew(SEditableTextBox)
					.HintText(LOCTEXT("SecretHint", "API secret"))
					.IsPassword(true)
					.OnTextChanged_Lambda([this](const FText& Text) { Secret = Text.ToString().TrimStartAndEnd(); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Left)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("SaveKey", "Check and save"))
						.ToolTipText(LOCTEXT("SaveKeyTip", "Checks the key with a free call, then stores it in the system keychain. It is never written to project files."))
						.IsEnabled_Lambda([this]() { return !Key.IsEmpty() && !Secret.IsEmpty(); })
						.OnClicked_Lambda([this, Done]()
						{
							if (FScenarioSession* S = ScenarioUI::Session())
							{
								Error.Reset();
								Status = LOCTEXT("Checking", "Checking the key...").ToString();
								S->SignInWithApiKey(Key, Secret, Done);
								Secret.Reset();
							}
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(12.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([]()
						{
							FScenarioSession* S = ScenarioUI::Session();
							return S ? FText::Format(LOCTEXT("StoredIn", "Stored in: {0}"), FText::FromString(S->DescribeCredentialStore())) : FText::GetEmpty();
						})
						.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
						.ColorAndOpacity(FSlateColor(ScenarioUI::Palette::Dim()))
					]
				]
			]
			// Errors
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					if (!Error.IsEmpty())
					{
						return FText::FromString(Error);
					}
					FScenarioSession* S = ScenarioUI::Session();
					return S && S->GetLastError().Kind != EScenarioErrorKind::None ? FText::FromString(S->GetLastError().Message) : FText::GetEmpty();
				})
				.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
				.ColorAndOpacity(ScenarioUI::Palette::Error())
				.AutoWrapText(true)
			]
			// Privacy
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 24.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Privacy", "Prompts and files you send go to Scenario. Nothing from this plugin ships in your game build."))
					.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
					.ColorAndOpacity(FSlateColor(ScenarioUI::Palette::Dim()))
					.AutoWrapText(true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SHyperlink)
					.Text(LOCTEXT("PrivacyLink", "Privacy"))
					.OnNavigate_Lambda([this]() { bShowPrivacy = !bShowPrivacy; })
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this]() { return bShowPrivacy ? EVisibility::Visible : EVisibility::Collapsed; })
				.Text(LOCTEXT("PrivacyDetail",
					"Sent to Scenario: the prompts and settings of each request, the references you add (viewport captures, files, exported meshes) and price checks.\n"
					"Kept on this machine: your sign-in (system keychain), the job list and downloads (Saved/Scenario), provenance on imported assets.\n"
					"Never sent: other project files, levels, source code. Usage statistics are off unless you turn them on in Editor Preferences."))
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				.ColorAndOpacity(FSlateColor(ScenarioUI::Palette::Dim()))
				.AutoWrapText(true)
			]
		]
	];
}

void SScenarioWorkspacePicker::Construct(const FArguments& InArgs)
{
	if (FScenarioSession* S = ScenarioUI::Session())
	{
		TeamId = S->GetWorkspace().TeamId;
		ProjectId = S->GetWorkspace().ProjectId;
		if (TeamId.IsEmpty() && S->GetTeams().Num() > 0)
		{
			TeamId = S->GetTeams()[0].Id;
		}
	}
	auto FindTeam = [this]() -> const FScenarioTeam*
	{
		FScenarioSession* S = ScenarioUI::Session();
		return S ? S->GetTeams().FindByPredicate([this](const FScenarioTeam& Team) { return Team.Id == TeamId; }) : nullptr;
	};

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PickWorkspace", "Choose the Scenario team and project to generate in. Unreal remembers it for this project."))
			.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
			.AutoWrapText(true)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SComboButton)
				.ButtonContent()
				[
					SNew(STextBlock).Text_Lambda([FindTeam]() { const FScenarioTeam* Team = FindTeam(); return Team ? FText::FromString(Team->Name) : LOCTEXT("Team", "Team"); })
				]
				.OnGetMenuContent_Lambda([this]()
				{
					FMenuBuilder Menu(true, nullptr);
					if (FScenarioSession* S = ScenarioUI::Session())
					{
						for (const FScenarioTeam& Team : S->GetTeams())
						{
							const FString Id = Team.Id;
							Menu.AddMenuEntry(FText::FromString(Team.Name), FText::FromString(Team.Plan), FSlateIcon(),
								FUIAction(FExecuteAction::CreateLambda([this, Id]() { TeamId = Id; ProjectId.Reset(); })));
						}
					}
					return Menu.MakeWidget();
				})
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SComboButton)
				.ButtonContent()
				[
					SNew(STextBlock).Text_Lambda([this, FindTeam]()
					{
						const FScenarioTeam* Team = FindTeam();
						const FScenarioProject* Project = Team ? Team->Projects.FindByPredicate([this](const FScenarioProject& P) { return P.Id == ProjectId; }) : nullptr;
						return Project ? FText::FromString(Project->Name) : LOCTEXT("Project", "Project");
					})
				]
				.OnGetMenuContent_Lambda([this, FindTeam]()
				{
					FMenuBuilder Menu(true, nullptr);
					if (const FScenarioTeam* Team = FindTeam())
					{
						for (const FScenarioProject& Project : Team->Projects)
						{
							const FString Id = Project.Id;
							Menu.AddMenuEntry(FText::FromString(Project.Name), FText::GetEmpty(), FSlateIcon(),
								FUIAction(FExecuteAction::CreateLambda([this, Id]() { ProjectId = Id; })));
						}
					}
					return Menu.MakeWidget();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.Text(LOCTEXT("Use", "Use this project"))
				.IsEnabled_Lambda([this]() { return !TeamId.IsEmpty() && !ProjectId.IsEmpty(); })
				.OnClicked_Lambda([this, FindTeam]()
				{
					FScenarioSession* S = ScenarioUI::Session();
					const FScenarioTeam* Team = FindTeam();
					const FScenarioProject* Project = Team ? Team->Projects.FindByPredicate([this](const FScenarioProject& P) { return P.Id == ProjectId; }) : nullptr;
					if (S && Team && Project)
					{
						FScenarioWorkspace Workspace;
						Workspace.TeamId = Team->Id;
						Workspace.TeamName = Team->Name;
						Workspace.ProjectId = Project->Id;
						Workspace.ProjectName = Project->Name;
						S->SetWorkspace(Workspace);
					}
					return FReply::Handled();
				})
			]
		]
	];
}

#undef LOCTEXT_NAMESPACE
