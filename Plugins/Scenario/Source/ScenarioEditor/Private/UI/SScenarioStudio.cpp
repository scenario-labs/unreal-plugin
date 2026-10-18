// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioStudio.h"

#include "Framework/Commands/UICommandList.h"
#include "ScenarioCU.h"
#include "ScenarioEditorModule.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "SScenarioPages.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
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
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildHeader()
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
			.FillWidth(1.0f)
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
					.MaxDesiredWidth(640.0f)
					[
						SNew(SScenarioWorkspacePicker)
					]
				]
				+ SWidgetSwitcher::Slot()
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
	// The Connection page stays reachable to switch accounts or read diagnostics.
	if (!Session->IsReady() && Page != ScenarioPages::Connection)
	{
		return 1;
	}
	return 2;
}

TSharedRef<SWidget> SScenarioStudio::BuildHeader()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.Padding(FMargin(10.0f, 6.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SImage).Image(FScenarioStyle::Get().GetBrush("Scenario.Mark"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 16.0f, 0.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("Scenario", "Scenario")).Font(FScenarioStyle::Font(11, true))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("WorkspaceTip", "The Scenario team and project every request runs in. Click to change it."))
				.OnClicked_Lambda([this]() { SetPage(ScenarioPages::Connection); return FReply::Handled(); })
				[
					SNew(STextBlock)
					.Text_Lambda([]()
					{
						FScenarioSession* S = ScenarioUI::Session();
						if (!S || !S->IsSignedIn())
						{
							return LOCTEXT("NotSignedIn", "Not signed in");
						}
						return S->GetWorkspace().IsSet() ? FText::FromString(S->GetWorkspace().Label()) : LOCTEXT("ChooseProject", "Choose a project");
					})
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(16.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([]()
				{
					FScenarioSession* S = ScenarioUI::Session();
					const double Used = S && S->IsReady() ? S->GetUsageThisMonth() : -1.0;
					return Used < 0.0 ? FText::GetEmpty() : FText::Format(LOCTEXT("Used", "Used this month: {0}"), FText::FromString(ScenarioCU::Format(Used)));
				})
				.ToolTipText(LOCTEXT("UsedTip", "Creative Units the project used this month, across all members and tools. Scenario has no balance read for plugins."))
				.ColorAndOpacity(FScenarioStyle::Muted())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 12.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Offline", "Offline. Jobs continue on Scenario and resume here when you are back."))
				.ColorAndOpacity(FScenarioStyle::Warning())
				.Visibility_Lambda([]()
				{
					FScenarioSession* S = ScenarioUI::Session();
					const bool bOffline = S && S->IsSignedIn() && (S->GetLastError().Kind == EScenarioErrorKind::Transport || S->GetLastError().Kind == EScenarioErrorKind::Timeout);
					return bOffline ? EVisibility::Visible : EVisibility::Collapsed;
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				FScenarioEditorModule::MakeHeaderStatus()
			]
		];
}

TSharedRef<SWidget> SScenarioStudio::BuildRail()
{
	struct FEntry
	{
		FName Id;
		FText Label;
		FText Tip;
	};
	const TArray<FEntry> Entries = {
		{ ScenarioPages::Create, LOCTEXT("Create", "Create"), LOCTEXT("CreateTip", "Generate images, video, 3D, materials, audio and skies.") },
		{ ScenarioPages::Workflows, LOCTEXT("Workflows", "Workflows"), LOCTEXT("WorkflowsTip", "Run your team's and Scenario's featured workflows.") },
		{ ScenarioPages::Library, LOCTEXT("Library", "Library"), LOCTEXT("LibraryTip", "Your Scenario project's assets, and what this project generated.") },
		{ ScenarioPages::Jobs, LOCTEXT("Jobs", "Jobs"), LOCTEXT("JobsTip", "Every request, its state and cost. Survives restarts.") },
		{ ScenarioPages::Results, LOCTEXT("Results", "Results"), LOCTEXT("ResultsTip", "Finished generations, and jobs on Scenario not in this project yet.") },
		{ ScenarioPages::Connection, LOCTEXT("Connection", "Connection"), LOCTEXT("ConnectionTip", "Account, project, agents and diagnostics.") },
	};
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	for (const FEntry& Entry : Entries)
	{
		const FName Id = Entry.Id;
		Box->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
			.ToolTipText(Entry.Tip)
			.IsChecked_Lambda([this, Id]() { return Page == Id ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Id](ECheckBoxState) { SetPage(Id); })
			[
				SNew(SBox)
				.MinDesiredWidth(110.0f)
				.Padding(FMargin(10.0f, 6.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(STextBlock).Text(Entry.Label)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(STextBlock)
						.Text_Lambda([Id]()
						{
							if (Id != ScenarioPages::Jobs)
							{
								return FText::GetEmpty();
							}
							const int32 Active = CountJobs(true);
							return Active > 0 ? FText::AsNumber(Active) : FText::GetEmpty();
						})
						.ColorAndOpacity_Lambda([Id]() { return Id == ScenarioPages::Jobs && CountJobs(false) > 0 ? FScenarioStyle::Warning() : FScenarioStyle::Muted(); })
					]
				]
			]
		];
	}
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(6.0f)
		[
			Box
		];
}

TSharedRef<SWidget> SScenarioStudio::PageWidget(FName InPage)
{
	if (const TSharedPtr<SWidget>* Found = Pages.Find(InPage))
	{
		return Found->ToSharedRef();
	}
	TSharedPtr<SWidget> Widget;
	if (InPage == ScenarioPages::Workflows)
	{
		Widget = SNew(SScenarioWorkflowsPage);
	}
	else if (InPage == ScenarioPages::Library)
	{
		Widget = SNew(SScenarioLibraryPage);
	}
	else if (InPage == ScenarioPages::Jobs)
	{
		Widget = SNew(SScenarioJobsPage);
	}
	else if (InPage == ScenarioPages::Results)
	{
		Widget = SNew(SScenarioResultsPage);
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
	Page = InPage.IsNone() ? ScenarioPages::Create : InPage;
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
