// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ScenarioCU.h"
#include "ScenarioEditorModule.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "ScenarioWidgets.h"
#include "SScenarioJobCard.h"
#include "SScenarioPages.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Images/SImage.h"
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

	const TCHAR* DayPrefix = TEXT("#day:");

	bool IsDayHeader(const FString& Item)
	{
		return Item.StartsWith(DayPrefix);
	}

	/** The local day of a job, as a sortable key. */
	FString DayKey(double UnixSeconds)
	{
		if (UnixSeconds <= 0.0)
		{
			return FString();
		}
		const FDateTime Local = FDateTime::FromUnixTimestamp(static_cast<int64>(UnixSeconds)) + (FDateTime::Now() - FDateTime::UtcNow());
		return Local.ToString(TEXT("%Y-%m-%d"));
	}

	FText DayLabel(const FString& Key, double UnixSeconds)
	{
		const FDateTime Now = FDateTime::Now();
		if (Key.IsEmpty())
		{
			return LOCTEXT("Earlier", "Earlier");
		}
		if (Key == Now.ToString(TEXT("%Y-%m-%d")))
		{
			return LOCTEXT("TodayHeader", "Today");
		}
		if (Key == (Now - FTimespan::FromDays(1.0)).ToString(TEXT("%Y-%m-%d")))
		{
			return LOCTEXT("Yesterday", "Yesterday");
		}
		return FText::AsDate(FDateTime::FromUnixTimestamp(static_cast<int64>(UnixSeconds)), EDateTimeStyle::Medium);
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
	using namespace ScenarioUI;
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		SessionHandle = Session->OnChanged.AddSP(this, &SScenarioJobsPage::Refresh);
	}
	auto Segment = [](const FText& Label, int32 N) { return N > 0 ? FText::Format(LOCTEXT("SegmentCount", "{0}  {1}"), Label, FText::AsNumber(N)) : Label; };
	auto FilterChip = [this](const TCHAR* Id, TAttribute<FText> Label, const FText& Tip)
	{
		const FString Value = Id;
		return ChipToggle(Label, MakeAttributeLambda([this, Value]() { return Filter == Value; }), [this, Value]() { SetFilter(Value); }, NAME_None, Tip, true);
	};

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(PagePadding, PagePadding, PagePadding, 0.0f)
		[
			PageHeader(LOCTEXT("Title", "Jobs"), MakeAttributeLambda([]()
			{
				const double CU = TodayCU();
				return CU > 0.0 ? FText::Format(LOCTEXT("Today", "{0} today in this project. Everything you or an agent generates stays here, even after a restart."), FText::FromString(ScenarioCU::Format(CU)))
					: LOCTEXT("Subtitle", "Everything you or an agent generates in this project stays here, even after a restart.");
			}))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(PagePadding, 12.0f, PagePadding, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				FilterChip(TEXT("all"), MakeAttributeLambda([Segment]() { return Segment(LOCTEXT("All", "All"), Count([](const FScenarioJobRow&) { return true; })); }), FText::GetEmpty())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				FilterChip(TEXT("active"), MakeAttributeLambda([Segment]() { return Segment(LOCTEXT("Running", "Running"), Count([](const FScenarioJobRow& Row) { return Row.IsActive(); })); }),
					LOCTEXT("RunningTip", "Sent and not finished yet."))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				FilterChip(TEXT("ready"), MakeAttributeLambda([Segment]() { return Segment(LOCTEXT("Ready", "Ready"), Count([](const FScenarioJobRow& Row) { return IsReady(Row) && !Row.bImported; })); }),
					LOCTEXT("ReadyTip", "Finished results not imported yet."))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([this]() { return Filter == TEXT("attention") || Count(NeedsAttention) > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					FilterChip(TEXT("attention"), MakeAttributeLambda([Segment]() { return Segment(LOCTEXT("Attention", "Needs attention"), Count(NeedsAttention)); }),
						LOCTEXT("AttentionTip", "Failed, blocked, not confirmed or stopped downloads."))
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNullWidget::NullWidget
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SComboButton)
				.ComboButtonStyle(&Style().GetWidgetStyle<FComboButtonStyle>("Scenario.Combo"))
				.ToolTipText(LOCTEXT("KindTip", "Show one type of result."))
				.ButtonContent()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SImage)
						.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
						.Image_Lambda([this]() { return IconBrush(Kind.IsEmpty() ? FName(TEXT("grid")) : KindIconName(Kind)); })
						.ColorAndOpacity(FSlateColor(Palette::Dim()))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(Font(EText::Body))
						.Text_Lambda([this]()
						{
							const TPair<FString, FText>* Found = Kinds().FindByPredicate([this](const TPair<FString, FText>& Entry) { return Entry.Key == Kind; });
							return Found ? Found->Value : FText::GetEmpty();
						})
					]
				]
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
		]
		// Requests that may have reached Scenario: they hold back an identical request.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(PagePadding, 10.0f, PagePadding, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(Style().GetBrush("Scenario.Card"))
			.Padding(FMargin(12.0f, 8.0f, 8.0f, 8.0f))
			.Visibility_Lambda([]() { return Count([](const FScenarioJobRow& Row) { return Row.State == EScenarioJobState::Unknown; }) > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					Icon(TEXT("warning"), 16.0f, FSlateColor(Palette::Warning()))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(Font(EText::Body))
					.AutoWrapText(true)
					.Text_Lambda([]()
					{
						const int32 N = Count([](const FScenarioJobRow& Row) { return Row.State == EScenarioJobState::Unknown; });
						return N == 1
							? LOCTEXT("UnknownOne", "1 request may not have reached Scenario. An identical request is on hold until you check.")
							: FText::Format(LOCTEXT("UnknownMany", "{0} requests may not have reached Scenario. Identical requests are on hold until you check."), FText::AsNumber(N));
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(10.0f, 0.0f, 0.0f, 0.0f)
				[
					Button(LOCTEXT("CheckNow", "Check now"), EButton::Outline, []() { ScenarioJobActions::CheckUnresolved(); }, TEXT("refresh"),
						LOCTEXT("CheckNowTip", "Looks for these requests in Scenario's recent jobs and links each one that matches. Free."), true)
				]
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(PagePadding - 6.0f, 8.0f, PagePadding - 6.0f, 0.0f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SAssignNew(List, SListView<FItem>)
				.ListViewStyle(&FAppStyle::Get().GetWidgetStyle<FTableViewStyle>("SimpleListView"))
				.ListItemsSource(&Items)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SScenarioJobsPage::MakeRow)
				.OnIsSelectableOrNavigable_Lambda([](FItem Item) { return Item.IsValid() && !IsDayHeader(*Item); })
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
				.Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					Icon(TEXT("jobs"), 28.0f, FSlateColor(Palette::Faint()))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(SBox)
					.MaxDesiredWidth(320.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return Filter == TEXT("all") && Kind.IsEmpty()
								? LOCTEXT("Empty", "Nothing generated yet. Everything you or an agent generates shows here, even after a restart.")
								: LOCTEXT("EmptyFilter", "No job matches this filter.");
						})
						.Font(Font(EText::Body))
						.ColorAndOpacity(FSlateColor(Palette::Dim()))
						.Justification(ETextJustify::Center)
						.AutoWrapText(true)
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return Filter == TEXT("all") && Kind.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						Button(LOCTEXT("GoCreate", "Go to Create"), EButton::Secondary, []() { FScenarioEditorModule::OpenStudio(ScenarioPages::Create); }, TEXT("create"))
					]
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Divider()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(PagePadding, 6.0f, PagePadding - 4.0f, 6.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				Line(MakeAttributeLambda([]()
				{
					FScenarioSession* S = ScenarioUI::Session();
					const double Used = S ? S->GetUsageThisMonth() : -1.0;
					return Used < 0.0 ? LOCTEXT("UsageUnknown", "Usage unavailable")
						: FText::Format(LOCTEXT("Month", "Project this month: {0} (all members, all tools)"), FText::FromString(ScenarioCU::Format(Used)));
				}), EText::Meta, true)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				IconButton(TEXT("refresh"), LOCTEXT("RefreshUsage", "Refresh the project's usage this month. Free."), FSimpleDelegate::CreateLambda([]()
				{
					if (FScenarioSession* S = ScenarioUI::Session())
					{
						S->RefreshUsage();
					}
				}))
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
	// Newest first, with a header line where the day changes.
	TArray<FString> Ids;
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		FString Day = TEXT("?");
		for (const FScenarioJobRow& Row : Session->Ledger().Rows())
		{
			if (!Passes(Row))
			{
				continue;
			}
			const FString Key = DayKey(Row.CreatedAt);
			if (Key != Day)
			{
				Day = Key;
				Ids.Add(DayPrefix + DayLabel(Key, Row.CreatedAt).ToString());
			}
			Ids.Add(Row.LocalId);
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
	if (IsDayHeader(*Item))
	{
		return SNew(STableRow<FItem>, Owner)
			.Style(&ScenarioUI::Style().GetWidgetStyle<FTableRowStyle>("Scenario.TableRow.Header"))
			.ShowSelection(false)
			.Padding(FMargin(8.0f, 12.0f, 8.0f, 4.0f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->RightChop(FCString::Strlen(DayPrefix)).ToUpper()))
				.Font(ScenarioUI::Font(ScenarioUI::EText::Section))
				.ColorAndOpacity(FSlateColor(ScenarioUI::Palette::Faint()))
			];
	}
	return SNew(STableRow<FItem>, Owner)
		.Style(&ScenarioUI::Style().GetWidgetStyle<FTableRowStyle>("Scenario.TableRow"))
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SScenarioJobRow).LocalId(*Item)
		];
}

#undef LOCTEXT_NAMESPACE
