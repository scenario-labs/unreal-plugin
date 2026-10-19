// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ContentBrowserModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IContentBrowserSingleton.h"
#include "ScenarioCatalog.h"
#include "ScenarioCreateModel.h"
#include "ScenarioEditorModule.h"
#include "ScenarioLanes.h"
#include "ScenarioSchema.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "ScenarioThumbnails.h"
#include "ScenarioWidgets.h"
#include "SScenarioForm.h"
#include "SScenarioJobCard.h"
#include "SScenarioModelPicker.h"
#include "SScenarioPages.h"
#include "SSimpleComboButton.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioCreate"

namespace
{
	constexpr int32 RecentCount = 6;
	const FString WorkflowLane = TEXT("workflow");

	/** The newest jobs from any lane, by anyone (person, agent or script). */
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
			if (Row.State != EScenarioJobState::Dismissed && Row.Kind != TEXT("prompt_spark"))
			{
				Ids.Add(Row.LocalId);
			}
		}
		return Ids;
	}

	FText ModeLabel(const FString& Lane, const FString& Mode)
	{
		if (Mode == TEXT("edit"))
		{
			return LOCTEXT("ModeMesh", "Mesh");
		}
		if (Mode == TEXT("hdri"))
		{
			return LOCTEXT("ModeHdri", "HDRI lighting");
		}
		if (Mode == TEXT("generate"))
		{
			return LOCTEXT("ModeSky", "360 sky");
		}
		return FText::FromString(ScenarioSchema::Humanize(Mode));
	}

	FText ModeTip(const FString& Mode)
	{
		if (Mode == TEXT("text"))
		{
			return LOCTEXT("ModeTextTip", "A mesh from a prompt.");
		}
		if (Mode == TEXT("image"))
		{
			return LOCTEXT("ModeImageTip", "A mesh from one or more images.");
		}
		if (Mode == TEXT("edit"))
		{
			return LOCTEXT("ModeMeshTip", "Edit, rig, retexture or remesh a mesh you already have.");
		}
		if (Mode == TEXT("hdri"))
		{
			return LOCTEXT("ModeHdriTip", "An HDR panorama that lights the level.");
		}
		if (Mode == TEXT("generate"))
		{
			return LOCTEXT("ModeSkyTip", "A 360 panorama for the level's sky.");
		}
		return FText::GetEmpty();
	}

	FSlateColor ToneColor(EScenarioHintTone Tone)
	{
		switch (Tone)
		{
		case EScenarioHintTone::Error: return FStyleColors::Error;
		case EScenarioHintTone::Warning: return FStyleColors::Warning;
		case EScenarioHintTone::Success: return FSlateColor::UseForeground();
		default: return FSlateColor::UseSubduedForeground();
		}
	}

	const FSlateBrush* ToneIcon(EScenarioHintTone Tone)
	{
		switch (Tone)
		{
		case EScenarioHintTone::Error: return FAppStyle::GetBrush("Icons.ErrorWithColor");
		case EScenarioHintTone::Warning: return FAppStyle::GetBrush("Icons.WarningWithColor");
		case EScenarioHintTone::Success: return FAppStyle::GetBrush("Icons.SuccessWithColor");
		default: return nullptr;
		}
	}

	FText ActionLabel(EScenarioHintAction Action)
	{
		switch (Action)
		{
		case EScenarioHintAction::ChangeModel: return LOCTEXT("ActChange", "Change model");
		case EScenarioHintAction::OpenJobs: return LOCTEXT("ActJobs", "Check in Jobs");
		case EScenarioHintAction::CheckNow: return LOCTEXT("ActCheck", "Check now");
		case EScenarioHintAction::ImportAndPlace: return LOCTEXT("ActPlace", "Import and place");
		case EScenarioHintAction::ImportOnly: return LOCTEXT("ActImport", "Import only");
		case EScenarioHintAction::Show: return LOCTEXT("ActShow", "Show");
		case EScenarioHintAction::Details: return LOCTEXT("ActDetails", "Details");
		default: return FText::GetEmpty();
		}
	}
}

