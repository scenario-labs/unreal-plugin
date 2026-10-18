// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioForm.h"

#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IDesktopPlatform.h"
#include "ScenarioCreateModel.h"
#include "ScenarioJson.h"
#include "ScenarioLanes.h"
#include "ScenarioStyle.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioForm"

namespace
{
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

	FText LabelWithCost(const FScenarioField& Field)
	{
		return Field.bRequired ? FText::Format(LOCTEXT("Required", "{0} *"), FText::FromString(Field.Label)) : FText::FromString(Field.Label);
	}

	TSharedRef<SWidget> SmallButton(const FText& Label, const FText& Tip, TFunction<void()> Action)
	{
		return SNew(SBox)
			.Padding(0.0f, 0.0f, 4.0f, 4.0f)
			[
				SNew(SButton)
				.Text(Label)
				.ToolTipText(Tip)
				.OnClicked_Lambda([Action]() { Action(); return FReply::Handled(); })
			];
	}
}

void SScenarioForm::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;
	ChildSlot
	[
		SAssignNew(Content, SBox)
	];
	ModelHandle = Model->OnChanged.AddSP(this, &SScenarioForm::OnModelChanged);
	Rebuild();
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
	if (Signature() != BuiltSignature)
	{
		SparkVariants.Reset();
		SparkPrice.Reset();
		InlineError.Reset();
		Rebuild();
	}
}

void SScenarioForm::FocusPrompt()
{
	if (PromptBox.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(PromptBox, EFocusCause::SetDirectly);
	}
}

void SScenarioForm::ShowError(const FString& Error)
{
	InlineError = Error;
}

TSharedRef<SWidget> SScenarioForm::Section(const FText& Title, const TSharedRef<SWidget>& Body, const TSharedPtr<SWidget>& Extra)
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.Padding(10.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(Title)
					.Font(FScenarioStyle::Font(9, true))
					.ColorAndOpacity(FScenarioStyle::Muted())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					Extra.IsValid() ? Extra.ToSharedRef() : SNullWidget::NullWidget
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				Body
			]
		];
}

