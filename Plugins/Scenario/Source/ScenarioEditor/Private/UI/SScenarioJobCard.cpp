// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioJobCard.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetThumbnail.h"
#include "ContentBrowserModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IContentBrowserSingleton.h"
#include "ScenarioCreateModel.h"
#include "ScenarioCU.h"
#include "ScenarioEditorModule.h"
#include "ScenarioJson.h"
#include "ScenarioStyle.h"
#include "ScenarioSubsystem.h"
#include "ScenarioThumbnails.h"
#include "ScenarioWidgets.h"
#include "SScenarioModelPicker.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioJobs"

namespace
{
	FScenarioSession* SessionOrNull()
	{
		UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
		return Subsystem ? &Subsystem->Session() : nullptr;
	}

	const FScenarioJobRow* FindRow(const FString& LocalId)
	{
		FScenarioSession* Session = SessionOrNull();
		return Session ? Session->Ledger().Find(LocalId) : nullptr;
	}

	bool HasAllFiles(const FScenarioJobRow& Row)
	{
		if (Row.Files.Num() == 0)
		{
			return false;
		}
		for (const FScenarioJobFile& File : Row.Files)
		{
			if (File.LocalPath.IsEmpty() || !FPaths::FileExists(File.LocalPath))
			{
				return false;
			}
		}
		return true;
	}

	/** Downloads and running jobs report progress; the rest have none. */
	TOptional<float> BusyProgress(const FString& LocalId)
	{
		const FScenarioJobRow* Row = FindRow(LocalId);
		FScenarioSession* Session = SessionOrNull();
		if (!Row)
		{
			return TOptional<float>();
		}
		if (Session && Session->IsDownloading(LocalId))
		{
			return FMath::Max(0.0f, Session->DownloadProgress(LocalId));
		}
		return Row->Progress > 0.0 ? TOptional<float>(static_cast<float>(Row->Progress)) : TOptional<float>();
	}

	bool IsBusy(const FString& LocalId)
	{
		const FScenarioJobRow* Row = FindRow(LocalId);
		if (!Row)
		{
			return false;
		}
		const ScenarioJobActions::EView View = ScenarioJobActions::View(*Row);
		return View == ScenarioJobActions::EView::Sending || View == ScenarioJobActions::EView::Queued || View == ScenarioJobActions::EView::Running
			|| View == ScenarioJobActions::EView::Downloading || View == ScenarioJobActions::EView::Importing;
	}

	/** A one-line action button for a row: the state's action, hidden when there is none. */
	TSharedRef<SWidget> RowActionButton(const FString& LocalId)
	{
		auto Action = [LocalId]()
		{
			const FScenarioJobRow* Row = FindRow(LocalId);
			return Row ? ScenarioJobActions::PrimaryAction(*Row) : ScenarioJobActions::FRowAction();
		};
		return SNew(SButton)
			.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>(ScenarioUI::ButtonStyleName(ScenarioUI::EButton::Outline, true)))
			.VAlign(VAlign_Center)
			.Visibility_Lambda([Action]() { return Action().IsSet() ? EVisibility::Visible : EVisibility::Collapsed; })
			.ToolTipText_Lambda([Action]() { return Action().Tooltip; })
			.OnClicked_Lambda([Action]()
			{
				const ScenarioJobActions::FRowAction Now = Action();
				if (Now.IsSet())
				{
					Now.Run();
				}
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SImage)
					.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
					.Image_Lambda([Action]() { const FName Icon = Action().Icon; return Icon.IsNone() ? nullptr : ScenarioUI::IconBrush(Icon); })
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([Action]() { return Action().Label; })
					.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				]
			];
	}

	TSharedRef<SWidget> OverflowButton(const FString& LocalId)
	{
		return SNew(SComboButton)
			.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>(ScenarioUI::ButtonStyleName(ScenarioUI::EButton::Ghost, true)))
			.HasDownArrow(false)
			.ContentPadding(FMargin(2.0f))
			.ToolTipText(LOCTEXT("MoreTip", "More actions: send to another model, reuse settings, details, hide."))
			.ButtonContent()
			[
				ScenarioUI::Icon(TEXT("more"), 16.0f, FSlateColor(ScenarioUI::Palette::Dim()))
			]
			.OnGetMenuContent_Lambda([LocalId]() { return ScenarioJobActions::MoreMenu(LocalId); });
	}

	TSharedRef<SWidget> ThinProgress(const FString& LocalId)
	{
		return SNew(SBox)
			.HeightOverride(3.0f)
			.Visibility_Lambda([LocalId]() { return IsBusy(LocalId) ? EVisibility::HitTestInvisible : EVisibility::Hidden; })
			[
				SNew(SProgressBar)
				.Style(&ScenarioUI::Style().GetWidgetStyle<FProgressBarStyle>("Scenario.Progress"))
				.Percent_Lambda([LocalId]() { return BusyProgress(LocalId); })
			];
	}
}

