// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioCreateModel.h"
#include "ScenarioJson.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "SScenarioForm.h"
#include "SScenarioPages.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioWorkflows"

void SScenarioWorkflowsPage::Construct(const FArguments& InArgs)
{
	Model = FScenarioCreateModel::MakeForWorkflows();
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();

	ChildSlot
	[
		SNew(SSplitter)
		+ SSplitter::Slot()
		.Value(0.35f)
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
					.Text_Lambda([this]() { return bLoading ? LOCTEXT("Loading", "Loading workflows...") : FText::FromString(Status); })
					.ColorAndOpacity(FScenarioStyle::Muted())
					.AutoWrapText(true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Refresh", "Refresh"))
					.ToolTipText(LOCTEXT("RefreshTip", "Lists your team's ready workflows and Scenario's featured ones. Free."))
					.OnClicked_Lambda([this]() { Load(); return FReply::Handled(); })
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(List, SListView<FItem>)
				.ListItemsSource(&Items)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SScenarioWorkflowsPage::MakeRow)
				.OnSelectionChanged_Lambda([M](FItem Item, ESelectInfo::Type)
				{
					if (Item.IsValid())
					{
						M->SetWorkflow(Item->Id, Item->Name, Item->Description);
					}
				})
			]
		]
		+ SSplitter::Slot()
		.Value(0.65f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			.Padding(12.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text_Lambda([M]() { return M->GetModelId().IsEmpty() ? LOCTEXT("Pick", "Pick a workflow on the left.") : FText::FromString(M->GetModelName()); })
					.Font(FScenarioStyle::Font(12, true))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 10.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([M]() { return FText::FromString(M->GetModelNote()); })
					.ColorAndOpacity(FScenarioStyle::Muted())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SBox)
					.Visibility_Lambda([M]() { return M->GetModelId().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SScenarioForm).Model(Model)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 8.0f)
						[
							SNew(STextBlock)
							.Text_Lambda([M]() { return FText::Format(LOCTEXT("SavesTo", "Saves to {0}. {1}"), FText::FromString(M->DestinationText()), FText::FromString(M->PlacementText())); })
							.ColorAndOpacity(FScenarioStyle::Muted())
							.AutoWrapText(true)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							ScenarioUI::GenerateButton(M, LOCTEXT("Hint", "Runs every step on Scenario"))
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 6.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text_Lambda([M]() { return FText::FromString(M->GetLastError()); })
							.ColorAndOpacity(FScenarioStyle::Error())
							.AutoWrapText(true)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 6.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("Where", "Results appear in Create's results, Jobs and Results."))
							.ColorAndOpacity(FScenarioStyle::Muted())
						]
					]
				]
			]
		]
	];
	Load();
}

SScenarioWorkflowsPage::~SScenarioWorkflowsPage()
{
	if (Model.IsValid())
	{
		Model->Shutdown();
	}
}

void SScenarioWorkflowsPage::Load()
{
	FScenarioSession* Session = ScenarioUI::Session();
	if (!Session || !Session->IsReady() || bLoading)
	{
		Status = LOCTEXT("SignIn", "Sign in and choose a project to see workflows.").ToString();
		return;
	}
	bLoading = true;
	Items.Reset();
	List->RequestListRefresh();

	const TWeakPtr<SScenarioWorkflowsPage> Weak = SharedThis(this);
	auto Parse = [](const FScenarioReply& Reply, bool bTeam, TArray<FItem>& Out)
	{
		for (const TSharedPtr<FJsonValue>& Value : ScenarioJson::GetArray(Reply.Value, TEXT("workflows")))
		{
			const TSharedPtr<FJsonObject> Row = Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			const FString Id = ScenarioJson::GetString(Row, TEXT("id"));
			if (Id.IsEmpty() || Out.ContainsByPredicate([&Id](const FItem& Item) { return Item->Id == Id; }))
			{
				continue;
			}
			FItem Item = MakeShared<FWorkflow>();
			Item->Id = Id;
			Item->Name = ScenarioJson::GetString(Row, TEXT("name"), Id);
			Item->Description = ScenarioJson::GetString(Row, TEXT("description"), ScenarioJson::GetString(Row, TEXT("shortDescription")));
			Item->bTeam = bTeam;
			Out.Add(Item);
		}
	};
	Session->Api().ListWorkflows(TEXT("private"), TEXT("ready"), 50, FString(), {}, [Weak, Parse](const FScenarioReply& Team)
	{
		const TSharedPtr<SScenarioWorkflowsPage> This = Weak.Pin();
		FScenarioSession* S = ScenarioUI::Session();
		if (!This.IsValid() || !S)
		{
			return;
		}
		Parse(Team, true, This->Items);
		const FString TeamError = Team.bOk ? FString() : Team.Error.Message;
		S->Api().ListWorkflows(TEXT("public"), TEXT("ready"), 30, FString(), { TEXT("featured") }, [Weak, Parse, TeamError](const FScenarioReply& Featured)
		{
			const TSharedPtr<SScenarioWorkflowsPage> Page = Weak.Pin();
			if (!Page.IsValid())
			{
				return;
			}
			Parse(Featured, false, Page->Items);
			Page->bLoading = false;
			const FString Error = !TeamError.IsEmpty() ? TeamError : (Featured.bOk ? FString() : Featured.Error.Message);
			Page->Status = !Error.IsEmpty() ? Error
				: Page->Items.Num() == 0 ? LOCTEXT("None", "No ready workflow in this project yet. Build one on scenario.com.").ToString()
				: FText::Format(LOCTEXT("Count", "{0} workflows"), FText::AsNumber(Page->Items.Num())).ToString();
			Page->List->RequestListRefresh();
		});
	});
}

TSharedRef<ITableRow> SScenarioWorkflowsPage::MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(STableRow<FItem>, Owner)
		.Padding(FMargin(8.0f, 5.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock).Text(FText::FromString(Item->Name)).Font(FScenarioStyle::Font(9, true))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock)
					.Text(Item->bTeam ? LOCTEXT("Team", "Team") : LOCTEXT("Featured", "Featured"))
					.ColorAndOpacity(FScenarioStyle::Muted())
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Description.Left(140)))
				.ColorAndOpacity(FScenarioStyle::Muted())
				.AutoWrapText(true)
			]
		];
}

#undef LOCTEXT_NAMESPACE