void SScenarioCreatePage::Construct(const FArguments& InArgs)
{
	Model = FScenarioCreateModel::Get();
	ModelHandle = Model->OnChanged.AddSP(this, &SScenarioCreatePage::OnModelChanged);
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		SessionHandle = Session->OnChanged.AddSP(this, &SScenarioCreatePage::RefreshRecent);
	}
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();

	ChildSlot
	[
		SNew(SVerticalBox)
		// What to make and how: scrolls.
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			.Padding(ScenarioUI::Gutter)
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
				.Padding(0.0f, ScenarioUI::SectionGap, 0.0f, ScenarioUI::SectionGap)
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
				.Padding(0.0f, ScenarioUI::RowGap, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda([M]() { return M->IsUnrealReadyModel() ? EVisibility::Visible : EVisibility::Collapsed; })
					.ToolTipText_Lambda([]()
					{
						return FText::Format(LOCTEXT("UnrealReadyTip", "Adds these constraints to the prompt sent to Scenario: {0}"), FText::FromString(FScenarioLanes::Get().UnrealReadyPrompt()));
					})
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SBox)
						.WidthOverride(ScenarioUI::LabelWidth)
						[
							SNew(STextBlock).Text(LOCTEXT("UnrealReady", "Unreal-ready")).Font(ScenarioUI::Font(ScenarioUI::EText::Body))
						]
					]
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
						ScenarioUI::Line(LOCTEXT("UnrealReadyNote", "real-world scale, pivot at the base, clean topology"), ScenarioUI::EText::Meta, true)
					]
				]
			]
		]
		// What came back: fixed.
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildRecent()
		]
		// The spend: fixed, always in view.
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildActionBar()
		]
	];
	RefreshRecent();
	// The job list does not announce every new row: a sent job joins Recent within half a second.
	RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
	{
		RefreshRecent();
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
	if (M->IsWorkflow())
	{
		SScenarioModelPicker::OpenWorkflows([M](const FScenarioWorkflowItem& Item) { M->SetWorkflow(Item.Id, Item.Name, Item.Description); }, M->GetModelId());
		return;
	}
	TArray<FString> Picks;
	for (const FScenarioLaneModel& Pick : M->QuickPicks())
	{
		Picks.Add(Pick.Id);
	}
	SScenarioModelPicker::Open(M->PickerModality(), FString(), [M](const FString& Id, const FString& Name) { M->SetModel(Id, Name); }, Picks, M->GetModelId());
}

void SScenarioCreatePage::OnModelChanged()
{
	const FString Key = Model->GetLaneKey() + TEXT("|") + Model->GetMode();
	if (Key != ModesFor && ModesBox.IsValid())
	{
		ModesBox->SetContent(BuildModes());
	}
}

TSharedRef<SWidget> SScenarioCreatePage::BuildLanes()
{
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();
	TSharedRef<SSegmentedControl<FString>> Lanes = SNew(SSegmentedControl<FString>)
		.UniformPadding(FMargin(8.0f, 2.0f))
		.Value_Lambda([M]() { return M->GetLaneKey(); })
		.OnValueChanged_Lambda([M](const FString& Lane)
		{
			if (Lane == WorkflowLane)
			{
				M->EnterWorkflowLane();
			}
			else
			{
				M->SetLane(Lane);
			}
		});
	for (const FScenarioLane& Lane : FScenarioLanes::Get().All())
	{
		Lanes->AddSlot(Lane.Id, false)
			.Text(FText::FromString(Lane.Title))
			.ToolTip(FText::FromString(Lane.Hint));
	}
	Lanes->AddSlot(WorkflowLane, false)
		.Text(LOCTEXT("Workflow", "Workflow"))
		.ToolTip(LOCTEXT("WorkflowTip", "Runs a Scenario workflow: several models in one request, built on scenario.com."));
	Lanes->RebuildChildren();
	return SNew(SBox)
		.HAlign(HAlign_Left)
		[
			Lanes
		];
}

TSharedRef<SWidget> SScenarioCreatePage::BuildModes()
{
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();
	ModesFor = M->GetLaneKey() + TEXT("|") + M->GetMode();
	const FScenarioLane* Lane = M->GetLane();
	if (M->IsWorkflow() || !Lane || Lane->Modes.Num() == 0)
	{
		return SNullWidget::NullWidget;
	}
	TSharedRef<SSegmentedControl<FString>> Modes = SNew(SSegmentedControl<FString>)
		.UniformPadding(FMargin(8.0f, 1.0f))
		.Value_Lambda([M]() { return M->GetMode(); })
		.OnValueChanged_Lambda([M](const FString& Mode) { M->SetMode(Mode); });
	for (const FString& Mode : Lane->Modes)
	{
		Modes->AddSlot(Mode, false).Text(ModeLabel(Lane->Id, Mode)).ToolTip(ModeTip(Mode));
	}
	Modes->RebuildChildren();
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, ScenarioUI::RowGap * 2.0f, ScenarioUI::Gutter, 0.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("From", "From"))
			.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, ScenarioUI::RowGap * 2.0f, 0.0f, 0.0f)
		[
			Modes
		];
}

