// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioComposer.h"

#include "Framework/Application/SlateApplication.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "ScenarioCreateModel.h"
#include "ScenarioEditorModule.h"
#include "ScenarioLanes.h"
#include "ScenarioSession.h"
#include "ScenarioSettings.h"
#include "ScenarioStyle.h"
#include "SScenarioJobCard.h"
#include "SScenarioModelPicker.h"
#include "SScenarioPages.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
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

	/** The composer's lanes, in the spec's order; Sky is the Environment lane. */
	const TArray<TPair<FString, FText>>& ComposerLanes()
	{
		static const TArray<TPair<FString, FText>> Lanes = {
			{ TEXT("image"), LOCTEXT("Image", "Image") },
			{ TEXT("3d"), LOCTEXT("3D", "3D") },
			{ TEXT("material"), LOCTEXT("Material", "Material") },
			{ TEXT("audio"), LOCTEXT("Audio", "Audio") },
			{ TEXT("environment"), LOCTEXT("Sky", "Sky") },
			{ TEXT("render"), LOCTEXT("Render", "Render") },
		};
		return Lanes;
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

	TSharedRef<SHorizontalBox> Lanes = SNew(SHorizontalBox);
	for (const TPair<FString, FText>& Entry : ComposerLanes())
	{
		const FString Id = Entry.Key;
		Lanes->AddSlot().AutoWidth().Padding(0.0f, 0.0f, 2.0f, 0.0f)
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
			.IsChecked_Lambda([M, Id]() { return M->GetLaneId() == Id ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([M, Id](ECheckBoxState) { M->SetLane(Id); })
			[
				SNew(SBox).Padding(FMargin(6.0f, 1.0f))[SNew(STextBlock).Text(Entry.Value)]
			]
		];
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Menu.Background"))
		.Padding(10.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
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
				.Padding(8.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(LOCTEXT("ModelTip", "Changes the model (same as the Studio)."))
					.OnClicked_Lambda([M]()
					{
						TArray<FString> Picks;
						for (const FScenarioLaneModel& Pick : M->QuickPicks())
						{
							Picks.Add(Pick.Id);
						}
						SScenarioModelPicker::Open(M->PickerModality(), FString(), [M](const FString& Id, const FString& Name) { M->SetModel(Id, Name); }, Picks, M->GetModelId());
						return FReply::Handled();
					})
					[
						SNew(STextBlock)
						.Text_Lambda([M]() { return FText::Format(LOCTEXT("Model", "{0} ▾"), FText::FromString(M->GetModelName())); })
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("Settings", "Settings"))
					.ToolTipText(LOCTEXT("SettingsTip", "Opens the full form in the Studio: references, settings, destination."))
					.OnClicked_Lambda([]() { FScenarioEditorModule::OpenStudio(ScenarioPages::Create); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(LOCTEXT("CloseTip", "Hides the composer (Alt+Shift+Space brings it back)."))
					.OnClicked_Lambda([]() { FScenarioEditorModule::ToggleComposer(); return FReply::Handled(); })
					[
						SNew(STextBlock).Text(FText::FromString(TEXT("×")))
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SAssignNew(Prompt, SEditableTextBox)
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
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.IsEnabled_Lambda([M]() { return M->CanClickGenerate(); })
					.ToolTipText_Lambda([M]() { return M->GetButtonTooltip(); })
					.OnClicked_Lambda([M]() { M->Generate(); return FReply::Handled(); })
					[
						SNew(STextBlock).Text_Lambda([M]() { return M->GetButtonText(); })
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([M]() { return FText::FromString(M->GetLastError()); })
				.ColorAndOpacity(FScenarioStyle::Error())
				.Visibility_Lambda([M]() { return M->GetLastError().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
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
