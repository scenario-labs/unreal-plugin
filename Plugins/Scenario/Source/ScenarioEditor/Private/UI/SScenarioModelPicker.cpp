// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioModelPicker.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "ScenarioSettings.h"
#include "ScenarioStyle.h"
#include "ScenarioSubsystem.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioPicker"

namespace
{
	const TArray<TPair<FString, FText>>& Modalities()
	{
		static const TArray<TPair<FString, FText>> List = {
			{ TEXT("image"), LOCTEXT("Image", "Image") },
			{ TEXT("video"), LOCTEXT("Video", "Video") },
			{ TEXT("3d"), LOCTEXT("3D", "3D") },
			{ TEXT("audio"), LOCTEXT("Audio", "Audio") },
		};
		return List;
	}

	TSharedRef<SWidget> Toggle(const FText& Label, TFunction<bool()> IsOn, TFunction<void()> OnClick, const FText& Tip = FText::GetEmpty())
	{
		return SNew(SBox)
			.Padding(0.0f, 0.0f, 4.0f, 4.0f)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.IsChecked_Lambda([IsOn]() { return IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([OnClick](ECheckBoxState) { OnClick(); })
				.ToolTipText(Tip)
				[
					SNew(SBox)
					.Padding(FMargin(8.0f, 2.0f))
					[
						SNew(STextBlock).Text(Label)
					]
				]
			];
	}
}

void SScenarioModelPicker::Open(const FString& Modality, const FString& Chip, FOnPicked OnPicked, const TArray<FString>& QuickPicks, const FString& CurrentId)
{
	TSharedPtr<SScenarioModelPicker> Picker;
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("Title", "Choose a model"))
		.ClientSize(FVector2D(900.0f, 580.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		[
			SAssignNew(Picker, SScenarioModelPicker)
			.Modality(Modality)
			.Chip(Chip)
			.QuickPicks(QuickPicks)
			.CurrentId(CurrentId)
		];
	Picker->OnPicked = MoveTemp(OnPicked);
	Picker->Window = Window;
	Window->SetWidgetToFocusOnActivate(Picker->SearchBox);
	const TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow();
	if (Root.IsValid())
	{
		FSlateApplication::Get().AddWindowAsNativeChild(Window, Root.ToSharedRef());
	}
	else
	{
		FSlateApplication::Get().AddWindow(Window);
	}
}

FScenarioCatalog* SScenarioModelPicker::Catalog() const
{
	UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
	return Subsystem ? &Subsystem->Session().Catalog() : nullptr;
}

void SScenarioModelPicker::Construct(const FArguments& InArgs)
{
	Modality = InArgs._Modality;
	Chip = InArgs._Chip;
	QuickPicks = InArgs._QuickPicks;
	CurrentId = InArgs._CurrentId;

	if (UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get())
	{
		FScenarioCatalog& Cat = Subsystem->Session().Catalog();
		if (!Cat.IsLoaded())
		{
			Cat.LoadCache();
		}
		if (Subsystem->Session().IsReady())
		{
			Cat.RefreshIfNeeded(Subsystem->Session().Scope());
		}
		CatalogHandle = Cat.OnChanged.AddSP(this, &SScenarioModelPicker::Refresh);
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SAssignNew(SearchBox, SSearchBox)
					.HintText(LOCTEXT("Search", "Search models..."))
					.OnTextChanged_Lambda([this](const FText& InText) { Text = InText.ToString(); Refresh(); })
					.OnKeyDownHandler_Lambda([this](const FGeometry& Geometry, const FKeyEvent& KeyEvent) { return OnKeyDown(Geometry, KeyEvent); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(12.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]() { return bTeamModels ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bTeamModels = State == ECheckBoxState::Checked; Refresh(); })
					.ToolTipText(LOCTEXT("TeamTip", "Shows the models your team trained in this project."))
					[
						SNew(STextBlock).Text(LOCTEXT("Team", "Team models"))
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(12.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]() { return bShowLegacy ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bShowLegacy = State == ECheckBoxState::Checked; Refresh(); })
					.ToolTipText(LOCTEXT("LegacyTip", "Shows models Scenario has replaced with newer ones."))
					[
						SNew(STextBlock).Text(LOCTEXT("Legacy", "Show legacy"))
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				BuildTabs()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 8.0f)
			[
				SAssignNew(ChipsBox, SBox)
				[
					BuildChips()
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SSplitter)
				+ SSplitter::Slot()
				.Value(0.55f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SAssignNew(List, SListView<FItem>)
						.ListItemsSource(&Items)
						.SelectionMode(ESelectionMode::Single)
						.OnGenerateRow(this, &SScenarioModelPicker::MakeRow)
						.OnSelectionChanged_Lambda([this](FItem Item, ESelectInfo::Type) { Selected = Item; })
						.OnMouseButtonDoubleClick_Lambda([this](FItem Item) { Choose(Item); })
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							const FScenarioCatalog* Cat = Catalog();
							if (!Cat || (!Cat->IsLoaded() && Cat->IsRefreshing()))
							{
								return LOCTEXT("Loading", "Loading models...");
							}
							if (!Cat->IsLoaded())
							{
								return LOCTEXT("SignIn", "Sign in to Scenario to see the models.");
							}
							return LOCTEXT("NoMatch", "No model matches. Clear the search or pick another chip.");
						})
						.ColorAndOpacity(FScenarioStyle::Muted())
						.Visibility_Lambda([this]() { return Items.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed; })
					]
				]
				+ SSplitter::Slot()
				.Value(0.45f)
				[
					BuildDetails()
				]
			]
		]
	];
	Refresh();
}

