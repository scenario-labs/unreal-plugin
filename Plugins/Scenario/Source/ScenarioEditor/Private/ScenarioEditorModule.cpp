// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioEditorModule.h"

#include "ContentBrowserMenuContexts.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "ISettingsModule.h"
#include "LevelEditor.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "ScenarioActions.h"
#include "ScenarioCreateModel.h"
#include "ScenarioJson.h"
#include "ScenarioLanes.h"
#include "ScenarioLedger.h"
#include "ScenarioOutputLog.h"
#include "ScenarioProvenance.h"
#include "ScenarioSession.h"
#include "ScenarioSettings.h"
#include "ScenarioStyle.h"
#include "ScenarioSubsystem.h"
#include "ScenarioThumbnails.h"
#include "SScenarioComposer.h"
#include "SScenarioJobCard.h"
#include "SScenarioModelPicker.h"
#include "SScenarioPages.h"
#include "SScenarioStudio.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "ScenarioEditor"

IMPLEMENT_MODULE(FScenarioEditorModule, ScenarioEditor)

namespace
{
	const FName StudioTab(TEXT("ScenarioStudio"));
	TArray<TPair<FName, FScenarioEditorModule::FSectionFactory>> Sections;
	FScenarioEditorModule::FSectionFactory HeaderStatus;
	TSharedPtr<FUICommandList> GlobalCommands;

	/** Content Browser assets that can be a Scenario input. */
	bool IsInputClass(const FAssetData& Asset)
	{
		static const TArray<FString> Classes = { TEXT("Texture2D"), TEXT("StaticMesh"), TEXT("SkeletalMesh"), TEXT("SoundWave"), TEXT("FileMediaSource") };
		return Classes.Contains(Asset.AssetClassPath.GetAssetName().ToString());
	}

	FString KindOfClass(const FAssetData& Asset)
	{
		const FString Class = Asset.AssetClassPath.GetAssetName().ToString();
		if (Class == TEXT("StaticMesh") || Class == TEXT("SkeletalMesh"))
		{
			return TEXT("3d");
		}
		if (Class == TEXT("SoundWave"))
		{
			return TEXT("audio");
		}
		if (Class == TEXT("FileMediaSource"))
		{
			return TEXT("video");
		}
		return TEXT("image");
	}

	/** Opens Create on a lane and mode with a local file as the input (uploaded once the form is ready). */
	void OpenWithFile(const FString& Lane, const FString& Mode, const FString& Path, const FString& Label)
	{
		const TSharedRef<FScenarioCreateModel> Create = FScenarioCreateModel::Get();
		Create->SetLane(Lane);
		if (!Mode.IsEmpty())
		{
			Create->SetMode(Mode);
		}
		const FString Error = Create->UseFile(Path, Label);
		FScenarioEditorModule::OpenStudio(ScenarioPages::Create);
		if (!Error.IsEmpty())
		{
			FScenarioEditorModule::Toast(LOCTEXT("NotAdded", "Reference not added"), FText::FromString(Error), false);
		}
	}