namespace ScenarioJobActions
{
	TArray<FNextStep> NextSteps(const FString& Kind)
	{
		TArray<FNextStep> Steps;
		auto Lane = [&Steps](const FText& Label, const FText& Tip, const TCHAR* InLane, const TCHAR* InMode)
		{
			FNextStep Step;
			Step.Label = Label;
			Step.Tooltip = Tip;
			Step.Lane = InLane;
			Step.Mode = InMode;
			Steps.Add(Step);
		};
		auto Chip = [&Steps](const FText& Label, const FText& Tip, const TCHAR* InModality, const TCHAR* InChip)
		{
			FNextStep Step;
			Step.Label = Label;
			Step.Tooltip = Tip;
			Step.Modality = InModality;
			Step.Chip = InChip;
			Steps.Add(Step);
		};
		if (Kind == TEXT("3d"))
		{
			Lane(LOCTEXT("Edit3D", "Edit the mesh"), LOCTEXT("Edit3DTip", "Opens the 3D lane on Mesh with this mesh as the input."), TEXT("3d"), TEXT("edit"));
			Lane(LOCTEXT("Rig", "Rig"), LOCTEXT("RigTip", "Opens the 3D lane's rigging models with this mesh as the input."), TEXT("3d"), TEXT("edit"));
			Chip(LOCTEXT("Retexture", "Retexture"), LOCTEXT("RetextureTip", "Picks a retexture model for this mesh."), TEXT("3d"), TEXT("Retexture"));
			Chip(LOCTEXT("Remesh", "Remesh"), LOCTEXT("RemeshTip", "Picks a remesh model for this mesh."), TEXT("3d"), TEXT("Remesh"));
		}
		else if (Kind == TEXT("video"))
		{
			Chip(LOCTEXT("UpscaleVideo", "Upscale"), LOCTEXT("UpscaleVideoTip", "Picks a video upscaler for this clip."), TEXT("video"), TEXT("Upscale"));
		}
		else if (Kind == TEXT("audio"))
		{
		}
		else
		{
			Lane(LOCTEXT("EditImage", "Edit the image"), LOCTEXT("EditImageTip", "Opens the Image lane with this image as the reference."), TEXT("image"), TEXT(""));
			Lane(LOCTEXT("Make3D", "Make 3D"), LOCTEXT("Make3DTip", "Opens the 3D lane on Image with this image as the input."), TEXT("3d"), TEXT("image"));
			Lane(LOCTEXT("MakeMaterial", "Make a material"), LOCTEXT("MakeMaterialTip", "Opens the Material lane with this image as the reference."), TEXT("material"), TEXT(""));
			Lane(LOCTEXT("Animate", "Animate"), LOCTEXT("AnimateTip", "Opens the Video lane with this image as the first frame."), TEXT("video"), TEXT(""));
			Chip(LOCTEXT("Upscale", "Upscale"), LOCTEXT("UpscaleTip", "Picks an image upscaler for this image."), TEXT("image"), TEXT("Upscale"));
			Chip(LOCTEXT("RemoveBackground", "Remove background"), LOCTEXT("RemoveBackgroundTip", "Picks a background removal model for this image."), TEXT("image"), TEXT("Remove background"));
		}
		return Steps;
	}

	void RunNextStep(const FNextStep& Step, const FString& AssetId, const FString& Kind, const FString& Label)
	{
		const TSharedRef<FScenarioCreateModel> Create = FScenarioCreateModel::Get();
		if (!Step.Chip.IsEmpty())
		{
			SScenarioModelPicker::Open(Step.Modality, Step.Chip, [AssetId, Kind, Label](const FString& ModelId, const FString& Name)
			{
				const TSharedRef<FScenarioCreateModel> Form = FScenarioCreateModel::Get();
				Form->SetModel(ModelId, Name);
				Form->UseReference(AssetId, Kind, Label);
				FScenarioEditorModule::OpenStudio(ScenarioPages::Create);
			});
			return;
		}
		Create->SetLane(Step.Lane);
		if (!Step.Mode.IsEmpty())
		{
			Create->SetMode(Step.Mode);
		}
		Create->UseReference(AssetId, Kind, Label);
		FScenarioEditorModule::OpenStudio(ScenarioPages::Create);
	}