void SScenarioForm::Rebuild()
{
	BuiltSignature = Signature();
	PromptBox.Reset();
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	if (Model->IsLoadingSchema())
	{
		Box->AddSlot().AutoHeight().Padding(0.0f, 8.0f)
		[
			SNew(STextBlock).Text(LOCTEXT("Loading", "Loading the model's settings...")).ColorAndOpacity(FScenarioStyle::Muted())
		];
		Content->SetContent(Box);
		return;
	}
	if (!Model->GetSchemaError().IsEmpty())
	{
		Box->AddSlot().AutoHeight().Padding(0.0f, 8.0f)
		[
			SNew(STextBlock).Text(FText::FromString(Model->GetSchemaError())).ColorAndOpacity(FScenarioStyle::Error()).AutoWrapText(true)
		];
		Content->SetContent(Box);
		return;
	}

	const TArray<FScenarioField>& Fields = Model->GetFields();
	const FString PromptName = ScenarioSchema::PromptName(Fields);
	if (const FScenarioField* Prompt = ScenarioSchema::Find(Fields, PromptName))
	{
		Box->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[BuildPrompt(*Prompt)];
	}

	TSharedRef<SVerticalBox> References = SNew(SVerticalBox);
	int32 FileCount = 0;
	for (const FScenarioField& Field : Fields)
	{
		if (Field.IsFile())
		{
			References->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)[BuildReferences(Field)];
			++FileCount;
		}
	}
	if (FileCount > 0)
	{
		Box->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[Section(LOCTEXT("References", "References"), References)];
	}

	TSharedRef<SWrapBox> Primary = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(12.0f, 8.0f));
	TSharedRef<SWrapBox> More = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(12.0f, 8.0f));
	int32 PrimaryCount = 0;
	int32 MoreCount = 0;
	for (const FScenarioField& Field : Fields)
	{
		if (Field.IsFile() || Field.Name == PromptName)
		{
			continue;
		}
		if (Field.IsPrimary())
		{
			Primary->AddSlot()[BuildField(Field)];
			++PrimaryCount;
		}
		else
		{
			More->AddSlot()[BuildField(Field)];
			++MoreCount;
		}
	}
	if (PrimaryCount + MoreCount > 0)
	{
		TSharedRef<SVerticalBox> Settings = SNew(SVerticalBox);
		if (PrimaryCount > 0)
		{
			Settings->AddSlot().AutoHeight()[Primary];
		}
		if (MoreCount > 0)
		{
			Settings->AddSlot().AutoHeight().Padding(0.0f, PrimaryCount > 0 ? 8.0f : 0.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([this]() { return bShowMore ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					More
				]
			];
		}
		const TSharedPtr<SWidget> Toggle = MoreCount == 0 ? TSharedPtr<SWidget>() : StaticCastSharedRef<SWidget>(
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(LOCTEXT("MoreTip", "Shows the model's other settings."))
			.OnClicked_Lambda([this]() { bShowMore = !bShowMore; return FReply::Handled(); })
			[
				SNew(STextBlock)
				.Text_Lambda([this, MoreCount]()
				{
					return bShowMore ? LOCTEXT("Fewer", "Fewer settings") : FText::Format(LOCTEXT("More", "More settings ({0})"), FText::AsNumber(MoreCount));
				})
			]);
		Box->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[Section(LOCTEXT("Settings", "Settings"), Settings, Toggle)];
	}

	Box->AddSlot().AutoHeight()
	[
		SNew(STextBlock)
		.Text_Lambda([this]() { return FText::FromString(InlineError); })
		.ColorAndOpacity(FScenarioStyle::Error())
		.AutoWrapText(true)
		.Visibility_Lambda([this]() { return InlineError.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
	];
	Content->SetContent(Box);
}

TSharedRef<SWidget> SScenarioForm::BuildPrompt(const FScenarioField& Field)
{
	const FScenarioLane* Lane = Model->GetLane();
	FString Hint = Field.Placeholder;
	if (Hint.IsEmpty() && Lane && Lane->StarterPrompts.Num() > 0)
	{
		Hint = Lane->StarterPrompts[0];
	}
	const int32 MaxLength = Field.MaxLength;

	TSharedRef<SWrapBox> Starters = SNew(SWrapBox).UseAllottedSize(true);
	if (Lane)
	{
		for (int32 Index = 0; Index < FMath::Min(3, Lane->StarterPrompts.Num()); ++Index)
		{
			const FString Starter = Lane->StarterPrompts[Index];
			Starters->AddSlot()
			[
				SNew(SBox)
				.Padding(0.0f, 0.0f, 4.0f, 4.0f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(LOCTEXT("StarterTip", "Uses this example prompt."))
					.OnClicked_Lambda([this, Starter]() { Model->SetPrompt(Starter); return FReply::Handled(); })
					[
						SNew(STextBlock).Text(FText::FromString(Starter)).ColorAndOpacity(FScenarioStyle::Muted())
					]
				]
			];
		}
	}

	TSharedRef<SVerticalBox> Variants = SNew(SVerticalBox);
	for (const FString& Variant : SparkVariants)
	{
		Variants->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(LOCTEXT("VariantTip", "Uses this prompt. Sends nothing."))
			.OnClicked_Lambda([this, Variant]() { Model->SetPrompt(Variant); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(FText::FromString(Variant)).AutoWrapText(true)
			]
		];
	}

	const TSharedRef<SWidget> Spark =
		SNew(SButton)
		.Visibility(Model->IsWorkflow() ? EVisibility::Collapsed : EVisibility::Visible)
		.IsEnabled_Lambda([this]() { return !bSparkBusy && !Model->GetPrompt().TrimStartAndEnd().IsEmpty(); })
		.ToolTipText_Lambda([this]()
		{
			return SparkPrice.IsEmpty() || SparkPricedPrompt != Model->GetPrompt()
				? LOCTEXT("SparkPriceTip", "Prompt Spark writes three better prompts for this model. First click checks the price (free).")
				: FText::Format(LOCTEXT("SparkRunTip", "Runs Prompt Spark. Scenario charges {0} for it."), FText::FromString(SparkPrice));
		})
		.OnClicked_Lambda([this]()
		{
			const FString Prompt = Model->GetPrompt();
			TWeakPtr<SScenarioForm> WeakThis = StaticCastSharedRef<SScenarioForm>(AsShared());
			if (SparkPrice.IsEmpty() || SparkPricedPrompt != Prompt)
			{
				bSparkBusy = true;
				Model->PriceSpark([WeakThis, Prompt](const FString& PriceText)
				{
					if (const TSharedPtr<SScenarioForm> This = WeakThis.Pin())
					{
						This->bSparkBusy = false;
						This->SparkPrice = PriceText.EndsWith(TEXT("CU")) ? PriceText : FString();
						This->SparkPricedPrompt = Prompt;
						This->InlineError = PriceText.EndsWith(TEXT("CU")) ? FString() : PriceText;
					}
				});
				return FReply::Handled();
			}
			bSparkBusy = true;
			Model->RunSpark([WeakThis](const TArray<FString>& Result, const FString& Error)
			{
				if (const TSharedPtr<SScenarioForm> This = WeakThis.Pin())
				{
					This->bSparkBusy = false;
					This->SparkPrice.Reset();
					This->SparkVariants = Result;
					This->InlineError = Error;
					This->Rebuild();
				}
			});
			return FReply::Handled();
		})
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				if (bSparkBusy)
				{
					return LOCTEXT("SparkBusy", "Spark...");
				}
				return SparkPrice.IsEmpty() || SparkPricedPrompt != Model->GetPrompt()
					? LOCTEXT("Spark", "Spark")
					: FText::Format(LOCTEXT("SparkPriced", "Spark · {0}"), FText::FromString(SparkPrice));
			})
		];

	return Section(LOCTEXT("Prompt", "Prompt"),
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.MinDesiredHeight(72.0f)
			.MaxDesiredHeight(180.0f)
			[
				SAssignNew(PromptBox, SMultiLineEditableTextBox)
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
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.Visibility_Lambda([this]() { return Model->GetPrompt().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					Starters
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f)
			[
				SNew(STextBlock)
				.Visibility(MaxLength > 0 ? EVisibility::Visible : EVisibility::Collapsed)
				.Text_Lambda([this, MaxLength]() { return FText::Format(LOCTEXT("Count", "{0}/{1}"), FText::AsNumber(Model->GetPrompt().Len()), FText::AsNumber(MaxLength)); })
				.ColorAndOpacity(FScenarioStyle::Muted())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				Spark
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			Variants
		]);
}

TSharedRef<SWidget> SScenarioForm::BuildReferences(const FScenarioField& Field)
{
	const FString Name = Field.Name;
	const FString Kind = Field.Kind;
	TSharedRef<SWrapBox> Chips = SNew(SWrapBox).UseAllottedSize(true);
	const TArray<FScenarioReference> Items = Model->ReferencesFor(Name);
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		const int32 Id = Items[Index].Id;
		auto Find = [this, Id]() -> const FScenarioReference*
		{
			return Model->GetReferences().FindByPredicate([Id](const FScenarioReference& R) { return R.Id == Id; });
		};
		Chips->AddSlot()
		[
			SNew(SBox)
			.Padding(0.0f, 0.0f, 6.0f, 4.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(FMargin(8.0f, 3.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Items[Index].Label))
						.ColorAndOpacity_Lambda([Find]() { const FScenarioReference* R = Find(); return R && !R->Error.IsEmpty() ? FScenarioStyle::Error() : FSlateColor::UseForeground(); })
						.ToolTipText_Lambda([Find]()
						{
							const FScenarioReference* R = Find();
							if (!R)
							{
								return FText::GetEmpty();
							}
							if (!R->Error.IsEmpty())
							{
								return FText::FromString(R->Error);
							}
							return R->bUploading ? LOCTEXT("UploadingTip", "Uploading to Scenario...") : LOCTEXT("UploadedTip", "On Scenario. It is sent with the request.");
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SBox)
						.WidthOverride(40.0f)
						.Visibility_Lambda([Find]() { const FScenarioReference* R = Find(); return R && R->bUploading ? EVisibility::Visible : EVisibility::Collapsed; })
						[
							SNew(SProgressBar)
							.Percent_Lambda([Find]() { const FScenarioReference* R = Find(); return R ? TOptional<float>(R->Progress) : TOptional<float>(); })
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.ToolTipText(LOCTEXT("RemoveTip", "Removes this reference from the request."))
						.OnClicked_Lambda([this, Name, Index]() { Model->RemoveReference(Name, Index); Rebuild(); return FReply::Handled(); })
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("×")))
						]
					]
				]
			]
		];
	}

	TSharedRef<SWrapBox> Add = SNew(SWrapBox).UseAllottedSize(true);
	auto Run = [this](TFunction<FString()> Action)
	{
		InlineError = Action();
		Rebuild();
	};
	if (Kind.IsEmpty() || Kind == TEXT("image"))
	{
		Add->AddSlot()[SmallButton(LOCTEXT("Viewport", "+ Viewport"), LOCTEXT("ViewportTip", "Renders the active level viewport (no gizmos) and uses it as this reference."),
			[this, Run, Name]() { Run([this, Name]() { return Model->AddViewportCapture(Name); }); })];
	}
	if (Kind == TEXT("3d"))
	{
		Add->AddSlot()[SmallButton(LOCTEXT("Actors", "+ Selected actors"), LOCTEXT("ActorsTip", "Exports the selected actors to one GLB and uses it as this input."),
			[this, Run, Name]() { Run([this, Name]() { return Model->AddSelectedActors(Name); }); })];
	}
	Add->AddSlot()[SmallButton(LOCTEXT("Content", "+ Content Browser"), LOCTEXT("ContentTip", "Uses the assets selected in the Content Browser. Assets from Scenario are reused without an upload."),
		[this, Run, Name]() { Run([this, Name]() { return Model->AddContentSelection(Name); }); })];
	Add->AddSlot()[SmallButton(LOCTEXT("File", "+ File"), LOCTEXT("FileTip", "Picks a file on disk and uploads it to your Scenario project."),
		[this, Run, Name, Kind]()
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
				Run([this, Name, Path]() { return Model->AddReferenceFile(Name, Path, FString()); });
			}
		})];

	const FText Limit = Field.Type == EScenarioFieldType::Files && Field.MaxItems > 0
		? FText::Format(LOCTEXT("Limit", "up to {0}"), FText::AsNumber(Field.MaxItems))
		: FText::GetEmpty();
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(STextBlock).Text(LabelWithCost(Field)).ToolTipText(FText::FromString(Field.Description))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(Limit).ColorAndOpacity(FScenarioStyle::Muted())
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			Chips
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Add
		];
}