	/** Next steps for a Content Browser asset: reuses its Scenario asset id, or uploads its file. */
	void AddAssetSteps(FMenuBuilder& Menu, const FAssetData& Asset)
	{
		const FString Kind = KindOfClass(Asset);
		const FString Label = Asset.AssetName.ToString();
		for (const ScenarioJobActions::FNextStep& Step : ScenarioJobActions::NextSteps(Kind))
		{
			Menu.AddMenuEntry(Step.Label, Step.Tooltip, FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([Step, Asset, Kind, Label]()
			{
				UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
				if (!Subsystem)
				{
					return;
				}
				FScenarioUploadSource Source;
				FString Error;
				if (!Subsystem->ResolveUploadSource(Asset.GetAsset(), Source, Error))
				{
					FScenarioEditorModule::Toast(LOCTEXT("CannotUse", "Cannot use this asset"), FText::FromString(Error), false);
					return;
				}
				if (!Source.AssetId.IsEmpty())
				{
					ScenarioJobActions::RunNextStep(Step, Source.AssetId, Kind, Label);
					return;
				}
				if (!Step.Chip.IsEmpty())
				{
					const FString Path = Source.Path;
					SScenarioModelPicker::Open(Step.Modality, Step.Chip, [Path, Label](const FString& ModelId, const FString& Name)
					{
						const TSharedRef<FScenarioCreateModel> Create = FScenarioCreateModel::Get();
						Create->SetModel(ModelId, Name);
						Create->UseFile(Path, Label);
						FScenarioEditorModule::OpenStudio(ScenarioPages::Create);
					});
					return;
				}
				OpenWithFile(Step.Lane, Step.Mode, Source.Path, Label);
			})));
		}
	}

	void ShowGeneration(const FAssetData& Asset)
	{
		UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
		const TMap<FName, FString>* Values = Subsystem ? Subsystem->Provenance().Find(Asset.GetObjectPathString()) : nullptr;
		if (!Values)
		{
			return;
		}
		FString Text;
		for (const FName& Key : ScenarioProvenance::Keys())
		{
			if (const FString* Value = Values->Find(Key))
			{
				if (!Value->IsEmpty())
				{
					Text += FString::Printf(TEXT("%s: %s\n"), *Key.ToString().RightChop(9), **Value);
				}
			}
		}
		const FString* Lane = Values->Find(TEXT("Scenario.Lane"));
		const FString* ModelId = Values->Find(TEXT("Scenario.ModelId"));
		const FString* Parameters = Values->Find(TEXT("Scenario.Parameters"));
		const FString LaneId = Lane ? *Lane : FString();
		const FString Model = ModelId ? *ModelId : FString();
		const FString Params = Parameters ? *Parameters : FString();

		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(FText::Format(LOCTEXT("GenerationTitle", "How {0} was made"), FText::FromName(Asset.AssetName)))
			.ClientSize(FVector2D(560.0f, 420.0f))
			.SupportsMinimize(false)
			.SupportsMaximize(false);
		const TWeakPtr<SWindow> WeakWindow = Window;
		Window->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(12.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SMultiLineEditableTextBox).Text(FText::FromString(Text)).IsReadOnly(true).AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Right)
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("UseSettings", "Use settings"))
					.ToolTipText(LOCTEXT("UseSettingsTip", "Fills the Create form with this model and these settings. Sends nothing."))
					.IsEnabled(!Model.IsEmpty())
					.OnClicked_Lambda([LaneId, Model, Params, WeakWindow]()
					{
						FScenarioEditorModule::OpenCreate(LaneId, Model, ScenarioJson::ParseObject(Params));
						if (const TSharedPtr<SWindow> Pinned = WeakWindow.Pin())
						{
							Pinned->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
			]);
		FSlateApplication::Get().AddWindow(Window);
	}

	FString NewestReadyJob()
	{
		FScenarioSession* Session = ScenarioUI::Session();
		if (!Session)
		{
			return FString();
		}
		for (const FScenarioJobRow& Row : Session->Ledger().Rows())
		{
			if (Row.State == EScenarioJobState::Success && Row.Source == TEXT("person") && Row.Kind != TEXT("prompt_spark") && Row.AssetIds.Num() > 0)
			{
				return Row.LocalId;
			}
		}
		return FString();
	}
}

FScenarioEditorModule& FScenarioEditorModule::Get()
{
	return FModuleManager::LoadModuleChecked<FScenarioEditorModule>(TEXT("ScenarioEditor"));
}

void FScenarioEditorModule::StartupModule()
{
	FScenarioOutputLog::Get().Register();
	FScenarioActions::Get();
	FScenarioStyle::Register();
	FScenarioCommands::Register();
	BindCommands();
	RegisterTabs();
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FScenarioEditorModule::RegisterMenus));

	// Plugins in the PostEngineInit phase load after OnPostEngineInit fires, and
	// a module's editor subsystems are created after its StartupModule: start on
	// the first tick where the subsystem exists.
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FScenarioEditorModule::OnPostEngineInit);
	StartTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
	{
		OnPostEngineInit();
		return !bStarted;
	}));
}