	void AddNextStepsMenu(FMenuBuilder& Menu, const FString& AssetId, const FString& Kind, const FString& Label)
	{
		for (const FNextStep& Step : NextSteps(Kind))
		{
			Menu.AddMenuEntry(Step.Label, Step.Tooltip, FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([Step, AssetId, Kind, Label]()
			{
				RunNextStep(Step, AssetId, Kind, Label);
			})));
		}
	}

	EView View(const FScenarioJobRow& Row)
	{
		switch (Row.State)
		{
		case EScenarioJobState::Submitting: return EView::Sending;
		case EScenarioJobState::Queued: return EView::Queued;
		case EScenarioJobState::Running: return EView::Running;
		case EScenarioJobState::CancelRequested: return EView::Canceling;
		case EScenarioJobState::Canceled: return EView::Canceled;
		case EScenarioJobState::Failed: return EView::Failed;
		case EScenarioJobState::Blocked: return EView::Blocked;
		case EScenarioJobState::Unknown: return EView::Unknown;
		case EScenarioJobState::Rejected: return EView::NotSent;
		case EScenarioJobState::Dismissed: return EView::Hidden;
		case EScenarioJobState::Success:
		default:
			break;
		}
		FScenarioSession* Session = SessionOrNull();
		if (Session && Session->IsDownloading(Row.LocalId))
		{
			return EView::Downloading;
		}
		UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
		if (Subsystem && Subsystem->IsImporting(Row.LocalId))
		{
			return EView::Importing;
		}
		if (Row.bImported)
		{
			return EView::Imported;
		}
		return Row.Error.IsEmpty() ? EView::Ready : EView::DownloadStopped;
	}

	FText StateText(const FScenarioJobRow& Row)
	{
		switch (View(Row))
		{
		case EView::Failed: return FText::Format(LOCTEXT("FailedLong", "Failed: {0}"), FText::FromString(Row.Error));
		case EView::Blocked: return FText::Format(LOCTEXT("BlockedLong", "Blocked by Scenario's content rules. {0}"), FText::FromString(Row.Error));
		case EView::Unknown: return LOCTEXT("UnknownLong", "We do not know if Scenario received this request. An identical request is on hold until you check.");
		case EView::NotSent: return FText::Format(LOCTEXT("NotSentLong", "Not sent: {0}"), FText::FromString(Row.Error));
		case EView::DownloadStopped: return FText::Format(LOCTEXT("StoppedLong", "The download stopped: {0}"), FText::FromString(Row.Error));
		default: return ChipText(Row);
		}
	}

	FText ChipText(const FScenarioJobRow& Row)
	{
		const auto Percent = [](double Value) { return FText::AsNumber(FMath::RoundToInt(Value * 100.0)); };
		switch (View(Row))
		{
		case EView::Sending: return LOCTEXT("Sending", "Sending");
		case EView::Queued: return LOCTEXT("Queued", "Queued");
		case EView::Running:
			return Row.Progress > 0.0 ? FText::Format(LOCTEXT("Generating", "Generating {0}%"), Percent(Row.Progress)) : LOCTEXT("GeneratingNoProgress", "Generating");
		case EView::Downloading:
		{
			FScenarioSession* Session = SessionOrNull();
			const float Progress = Session ? Session->DownloadProgress(Row.LocalId) : 0.0f;
			return Progress >= 0.995f ? LOCTEXT("Finishing", "Finishing") : FText::Format(LOCTEXT("Downloading", "Downloading {0}%"), Percent(FMath::Max(0.0f, Progress)));
		}
		case EView::Importing: return LOCTEXT("Importing", "Importing");
		case EView::Ready: return Row.Kind == TEXT("prompt_spark") ? LOCTEXT("Done", "Done") : LOCTEXT("Ready", "Ready");
		case EView::Imported: return LOCTEXT("Imported", "Imported");
		case EView::DownloadStopped: return LOCTEXT("Stopped", "Download stopped");
		case EView::Failed: return LOCTEXT("Failed", "Failed");
		case EView::Blocked: return LOCTEXT("Blocked", "Blocked");
		case EView::Unknown: return LOCTEXT("Unknown", "Not confirmed");
		case EView::Canceling: return LOCTEXT("Canceling", "Canceling");
		case EView::Canceled: return LOCTEXT("Canceled", "Canceled");
		case EView::NotSent: return LOCTEXT("NotSent", "Not sent");
		case EView::Hidden: return LOCTEXT("Hidden", "Hidden");
		}
		return FText::GetEmpty();
	}

	FSlateColor ChipColor(const FScenarioJobRow& Row)
	{
		switch (View(Row))
		{
		case EView::Running:
		case EView::Downloading:
		case EView::Importing: return ScenarioUI::Palette::Info();
		case EView::Ready: return ScenarioUI::Palette::Success();
		case EView::DownloadStopped:
		case EView::Unknown: return ScenarioUI::Palette::Warning();
		case EView::Failed:
		case EView::Blocked: return ScenarioUI::Palette::Error();
		default: return ScenarioUI::Palette::Dim();
		}
	}

	TSharedRef<SWidget> StateChip(const FString& LocalId)
	{
		return ScenarioUI::Chip(
			MakeAttributeLambda([LocalId]() { const FScenarioJobRow* Row = FindRow(LocalId); return Row ? ChipText(*Row) : FText::GetEmpty(); }),
			MakeAttributeLambda([LocalId]() { const FScenarioJobRow* Row = FindRow(LocalId); return Row ? ChipColor(*Row) : FSlateColor(ScenarioUI::Palette::Dim()); }),
			MakeAttributeLambda([LocalId]() { const FScenarioJobRow* Row = FindRow(LocalId); return Row ? StateText(*Row) : FText::GetEmpty(); }));
	}

	FText CostText(const FScenarioJobRow& Row)
	{
		if (Row.ChargedCU >= 0.0)
		{
			return FText::FromString(ScenarioCU::Format(Row.ChargedCU));
		}
		if (Row.QuotedCU > 0.0 && Row.State != EScenarioJobState::Rejected && Row.State != EScenarioJobState::Canceled)
		{
			return FText::FromString(TEXT("~") + ScenarioCU::Format(Row.QuotedCU));
		}
		return FText::GetEmpty();
	}

	FText CostTooltip(const FScenarioJobRow& Row)
	{
		if (Row.ChargedCU >= 0.0)
		{
			return FText::Format(LOCTEXT("Charged", "Scenario charged {0}."), FText::FromString(ScenarioCU::Format(Row.ChargedCU)));
		}
		if (Row.QuotedCU > 0.0)
		{
			return FText::Format(LOCTEXT("QuotedTip", "Quoted {0}, final charge pending."), FText::FromString(ScenarioCU::Format(Row.QuotedCU)));
		}
		return FText::GetEmpty();
	}

	FString ModelLabel(const FScenarioJobRow& Row)
	{
		if (Row.Kind == TEXT("prompt_spark"))
		{
			return TEXT("Improve prompt");
		}
		if (!Row.WorkflowId.IsEmpty() && Row.ModelId.IsEmpty())
		{
			return Row.ModelName.IsEmpty() ? TEXT("Workflow") : Row.ModelName;
		}
		return ScenarioUI::ModelName(Row.ModelId, Row.ModelName);
	}

	FString Title(const FScenarioJobRow& Row)
	{
		FString Prompt = Row.Prompt;
		int32 Break = INDEX_NONE;
		if (Prompt.FindChar(TEXT('\n'), Break))
		{
			Prompt.LeftInline(Break);
		}
		Prompt.TrimStartAndEndInline();
		return Prompt.IsEmpty() ? ModelLabel(Row) : ModelLabel(Row) + TEXT(" · ") + Prompt;
	}

	FString OutputKind(const FScenarioJobRow& Row)
	{
		for (const FScenarioJobFile& File : Row.Files)
		{
			if (!File.Kind.IsEmpty())
			{
				return File.Kind.StartsWith(TEXT("image")) ? TEXT("image") : File.Kind;
			}
		}
		if (Row.Lane == TEXT("3d") || Row.Lane == TEXT("video") || Row.Lane == TEXT("audio"))
		{
			return Row.Lane;
		}
		return TEXT("image");
	}

	FString DisplayKind(const FScenarioJobRow& Row)
	{
		if (Row.Lane == TEXT("material"))
		{
			return TEXT("material");
		}
		if (Row.Lane == TEXT("environment"))
		{
			return TEXT("sky");
		}
		return OutputKind(Row);
	}

	FString PreviewFile(const FScenarioJobRow& Row)
	{
		// Data maps never stand for the result: a normal map reads as a purple square.
		static const TArray<FString> DataRoles = { TEXT("normal"), TEXT("roughness"), TEXT("metallic"), TEXT("metalness"), TEXT("height"),
			TEXT("displacement"), TEXT("ao"), TEXT("occlusion"), TEXT("ambient_occlusion"), TEXT("orm"), TEXT("mask"), TEXT("opacity"), TEXT("specular") };
		static const TArray<FString> Preferred = { TEXT("preview"), TEXT("thumbnail"), TEXT("basecolor"), TEXT("base_color"), TEXT("albedo"), TEXT("diffuse"), TEXT("color") };
		FString Fallback;
		for (const FScenarioJobFile& File : Row.Files)
		{
			if (!File.Kind.StartsWith(TEXT("image")) || File.Kind == TEXT("image-hdr") || File.LocalPath.IsEmpty())
			{
				continue;
			}
			const FString Role = File.Role.ToLower();
			if (DataRoles.ContainsByPredicate([&Role](const FString& Data) { return Role.Contains(Data); }))
			{
				continue;
			}
			if (Role.IsEmpty() || Preferred.ContainsByPredicate([&Role](const FString& Good) { return Role.Contains(Good); }))
			{
				return File.LocalPath;
			}
			if (Fallback.IsEmpty())
			{
				Fallback = File.LocalPath;
			}
		}
		return Fallback;
	}

	FString PrimaryImportedObject(const FScenarioJobRow& Row)
	{
		if (!Row.bImported)
		{
			return FString();
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		static const TArray<FString> Order = { TEXT("StaticMesh"), TEXT("SkeletalMesh"), TEXT("MaterialInstanceConstant"), TEXT("Material"),
			TEXT("TextureCube"), TEXT("Texture2D"), TEXT("SoundWave"), TEXT("FileMediaSource") };
		FString Best;
		int32 BestRank = MAX_int32;
		for (const FScenarioJobFile& File : Row.Files)
		{
			for (const FString& Path : File.ImportedObjects)
			{
				const FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
				if (!Asset.IsValid())
				{
					continue;
				}
				const int32 Rank = Order.IndexOfByKey(Asset.AssetClassPath.GetAssetName().ToString());
				const int32 Effective = Rank == INDEX_NONE ? Order.Num() : Rank;
				if (Effective < BestRank)
				{
					Best = Path;
					BestRank = Effective;
				}
			}
		}
		return Best;
	}

	FRowAction PrimaryAction(const FScenarioJobRow& Row)
	{
		FRowAction Action;
		const FString Id = Row.LocalId;
		switch (View(Row))
		{
		case EView::Queued:
		case EView::Running:
			Action.Label = LOCTEXT("Cancel", "Cancel");
			Action.Icon = TEXT("close");
			Action.Tooltip = LOCTEXT("CancelTip", "Asks Scenario to stop this job. Scenario decides whether any Creative Units are refunded.");
			Action.Run = [Id]()
			{
				if (FScenarioSession* Session = SessionOrNull())
				{
					Session->CancelJob(Id);
				}
			};
			break;
		case EView::Ready:
			if (Row.Kind == TEXT("prompt_spark") || Row.AssetIds.Num() == 0)
			{
				break;
			}
			Action.Label = LOCTEXT("ImportPlace", "Import and place");
			Action.Icon = TEXT("pin");
			Action.Tooltip = LOCTEXT("ImportPlaceTip", "Imports the result and places it in the level (on the selection, or in front of the camera). One undo step.");
			Action.Run = [Id]() { Import(Id, true); };
			break;
		case EView::Imported:
			Action.Label = LOCTEXT("Show", "Show");
			Action.Icon = TEXT("folder");
			Action.Tooltip = LOCTEXT("ShowTip", "Selects the imported assets in the Content Browser.");
			Action.Run = [Id]() { Show(Id); };
			break;
		case EView::DownloadStopped:
			Action.Label = LOCTEXT("Resume", "Resume download");
			Action.Icon = TEXT("refresh");
			Action.Tooltip = LOCTEXT("ResumeTip", "Downloads the files again with a fresh link, resuming partial files. Free.");
			Action.Run = [Id]()
			{
				if (FScenarioSession* Session = SessionOrNull())
				{
					Session->DownloadFiles(Id);
				}
			};
			break;
		case EView::Failed:
		case EView::Blocked:
			Action.Label = LOCTEXT("Details", "Details");
			Action.Icon = TEXT("info");
			Action.Tooltip = LOCTEXT("DetailsTip", "Shows the inputs, ids and errors of this job.");
			Action.Run = [Id]() { ShowDetails(Id); };
			break;
		case EView::Unknown:
			Action.Label = LOCTEXT("Check", "Check");
			Action.Icon = TEXT("refresh");
			Action.Tooltip = LOCTEXT("CheckTip", "Looks for this request in Scenario's recent jobs and links it when there is exactly one match. Free.");
			Action.Run = []() { CheckUnresolved(); };
			break;
		default:
			break;
		}
		return Action;
	}

	void Import(const FString& LocalId, bool bPlace)
	{
		UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
		if (!Subsystem)
		{
			return;
		}
		FScenarioPlaceOptions Options;
		Options.Placement = bPlace ? EScenarioPlacement::Auto : EScenarioPlacement::None;
		Subsystem->ImportJob(LocalId, Options, [](const FScenarioChangeReport& Report)
		{
			if (!Report.bOk)
			{
				FScenarioEditorModule::Toast(LOCTEXT("ImportFailed", "Import failed"), FText::FromString(Report.Error), false);
			}
		});
	}

	void Show(const FString& LocalId)
	{
		const FScenarioJobRow* Row = FindRow(LocalId);
		if (!Row)
		{
			return;
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		for (const FScenarioJobFile& File : Row->Files)
		{
			for (const FString& Path : File.ImportedObjects)
			{
				const FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
				if (Asset.IsValid())
				{
					Assets.Add(Asset);
				}
			}
		}
		if (Assets.Num() == 0)
		{
			FScenarioEditorModule::Toast(LOCTEXT("NothingToShow", "The imported assets are gone"), LOCTEXT("NothingToShowSub", "They were moved or deleted. Import the result again from its menu."), false);
			return;
		}
		FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		ContentBrowser.Get().SyncBrowserToAssets(Assets);
	}

	void UseSettings(const FString& LocalId)
	{
		const FScenarioJobRow* Row = FindRow(LocalId);
		if (!Row)
		{
			return;
		}
		if (!Row->WorkflowId.IsEmpty() && Row->ModelId.IsEmpty())
		{
			const TSharedRef<FScenarioCreateModel> Create = FScenarioCreateModel::Get();
			Create->SetWorkflow(Row->WorkflowId, Row->ModelName, FString());
			Create->ApplyValues(Row->Parameters);
			FScenarioEditorModule::OpenStudio(ScenarioPages::Create);
			return;
		}
		FScenarioEditorModule::OpenCreate(Row->Lane, Row->ModelId, Row->Parameters);
	}

	void ShowDetails(const FString& LocalId)
	{
		const FScenarioJobRow* Row = FindRow(LocalId);
		if (!Row)
		{
			return;
		}
		const FString Text = ScenarioJson::Write(Row->ToJson(), true);
		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(FText::Format(LOCTEXT("DetailsTitle", "Scenario job: {0}"), FText::FromString(Title(*Row).Left(60))))
			.ClientSize(FVector2D(640, 520))
			.SupportsMinimize(false)
			.SupportsMaximize(false);
		Window->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(12.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SNew(STextBlock)
					.Text(StateText(*Row))
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SMultiLineEditableTextBox)
					.Text(FText::FromString(Text))
					.IsReadOnly(true)
					.AlwaysShowScrollbars(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Right)
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Copy", "Copy"))
					.ToolTipText(LOCTEXT("CopyTip", "Copies these details (ids, settings, errors; no credentials) for support."))
					.OnClicked_Lambda([Text]()
					{
						FPlatformApplicationMisc::ClipboardCopy(*Text);
						return FReply::Handled();
					})
				]
			]);
		FSlateApplication::Get().AddWindow(Window);
	}

	void CheckUnresolved()
	{
		FScenarioSession* Session = SessionOrNull();
		if (!Session)
		{
			return;
		}
		Session->CheckUnresolved([](int32 Linked, const FScenarioError& Error)
		{
			if (Error.Kind != EScenarioErrorKind::None)
			{
				FScenarioEditorModule::Toast(LOCTEXT("CheckFailed", "Could not check Scenario"), FText::FromString(Error.Message), false);
				return;
			}
			FScenarioEditorModule::Toast(Linked > 0
				? FText::Format(LOCTEXT("Linked", "Found {0} on Scenario"), Linked == 1 ? LOCTEXT("OneRequest", "1 request") : FText::Format(LOCTEXT("Requests", "{0} requests"), FText::AsNumber(Linked)))
				: LOCTEXT("NotFound", "Not found on Scenario yet"),
				Linked > 0 ? FText::GetEmpty() : LOCTEXT("NotFoundSub", "If it never shows up, mark it as not sent from its menu to unblock the same request."), Linked > 0);
		});
	}

	TSharedRef<SWidget> MoreMenu(const FString& LocalId)
	{
		FMenuBuilder Menu(true, nullptr);
		const FScenarioJobRow* Row = FindRow(LocalId);
		FScenarioSession* Session = SessionOrNull();
		if (!Row || !Session)
		{
			return Menu.MakeWidget();
		}
		const EView State = View(*Row);
		const bool bHasOutput = Row->State == EScenarioJobState::Success && Row->AssetIds.Num() > 0 && Row->Kind != TEXT("prompt_spark");
		auto Entry = [&Menu](const FText& Label, const FText& Tip, const FName Icon, TFunction<void()> Run)
		{
			Menu.AddMenuEntry(Label, Tip, Icon.IsNone() ? FSlateIcon() : FSlateIcon(FAppStyle::GetAppStyleSetName(), Icon), FUIAction(FExecuteAction::CreateLambda(MoveTemp(Run))));
		};

		if (State == EView::Ready && bHasOutput)
		{
			Entry(LOCTEXT("ImportOnly", "Import only"), LOCTEXT("ImportOnlyTip", "Imports the result into the Content Browser without placing it."), TEXT("Icons.Import"), [LocalId]() { Import(LocalId, false); });
		}
		if (State == EView::Imported)
		{
			Entry(LOCTEXT("PlaceAgain", "Place again"), LOCTEXT("PlaceAgainTip", "Places another copy of the imported result in the level. One undo step."), TEXT("Icons.PlaceActors"), [LocalId]() { Import(LocalId, true); });
		}
		if (bHasOutput)
		{
			const FString AssetId = Row->AssetIds[0];
			const FString Kind = OutputKind(*Row);
			const FString Label = Row->Prompt.Left(40);
			if (NextSteps(Kind).Num() > 0)
			{
				Menu.AddSubMenu(LOCTEXT("SendTo", "Send to"), LOCTEXT("SendToTip", "Opens Create with this result as the input of another model. Sends nothing."),
					FNewMenuDelegate::CreateLambda([AssetId, Kind, Label](FMenuBuilder& Sub) { AddNextStepsMenu(Sub, AssetId, Kind, Label); }));
			}
		}
		if (Row->Kind != TEXT("prompt_spark") && State != EView::Unknown && State != EView::Sending)
		{
			Entry(LOCTEXT("UseSettings", "Reuse settings"), LOCTEXT("UseSettingsTip", "Fills the Create form with this job's model and settings. Sends nothing."), TEXT("Icons.Edit"), [LocalId]() { UseSettings(LocalId); });
		}
		if (Row->State == EScenarioJobState::Success && !HasAllFiles(*Row) && State != EView::Downloading && State != EView::DownloadStopped)
		{
			Entry(LOCTEXT("Download", "Download again"), LOCTEXT("DownloadTip", "Downloads the files again with a fresh link, resuming partial files. Free."), TEXT("Icons.Refresh"), [LocalId]()
			{
				if (FScenarioSession* S = SessionOrNull())
				{
					S->DownloadFiles(LocalId);
				}
			});
		}
		if (State == EView::Unknown)
		{
			Entry(LOCTEXT("NotSentAction", "Mark as not sent"), LOCTEXT("NotSentTip", "You checked Scenario and this request is not there. Unblocks sending the same request again."), NAME_None, [LocalId]()
			{
				if (FScenarioSession* S = SessionOrNull())
				{
					S->MarkNotSent(LocalId);
				}
			});
		}
		if (State == EView::Imported)
		{
			Entry(LOCTEXT("RemoveImported", "Delete imported assets"), LOCTEXT("RemoveImportedTip", "Deletes the assets imported from this job, if nothing in the project uses them."), TEXT("Icons.Delete"), [LocalId]()
			{
				if (UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get())
				{
					const FScenarioChangeReport Report = Subsystem->RemoveImported(LocalId);
					FScenarioEditorModule::Toast(Report.bOk ? LOCTEXT("Removed", "Imported assets deleted") : LOCTEXT("NotRemoved", "Nothing deleted"),
						FText::FromString(Report.bOk ? Report.Summary() : Report.Error), Report.bOk);
				}
			});
		}
		Menu.AddSeparator();
		Entry(LOCTEXT("DetailsEntry", "Details"), LOCTEXT("DetailsEntryTip", "Shows the inputs, ids and errors of this job."), TEXT("Icons.Info"), [LocalId]() { ShowDetails(LocalId); });
		if (Row->IsFinal() || Row->State == EScenarioJobState::Rejected)
		{
			Entry(LOCTEXT("Dismiss", "Hide from list"), LOCTEXT("DismissTip", "Hides this job from the lists. Imported assets stay."), TEXT("Icons.Hidden"), [LocalId]()
			{
				if (FScenarioSession* S = SessionOrNull())
				{
					S->Dismiss(LocalId);
				}
			});
		}
		return Menu.MakeWidget();
	}
}

