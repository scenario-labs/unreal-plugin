// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioModelPicker.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ScenarioCreateModel.h"
#include "ScenarioCU.h"
#include "ScenarioLanes.h"
#include "ScenarioQuotes.h"
#include "ScenarioSchema.h"
#include "ScenarioSession.h"
#include "ScenarioSettings.h"
#include "ScenarioSubsystem.h"
#include "ScenarioWidgets.h"
#include "SPrimaryButton.h"
#include "SSimpleComboButton.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioPicker"

namespace
{
	struct FPickerTab
	{
		FString Id;
		FText Label;
	};

	/** The picker's type tabs, in the Create lanes' order. Material and Sky are image models picked for that use. */
	const TArray<FPickerTab>& Tabs()
	{
		static const TArray<FPickerTab> List = {
			{ FString(), LOCTEXT("All", "All") },
			{ TEXT("image"), LOCTEXT("Image", "Image") },
			{ TEXT("3d"), LOCTEXT("3D", "3D") },
			{ TEXT("material"), LOCTEXT("Material", "Material") },
			{ TEXT("sky"), LOCTEXT("Sky", "Sky") },
			{ TEXT("audio"), LOCTEXT("Audio", "Audio") },
			{ TEXT("video"), LOCTEXT("Video", "Video") },
		};
		return List;
	}

	bool IsUseTab(const FString& Tab)
	{
		return Tab == TEXT("material") || Tab == TEXT("sky");
	}