void FScenarioEditorModule::OnPostEngineInit()
{
	if (bStarted || !GEditor || !UScenarioSubsystem::Get())
	{
		return;
	}
	bStarted = true;
	FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
	FScenarioCreateModel::Get()->Initialize();
	if (FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
	{
		LevelEditor->GetGlobalLevelEditorActions()->Append(GlobalCommands.ToSharedRef());
		// The composer goes into the viewport once the level editor exists.
		LevelEditor->OnLevelEditorCreated().AddLambda([](TSharedPtr<class ILevelEditor>)
		{
			if (UScenarioEditorSettings::Get().bShowComposer)
			{
				SScenarioComposer::Show(true);
			}
		});
		// Loaded after the level editor (plugin enabled in a running editor).
		if (LevelEditor->GetFirstActiveViewport().IsValid() && UScenarioEditorSettings::Get().bShowComposer)
		{
			SScenarioComposer::Show(true);
		}
	}
	UScenarioSubsystem::Get()->ShowWelcomeIfNeeded();
}

void FScenarioEditorModule::ShutdownModule()
{
	FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
	FTSTicker::GetCoreTicker().RemoveTicker(StartTicker);
	if (bStarted)
	{
		SScenarioComposer::Show(false);
		FScenarioCreateModel::Get()->Shutdown();
	}
	FScenarioThumbnails::Get().Shutdown();
	FScenarioActions::Get().Reset();
	FScenarioOutputLog::Get().Unregister();
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(StudioTab);
	}
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	FScenarioCommands::Unregister();
	FScenarioStyle::Unregister();
	Sections.Reset();
	HeaderStatus = nullptr;
	GlobalCommands.Reset();
	Commands.Reset();
}

void FScenarioEditorModule::BindCommands()
{
	const FScenarioCommands& C = FScenarioCommands::Get();

	// Work anywhere in the level editor.
	GlobalCommands = MakeShared<FUICommandList>();
	GlobalCommands->MapAction(C.OpenStudio, FExecuteAction::CreateLambda([]() { OpenStudio(); }));
	GlobalCommands->MapAction(C.ToggleComposer, FExecuteAction::CreateLambda([]() { ToggleComposer(); }), FCanExecuteAction(),
		FIsActionChecked::CreateLambda([]() { return IsComposerVisible(); }));

	// Work while the Studio has focus.
	Commands = MakeShared<FUICommandList>();
	Commands->Append(GlobalCommands.ToSharedRef());
	Commands->MapAction(C.Generate, FExecuteAction::CreateLambda([]() { FScenarioCreateModel::Get()->Generate(); }),
		FCanExecuteAction::CreateLambda([]() { return FScenarioCreateModel::Get()->CanClickGenerate(); }));
	Commands->MapAction(C.OpenPicker, FExecuteAction::CreateLambda([]()
	{
		if (const TSharedPtr<SScenarioStudio> Studio = SScenarioStudio::Current())
		{
			Studio->SetPage(ScenarioPages::Create);
			Studio->GetCreatePage()->OpenPicker();
		}
	}));
	Commands->MapAction(C.ImportAndPlace, FExecuteAction::CreateLambda([]()
	{
		const FString Id = NewestReadyJob();
		if (!Id.IsEmpty())
		{
			ScenarioJobActions::Import(Id, true);
		}
	}), FCanExecuteAction::CreateLambda([]() { return !NewestReadyJob().IsEmpty(); }));
}

