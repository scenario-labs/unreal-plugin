// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioForm.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetThumbnail.h"
#include "DesktopPlatformModule.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IDesktopPlatform.h"
#include "Input/DragAndDrop.h"
#include "ScenarioCreateModel.h"
#include "ScenarioCU.h"
#include "ScenarioJson.h"
#include "ScenarioLanes.h"
#include "ScenarioLedger.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "ScenarioThumbnails.h"
#include "ScenarioWidgets.h"
#include "SDropTarget.h"
#include "SScenarioPages.h"
#include "SSimpleComboButton.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioForm"

namespace
{
	constexpr float SlotSize = 56.0f;
	constexpr float RowHeight = 28.0f;
	constexpr float ControlWidth = 280.0f;
	constexpr int32 SparkMinWords = 3;
	/** Quotes live 120 seconds; older prices are checked again before a run. */
	constexpr double SparkQuoteAge = 100.0;

	FString FileTypesFor(const FString& Kind)
	{
		if (Kind == TEXT("3d"))
		{
			return TEXT("3D models (*.glb;*.gltf;*.fbx;*.obj)|*.glb;*.gltf;*.fbx;*.obj");
		}
		if (Kind == TEXT("audio"))
		{
			return TEXT("Audio (*.wav;*.mp3;*.ogg;*.flac)|*.wav;*.mp3;*.ogg;*.flac");
		}
		if (Kind == TEXT("video"))
		{
			return TEXT("Video (*.mp4;*.mov;*.webm)|*.mp4;*.mov;*.webm");
		}
		return TEXT("Images (*.png;*.jpg;*.jpeg;*.webp)|*.png;*.jpg;*.jpeg;*.webp");
	}

	bool IsImageFile(const FString& Path)
	{
		const FString Extension = FPaths::GetExtension(Path).ToLower();
		return Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg") || Extension == TEXT("webp");
	}

	int32 WordCount(const FString& Text)
	{
		TArray<FString> Words;
		Text.ParseIntoArrayWS(Words);
		return Words.Num();
	}

	FText FieldTooltip(const FScenarioField& Field)
	{
		FString Text = Field.Description;
		if (Field.Type == EScenarioFieldType::Files && Field.MaxItems > 0)
		{
			Text += FString::Printf(TEXT("%sUp to %d."), Text.IsEmpty() ? TEXT("") : TEXT(" "), Field.MaxItems);
		}
		if (Field.bCostImpact)
		{
			Text += FString(Text.IsEmpty() ? TEXT("") : TEXT("\n")) + TEXT("Changes the price.");
		}
		return FText::FromString(Text);
	}

	bool IsDropRecognized(TSharedPtr<FDragDropOperation> Operation)
	{
		if (!Operation.IsValid())
		{
			return false;
		}
		if (Operation->IsOfType<FAssetDragDropOp>())
		{
			return StaticCastSharedPtr<FAssetDragDropOp>(Operation)->HasAssets();
		}
		return Operation->IsOfType<FExternalDragOperation>() && StaticCastSharedPtr<FExternalDragOperation>(Operation)->HasFiles();
	}
}

void SScenarioForm::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;
	LastPrompt = Model->GetPrompt();
	ChildSlot
	[
		SAssignNew(Content, SBox)
	];
	ModelHandle = Model->OnChanged.AddSP(this, &SScenarioForm::OnModelChanged);
	Rebuild();
	SchedulePriceSpark();
}

SScenarioForm::~SScenarioForm()
{
	if (Model.IsValid())
	{
		Model->OnChanged.Remove(ModelHandle);
	}
}

FString SScenarioForm::Signature() const
{
	FString Out = Model->GetModelId() + TEXT("|") + (Model->IsLoadingSchema() ? TEXT("loading") : TEXT("ready")) + TEXT("|") + Model->GetSchemaError();
	for (const FScenarioField& Field : Model->GetFields())
	{
		Out += TEXT("|") + Field.Name;
	}
	for (const FScenarioReference& Reference : Model->GetReferences())
	{
		Out += FString::Printf(TEXT("|r%d:%s"), Reference.Id, *Reference.Field);
	}
	return Out;
}

void SScenarioForm::OnModelChanged()
{
	const FString Prompt = Model->GetPrompt();
	const bool bPromptChanged = Prompt != LastPrompt;
	LastPrompt = Prompt;
	if (Signature() != BuiltSignature)
	{
		const bool bNewModel = !BuiltSignature.StartsWith(Model->GetModelId() + TEXT("|"));
		if (bNewModel)
		{
			SparkVariants.Reset();
			SparkPrice.Reset();
			SparkPricedPrompt.Reset();
			SparkMessage.Reset();
			bShowMore = false;
		}
		InlineError.Reset();
		Rebuild();
		SchedulePriceSpark();
		return;
	}
	if (bPromptChanged)
	{
		SparkMessage.Reset();
		SchedulePriceSpark();
	}
}

void SScenarioForm::FocusPrompt()
{
	if (PromptBox.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(PromptBox, EFocusCause::SetDirectly);
	}
}

void SScenarioForm::RunAdd(TFunction<FString()> Action)
{
	InlineError = Action();
	Rebuild();
}

// Layout