	bool HasTag(const FScenarioCatalogModel& Model, std::initializer_list<const TCHAR*> Wanted)
	{
		for (const FString& Tag : Model.Summary.Tags)
		{
			for (const TCHAR* Name : Wanted)
			{
				if (Tag.Equals(Name, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
		}
		return false;
	}

	bool InLane(const FString& LaneId, const FString& ModelId)
	{
		const FScenarioLane* Lane = FScenarioLanes::Get().Find(LaneId);
		return Lane && Lane->Models.ContainsByPredicate([&ModelId](const FScenarioLaneModel& Model) { return Model.Id == ModelId; });
	}

	bool IsSkyModel(const FScenarioCatalogModel& Model)
	{
		return InLane(TEXT("environment"), Model.Id()) || HasTag(Model, { TEXT("sc:skybox"), TEXT("skybox"), TEXT("hdri") });
	}

	bool IsMaterialModel(const FScenarioCatalogModel& Model)
	{
		return InLane(TEXT("material"), Model.Id()) || HasTag(Model, { TEXT("sc:texture"), TEXT("sc:material") });
	}

	bool InTab(const FString& Tab, const FScenarioCatalogModel& Model)
	{
		if (Tab == TEXT("material"))
		{
			return IsMaterialModel(Model);
		}
		if (Tab == TEXT("sky"))
		{
			return IsSkyModel(Model);
		}
		return true;
	}

	/** What the model makes, for its icon: image, 3d, material, sky, audio or video. */
	FString KindOf(const FScenarioCatalogModel& Model)
	{
		if (IsSkyModel(Model))
		{
			return TEXT("sky");
		}
		if (IsMaterialModel(Model))
		{
			return TEXT("material");
		}
		return Model.Modality;
	}

	/** "OpenAI", "Your team", "Scenario". */
	FString MakerOf(const FScenarioCatalogModel& Model)
	{
		if (Model.bTeam || Model.bTrained)
		{
			return TEXT("Your team");
		}
		return Model.Summary.Provider.IsEmpty() ? FString(TEXT("Scenario")) : Model.Summary.Provider;
	}

	/** "txt2img" -> "Text to image", "img23d" -> "Image to 3D", "inpaint" -> "Inpaint". */
	FString CapabilityText(FString Capability)
	{
		Capability.RemoveFromEnd(TEXT("_texture"));
		auto Word = [](const FString& Token) -> FString
		{
			if (Token == TEXT("txt")) return TEXT("text");
			if (Token == TEXT("img")) return TEXT("image");
			if (Token == TEXT("3d")) return TEXT("3D");
			return Token;
		};
		FString From;
		FString To;
		FString Out = Capability.Split(TEXT("2"), &From, &To) && !From.IsEmpty() && !To.IsEmpty()
			? FString::Printf(TEXT("%s to %s"), *Word(From), *Word(To))
			: ScenarioSchema::Humanize(Capability);
		if (Out.Len() > 0)
		{
			Out[0] = FChar::ToUpper(Out[0]);
		}
		return Out;
	}

	FString CapabilitiesText(const FScenarioCatalogModel& Model)
	{
		TArray<FString> Parts;
		for (const FString& Capability : Model.Summary.Capabilities)
		{
			Parts.AddUnique(CapabilityText(Capability));
		}
		if (Parts.Num() > 4)
		{
			Parts.SetNum(4);
		}
		return FString::Join(Parts, TEXT(", "));
	}

	/** A prompt that fits the model's lane, for its default-settings price. */
	FString ExamplePrompt(const FScenarioCatalogModel& Model)
	{
		const FScenarioLanes& Lanes = FScenarioLanes::Get();
		const FScenarioLane* Lane = Lanes.LaneForModel(Model.Id());
		if (!Lane)
		{
			const FString Kind = KindOf(Model);
			Lane = Lanes.Find(Kind == TEXT("sky") ? FString(TEXT("environment")) : Kind);
		}
		return Lane && Lane->StarterPrompts.Num() > 0 ? Lane->StarterPrompts[0] : FString(TEXT("a small stone lantern"));
	}

	// Default-settings prices for the editor session, by workspace and model:
	// "30 CU", or empty when the model needs an input before it can be priced.
	TMap<FString, FString>& Estimates()
	{
		static TMap<FString, FString> Map;
		return Map;
	}

	TSet<FString>& PendingEstimates()
	{
		static TSet<FString> Set;
		return Set;
	}

	FString EstimateKey(const FString& ModelId)
	{
		const UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
		return Subsystem ? Subsystem->Session().Scope() + TEXT("|") + ModelId : ModelId;
	}

	/** A free dry run of the model at its default settings with an example prompt. */
	void Estimate(const FScenarioCatalogModel& Model)
	{
		UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
		if (!Subsystem || !Subsystem->Session().IsReady())
		{
			return;
		}
		const FString Key = EstimateKey(Model.Id());
		if (Estimates().Contains(Key) || PendingEstimates().Contains(Key))
		{
			return;
		}
		PendingEstimates().Add(Key);
		const FString Id = Model.Id();
		const FString Example = ExamplePrompt(Model);
		TWeakObjectPtr<UScenarioSubsystem> WeakSubsystem(Subsystem);
		Subsystem->GetModelFields(Id, [WeakSubsystem, Id, Key, Example](const TArray<FScenarioField>& Fields, const FScenarioError& Error)
		{
			UScenarioSubsystem* Owner = WeakSubsystem.Get();
			if (!Owner || Error.Kind != EScenarioErrorKind::None || Fields.Num() == 0)
			{
				// Not cached: selecting the model again retries.
				PendingEstimates().Remove(Key);
				return;
			}
			const TSharedRef<FJsonObject> Values = ScenarioSchema::InitialValues(Fields);
			const FString PromptField = ScenarioSchema::PromptName(Fields);
			if (!PromptField.IsEmpty())
			{
				Values->SetStringField(PromptField, Example);
			}
			// A model that needs a reference (image, mesh, audio) is priced on Generate once one is added.
			if (ScenarioSchema::Validate(Fields, Values).Num() > 0)
			{
				PendingEstimates().Remove(Key);
				Estimates().Add(Key, FString());
				return;
			}
			FScenarioRunRequest Request;
			Request.TargetId = Id;
			Request.Parameters = ScenarioSchema::Payload(Fields, Values);
			Owner->Session().Price(Request, true, [Key](const TScenarioResult<FScenarioQuote>& Result)
			{
				PendingEstimates().Remove(Key);
				if (Result.bOk)
				{
					Estimates().Add(Key, ScenarioCU::Format(Result.Value.CU));
				}
			});
		});
	}

	/** Shows the widget only while When() holds. */
	TSharedRef<SWidget> ShownWhen(const TSharedRef<SWidget>& Widget, TFunction<bool()> When)
	{
		Widget->SetVisibility(MakeAttributeLambda([When]() { return When() ? EVisibility::Visible : EVisibility::Collapsed; }));
		return Widget;
	}

	TSharedRef<SWidget> Toggle(const FText& Label, const FText& Tooltip, TFunction<bool()> IsOn, TFunction<void(bool)> Set)
	{
		return SNew(SCheckBox)
			.IsChecked_Lambda([IsOn]() { return IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([Set](ECheckBoxState State) { Set(State == ECheckBoxState::Checked); })
			.ToolTipText(Tooltip)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
			];
	}
}

void SScenarioModelPicker::Open(const FString& Modality, const FString& Chip, FOnPicked OnPicked, const TArray<FString>& QuickPicks, const FString& CurrentId)
{
	TSharedPtr<SScenarioModelPicker> Picker;
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("Title", "Choose a model"))
		.ClientSize(FVector2D(900.0f, 600.0f))
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
	if (!Tabs().ContainsByPredicate([this](const FPickerTab& Tab) { return Tab.Id == Modality; }))
	{
		Modality.Reset();
	}

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
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(ScenarioUI::SectionGap)
		[
			SNew(SVerticalBox)
			// Search, and what the list includes.
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
				.Padding(ScenarioUI::SectionGap, 0.0f, 0.0f, 0.0f)
				[
					Toggle(LOCTEXT("Team", "Team models"), LOCTEXT("TeamTip", "Shows the models your team trained in this project."),
						[this]() { return bTeamModels; }, [this](bool bOn) { bTeamModels = bOn; Refresh(); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(ScenarioUI::SectionGap, 0.0f, 0.0f, 0.0f)
				[
					Toggle(LOCTEXT("Older", "Show older models"), LOCTEXT("OlderTip", "Shows models Scenario has replaced with newer ones."),
						[this]() { return bShowLegacy; }, [this](bool bOn) { bShowLegacy = bOn; Refresh(); })
				]
			]
			// Type and category.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, ScenarioUI::Gutter, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					BuildTabs()
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
					BuildCategories()
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0.0f, ScenarioUI::Gutter, 0.0f, 0.0f)
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
						.OnSelectionChanged_Lambda([this](FItem Item, ESelectInfo::Type) { if (Item.IsValid()) { Select(Item); } })
						.OnMouseButtonDoubleClick_Lambda([this](FItem Item) { Choose(Item); })
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Padding(ScenarioUI::SectionGap)
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
							return Text.IsEmpty() ? LOCTEXT("NoModel", "No model here. Try another type or category.")
								: FText::Format(LOCTEXT("NoMatch", "Nothing matches \"{0}\"."), FText::FromString(Text));
						})
						.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
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
	TSharedRef<SSegmentedControl<FString>> Control = SNew(SSegmentedControl<FString>)
		.UniformPadding(FMargin(8.0f, 2.0f))
		.Value_Lambda([this]() { return Modality; })
		.OnValueChanged_Lambda([this](const FString& Value) { SetModality(Value); });
	for (const FPickerTab& Tab : Tabs())
	{
		Control->AddSlot(Tab.Id).Text(Tab.Label);
	}
	return Control;
}

TArray<FString> SScenarioModelPicker::Categories() const
{
	const FScenarioCatalog* Cat = Catalog();
	return Cat && !Modality.IsEmpty() && !IsUseTab(Modality) ? Cat->ChipsForModality(Modality) : TArray<FString>();
}

TSharedRef<SWidget> SScenarioModelPicker::BuildCategories()
{
	return SNew(SSimpleComboButton)
		.Icon(FAppStyle::GetBrush("Icons.Filter"))
		.Text_Lambda([this]() { return Chip.IsEmpty() ? LOCTEXT("AllCategories", "All categories") : FText::FromString(Chip); })
		.UsesSmallText(true)
		.HasDownArrow(true)
		.ToolTipText(LOCTEXT("CategoryTip", "Show one kind of model: generate, edit, upscale and more."))
		.Visibility_Lambda([this]() { return Categories().Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
		.OnGetMenuContent_Lambda([this]()
		{
			FMenuBuilder Menu(true, nullptr);
			auto Add = [this, &Menu](const FString& Name, const FText& Label)
			{
				Menu.AddMenuEntry(Label, FText::GetEmpty(), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([this, Name]() { Chip = Name; Refresh(); }), FCanExecuteAction(),
						FIsActionChecked::CreateLambda([this, Name]() { return Chip == Name; })),
					NAME_None, EUserInterfaceActionType::RadioButton);
			};
			Add(FString(), LOCTEXT("AllCategories", "All categories"));
			for (const FString& Name : Categories())
			{
				Add(Name, FText::FromString(Name));
			}
			return Menu.MakeWidget();
		});
}

void SScenarioModelPicker::SetModality(const FString& InModality)
{
	Modality = InModality;
	Chip.Reset();
	Refresh();
}

TSharedRef<SWidget> SScenarioModelPicker::BuildDetails()
{
	auto Has = [this]() { return Selected.IsValid(); };
	auto Is = [this](TFunction<bool(const FScenarioCatalogModel&)> Test) { return [this, Test]() { return Selected.IsValid() && Test(*Selected); }; };
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(ScenarioUI::SectionGap)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SNew(SVerticalBox)
					// Kind thumbnail, name, maker and what it does.
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Top)
						.Padding(0.0f, 0.0f, ScenarioUI::SectionGap, 0.0f)
						[
							ShownWhen(ScenarioUI::Thumbnail(64.0f, TAttribute<const FSlateBrush*>(nullptr),
								MakeAttributeLambda([this]() { return Selected.IsValid() ? KindOf(*Selected) : FString(TEXT("image")); })), Has)
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(STextBlock)
								.Text_Lambda([this]() { return Selected.IsValid() ? FText::FromString(Selected->Summary.Name) : LOCTEXT("PickOne", "Pick a model on the left."); })
								.Font(ScenarioUI::Font(ScenarioUI::EText::Title))
								.AutoWrapText(true)
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.0f, 2.0f, 0.0f, 0.0f)
							[
								SNew(STextBlock)
								.Text_Lambda([this]()
								{
									return Selected.IsValid()
										? FText::FromString(FString::Printf(TEXT("%s · %s"), *MakerOf(*Selected), *CapabilitiesText(*Selected)))
										: FText::GetEmpty();
								})
								.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
								.ColorAndOpacity(FSlateColor::UseSubduedForeground())
								.AutoWrapText(true)
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.HAlign(HAlign_Left)
							.Padding(0.0f, ScenarioUI::RowGap, 0.0f, 0.0f)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, ScenarioUI::RowGap, 0.0f)
								[
									ShownWhen(ScenarioUI::Chip(LOCTEXT("RecommendedChip", "Recommended"), FStyleColors::AccentGreen,
										LOCTEXT("RecommendedTip", "Recommended for this lane: good results at a low price.")), Is([this](const FScenarioCatalogModel& M) { return QuickPicks.Contains(M.Id()); }))
								]
								+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, ScenarioUI::RowGap, 0.0f)
								[
									ShownWhen(ScenarioUI::Chip(LOCTEXT("FeaturedChip", "Featured"), FStyleColors::AccentBlue,
										LOCTEXT("FeaturedTip", "Featured by Scenario.")), Is([](const FScenarioCatalogModel& M) { return M.bFeatured; }))
								]
								+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, ScenarioUI::RowGap, 0.0f)
								[
									ShownWhen(ScenarioUI::Chip(LOCTEXT("TeamChip", "Team"), FStyleColors::AccentGray,
										LOCTEXT("TeamChipTip", "Trained by your team.")), Is([](const FScenarioCatalogModel& M) { return M.bTeam || M.bTrained; }))
								]
								+ SHorizontalBox::Slot().AutoWidth()
								[
									ShownWhen(ScenarioUI::Chip(LOCTEXT("OlderChip", "Older"), FStyleColors::Warning,
										LOCTEXT("OlderChipTip", "Scenario has a newer model for this.")), Is([](const FScenarioCatalogModel& M) { return M.bLegacy; }))
								]
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, ScenarioUI::SectionGap, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return Selected.IsValid() ? FText::FromString(Selected->Summary.ShortDescription) : FText::GetEmpty(); })
						.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, ScenarioUI::Gutter, 0.0f, 0.0f)
					[
						ShownWhen(SNew(STextBlock)
							.Text_Lambda([this]()
							{
								return Selected.IsValid()
									? FText::Format(LOCTEXT("Replaced", "Scenario replaced this model with {0}."), FText::FromString(ScenarioUI::ModelName(Selected->ReplacedBy)))
									: FText::GetEmpty();
							})
							.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
							.ColorAndOpacity(FStyleColors::Warning)
							.AutoWrapText(true),
							Is([](const FScenarioCatalogModel& M) { return M.bLegacy && !M.ReplacedBy.IsEmpty(); }))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, ScenarioUI::SectionGap, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return PriceNote(); })
						.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			.Padding(0.0f, ScenarioUI::Gutter, 0.0f, 0.0f)
			[
				SNew(SPrimaryButton)
				.Text(LOCTEXT("Use", "Use this model"))
				.ToolTipText(LOCTEXT("UseTip", "Selects this model in the form (Enter). Sends nothing."))
				.IsEnabled_Lambda([this]() { return Selected.IsValid(); })
				.OnClicked_Lambda([this]() { Choose(Selected); return FReply::Handled(); })
			]
		];
}

