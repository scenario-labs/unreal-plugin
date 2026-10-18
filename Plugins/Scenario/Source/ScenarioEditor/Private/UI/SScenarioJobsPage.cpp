// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ScenarioCU.h"
#include "ScenarioEditorModule.h"
#include "ScenarioLanes.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "SScenarioJobCard.h"
#include "SScenarioPages.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioJobsPage"

namespace
{
	TSharedRef<SWidget> FilterToggle(const FText& Label, TFunction<bool()> IsOn, TFunction<void()> OnClick)
	{
		return SNew(SBox)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.IsChecked_Lambda([IsOn]() { return IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([OnClick](ECheckBoxState) { OnClick(); })
				[
					SNew(SBox).Padding(FMargin(8.0f, 2.0f))[SNew(STextBlock).Text(Label)]
				]
			];
	}

	bool NeedsAttention(const FScenarioJobRow& Row)
	{
		return Row.State == EScenarioJobState::Unknown || Row.State == EScenarioJobState::Failed || Row.State == EScenarioJobState::Blocked
			|| Row.State == EScenarioJobState::Rejected || (Row.State == EScenarioJobState::Success && !Row.Error.IsEmpty());
	}
}

// Jobs

void SScenarioJobsPage::Construct(const FArguments& InArgs)
{
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		SessionHandle = Session->OnChanged.AddSP(this, &SScenarioJobsPage::Refresh);
	}
	auto Count = [](TFunction<bool(const FScenarioJobRow&)> Test)
	{
		int32 N = 0;
		if (FScenarioSession* Session = ScenarioUI::Session())
		{
			for (const FScenarioJobRow& Row : Session->Ledger().Rows())
			{
				N += Row.State != EScenarioJobState::Dismissed && Test(Row) ? 1 : 0;
			}
		}
		return N;
	};

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				FilterToggle(LOCTEXT("All", "All"), [this]() { return Filter == TEXT("all"); }, [this]() { Filter = TEXT("all"); Shown.Reset(); Refresh(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				FilterToggle(LOCTEXT("Active", "Running"), [this]() { return Filter == TEXT("active"); }, [this]() { Filter = TEXT("active"); Shown.Reset(); Refresh(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox)
				[
					FilterToggle(LOCTEXT("Attention", "Needs attention"), [this]() { return Filter == TEXT("attention"); }, [this]() { Filter = TEXT("attention"); Shown.Reset(); Refresh(); })
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([Count]()
				{
					return FText::Format(LOCTEXT("Counts", "{0} running · {1} need attention"),
						FText::AsNumber(Count([](const FScenarioJobRow& Row) { return Row.IsActive(); })),
						FText::AsNumber(Count([](const FScenarioJobRow& Row) { return NeedsAttention(Row); })));
				})
				.ColorAndOpacity(FScenarioStyle::Muted())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Check", "Check Scenario"))
				.ToolTipText(LOCTEXT("CheckTip", "Looks for requests with an unknown outcome in Scenario's recent jobs. Free."))
				.OnClicked_Lambda([]()
				{
					if (FScenarioSession* Session = ScenarioUI::Session())
					{
						Session->CheckUnresolved([](int32 Linked, const FScenarioError& Error)
						{
							FScenarioEditorModule::Toast(Error.Kind == EScenarioErrorKind::None
								? FText::Format(LOCTEXT("Linked", "{0} request(s) found on Scenario"), FText::AsNumber(Linked))
								: LOCTEXT("CheckFailed", "Could not check Scenario"), FText::FromString(Error.Message), Error.Kind == EScenarioErrorKind::None);
						});
					}
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(List, SListView<FItem>)
			.ListItemsSource(&Items)
			.SelectionMode(ESelectionMode::None)
			.OnGenerateRow(this, &SScenarioJobsPage::MakeRow)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Empty", "No jobs yet. Everything you or an agent generates appears here, and stays after a restart."))
			.ColorAndOpacity(FScenarioStyle::Muted())
			.Visibility_Lambda([this]() { return Items.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed; })
		]
	];
	Refresh();
}

SScenarioJobsPage::~SScenarioJobsPage()
{
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		Session->OnChanged.Remove(SessionHandle);
	}
}

void SScenarioJobsPage::Refresh()
{
	TArray<FString> Ids;
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		for (const FScenarioJobRow& Row : Session->Ledger().Rows())
		{
			if (Row.State == EScenarioJobState::Dismissed)
			{
				continue;
			}
			if ((Filter == TEXT("active") && !Row.IsActive()) || (Filter == TEXT("attention") && !NeedsAttention(Row)))
			{
				continue;
			}
			Ids.Add(Row.LocalId);
		}
	}
	if (Ids == Shown)
	{
		return;
	}
	Shown = Ids;
	Items.Reset();
	for (const FString& Id : Ids)
	{
		Items.Add(MakeShared<FString>(Id));
	}
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SScenarioJobsPage::MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(STableRow<FItem>, Owner)
		.Padding(FMargin(8.0f, 3.0f))
		[
			SNew(SScenarioJobCard).LocalId(*Item)
		];
}

// Results

void SScenarioResultsPage::Construct(const FArguments& InArgs)
{
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		SessionHandle = Session->OnChanged.AddSP(this, &SScenarioResultsPage::Refresh);
	}

	ChildSlot
	[
		SNew(SSplitter)
		.Orientation(Orient_Vertical)
		+ SSplitter::Slot()
		.Value(0.6f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Local", "Results in this project"))
					.Font(FScenarioStyle::Font(10, true))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SComboButton)
					.ButtonContent()
					[
						SNew(STextBlock).Text_Lambda([this]()
						{
							const FScenarioLane* L = FScenarioLanes::Get().Find(Lane);
							return L ? FText::FromString(L->Title) : LOCTEXT("EveryLane", "Every lane");
						})
					]
					.OnGetMenuContent_Lambda([this]()
					{
						FMenuBuilder Menu(true, nullptr);
						Menu.AddMenuEntry(LOCTEXT("EveryLaneEntry", "Every lane"), FText::GetEmpty(), FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([this]() { Lane.Reset(); Shown.Reset(); Refresh(); })));
						for (const FScenarioLane& Entry : FScenarioLanes::Get().All())
						{
							const FString Id = Entry.Id;
							Menu.AddMenuEntry(FText::FromString(Entry.Title), FText::GetEmpty(), FSlateIcon(),
								FUIAction(FExecuteAction::CreateLambda([this, Id]() { Lane = Id; Shown.Reset(); Refresh(); })));
						}
						return Menu.MakeWidget();
					})
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(List, SListView<FItem>)
				.ListItemsSource(&Items)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SScenarioResultsPage::MakeRow)
			]
		]
		+ SSplitter::Slot()
		.Value(0.4f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f)
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
						.Text(LOCTEXT("Cloud", "On Scenario, not in this project"))
						.Font(FScenarioStyle::Font(10, true))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return bCloudLoading ? LOCTEXT("CloudLoading", "Loading the project's recent jobs...") : FText::FromString(CloudStatus); })
						.ColorAndOpacity(FScenarioStyle::Muted())
						.AutoWrapText(true)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("LoadCloud", "Load from Scenario"))
					.ToolTipText(LOCTEXT("LoadCloudTip", "Lists the project's 50 most recent jobs on Scenario, for results made elsewhere or lost locally. Free."))
					.OnClicked_Lambda([this]() { LoadCloud(); return FReply::Handled(); })
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(CloudList, SListView<FCloud>)
				.ListItemsSource(&Cloud)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SScenarioResultsPage::MakeCloudRow)
			]
		]
	];
	Refresh();
}

