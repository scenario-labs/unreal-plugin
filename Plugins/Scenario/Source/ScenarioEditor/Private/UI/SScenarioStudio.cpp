// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioStudio.h"

#include "Framework/Commands/UICommandList.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ScenarioCreateModel.h"
#include "ScenarioCU.h"
#include "ScenarioEditorModule.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "ScenarioWidgets.h"
#include "SScenarioPages.h"
#include "SWarningOrErrorBox.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioStudio"

namespace
{
	TWeakPtr<SScenarioStudio> OpenStudioWidget;

	constexpr float NarrowWidth = 560.0f;
	constexpr float RailWidth = 148.0f;
	constexpr float RailNarrowWidth = 52.0f;
	constexpr float HeaderHeight = 42.0f;

	int32 CountJobs(bool bActive)
	{
		int32 Count = 0;
		if (FScenarioSession* Session = ScenarioUI::Session())
		{
			for (const FScenarioJobRow& Row : Session->Ledger().Rows())
			{
				Count += (bActive ? Row.IsActive() : Row.IsUnresolved()) ? 1 : 0;
			}
		}
		return Count;
	}

	bool IsOffline()
	{
		FScenarioSession* S = ScenarioUI::Session();
		return S && S->IsSignedIn() && (S->GetLastError().Kind == EScenarioErrorKind::Transport || S->GetLastError().Kind == EScenarioErrorKind::Timeout);
	}

	struct FRailEntry
	{
		FName Page;
		FText Label;
		FName Icon;
		FText Tooltip;
	};

	const TArray<FRailEntry>& RailEntries()
	{
		static const TArray<FRailEntry> Entries = {
			{ ScenarioPages::Create, LOCTEXT("Create", "Create"), TEXT("create"), LOCTEXT("CreateTip", "Generate images, 3D, materials, skies, audio and video, priced before you spend.") },
			{ ScenarioPages::Library, LOCTEXT("Library", "Library"), TEXT("library"), LOCTEXT("LibraryTip", "Assets on Scenario and the ones imported into this project.") },
			{ ScenarioPages::Jobs, LOCTEXT("Jobs", "Jobs"), TEXT("jobs"), LOCTEXT("JobsTip", "Every request, its state and cost. Survives restarts.") },
			{ ScenarioPages::Workflows, LOCTEXT("Workflows", "Workflows"), TEXT("workflows"), LOCTEXT("WorkflowsTip", "Your team's workflows and Scenario's featured ones: several models in one request.") },
			{ ScenarioPages::Agent, LOCTEXT("Agent", "Agent"), TEXT("agent"), LOCTEXT("AgentTip", "Scenario MCP in this editor: connect Claude Code, Codex or Cursor, permissions and the agent log.") },
			{ ScenarioPages::Settings, LOCTEXT("Settings", "Settings"), TEXT("settings"), LOCTEXT("SettingsTip", "Account, project, import defaults and diagnostics.") },
		};
		return Entries;
	}
}

TSharedPtr<SScenarioStudio> SScenarioStudio::Current()
{
	return OpenStudioWidget.Pin();
}

void SScenarioStudio::Construct(const FArguments& InArgs)
{
	OpenStudioWidget = SharedThis(this);
	Page = InArgs._Page.IsNone() ? ScenarioPages::Create : InArgs._Page;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(ScenarioUI::Style().GetBrush("Scenario.Panel"))
		.ForegroundColor(ScenarioUI::Palette::Text())
		.Padding(0.0f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex(this, &SScenarioStudio::GateIndex)
			+ SWidgetSwitcher::Slot()
			[
				SNew(SScenarioSignIn)
			]
			+ SWidgetSwitcher::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.Padding(24.0f)
			[
				SNew(SBox)
				.MaxDesiredWidth(560.0f)
				[
					SNew(SScenarioWorkspacePicker)
				]
			]
			+ SWidgetSwitcher::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildHeader()
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					ScenarioUI::Divider()
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(ScenarioUI::Gutter, ScenarioUI::RowGap))
				[
					SNew(SWarningOrErrorBox)
					.MessageStyle(EMessageStyle::Warning)
					.Message(LOCTEXT("Offline", "Offline. Jobs continue on Scenario and resume here when you are back."))
					.Visibility_Lambda([]() { return IsOffline() ? EVisibility::Visible : EVisibility::Collapsed; })
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						BuildRail()
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						ScenarioUI::Divider(true)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SAssignNew(PageBox, SBox)
					]
				]
			]
		]
	];
	SetPage(Page);
}

