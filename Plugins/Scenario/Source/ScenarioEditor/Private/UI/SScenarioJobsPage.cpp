// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ScenarioCU.h"
#include "ScenarioEditorModule.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "ScenarioWidgets.h"
#include "SScenarioJobCard.h"
#include "SScenarioPages.h"
#include "SSimpleComboButton.h"
#include "SWarningOrErrorBox.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioJobsPage"

namespace
{
	bool NeedsAttention(const FScenarioJobRow& Row)
	{
		return Row.State == EScenarioJobState::Unknown || Row.State == EScenarioJobState::Failed || Row.State == EScenarioJobState::Blocked
			|| Row.State == EScenarioJobState::Rejected || (Row.State == EScenarioJobState::Success && !Row.Error.IsEmpty());
	}

	bool IsReady(const FScenarioJobRow& Row)
	{
		return Row.State == EScenarioJobState::Success && Row.Error.IsEmpty() && Row.Kind != TEXT("prompt_spark");
	}

	int32 Count(TFunction<bool(const FScenarioJobRow&)> Test)
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
	}

	/** What this editor's jobs cost today: charged amounts, or quotes until Scenario reports the charge. */
	double TodayCU()
	{
		double Total = 0.0;
		FScenarioSession* Session = ScenarioUI::Session();
		if (!Session)
		{
			return Total;
		}
		const FDateTime Today = FDateTime::Now().GetDate();
		const FTimespan LocalOffset = FDateTime::Now() - FDateTime::UtcNow();
		for (const FScenarioJobRow& Row : Session->Ledger().Rows())
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

	const TArray<TPair<FString, FText>>& Kinds()
	{
		static const TArray<TPair<FString, FText>> List = {
			{ FString(), LOCTEXT("AllTypes", "All types") },
			{ TEXT("image"), LOCTEXT("Images", "Images") },
			{ TEXT("3d"), LOCTEXT("Meshes", "3D") },
			{ TEXT("material"), LOCTEXT("Materials", "Materials") },
			{ TEXT("sky"), LOCTEXT("Skies", "Skies") },
			{ TEXT("audio"), LOCTEXT("Audio", "Audio") },
			{ TEXT("video"), LOCTEXT("Video", "Video") },
		};
		return List;
	}
}