void SScenarioForm::Rebuild()
{
	BuiltSignature = Signature();
	PromptBox.Reset();
	VariantsBox.Reset();
	SlotThumbnails.Reset();
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	if (Model->IsLoadingSchema())
	{
		Box->AddSlot().AutoHeight().Padding(ScenarioUI::Gutter, ScenarioUI::Gutter)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SCircularThrobber).Radius(7.0f)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("Loading", "Loading {0} settings..."), FText::FromString(Model->GetModelName())))
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				.ColorAndOpacity(FSlateColor(ScenarioUI::Palette::Dim()))
			]
		];
		Content->SetContent(Box);
		return;
	}
	if (!Model->GetSchemaError().IsEmpty())
	{
		Box->AddSlot().AutoHeight().Padding(ScenarioUI::Gutter, ScenarioUI::Gutter)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Model->GetSchemaError()))
			.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
			.ColorAndOpacity(ScenarioUI::Palette::Error())
			.AutoWrapText(true)
		];
		Content->SetContent(Box);
		return;
	}

	const TArray<FScenarioField>& Fields = Model->GetFields();
	const FString PromptName = ScenarioSchema::PromptName(Fields);
	if (const FScenarioField* Prompt = ScenarioSchema::Find(Fields, PromptName))
	{
		Box->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, ScenarioUI::SectionGap)[BuildPrompt(*Prompt)];
	}

	// References: one row of drop-in slots per file input.
	TSharedRef<SGridPanel> References = SNew(SGridPanel).FillColumn(1, 1.0f);
	int32 ReferenceRows = 0;
	for (const FScenarioField& Field : Fields)
	{
		if (Field.IsFile())
		{
			AddRow(References, ReferenceRows, FText::FromString(Field.Label), FieldTooltip(Field), BuildSlots(Field), !Field.bRequired, false);
		}
	}
	if (ReferenceRows > 0)
	{
		Box->AddSlot().AutoHeight()[ScenarioUI::SectionHeader(LOCTEXT("Inputs", "Inputs"))];
		Box->AddSlot().AutoHeight().Padding(ScenarioUI::Gutter, ScenarioUI::RowGap, ScenarioUI::Gutter, 0.0f)[References];
		Box->AddSlot().AutoHeight().Padding(ScenarioUI::Gutter, 2.0f, ScenarioUI::Gutter, 0.0f)
		[
			SNew(STextBlock)
			.Text_Lambda([this]() { return FText::FromString(InlineError); })
			.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
			.ColorAndOpacity(ScenarioUI::Palette::Error())
			.AutoWrapText(true)
			.Visibility_Lambda([this]() { return InlineError.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		];
		Box->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(ScenarioUI::SectionGap)];
	}

	// Settings: the ones that matter first, the rest behind "N more settings".
	const FScenarioField* Width = ScenarioSchema::Find(Fields, TEXT("width"));
	const FScenarioField* Height = ScenarioSchema::Find(Fields, TEXT("height"));
	const bool bSize = Model->GetLaneId() == TEXT("material") && !Model->IsWorkflow() && Width && Height
		&& Width->Type == EScenarioFieldType::Integer && Height->Type == EScenarioFieldType::Integer;
	TSharedRef<SGridPanel> Settings = SNew(SGridPanel).FillColumn(1, 1.0f);
	int32 SettingRows = 0;
	if (bSize)
	{
		AddRow(Settings, SettingRows, LOCTEXT("Size", "Size"), LOCTEXT("SizeTip", "The texture size in pixels.\nChanges the price."), BuildSize(*Width, *Height), false, false);
	}
	auto Skip = [&PromptName, bSize](const FScenarioField& Field)
	{
		return Field.IsFile() || Field.Name == PromptName || (bSize && (Field.Name == TEXT("width") || Field.Name == TEXT("height")));
	};
	for (const FScenarioField& Field : Fields)
	{
		if (!Skip(Field) && Field.IsPrimary())
		{
			AddFieldRow(Settings, SettingRows, Field, false);
		}
	}
	if (Model->IsUnrealReadyModel())
	{
		const TSharedPtr<FScenarioCreateModel> M = Model;
		AddRow(Settings, SettingRows, LOCTEXT("UnrealReady", "Unreal-ready"),
			FText::Format(LOCTEXT("UnrealReadyTip", "Adds these constraints to the prompt sent to Scenario: {0}"), FText::FromString(FScenarioLanes::Get().UnrealReadyPrompt())),
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([M]() { return M->IsUnrealReady() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([M](ECheckBoxState State) { M->SetUnrealReady(State == ECheckBoxState::Checked); })
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
			[
				ScenarioUI::Line(LOCTEXT("UnrealReadyNote", "Real-world scale, pivot at the base, clean topology"), ScenarioUI::EText::Meta, true)
			],
			false, false);
	}
	int32 MoreCount = 0;
	for (const FScenarioField& Field : Fields)
	{
		if (!Skip(Field) && !Field.IsPrimary())
		{
			AddFieldRow(Settings, SettingRows, Field, true);
			++MoreCount;
		}
	}
	if (SettingRows > 0)
	{
		const TSharedPtr<SWidget> Toggle = MoreCount == 0 ? TSharedPtr<SWidget>() : StaticCastSharedRef<SWidget>(
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(FMargin(4.0f, 0.0f))
			.ToolTipText(LOCTEXT("MoreTip", "Shows or hides the model's other settings."))
			.OnClicked_Lambda([this]() { bShowMore = !bShowMore; return FReply::Handled(); })
			[
				SNew(STextBlock)
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				.Text_Lambda([this, MoreCount]()
				{
					return bShowMore ? LOCTEXT("Fewer", "Fewer settings") : FText::Format(LOCTEXT("More", "{0} more {0}|plural(one=setting,other=settings)"), FText::AsNumber(MoreCount));
				})
			]);
		Box->AddSlot().AutoHeight()[ScenarioUI::SectionHeader(LOCTEXT("Settings", "Settings"), Toggle)];
		Box->AddSlot().AutoHeight().Padding(ScenarioUI::Gutter, ScenarioUI::RowGap, ScenarioUI::Gutter, 0.0f)[Settings];
	}
	Content->SetContent(Box);
}

void SScenarioForm::AddRow(const TSharedRef<SGridPanel>& Grid, int32& Row, const FText& Label, const FText& Tooltip, const TSharedRef<SWidget>& Value, bool bOptional, bool bMore, const FText& Hint)
{
	const TAttribute<EVisibility> Visible = bMore
		? TAttribute<EVisibility>::CreateLambda([this]() { return bShowMore ? EVisibility::Visible : EVisibility::Collapsed; })
		: TAttribute<EVisibility>(EVisibility::Visible);
	const FText Note = !Hint.IsEmpty() ? Hint : bOptional ? LOCTEXT("Optional", "Optional") : FText::GetEmpty();
	Grid->AddSlot(0, Row)
		.VAlign(VAlign_Top)
		[
			SNew(SBox)
			.Visibility(Visible)
			.WidthOverride(ScenarioUI::LabelWidth)
			.MinDesiredHeight(RowHeight)
			.VAlign(VAlign_Center)
			.Padding(0.0f, 3.0f, ScenarioUI::Gutter, 3.0f)
			[
				ScenarioUI::FieldLabel(Label, Note, Tooltip)
			]
		];
	Grid->AddSlot(1, Row)
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.Visibility(Visible)
			.MinDesiredHeight(RowHeight)
			.VAlign(VAlign_Center)
			.Padding(0.0f, 3.0f)
			[
				Value
			]
		];
	++Row;
}

void SScenarioForm::AddFieldRow(const TSharedRef<SGridPanel>& Grid, int32& Row, const FScenarioField& Field, bool bMore)
{
	AddRow(Grid, Row, FText::FromString(Field.Label), FieldTooltip(Field), BuildControl(Field), false, bMore,
		Field.bCostImpact ? LOCTEXT("ChangesPrice", "Changes the price") : FText::GetEmpty());
}

// Prompt

TSharedRef<SWidget> SScenarioForm::BuildPrompt(const FScenarioField& Field)
{
	const FScenarioLane* Lane = Model->GetLane();
	FString Hint = Field.Placeholder;
	if (Hint.IsEmpty() && Lane && Lane->StarterPrompts.Num() > 0 && !Model->IsWorkflow())
	{
		Hint = Lane->StarterPrompts[0];
	}
	const int32 MaxLength = Field.MaxLength;

	TSharedRef<SHorizontalBox> Tools = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			BuildExamples()
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			BuildImprove()
		];

	SAssignNew(VariantsBox, SVerticalBox);
	RefreshVariants();

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			ScenarioUI::SectionHeader(FText::FromString(Field.Label.IsEmpty() || Field.Label == TEXT("Text") ? FString(TEXT("Prompt")) : Field.Label), Tools)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(ScenarioUI::Gutter, ScenarioUI::RowGap, ScenarioUI::Gutter, 0.0f)
		[
			SNew(SBox)
			.MinDesiredHeight(76.0f)
			.MaxDesiredHeight(180.0f)
			[
				SAssignNew(PromptBox, SMultiLineEditableTextBox)
				.Style(&ScenarioUI::Style().GetWidgetStyle<FEditableTextBoxStyle>("Scenario.Prompt"))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				.Text_Lambda([this]() { return FText::FromString(Model->GetPrompt()); })
				.HintText(FText::FromString(Hint))
				.AutoWrapText(true)
				.OnTextChanged_Lambda([this, MaxLength](const FText& Text)
				{
					FString Value = Text.ToString();
					if (MaxLength > 0 && Value.Len() > MaxLength)
					{
						Value.LeftInline(MaxLength);
					}
					Model->SetPrompt(Value);
				})
				.OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& Key)
				{
					if (Key.GetKey() == EKeys::Enter && (Key.IsControlDown() || Key.IsCommandDown()))
					{
						Model->Generate();
						return FReply::Handled();
					}
					return FReply::Unhandled();
				})
			]
		]
		// The counter shows once the prompt nears the model's limit.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Right)
		.Padding(ScenarioUI::Gutter, 2.0f, ScenarioUI::Gutter, 0.0f)
		[
			SNew(STextBlock)
			.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
			.ColorAndOpacity_Lambda([this, MaxLength]() { return Model->GetPrompt().Len() >= MaxLength ? FSlateColor(ScenarioUI::Palette::Warning()) : FSlateColor(ScenarioUI::Palette::Dim()); })
			.Visibility_Lambda([this, MaxLength]() { return MaxLength > 0 && Model->GetPrompt().Len() > MaxLength * 8 / 10 ? EVisibility::Visible : EVisibility::Collapsed; })
			.Text_Lambda([this, MaxLength]() { return FText::Format(LOCTEXT("Count", "{0} / {1}"), FText::AsNumber(Model->GetPrompt().Len()), FText::AsNumber(MaxLength)); })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(ScenarioUI::Gutter, 2.0f, ScenarioUI::Gutter, 0.0f)
		[
			SNew(STextBlock)
			.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
			.AutoWrapText(true)
			.Text_Lambda([this]() { return FText::FromString(SparkMessage); })
			.ColorAndOpacity_Lambda([this]() { return bSparkMessageError ? FSlateColor(ScenarioUI::Palette::Error()) : FSlateColor(ScenarioUI::Palette::Dim()); })
			.Visibility_Lambda([this]() { return SparkMessage.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(ScenarioUI::Gutter, 0.0f, ScenarioUI::Gutter, 0.0f)
		[
			VariantsBox.ToSharedRef()
		];
}

TSharedRef<SWidget> SScenarioForm::BuildExamples()
{
	const FScenarioLane* Lane = Model->GetLane();
	if (Model->IsWorkflow() || !Lane || Lane->StarterPrompts.Num() == 0)
	{
		return SNullWidget::NullWidget;
	}
	const TArray<FString> Starters = Lane->StarterPrompts;
	const TSharedPtr<FScenarioCreateModel> M = Model;
	return SNew(SComboButton)
		.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
		.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>("Scenario.Button.Ghost.Small"))
		.HasDownArrow(false)
		.ToolTipText(LOCTEXT("ExamplesTip", "Fills the prompt with an example for this lane. Sends nothing."))
		.ButtonContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("Examples", "Examples")).Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(3.0f, 1.0f, 0.0f, 0.0f)
			[
				ScenarioUI::Icon(TEXT("chevron-down"), 10.0f, ScenarioUI::Palette::Dim())
			]
		]
		.OnGetMenuContent_Lambda([M, Starters]()
		{
			FMenuBuilder Menu(true, nullptr);
			for (const FString& Starter : Starters)
			{
				Menu.AddMenuEntry(FText::FromString(Starter), LOCTEXT("ExampleTip", "Uses this prompt. Sends nothing."), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([M, Starter]() { M->SetPrompt(Starter); })));
			}
			return Menu.MakeWidget();
		});
}