FText SScenarioModelPicker::PriceNote() const
{
	if (!Selected.IsValid())
	{
		return FText::GetEmpty();
	}
	const FString Key = EstimateKey(Selected->Id());
	if (const FString* Price = Estimates().Find(Key))
	{
		return Price->IsEmpty()
			? LOCTEXT("PriceAfterInput", "Needs an input before it can be priced. The exact price shows on Generate once it is added, checked for free.")
			: FText::Format(LOCTEXT("PriceDefault", "{0} at the default settings. The exact price shows on Generate, checked for free."), FText::FromString(*Price));
	}
	if (EstimateTimer.IsValid() || PendingEstimates().Contains(Key))
	{
		return LOCTEXT("PriceChecking", "Checking the price at the default settings (free)...");
	}
	return LOCTEXT("PriceLater", "The exact price shows on Generate, checked for free.");
}

void SScenarioModelPicker::ScheduleEstimate()
{
	if (EstimateTimer.IsValid())
	{
		UnRegisterActiveTimer(EstimateTimer.ToSharedRef());
		EstimateTimer.Reset();
	}
	if (!Selected.IsValid() || Estimates().Contains(EstimateKey(Selected->Id())))
	{
		return;
	}
	// Arrowing through the list prices only the model the selection rests on.
	EstimateTimer = RegisterActiveTimer(0.4f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
	{
		EstimateTimer.Reset();
		if (Selected.IsValid())
		{
			Estimate(*Selected);
		}
		return EActiveTimerReturnType::Stop;
	}));
}