void SScenarioJobsPage::Construct(const FArguments& InArgs)
{
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		SessionHandle = Session->OnChanged.AddSP(this, &SScenarioJobsPage::Refresh);
	}
	auto Segment = [](const FText& Label, int32 N) { return N > 0 ? FText::Format(LOCTEXT("SegmentCount", "{0} {1}"), Label, FText::AsNumber(N)) : Label; };

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(ScenarioUI::Gutter)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SSegmentedControl<FString>)
				.UniformPadding(FMargin(8.0f, 2.0f))
				.Value_Lambda([this]() { return Filter; })
				.OnValueChanged_Lambda([this](const FString& Value) { SetFilter(Value); })
				+ SSegmentedControl<FString>::Slot(TEXT("all"))
					.Text_Lambda([Segment]() { return Segment(LOCTEXT("All", "All"), Count([](const FScenarioJobRow&) { return true; })); })
				+ SSegmentedControl<FString>::Slot(TEXT("active"))
					.Text_Lambda([Segment]() { return Segment(LOCTEXT("Running", "Running"), Count([](const FScenarioJobRow& Row) { return Row.IsActive(); })); })
				+ SSegmentedControl<FString>::Slot(TEXT("ready"))
					.Text_Lambda([Segment]() { return Segment(LOCTEXT("Ready", "Ready"), Count([](const FScenarioJobRow& Row) { return IsReady(Row) && !Row.bImported; })); })
					.ToolTip(LOCTEXT("ReadyTip", "Finished results not imported yet."))
				+ SSegmentedControl<FString>::Slot(TEXT("attention"))
					.Text_Lambda([Segment]() { return Segment(LOCTEXT("Attention", "Needs attention"), Count(NeedsAttention)); })
					.ToolTip(LOCTEXT("AttentionTip", "Failed, blocked, not confirmed or stopped downloads."))
					.Visibility_Lambda([this]() { return Filter == TEXT("attention") || Count(NeedsAttention) > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
			[
				SNew(SSimpleComboButton)
				.UsesSmallText(true)
				.HasDownArrow(true)
				.Text_Lambda([this]()
				{
					const TPair<FString, FText>* Found = Kinds().FindByPredicate([this](const TPair<FString, FText>& Entry) { return Entry.Key == Kind; });
					return Found ? Found->Value : FText::GetEmpty();
				})
				.OnGetMenuContent_Lambda([this]()
				{
					FMenuBuilder Menu(true, nullptr);
					for (const TPair<FString, FText>& Entry : Kinds())
					{
						const FString Id = Entry.Key;
						Menu.AddMenuEntry(Entry.Value, FText::GetEmpty(), FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([this, Id]() { Kind = Id; Shown.Reset(); Refresh(); }),
								FCanExecuteAction(), FIsActionChecked::CreateLambda([this, Id]() { return Kind == Id; })),
							NAME_None, EUserInterfaceActionType::RadioButton);
					}
					return Menu.MakeWidget();
				})
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Center)
			.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([]() { const double CU = TodayCU(); return CU > 0.0 ? FText::Format(LOCTEXT("Today", "Today {0}"), FText::FromString(ScenarioCU::Format(CU))) : FText::GetEmpty(); })
				.ToolTipText(LOCTEXT("TodayTip", "What the jobs in this list cost today: charged amounts, or the quote until Scenario reports the charge."))
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		]
		// Requests that may have reached Scenario: they hold back an identical request.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(ScenarioUI::Gutter, 0.0f, ScenarioUI::Gutter, ScenarioUI::Gutter))
		[
			SNew(SWarningOrErrorBox)
			.MessageStyle(EMessageStyle::Warning)
			.Visibility_Lambda([]() { return Count([](const FScenarioJobRow& Row) { return Row.State == EScenarioJobState::Unknown; }) > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
			.Message_Lambda([]()
			{
				const int32 N = Count([](const FScenarioJobRow& Row) { return Row.State == EScenarioJobState::Unknown; });
				return N == 1
					? LOCTEXT("UnknownOne", "1 request may not have reached Scenario. An identical request is on hold until you check.")
					: FText::Format(LOCTEXT("UnknownMany", "{0} requests may not have reached Scenario. Identical requests are on hold until you check."), FText::AsNumber(N));
			})
			[
				SNew(SButton)
				.Text(LOCTEXT("CheckNow", "Check now"))
				.ToolTipText(LOCTEXT("CheckNowTip", "Looks for these requests in Scenario's recent jobs and links each one that matches. Free."))
				.OnClicked_Lambda([]() { ScenarioJobActions::CheckUnresolved(); return FReply::Handled(); })
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SAssignNew(List, SListView<FItem>)
				.ListItemsSource(&Items)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SScenarioJobsPage::MakeRow)
				.OnMouseButtonDoubleClick_Lambda([](FItem Item)
				{
					FScenarioSession* Session = ScenarioUI::Session();
					const FScenarioJobRow* Row = Session && Item.IsValid() ? Session->Ledger().Find(*Item) : nullptr;
					const ScenarioJobActions::FRowAction Action = Row ? ScenarioJobActions::PrimaryAction(*Row) : ScenarioJobActions::FRowAction();
					if (Action.IsSet())
					{
						Action.Run();
					}
				})
				.OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& KeyEvent)
				{
					if (KeyEvent.GetKey() != EKeys::Enter || !List.IsValid())
					{
						return FReply::Unhandled();
					}
					const TArray<FItem> Selection = List->GetSelectedItems();
					FScenarioSession* Session = ScenarioUI::Session();
					const FScenarioJobRow* Row = Session && Selection.Num() == 1 ? Session->Ledger().Find(*Selection[0]) : nullptr;
					const ScenarioJobActions::FRowAction Action = Row ? ScenarioJobActions::PrimaryAction(*Row) : ScenarioJobActions::FRowAction();
					if (!Action.IsSet())
					{
						return FReply::Unhandled();
					}
					Action.Run();
					return FReply::Handled();
				})
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.Padding(24.0f)
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([this]() { return Items.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						return Filter == TEXT("all") && Kind.IsEmpty()
							? LOCTEXT("Empty", "Nothing generated yet. Everything you or an agent generates shows here, even after a restart.")
							: LOCTEXT("EmptyFilter", "No job matches this filter.");
					})
					.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(0.0f, ScenarioUI::Gutter, 0.0f, 0.0f)
				[
					SNew(SHyperlink)
					.Text(LOCTEXT("GoCreate", "Go to Create"))
					.Visibility_Lambda([this]() { return Filter == TEXT("all") && Kind.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
					.OnNavigate_Lambda([]() { FScenarioEditorModule::OpenStudio(ScenarioPages::Create); })
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
			.Padding(FMargin(ScenarioUI::Gutter, 4.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					ScenarioUI::Line(MakeAttributeLambda([]()
					{
						FScenarioSession* S = ScenarioUI::Session();
						const double Used = S ? S->GetUsageThisMonth() : -1.0;
						return Used < 0.0 ? LOCTEXT("UsageUnknown", "Usage unavailable")
							: FText::Format(LOCTEXT("Month", "Project this month: {0} (all members, all tools)"), FText::FromString(ScenarioCU::Format(Used)));
					}), ScenarioUI::EText::Meta, true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					ScenarioUI::IconButton(TEXT("Icons.Refresh"), LOCTEXT("RefreshUsage", "Refresh the project's usage this month. Free."), FSimpleDelegate::CreateLambda([]()
					{
						if (FScenarioSession* S = ScenarioUI::Session())
						{
							S->RefreshUsage();
						}
					}))
				]
			]
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

void SScenarioJobsPage::SetFilter(const FString& InFilter)
{
	Filter = InFilter;
	Shown.Reset();
	Refresh();
}

bool SScenarioJobsPage::Passes(const FScenarioJobRow& Row) const
{
	if (Row.State == EScenarioJobState::Dismissed)
	{
		return false;
	}
	if ((Filter == TEXT("active") && !Row.IsActive()) || (Filter == TEXT("attention") && !NeedsAttention(Row)) || (Filter == TEXT("ready") && !(IsReady(Row) && !Row.bImported)))
	{
		return false;
	}
	return Kind.IsEmpty() || ScenarioJobActions::DisplayKind(Row) == Kind;
}

void SScenarioJobsPage::Refresh()
{
	TArray<FString> Ids;
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		for (const FScenarioJobRow& Row : Session->Ledger().Rows())
		{
			if (Passes(Row))
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
	TArray<FItem> Next;
	for (const FString& Id : Ids)
	{
		// Keep the item objects of rows already shown, so the selection and row widgets survive.
		const FItem* Existing = Items.FindByPredicate([&Id](const FItem& Item) { return *Item == Id; });
		Next.Add(Existing ? *Existing : MakeShared<FString>(Id));
	}
	Items = MoveTemp(Next);
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SScenarioJobsPage::MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(STableRow<FItem>, Owner)
		.Padding(0.0f)
		[
			SNew(SScenarioJobRow).LocalId(*Item)
		];
}

#undef LOCTEXT_NAMESPACE