// Thumbnail

void SScenarioJobThumb::Construct(const FArguments& InArgs)
{
	LocalId = InArgs._LocalId;
	Size = InArgs._Size;
	const FString Id = LocalId;
	auto Preview = [Id]() -> const FSlateBrush*
	{
		const FScenarioJobRow* Row = FindRow(Id);
		const FString File = Row ? ScenarioJobActions::PreviewFile(*Row) : FString();
		return File.IsEmpty() ? nullptr : FScenarioThumbnails::Get().ForFile(File);
	};
	auto Kind = [Id]() { const FScenarioJobRow* Row = FindRow(Id); return Row ? ScenarioJobActions::DisplayKind(*Row) : FString(TEXT("image")); };
	const float Badge = Size >= 48.0f ? 16.0f : 12.0f;
	const bool bBadge = Size >= 40.0f;

	ChildSlot
	[
		SNew(SBox)
		.WidthOverride(Size)
		.HeightOverride(Size)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				ScenarioUI::Thumbnail(Size, MakeAttributeLambda(Preview), MakeAttributeLambda(Kind))
			]
			+ SOverlay::Slot()
			[
				SAssignNew(AssetBox, SBox)
			]
			// The kind, when a picture stands in for something else (a mesh's base color).
			+ SOverlay::Slot()
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Bottom)
			.Padding(3.0f)
			[
				SNew(SBorder)
				.BorderImage(ScenarioUI::Style().GetBrush("Scenario.Overlay"))
				.Padding(2.0f)
				.Visibility_Lambda([Kind, Preview, bBadge]()
				{
					return bBadge && Kind() != TEXT("image") && Preview() ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
				})
				[
					SNew(SImage)
					.DesiredSizeOverride(FVector2D(Badge - 4.0f, Badge - 4.0f))
					.Image_Lambda([Kind]() { return ScenarioUI::KindIcon(Kind()); })
				]
			]
			+ SOverlay::Slot()
			.VAlign(VAlign_Bottom)
			[
				InArgs._bProgress ? ThinProgress(Id) : SNullWidget::NullWidget
			]
		]
	];
	Rebuild();
}

