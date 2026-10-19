// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioStudio.h"

#include "Framework/Commands/UICommandList.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ScenarioCreateModel.h"
#include "ScenarioEditorModule.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "ScenarioWidgets.h"
#include "SScenarioPages.h"
#include "SWarningOrErrorBox.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioStudio"

namespace
{
	TWeakPtr<SScenarioStudio> OpenStudioWidget;

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
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
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
					BuildTopBar()
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
					SAssignNew(PageBox, SBox)
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
	// Account and agents stays reachable to switch accounts or read diagnostics.
	if (!Session->IsReady() && Page != ScenarioPages::Connection)
	{
		return 1;
	}
	return 2;
}

TSharedRef<SWidget> SScenarioStudio::BuildTopBar()
{
	TSharedRef<SSegmentedControl<FName>> Nav = SNew(SSegmentedControl<FName>)
		.UniformPadding(FMargin(12.0f, 3.0f))
		.Value_Lambda([this]() { return Page == ScenarioPages::Connection ? NAME_None : Page; })
		.OnValueChanged_Lambda([this](FName Value) { SetPage(Value); })
		+ SSegmentedControl<FName>::Slot(ScenarioPages::Create)
			.Text(LOCTEXT("Create", "Create"))
			.ToolTip(LOCTEXT("CreateTip", "Generate images, 3D, materials, skies, audio, video, or run a workflow."))
		+ SSegmentedControl<FName>::Slot(ScenarioPages::Library)
			.Text(LOCTEXT("Library", "Library"))
			.ToolTip(LOCTEXT("LibraryTip", "Assets on Scenario and the ones imported into this project."))
		+ SSegmentedControl<FName>::Slot(ScenarioPages::Jobs)
			.Text_Lambda([]()
			{
				const int32 Active = CountJobs(true);
				return Active > 0 ? FText::Format(LOCTEXT("JobsCount", "Jobs {0}"), FText::AsNumber(Active)) : LOCTEXT("Jobs", "Jobs");
			})
			.Icon_Lambda([]() { return CountJobs(false) > 0 ? FAppStyle::GetBrush("Icons.WarningWithColor") : nullptr; })
			.ToolTip_Lambda([]()
			{
				const int32 Unresolved = CountJobs(false);
				const int32 Active = CountJobs(true);
				if (Unresolved > 0)
				{
					return FText::Format(LOCTEXT("JobsAttentionTip", "{0} need attention. Every request, its state and cost."), FText::AsNumber(Unresolved));
				}
				return Active > 0 ? FText::Format(LOCTEXT("JobsRunningTip", "{0} running. Every request, its state and cost."), FText::AsNumber(Active))
					: LOCTEXT("JobsTip", "Every request, its state and cost. Survives restarts.");
			});

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
		.Padding(FMargin(ScenarioUI::Gutter, 4.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				Nav
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNullWidget::NullWidget
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.MaxWidth(220.0f)
			.VAlign(VAlign_Center)
			.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
			[
				SNew(SComboButton)
				.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
				.HasDownArrow(true)
				.ToolTipText_Lambda([]()
				{
					FScenarioSession* S = ScenarioUI::Session();
					return S ? FText::Format(LOCTEXT("ProjectTip", "Every request runs in this Scenario project (team {0}). Click to switch."), FText::FromString(S->GetWorkspace().TeamName))
						: FText::GetEmpty();
				})
				.OnGetMenuContent(this, &SScenarioStudio::BuildProjectMenu)
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text_Lambda([]()
					{
						FScenarioSession* S = ScenarioUI::Session();
						return S && S->GetWorkspace().IsSet() ? FText::FromString(S->GetWorkspace().ProjectName) : LOCTEXT("ChooseProject", "Choose a project");
					})
					.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
			]
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
				ScenarioUI::IconButton(TEXT("Icons.Settings"), LOCTEXT("AccountTip", "Open Account and agents: sign-in, project, usage, Scenario MCP for agents, diagnostics."),
					FSimpleDelegate::CreateLambda([this]() { SetPage(Page == ScenarioPages::Connection ? ScenarioPages::Create : ScenarioPages::Connection); }))
			]
		];
}

TSharedRef<SWidget> SScenarioStudio::BuildProjectMenu()
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
	else if (InPage == ScenarioPages::Connection)
	{
		Widget = SNew(SScenarioConnectionPage);
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
	if (Target == ScenarioPages::Workflows)
	{
		FScenarioCreateModel::Get()->EnterWorkflowLane();
		Target = ScenarioPages::Create;
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