SScenarioModelPicker::~SScenarioModelPicker()
{
	if (FScenarioCatalog* Cat = Catalog())
	{
		Cat->OnChanged.Remove(CatalogHandle);
	}
}

TSharedRef<SWidget> SScenarioModelPicker::BuildTabs()
{
	TSharedRef<SWrapBox> Box = SNew(SWrapBox).UseAllottedSize(true);
	Box->AddSlot()[Toggle(LOCTEXT("All", "All"), [this]() { return Modality.IsEmpty(); }, [this]() { SetModality(FString()); })];
	for (const TPair<FString, FText>& Entry : Modalities())
	{
		const FString Id = Entry.Key;
		Box->AddSlot()[Toggle(Entry.Value, [this, Id]() { return Modality == Id; }, [this, Id]() { SetModality(Id); })];
	}
	return Box;
}

TSharedRef<SWidget> SScenarioModelPicker::BuildChips()
{
	TSharedRef<SWrapBox> Box = SNew(SWrapBox).UseAllottedSize(true);
	Box->AddSlot()[Toggle(LOCTEXT("AllChips", "All"), [this]() { return Chip.IsEmpty(); }, [this]() { Chip.Reset(); Refresh(); })];
	const FScenarioCatalog* Cat = Catalog();
	if (Cat && !Modality.IsEmpty())
	{
		for (const FString& Name : Cat->ChipsForModality(Modality))
		{
			Box->AddSlot()[Toggle(FText::FromString(Name), [this, Name]() { return Chip == Name; }, [this, Name]() { Chip = Name; Refresh(); })];
		}
	}
	return Box;
}

void SScenarioModelPicker::SetModality(const FString& InModality)
{
	Modality = InModality;
	Chip.Reset();
	if (ChipsBox.IsValid())
	{
		ChipsBox->SetContent(BuildChips());
	}
	Refresh();
}

TSharedRef<SWidget> SScenarioModelPicker::BuildDetails()
{
	auto Model = [this]() -> const FScenarioCatalogModel* { return Selected.Get(); };
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.Padding(16.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Left)
				[
					SNew(SBox)
					.WidthOverride(96.0f)
					.HeightOverride(96.0f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text_Lambda([Model]() { const FScenarioCatalogModel* M = Model(); return M ? FText::FromString(M->Badge()) : FText::GetEmpty(); })
							.Font(FScenarioStyle::Font(18, true))
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([Model]() { const FScenarioCatalogModel* M = Model(); return M ? FText::FromString(M->Summary.Name) : LOCTEXT("PickOne", "Pick a model on the left."); })
					.Font(FScenarioStyle::Font(14, true))
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([Model]()
					{
						const FScenarioCatalogModel* M = Model();
						if (!M)
						{
							return FText::GetEmpty();
						}
						TArray<FString> Parts;
						if (!M->Summary.Provider.IsEmpty())
						{
							Parts.Add(M->Summary.Provider);
						}
						if (M->Summary.Capabilities.Num() > 0)
						{
							Parts.Add(FString::Join(M->Summary.Capabilities, TEXT(", ")));
						}
						return FText::FromString(FString::Join(Parts, TEXT(" · ")));
					})
					.ColorAndOpacity(FScenarioStyle::Muted())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([Model]()
					{
						const FScenarioCatalogModel* M = Model();
						if (!M)
						{
							return FText::GetEmpty();
						}
						FString Out = M->Summary.ShortDescription;
						if (M->bLegacy && !M->ReplacedBy.IsEmpty())
						{
							Out += FString::Printf(TEXT("\n\nScenario replaced this model with %s."), *M->ReplacedBy);
						}
						return FText::FromString(Out);
					})
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PriceNote", "The price shows on the Generate button once the settings are filled in. Checking it is free."))
					.ColorAndOpacity(FScenarioStyle::Muted())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Left)
				.Padding(0.0f, 16.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.Text(LOCTEXT("Use", "Use this model"))
					.ToolTipText(LOCTEXT("UseTip", "Selects this model in the form. Sends nothing."))
					.IsEnabled_Lambda([this]() { return Selected.IsValid(); })
					.OnClicked_Lambda([this]() { Choose(Selected); return FReply::Handled(); })
				]
			]
		];
}

