// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioJobCard.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "ScenarioCreateModel.h"
#include "ScenarioCU.h"
#include "ScenarioEditorModule.h"
#include "ScenarioJson.h"
#include "ScenarioStyle.h"
#include "ScenarioSubsystem.h"
#include "ScenarioThumbnails.h"
#include "SScenarioModelPicker.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
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
			Lane(LOCTEXT("Edit3D", "Edit"), LOCTEXT("Edit3DTip", "Opens the 3D lane in Edit mode with this mesh as the input."), TEXT("3d"), TEXT("edit"));
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
			Lane(LOCTEXT("EditImage", "Edit"), LOCTEXT("EditImageTip", "Opens the Image lane with this image as the reference."), TEXT("image"), TEXT(""));
			Lane(LOCTEXT("Make3D", "Make 3D"), LOCTEXT("Make3DTip", "Opens the 3D lane in Image mode with this image as the input."), TEXT("3d"), TEXT("image"));
			Lane(LOCTEXT("MakeMaterial", "Make material"), LOCTEXT("MakeMaterialTip", "Opens the Material lane with this image as the reference."), TEXT("material"), TEXT(""));
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

	FText StateText(const FScenarioJobRow& Row)
	{
		FScenarioSession* Session = SessionOrNull();
		switch (Row.State)
		{
		case EScenarioJobState::Submitting: return LOCTEXT("Submitting", "Sending...");
		case EScenarioJobState::Queued: return LOCTEXT("Queued", "Queued");
		case EScenarioJobState::Running:
			return Row.Progress > 0.0
				? FText::Format(LOCTEXT("Generating", "Generating {0}%"), FText::AsNumber(FMath::RoundToInt(Row.Progress * 100.0)))
				: LOCTEXT("GeneratingNoProgress", "Generating");
		case EScenarioJobState::Success:
		{
			if (Session && Session->IsDownloading(Row.LocalId))
			{
				return FText::Format(LOCTEXT("Downloading", "Downloading {0}%"), FText::AsNumber(FMath::RoundToInt(FMath::Max(0.0f, Session->DownloadProgress(Row.LocalId)) * 100.0f)));
			}
			if (UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get())
			{
				if (Subsystem->IsImporting(Row.LocalId))
				{
					return LOCTEXT("Importing", "Importing...");
				}
			}
			if (Row.bImported)
			{
				return LOCTEXT("Imported", "Imported");
			}
			if (!Row.Error.IsEmpty())
			{
				return FText::FromString(Row.Error);
			}
			return LOCTEXT("Ready", "Ready");
		}
		case EScenarioJobState::Failed: return FText::Format(LOCTEXT("Failed", "Failed: {0}"), FText::FromString(Row.Error));
		case EScenarioJobState::Blocked: return FText::Format(LOCTEXT("BlockedState", "Blocked by Scenario's content rules. {0}"), FText::FromString(Row.Error));
		case EScenarioJobState::CancelRequested: return LOCTEXT("Cancelling", "Cancelling...");
		case EScenarioJobState::Canceled: return LOCTEXT("Canceled", "Canceled");
		case EScenarioJobState::Unknown: return LOCTEXT("Unknown", "We do not know if Scenario received this request.");
		case EScenarioJobState::Rejected: return FText::Format(LOCTEXT("Rejected", "Not sent: {0}"), FText::FromString(Row.Error));
		case EScenarioJobState::Dismissed: return LOCTEXT("Dismissed", "Dismissed");
		}
		return FText::GetEmpty();
	}

	FSlateColor StateColor(const FScenarioJobRow& Row)
	{
		switch (Row.State)
		{
		case EScenarioJobState::Success: return Row.Error.IsEmpty() ? FScenarioStyle::Success() : FScenarioStyle::Warning();
		case EScenarioJobState::Failed:
		case EScenarioJobState::Blocked:
		case EScenarioJobState::Rejected: return FScenarioStyle::Error();
		case EScenarioJobState::Unknown: return FScenarioStyle::Warning();
		default: return FSlateColor::UseForeground();
		}
	}

	FText CostText(const FScenarioJobRow& Row)
	{
		if (Row.ChargedCU >= 0.0)
		{
			return FText::FromString(ScenarioCU::Format(Row.ChargedCU));
		}
		if (Row.QuotedCU > 0.0)
		{
			return FText::Format(LOCTEXT("Quoted", "{0} quoted"), FText::FromString(ScenarioCU::Format(Row.QuotedCU)));
		}
		return FText::GetEmpty();
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

	FString PreviewFile(const FScenarioJobRow& Row)
	{
		for (const FScenarioJobFile& File : Row.Files)
		{
			if (File.Kind.StartsWith(TEXT("image")) && !File.LocalPath.IsEmpty())
			{
				return File.LocalPath;
			}
		}
		return FString();
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

	void UseSettings(const FString& LocalId)
	{
		const FScenarioJobRow* Row = FindRow(LocalId);
		if (!Row)
		{
			return;
		}
		if (!Row->WorkflowId.IsEmpty())
		{
			FScenarioEditorModule::OpenStudio(ScenarioPages::Workflows);
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
			.Title(FText::Format(LOCTEXT("DetailsTitle", "Scenario job {0}"), FText::FromString(Row->JobId.IsEmpty() ? Row->LocalId : Row->JobId)))
			.ClientSize(FVector2D(640, 520))
			.SupportsMinimize(false)
			.SupportsMaximize(false);
		Window->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(12.0f)
			[
				SNew(SVerticalBox)
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

	TSharedRef<SWidget> MoreMenu(const FString& LocalId)
	{
		FMenuBuilder Menu(true, nullptr);
		const FScenarioJobRow* Row = FindRow(LocalId);
		FScenarioSession* Session = SessionOrNull();
		if (!Row || !Session)
		{
			return Menu.MakeWidget();
		}
		if (Row->State == EScenarioJobState::Success && Row->AssetIds.Num() > 0)
		{
			Menu.BeginSection(NAME_None, LOCTEXT("Next", "Next"));
			AddNextStepsMenu(Menu, Row->AssetIds[0], OutputKind(*Row), Row->Prompt.Left(40));
			Menu.EndSection();
		}
		Menu.BeginSection(NAME_None, LOCTEXT("Job", "Job"));
		if (Row->Kind != TEXT("prompt_spark"))
		{
			Menu.AddMenuEntry(LOCTEXT("UseSettings", "Use settings"), LOCTEXT("UseSettingsTip", "Fills the Create form with this job's model and settings. Sends nothing."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([LocalId]() { UseSettings(LocalId); })));
		}
		if (Row->State == EScenarioJobState::Success && !HasAllFiles(*Row))
		{
			Menu.AddMenuEntry(LOCTEXT("Download", "Download again"), LOCTEXT("DownloadTip", "Downloads the files again with a fresh link, resuming partial files."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([LocalId]()
				{
					if (FScenarioSession* S = SessionOrNull())
					{
						S->DownloadFiles(LocalId);
					}
				})));
		}
		if (Row->bImported)
		{
			Menu.AddMenuEntry(LOCTEXT("RemoveImported", "Delete imported assets"), LOCTEXT("RemoveImportedTip", "Deletes the assets imported from this job, if nothing in the project uses them."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([LocalId]()
				{
					if (UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get())
					{
						const FScenarioChangeReport Report = Subsystem->RemoveImported(LocalId);
						FScenarioEditorModule::Toast(Report.bOk ? LOCTEXT("Removed", "Imported assets deleted") : LOCTEXT("NotRemoved", "Nothing deleted"),
							FText::FromString(Report.bOk ? Report.Summary() : Report.Error), Report.bOk);
					}
				})));
		}
		Menu.AddMenuEntry(LOCTEXT("Details", "Details"), LOCTEXT("DetailsTip", "Shows the inputs, ids and errors of this job."), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([LocalId]() { ShowDetails(LocalId); })));
		if (Row->IsFinal() || Row->State == EScenarioJobState::Rejected)
		{
			Menu.AddMenuEntry(LOCTEXT("Dismiss", "Dismiss"), LOCTEXT("DismissTip", "Hides this job from the lists. Imported assets stay."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([LocalId]()
				{
					if (FScenarioSession* S = SessionOrNull())
					{
						S->Dismiss(LocalId);
					}
				})));
		}
		Menu.EndSection();
		return Menu.MakeWidget();
	}
}

void SScenarioJobCard::Construct(const FArguments& InArgs)
{
	LocalId = InArgs._LocalId;
	bCompact = InArgs._bCompact;

	const float Thumb = bCompact ? 28.0f : 64.0f;
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush(bCompact ? "NoBorder" : "ToolPanel.DarkGroupBorder"))
		.Padding(bCompact ? 2.0f : 8.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(Thumb)
				.HeightOverride(Thumb)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(this, &SScenarioJobCard::PreviewLabel)
							.Font(FScenarioStyle::Font(bCompact ? 7 : 9, true))
							.ColorAndOpacity(FScenarioStyle::Muted())
						]
					]
					+ SOverlay::Slot()
					[
						SNew(SImage)
						.Image(this, &SScenarioJobCard::PreviewBrush)
						.Visibility_Lambda([this]() { return PreviewBrush() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
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
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							const FScenarioJobRow* R = Row();
							return R ? FText::FromString(R->ModelName.IsEmpty() ? (R->ModelId.IsEmpty() ? R->WorkflowId : R->ModelId) : R->ModelName) : FText::GetEmpty();
						})
						.Font(FScenarioStyle::Font(9, true))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { const FScenarioJobRow* R = Row(); return R ? ScenarioJobActions::StateText(*R) : FText::GetEmpty(); })
						.ColorAndOpacity_Lambda([this]() { const FScenarioJobRow* R = Row(); return R ? ScenarioJobActions::StateColor(*R) : FSlateColor::UseForeground(); })
						.AutoWrapText(!bCompact)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { const FScenarioJobRow* R = Row(); return R ? ScenarioJobActions::CostText(*R) : FText::GetEmpty(); })
						.ColorAndOpacity(FScenarioStyle::Muted())
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f)
				[
					SNew(SProgressBar)
					.Percent_Lambda([this]() -> TOptional<float>
					{
						const FScenarioJobRow* R = Row();
						FScenarioSession* Session = SessionOrNull();
						if (R && Session && Session->IsDownloading(LocalId))
						{
							return Session->DownloadProgress(LocalId);
						}
						return R && R->Progress > 0.0 ? TOptional<float>(static_cast<float>(R->Progress)) : TOptional<float>();
					})
					.Visibility_Lambda([this]()
					{
						const FScenarioJobRow* R = Row();
						FScenarioSession* Session = SessionOrNull();
						const bool bBusy = R && (R->IsActive() || (Session && Session->IsDownloading(LocalId)));
						return bBusy ? EVisibility::Visible : EVisibility::Collapsed;
					})
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { const FScenarioJobRow* R = Row(); return R ? FText::FromString(R->Prompt) : FText::GetEmpty(); })
					.ColorAndOpacity(FScenarioStyle::Muted())
					.Visibility(bCompact ? EVisibility::Collapsed : EVisibility::Visible)
					.ToolTipText_Lambda([this]() { const FScenarioJobRow* R = Row(); return R ? FText::FromString(R->Prompt) : FText::GetEmpty(); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, bCompact ? 0.0f : 6.0f, 0.0f, 0.0f)
				[
					BuildActions()
				]
			]
		]
	];
}