void FScenarioEditorModule::RegisterTabs()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(StudioTab, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			// The running count stays visible when the tab sits behind another one.
			.Label_Lambda([]()
			{
				int32 Running = 0;
				if (FScenarioSession* S = ScenarioUI::Session())
				{
					for (const FScenarioJobRow& Row : S->Ledger().Rows())
					{
						Running += Row.IsActive() ? 1 : 0;
					}
				}
				return Running > 0 ? FText::Format(LOCTEXT("StudioTabRunning", "Scenario · {0}"), FText::AsNumber(Running)) : LOCTEXT("StudioTabLabel", "Scenario");
			})
			[
				SNew(SScenarioStudio)
			];
	}))
	.SetDisplayName(LOCTEXT("StudioTab", "Scenario"))
	.SetTooltipText(LOCTEXT("StudioTabTip", "Every Scenario model, priced before you spend."))
	.SetIcon(FSlateIcon(FScenarioStyle::GetStyleSetName(), "Scenario.Studio"))
	.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FScenarioEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	const FScenarioCommands& C = FScenarioCommands::Get();

	// Window menu
	if (UToolMenu* Window = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window")))
	{
		FToolMenuSection& Section = Window->FindOrAddSection(TEXT("Scenario"), LOCTEXT("ScenarioSection", "Scenario"));
		Section.AddMenuEntryWithCommandList(C.OpenStudio, GlobalCommands);
		Section.AddMenuEntryWithCommandList(C.ToggleComposer, GlobalCommands);
	}

	// Level editor toolbar: the button opens the Studio, the arrow shows quick actions.
	if (UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.User")))
	{
		FToolMenuSection& Section = Toolbar->FindOrAddSection(TEXT("Scenario"));
		FToolMenuEntry Button = FToolMenuEntry::InitToolBarButton(C.OpenStudio, LOCTEXT("ToolbarLabel", "Scenario"), LOCTEXT("ToolbarTip", "Opens the Scenario Studio (Alt+Shift+S)."),
			FSlateIcon(FScenarioStyle::GetStyleSetName(), "Scenario.Mark"));
		Button.SetCommandList(GlobalCommands);
		Section.AddEntry(Button);
		Section.AddEntry(FToolMenuEntry::InitComboButton(TEXT("ScenarioCombo"), FUIAction(), FOnGetContent::CreateLambda([]()
		{
			FMenuBuilder Menu(true, GlobalCommands);
			Menu.BeginSection(NAME_None, LOCTEXT("Account", "Account"));
			Menu.AddMenuEntry(TAttribute<FText>::CreateLambda([]()
				{
					FScenarioSession* S = ScenarioUI::Session();
					return S && S->IsSignedIn() ? FText::FromString(S->GetWorkspace().Label()) : LOCTEXT("SignIn", "Sign in to Scenario");
				}), LOCTEXT("AccountTip", "Opens Connection: account, project, agents, diagnostics."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]() { OpenStudio(ScenarioPages::Connection); })));
			Menu.EndSection();
			Menu.BeginSection(NAME_None, LOCTEXT("Generate", "Generate"));
			for (const FScenarioLane& Lane : FScenarioLanes::Get().All())
			{
				const FString Id = Lane.Id;
				Menu.AddMenuEntry(FText::FromString(Lane.Title), FText::FromString(Lane.Hint), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([Id]() { OpenCreate(Id); })));
			}
			Menu.EndSection();
			Menu.BeginSection(NAME_None, LOCTEXT("Recent", "Recent jobs"));
			if (FScenarioSession* S = ScenarioUI::Session())
			{
				int32 Shown = 0;
				for (const FScenarioJobRow& Row : S->Ledger().Rows())
				{
					if (Row.State == EScenarioJobState::Dismissed || Row.Kind == TEXT("prompt_spark") || Shown >= 5)
					{
						continue;
					}
					++Shown;
					const FText Label = FText::Format(LOCTEXT("RecentEntry", "{0}: {1}"), FText::FromString(Row.ModelName.IsEmpty() ? Row.ModelId : Row.ModelName), ScenarioJobActions::StateText(Row));
					Menu.AddMenuEntry(Label, FText::FromString(Row.Prompt), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]() { OpenStudio(ScenarioPages::Jobs); })));
				}
			}
			Menu.EndSection();
			Menu.BeginSection(NAME_None, LOCTEXT("More", "More"));
			Menu.AddMenuEntry(FScenarioCommands::Get().ToggleComposer);
			Menu.AddMenuEntry(LOCTEXT("Agents", "Agents and diagnostics"), LOCTEXT("AgentsTip", "Scenario MCP connection for agents, activity log, diagnostics."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]() { OpenStudio(ScenarioPages::Connection); })));
			Menu.AddMenuEntry(LOCTEXT("Settings", "Settings"), LOCTEXT("SettingsTip", "Scenario editor preferences."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]() { OpenSettings(false); })));
			Menu.EndSection();
			return Menu.MakeWidget();
		}), LOCTEXT("ComboLabel", "Scenario options"), LOCTEXT("ComboTip", "Account, quick actions per lane, recent jobs."), FSlateIcon(), true));
	}

	// Content Browser: folders
	if (UToolMenu* Folder = UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.FolderContextMenu")))
	{
		Folder->AddDynamicSection(TEXT("Scenario"), FNewToolMenuDelegate::CreateLambda([](UToolMenu* Menu)
		{
			const UContentBrowserFolderContext* Context = Menu->FindContext<UContentBrowserFolderContext>();
			if (!Context || Context->GetSelectedPackagePaths().Num() != 1)
			{
				return;
			}
			const FString Path = Context->GetSelectedPackagePaths()[0];
			FToolMenuSection& Section = Menu->AddSection(TEXT("Scenario"), LOCTEXT("ScenarioSection", "Scenario"));
			Section.AddMenuEntry(TEXT("ScenarioGenerateHere"), LOCTEXT("GenerateHere", "Generate into this folder"),
				LOCTEXT("GenerateHereTip", "Opens Create with this folder as the destination. Sends nothing."),
				FSlateIcon(FScenarioStyle::GetStyleSetName(), "Scenario.Mark.Small"),
				FUIAction(FExecuteAction::CreateLambda([Path]()
				{
					FScenarioCreateModel::Get()->Destination = Path;
					OpenStudio(ScenarioPages::Create);
				})));
		}));
	}

	// Content Browser: assets
	if (UToolMenu* AssetMenu = UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.AssetContextMenu")))
	{
		AssetMenu->AddDynamicSection(TEXT("Scenario"), FNewToolMenuDelegate::CreateLambda([](UToolMenu* Menu)
		{
			const UContentBrowserAssetContextMenuContext* Context = Menu->FindContext<UContentBrowserAssetContextMenuContext>();
			if (!Context || Context->SelectedAssets.Num() != 1 || !IsInputClass(Context->SelectedAssets[0]))
			{
				return;
			}
			const FAssetData Asset = Context->SelectedAssets[0];
			UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
			const bool bFromScenario = Subsystem && Subsystem->Provenance().Contains(Asset.GetObjectPathString());
			FToolMenuSection& Section = Menu->AddSection(TEXT("Scenario"), LOCTEXT("ScenarioSection", "Scenario"));
			Section.AddSubMenu(TEXT("ScenarioAssetMenu"), LOCTEXT("ScenarioSub", "Scenario"), LOCTEXT("ScenarioSubTip", "Next steps with Scenario. Each opens Create; nothing is sent until you press Generate."),
				FNewMenuDelegate::CreateLambda([Asset, bFromScenario](FMenuBuilder& Sub)
				{
					Sub.BeginSection(NAME_None, LOCTEXT("Next", "Next"));
					AddAssetSteps(Sub, Asset);
					Sub.EndSection();
					Sub.BeginSection(NAME_None, LOCTEXT("Other", "Other"));
					Sub.AddMenuEntry(LOCTEXT("UseAsReference", "Use as reference"), LOCTEXT("UseAsReferenceTip", "Adds this asset to the current Create form."), FSlateIcon(),
						FUIAction(FExecuteAction::CreateLambda([Asset]()
						{
							UScenarioSubsystem* S = UScenarioSubsystem::Get();
							FScenarioUploadSource Source;
							FString Error;
							if (!S || !S->ResolveUploadSource(Asset.GetAsset(), Source, Error))
							{
								Toast(LOCTEXT("CannotUseRef", "Cannot use this asset"), FText::FromString(Error), false);
								return;
							}
							const TSharedRef<FScenarioCreateModel> Create = FScenarioCreateModel::Get();
							if (Source.AssetId.IsEmpty())
							{
								Error = Create->UseFile(Source.Path, Source.Label);
							}
							else
							{
								Create->UseReference(Source.AssetId, Source.Kind, Source.Label);
							}
							OpenStudio(ScenarioPages::Create);
							if (!Error.IsEmpty())
							{
								Toast(LOCTEXT("RefNotAdded", "Reference not added"), FText::FromString(Error), false);
							}
						})));
					if (bFromScenario)
					{
						Sub.AddMenuEntry(LOCTEXT("ShowGeneration", "Show generation"), LOCTEXT("ShowGenerationTip", "Model, prompt, cost and date of this asset."), FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([Asset]() { ShowGeneration(Asset); })));
					}
					Sub.EndSection();
				}),
				false, FSlateIcon(FScenarioStyle::GetStyleSetName(), "Scenario.Mark.Small"));
		}));
	}

	// Level viewport and Outliner: actors
	if (UToolMenu* ActorMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.ActorContextMenu")))
	{
		FToolMenuSection& Section = ActorMenu->FindOrAddSection(TEXT("Scenario"), LOCTEXT("ScenarioSection", "Scenario"));
		Section.AddSubMenu(TEXT("ScenarioActorMenu"), LOCTEXT("ScenarioSub", "Scenario"), LOCTEXT("ActorSubTip", "Generate for the selected actors. Each opens Create; nothing is sent until you press Generate."),
			FNewMenuDelegate::CreateLambda([](FMenuBuilder& Sub)
			{
				Sub.AddMenuEntry(LOCTEXT("Material", "Generate material for this"), LOCTEXT("MaterialTip", "Opens the Material lane. Import and place applies the result to the selected actor."), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([]() { OpenCreate(TEXT("material")); })));
				Sub.AddMenuEntry(LOCTEXT("Retexture", "Retexture or edit (3D)"), LOCTEXT("RetextureTip", "Exports the selection to GLB and opens the 3D lane in Edit mode with it as the input."), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([]()
					{
						UScenarioSubsystem* S = UScenarioSubsystem::Get();
						if (!S || !GEditor)
						{
							return;
						}
						TArray<AActor*> Actors;
						GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Actors);
						FString Error;
						const FString Path = S->ExportActorsToGlb(Actors, Error);
						if (Path.IsEmpty())
						{
							Toast(LOCTEXT("ExportFailed", "Could not export the selection"), FText::FromString(Error), false);
							return;
						}
						OpenWithFile(TEXT("3d"), TEXT("edit"), Path, Actors.Num() == 1 ? Actors[0]->GetActorLabel() : TEXT("Selection"));
					})));
				Sub.AddMenuEntry(LOCTEXT("Restyle", "Restyle this view"), LOCTEXT("RestyleTip", "Captures the active viewport and opens the Render lane with it as the reference."), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([]()
					{
						UScenarioSubsystem* S = UScenarioSubsystem::Get();
						if (!S)
						{
							return;
						}
						FString Error;
						const FString Path = S->CaptureViewport(1536, 864, Error);
						if (Path.IsEmpty())
						{
							Toast(LOCTEXT("CaptureFailed", "Could not capture the viewport"), FText::FromString(Error), false);
							return;
						}
						OpenWithFile(TEXT("render"), FString(), Path, TEXT("Viewport"));
					})));
			}),
			false, FSlateIcon(FScenarioStyle::GetStyleSetName(), "Scenario.Mark.Small"));
	}
}