TSharedRef<SWidget> SScenarioCreatePage::BuildModelCard()
{
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();
	auto Thumbnail = [M]() -> const FSlateBrush*
	{
		FScenarioSession* Session = ScenarioUI::Session();
		const FScenarioCatalogModel* Found = Session && !M->IsWorkflow() ? Session->Catalog().Find(M->GetModelId()) : nullptr;
		return Found && !Found->Summary.ThumbnailUrl.IsEmpty() ? FScenarioThumbnails::Get().ForUrl(TEXT("model:") + Found->Summary.Id, Found->Summary.ThumbnailUrl) : nullptr;
	};
	auto Kind = [M]()
	{
		if (M->IsWorkflow())
		{
			return FString(TEXT("image"));
		}
		const FString Lane = M->GetLaneId();
		return Lane == TEXT("environment") ? FString(TEXT("sky")) : Lane == TEXT("render") ? FString(TEXT("image")) : Lane;
	};

	ModelCard = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, ScenarioUI::Gutter, 0.0f)
		[
			ScenarioUI::Thumbnail(40.0f, MakeAttributeLambda(Thumbnail), MakeAttributeLambda(Kind))
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					ScenarioUI::Line(MakeAttributeLambda([M]()
					{
						const FString Name = M->GetModelName();
						if (!Name.IsEmpty())
						{
							return FText::FromString(Name);
						}
						return M->IsWorkflow() ? LOCTEXT("PickWorkflow", "Pick a workflow") : LOCTEXT("PickModel", "Pick a model");
					}), ScenarioUI::EText::Title)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.Visibility_Lambda([M]() { return M->IsWorkflow() && !M->GetModelId().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						ScenarioUI::Chip(MakeAttributeLambda([M]()
						{
							const FString Id = M->GetModelId();
							const FScenarioWorkflowItem* Item = M->GetWorkflows().FindByPredicate([&Id](const FScenarioWorkflowItem& W) { return W.Id == Id; });
							return Item && !Item->bTeam ? LOCTEXT("Featured", "Featured") : LOCTEXT("Team", "Team");
						}), FStyleColors::AccentGray)
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				ScenarioUI::Line(MakeAttributeLambda([M]()
				{
					FString Note = M->GetModelNote();
					if (!Note.IsEmpty())
					{
						Note[0] = FChar::ToUpper(Note[0]);
					}
					return FText::FromString(Note);
				}), ScenarioUI::EText::Meta, true)
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
		[
			SNew(SComboButton)
			.ToolTipText_Lambda([M]()
			{
				return M->IsWorkflow()
					? LOCTEXT("ChangeWorkflowTip", "Picks another workflow from your team or Scenario's featured ones.")
					: LOCTEXT("ChangeModelTip", "Recommended models for this lane, Scenario's suggestion for your prompt, or every model (Ctrl+M).");
			})
			.OnGetMenuContent(this, &SScenarioCreatePage::BuildModelMenu)
			.ButtonContent()
			[
				SNew(STextBlock)
				.Text_Lambda([M]() { return M->IsWorkflow() ? LOCTEXT("ChangeWorkflow", "Change workflow") : LOCTEXT("ChangeModel", "Change model"); })
			]
		];
	return ModelCard.ToSharedRef();
}

TSharedRef<SWidget> SScenarioCreatePage::BuildModelMenu()
{
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();
	FMenuBuilder Menu(true, nullptr);
	if (M->IsWorkflow())
	{
		M->LoadWorkflows();
		auto Section = [&Menu, M](bool bTeam, const FText& Title)
		{
			Menu.BeginSection(NAME_None, Title);
			int32 Count = 0;
			for (const FScenarioWorkflowItem& Item : M->GetWorkflows())
			{
				if (Item.bTeam != bTeam || Count >= 12)
				{
					continue;
				}
				++Count;
				const FScenarioWorkflowItem Copy = Item;
				Menu.AddMenuEntry(FText::FromString(Item.Name), FText::FromString(Item.Description.Left(200)), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([M, Copy]() { M->SetWorkflow(Copy.Id, Copy.Name, Copy.Description); }),
						FCanExecuteAction(), FIsActionChecked::CreateLambda([M, Copy]() { return M->GetModelId() == Copy.Id; })),
					NAME_None, EUserInterfaceActionType::RadioButton);
			}
			if (Count == 0)
			{
				Menu.AddMenuEntry(M->IsLoadingWorkflows() ? LOCTEXT("WorkflowsLoading", "Loading workflows...") : LOCTEXT("NoWorkflows", "None yet"),
					FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
			}
			Menu.EndSection();
		};
		Section(true, LOCTEXT("TeamWorkflows", "Your team's workflows"));
		Section(false, LOCTEXT("FeaturedWorkflows", "Featured by Scenario"));
		Menu.AddSeparator();
		Menu.AddMenuEntry(LOCTEXT("BrowseWorkflows", "Browse all workflows..."), LOCTEXT("BrowseWorkflowsTip", "Search every workflow with its description (Ctrl+M)."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"), FUIAction(FExecuteAction::CreateSP(this, &SScenarioCreatePage::OpenPicker)));
		return Menu.MakeWidget();
	}

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
		Menu.AddMenuEntry(FText::FromString(Pick.Name), FText::FromString(Note), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([M, Id, Name]() { M->SetModel(Id, Name); }),
				FCanExecuteAction(), FIsActionChecked::CreateLambda([M, Id]() { return M->GetModelId() == Id; })),
			NAME_None, EUserInterfaceActionType::RadioButton);
	}
	Menu.EndSection();
	Menu.BeginSection(NAME_None, LOCTEXT("More", "More"));
	Menu.AddMenuEntry(LOCTEXT("Suggest", "Suggest for my prompt"), LOCTEXT("SuggestTip", "Asks Scenario which models fit this prompt best. Free."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Star"), FUIAction(FExecuteAction::CreateSP(this, &SScenarioCreatePage::ShowSuggestions)));
	Menu.AddMenuEntry(LOCTEXT("Browse", "Browse all models..."), LOCTEXT("BrowseTip", "Every Scenario model and your team's trained ones (Ctrl+M)."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"), FUIAction(FExecuteAction::CreateSP(this, &SScenarioCreatePage::OpenPicker)));
	Menu.EndSection();
	return Menu.MakeWidget();
}

void SScenarioCreatePage::ShowSuggestions()
{
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();
	const TWeakPtr<SWidget> WeakCard = ModelCard;
	M->Suggest([WeakCard, M](const TArray<TPair<FString, FString>>& Models, const FString& Error)
	{
		const TSharedPtr<SWidget> Anchor = WeakCard.Pin();
		if (!Anchor.IsValid())
		{
			return;
		}
		FMenuBuilder Menu(true, nullptr);
		Menu.BeginSection(NAME_None, LOCTEXT("Suggested", "Suggested for your prompt"));
		if (Models.Num() == 0)
		{
			Menu.AddMenuEntry(Error.IsEmpty() ? LOCTEXT("NoSuggestion", "No suggestion for this prompt") : FText::FromString(Error), FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
		}
		for (const TPair<FString, FString>& Entry : Models)
		{
			const FString Id = Entry.Key;
			const FString Name = Entry.Value;
			Menu.AddMenuEntry(FText::FromString(Name), FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([M, Id, Name]() { M->SetModel(Id, Name); }),
					FCanExecuteAction(), FIsActionChecked::CreateLambda([M, Id]() { return M->GetModelId() == Id; })),
				NAME_None, EUserInterfaceActionType::RadioButton);
		}
		Menu.EndSection();
		const FGeometry Geometry = Anchor->GetCachedGeometry();
		const FVector2D Where = Geometry.GetAbsolutePositionAtCoordinates(FVector2D(1.0f, 1.0f)) - FVector2D(220.0f, 0.0f);
		FSlateApplication::Get().PushMenu(Anchor.ToSharedRef(), FWidgetPath(), Menu.MakeWidget(), Where, FPopupTransitionEffect(FPopupTransitionEffect::ComboButton));
	});
}

TSharedRef<SWidget> SScenarioCreatePage::BuildRecent()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			ScenarioUI::SectionHeader(LOCTEXT("Recent", "Recent"),
				SNew(SHyperlink)
				.Text(LOCTEXT("AllJobs", "All jobs"))
				.ToolTipText(LOCTEXT("AllJobsTip", "Every request with its state and cost."))
				.OnNavigate_Lambda([]() { FScenarioEditorModule::OpenStudio(ScenarioPages::Jobs); }))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(ScenarioUI::Gutter, 6.0f))
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SAssignNew(Recent, SHorizontalBox)
			]
			+ SOverlay::Slot()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.HeightOverride(72.0f)
				.VAlign(VAlign_Center)
				.Visibility_Lambda([this]() { return RecentShown.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					SNew(STextBlock)
					.Text(LOCTEXT("RecentEmpty", "Your results appear here."))
					.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		];
}

void SScenarioCreatePage::RefreshRecent()
{
	const TArray<FString> Ids = RecentIds(RecentCount);
	if (Ids == RecentShown || !Recent.IsValid())
	{
		return;
	}
	RecentShown = Ids;
	Recent->ClearChildren();
	for (const FString& Id : Ids)
	{
		Recent->AddSlot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SScenarioJobTile).LocalId(Id)
		];
	}
}

TSharedRef<SWidget> SScenarioCreatePage::BuildDestination()
{
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();
	return SNew(SSimpleComboButton)
		.Icon(FAppStyle::GetBrush("Icons.FolderClosed"))
		.HasDownArrow(true)
		.UsesSmallText(true)
		.Text_Lambda([M]()
		{
			return M->IsWorkflow() ? LOCTEXT("TypeFolders", "Each output goes to its type folder") : FText::FromString(ScenarioUI::ShortPath(M->DestinationText()));
		})
		.ToolTipText_Lambda([M]()
		{
			return FText::Format(LOCTEXT("FolderTip", "Saves to {0}.\n{1}\nClick to choose another folder."), FText::FromString(M->DestinationText()), FText::FromString(M->PlacementText()));
		})
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
		});
}