int32 SScenarioStudio::GateIndex() const
{
	FScenarioSession* Session = ScenarioUI::Session();
	if (!Session || !Session->IsSignedIn())
	{
		return 0;
	}
	// Settings and Agent stay reachable to switch accounts or read diagnostics.
	if (!Session->IsReady() && Page != ScenarioPages::Settings && Page != ScenarioPages::Agent)
	{
		return 1;
	}
	return 2;
}

bool SScenarioStudio::IsNarrow() const
{
	const float Width = GetCachedGeometry().GetLocalSize().X;
	return Width > 0.0f && Width < NarrowWidth;
}

TSharedRef<SWidget> SScenarioStudio::BuildHeader()
{
	using namespace ScenarioUI;
	return SNew(SBox)
		.HeightOverride(HeaderHeight)
		.Padding(FMargin(12.0f, 0.0f, 8.0f, 0.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SImage)
				.Image(Style().GetBrush("Scenario.Mark"))
				.ToolTipText(LOCTEXT("MarkTip", "Scenario"))
			]
			// The project every request runs in.
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Center)
			[
				SNew(SComboButton)
				.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
				.ButtonStyle(&Style().GetWidgetStyle<FButtonStyle>("Scenario.Button.Ghost"))
				.HasDownArrow(false)
				.ToolTipText_Lambda([]()
				{
					FScenarioSession* S = Session();
					return S && S->GetWorkspace().IsSet()
						? FText::Format(LOCTEXT("ProjectTip", "Every request runs in {0} (team {1}). Click to switch."), FText::FromString(S->GetWorkspace().ProjectName), FText::FromString(S->GetWorkspace().TeamName))
						: LOCTEXT("ProjectNoneTip", "Choose the Scenario project requests run in.");
				})
				.OnGetMenuContent(this, &SScenarioStudio::BuildProjectMenu)
				.ButtonContent()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([]()
						{
							FScenarioSession* S = Session();
							if (S && S->GetWorkspace().IsSet())
							{
								return FText::FromString(S->GetWorkspace().ProjectName);
							}
							return S && S->GetAuthMode() == EScenarioAuthMode::ApiKey ? LOCTEXT("ApiKey", "API key") : LOCTEXT("ChooseProject", "Choose a project");
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(4.0f, 1.0f, 0.0f, 0.0f)
					[
						Icon(TEXT("chevron-down"), 12.0f, Palette::Dim())
					]
				]
			]
			// What this editor spent today.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SBorder)
				.BorderImage(Style().GetBrush("Scenario.Usage"))
				.Padding(FMargin(10.0f, 3.0f))
				.ToolTipText(LOCTEXT("UsageTip", "Creative Units spent today from this editor (charged, or quoted until Scenario reports the charge). The project's month is on Settings and Jobs."))
				.Visibility_Lambda([this]() { return !IsNarrow() && TodayCU() > 0.0 ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					SNew(STextBlock)
					.Text_Lambda([]() { return FText::Format(LOCTEXT("UsageToday", "{0} today"), FText::FromString(ScenarioCU::Format(TodayCU()))); })
					.Font(Font(EText::Meta))
					.ColorAndOpacity(Palette::Dim())
				]
			]
			// Agents: a dot, green when Scenario MCP is on.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				FScenarioEditorModule::MakeHeaderStatus()
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SComboButton)
				.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
				.ButtonStyle(&Style().GetWidgetStyle<FButtonStyle>("Scenario.Button.Ghost.Small"))
				.HasDownArrow(false)
				.ContentPadding(FMargin(4.0f))
				.ToolTipText(LOCTEXT("AccountTip", "Account"))
				.OnGetMenuContent(this, &SScenarioStudio::BuildAccountMenu)
				.ButtonContent()
				[
					Icon(TEXT("user"), 16.0f)
				]
			]
		];
}