void SScenarioJobThumb::Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime)
{
	SCompoundWidget::Tick(Geometry, CurrentTime, DeltaTime);
	const FScenarioJobRow* Row = FindRow(LocalId);
	// Cheap while nothing is imported: the registry is only asked once the row says so.
	const bool bImported = Row && Row->bImported;
	if (bImported == !ShownObject.IsEmpty() && !(ShownObject == TEXT("none") && CurrentTime > RetryAt))
	{
		return;
	}
	RetryAt = CurrentTime + 3.0;
	Rebuild();
}

void SScenarioJobThumb::Rebuild()
{
	const FScenarioJobRow* Row = FindRow(LocalId);
	const FString Object = Row ? ScenarioJobActions::PrimaryImportedObject(*Row) : FString();
	const TSharedPtr<FAssetThumbnailPool> Pool = FScenarioThumbnails::Get().AssetPool();
	if (Object.IsEmpty() || !Pool.IsValid())
	{
		ShownObject.Reset();
		AssetThumbnail.Reset();
		AssetBox->SetContent(SNullWidget::NullWidget);
		// A row that says imported but whose assets are gone keeps the preview; check again later.
		ShownObject = Row && Row->bImported ? TEXT("none") : FString();
		return;
	}
	ShownObject = Object;
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(Object));
	if (!Asset.IsAssetLoaded())
	{
		// The downloaded preview stays until the asset is in memory and Unreal can draw it.
		AssetThumbnail.Reset();
		AssetBox->SetContent(SNullWidget::NullWidget);
		const TWeakPtr<SScenarioJobThumb> Weak = SharedThis(this);
		FScenarioThumbnails::Get().LoadForThumbnail(Asset, [Weak]()
		{
			if (const TSharedPtr<SScenarioJobThumb> This = Weak.Pin())
			{
				This->Rebuild();
			}
		});
		return;
	}
	const uint32 Pixels = static_cast<uint32>(FMath::Max(64.0f, Size * 2.0f));
	AssetThumbnail = MakeShared<FAssetThumbnail>(Asset, Pixels, Pixels, Pool);
	FAssetThumbnailConfig Config;
	Config.bAllowFadeIn = false;
	Config.bAllowHintText = false;
	Config.ShowAssetColor = false;
	AssetBox->SetContent(AssetThumbnail->MakeThumbnailWidget(Config));
}