const FScenarioJobRow* SScenarioJobCard::Row() const
{
	return FindRow(LocalId);
}

const FSlateBrush* SScenarioJobCard::PreviewBrush() const
{
	const FScenarioJobRow* R = Row();
	const FString File = R ? ScenarioJobActions::PreviewFile(*R) : FString();
	return File.IsEmpty() ? nullptr : FScenarioThumbnails::Get().ForFile(File);
}

FText SScenarioJobCard::PreviewLabel() const
{
	const FScenarioJobRow* R = Row();
	if (!R)
	{
		return FText::GetEmpty();
	}
	const FString Kind = ScenarioJobActions::OutputKind(*R);
	if (Kind == TEXT("3d"))
	{
		return LOCTEXT("Kind3D", "3D");
	}
	if (Kind == TEXT("audio"))
	{
		return LOCTEXT("KindAudio", "Audio");
	}
	if (Kind == TEXT("video"))
	{
		return LOCTEXT("KindVideo", "Video");
	}
	return R->Kind == TEXT("prompt_spark") ? LOCTEXT("KindText", "Text") : LOCTEXT("KindImage", "Image");
}

EVisibility SScenarioJobCard::ShowWhen(TFunction<bool(const FScenarioJobRow&)> Test) const
{
	const FScenarioJobRow* R = Row();
	return R && Test(*R) ? EVisibility::Visible : EVisibility::Collapsed;
}

