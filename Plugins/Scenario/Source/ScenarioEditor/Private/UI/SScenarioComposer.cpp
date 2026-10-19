// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioComposer.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "ScenarioCreateModel.h"
#include "ScenarioEditorModule.h"
#include "ScenarioLanes.h"
#include "ScenarioSession.h"
#include "ScenarioSettings.h"
#include "ScenarioStyle.h"
#include "ScenarioWidgets.h"
#include "SScenarioJobCard.h"
#include "SScenarioModelPicker.h"
#include "SScenarioPages.h"
#include "SSimpleComboButton.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioComposer"

namespace
{
	TSharedPtr<SWidget> Overlay;
	TWeakPtr<IAssetViewport> OverlayViewport;
	TWeakPtr<SScenarioComposer> ComposerWidget;

	void OpenPicker(const TSharedRef<FScenarioCreateModel>& M)
	{
		TArray<FString> Picks;
		for (const FScenarioLaneModel& Pick : M->QuickPicks())
		{
			Picks.Add(Pick.Id);
		}
		SScenarioModelPicker::Open(M->PickerModality(), FString(), [M](const FString& Id, const FString& Name) { M->SetModel(Id, Name); }, Picks, M->GetModelId());
	}

	/** The lane's recommended models, then every model. */
	TSharedRef<SWidget> ModelMenu(const TSharedRef<FScenarioCreateModel>& M)
	{
		FMenuBuilder Menu(true, nullptr);
		const FScenarioLane* Lane = M->GetLane();
		Menu.BeginSection(NAME_None, FText::Format(LOCTEXT("Recommended", "Recommended for {0}"), FText::FromString(Lane ? Lane->Title : FString())));
		for (const FScenarioLaneModel& Pick : M->QuickPicks())
		{
			const FString Id = Pick.Id;
			const FString Name = Pick.Name;
			FString Note = Pick.Note;
			if (!Note.IsEmpty())
			{
				Note[0] = FChar::ToUpper(Note[0]);
			}
			Menu.AddMenuEntry(FText::FromString(Name), FText::FromString(Note), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([M, Id, Name]() { M->SetModel(Id, Name); }),
					FCanExecuteAction(), FIsActionChecked::CreateLambda([M, Id]() { return M->GetModelId() == Id; })),
				NAME_None, EUserInterfaceActionType::RadioButton);
		}
		Menu.EndSection();
		Menu.AddSeparator();
		Menu.AddMenuEntry(LOCTEXT("Browse", "Browse all models..."), LOCTEXT("BrowseTip", "Every Scenario model and your team's trained ones."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"), FUIAction(FExecuteAction::CreateLambda([M]() { OpenPicker(M); })));
		return Menu.MakeWidget();
	}

	FSlateColor HintColor(EScenarioHintTone Tone)
	{
		switch (Tone)
		{
		case EScenarioHintTone::Error: return ScenarioUI::Palette::Error();
		case EScenarioHintTone::Warning: return ScenarioUI::Palette::Warning();
		case EScenarioHintTone::Success: return ScenarioUI::Palette::Success();
		default: return FSlateColor(ScenarioUI::Palette::Dim());
		}
	}
}

void SScenarioComposer::Show(bool bVisible)
{
	if (const TSharedPtr<IAssetViewport> Viewport = OverlayViewport.Pin())
	{
		if (Overlay.IsValid())
		{
			Viewport->RemoveOverlayWidget(Overlay.ToSharedRef());
		}
	}
	Overlay.Reset();
	OverlayViewport.Reset();
	if (!bVisible)
	{
		return;
	}
	FLevelEditorModule& LevelEditor = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
	const TSharedPtr<IAssetViewport> Viewport = LevelEditor.GetFirstActiveViewport();
	if (!Viewport.IsValid())
	{
		return;
	}
	TSharedPtr<SScenarioComposer> Composer;
	Overlay = SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(16.0f, 0.0f, 16.0f, 28.0f))
		[
			SNew(SBox)
			.MaxDesiredWidth(820.0f)
			[
				SAssignNew(Composer, SScenarioComposer)
			]
		];
	Viewport->AddOverlayWidget(Overlay.ToSharedRef());
	OverlayViewport = Viewport;
	Composer->FocusPrompt();
}

bool SScenarioComposer::IsShown()
{
	return Overlay.IsValid() && OverlayViewport.IsValid();
}