// Improve prompt (Prompt Spark)

TSharedRef<SWidget> SScenarioForm::BuildImprove()
{
	if (Model->IsWorkflow())
	{
		return SNullWidget::NullWidget;
	}
	return SNew(SButton)
		.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>("Scenario.Button.Outline.Small"))
		.IsEnabled_Lambda([this]() { return !bSparkRunning && !bSparkPricing && IsSparkPriced(); })
		.ToolTipText_Lambda([this]()
		{
			if (WordCount(Model->GetPrompt()) < SparkMinWords)
			{
				return LOCTEXT("ImproveShortTip", "Write at least three words. Scenario then suggests three better prompts for this model.");
			}
			if (!IsSparkPriced())
			{
				return LOCTEXT("ImprovePricingTip", "Checking the price (free).");
			}
			return FText::Format(LOCTEXT("ImproveTip", "Suggests three better prompts for {0}. Scenario charges {1}. Your prompt stays until you pick one."),
				FText::FromString(Model->GetModelName()), FText::FromString(SparkPrice));
		})
		.OnClicked_Lambda([this]()
		{
			if (!IsSparkPriced())
			{
				return FReply::Handled();
			}
			if (FPlatformTime::Seconds() - SparkPricedAt > SparkQuoteAge)
			{
				// The quote is about to expire: check it again and run if the price the person saw holds.
				PriceSpark(true, SparkPrice);
				return FReply::Handled();
			}
			RunSpark();
			return FReply::Handled();
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SCircularThrobber)
					.Radius(5.0f)
					.Visibility_Lambda([this]() { return bSparkRunning ? EVisibility::Visible : EVisibility::Collapsed; })
				]
				+ SOverlay::Slot()
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return bSparkRunning ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						ScenarioUI::Icon(TEXT("wand"), 12.0f)
					]
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				.Text_Lambda([this]()
				{
					if (bSparkRunning)
					{
						return LOCTEXT("Improving", "Improving...");
					}
					return IsSparkPriced()
						? FText::Format(LOCTEXT("ImprovePriced", "Improve prompt · {0}"), FText::FromString(SparkPrice))
						: LOCTEXT("Improve", "Improve prompt");
				})
			]
		];
}