void SScenarioModelPicker::Select(const FItem& Item)
{
	if (Selected == Item)
	{
		return;
	}
	Selected = Item;
	ScheduleEstimate();
}

void SScenarioModelPicker::Refresh()
{
	Items.Reset();
	if (const FScenarioCatalog* Cat = Catalog())
	{
		const UScenarioEditorSettings& Settings = UScenarioEditorSettings::Get();
		const UScenarioProjectSettings& Project = UScenarioProjectSettings::Get();
		FScenarioCatalogQuery Query;
		Query.Text = Text;
		Query.Modality = IsUseTab(Modality) ? FString() : Modality;
		Query.Chip = IsUseTab(Modality) ? FString() : Chip;
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
			if (Project.IsModelAllowed(Model.Id()) && InTab(Modality, Model))
			{
				Items.Add(MakeShared<FScenarioCatalogModel>(Model));
			}
		}
	}
	if (!List.IsValid())
	{
		return;
	}
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
	Select(Keep);
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

TSharedRef<ITableRow> SScenarioModelPicker::MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner)
{
	const FString Id = Item->Id();
	TArray<FString> Meta = { MakerOf(*Item) };
	if (Item->Chips.Num() > 0)
	{
		Meta.Add(FString::Join(Item->Chips, TEXT(", ")));
	}
	auto IsFavorite = [Id]() { return UScenarioEditorSettings::Get().IsFavorite(Id); };
	return SNew(STableRow<FItem>, Owner)
		.Padding(FMargin(ScenarioUI::Gutter, 4.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, ScenarioUI::Gutter, 0.0f)
			[
				SNew(SImage)
				.Image(ScenarioUI::KindIcon(KindOf(*Item)))
				.DesiredSizeOverride(FVector2D(16.0f, 16.0f))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->Summary.Name))
					.ToolTipText(FText::FromString(Item->Summary.Name))
					.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.HighlightText_Lambda([this]() { return FText::FromString(Text); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					ScenarioUI::Line(FText::FromString(FString::Join(Meta, TEXT(" · "))), ScenarioUI::EText::Meta, true)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(ScenarioUI::RowGap, 0.0f, 0.0f, 0.0f)
			[
				QuickPicks.Contains(Id)
					? ScenarioUI::Chip(LOCTEXT("RecommendedChip", "Recommended"), FStyleColors::AccentGreen, LOCTEXT("RecommendedTip", "Recommended for this lane: good results at a low price."))
					: SNullWidget::NullWidget
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(ScenarioUI::RowGap, 0.0f, 0.0f, 0.0f)
			[
				Item->bTeam || Item->bTrained
					? ScenarioUI::Chip(LOCTEXT("TeamChip", "Team"), FStyleColors::AccentGray, LOCTEXT("TeamChipTip", "Trained by your team."))
					: SNullWidget::NullWidget
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(ScenarioUI::RowGap, 0.0f, 0.0f, 0.0f)
			[
				Item->bLegacy
					? ScenarioUI::Chip(LOCTEXT("OlderChip", "Older"), FStyleColors::Warning, LOCTEXT("OlderChipTip", "Scenario has a newer model for this."))
					: SNullWidget::NullWidget
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(ScenarioUI::RowGap, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(2.0f))
				.ToolTipText_Lambda([IsFavorite]()
				{
					return IsFavorite() ? LOCTEXT("Unpin", "Unpin this model from the top of the list.") : LOCTEXT("Pin", "Pin this model to the top of the list.");
				})
				.OnClicked_Lambda([this, Id]()
				{
					UScenarioEditorSettings::GetMutable().ToggleFavorite(Id);
					Refresh();
					return FReply::Handled();
				})
				[
					SNew(SImage)
					.Image_Lambda([IsFavorite]() { return FAppStyle::GetBrush(IsFavorite() ? "Icons.Star" : "Icons.Star.Outline"); })
					.ColorAndOpacity_Lambda([IsFavorite]() { return IsFavorite() ? FStyleColors::AccentYellow : FSlateColor::UseSubduedForeground(); })
				]
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
		Select(Items[Index]);
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

// Workflows

void SScenarioModelPicker::OpenWorkflows(TFunction<void(const FScenarioWorkflowItem& Workflow)> OnPicked, const FString& CurrentId)
{
	TSharedPtr<SScenarioWorkflowPicker> Picker;
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("WorkflowsTitle", "Choose a workflow"))
		.ClientSize(FVector2D(900.0f, 600.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		[
			SAssignNew(Picker, SScenarioWorkflowPicker)
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

void SScenarioWorkflowPicker::Construct(const FArguments& InArgs)
{
	CurrentId = InArgs._CurrentId;
	const TSharedRef<FScenarioCreateModel> Model = FScenarioCreateModel::Get();
	ModelHandle = Model->OnChanged.AddSP(this, &SScenarioWorkflowPicker::Refresh);
	Model->LoadWorkflows();
	auto Count = [Model](bool bTeam) { int32 N = 0; for (const FScenarioWorkflowItem& Item : Model->GetWorkflows()) { N += Item.bTeam == bTeam ? 1 : 0; } return N; };

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(ScenarioUI::SectionGap)
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
					.HintText(LOCTEXT("SearchWorkflows", "Search workflows..."))
					.OnTextChanged_Lambda([this](const FText& InText) { Text = InText.ToString(); Refresh(); })
					.OnKeyDownHandler_Lambda([this](const FGeometry& Geometry, const FKeyEvent& KeyEvent) { return OnKeyDown(Geometry, KeyEvent); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(ScenarioUI::SectionGap, 0.0f, 0.0f, 0.0f)
				[
					SNew(SSegmentedControl<FString>)
					.UniformPadding(FMargin(8.0f, 2.0f))
					.Value_Lambda([this]() { return Source; })
					.OnValueChanged_Lambda([this](const FString& Value) { Source = Value; Refresh(); })
					+ SSegmentedControl<FString>::Slot(TEXT("all")).Text(LOCTEXT("AllWorkflows", "All"))
					+ SSegmentedControl<FString>::Slot(TEXT("team")).Text_Lambda([Count]() { return FText::Format(LOCTEXT("TeamCount", "Team {0}"), FText::AsNumber(Count(true))); })
						.ToolTip(LOCTEXT("TeamWorkflowsTip", "Workflows your team built in this project."))
					+ SSegmentedControl<FString>::Slot(TEXT("featured")).Text_Lambda([Count]() { return FText::Format(LOCTEXT("FeaturedCount", "Featured {0}"), FText::AsNumber(Count(false))); })
						.ToolTip(LOCTEXT("FeaturedWorkflowsTip", "Workflows Scenario features for every project."))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
				[
					ScenarioUI::IconButton(TEXT("Icons.Refresh"), LOCTEXT("RefreshWorkflows", "Reload the workflows from Scenario. Free."),
						FSimpleDelegate::CreateLambda([]() { FScenarioCreateModel::Get()->LoadWorkflows(true); }))
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0.0f, ScenarioUI::Gutter, 0.0f, 0.0f)
			[
				SNew(SSplitter)
				+ SSplitter::Slot()
				.Value(0.5f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SAssignNew(List, SListView<FItem>)
						.ListItemsSource(&Items)
						.SelectionMode(ESelectionMode::Single)
						.OnGenerateRow(this, &SScenarioWorkflowPicker::MakeRow)
						.OnSelectionChanged_Lambda([this](FItem Item, ESelectInfo::Type) { if (Item.IsValid()) { Selected = Item; } })
						.OnMouseButtonDoubleClick_Lambda([this](FItem Item) { Choose(Item); })
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
						.AutoWrapText(true)
						.Text_Lambda([Model, this]()
						{
							if (Model->IsLoadingWorkflows())
							{
								return LOCTEXT("WorkflowsLoading", "Loading workflows...");
							}
							if (!Model->GetWorkflowsError().IsEmpty())
							{
								return FText::FromString(Model->GetWorkflowsError());
							}
							return Text.IsEmpty() ? LOCTEXT("NoWorkflows", "No ready workflow here yet. Build one on scenario.com.")
								: FText::Format(LOCTEXT("NoWorkflowMatch", "Nothing matches \"{0}\"."), FText::FromString(Text));
						})
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Visibility_Lambda([this]() { return Items.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed; })
					]
				]
				+ SSplitter::Slot()
				.Value(0.5f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
					.Padding(ScenarioUI::SectionGap)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							SNew(SScrollBox)
							+ SScrollBox::Slot()
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot()
								.AutoHeight()
								[
									SNew(STextBlock)
									.Text_Lambda([this]() { return Selected.IsValid() ? FText::FromString(Selected->Name) : LOCTEXT("PickWorkflow", "Pick a workflow on the left."); })
									.Font(ScenarioUI::Font(ScenarioUI::EText::Title))
									.AutoWrapText(true)
								]
								+ SVerticalBox::Slot()
								.AutoHeight()
								.HAlign(HAlign_Left)
								.Padding(0.0f, ScenarioUI::RowGap, 0.0f, 0.0f)
								[
									SNew(SBox)
									.Visibility_Lambda([this]() { return Selected.IsValid() ? EVisibility::Visible : EVisibility::Collapsed; })
									[
										ScenarioUI::Chip(MakeAttributeLambda([this]() { return Selected.IsValid() && Selected->bTeam ? LOCTEXT("TeamChip", "Team") : LOCTEXT("FeaturedChip", "Featured"); }), FStyleColors::AccentGray)
									]
								]
								+ SVerticalBox::Slot()
								.AutoHeight()
								.Padding(0.0f, ScenarioUI::Gutter, 0.0f, 0.0f)
								[
									SNew(STextBlock)
									.Text_Lambda([this]() { return Selected.IsValid() ? FText::FromString(Selected->Description) : FText::GetEmpty(); })
									.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
									.AutoWrapText(true)
								]
								+ SVerticalBox::Slot()
								.AutoHeight()
								.Padding(0.0f, ScenarioUI::SectionGap, 0.0f, 0.0f)
								[
									SNew(STextBlock)
									.Text(LOCTEXT("WorkflowPriceNote", "The exact price shows on Generate once the inputs are filled, checked for free."))
									.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
									.ColorAndOpacity(FSlateColor::UseSubduedForeground())
									.AutoWrapText(true)
									.Visibility_Lambda([this]() { return Selected.IsValid() ? EVisibility::Visible : EVisibility::Collapsed; })
								]
							]
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.HAlign(HAlign_Right)
						.Padding(0.0f, ScenarioUI::Gutter, 0.0f, 0.0f)
						[
							SNew(SPrimaryButton)
							.Text(LOCTEXT("UseWorkflow", "Use this workflow"))
							.ToolTipText(LOCTEXT("UseWorkflowTip", "Selects this workflow in the form (Enter). Sends nothing."))
							.IsEnabled_Lambda([this]() { return Selected.IsValid(); })
							.OnClicked_Lambda([this]() { Choose(Selected); return FReply::Handled(); })
						]
					]
				]
			]
		]
	];
	Refresh();
}

SScenarioWorkflowPicker::~SScenarioWorkflowPicker()
{
	FScenarioCreateModel::Get()->OnChanged.Remove(ModelHandle);
}

void SScenarioWorkflowPicker::Refresh()
{
	const FString SelectedId = Selected.IsValid() ? Selected->Id : CurrentId;
	TArray<FItem> Next;
	for (const FScenarioWorkflowItem& Item : FScenarioCreateModel::Get()->GetWorkflows())
	{
		if ((Source == TEXT("team") && !Item.bTeam) || (Source == TEXT("featured") && Item.bTeam))
		{
			continue;
		}
		if (!Text.IsEmpty() && !Item.Name.Contains(Text) && !Item.Description.Contains(Text))
		{
			continue;
		}
		Next.Add(MakeShared<FScenarioWorkflowItem>(Item));
	}
	const bool bSame = Next.Num() == Items.Num() && [&]()
	{
		for (int32 Index = 0; Index < Next.Num(); ++Index)
		{
			if (Next[Index]->Id != Items[Index]->Id)
			{
				return false;
			}
		}
		return true;
	}();
	if (bSame)
	{
		return;
	}
	Items = MoveTemp(Next);
	Selected.Reset();
	for (const FItem& Item : Items)
	{
		if (Item->Id == SelectedId)
		{
			Selected = Item;
		}
	}
	if (!Selected.IsValid() && Items.Num() > 0)
	{
		Selected = Items[0];
	}
	if (List.IsValid())
	{
		List->RequestListRefresh();
		if (Selected.IsValid())
		{
			List->SetSelection(Selected, ESelectInfo::Direct);
			List->RequestScrollIntoView(Selected);
		}
	}
}

TSharedRef<ITableRow> SScenarioWorkflowPicker::MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(STableRow<FItem>, Owner)
		.Padding(FMargin(ScenarioUI::Gutter, 4.0f))
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
					.Text(FText::FromString(Item->Name))
					.ToolTipText(FText::FromString(Item->Name))
					.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.HighlightText_Lambda([this]() { return FText::FromString(Text); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					ScenarioUI::Line(FText::FromString(Item->Description), ScenarioUI::EText::Meta, true)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(ScenarioUI::RowGap, 0.0f, 0.0f, 0.0f)
			[
				Item->bTeam
					? ScenarioUI::Chip(LOCTEXT("TeamChip", "Team"), FStyleColors::AccentGray, LOCTEXT("TeamWorkflowTip", "Built by your team."))
					: SNullWidget::NullWidget
			]
		];
}

void SScenarioWorkflowPicker::Choose(const FItem& Item)
{
	if (!Item.IsValid())
	{
		return;
	}
	const FScenarioWorkflowItem Copy = *Item;
	const TFunction<void(const FScenarioWorkflowItem&)> Callback = OnPicked;
	if (const TSharedPtr<SWindow> Pinned = Window.Pin())
	{
		Pinned->RequestDestroyWindow();
	}
	if (Callback)
	{
		Callback(Copy);
	}
}

FReply SScenarioWorkflowPicker::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
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