void FScenarioEditorModule::OpenStudio(FName Page)
{
	FGlobalTabmanager::Get()->TryInvokeTab(StudioTab);
	if (const TSharedPtr<SScenarioStudio> Studio = SScenarioStudio::Current())
	{
		if (!Page.IsNone())
		{
			Studio->SetPage(Page);
		}
		if (Page == ScenarioPages::Create || Page.IsNone())
		{
			if (const TSharedPtr<SScenarioCreatePage> Create = Studio->GetCreatePage())
			{
				Create->FocusPrompt();
			}
		}
	}
}

void FScenarioEditorModule::OpenCreate(const FString& Lane, const FString& ModelId, const TSharedPtr<FJsonObject>& Values)
{
	const TSharedRef<FScenarioCreateModel> Create = FScenarioCreateModel::Get();
	if (!Lane.IsEmpty())
	{
		Create->SetLane(Lane);
	}
	if (!ModelId.IsEmpty() && ModelId != Create->GetModelId())
	{
		Create->SetModel(ModelId);
	}
	if (Values.IsValid())
	{
		Create->ApplyValues(Values);
	}
	OpenStudio(ScenarioPages::Create);
}

void FScenarioEditorModule::ToggleComposer()
{
	const bool bShow = !SScenarioComposer::IsShown();
	SScenarioComposer::Show(bShow);
	UScenarioEditorSettings& Settings = UScenarioEditorSettings::GetMutable();
	Settings.bShowComposer = bShow;
	Settings.SaveConfig();
}