bool SScenarioForm::IsSparkPriced() const
{
	return !SparkPrice.IsEmpty() && SparkPricedPrompt == Model->GetPrompt();
}

void SScenarioForm::SetSparkMessage(const FString& Text, bool bError)
{
	SparkMessage = Text;
	bSparkMessageError = bError;
}

void SScenarioForm::SchedulePriceSpark()
{
	if (SparkTimer.IsValid())
	{
		UnRegisterActiveTimer(SparkTimer.ToSharedRef());
		SparkTimer.Reset();
	}
	if (Model->IsWorkflow() || Model->IsLoadingSchema() || bSparkRunning || IsSparkPriced() || WordCount(Model->GetPrompt()) < SparkMinWords)
	{
		return;
	}
	SparkTimer = RegisterActiveTimer(0.8f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
	{
		SparkTimer.Reset();
		PriceSpark(false, FString());
		return EActiveTimerReturnType::Stop;
	}));
}

void SScenarioForm::PriceSpark(bool bRunAfter, const FString& ShownPrice)
{
	const FString Prompt = Model->GetPrompt();
	bSparkPricing = true;
	TWeakPtr<SScenarioForm> WeakThis = StaticCastSharedRef<SScenarioForm>(AsShared());
	Model->PriceSpark([WeakThis, Prompt, bRunAfter, ShownPrice](const FString& Price, const FString& Error)
	{
		const TSharedPtr<SScenarioForm> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		This->bSparkPricing = false;
		if (Prompt != This->Model->GetPrompt())
		{
			// Typed on since: the new prompt gets its own check.
			This->SchedulePriceSpark();
			return;
		}
		if (!Error.IsEmpty())
		{
			This->SparkPrice.Reset();
			This->SetSparkMessage(FString::Printf(TEXT("Improve prompt is unavailable: %s"), *Error), true);
			return;
		}
		This->SparkPrice = Price;
		This->SparkPricedPrompt = Prompt;
		This->SparkPricedAt = FPlatformTime::Seconds();
		if (bRunAfter)
		{
			if (Price == ShownPrice)
			{
				This->RunSpark();
			}
			else
			{
				This->SetSparkMessage(FString::Printf(TEXT("The price is now %s. Click Improve prompt again to run it."), *Price), false);
			}
		}
	});
}

void SScenarioForm::RunSpark()
{
	bSparkRunning = true;
	SparkMessage.Reset();
	const FString Price = SparkPrice;
	TWeakPtr<SScenarioForm> WeakThis = StaticCastSharedRef<SScenarioForm>(AsShared());
	Model->RunSpark([WeakThis, Price](const TArray<FString>& Variants, const FString& Error, const FString& LocalId)
	{
		const TSharedPtr<SScenarioForm> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		This->bSparkRunning = false;
		// The quote is used: the next run needs a new price.
		This->SparkPrice.Reset();
		This->SparkPricedPrompt.Reset();
		if (!Error.IsEmpty())
		{
			This->SetSparkMessage(Error, true);
		}
		else if (Variants.Num() == 0)
		{
			FString Charge = FString::Printf(TEXT("Priced at %s."), *Price);
			const FScenarioSession* Session = ScenarioUI::Session();
			if (const FScenarioJobRow* Row = Session && !LocalId.IsEmpty() ? Session->Ledger().Find(LocalId) : nullptr)
			{
				if (Row->ChargedCU == 0.0)
				{
					Charge = TEXT("Nothing was charged.");
				}
				else if (Row->ChargedCU > 0.0)
				{
					Charge = FString::Printf(TEXT("Charged %s."), *ScenarioCU::Format(Row->ChargedCU));
				}
			}
			This->SetSparkMessage(FString::Printf(TEXT("No suggestion came back. %s Edit the prompt and try again."), *Charge), true);
		}
		else
		{
			This->SetSparkMessage(TEXT("Pick a suggestion to use it, or keep your prompt."), false);
		}
		This->SparkVariants = Variants;
		This->RefreshVariants();
		This->SchedulePriceSpark();
	});
}