void SScenarioComposer::Construct(const FArguments& InArgs)
{
	ComposerWidget = SharedThis(this);
	Model = FScenarioCreateModel::Get();
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		SessionHandle = Session->OnChanged.AddSP(this, &SScenarioComposer::RefreshLatest);
	}

	// The Create lanes, as in the Studio.
	TSharedRef<SSegmentedControl<FString>> Lanes = SNew(SSegmentedControl<FString>)
		.UniformPadding(FMargin(8.0f, 2.0f))
		.Value_Lambda([M]() { return M->GetLaneKey(); })
		.OnValueChanged_Lambda([M](const FString& Lane) { M->SetLane(Lane); });
	for (const FScenarioLane& Lane : FScenarioLanes::Get().All())
	{
		Lanes->AddSlot(Lane.Id, false)
			.Text(FText::FromString(Lane.Title))
			.ToolTip(FText::FromString(Lane.Hint));
	}
	Lanes->RebuildChildren();

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Menu.Background"))
		.Padding(ScenarioUI::SectionGap)
		[
			SNew(SVerticalBox)
			// Lane, model, Studio, close.
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, ScenarioUI::Gutter, 0.0f)
				[
					SNew(SImage).Image(FScenarioStyle::Get().GetBrush("Scenario.Mark.Small"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					Lanes
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Left)
				.Padding(ScenarioUI::Gutter, 0.0f)
				[
					SNew(SSimpleComboButton)
					.Text_Lambda([M]() { const FString Name = M->GetModelName(); return Name.IsEmpty() ? LOCTEXT("PickModel", "Pick a model") : FText::FromString(Name); })
					.HasDownArrow(true)
					.ToolTipText(LOCTEXT("ModelTip", "Changes the model: the lane's recommended ones, or every model. The Studio uses the same form."))
					.OnGetMenuContent_Lambda([M]() { return ModelMenu(M); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenStudio", "Open in Studio"))
					.ToolTipText(LOCTEXT("OpenStudioTip", "Opens the full form in the Studio: references, settings, destination."))
					.OnClicked_Lambda([]() { FScenarioEditorModule::OpenStudio(ScenarioPages::Create); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(ScenarioUI::RowGap, 0.0f, 0.0f, 0.0f)
				[
					ScenarioUI::IconButton(TEXT("Icons.X"), LOCTEXT("CloseTip", "Hide the composer (Alt+Shift+Space brings it back)."),
						FSimpleDelegate::CreateLambda([]() { FScenarioEditorModule::ToggleComposer(); }))
				]
			]
			// Prompt and Generate.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, ScenarioUI::Gutter, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, ScenarioUI::Gutter, 0.0f)
				[
					SAssignNew(Prompt, SEditableTextBox)
					.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
					.HintText_Lambda([M]()
					{
						const FScenarioLane* Lane = M->GetLane();
						return Lane && Lane->StarterPrompts.Num() > 0 ? FText::FromString(Lane->StarterPrompts[0]) : LOCTEXT("Hint", "Describe what you need");
					})
					.Text_Lambda([M]() { return FText::FromString(M->GetPrompt()); })
					.OnTextChanged_Lambda([M](const FText& Text) { M->SetPrompt(Text.ToString()); })
					.OnTextCommitted_Lambda([M](const FText&, ETextCommit::Type Commit)
					{
						if (Commit == ETextCommit::OnEnter && M->CanClickGenerate())
						{
							M->Generate();
						}
					})
					.OnKeyDownHandler_Lambda([](const FGeometry&, const FKeyEvent& Key)
					{
						if (Key.GetKey() == EKeys::Escape)
						{
							FSlateApplication::Get().ClearKeyboardFocus(EFocusCause::Cleared);
							return FReply::Handled();
						}
						return FReply::Unhandled();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SBox)
					.HeightOverride(28.0f)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
						.VAlign(VAlign_Center)
						.IsEnabled_Lambda([M]() { return M->CanClickGenerate(); })
						.ToolTipText_Lambda([M]() { return M->GetButtonTooltip(); })
						.OnClicked_Lambda([M]() { M->Generate(); return FReply::Handled(); })
						[
							SNew(STextBlock)
							.Text_Lambda([M]() { return M->GetButtonText(); })
							.Font(ScenarioUI::Font(ScenarioUI::EText::Title))
							.ColorAndOpacity_Lambda([M]() { return M->IsPriceStale() ? FLinearColor(1.0f, 1.0f, 1.0f, 0.55f) : FLinearColor::White; })
						]
					]
				]
			]
			// Why Generate is disabled, or how the last request is going.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, ScenarioUI::RowGap, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([M]() { return M->GetHint().Text; })
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				.ColorAndOpacity_Lambda([M]() { return HintColor(M->GetHint().Tone); })
				.AutoWrapText(true)
				.Visibility_Lambda([M]() { return M->GetHint().Text.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, ScenarioUI::Gutter, 0.0f, 0.0f)
			[
				SAssignNew(Latest, SBox)
			]
		]
	];
	RefreshLatest();
}

SScenarioComposer::~SScenarioComposer()
{
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		Session->OnChanged.Remove(SessionHandle);
	}
}

void SScenarioComposer::FocusPrompt()
{
	if (Prompt.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(Prompt, EFocusCause::SetDirectly);
	}
}

void SScenarioComposer::RefreshLatest()
{
	FString Newest;
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		for (const FScenarioJobRow& Row : Session->Ledger().Rows())
		{
			if (Row.Source == TEXT("person") && Row.Kind != TEXT("prompt_spark") && Row.State != EScenarioJobState::Dismissed)
			{
				Newest = Row.LocalId;
				break;
			}
		}
	}
	if (Newest == LatestId || !Latest.IsValid())
	{
		return;
	}
	LatestId = Newest;
	Latest->SetContent(Newest.IsEmpty() ? SNullWidget::NullWidget : StaticCastSharedRef<SWidget>(SNew(SScenarioJobCard).LocalId(Newest)));
}

#undef LOCTEXT_NAMESPACE
