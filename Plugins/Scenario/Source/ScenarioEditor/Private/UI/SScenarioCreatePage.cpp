// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ContentBrowserModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IContentBrowserSingleton.h"
#include "ScenarioEditorModule.h"
#include "ScenarioCreateModel.h"
#include "ScenarioLanes.h"
#include "ScenarioSchema.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "SScenarioForm.h"
#include "SScenarioJobCard.h"
#include "SScenarioModelPicker.h"
#include "SScenarioPages.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioCreate"

namespace
{
	TSharedRef<SWidget> Tab(const FText& Label, const FText& Tip, TFunction<bool()> IsOn, TFunction<void()> OnClick)
	{
		return SNew(SBox)
			.Padding(0.0f, 0.0f, 4.0f, 4.0f)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.ToolTipText(Tip)
				.IsChecked_Lambda([IsOn]() { return IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([OnClick](ECheckBoxState) { OnClick(); })
				[
					SNew(SBox)
					.Padding(FMargin(10.0f, 3.0f))
					[
						SNew(STextBlock).Text(Label)
					]
				]
			];
	}

	/** Recent results of the person's own runs, newest first. */
	TArray<FString> RecentIds(int32 Max)
	{
		TArray<FString> Ids;
		FScenarioSession* Session = ScenarioUI::Session();
		if (!Session)
		{
			return Ids;
		}
		// The job list keeps the newest first.
		for (const FScenarioJobRow& Row : Session->Ledger().Rows())
		{
			if (Ids.Num() >= Max)
			{
				break;
			}
			if (Row.State != EScenarioJobState::Dismissed && Row.Kind != TEXT("prompt_spark") && Row.Source == TEXT("person"))
			{
				Ids.Add(Row.LocalId);
			}
		}
		return Ids;
	}
}

void SScenarioCreatePage::Construct(const FArguments& InArgs)
{
	Model = FScenarioCreateModel::Get();
	ModelHandle = Model->OnChanged.AddSP(this, &SScenarioCreatePage::OnModelChanged);
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		SessionHandle = Session->OnChanged.AddSP(this, &SScenarioCreatePage::RefreshTray);
	}
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildLanes()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(ModesBox, SBox)
				[
					BuildModes()
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 8.0f)
			[
				BuildModelCard()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(Form, SScenarioForm).Model(Model)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SCheckBox)
				.Visibility_Lambda([M]() { return M->IsUnrealReadyModel() ? EVisibility::Visible : EVisibility::Collapsed; })
				.IsChecked_Lambda([M]() { return M->IsUnrealReady() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([M](ECheckBoxState State) { M->SetUnrealReady(State == ECheckBoxState::Checked); })
				.ToolTipText_Lambda([]()
				{
					return FText::Format(LOCTEXT("UnrealReadyTip", "Adds these constraints to the prompt sent to Scenario: {0}"), FText::FromString(FScenarioLanes::Get().UnrealReadyPrompt()));
				})
				[
					SNew(STextBlock).Text(LOCTEXT("UnrealReady", "Unreal-ready (real-world scale, pivot at the base, clean topology)"))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				BuildDestination()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				ScenarioUI::GenerateButton(M, LOCTEXT("Shortcut", "Ctrl+Enter"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				// Where the eye is after a click: the request was sent, and how it is going.
				SNew(SHorizontalBox)
				.Visibility_Lambda([M]() { return M->GetSentJobText().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SThrobber)
					.NumPieces(3)
					.Visibility_Lambda([M]() { return M->IsSentJobRunning() ? EVisibility::Visible : EVisibility::Collapsed; })
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([M]() { return M->GetSentJobText(); })
					.AutoWrapText(true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SHyperlink)
					.Text(LOCTEXT("SentJobs", "Jobs"))
					.OnNavigate_Lambda([]() { FScenarioEditorModule::OpenStudio(ScenarioPages::Jobs); })
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([M]() { return FText::FromString(M->GetLastError()); })
				.ColorAndOpacity(FScenarioStyle::Error())
				.AutoWrapText(true)
				.Visibility_Lambda([M]() { return M->GetLastError().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 16.0f, 0.0f, 0.0f)
			[
				BuildTray()
			]
		]
	];
	RefreshTray();
	// The job list does not announce every new row: a sent job joins the tray within half a second.
	RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
	{
		RefreshTray();
		return EActiveTimerReturnType::Continue;
	}));
}

SScenarioCreatePage::~SScenarioCreatePage()
{
	if (Model.IsValid())
	{
		Model->OnChanged.Remove(ModelHandle);
	}
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		Session->OnChanged.Remove(SessionHandle);
	}
}

void SScenarioCreatePage::FocusPrompt()
{
	if (Form.IsValid())
	{
		Form->FocusPrompt();
	}
}

void SScenarioCreatePage::OpenPicker()
{
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();
	TArray<FString> Picks;
	for (const FScenarioLaneModel& Pick : M->QuickPicks())
	{
		Picks.Add(Pick.Id);
	}
	SScenarioModelPicker::Open(M->PickerModality(), FString(), [M](const FString& Id, const FString& Name) { M->SetModel(Id, Name); }, Picks, M->GetModelId());
}

void SScenarioCreatePage::OnModelChanged()
{
	const FString Key = Model->GetLaneId();
	if (Key != ModesFor && ModesBox.IsValid())
	{
		ModesBox->SetContent(BuildModes());
	}
}

TSharedRef<SWidget> SScenarioCreatePage::BuildLanes()
{
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();
	TSharedRef<SWrapBox> Box = SNew(SWrapBox).UseAllottedSize(true);
	for (const FScenarioLane& Lane : FScenarioLanes::Get().All())
	{
		const FString Id = Lane.Id;
		Box->AddSlot()[Tab(FText::FromString(Lane.Title), FText::FromString(Lane.Hint),
			[M, Id]() { return M->GetLaneId() == Id; },
			[M, Id]() { M->SetLane(Id); })];
	}
	Box->AddSlot()[Tab(LOCTEXT("All", "All"), LOCTEXT("AllTip", "Every Scenario model. Picking one switches to its lane."),
		[]() { return false; },
		[M]()
		{
			SScenarioModelPicker::Open(FString(), FString(), [M](const FString& Id, const FString& Name) { M->SetModel(Id, Name); }, {}, M->GetModelId());
		})];
	return Box;
}

TSharedRef<SWidget> SScenarioCreatePage::BuildModes()
{
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();
	ModesFor = M->GetLaneId();
	const FScenarioLane* Lane = M->GetLane();
	if (!Lane || Lane->Modes.Num() == 0)
	{
		return SNullWidget::NullWidget;
	}
	TSharedRef<SWrapBox> Box = SNew(SWrapBox).UseAllottedSize(true);
	Box->AddSlot()
	[
		SNew(SBox)
		.Padding(0.0f, 0.0f, 8.0f, 4.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("From", "From")).ColorAndOpacity(FScenarioStyle::Muted())
		]
	];
	for (const FString& Mode : Lane->Modes)
	{
		const FString Id = Mode;
		const FText Label = Mode == TEXT("hdri") ? LOCTEXT("HDRI", "HDRI") : FText::FromString(ScenarioSchema::Humanize(Mode));
		Box->AddSlot()[Tab(Label, FText::GetEmpty(), [M, Id]() { return M->GetMode() == Id; }, [M, Id]() { M->SetMode(Id); })];
	}
	return Box;
}

TSharedRef<SWidget> SScenarioCreatePage::BuildModelCard()
{
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.Padding(10.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[
				// The provider name ("OpenAI", "ByteDance", "Trained") sizes the badge, so it is never cut.
				SNew(SBox)
				.MinDesiredWidth(40.0f)
				.HeightOverride(40.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.Padding(FMargin(8.0f, 0.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([M]() { return FText::FromString(M->GetModelBadge()); })
						.Font(FScenarioStyle::Font(9, true))
					]
				]
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
					.Text_Lambda([M]() { return M->GetModelId().IsEmpty() ? LOCTEXT("NoModel", "No model chosen") : FText::FromString(M->GetModelName()); })
					.Font(FScenarioStyle::Font(11, true))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text_Lambda([M]() { return FText::FromString(M->GetModelNote()); })
					.ColorAndOpacity(FScenarioStyle::Muted())
					.AutoWrapText(true)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SComboButton)
				.ToolTipText(LOCTEXT("QuickTip", "The lane's recommended models."))
				.ButtonContent()
				[
					SNew(STextBlock).Text(LOCTEXT("Picks", "Picks"))
				]
				.OnGetMenuContent_Lambda([M]()
				{
					FMenuBuilder Menu(true, nullptr);
					for (const FScenarioLaneModel& Pick : M->QuickPicks())
					{
						const FString Id = Pick.Id;
						const FString Name = Pick.Name;
						Menu.AddMenuEntry(FText::FromString(Pick.Name), FText::FromString(Pick.Note), FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([M, Id, Name]() { M->SetModel(Id, Name); }),
								FCanExecuteAction(),
								FIsActionChecked::CreateLambda([M, Id]() { return M->GetModelId() == Id; })),
							NAME_None, EUserInterfaceActionType::RadioButton);
					}
					return Menu.MakeWidget();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Change", "Change"))
				.ToolTipText(LOCTEXT("ChangeTip", "Opens the model picker: every Scenario model and your team's trained ones (Ctrl+M)."))
				.OnClicked_Lambda([this]() { OpenPicker(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Suggest", "Suggest"))
				.ToolTipText(LOCTEXT("SuggestTip", "Asks Scenario which models fit this prompt best. Free."))
				.OnClicked_Lambda([this, M]()
				{
					const TWeakPtr<SWidget> Weak = AsShared();
					M->Suggest([Weak, M](const TArray<TPair<FString, FString>>& Models, const FString& Error)
					{
						const TSharedPtr<SWidget> Anchor = Weak.Pin();
						if (!Anchor.IsValid())
						{
							return;
						}
						FMenuBuilder Menu(true, nullptr);
						if (Models.Num() == 0)
						{
							Menu.AddMenuEntry(FText::FromString(Error), FText::GetEmpty(), FSlateIcon(), FUIAction());
						}
						for (const TPair<FString, FString>& Entry : Models)
						{
							const FString Id = Entry.Key;
							const FString Name = Entry.Value;
							Menu.AddMenuEntry(FText::FromString(Name), FText::FromString(Id), FSlateIcon(),
								FUIAction(FExecuteAction::CreateLambda([M, Id, Name]() { M->SetModel(Id, Name); })));
						}
						FSlateApplication::Get().PushMenu(Anchor.ToSharedRef(), FWidgetPath(), Menu.MakeWidget(),
							FSlateApplication::Get().GetCursorPos(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
					});
					return FReply::Handled();
				})
			]
		];
}

TSharedRef<SWidget> SScenarioCreatePage::BuildDestination()
{
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text_Lambda([M]()
			{
				return FText::Format(LOCTEXT("SavesTo", "Saves to {0}. {1}"), FText::FromString(M->DestinationText()), FText::FromString(M->PlacementText()));
			})
			.ColorAndOpacity(FScenarioStyle::Muted())
			.AutoWrapText(true)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SComboButton)
			.ToolTipText(LOCTEXT("FolderTip", "Chooses the Content Browser folder for this lane's results."))
			.ButtonContent()
			[
				SNew(STextBlock).Text(LOCTEXT("Folder", "Folder"))
			]
			.OnGetMenuContent_Lambda([M]()
			{
				FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
				FPathPickerConfig Config;
				Config.DefaultPath = M->DestinationText();
				Config.bAddDefaultPath = false;
				Config.bAllowContextMenu = true;
				Config.OnPathSelected = FOnPathSelected::CreateLambda([M](const FString& Path)
				{
					M->Destination = Path;
					FSlateApplication::Get().DismissAllMenus();
				});
				FMenuBuilder Menu(true, nullptr);
				Menu.AddMenuEntry(LOCTEXT("Default", "Use the default folder"), LOCTEXT("DefaultTip", "Project Settings > Plugins > Scenario decides the folder."), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([M]() { M->Destination.Reset(); })));
				Menu.AddWidget(
					SNew(SBox)
					.WidthOverride(320.0f)
					.HeightOverride(360.0f)
					[
						ContentBrowser.Get().CreatePathPicker(Config)
					], FText::GetEmpty(), true);
				return Menu.MakeWidget();
			})
		];
}

TSharedRef<SWidget> SScenarioCreatePage::BuildTray()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Results", "Results"))
			.Font(FScenarioStyle::Font(10, true))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Empty", "Nothing generated yet. Type what you need and press Generate."))
			.ColorAndOpacity(FScenarioStyle::Muted())
			.Visibility_Lambda([this]() { return TrayIds.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed; })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(Tray, SVerticalBox)
		];
}

void SScenarioCreatePage::RefreshTray()
{
	const TArray<FString> Ids = RecentIds(8);
	if (Ids == TrayIds || !Tray.IsValid())
	{
		return;
	}
	TrayIds = Ids;
	Tray->ClearChildren();
	for (const FString& Id : Ids)
	{
		Tray->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SNew(SScenarioJobCard).LocalId(Id)
		];
	}
}

#undef LOCTEXT_NAMESPACE