SScenarioResultsPage::~SScenarioResultsPage()
{
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		Session->OnChanged.Remove(SessionHandle);
	}
}

void SScenarioResultsPage::Refresh()
{
	TArray<FString> Ids;
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		for (const FScenarioJobRow& Row : Session->Ledger().Rows())
		{
			if (Row.State == EScenarioJobState::Success && Row.Kind != TEXT("prompt_spark") && (Lane.IsEmpty() || Row.Lane == Lane))
			{
				Ids.Add(Row.LocalId);
			}
		}
	}
	if (Ids == Shown)
	{
		return;
	}
	Shown = Ids;
	Items.Reset();
	for (const FString& Id : Ids)
	{
		Items.Add(MakeShared<FString>(Id));
	}
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

void SScenarioResultsPage::LoadCloud()
{
	FScenarioSession* Session = ScenarioUI::Session();
	if (!Session || !Session->IsReady())
	{
		CloudStatus = LOCTEXT("SignIn", "Sign in and choose a project first.").ToString();
		return;
	}
	bCloudLoading = true;
	const TWeakPtr<SScenarioResultsPage> Weak = SharedThis(this);
	Session->Api().ListJobs(50, [Weak](const TScenarioResult<TArray<FScenarioJobInfo>>& Result)
	{
		const TSharedPtr<SScenarioResultsPage> This = Weak.Pin();
		FScenarioSession* S = ScenarioUI::Session();
		if (!This.IsValid() || !S)
		{
			return;
		}
		This->bCloudLoading = false;
		This->Cloud.Reset();
		if (!Result.bOk)
		{
			This->CloudStatus = Result.Error.Message;
			This->CloudList->RequestListRefresh();
			return;
		}
		for (const FScenarioJobInfo& Job : Result.Value)
		{
			if (Job.Phase == EScenarioJobPhase::Success && Job.AssetIds.Num() > 0 && !S->Ledger().FindByJob(Job.JobId))
			{
				This->Cloud.Add(MakeShared<FScenarioJobInfo>(Job));
			}
		}
		This->CloudStatus = This->Cloud.Num() == 0
			? LOCTEXT("AllLocal", "Every recent finished job is already in this project's list.").ToString()
			: FText::Format(LOCTEXT("CloudCount", "{0} finished jobs are not in this project's list. Save one to download and import it."), FText::AsNumber(This->Cloud.Num())).ToString();
		This->CloudList->RequestListRefresh();
	});
}

