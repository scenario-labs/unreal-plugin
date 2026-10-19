// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioCreateModel.h"
#include "ScenarioEditorModule.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "ScenarioThumbnails.h"
#include "ScenarioWidgets.h"
#include "SScenarioPages.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioWorkflows"

namespace
{
	const FName TeamSource = TEXT("team");
	const FName FeaturedSource = TEXT("featured");
}

void SScenarioWorkflowsPage::Construct(const FArguments& InArgs)
{
	using namespace ScenarioUI;
	const TSharedRef<FScenarioCreateModel> Model = FScenarioCreateModel::Get();
	ModelHandle = Model->OnChanged.AddSP(this, &SScenarioWorkflowsPage::Refresh);
	Model->LoadWorkflows();

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(PagePadding, PagePadding, PagePadding, 0.0f)
		[
			PageHeader(LOCTEXT("Title", "Workflows"), LOCTEXT("Subtitle", "Several models in one request, built on scenario.com. Pick one to fill its inputs in Create."),
				IconButton(TEXT("refresh"), LOCTEXT("RefreshTip", "Reload the workflows from Scenario. Free."),
					FSimpleDelegate::CreateLambda([]() { FScenarioCreateModel::Get()->LoadWorkflows(true); })))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(PagePadding, 10.0f, PagePadding, 0.0f)
		[
			UnderlineTabs({ { TeamSource, LOCTEXT("Team", "Your team") }, { FeaturedSource, LOCTEXT("Featured", "Featured") } },
				MakeAttributeLambda([this]() { return Source; }),
				[this](FName Value) { Source = Value; Refresh(); })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(PagePadding, 10.0f, PagePadding, 6.0f)
		[
			SNew(SSearchBox)
			.Style(&Style().GetWidgetStyle<FSearchBoxStyle>("Scenario.Search"))
			.HintText(LOCTEXT("Search", "Filter workflows"))
			.OnTextChanged_Lambda([this](const FText& Text) { Query = Text.ToString(); Refresh(); })
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			.Padding(PagePadding, 4.0f, PagePadding, PagePadding)
			[
				SAssignNew(Rows, SVerticalBox)
			]
		]
	];
	Refresh();
}

SScenarioWorkflowsPage::~SScenarioWorkflowsPage()
{
	FScenarioCreateModel::Get()->OnChanged.Remove(ModelHandle);
}

void SScenarioWorkflowsPage::Refresh()
{
	using namespace ScenarioUI;
	const TSharedRef<FScenarioCreateModel> Model = FScenarioCreateModel::Get();
	const bool bTeam = Source == TeamSource;
	TArray<const FScenarioWorkflowItem*> Matches;
	for (const FScenarioWorkflowItem& Item : Model->GetWorkflows())
	{
		if (Item.bTeam == bTeam && (Query.IsEmpty() || Item.Name.Contains(Query) || Item.Description.Contains(Query)
			|| Item.Tags.ContainsByPredicate([this](const FString& Tag) { return Tag.Contains(Query); })))
		{
			Matches.Add(&Item);
		}
	}
	// The model announces many changes (prompt, price): rebuild only when the list changes.
	FString Key = FString::Printf(TEXT("%d|%s|%s|"), Model->IsLoadingWorkflows() ? 1 : 0, *Model->GetWorkflowsError(), *Source.ToString());
	for (const FScenarioWorkflowItem* Item : Matches)
	{
		Key += Item->Id + TEXT(",");
	}
	if (Key == Shown || !Rows.IsValid())
	{
		return;
	}
	Shown = Key;
	Rows->ClearChildren();

	auto Note = [this](const FText& Text, bool bBusy)
	{
		Rows->AddSlot().AutoHeight().Padding(0.0f, 12.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SCircularThrobber).Radius(7.0f).Visibility(bBusy ? EVisibility::Visible : EVisibility::Collapsed)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Text).Font(Font(EText::Body)).ColorAndOpacity(Palette::Dim()).AutoWrapText(true)
			]
		];
	};
	if (Matches.Num() == 0)
	{
		if (Model->IsLoadingWorkflows())
		{
			Note(LOCTEXT("Loading", "Loading workflows..."), true);
		}
		else if (!Model->GetWorkflowsError().IsEmpty())
		{
			Note(FText::FromString(Model->GetWorkflowsError()), false);
		}
		else if (!Query.IsEmpty())
		{
			Note(LOCTEXT("NoMatch", "No workflow matches this filter."), false);
		}
		else
		{
			Note(bTeam ? LOCTEXT("NoTeam", "Your team has no ready workflow in this project yet. Build one on scenario.com, or start from a featured one.")
				: LOCTEXT("NoFeatured", "No featured workflow right now."), false);
		}
		return;
	}
	for (const FScenarioWorkflowItem* Item : Matches)
	{
		Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)[BuildRow(*Item)];
	}
}