void SScenarioForm::RefreshVariants()
{
	if (!VariantsBox.IsValid())
	{
		return;
	}
	VariantsBox->ClearChildren();
	const TSharedPtr<FScenarioCreateModel> M = Model;
	for (const FString& Variant : SparkVariants)
	{
		VariantsBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, ScenarioUI::RowGap, 0.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(ScenarioUI::Style().GetBrush("Scenario.Card"))
			.Padding(FMargin(10.0f, 8.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Variant))
					.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
					.AutoWrapText(true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>("Scenario.Button.Small"))
					.Text(LOCTEXT("UseVariant", "Use"))
					.ToolTipText(LOCTEXT("UseVariantTip", "Replaces the prompt with this one. Sends nothing."))
					.IsEnabled_Lambda([M, Variant]() { return M->GetPrompt() != Variant; })
					.OnClicked_Lambda([M, Variant]() { M->SetPrompt(Variant); return FReply::Handled(); })
				]
			]
		];
	}
}

// References

TSharedRef<SWidget> SScenarioForm::BuildSlots(const FScenarioField& Field)
{
	TSharedRef<SWrapBox> Box = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 4.0f));
	const TArray<FScenarioReference> Items = Model->ReferencesFor(Field.Name);
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		Box->AddSlot()[BuildSlot(Field, Items[Index], Index)];
	}
	const bool bRoom = Field.Type == EScenarioFieldType::Files ? (Field.MaxItems <= 0 || Items.Num() < Field.MaxItems) : Items.Num() == 0;
	if (bRoom)
	{
		Box->AddSlot()[BuildAddSlot(Field, Items.Num() == 0)];
	}
	const FString Name = Field.Name;
	return SNew(SDropTarget)
		.OnAllowDrop_Static(&IsDropRecognized)
		.OnIsRecognized_Static(&IsDropRecognized)
		.OnDropped_Lambda([this, Name](const FGeometry&, const FDragDropEvent& Event)
		{
			if (const TSharedPtr<FAssetDragDropOp> Assets = Event.GetOperationAs<FAssetDragDropOp>())
			{
				const TArray<FAssetData> Dropped = Assets->GetAssets();
				RunAdd([this, Name, Dropped]() { return Model->AddAssets(Name, Dropped); });
			}
			else if (const TSharedPtr<FExternalDragOperation> External = Event.GetOperationAs<FExternalDragOperation>())
			{
				if (External->HasFiles())
				{
					const TArray<FString> Files = External->GetFiles();
					RunAdd([this, Name, Files]() { return Model->AddFiles(Name, Files); });
				}
			}
			return FReply::Handled();
		})
		[
			SNew(SBox)
			.HAlign(HAlign_Left)
			[
				Box
			]
		];
}