TSharedRef<ITableRow> SScenarioResultsPage::MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(STableRow<FItem>, Owner)
		.Padding(FMargin(8.0f, 3.0f))
		[
			SNew(SScenarioJobCard).LocalId(*Item)
		];
}

TSharedRef<ITableRow> SScenarioResultsPage::MakeCloudRow(FCloud Item, const TSharedRef<STableViewBase>& Owner)
{
	const FScenarioJobInfo Job = *Item;
	const FScenarioLane* GuessLane = FScenarioLanes::Get().LaneForModel(Job.ModelId);
	const FString LaneId = GuessLane ? GuessLane->Id : FString();
	const FString When = Job.CreatedAt > 0.0 ? FDateTime::FromUnixTimestamp(static_cast<int64>(Job.CreatedAt)).ToString(TEXT("%Y-%m-%d %H:%M")) : FString();
	return SNew(STableRow<FCloud>, Owner)
		.Padding(FMargin(8.0f, 3.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("%s · %d file(s) · %s%s"),
					Job.ModelId.IsEmpty() ? *Job.WorkflowId : *Job.ModelId, Job.AssetIds.Num(), *When,
					Job.ChargedCU >= 0.0 ? *(TEXT(" · ") + ScenarioCU::Format(Job.ChargedCU)) : TEXT(""))))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Save", "Save for recovery"))
				.ToolTipText(LOCTEXT("SaveTip", "Adds this job to the project's list and downloads its files. Nothing is charged."))
				.OnClicked_Lambda([this, Job, LaneId]()
				{
					if (FScenarioSession* Session = ScenarioUI::Session())
					{
						Session->Recover(Job, LaneId);
						Cloud.RemoveAll([&Job](const FCloud& Entry) { return Entry->JobId == Job.JobId; });
						CloudList->RequestListRefresh();
					}
					return FReply::Handled();
				})
			]
		];
}

#undef LOCTEXT_NAMESPACE