// Jobs row

void SScenarioJobRow::Construct(const FArguments& InArgs)
{
	LocalId = InArgs._LocalId;
	const FString Id = LocalId;
	auto RowText = [Id](TFunction<FText(const FScenarioJobRow&)> Get)
	{
		return MakeAttributeLambda([Id, Get]() { const FScenarioJobRow* Row = FindRow(Id); return Row ? Get(*Row) : FText::GetEmpty(); });
	};

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 7.0f, 6.0f, 5.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[
				SNew(SScenarioJobThumb).LocalId(Id).Size(44.0f)
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
						ScenarioUI::Line(RowText([](const FScenarioJobRow& Row) { return FText::FromString(ScenarioJobActions::Title(Row)); }))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
					[
						ScenarioJobActions::StateChip(Id)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SBox)
						.MinDesiredWidth(52.0f)
						.HAlign(HAlign_Right)
						[
							SNew(STextBlock)
							.Text(RowText([](const FScenarioJobRow& Row) { return ScenarioJobActions::CostText(Row); }))
							.ToolTipText(RowText([](const FScenarioJobRow& Row) { return ScenarioJobActions::CostTooltip(Row); }))
							.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
							.ColorAndOpacity(FSlateColor(ScenarioUI::Palette::Dim()))
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						ScenarioUI::Line(RowText([](const FScenarioJobRow& Row)
						{
							const FText When = ScenarioUI::RelativeTime(Row.CreatedAt);
							if (Row.Source == TEXT("agent"))
							{
								return FText::Format(LOCTEXT("ByAgent", "{0} · {1}"), When, FText::FromString(Row.Client.IsEmpty() ? TEXT("an agent") : Row.Client));
							}
							if (Row.Source == TEXT("script"))
							{
								return FText::Format(LOCTEXT("ByScript", "{0} · a script"), When);
							}
							return When;
						}), ScenarioUI::EText::Meta, true)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(ScenarioUI::Gutter, 0.0f, 2.0f, 0.0f)
					[
						RowActionButton(Id)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						OverflowButton(Id)
					]
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 0.0f, 8.0f, 2.0f))
		[
			ThinProgress(Id)
		]
	];
}