void SScenarioModelPicker::Refresh()
{
	Items.Reset();
	const FScenarioCatalog* Cat = Catalog();
	if (Cat)
	{
		const UScenarioEditorSettings& Settings = UScenarioEditorSettings::Get();
		const UScenarioProjectSettings& Project = UScenarioProjectSettings::Get();
		FScenarioCatalogQuery Query;
		Query.Text = Text;
		Query.Modality = Modality;
		Query.Chip = Chip;
		Query.bShowLegacy = bShowLegacy;
		Query.bTeamModels = bTeamModels;
		Query.PinnedIds = Settings.FavoriteModels;
		for (const FString& Id : QuickPicks)
		{
			Query.PinnedIds.AddUnique(Id);
		}
		Query.Limit = 500;
		for (const FScenarioCatalogModel& Model : Cat->Query(Query))
		{
			if (Project.IsModelAllowed(Model.Id()))
			{
				Items.Add(MakeShared<FScenarioCatalogModel>(Model));
			}
		}
	}
	if (List.IsValid())
	{
		List->RequestListRefresh();
		FItem Keep;
		for (const FItem& Item : Items)
		{
			if ((Selected.IsValid() && Item->Id() == Selected->Id()) || (!Selected.IsValid() && Item->Id() == CurrentId))
			{
				Keep = Item;
				break;
			}
		}
		if (!Keep.IsValid() && Items.Num() > 0)
		{
			Keep = Items[0];
		}
		Selected = Keep;
		if (Keep.IsValid())
		{
			List->SetSelection(Keep, ESelectInfo::Direct);
			List->RequestScrollIntoView(Keep);
		}
		else
		{
			List->ClearSelection();
		}
	}
}

TSharedRef<ITableRow> SScenarioModelPicker::MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner)
{
	const FString Id = Item->Id();
	TArray<FString> Tags;
	if (Item->bFeatured)
	{
		Tags.Add(TEXT("Featured"));
	}
	if (Item->bTeam || Item->bTrained)
	{
		Tags.Add(TEXT("Team"));
	}
	if (Item->bLegacy)
	{
		Tags.Add(TEXT("Legacy"));
	}
	if (QuickPicks.Contains(Id))
	{
		Tags.Add(TEXT("Lane pick"));
	}
	return SNew(STableRow<FItem>, Owner)
		.Padding(FMargin(4.0f, 3.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("FavoriteTip", "Keeps this model at the top of the picker."))
				.OnClicked_Lambda([this, Id]()
				{
					UScenarioEditorSettings::GetMutable().ToggleFavorite(Id);
					Refresh();
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text_Lambda([Id]() { return FText::FromString(UScenarioEditorSettings::Get().IsFavorite(Id) ? TEXT("★") : TEXT("☆")); })
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Summary.Name))
				.HighlightText_Lambda([this]() { return FText::FromString(Text); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Join(Tags, TEXT(" · "))))
				.ColorAndOpacity(FScenarioStyle::Muted())
			]
		];
}

void SScenarioModelPicker::Choose(const FItem& Item)
{
	if (!Item.IsValid())
	{
		return;
	}
	const FString Id = Item->Id();
	const FString Name = Item->Summary.Name;
	const FOnPicked Callback = OnPicked;
	if (const TSharedPtr<SWindow> Pinned = Window.Pin())
	{
		Pinned->RequestDestroyWindow();
	}
	if (Callback)
	{
		Callback(Id, Name);
	}
}

FReply SScenarioModelPicker::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	const FKey Key = KeyEvent.GetKey();
	if (Key == EKeys::Escape)
	{
		if (const TSharedPtr<SWindow> Pinned = Window.Pin())
		{
			Pinned->RequestDestroyWindow();
		}
		return FReply::Handled();
	}
	if ((Key == EKeys::Up || Key == EKeys::Down) && Items.Num() > 0)
	{
		int32 Index = Items.IndexOfByKey(Selected);
		Index = FMath::Clamp(Index + (Key == EKeys::Down ? 1 : -1), 0, Items.Num() - 1);
		Selected = Items[Index];
		List->SetSelection(Selected, ESelectInfo::OnNavigation);
		List->RequestScrollIntoView(Selected);
		return FReply::Handled();
	}
	if (Key == EKeys::Enter && Selected.IsValid())
	{
		Choose(Selected);
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

#undef LOCTEXT_NAMESPACE