TSharedRef<SWidget> SScenarioWorkflowsPage::BuildRow(const FScenarioWorkflowItem& Item)
{
	using namespace ScenarioUI;
	const FScenarioWorkflowItem Copy = Item;
	const FString Description = Item.Description.IsEmpty() ? Item.ShortDescription : Item.Description;
	auto Thumbnail = [Copy]() -> const FSlateBrush*
	{
		return Copy.ThumbnailUrl.IsEmpty() ? nullptr : FScenarioThumbnails::Get().ForUrl(TEXT("workflow:") + Copy.Id, Copy.ThumbnailUrl);
	};
	return SNew(SButton)
		.ButtonStyle(&Style().GetWidgetStyle<FButtonStyle>("Scenario.Button.Row"))
		.ToolTipText(FText::Format(LOCTEXT("RowTip", "Open {0} in Create: fill its inputs, see the price, then run it."), FText::FromString(Item.Name)))
		.OnClicked_Lambda([Copy]()
		{
			const TSharedRef<FScenarioCreateModel> M = FScenarioCreateModel::Get();
			M->EnterWorkflowLane();
			M->SetWorkflow(Copy.Id, Copy.Name, Copy.Description);
			FScenarioEditorModule::OpenStudio(ScenarioPages::Create);
			return FReply::Handled();
		})
		[
			SNew(SBorder)
			.BorderImage(Style().GetBrush("Scenario.Card"))
			.Padding(10.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 12.0f, 0.0f)
				[
					ScenarioUI::Thumbnail(56.0f, MakeAttributeLambda(Thumbnail), FString(TEXT("workflow")))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						Line(FText::FromString(Item.Name), EText::Title)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 3.0f, 0.0f, 0.0f)
					[
						SNew(SBox)
						.MaxDesiredHeight(30.0f)
						[
							SNew(STextBlock)
							.Text(FText::FromString(Description))
							.Font(Font(EText::Meta))
							.ColorAndOpacity(Palette::Dim())
							.AutoWrapText(true)
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 5.0f, 0.0f, 0.0f)
					[
						Line(FText::FromString(FString::Join(Item.Tags, TEXT(" · "))), EText::Meta, true)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(10.0f, 0.0f, 6.0f, 0.0f)
				[
					Pill(Item.InputCount == 1 ? LOCTEXT("OneInput", "1 input") : FText::Format(LOCTEXT("Inputs", "{0} inputs"), FText::AsNumber(Item.InputCount)), ETone::Neutral)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					Icon(TEXT("chevron-right"), 16.0f, Palette::Faint())
				]
			]
		];
}

void SScenarioAgentPage::Construct(const FArguments& InArgs)
{
	using namespace ScenarioUI;
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	Box->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, SectionGap)
	[
		PageHeader(LOCTEXT("AgentTitle", "Agent"), LOCTEXT("AgentSubtitle", "Claude Code, Codex, Cursor and other agents on this computer use Scenario and this project through Scenario MCP."))
	];
	const TArray<TPair<FName, FScenarioEditorModule::FSectionFactory>>& Sections = FScenarioEditorModule::ConnectionSections();
	for (const TPair<FName, FScenarioEditorModule::FSectionFactory>& Section : Sections)
	{
		Box->AddSlot().AutoHeight()[Section.Value()];
	}
	if (Sections.Num() == 0)
	{
		Box->AddSlot().AutoHeight()
		[
			Card(SNew(STextBlock)
				.Text(LOCTEXT("NoAgentModule", "The ScenarioAgent module is not loaded, so agents cannot reach this editor. Enable it with the Scenario plugin and restart the editor."))
				.Font(Font(EText::Body))
				.ColorAndOpacity(Palette::Dim())
				.AutoWrapText(true), FMargin(12.0f))
		];
	}
	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(PagePadding)
		[
			Box
		]
	];
}

#undef LOCTEXT_NAMESPACE