TSharedRef<SWidget> SScenarioStudio::BuildRail()
{
	using namespace ScenarioUI;
	TSharedRef<SVerticalBox> Items = SNew(SVerticalBox);
	for (const FRailEntry& Entry : RailEntries())
	{
		const FName Target = Entry.Page;
		const bool bJobs = Target == ScenarioPages::Jobs;
		auto IsCurrent = [this, Target]() { return Page == Target; };
		auto MakeBadge = []() -> TSharedRef<SWidget>
		{
			return SNew(SBorder)
				.BorderImage(Style().GetBrush("Scenario.Badge"))
				.BorderBackgroundColor_Lambda([]() { return CountJobs(false) > 0 ? FSlateColor(Palette::Warning()) : FSlateColor(Palette::Info()); })
				.Padding(FMargin(4.0f, 0.0f))
				.HAlign(HAlign_Center)
				.Visibility_Lambda([]() { return CountJobs(true) + CountJobs(false) > 0 ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				[
					SNew(SBox)
					.MinDesiredWidth(8.0f)
					.HAlign(HAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([]() { return FText::AsNumber(CountJobs(true) + CountJobs(false)); })
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 7))
						.ColorAndOpacity(Palette::OnPrimary())
					]
				];
		};
		Items->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 2.0f)
			[
				SNew(SCheckBox)
				.Style(&Style().GetWidgetStyle<FCheckBoxStyle>("Scenario.Rail.Item"))
				.AddMetaData<FTagMetaData>(FTagMetaData(FName(*(TEXT("Scenario.Rail.") + Target.ToString()))))
				.ToolTipText_Lambda([this, Entry, bJobs]()
				{
					if (bJobs && CountJobs(false) > 0)
					{
						return FText::Format(LOCTEXT("JobsAttentionTip", "{0} need a look. Every request, its state and cost."), FText::AsNumber(CountJobs(false)));
					}
					if (bJobs && CountJobs(true) > 0)
					{
						return FText::Format(LOCTEXT("JobsRunningTip", "{0} running. Every request, its state and cost."), FText::AsNumber(CountJobs(true)));
					}
					return IsNarrow() ? FText::Format(LOCTEXT("NarrowTip", "{0}: {1}"), Entry.Label, Entry.Tooltip) : Entry.Tooltip;
				})
				.IsChecked_Lambda([IsCurrent]() { return IsCurrent() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this, Target](ECheckBoxState) { SetPage(Target); })
				[
					SNew(SBox)
					.HeightOverride(36.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SOverlay)
							+ SOverlay::Slot()
							[
								Icon(Entry.Icon, 18.0f)
							]
							// Narrow: the badge sits on the icon.
							+ SOverlay::Slot()
							.HAlign(HAlign_Right)
							.VAlign(VAlign_Top)
							.Padding(0.0f, -6.0f, -10.0f, 0.0f)
							[
								SNew(SBox)
								.Visibility_Lambda([this, bJobs]() { return bJobs && IsNarrow() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
								[
									bJobs ? MakeBadge() : SNullWidget::NullWidget
								]
							]
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						.Padding(10.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(Entry.Label)
							.Font_Lambda([IsCurrent]() { return FCoreStyle::GetDefaultFontStyle(IsCurrent() ? "Bold" : "Regular", 9); })
							.Visibility_Lambda([this]() { return IsNarrow() ? EVisibility::Collapsed : EVisibility::Visible; })
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SBox)
							.Visibility_Lambda([this, bJobs]() { return bJobs && !IsNarrow() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
							[
								bJobs ? MakeBadge() : SNullWidget::NullWidget
							]
						]
					]
				]
			];
	}
	return SNew(SBorder)
		.BorderImage(Style().GetBrush("Scenario.Rail"))
		.Padding(8.0f)
		[
			SNew(SBox)
			.WidthOverride_Lambda([this]() { return FOptionalSize(IsNarrow() ? RailNarrowWidth - 16.0f : RailWidth - 16.0f); })
			[
				Items
			]
		];
}

TSharedRef<SWidget> SScenarioStudio::BuildProjectMenu()
{
	return ScenarioUI::ProjectMenu();
}

TSharedRef<SWidget> ScenarioUI::ProjectMenu()
{
	FMenuBuilder Menu(true, nullptr);
	FScenarioSession* Session = ScenarioUI::Session();
	if (!Session)
	{
		return Menu.MakeWidget();
	}
	if (Session->GetAuthMode() != EScenarioAuthMode::OAuth)
	{
		Menu.AddMenuEntry(LOCTEXT("KeyScope", "An API key belongs to one project"), LOCTEXT("KeyScopeTip", "Sign in with Scenario to switch projects."), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
		return Menu.MakeWidget();
	}
	for (const FScenarioTeam& Team : Session->GetTeams())
	{
		Menu.BeginSection(NAME_None, FText::FromString(Team.Name));
		for (const FScenarioProject& Project : Team.Projects)
		{
			FScenarioWorkspace Workspace;
			Workspace.TeamId = Team.Id;
			Workspace.TeamName = Team.Name;
			Workspace.ProjectId = Project.Id;
			Workspace.ProjectName = Project.Name;
			Menu.AddMenuEntry(FText::FromString(Project.Name), FText::Format(LOCTEXT("UseProjectTip", "Generate in {0} / {1}. Running jobs continue and stay in Jobs."), FText::FromString(Team.Name), FText::FromString(Project.Name)), FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([Workspace]()
					{
						if (FScenarioSession* S = ScenarioUI::Session())
						{
							S->SetWorkspace(Workspace);
						}
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([Workspace]()
					{
						FScenarioSession* S = ScenarioUI::Session();
						return S && S->GetWorkspace().ProjectId == Workspace.ProjectId;
					})),
				NAME_None, EUserInterfaceActionType::RadioButton);
		}
		Menu.EndSection();
	}
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SScenarioStudio::BuildAccountMenu()
{
	FMenuBuilder Menu(true, nullptr);
	FScenarioSession* Session = ScenarioUI::Session();
	const FText Who = !Session ? FText::GetEmpty()
		: Session->GetAuthMode() == EScenarioAuthMode::OAuth ? LOCTEXT("WhoOAuth", "Signed in with your Scenario account")
		: LOCTEXT("WhoKey", "Signed in with an API key");
	Menu.BeginSection(NAME_None, Who);
	Menu.AddMenuEntry(LOCTEXT("OpenSettings", "Settings"), LOCTEXT("OpenSettingsTip", "Account, project, import defaults and diagnostics."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { SetPage(ScenarioPages::Settings); })));
	Menu.AddMenuEntry(LOCTEXT("OpenAgent", "Agent"), LOCTEXT("OpenAgentTip", "Scenario MCP for agents in this editor."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { SetPage(ScenarioPages::Agent); })));
	Menu.AddMenuEntry(LOCTEXT("SignOut", "Sign out"), LOCTEXT("SignOutTip", "Removes the saved sign-in from this machine. Jobs and imported assets stay."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			if (FScenarioSession* S = ScenarioUI::Session())
			{
				S->SignOut();
			}
		})));
	Menu.EndSection();
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SScenarioStudio::PageWidget(FName InPage)
{
	if (const TSharedPtr<SWidget>* Found = Pages.Find(InPage))
	{
		return Found->ToSharedRef();
	}
	TSharedPtr<SWidget> Widget;
	if (InPage == ScenarioPages::Library)
	{
		Widget = SNew(SScenarioLibraryPage);
	}
	else if (InPage == ScenarioPages::Jobs)
	{
		Widget = SNew(SScenarioJobsPage);
	}
	else if (InPage == ScenarioPages::Workflows)
	{
		Widget = SNew(SScenarioWorkflowsPage);
	}
	else if (InPage == ScenarioPages::Agent)
	{
		Widget = SNew(SScenarioAgentPage);
	}
	else if (InPage == ScenarioPages::Settings)
	{
		Widget = SNew(SScenarioSettingsPage);
	}
	else
	{
		Widget = SNew(SScenarioCreatePage);
	}
	Pages.Add(InPage, Widget);
	return Widget.ToSharedRef();
}

void SScenarioStudio::SetPage(FName InPage)
{
	// Older page names still open the right place: menus, toasts and agents use them.
	FName Target = InPage.IsNone() ? ScenarioPages::Create : InPage;
	if (Target == ScenarioPages::Connection)
	{
		Target = ScenarioPages::Settings;
	}
	if (Target == ScenarioPages::Results)
	{
		Target = ScenarioPages::Jobs;
		StaticCastSharedRef<SScenarioJobsPage>(PageWidget(ScenarioPages::Jobs))->SetFilter(TEXT("ready"));
	}
	Page = Target;
	if (PageBox.IsValid())
	{
		PageBox->SetContent(PageWidget(Page));
	}
}

TSharedPtr<SScenarioCreatePage> SScenarioStudio::GetCreatePage()
{
	return StaticCastSharedRef<SScenarioCreatePage>(PageWidget(ScenarioPages::Create));
}

FReply SScenarioStudio::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	const TSharedPtr<FUICommandList> Commands = FScenarioEditorModule::Get().GetCommands();
	if (Commands.IsValid() && Commands->ProcessCommandBindings(KeyEvent))
	{
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(Geometry, KeyEvent);
}

#undef LOCTEXT_NAMESPACE