TSharedRef<SWidget> SScenarioForm::BuildField(const FScenarioField& Field)
{
	return SNew(SBox)
		.WidthOverride(Field.Type == EScenarioFieldType::Text || Field.Type == EScenarioFieldType::Prompt || Field.Type == EScenarioFieldType::Multi ? 460.0f : 220.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 3.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock)
					.Text(LabelWithCost(Field))
					.ToolTipText(FText::FromString(Field.Description))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Visibility(Field.bCostImpact ? EVisibility::Visible : EVisibility::Collapsed)
					.Text(LOCTEXT("CostImpact", "CU"))
					.Font(FScenarioStyle::Font(7, true))
					.ColorAndOpacity(FScenarioStyle::Warning())
					.ToolTipText(LOCTEXT("CostImpactTip", "Changing this changes the price."))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildControl(Field)
			]
		];
}

TSharedRef<SWidget> SScenarioForm::BuildControl(const FScenarioField& Field)
{
	const FString Name = Field.Name;
	const TSharedPtr<FScenarioCreateModel> M = Model;
	switch (Field.Type)
	{
	case EScenarioFieldType::Enum:
	{
		const TArray<TSharedPtr<FJsonValue>> Options = Field.Options;
		return SNew(SComboButton)
			.ButtonContent()
			[
				SNew(STextBlock).Text_Lambda([M, Name]()
				{
					const TSharedPtr<FJsonValue> Value = M->GetValue(Name);
					return Value.IsValid() ? FText::FromString(ScenarioJson::ValueText(Value)) : LOCTEXT("Default", "Default");
				})
			]
			.OnGetMenuContent_Lambda([M, Name, Options]()
			{
				FMenuBuilder Menu(true, nullptr);
				for (const TSharedPtr<FJsonValue>& Option : Options)
				{
					Menu.AddMenuEntry(FText::FromString(ScenarioJson::ValueText(Option)), FText::GetEmpty(), FSlateIcon(),
						FUIAction(
							FExecuteAction::CreateLambda([M, Name, Option]() { M->SetValue(Name, Option); }),
							FCanExecuteAction(),
							FIsActionChecked::CreateLambda([M, Name, Option]() { return ScenarioJson::Canonical(M->GetValue(Name)) == ScenarioJson::Canonical(Option); })),
						NAME_None, EUserInterfaceActionType::RadioButton);
				}
				return Menu.MakeWidget();
			});
	}
	case EScenarioFieldType::Number:
	case EScenarioFieldType::Integer:
	{
		const bool bInteger = Field.Type == EScenarioFieldType::Integer;
		const TOptional<double> Min = Field.Min;
		const TOptional<double> Max = Field.Max;
		return SNew(SNumericEntryBox<double>)
			.AllowSpin(Min.IsSet() && Max.IsSet())
			.MinValue(Min)
			.MaxValue(Max)
			.MinSliderValue(Min)
			.MaxSliderValue(Max)
			.Delta(Field.Step.IsSet() ? Field.Step.GetValue() : (bInteger ? 1.0 : 0.0))
			.Value_Lambda([M, Name]() -> TOptional<double>
			{
				const TSharedPtr<FJsonValue> Value = M->GetValue(Name);
				double Number = 0.0;
				return Value.IsValid() && Value->TryGetNumber(Number) ? TOptional<double>(Number) : TOptional<double>();
			})
			.OnValueCommitted_Lambda([M, Name, bInteger](double Value, ETextCommit::Type)
			{
				M->SetValue(Name, MakeShared<FJsonValueNumber>(bInteger ? FMath::RoundToDouble(Value) : Value));
			})
			.OnValueChanged_Lambda([M, Name, bInteger](double Value)
			{
				M->SetValue(Name, MakeShared<FJsonValueNumber>(bInteger ? FMath::RoundToDouble(Value) : Value));
			});
	}
	case EScenarioFieldType::Boolean:
		return SNew(SCheckBox)
			.IsChecked_Lambda([M, Name]() { return ScenarioJson::Truthy(M->GetValue(Name)) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([M, Name](ECheckBoxState State) { M->SetValue(Name, MakeShared<FJsonValueBoolean>(State == ECheckBoxState::Checked)); });
	case EScenarioFieldType::Multi:
	{
		TSharedRef<SWrapBox> Box = SNew(SWrapBox).UseAllottedSize(true);
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
				SNew(SBox)
				.Padding(0.0f, 0.0f, 8.0f, 2.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([Has]() { return Has() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([M, Name, Item](ECheckBoxState State)
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
						if (State == ECheckBoxState::Checked)
						{
							Items.AddUnique(Item);
						}
						else
						{
							Items.Remove(Item);
						}
						M->SetValue(Name, ScenarioJson::StringArrayValue(Items));
					})
					[
						SNew(STextBlock).Text(FText::FromString(Item))
					]
				]
			];
		}
		return Box;
	}
	case EScenarioFieldType::Prompt:
		return SNew(SBox)
			.MinDesiredHeight(48.0f)
			[
				SNew(SMultiLineEditableTextBox)
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
			.HintText(FText::FromString(Field.Placeholder))
			.Text_Lambda([M, Name]() { return FText::FromString(ScenarioJson::ValueText(M->GetValue(Name))); })
			.OnTextChanged_Lambda([M, Name](const FText& Text)
			{
				M->SetValue(Name, Text.IsEmpty() ? TSharedPtr<FJsonValue>() : MakeShared<FJsonValueString>(Text.ToString()));
			});
	}
}

#undef LOCTEXT_NAMESPACE