TSharedRef<SWidget> SScenarioCreatePage::BuildActionBar()
{
	const TSharedRef<FScenarioCreateModel> M = Model.ToSharedRef();
	auto RunAction = [this, M](EScenarioHintAction Action)
	{
		const FString Id = M->GetHint().LocalId;
		switch (Action)
		{
		case EScenarioHintAction::ChangeModel: OpenPicker(); break;
		case EScenarioHintAction::OpenJobs: FScenarioEditorModule::OpenStudio(ScenarioPages::Jobs); break;
		case EScenarioHintAction::CheckNow: ScenarioJobActions::CheckUnresolved(); break;
		case EScenarioHintAction::ImportAndPlace: ScenarioJobActions::Import(Id, true); break;
		case EScenarioHintAction::ImportOnly: ScenarioJobActions::Import(Id, false); break;
		case EScenarioHintAction::Show: ScenarioJobActions::Show(Id); break;
		case EScenarioHintAction::Details: ScenarioJobActions::ShowDetails(Id); break;
		default: break;
		}
	};
	auto Link = [M, RunAction](bool bPrimary) -> TSharedRef<SWidget>
	{
		auto Which = [M, bPrimary]() { const FScenarioHint Hint = M->GetHint(); return bPrimary ? Hint.Primary : Hint.Secondary; };
		return SNew(SBox)
			.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
			.VAlign(VAlign_Center)
			.Visibility_Lambda([Which]() { return Which() == EScenarioHintAction::None ? EVisibility::Collapsed : EVisibility::Visible; })
			[
				SNew(SHyperlink)
				.Text_Lambda([Which]() { return ActionLabel(Which()); })
				.OnNavigate_Lambda([Which, RunAction]() { RunAction(Which()); })
			];
	};

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
		.Padding(FMargin(ScenarioUI::Gutter, 6.0f, ScenarioUI::Gutter, ScenarioUI::Gutter))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Left)
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				BuildDestination()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBox)
				.HeightOverride(32.0f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.HAlign(HAlign_Center)
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
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(16.0f)
					.HeightOverride(16.0f)
					.Visibility_Lambda([M]() { const EScenarioHintTone Tone = M->GetHint().Tone; return Tone == EScenarioHintTone::None || Tone == EScenarioHintTone::Info ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						[
							SNew(SCircularThrobber)
							.Radius(7.0f)
							.Visibility_Lambda([M]() { return M->GetHint().Tone == EScenarioHintTone::Busy ? EVisibility::Visible : EVisibility::Collapsed; })
						]
						+ SOverlay::Slot()
						[
							SNew(SImage)
							.Image_Lambda([M]() { return ToneIcon(M->GetHint().Tone); })
						]
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([M]() { return M->GetHint().Text; })
					.ToolTipText_Lambda([M]() { return M->GetHint().Text; })
					.ColorAndOpacity_Lambda([M]() { return ToneColor(M->GetHint().Tone); })
					.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
					.AutoWrapText(true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					Link(true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					Link(false)
				]
			]
		];
}

#undef LOCTEXT_NAMESPACE