bool FScenarioEditorModule::IsComposerVisible()
{
	return SScenarioComposer::IsShown();
}

void FScenarioEditorModule::OpenSettings(bool bProject)
{
	if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
	{
		Settings->ShowViewer(bProject ? TEXT("Project") : TEXT("Editor"), TEXT("Plugins"), TEXT("Scenario"));
	}
}

void FScenarioEditorModule::Toast(const FText& Text, const FText& SubText, bool bSuccess, const TArray<FText>& ButtonLabels, const TArray<TFunction<void()>>& Buttons)
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	FNotificationInfo Info(Text);
	Info.SubText = SubText;
	Info.ExpireDuration = Buttons.Num() > 0 ? 10.0f : 5.0f;
	Info.bFireAndForget = true;
	Info.bUseSuccessFailIcons = true;
	Info.Image = FScenarioStyle::Get().GetBrush("Scenario.Mark");
	TSharedRef<TWeakPtr<SNotificationItem>> Item = MakeShared<TWeakPtr<SNotificationItem>>();
	for (int32 Index = 0; Index < FMath::Min(ButtonLabels.Num(), Buttons.Num()); ++Index)
	{
		const TFunction<void()> Action = Buttons[Index];
		Info.ButtonDetails.Add(FNotificationButtonInfo(ButtonLabels[Index], FText::GetEmpty(), FSimpleDelegate::CreateLambda([Action, Item]()
		{
			Action();
			if (const TSharedPtr<SNotificationItem> Pinned = Item->Pin())
			{
				Pinned->ExpireAndFadeout();
			}
		}), SNotificationItem::CS_None));
	}
	const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification.IsValid())
	{
		*Item = Notification;
		Notification->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}
}

void FScenarioEditorModule::RegisterConnectionSection(FName Name, FSectionFactory Factory)
{
	UnregisterConnectionSection(Name);
	Sections.Add({ Name, MoveTemp(Factory) });
}

void FScenarioEditorModule::UnregisterConnectionSection(FName Name)
{
	Sections.RemoveAll([Name](const TPair<FName, FSectionFactory>& Entry) { return Entry.Key == Name; });
}

const TArray<TPair<FName, FScenarioEditorModule::FSectionFactory>>& FScenarioEditorModule::ConnectionSections()
{
	return Sections;
}

void FScenarioEditorModule::SetHeaderStatus(FSectionFactory Factory)
{
	HeaderStatus = MoveTemp(Factory);
}

TSharedRef<SWidget> FScenarioEditorModule::MakeHeaderStatus()
{
	return HeaderStatus ? HeaderStatus() : SNullWidget::NullWidget;
}

#undef LOCTEXT_NAMESPACE