TSharedRef<SWidget> SScenarioJobCard::BuildActions()
{
	const FString Id = LocalId;
	auto Button = [this](const FText& Label, const FText& Tip, TFunction<void()> Action, TFunction<bool(const FScenarioJobRow&)> When, bool bPrimary = false) -> TSharedRef<SWidget>
	{
		return SNew(SBox)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			.Visibility_Lambda([this, When]() { return ShowWhen(When); })
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), bPrimary ? "PrimaryButton" : "Button")
				.Text(Label)
				.ToolTipText(Tip)
				.OnClicked_Lambda([Action]() { Action(); return FReply::Handled(); })
			];
	};
	auto Ready = [](const FScenarioJobRow& R)
	{
		UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
		FScenarioSession* Session = SessionOrNull();
		return R.State == EScenarioJobState::Success && R.Kind != TEXT("prompt_spark") && R.AssetIds.Num() > 0
			&& !(Subsystem && Subsystem->IsImporting(R.LocalId)) && !(Session && Session->IsDownloading(R.LocalId));
	};

	TSharedRef<SWrapBox> Box = SNew(SWrapBox).UseAllottedSize(true);
	Box->AddSlot()[Button(LOCTEXT("ImportPlace", "Import and place"),
		LOCTEXT("ImportPlaceTip", "Imports the result and places it in the level (on the selection, or in front of the camera). One undo step."),
		[Id]() { ScenarioJobActions::Import(Id, true); }, Ready, true)];
	Box->AddSlot()[Button(LOCTEXT("Import", "Import"),
		LOCTEXT("ImportTip", "Imports the result into the Content Browser only."),
		[Id]() { ScenarioJobActions::Import(Id, false); }, [Ready](const FScenarioJobRow& R) { return Ready(R) && !R.bImported; })];
	Box->AddSlot()[Button(LOCTEXT("Cancel", "Cancel"),
		LOCTEXT("CancelTip", "Asks Scenario to stop this job. Scenario decides whether any Creative Units are refunded."),
		[Id]()
		{
			if (FScenarioSession* Session = SessionOrNull())
			{
				Session->CancelJob(Id);
			}
		}, [](const FScenarioJobRow& R) { return R.State == EScenarioJobState::Queued || R.State == EScenarioJobState::Running; })];
	Box->AddSlot()[Button(LOCTEXT("Check", "Check Scenario"),
		LOCTEXT("CheckTip", "Looks for this request in Scenario's recent jobs and links it when there is exactly one match. Free."),
		[]()
		{
			if (FScenarioSession* Session = SessionOrNull())
			{
				Session->CheckUnresolved([](int32 Linked, const FScenarioError& Error)
				{
					FScenarioEditorModule::Toast(Error.Kind == EScenarioErrorKind::None
						? FText::Format(LOCTEXT("Linked", "{0} request(s) found on Scenario"), FText::AsNumber(Linked))
						: LOCTEXT("CheckFailed", "Could not check Scenario"), FText::FromString(Error.Message), Error.Kind == EScenarioErrorKind::None);
				});
			}
		}, [](const FScenarioJobRow& R) { return R.State == EScenarioJobState::Unknown; })];
	Box->AddSlot()[Button(LOCTEXT("NotSent", "Mark as not sent"),
		LOCTEXT("NotSentTip", "You checked Scenario and this request is not there. Unblocks sending the same request again."),
		[Id]()
		{
			if (FScenarioSession* Session = SessionOrNull())
			{
				Session->MarkNotSent(Id);
			}
		}, [](const FScenarioJobRow& R) { return R.State == EScenarioJobState::Unknown; })];
	Box->AddSlot()[Button(LOCTEXT("Resume", "Resume"),
		LOCTEXT("ResumeTip", "Downloads the files again with a fresh link, resuming partial files."),
		[Id]()
		{
			if (FScenarioSession* Session = SessionOrNull())
			{
				Session->DownloadFiles(Id);
			}
		}, [](const FScenarioJobRow& R)
		{
			FScenarioSession* Session = SessionOrNull();
			return R.State == EScenarioJobState::Success && !R.Error.IsEmpty() && !(Session && Session->IsDownloading(R.LocalId));
		})];
	Box->AddSlot()
	[
		SNew(SComboButton)
		.HasDownArrow(true)
		.ButtonContent()
		[
			SNew(STextBlock).Text(LOCTEXT("More", "More"))
		]
		.ToolTipText(LOCTEXT("MoreTip", "Next steps, settings, details and dismiss."))
		.OnGetMenuContent_Lambda([Id]() { return ScenarioJobActions::MoreMenu(Id); })
	];
	return Box;
}

#undef LOCTEXT_NAMESPACE