TSharedRef<SWidget> SScenarioForm::BuildSlot(const FScenarioField& Field, const FScenarioReference& Reference, int32 IndexInField)
{
	const int32 Id = Reference.Id;
	const TSharedPtr<FScenarioCreateModel> M = Model;
	auto Find = [M, Id]() -> const FScenarioReference*
	{
		return M->GetReferences().FindByPredicate([Id](const FScenarioReference& R) { return R.Id == Id; });
	};

	// The picture: the source asset's own thumbnail, else the file, else the kind.
	TSharedRef<SWidget> Picture = SNullWidget::NullWidget;
	const FString Kind = Field.Kind.IsEmpty() ? FString(TEXT("image")) : Field.Kind;
	if (!Reference.ObjectPath.IsEmpty())
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		const FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(Reference.ObjectPath));
		if (Asset.IsValid())
		{
			const TSharedPtr<FAssetThumbnail> Thumbnail = MakeShared<FAssetThumbnail>(Asset, 96, 96, FScenarioThumbnails::Get().AssetPool());
			SlotThumbnails.Add(Thumbnail);
			FAssetThumbnailConfig Config;
			Config.bAllowFadeIn = false;
			Config.bAllowHintText = false;
			Config.ShowAssetColor = false;
			Picture = SNew(SBox).WidthOverride(SlotSize).HeightOverride(SlotSize)[Thumbnail->MakeThumbnailWidget(Config)];
		}
	}
	if (Picture == SNullWidget::NullWidget)
	{
		const FString Path = Reference.LocalPath;
		Picture = ScenarioUI::Thumbnail(SlotSize,
			TAttribute<const FSlateBrush*>::CreateLambda([Path]() { return IsImageFile(Path) ? FScenarioThumbnails::Get().ForFile(Path) : nullptr; }),
			Kind);
	}

	// Built first so the remove button can follow its hover.
	const TSharedRef<SBox> Tile = SNew(SBox)
		.WidthOverride(SlotSize)
		.HeightOverride(SlotSize)
		.ToolTipText_Lambda([Find]()
		{
			const FScenarioReference* R = Find();
			if (!R)
			{
				return FText::GetEmpty();
			}
			if (!R->Error.IsEmpty())
			{
				return FText::Format(LOCTEXT("SlotError", "{0}\n{1}"), FText::FromString(R->Label), FText::FromString(R->Error));
			}
			return FText::Format(R->bUploading ? LOCTEXT("SlotUploading", "{0}\nUploading to Scenario...") : LOCTEXT("SlotReady", "{0}\nOn Scenario. It is sent with the request."), FText::FromString(R->Label));
		});
	const TWeakPtr<SBox> WeakTile = Tile;
	Tile->SetContent(
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				Picture
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Top)
			.Padding(2.0f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.WarningWithColor"))
				.DesiredSizeOverride(FVector2D(12.0f, 12.0f))
				.Visibility_Lambda([Find]() { const FScenarioReference* R = Find(); return R && !R->Error.IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Top)
			[
				SNew(SButton)
				.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>("Scenario.Button.Small"))
				.ContentPadding(FMargin(2.0f))
				.ToolTipText(LOCTEXT("RemoveTip", "Remove this reference from the request."))
				.Visibility_Lambda([WeakTile]()
				{
					const TSharedPtr<SBox> Pinned = WeakTile.Pin();
					return Pinned.IsValid() && Pinned->IsHovered() ? EVisibility::Visible : EVisibility::Hidden;
				})
				.OnClicked_Lambda([this, Name = Field.Name, IndexInField]()
				{
					Model->RemoveReference(Name, IndexInField);
					InlineError.Reset();
					Rebuild();
					return FReply::Handled();
				})
				[
					ScenarioUI::Icon(TEXT("close"), 10.0f)
				]
			]
			+ SOverlay::Slot()
			.VAlign(VAlign_Bottom)
			[
				SNew(SBox)
				.HeightOverride(3.0f)
				.Visibility_Lambda([Find]() { const FScenarioReference* R = Find(); return R && R->bUploading ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				[
					SNew(SProgressBar)
					.Percent_Lambda([Find]() { const FScenarioReference* R = Find(); return R ? TOptional<float>(R->Progress) : TOptional<float>(); })
				]
			]);
	return Tile;
}

TSharedRef<SWidget> SScenarioForm::BuildAddSlot(const FScenarioField& Field, bool bWide)
{
	const FScenarioField Copy = Field;
	const FString Kind = Field.Kind.IsEmpty() ? FString(TEXT("image")) : Field.Kind;
	const FText Noun = Kind == TEXT("3d") ? LOCTEXT("Noun3d", "a mesh") : Kind == TEXT("audio") ? LOCTEXT("NounAudio", "a sound") : Kind == TEXT("video") ? LOCTEXT("NounVideo", "a video") : LOCTEXT("NounImage", "an image");
	return SNew(SComboButton)
		.HasDownArrow(false)
		.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
		.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>("Scenario.Button.Row"))
		.ContentPadding(FMargin(0.0f))
		.ToolTipText(LOCTEXT("AddTip", "Add a reference: capture the viewport, use the Content Browser selection or choose a file. You can also drop assets or files here."))
		.OnGetMenuContent_Lambda([this, Copy]() { return BuildAddMenu(Copy); })
		.ButtonContent()
		[
			SNew(SBox)
			.WidthOverride(bWide ? FOptionalSize() : FOptionalSize(SlotSize))
			.MinDesiredWidth(bWide ? 240.0f : SlotSize)
			.HeightOverride(SlotSize)
			[
				SNew(SBorder)
				.BorderImage(ScenarioUI::Style().GetBrush("Scenario.Slot"))
				.HAlign(bWide ? HAlign_Left : HAlign_Center)
				.VAlign(VAlign_Center)
				.Padding(FMargin(bWide ? 14.0f : 0.0f, 0.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						ScenarioUI::Icon(bWide ? TEXT("upload") : TEXT("plus"), 16.0f, ScenarioUI::Palette::Dim())
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(10.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SVerticalBox)
						.Visibility(bWide ? EVisibility::Visible : EVisibility::Collapsed)
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(STextBlock)
							.Text(FText::Format(LOCTEXT("DropHere", "Drop {0} here"), Noun))
							.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("DropHint", "or click: viewport, Content Browser, file"))
							.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
							.ColorAndOpacity(ScenarioUI::Palette::Faint())
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SScenarioForm::BuildAddMenu(FScenarioField Field)
{
	const FString Name = Field.Name;
	const FString Kind = Field.Kind;
	FMenuBuilder Menu(true, nullptr);
	if (Kind.IsEmpty() || Kind == TEXT("image"))
	{
		Menu.AddMenuEntry(LOCTEXT("Viewport", "Capture the viewport"), LOCTEXT("ViewportTip", "Renders the active level viewport (no gizmos) and uses it as this reference."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Camera"),
			FUIAction(FExecuteAction::CreateLambda([this, Name]() { RunAdd([this, Name]() { return Model->AddViewportCapture(Name); }); })));
	}
	Menu.AddMenuEntry(LOCTEXT("Content", "Use the Content Browser selection"), LOCTEXT("ContentTip", "Uses the assets selected in the Content Browser. Assets from Scenario are reused without an upload."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.BrowseContent"),
		FUIAction(FExecuteAction::CreateLambda([this, Name]() { RunAdd([this, Name]() { return Model->AddContentSelection(Name); }); })));
	if (Kind == TEXT("3d"))
	{
		Menu.AddMenuEntry(LOCTEXT("Actors", "Selected actors"), LOCTEXT("ActorsTip", "Exports the selected actors to one GLB and uses it as this input."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.PlaceActors"),
			FUIAction(FExecuteAction::CreateLambda([this, Name]() { RunAdd([this, Name]() { return Model->AddSelectedActors(Name); }); })));
	}
	Menu.AddMenuEntry(LOCTEXT("File", "Choose a file..."), LOCTEXT("FileTip", "Picks a file on disk and uploads it to your Scenario project."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.OpenFile"),
		FUIAction(FExecuteAction::CreateLambda([this, Name, Kind]()
		{
			IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
			if (!Desktop)
			{
				return;
			}
			TArray<FString> Files;
			const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
			if (Desktop->OpenFileDialog(Parent, LOCTEXT("PickFile", "Choose a reference").ToString(), FString(), FString(), FileTypesFor(Kind), EFileDialogFlags::None, Files) && Files.Num() > 0)
			{
				const FString Path = Files[0];
				RunAdd([this, Name, Path]() { return Model->AddReferenceFile(Name, Path, FString()); });
			}
		})));
	return Menu.MakeWidget();
}

// Settings

TSharedRef<SWidget> SScenarioForm::BuildSize(const FScenarioField& Width, const FScenarioField& Height)
{
	const TSharedPtr<FScenarioCreateModel> M = Model;
	TArray<int32> Sizes;
	for (const int32 Size : { 512, 1024, 2048, 4096 })
	{
		const bool bFits = (!Width.Min.IsSet() || Size >= Width.Min.GetValue()) && (!Width.Max.IsSet() || Size <= Width.Max.GetValue())
			&& (!Height.Min.IsSet() || Size >= Height.Min.GetValue()) && (!Height.Max.IsSet() || Size <= Height.Max.GetValue());
		if (bFits)
		{
			Sizes.Add(Size);
		}
	}
	auto Number = [M](const FString& Name)
	{
		double Value = 0.0;
		const TSharedPtr<FJsonValue> Json = M->GetValue(Name);
		return Json.IsValid() && Json->TryGetNumber(Value) ? FMath::RoundToInt(Value) : 0;
	};
	return SNew(SBox)
		.HAlign(HAlign_Left)
		[
			SNew(SBox)
			.MinDesiredWidth(160.0f)
			[
				SNew(SComboButton)
				.ComboButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FComboButtonStyle>("Scenario.Combo"))
				.ButtonContent()
				[
					SNew(STextBlock)
					.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
					.Text_Lambda([Number]()
					{
						const int32 W = Number(TEXT("width"));
						const int32 H = Number(TEXT("height"));
						return W > 0 && H > 0 ? FText::Format(LOCTEXT("SizeValue", "{0} × {1}"), FText::AsNumber(W, &FNumberFormattingOptions::DefaultNoGrouping()), FText::AsNumber(H, &FNumberFormattingOptions::DefaultNoGrouping()))
							: LOCTEXT("SizeDefault", "Model default");
					})
				]
				.OnGetMenuContent_Lambda([M, Sizes, Number]()
				{
					FMenuBuilder Menu(true, nullptr);
					for (const int32 Size : Sizes)
					{
						Menu.AddMenuEntry(FText::Format(LOCTEXT("SizeEntry", "{0} × {0}"), FText::AsNumber(Size, &FNumberFormattingOptions::DefaultNoGrouping())), FText::GetEmpty(), FSlateIcon(),
							FUIAction(
								FExecuteAction::CreateLambda([M, Size]()
								{
									M->SetValue(TEXT("width"), MakeShared<FJsonValueNumber>(Size));
									M->SetValue(TEXT("height"), MakeShared<FJsonValueNumber>(Size));
								}),
								FCanExecuteAction(),
								FIsActionChecked::CreateLambda([Number, Size]() { return Number(TEXT("width")) == Size && Number(TEXT("height")) == Size; })),
							NAME_None, EUserInterfaceActionType::RadioButton);
					}
					return Menu.MakeWidget();
				})
			]
		];
}

TSharedRef<SWidget> SScenarioForm::BuildControl(const FScenarioField& Field)
{
	const FString Name = Field.Name;
	const TSharedPtr<FScenarioCreateModel> M = Model;
	// Numbers carry their unit after the box.
	auto WithUnit = [&Field](const TSharedRef<SWidget>& Box) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(140.0f)
				[
					Box
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Visibility(Field.Unit.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				.Text(FText::FromString(Field.Unit))
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				.ColorAndOpacity(ScenarioUI::Palette::Dim())
			];
	};
	switch (Field.Type)
	{
	case EScenarioFieldType::Enum:
	{
		const FScenarioField Copy = Field;
		return SNew(SBox)
			.HAlign(HAlign_Left)
			[
				SNew(SBox)
				.MinDesiredWidth(160.0f)
				.MaxDesiredWidth(ControlWidth)
				[
					SNew(SComboButton)
					.ComboButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FComboButtonStyle>("Scenario.Combo"))
					.ButtonContent()
					[
						SNew(STextBlock)
						.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.Text_Lambda([M, Copy]()
						{
							const TSharedPtr<FJsonValue> Value = M->GetValue(Copy.Name);
							FString Text = Value.IsValid() ? ScenarioSchema::OptionText(Copy, Value) : FString(TEXT("Model default"));
							if (!Copy.Unit.IsEmpty() && Value.IsValid() && ScenarioJson::ValueText(Value).IsNumeric() && !Copy.OptionLabels.Contains(ScenarioJson::ValueText(Value)))
							{
								Text += TEXT(" ") + Copy.Unit;
							}
							return FText::FromString(Text);
						})
					]
					.OnGetMenuContent_Lambda([M, Copy]()
					{
						FMenuBuilder Menu(true, nullptr);
						for (const TSharedPtr<FJsonValue>& Option : Copy.Options)
						{
							FString Text = ScenarioSchema::OptionText(Copy, Option);
							if (!Copy.Unit.IsEmpty() && ScenarioJson::ValueText(Option).IsNumeric() && !Copy.OptionLabels.Contains(ScenarioJson::ValueText(Option)))
							{
								Text += TEXT(" ") + Copy.Unit;
							}
							Menu.AddMenuEntry(FText::FromString(Text), FText::GetEmpty(), FSlateIcon(),
								FUIAction(
									FExecuteAction::CreateLambda([M, Name = Copy.Name, Option]() { M->SetValue(Name, Option); }),
									FCanExecuteAction(),
									FIsActionChecked::CreateLambda([M, Name = Copy.Name, Option]() { return ScenarioJson::Canonical(M->GetValue(Name)) == ScenarioJson::Canonical(Option); })),
								NAME_None, EUserInterfaceActionType::RadioButton);
						}
						return Menu.MakeWidget();
					})
				]
			];
	}
	case EScenarioFieldType::Integer:
	{
		// Whole numbers show as "4", not "4.0".
		auto ToInt = [](const TOptional<double>& Value) { return Value.IsSet() ? TOptional<int32>(FMath::RoundToInt(Value.GetValue())) : TOptional<int32>(); };
		const TOptional<int32> Min = ToInt(Field.Min);
		const TOptional<int32> Max = ToInt(Field.Max);
		return WithUnit(SNew(SNumericEntryBox<int32>)
			.AllowSpin(Min.IsSet() && Max.IsSet())
			.MinValue(Min)
			.MaxValue(Max)
			.MinSliderValue(Min)
			.MaxSliderValue(Max)
			.UndeterminedString(LOCTEXT("Auto", "Auto"))
			.Delta(Field.Step.IsSet() ? FMath::Max(1, FMath::RoundToInt(Field.Step.GetValue())) : 1)
			.Value_Lambda([M, Name]() -> TOptional<int32>
			{
				const TSharedPtr<FJsonValue> Value = M->GetValue(Name);
				double Number = 0.0;
				return Value.IsValid() && Value->TryGetNumber(Number) ? TOptional<int32>(FMath::RoundToInt(Number)) : TOptional<int32>();
			})
			.OnValueCommitted_Lambda([M, Name](int32 Value, ETextCommit::Type) { M->SetValue(Name, MakeShared<FJsonValueNumber>(Value)); })
			.OnValueChanged_Lambda([M, Name](int32 Value) { M->SetValue(Name, MakeShared<FJsonValueNumber>(Value)); }));
	}
	case EScenarioFieldType::Number:
	{
		const TOptional<double> Min = Field.Min;
		const TOptional<double> Max = Field.Max;
		return WithUnit(SNew(SNumericEntryBox<double>)
			.AllowSpin(Min.IsSet() && Max.IsSet())
			.MinValue(Min)
			.MaxValue(Max)
			.MinSliderValue(Min)
			.MaxSliderValue(Max)
			.UndeterminedString(LOCTEXT("Auto", "Auto"))
			.Delta(Field.Step.IsSet() ? Field.Step.GetValue() : 0.0)
			.Value_Lambda([M, Name]() -> TOptional<double>
			{
				const TSharedPtr<FJsonValue> Value = M->GetValue(Name);
				double Number = 0.0;
				return Value.IsValid() && Value->TryGetNumber(Number) ? TOptional<double>(Number) : TOptional<double>();
			})
			.OnValueCommitted_Lambda([M, Name](double Value, ETextCommit::Type) { M->SetValue(Name, MakeShared<FJsonValueNumber>(Value)); })
			.OnValueChanged_Lambda([M, Name](double Value) { M->SetValue(Name, MakeShared<FJsonValueNumber>(Value)); }));
	}
	case EScenarioFieldType::Boolean:
		return SNew(SBox)
			.HAlign(HAlign_Left)
			[
				ScenarioUI::Switch(MakeAttributeLambda([M, Name]() { return ScenarioJson::Truthy(M->GetValue(Name)); }),
					[M, Name](bool bOn) { M->SetValue(Name, MakeShared<FJsonValueBoolean>(bOn)); })
			];
	case EScenarioFieldType::Multi:
	{
		TSharedRef<SWrapBox> Box = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6.0f, 6.0f));
		for (const TSharedPtr<FJsonValue>& Option : Field.Options)
		{
			const FString Item = ScenarioJson::ValueText(Option);
			auto Has = [M, Name, Item]()
			{
				const TSharedPtr<FJsonValue> Value = M->GetValue(Name);
				if (Value.IsValid() && Value->Type == EJson::Array)
				{
					for (const TSharedPtr<FJsonValue>& Entry : Value->AsArray())
					{
						if (ScenarioJson::ValueText(Entry) == Item)
						{
							return true;
						}
					}
				}
				return false;
			};
			Box->AddSlot()
			[
				ScenarioUI::ChipToggle(FText::FromString(ScenarioSchema::OptionText(Field, Option)), MakeAttributeLambda(Has), [M, Name, Item, Has]()
				{
					TArray<FString> Items;
					const TSharedPtr<FJsonValue> Value = M->GetValue(Name);
					if (Value.IsValid() && Value->Type == EJson::Array)
					{
						for (const TSharedPtr<FJsonValue>& Entry : Value->AsArray())
						{
							Items.Add(ScenarioJson::ValueText(Entry));
						}
					}
					if (!Has())
					{
						Items.AddUnique(Item);
					}
					else
					{
						Items.Remove(Item);
					}
					M->SetValue(Name, ScenarioJson::StringArrayValue(Items));
				}, NAME_None, TAttribute<FText>(), true)
			];
		}
		return Box;
	}
	case EScenarioFieldType::Prompt:
		return SNew(SBox)
			.MinDesiredHeight(48.0f)
			[
				SNew(SMultiLineEditableTextBox)
				.Style(&ScenarioUI::Style().GetWidgetStyle<FEditableTextBoxStyle>("Scenario.Prompt"))
				.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
				.AutoWrapText(true)
				.HintText(FText::FromString(Field.Placeholder))
				.Text_Lambda([M, Name]() { return FText::FromString(ScenarioJson::ValueText(M->GetValue(Name))); })
				.OnTextChanged_Lambda([M, Name](const FText& Text)
				{
					M->SetValue(Name, Text.IsEmpty() ? TSharedPtr<FJsonValue>() : MakeShared<FJsonValueString>(Text.ToString()));
				})
			];
	default:
		return SNew(SEditableTextBox)
			.Style(&ScenarioUI::Style().GetWidgetStyle<FEditableTextBoxStyle>("Scenario.Input"))
			.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
			.HintText(FText::FromString(Field.Placeholder))
			.Text_Lambda([M, Name]() { return FText::FromString(ScenarioJson::ValueText(M->GetValue(Name))); })
			.OnTextChanged_Lambda([M, Name](const FText& Text)
			{
				M->SetValue(Name, Text.IsEmpty() ? TSharedPtr<FJsonValue>() : MakeShared<FJsonValueString>(Text.ToString()));
			});
	}
}

#undef LOCTEXT_NAMESPACE