// Recent tile

void SScenarioJobTile::Construct(const FArguments& InArgs)
{
	LocalId = InArgs._LocalId;
	const FString Id = LocalId;
	auto Action = [Id]()
	{
		const FScenarioJobRow* Row = FindRow(Id);
		return Row ? ScenarioJobActions::PrimaryAction(*Row) : ScenarioJobActions::FRowAction();
	};
	constexpr float Size = 56.0f;

	SetToolTipText(MakeAttributeLambda([Id]()
	{
		const FScenarioJobRow* Row = FindRow(Id);
		if (!Row)
		{
			return FText::GetEmpty();
		}
		const FText Cost = ScenarioJobActions::CostText(*Row);
		return FText::Format(LOCTEXT("TileTip", "{0}\n{1}{2}{3}"), FText::FromString(ScenarioJobActions::Title(*Row)), ScenarioJobActions::StateText(*Row),
			Cost.IsEmpty() ? FText::GetEmpty() : FText::FromString(TEXT(" · ")), Cost);
	}));

	ChildSlot
	[
		SNew(SBox)
		.WidthOverride(Size)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SScenarioJobThumb).LocalId(Id).Size(Size).bProgress(true)
				]
				// On hover: the state's action and the overflow menu.
				+ SOverlay::Slot()
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Top)
				[
					SNew(SBorder)
					.BorderImage(ScenarioUI::Style().GetBrush("Scenario.Overlay"))
					.Padding(1.0f)
					.Visibility_Lambda([this]() { return IsHovered() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>(ScenarioUI::ButtonStyleName(ScenarioUI::EButton::Ghost, true)))
							.ContentPadding(FMargin(2.0f))
							.Visibility_Lambda([Action]() { return Action().IsSet() && !Action().Icon.IsNone() ? EVisibility::Visible : EVisibility::Collapsed; })
							.ToolTipText_Lambda([Action]() { return FText::Format(LOCTEXT("TileAction", "{0}. {1}"), Action().Label, Action().Tooltip); })
							.OnClicked_Lambda([Action]()
							{
								const ScenarioJobActions::FRowAction Now = Action();
								if (Now.IsSet())
								{
									Now.Run();
								}
								return FReply::Handled();
							})
							[
								SNew(SImage)
								.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
								.Image_Lambda([Action]() { const FName Icon = Action().Icon; return Icon.IsNone() ? nullptr : ScenarioUI::IconBrush(Icon); })
								.ColorAndOpacity(FSlateColor::UseForeground())
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							OverflowButton(Id)
						]
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([Id]() { const FScenarioJobRow* Row = FindRow(Id); return Row ? ScenarioJobActions::ChipText(*Row) : FText::GetEmpty(); })
				.ColorAndOpacity_Lambda([Id]()
				{
					const FScenarioJobRow* Row = FindRow(Id);
					if (!Row)
					{
						return FSlateColor(ScenarioUI::Palette::Dim());
					}
					const ScenarioJobActions::EView View = ScenarioJobActions::View(*Row);
					const bool bNeutral = View == ScenarioJobActions::EView::Imported || View == ScenarioJobActions::EView::Sending || View == ScenarioJobActions::EView::Queued
						|| View == ScenarioJobActions::EView::Canceled || View == ScenarioJobActions::EView::NotSent;
					return bNeutral ? FSlateColor(ScenarioUI::Palette::Dim()) : ScenarioJobActions::ChipColor(*Row);
				})
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		]
	];
}

FReply SScenarioJobTile::OnMouseButtonDoubleClick(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	if (const FScenarioJobRow* Row = FindRow(LocalId))
	{
		const ScenarioJobActions::FRowAction Action = ScenarioJobActions::PrimaryAction(*Row);
		if (Action.IsSet())
		{
			Action.Run();
			return FReply::Handled();
		}
	}
	return SCompoundWidget::OnMouseButtonDoubleClick(Geometry, MouseEvent);
}

// Composer line

void SScenarioJobCard::Construct(const FArguments& InArgs)
{
	LocalId = InArgs._LocalId;
	const FString Id = LocalId;
	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SScenarioJobThumb).LocalId(Id).Size(28.0f).bProgress(true)
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			ScenarioUI::Line(MakeAttributeLambda([Id]() { const FScenarioJobRow* Row = FindRow(Id); return Row ? FText::FromString(ScenarioJobActions::Title(*Row)) : FText::GetEmpty(); }))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f)
		[
			ScenarioJobActions::StateChip(Id)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			RowActionButton(Id)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			OverflowButton(Id)
		]
	];
}

#undef LOCTEXT_NAMESPACE
