// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioSubsystem.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EditorFramework/AssetImportData.h"
#include "FileMediaSource.h"
#include "Framework/Application/SlateApplication.h"
#include "Exporters/GLTFExporter.h"
#include "Framework/Notifications/NotificationManager.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Options/GLTFExportOptions.h"
#include "ScenarioAssets.h"
#include "ScenarioCoreModule.h"
#include "ScenarioCU.h"
#include "ScenarioEditorModule.h"
#include "ScenarioFiles.h"
#include "ScenarioJson.h"
#include "ScenarioLanes.h"
#include "ScenarioProvenance.h"
#include "ScenarioSettings.h"
#include "ScenarioTelemetry.h"
#include "UI/ScenarioStyle.h"
#include "UI/SScenarioToast.h"
#include "ScopedTransaction.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "TextureResource.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "Scenario"

namespace
{
	const FName ImagePlaneTag(TEXT("ScenarioImagePlane"));

	/** The importer a job needs: the lane's, or one inferred from its files. */
	FString ImporterFor(const FScenarioJobRow& Row)
	{
		if (const FScenarioLane* Lane = FScenarioLanes::Get().Find(Row.Lane))
		{
			return Lane->Importer;
		}
		int32 Roles = 0;
		for (const FScenarioJobFile& File : Row.Files)
		{
			if (File.Kind == TEXT("3d"))
			{
				return TEXT("mesh");
			}
			if (File.Kind == TEXT("video"))
			{
				return TEXT("media");
			}
			if (File.Kind == TEXT("audio"))
			{
				return TEXT("sound");
			}
			if (File.Kind == TEXT("image-hdr") || File.Role == TEXT("panorama"))
			{
				return TEXT("sky");
			}
			Roles += File.Role == TEXT("normal") || File.Role == TEXT("roughness") ? 1 : 0;
		}
		return Roles > 0 ? TEXT("material") : TEXT("texture");
	}

	FString PrefixFor(const FString& Importer, const FScenarioJobFile& File)
	{
		if (File.Kind == TEXT("3d"))
		{
			return TEXT("SM");
		}
		if (File.Kind == TEXT("audio"))
		{
			return TEXT("SW");
		}
		if (File.Kind == TEXT("video"))
		{
			return TEXT("FMS");
		}
		return TEXT("T");
	}

	/** The role a file gets in this importer (sky files are panoramas). */
	FString RoleIn(const FString& Importer, const FScenarioJobFile& File)
	{
		if (Importer == TEXT("sky"))
		{
			return TEXT("panorama");
		}
		return File.Role.IsEmpty() ? TEXT("main") : File.Role;
	}

	FName ParameterForRole(const FString& Role)
	{
		if (Role == TEXT("normal")) return TEXT("Normal");
		if (Role == TEXT("roughness")) return TEXT("Roughness");
		if (Role == TEXT("metallic")) return TEXT("Metallic");
		if (Role == TEXT("ao")) return TEXT("AmbientOcclusion");
		if (Role == TEXT("height")) return NAME_None;
		return TEXT("BaseColor");
	}

	/** What a job's import produced, sorted by use. */
	struct FImportedSet
	{
		TArray<UObject*> All;
		UObject* Mesh = nullptr;
		UTexture* Texture = nullptr;
		UTextureCube* Cube = nullptr;
		USoundBase* Sound = nullptr;
		UObject* Media = nullptr;
		UMaterialInterface* Material = nullptr;
		UMaterialInterface* SkyMaterial = nullptr;
		float Aspect = 1.0f;

		void Classify()
		{
			for (UObject* Object : All)
			{
				if (!Mesh && (Object->IsA<UStaticMesh>() || Object->IsA<USkeletalMesh>()))
				{
					Mesh = Object;
				}
				else if (UTextureCube* AsCube = Cast<UTextureCube>(Object))
				{
					Cube = Cube ? Cube : AsCube;
				}
				else if (UTexture2D* AsTexture = Cast<UTexture2D>(Object))
				{
					if (!Texture)
					{
						Texture = AsTexture;
						const float Height = AsTexture->GetSurfaceHeight();
						Aspect = Height > 0.0f ? AsTexture->GetSurfaceWidth() / Height : 1.0f;
					}
				}
				else if (USoundBase* AsSound = Cast<USoundBase>(Object))
				{
					Sound = Sound ? Sound : AsSound;
				}
				else if (Object->GetClass()->GetName() == TEXT("FileMediaSource"))
				{
					Media = Media ? Media : Object;
				}
				else if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Object))
				{
					const FString Parent = Instance->Parent ? Instance->Parent->GetName() : FString();
					if (Parent == TEXT("M_Scenario_SkySphere"))
					{
						SkyMaterial = Instance;
					}
					else if (Parent == TEXT("M_Scenario_PBR") && !Material)
					{
						Material = Instance;
					}
				}
			}
		}
	};

	FString LabelFor(const FScenarioJobRow& Row)
	{
		const FString Slug = ScenarioFiles::PascalSlug(Row.Prompt.IsEmpty() ? Row.ModelName : Row.Prompt, 4);
		return TEXT("Scenario_") + (Slug.IsEmpty() ? FString(TEXT("Result")) : Slug);
	}
}

FString FScenarioChangeReport::Summary() const
{
	if (!bOk)
	{
		return Error;
	}
	TArray<FString> Parts;
	if (CreatedAssets.Num() > 0)
	{
		Parts.Add(FString::Printf(TEXT("%d asset%s imported"), CreatedAssets.Num(), CreatedAssets.Num() == 1 ? TEXT("") : TEXT("s")));
	}
	if (SpawnedActors.Num() > 0)
	{
		Parts.Add(FString::Printf(TEXT("placed %s"), *SpawnedActors[0]));
	}
	if (ModifiedActors.Num() > 0)
	{
		Parts.Add(FString::Printf(TEXT("changed %s"), *ModifiedActors[0]));
	}
	if (SavedFiles.Num() > 0)
	{
		Parts.Add(FString::Printf(TEXT("%d file%s saved"), SavedFiles.Num(), SavedFiles.Num() == 1 ? TEXT("") : TEXT("s")));
	}
	return Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : TEXT("Nothing changed.");
}

TSharedRef<FJsonObject> FScenarioChangeReport::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("ok"), bOk);
	if (!Error.IsEmpty())
	{
		Json->SetStringField(TEXT("error"), Error);
	}
	Json->SetField(TEXT("created_assets"), ScenarioJson::StringArrayValue(CreatedAssets));
	Json->SetField(TEXT("spawned_actors"), ScenarioJson::StringArrayValue(SpawnedActors));
	Json->SetField(TEXT("modified_actors"), ScenarioJson::StringArrayValue(ModifiedActors));
	Json->SetField(TEXT("saved_files"), ScenarioJson::StringArrayValue(SavedFiles));
	Json->SetField(TEXT("checks"), ScenarioJson::StringArrayValue(Checks));
	Json->SetField(TEXT("warnings"), ScenarioJson::StringArrayValue(Warnings));
	return Json;
}

void FScenarioChangeReport::Append(const FScenarioChangeReport& Other)
{
	CreatedAssets.Append(Other.CreatedAssets);
	SpawnedActors.Append(Other.SpawnedActors);
	ModifiedActors.Append(Other.ModifiedActors);
	SavedFiles.Append(Other.SavedFiles);
	Checks.Append(Other.Checks);
	Warnings.Append(Other.Warnings);
}

FScenarioChangeReport FScenarioChangeReport::Fail(const FString& InError)
{
	FScenarioChangeReport Report;
	Report.bOk = false;
	Report.Error = InError;
	return Report;
}

UScenarioSubsystem* UScenarioSubsystem::Get()
{
	return GEditor ? GEditor->GetEditorSubsystem<UScenarioSubsystem>() : nullptr;
}

FString UScenarioSubsystem::StateDir()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Scenario")));
}

void UScenarioSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	IFileManager::Get().MakeDirectory(*StateDir(), true);

	FScenarioSessionConfig Config;
	Config.StateDir = StateDir();
	SessionPtr = MakeShared<FScenarioSession>(Config);
	ProvenancePtr = MakeShared<FScenarioProvenanceIndex>(FPaths::Combine(StateDir(), TEXT("provenance-index.json")));
	ProvenancePtr->Load();

	SessionPtr->Start();
	for (const FScenarioJobRow& Row : SessionPtr->Ledger().Rows())
	{
		SeenStates.Add(Row.LocalId, Row.State);
	}
	ScenarioTelemetry::SetEnabled(UScenarioEditorSettings::Get().bTelemetry);
	LedgerHandle = SessionPtr->Ledger().OnChanged.AddUObject(this, &UScenarioSubsystem::OnLedgerChanged);
	FilesHandle = SessionPtr->OnFilesReady.AddUObject(this, &UScenarioSubsystem::OnFilesReady);
	UE_LOG(LogScenario, Log, TEXT("Scenario %s ready (credentials: %s)."), *ScenarioVersion::PluginVersion(), *SessionPtr->DescribeCredentialStore());
}

void UScenarioSubsystem::Deinitialize()
{
	if (SessionPtr.IsValid())
	{
		SessionPtr->Ledger().OnChanged.Remove(LedgerHandle);
		SessionPtr->OnFilesReady.Remove(FilesHandle);
		SessionPtr->Shutdown();
	}
	SessionPtr.Reset();
	Super::Deinitialize();
}

void UScenarioSubsystem::GetModelFields(const FString& ModelId, TFunction<void(const TArray<FScenarioField>&, const FScenarioError&)> OnDone)
{
	if (const TArray<FScenarioField>* Cached = FieldCache.Find(ModelId))
	{
		OnDone(*Cached, FScenarioError());
		return;
	}
	TWeakObjectPtr<UScenarioSubsystem> WeakThis(this);
	SessionPtr->Api().GetSchema(ModelId, [WeakThis, ModelId, OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone({}, Reply.Error);
			return;
		}
		TArray<FScenarioField> Fields = ScenarioSchema::FieldsFromSchema(Reply.Value, FScenarioLanes::Get().Restrict());
		if (WeakThis.IsValid())
		{
			WeakThis->FieldCache.Add(ModelId, Fields);
		}
		OnDone(Fields, FScenarioError());
	});
}

void UScenarioSubsystem::GetWorkflowFields(const FString& WorkflowId, TFunction<void(const TArray<FScenarioField>&, const TSharedPtr<FJsonObject>&, const FScenarioError&)> OnDone)
{
	SessionPtr->Api().GetWorkflow(WorkflowId, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone({}, nullptr, Reply.Error);
			return;
		}
		const TSharedPtr<FJsonObject> Workflow = ScenarioJson::GetObject(Reply.Value, TEXT("workflow"));
		OnDone(ScenarioSchema::FieldsFromSchema(Reply.Value, FScenarioLanes::Get().Restrict()), Workflow.IsValid() ? Workflow : Reply.Value, FScenarioError());
	});
}

FString UScenarioSubsystem::DestinationFor(const FScenarioJobRow& Row) const
{
	if (!Row.Destination.IsEmpty())
	{
		return Row.Destination;
	}
	const UScenarioProjectSettings& Settings = UScenarioProjectSettings::Get();
	FString Lane = Row.Lane;
	if (Lane.IsEmpty())
	{
		Lane = Row.Kind == TEXT("workflow") ? TEXT("workflow") : TEXT("image");
	}
	FString Path = Settings.RootPath() / ScenarioFiles::KindFolder(Lane);
	if (Settings.bDateFolders)
	{
		Path /= ScenarioFiles::LocalDate(Row.CreatedAt > 0.0 ? Row.CreatedAt : ScenarioNow());
	}
	return Path;
}

void UScenarioSubsystem::OnFilesReady(const FString& LocalId)
{
	const FScenarioJobRow* Row = SessionPtr->Ledger().Find(LocalId);
	if (!Row)
	{
		return;
	}
	// Agents and scripts import through their own calls; the person's results import here when asked to.
	if (UScenarioEditorSettings::Get().bAutoImport && Row->Source == TEXT("person") && !Row->bImported)
	{
		ImportJob(LocalId, FScenarioPlaceOptions());
	}
}

void UScenarioSubsystem::OnLedgerChanged(const FString& LocalId)
{
	const FScenarioJobRow* Row = SessionPtr->Ledger().Find(LocalId);
	if (!Row)
	{
		return;
	}
	const EScenarioJobState* Seen = SeenStates.Find(LocalId);
	const EScenarioJobState Previous = Seen ? *Seen : EScenarioJobState::Submitting;
	if (!Seen || Previous != Row->State)
	{
		SeenStates.Add(LocalId, Row->State);
		Notify(*Row, Previous);
	}
}

void UScenarioSubsystem::Notify(const FScenarioJobRow& Row, EScenarioJobState Previous)
{
	const FString LocalId = Row.LocalId;
	const FText Model = FText::FromString(Row.ModelName.IsEmpty() ? (Row.ModelId.IsEmpty() ? Row.WorkflowId : Row.ModelId) : Row.ModelName);
	const FText Prompt = FText::FromString(Row.Prompt.Left(80));
	if (Row.IsFinal())
	{
		// Read each time so a change in Editor Preferences applies at once.
		ScenarioTelemetry::SetEnabled(UScenarioEditorSettings::Get().bTelemetry);
		ScenarioTelemetry::Event(TEXT("job_finished"), {
			{ TEXT("lane"), Row.Lane }, { TEXT("kind"), Row.Kind }, { TEXT("state"), LexToString(Row.State) }, { TEXT("source"), Row.Source }, { TEXT("client"), Row.Client } });
	}
	// A person's job keeps one notification from send to result; the toasts below are for the others.
	if (UpdateProgress(Row))
	{
		if (Row.State == EScenarioJobState::Success && Row.Source == TEXT("person"))
		{
			AskTelemetryOnce();
		}
		return;
	}
	switch (Row.State)
	{
	case EScenarioJobState::Queued:
	case EScenarioJobState::Running:
		if (Previous == EScenarioJobState::Submitting && Row.Source == TEXT("agent"))
		{
			FScenarioEditorModule::Toast(
				FText::Format(LOCTEXT("AgentSpent", "{0} started {1} for {2}"), FText::FromString(Row.Client.IsEmpty() ? TEXT("An agent") : Row.Client), Model,
					FText::FromString(ScenarioCU::Format(Row.QuotedCU))),
				Prompt, true, { LOCTEXT("OpenJobs", "Jobs") }, { []() { FScenarioEditorModule::OpenStudio(ScenarioPages::Jobs); } });
		}
		break;
	case EScenarioJobState::Success:
		if (Row.Source == TEXT("person") && Row.Kind != TEXT("prompt_spark") && !UScenarioEditorSettings::Get().bAutoImport)
		{
			FScenarioEditorModule::Toast(FText::Format(LOCTEXT("Ready", "Ready: {0}"), Model), Prompt, true,
				{ LOCTEXT("Import", "Import"), LOCTEXT("ImportPlace", "Import and place") },
				{
					[LocalId]() { if (UScenarioSubsystem* Self = Get()) { Self->ImportJob(LocalId, FScenarioPlaceOptions()); } },
					[LocalId]() { if (UScenarioSubsystem* Self = Get()) { FScenarioPlaceOptions Options; Options.Placement = EScenarioPlacement::Auto; Self->ImportJob(LocalId, Options); } },
				});
		}
		if (Row.Source == TEXT("person"))
		{
			AskTelemetryOnce();
		}
		break;
	case EScenarioJobState::Failed:
	case EScenarioJobState::Blocked:
		FScenarioEditorModule::Toast(FText::Format(LOCTEXT("Failed", "{0} failed"), Model), FText::FromString(Row.Error.Left(200)), false,
			{ LOCTEXT("Details", "Details") }, { []() { FScenarioEditorModule::OpenStudio(ScenarioPages::Jobs); } });
		break;
	case EScenarioJobState::Unknown:
		FScenarioEditorModule::Toast(LOCTEXT("Unknown", "We do not know if Scenario received this request"),
			LOCTEXT("UnknownSub", "An identical request is blocked until you check it in Jobs."), false,
			{ LOCTEXT("Check", "Open Jobs") }, { []() { FScenarioEditorModule::OpenStudio(ScenarioPages::Jobs); } });
		break;
	default:
		break;
	}
}

bool UScenarioSubsystem::UpdateProgress(const FScenarioJobRow& Row)
{
	if (Row.Source != TEXT("person") || Row.Kind == TEXT("prompt_spark") || !FSlateApplication::IsInitialized())
	{
		return false;
	}
	const FString LocalId = Row.LocalId;
	TSharedPtr<SNotificationItem> Item = ProgressItems.FindRef(LocalId).Pin();
	const bool bRunning = Row.State == EScenarioJobState::Queued || Row.State == EScenarioJobState::Running;
	if (!Item.IsValid())
	{
		if (!bRunning)
		{
			return false;
		}
		const FString Model = Row.ModelName.IsEmpty() ? (Row.ModelId.IsEmpty() ? Row.WorkflowId : Row.ModelId) : Row.ModelName;
		const FString Cost = ScenarioCU::Format(Row.QuotedCU);
		// Text and detail read the job list, so progress and the final state show without more calls.
		auto Find = [LocalId]() -> const FScenarioJobRow*
		{
			UScenarioSubsystem* Self = Get();
			return Self ? Self->Session().Ledger().Find(LocalId) : nullptr;
		};
		FScenarioToastContent Content;
		Content.Title = TAttribute<FText>::CreateLambda([Find, Model]()
		{
			const FScenarioJobRow* Job = Find();
			if (!Job || Job->State == EScenarioJobState::Queued)
			{
				return FText::Format(LOCTEXT("ProgressQueued", "Sent to Scenario: {0}"), FText::FromString(Model));
			}
			switch (Job->State)
			{
			case EScenarioJobState::Running:
				return Job->Progress > 0.0
					? FText::Format(LOCTEXT("ProgressRunning", "Generating with {0}: {1}%"), FText::FromString(Model), FText::AsNumber(FMath::RoundToInt(Job->Progress * 100.0)))
					: FText::Format(LOCTEXT("ProgressStarted", "Generating with {0}"), FText::FromString(Model));
			case EScenarioJobState::Success:
				return FText::Format(LOCTEXT("ProgressReady", "Ready: {0}"), FText::FromString(Model));
			case EScenarioJobState::Unknown:
				return LOCTEXT("ProgressUnknown", "We do not know if Scenario received this request");
			case EScenarioJobState::Canceled:
				return FText::Format(LOCTEXT("ProgressCanceled", "Canceled: {0}"), FText::FromString(Model));
			default:
				return FText::Format(LOCTEXT("ProgressFailed", "{0} failed"), FText::FromString(Model));
			}
		});
		Content.Detail = TAttribute<FText>::CreateLambda([Find, Cost, Prompt = Row.Prompt.Left(80)]()
		{
			const FScenarioJobRow* Job = Find();
			if (Job && (Job->State == EScenarioJobState::Failed || Job->State == EScenarioJobState::Blocked) && !Job->Error.IsEmpty())
			{
				return FText::FromString(Job->Error.Left(200));
			}
			if (Job && Job->State == EScenarioJobState::Unknown)
			{
				return LOCTEXT("ProgressUnknownSub", "An identical request is blocked until you check it in Jobs.");
			}
			return Prompt.IsEmpty() ? FText::FromString(Cost) : FText::Format(LOCTEXT("ProgressSub", "{0} · {1}"), FText::FromString(Prompt), FText::FromString(Cost));
		});
		Content.Progress = TAttribute<TOptional<float>>::CreateLambda([Find]()
		{
			const FScenarioJobRow* Job = Find();
			return Job && Job->State == EScenarioJobState::Running && Job->Progress > 0.0 ? TOptional<float>(static_cast<float>(Job->Progress)) : TOptional<float>();
		});
		Content.CloseTooltip = LOCTEXT("ProgressClose", "Close this notification. The job keeps running; Jobs shows it.");
		auto Action = [](const FText& Label, TFunction<void()> Run, SNotificationItem::ECompletionState Shown)
		{
			FScenarioToastAction Out;
			Out.Label = Label;
			Out.Run = MoveTemp(Run);
			Out.States = { Shown };
			return Out;
		};
		Content.Actions.Add(Action(LOCTEXT("ProgressImportPlace", "Import and place"),
			[LocalId]() { if (UScenarioSubsystem* Self = Get()) { FScenarioPlaceOptions Options; Options.Placement = EScenarioPlacement::Auto; Self->ImportJob(LocalId, Options); } }, SNotificationItem::CS_Success));
		Content.Actions.Add(Action(LOCTEXT("ProgressImport", "Import only"),
			[LocalId]() { if (UScenarioSubsystem* Self = Get()) { Self->ImportJob(LocalId, FScenarioPlaceOptions()); } }, SNotificationItem::CS_Success));
		Content.Actions.Add(Action(LOCTEXT("ProgressDetails", "Details"),
			[]() { FScenarioEditorModule::OpenStudio(ScenarioPages::Jobs); }, SNotificationItem::CS_Fail));
		FScenarioToastAction Jobs = Action(LOCTEXT("ProgressJobs", "Open Jobs"), []() { FScenarioEditorModule::OpenStudio(ScenarioPages::Jobs); }, SNotificationItem::CS_Pending);
		Jobs.bCloses = false;
		Content.Actions.Add(MoveTemp(Jobs));
		Item = SScenarioToast::Show(Content, SNotificationItem::CS_Pending, 0.0f);
		if (!Item.IsValid())
		{
			return false;
		}
		ProgressItems.Add(LocalId, Item);
	}

	if (bRunning)
	{
		return true;
	}
	const bool bOk = Row.State == EScenarioJobState::Success;
	const bool bStopped = Row.State == EScenarioJobState::Canceled || Row.State == EScenarioJobState::Rejected || Row.State == EScenarioJobState::Dismissed;
	// With automatic import the result imports on its own: no Import buttons.
	Item->SetCompletionState(bStopped || (bOk && UScenarioEditorSettings::Get().bAutoImport) ? SNotificationItem::CS_None
		: bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	Item->SetExpireDuration(bStopped ? 4.0f : bOk ? 12.0f : 20.0f);
	Item->ExpireAndFadeout();
	ProgressItems.Remove(LocalId);
	return true;
}

void UScenarioSubsystem::ImportJob(const FString& LocalId, const FScenarioPlaceOptions& Options, TFunction<void(const FScenarioChangeReport&)> OnDone)
{
	auto Fail = [&OnDone](const FString& Message)
	{
		if (OnDone)
		{
			OnDone(FScenarioChangeReport::Fail(Message));
		}
	};
	const FScenarioJobRow* Found = SessionPtr->Ledger().Find(LocalId);
	if (!Found)
	{
		Fail(TEXT("This job is not in the job list."));
		return;
	}
	if (Found->State != EScenarioJobState::Success)
	{
		Fail(TEXT("This job has not finished on Scenario."));
		return;
	}
	if (Importing.Contains(LocalId))
	{
		Fail(TEXT("This job is already being imported."));
		return;
	}
	if (Found->bImported && Options.Placement != EScenarioPlacement::None)
	{
		const FScenarioChangeReport Report = PlaceJob(LocalId, Options);
		if (OnDone)
		{
			OnDone(Report);
		}
		return;
	}
	if (Found->bImported)
	{
		Fail(TEXT("This job is already imported. Use Place, or remove the imported assets first."));
		return;
	}
	if (SessionPtr->IsDownloading(LocalId))
	{
		Fail(FString::Printf(TEXT("Still downloading (%d%%). Import when the download finishes."), FMath::RoundToInt(100.0f * FMath::Max(0.0f, SessionPtr->DownloadProgress(LocalId)))));
		return;
	}

	const FScenarioJobRow Row = *Found;
	TArray<int32> Indices = Options.Files;
	if (Indices.Num() == 0)
	{
		for (int32 Index = 0; Index < Row.Files.Num(); ++Index)
		{
			Indices.Add(Index);
		}
	}
	for (const int32 Index : Indices)
	{
		if (!Row.Files.IsValidIndex(Index) || Row.Files[Index].LocalPath.IsEmpty() || !FPaths::FileExists(Row.Files[Index].LocalPath))
		{
			SessionPtr->DownloadFiles(LocalId);
			Fail(Row.AssetIds.Num() == 0 ? TEXT("This job produced no files.") : TEXT("The files are not downloaded yet; the download has started. Import again when it finishes."));
			return;
		}
	}

	const FString Importer = ImporterFor(Row);
	FString Destination = DestinationFor(Row);
	if (Importer == TEXT("mesh") || Importer == TEXT("material") || Indices.Num() > 1)
	{
		// Multi-asset results get a folder per generation.
		const FString First = Row.Files[Indices[0]].AssetId;
		Destination /= ScenarioFiles::PascalSlug(Row.Prompt.IsEmpty() ? Row.ModelName : Row.Prompt) + TEXT("_") + ScenarioFiles::Tail(First);
	}

	struct FPending
	{
		int32 Remaining = 0;
		TMap<int32, TArray<UObject*>> PerFile;
		FScenarioChangeReport Report;
	};
	TSharedRef<FPending> Pending = MakeShared<FPending>();
	Importing.Add(LocalId);
	TWeakObjectPtr<UScenarioSubsystem> WeakThis(this);

	auto Complete = [WeakThis, LocalId, Options, OnDone, Pending, Importer, Destination, Indices]()
	{
		UScenarioSubsystem* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		const FScenarioJobRow* Current = Self->SessionPtr->Ledger().Find(LocalId);
		if (!Current)
		{
			Self->Importing.Remove(LocalId);
			if (OnDone)
			{
				OnDone(FScenarioChangeReport::Fail(TEXT("The job left the list during the import.")));
			}
			return;
		}
		const FScenarioJobRow Row = *Current;
		FScenarioChangeReport& Report = Pending->Report;
		const UScenarioProjectSettings& Settings = UScenarioProjectSettings::Get();
		TArray<UObject*> Created;
		TMap<UObject*, int32> FileOf;

		// Per-file settings, names and provenance.
		for (const TPair<int32, TArray<UObject*>>& Pair : Pending->PerFile)
		{
			const FScenarioJobFile& File = Row.Files[Pair.Key];
			const FString Role = RoleIn(Importer, File);
			for (UObject* Object : Pair.Value)
			{
				UObject* Asset = Object;
				if (UTexture* Texture = Cast<UTexture>(Asset))
				{
					ScenarioAssets::ConfigureTexture(Texture, Importer == TEXT("mesh") ? FString() : Role, Settings.MaxTextureSize);
				}
				if (Asset->IsA<UStaticMesh>() || Asset->IsA<USkeletalMesh>())
				{
					ScenarioAssets::ConfigureStaticMesh(Asset);
					const FString Wanted = ScenarioFiles::AssetName(Asset->IsA<USkeletalMesh>() ? TEXT("SK") : TEXT("SM"), Row.Prompt, File.AssetId);
					if (!Asset->GetName().StartsWith(TEXT("SM_")) && !Asset->GetName().StartsWith(TEXT("SK_")))
					{
						Asset = ScenarioAssets::RenameAsset(Asset, Wanted);
					}
					ScenarioAssets::CheckMesh(Asset, Report.Checks);
				}
				Created.Add(Asset);
				FileOf.Add(Asset, Pair.Key);
			}
		}
		if (Importer == TEXT("mesh"))
		{
			if (const int32 Linear = ScenarioAssets::FixMeshTextures(Created))
			{
				Report.Checks.Add(FString::Printf(TEXT("%d metallic, roughness or occlusion map%s set to linear."), Linear, Linear == 1 ? TEXT("") : TEXT("s")));
			}
		}

		// Lane assets built from the imported files.
		FImportedSet Set;
		Set.All = Created;
		Set.Classify();
		const FScenarioJobFile& FirstFile = Row.Files[Indices[0]];
		if (Importer == TEXT("material"))
		{
			TMap<FName, UTexture*> Maps;
			for (UObject* Object : Created)
			{
				if (UTexture* Texture = Cast<UTexture>(Object))
				{
					const FName Parameter = ParameterForRole(RoleIn(Importer, Row.Files[FileOf[Object]]));
					if (!Parameter.IsNone() && !Maps.Contains(Parameter))
					{
						Maps.Add(Parameter, Texture);
					}
				}
			}
			if (UMaterialInstanceConstant* Instance = ScenarioAssets::CreateMaterialInstance(ScenarioAssets::Master(EScenarioMaster::Pbr, Report), Destination,
				ScenarioFiles::AssetName(TEXT("MI"), Row.Prompt, FirstFile.AssetId), Maps, { { TEXT("Tiling"), 1.0f } }, Report))
			{
				Created.Add(Instance);
				FileOf.Add(Instance, Indices[0]);
			}
		}
		else if (Importer == TEXT("sky") && Set.Texture && !Set.Cube && (Set.Texture->CompressionSettings == TC_HDR || Set.Texture->CompressionSettings == TC_HDR_Compressed))
		{
			Report.Warnings.Add(TEXT("The HDR panorama came in as a 2D texture: set it as a long-lat cubemap to use it on an HDRI Backdrop."));
		}
		else if (Importer == TEXT("sky") && Set.Texture && !Set.Cube)
		{
			if (UMaterialInstanceConstant* Instance = ScenarioAssets::CreateMaterialInstance(ScenarioAssets::Master(EScenarioMaster::SkySphere, Report), Destination,
				ScenarioFiles::AssetName(TEXT("MI"), Row.Prompt, FirstFile.AssetId), { { TEXT("Panorama"), Set.Texture } }, {}, Report))
			{
				Created.Add(Instance);
				FileOf.Add(Instance, Indices[0]);
			}
		}
		else if (Importer == TEXT("media"))
		{
			for (const int32 Index : Indices)
			{
				const FScenarioJobFile& File = Row.Files[Index];
				if (File.Kind == TEXT("video") || ScenarioFiles::MimeForExtension(FPaths::GetExtension(File.LocalPath)).StartsWith(TEXT("video/")))
				{
					if (UObject* Source = ScenarioAssets::CreateMediaSource(File.LocalPath, Destination, ScenarioFiles::AssetName(TEXT("FMS"), Row.Prompt, File.AssetId), Report))
					{
						Created.Add(Source);
						FileOf.Add(Source, Index);
					}
				}
			}
		}

		for (UObject* Object : Created)
		{
			const TMap<FName, FString> Values = ScenarioProvenance::Values(Row, Row.Files[FileOf.FindRef(Object)], Settings.bOmitPromptFromAssets);
			ScenarioProvenance::Write(Object, Values);
			Self->ProvenancePtr->Add(ScenarioAssets::PathOf(Object), Values);
			Report.CreatedAssets.AddUnique(ScenarioAssets::PathOf(Object));
			Self->TrackPackage(Object->GetOutermost());
		}
		if (Settings.bSaveImportedAssets)
		{
			Report.SavedFiles.Append(ScenarioAssets::SaveAssets(Created));
		}

		// The job remembers what came out of each file.
		TArray<FScenarioJobFile> Files = Row.Files;
		for (UObject* Object : Created)
		{
			Files[FileOf.FindRef(Object)].ImportedObjects.AddUnique(ScenarioAssets::PathOf(Object));
		}
		Self->SessionPtr->Ledger().SetFiles(LocalId, Files, ScenarioNow());
		if (Created.Num() > 0)
		{
			Self->SessionPtr->Ledger().MarkImported(LocalId, ScenarioNow());
		}
		Self->FinishImport(LocalId, Options, Report, OnDone);
	};

	for (const int32 Index : Indices)
	{
		const FScenarioJobFile& File = Row.Files[Index];
		if (Importer == TEXT("media") && File.Kind == TEXT("video"))
		{
			continue;
		}
		const FString Name = ScenarioAssets::UniqueName(Destination, ScenarioFiles::AssetName(PrefixFor(Importer, File), Row.Prompt, File.AssetId, File.Role));
		FString Staged;
		FString Error;
		if (!ScenarioAssets::StageFile(File.LocalPath, LocalId, Name, Staged, Error))
		{
			Pending->Report.Warnings.Add(Error);
			continue;
		}
		++Pending->Remaining;
		ScenarioAssets::ImportFile(Staged, Destination, [Pending, Index, Complete](const TArray<UObject*>& Objects, const FString& Error)
		{
			if (!Error.IsEmpty())
			{
				Pending->Report.Warnings.Add(Error);
			}
			Pending->PerFile.Add(Index, Objects);
			if (--Pending->Remaining == 0)
			{
				Complete();
			}
		});
	}
	if (Pending->Remaining == 0)
	{
		Complete();
	}
}

void UScenarioSubsystem::FinishImport(const FString& LocalId, const FScenarioPlaceOptions& Options, FScenarioChangeReport Report, TFunction<void(const FScenarioChangeReport&)> OnDone)
{
	Importing.Remove(LocalId);
	Report.bOk = Report.CreatedAssets.Num() > 0;
	if (!Report.bOk)
	{
		Report.Error = Report.Warnings.Num() > 0 ? TEXT("Import failed: ") + Report.Warnings[0] : TEXT("Import failed: Unreal created no asset.");
		SessionPtr->Ledger().SetError(LocalId, Report.Error, ScenarioNow());
	}
	else if (Options.Placement != EScenarioPlacement::None)
	{
		const FScenarioChangeReport Placed = PlaceJob(LocalId, Options);
		Report.Append(Placed);
		if (!Placed.bOk)
		{
			Report.Warnings.Add(Placed.Error);
		}
	}
	Reports.Add(LocalId, Report);
	if (Report.bOk && Report.CreatedAssets.Num() > 0 && GEditor)
	{
		// Show the new assets in the Content Browser.
		TArray<UObject*> Objects;
		for (const FString& Path : Report.CreatedAssets)
		{
			if (UObject* Object = FindObject<UObject>(nullptr, *Path))
			{
				Objects.Add(Object);
			}
		}
		GEditor->SyncBrowserToObjects(Objects);
	}
	OnImported.Broadcast(LocalId, Report);
	if (OnDone)
	{
		OnDone(Report);
	}
}

FScenarioChangeReport UScenarioSubsystem::PlaceJob(const FString& LocalId, const FScenarioPlaceOptions& Options)
{
	const FScenarioJobRow* Found = SessionPtr->Ledger().Find(LocalId);
	if (!Found || !Found->bImported)
	{
		return FScenarioChangeReport::Fail(TEXT("Import the result before placing it."));
	}
	const FScenarioJobRow Row = *Found;
	FImportedSet Set;
	for (const FScenarioJobFile& File : Row.Files)
	{
		for (const FString& Path : File.ImportedObjects)
		{
			if (UObject* Object = LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				Set.All.AddUnique(Object);
			}
		}
	}
	Set.Classify();
	if (Set.All.Num() == 0)
	{
		return FScenarioChangeReport::Fail(TEXT("The imported assets are gone (deleted or moved outside the editor)."));
	}

	const FString Importer = ImporterFor(Row);
	const FString Label = LabelFor(Row);
	FScenarioChangeReport Report;
	FScenarioPlaceOptions Place = Options;
	if (Place.Placement == EScenarioPlacement::None)
	{
		Place.Placement = EScenarioPlacement::Auto;
	}
	const bool bUseTargets = Place.Placement == EScenarioPlacement::Auto || Place.Placement == EScenarioPlacement::OnSelection;
	TArray<AActor*> Targets;
	if (bUseTargets)
	{
		// Image planes the plugin placed are not targets in Auto: a new image gets its own plane.
		for (AActor* Actor : ScenarioPlacement::ResolveTargets(Place.TargetActors))
		{
			if (Place.Placement == EScenarioPlacement::OnSelection || Place.TargetActors.Num() > 0 || !Actor->Tags.Contains(ImagePlaneTag))
			{
				Targets.Add(Actor);
			}
		}
	}
	if (Place.Placement == EScenarioPlacement::OnSelection && Targets.Num() == 0)
	{
		return FScenarioChangeReport::Fail(TEXT("Select the actors to apply this to, or choose another placement."));
	}

	const FString Destination = Set.All[0] ? FPackageName::GetLongPackagePath(Set.All[0]->GetOutermost()->GetName()) : DestinationFor(Row);
	const FString FirstAsset = Row.AssetIds.Num() > 0 ? Row.AssetIds[0] : FString();
	TArray<AActor*> Spawned;
	{
		FScopedTransaction Transaction(FText::Format(LOCTEXT("PlaceTransaction", "Scenario: Place {0}"), FText::FromString(Label)));
		if (Importer == TEXT("mesh"))
		{
			if (AActor* Actor = ScenarioPlacement::SpawnMesh(Set.Mesh, Place, Label, Report))
			{
				Spawned.Add(Actor);
			}
		}
		else if (Importer == TEXT("material"))
		{
			UMaterialInterface* Material = Set.Material;
			if (Targets.Num() > 0)
			{
				for (AActor* Target : Targets)
				{
					ScenarioPlacement::ApplyMaterial(Target, Place.MaterialSlot, Material, Report);
				}
			}
			else if (AActor* Sphere = ScenarioPlacement::SpawnPreviewSphere(Material, Place, Label, Report))
			{
				Report.Warnings.Add(TEXT("Nothing was selected, so the material went on a preview sphere."));
				Spawned.Add(Sphere);
			}
		}
		else if (Importer == TEXT("sound"))
		{
			if (AActor* Actor = ScenarioPlacement::SpawnAmbientSound(Set.Sound, Place, Label, Report))
			{
				Spawned.Add(Actor);
			}
		}
		else if (Importer == TEXT("media"))
		{
			if (AActor* Actor = ScenarioPlacement::SpawnMediaPlate(Set.Media, Place, Label, Report))
			{
				Spawned.Add(Actor);
			}
		}
		else if (Importer == TEXT("sky"))
		{
			AActor* Actor = Set.Cube ? ScenarioPlacement::SpawnHdriBackdrop(Set.Cube, Label, Report) : ScenarioPlacement::SpawnSkySphere(Set.SkyMaterial, Label, Report);
			if (Actor)
			{
				Spawned.Add(Actor);
			}
		}
		else if (Set.Texture)
		{
			if (Targets.Num() > 0)
			{
				// On the selected actors: a lit material with the image as base color.
				UMaterialInstanceConstant* Instance = ScenarioAssets::CreateMaterialInstance(ScenarioAssets::Master(EScenarioMaster::Pbr, Report), Destination,
					ScenarioFiles::AssetName(TEXT("MI"), Row.Prompt, FirstAsset), { { TEXT("BaseColor"), Set.Texture } }, {}, Report);
				for (AActor* Target : Targets)
				{
					ScenarioPlacement::ApplyMaterial(Target, Place.MaterialSlot, Instance, Report);
				}
			}
			else
			{
				UMaterialInstanceConstant* Instance = ScenarioAssets::CreateMaterialInstance(ScenarioAssets::Master(EScenarioMaster::Unlit, Report), Destination,
					ScenarioFiles::AssetName(TEXT("MI"), Row.Prompt, FirstAsset, TEXT("plane")), { { TEXT("Image"), Set.Texture } }, {}, Report);
				if (AActor* Plane = ScenarioPlacement::SpawnImagePlane(Instance, Set.Aspect, Place, Label, Report))
				{
					Plane->Tags.AddUnique(ImagePlaneTag);
					Spawned.Add(Plane);
				}
			}
		}
		if (!Report.ChangedAnything())
		{
			Transaction.Cancel();
		}
	}

	for (AActor* Actor : Spawned)
	{
		TrackPackage(Actor->GetPackage());
	}
	ScenarioPlacement::Select(Spawned);
	// Assets made for the placement (material instances) are saved like imports.
	TArray<UObject*> NewAssets;
	for (const FString& Path : Report.CreatedAssets)
	{
		if (UObject* Object = FindObject<UObject>(nullptr, *Path))
		{
			NewAssets.Add(Object);
			TrackPackage(Object->GetOutermost());
		}
	}
	if (NewAssets.Num() > 0 && UScenarioProjectSettings::Get().bSaveImportedAssets)
	{
		Report.SavedFiles.Append(ScenarioAssets::SaveAssets(NewAssets));
	}
	Report.bOk = Report.SpawnedActors.Num() + Report.ModifiedActors.Num() > 0;
	if (!Report.bOk)
	{
		Report.Error = Report.Warnings.Num() > 0 ? Report.Warnings[0] : TEXT("Nothing was placed.");
	}
	return Report;
}

FScenarioChangeReport UScenarioSubsystem::RemoveImported(const FString& LocalId)
{
	const FScenarioJobRow* Found = SessionPtr->Ledger().Find(LocalId);
	if (!Found || !Found->bImported)
	{
		return FScenarioChangeReport::Fail(TEXT("This job has no imported assets."));
	}
	TArray<UObject*> Objects;
	TArray<FString> Paths;
	for (const FScenarioJobFile& File : Found->Files)
	{
		for (const FString& Path : File.ImportedObjects)
		{
			if (UObject* Object = LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				Objects.AddUnique(Object);
				Paths.AddUnique(Path);
			}
		}
	}
	// Refuse when anything outside this result uses the assets (levels, other assets, undo history).
	for (UObject* Object : Objects)
	{
		bool bReferenced = false;
		bool bReferencedByUndo = false;
		ObjectTools::GatherObjectReferencersForDeletion(Object, bReferenced, bReferencedByUndo);
		if (bReferenced)
		{
			FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			TArray<FName> Referencers;
			Registry.Get().GetReferencers(Object->GetOutermost()->GetFName(), Referencers);
			bool bExternal = false;
			for (const FName& Referencer : Referencers)
			{
				bExternal |= !Objects.ContainsByPredicate([&Referencer](const UObject* Each) { return Each->GetOutermost()->GetFName() == Referencer; });
			}
			if (bExternal || Referencers.Num() == 0)
			{
				return FScenarioChangeReport::Fail(FString::Printf(TEXT("%s is still used (by a level, another asset or the undo history). Remove those uses first."), *Object->GetName()));
			}
		}
	}
	const int32 Deleted = ObjectTools::DeleteObjectsUnchecked(Objects);
	FScenarioChangeReport Report;
	Report.bOk = Deleted > 0;
	Report.Error = Deleted > 0 ? FString() : TEXT("Nothing was removed.");
	for (const FString& Path : Paths)
	{
		Provenance().Remove(Path);
		Report.Warnings.Add(TEXT("Removed ") + Path);
	}
	TArray<FScenarioJobFile> Files = Found->Files;
	for (FScenarioJobFile& File : Files)
	{
		File.ImportedObjects.Reset();
	}
	SessionPtr->Ledger().SetFiles(LocalId, Files, ScenarioNow());
	if (FScenarioJobRow* Row = SessionPtr->Ledger().Find(LocalId))
	{
		Row->bImported = false;
		SessionPtr->Ledger().SetError(LocalId, FString(), ScenarioNow());
	}
	return Report;
}

void UScenarioSubsystem::TrackPackage(UPackage* Package)
{
	if (Package)
	{
		TouchedPackages.Add(Package->GetName());
	}
}

FScenarioChangeReport UScenarioSubsystem::SaveChanges(bool bIncludeLevels)
{
	TArray<UPackage*> Packages;
	for (const FString& Name : TouchedPackages)
	{
		UPackage* Package = FindPackage(nullptr, *Name);
		if (!Package || !Package->IsDirty())
		{
			continue;
		}
		const bool bIsMap = Package->ContainsMap() || Name.Contains(TEXT("__ExternalActors__"));
		if (bIsMap && !bIncludeLevels)
		{
			continue;
		}
		Packages.Add(Package);
	}
	FScenarioChangeReport Report;
	if (Packages.Num() == 0)
	{
		return FScenarioChangeReport::Fail(TEXT("Nothing to save: the plugin has no unsaved changes."));
	}
	if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, true))
	{
		return FScenarioChangeReport::Fail(TEXT("Unreal could not save every package (see the Output Log)."));
	}
	for (const UPackage* Package : Packages)
	{
		const FString Extension = Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
		FString File;
		if (FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), File, Extension))
		{
			Report.SavedFiles.Add(FPaths::ConvertRelativePathToFull(File));
		}
	}
	Report.bOk = true;
	return Report;
}

FString UScenarioSubsystem::CaptureViewport(int32 Width, int32 Height, FString& OutError, FString* OutDescription)
{
	UWorld* World = ScenarioPlacement::EditorWorld();
	const ScenarioPlacement::FView View = ScenarioPlacement::ActiveView();
	if (!World || !View.bValid)
	{
		OutError = TEXT("No perspective level viewport to capture.");
		return FString();
	}
	Width = FMath::Clamp(Width, 64, 4096);
	Height = FMath::Clamp(Height, 64, 4096);

	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	Target->RenderTargetFormat = RTF_RGBA8_SRGB;
	Target->InitAutoFormat(Width, Height);
	Target->UpdateResourceImmediate(true);

	USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage());
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->CaptureSource = SCS_FinalColorLDR;
	Capture->TextureTarget = Target;
	Capture->FOVAngle = View.Fov;
	Capture->SetWorldLocationAndRotation(View.Location, View.Rotation);
	Capture->RegisterComponentWithWorld(World);
	Capture->CaptureScene();

	TArray<FColor> Pixels;
	FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
	const bool bRead = Resource && Resource->ReadPixels(Pixels);
	Capture->UnregisterComponent();
	Capture->DestroyComponent();
	if (!bRead || Pixels.Num() != Width * Height)
	{
		OutError = TEXT("The viewport capture returned no pixels.");
		return FString();
	}
	for (FColor& Pixel : Pixels)
	{
		Pixel.A = 255;
	}
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
	const FString Path = FPaths::Combine(StateDir(), TEXT("Captures"), FString::Printf(TEXT("viewport_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
	if (!FFileHelper::SaveArrayToFile(Png, *Path))
	{
		OutError = TEXT("Could not write the capture to Saved/Scenario/Captures.");
		return FString();
	}
	AllowUpload(Path);
	if (OutDescription)
	{
		*OutDescription = FString::Printf(TEXT("Editor world, level viewport camera at (%.0f, %.0f, %.0f), FOV %.0f, %d x %d"),
			View.Location.X, View.Location.Y, View.Location.Z, View.Fov, Width, Height);
	}
	return Path;
}

FString UScenarioSubsystem::ExportActorsToGlb(const TArray<AActor*>& Actors, FString& OutError)
{
	UWorld* World = ScenarioPlacement::EditorWorld();
	if (!World || Actors.Num() == 0)
	{
		OutError = TEXT("Select the actors to export.");
		return FString();
	}
	UGLTFExportOptions* ExportOptions = NewObject<UGLTFExportOptions>();
	ExportOptions->ResetToDefault();
	const FString Path = FPaths::Combine(StateDir(), TEXT("Captures"), FString::Printf(TEXT("%s_%s.glb"),
		*ScenarioFiles::SafeObjectName(Actors[0]->GetActorLabel()), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
	TSet<AActor*> Selected(Actors);
	FGLTFExportMessages Messages;
	if (!UGLTFExporter::ExportToGLTF(World, Path, ExportOptions, Selected, Messages) || !FPaths::FileExists(Path))
	{
		OutError = Messages.Errors.Num() > 0 ? TEXT("GLB export failed: ") + Messages.Errors[0] : TEXT("GLB export failed.");
		return FString();
	}
	AllowUpload(Path);
	return Path;
}

namespace
{
	FString SourceFileOf(UObject* Asset)
	{
		// AssetImportData is a reflected property on textures, meshes and sounds.
		const FObjectProperty* Property = FindFProperty<FObjectProperty>(Asset->GetClass(), TEXT("AssetImportData"));
		const UAssetImportData* Data = Property ? Cast<UAssetImportData>(Property->GetObjectPropertyValue_InContainer(Asset)) : nullptr;
		const FString File = Data ? Data->GetFirstFilename() : FString();
		return !File.IsEmpty() && FPaths::FileExists(File) ? File : FString();
	}

	bool HasExtension(const FString& Path, std::initializer_list<const TCHAR*> Extensions)
	{
		const FString Extension = FPaths::GetExtension(Path).ToLower();
		for (const TCHAR* Candidate : Extensions)
		{
			if (Extension == Candidate)
			{
				return true;
			}
		}
		return false;
	}
}

bool UScenarioSubsystem::ResolveUploadSource(UObject* Asset, FScenarioUploadSource& Out, FString& OutError)
{
	Out = FScenarioUploadSource();
	if (!Asset)
	{
		OutError = TEXT("The asset could not be loaded.");
		return false;
	}
	Out.Label = Asset->GetName();
	const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString Captures = FPaths::Combine(StateDir(), TEXT("Captures"));

	if (Cast<UTexture2D>(Asset))
	{
		Out.Kind = TEXT("image");
	}
	else if (Cast<UStaticMesh>(Asset) || Cast<USkeletalMesh>(Asset))
	{
		Out.Kind = TEXT("3d");
	}
	else if (Cast<USoundWave>(Asset))
	{
		Out.Kind = TEXT("audio");
	}
	else if (Cast<UFileMediaSource>(Asset))
	{
		Out.Kind = TEXT("video");
	}
	else
	{
		OutError = FString::Printf(TEXT("%s is a %s. Scenario takes textures, meshes, sounds and file media sources."), *Asset->GetName(), *Asset->GetClass()->GetName());
		return false;
	}

	// Generated by Scenario: reuse the asset, no upload.
	const FString* AssetId = ScenarioProvenance::Read(Asset).Find(TEXT("Scenario.AssetId"));
	if (AssetId && !AssetId->IsEmpty())
	{
		Out.AssetId = *AssetId;
		return true;
	}

	const FString Source = SourceFileOf(Asset);
	if (Out.Kind == TEXT("image"))
	{
		if (HasExtension(Source, { TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("webp") }))
		{
			Out.Path = Source;
		}
		else
		{
			UTexture2D* Texture = CastChecked<UTexture2D>(Asset);
			FImage Image;
			if (!Texture->Source.IsValid() || !Texture->Source.GetMipImage(Image, 0, 0, 0))
			{
				OutError = FString::Printf(TEXT("%s has no source pixels to export."), *Asset->GetName());
				return false;
			}
			Out.Path = FPaths::Combine(Captures, FString::Printf(TEXT("%s_%s.png"), *ScenarioFiles::SafeObjectName(Asset->GetName()), *Stamp));
			IFileManager::Get().MakeDirectory(*Captures, true);
			if (!FImageUtils::SaveImageByExtension(*Out.Path, Image))
			{
				OutError = FString::Printf(TEXT("%s could not be exported as PNG."), *Asset->GetName());
				return false;
			}
		}
	}
	else if (Out.Kind == TEXT("3d"))
	{
		if (HasExtension(Source, { TEXT("glb") }))
		{
			Out.Path = Source;
		}
		else
		{
			UGLTFExportOptions* ExportOptions = NewObject<UGLTFExportOptions>();
			ExportOptions->ResetToDefault();
			Out.Path = FPaths::Combine(Captures, FString::Printf(TEXT("%s_%s.glb"), *ScenarioFiles::SafeObjectName(Asset->GetName()), *Stamp));
			FGLTFExportMessages Messages;
			if (!UGLTFExporter::ExportToGLTF(Asset, Out.Path, ExportOptions, TSet<AActor*>(), Messages) || !FPaths::FileExists(Out.Path))
			{
				OutError = Messages.Errors.Num() > 0 ? TEXT("GLB export failed: ") + Messages.Errors[0] : TEXT("GLB export failed.");
				return false;
			}
		}
	}
	else if (Out.Kind == TEXT("audio"))
	{
		if (!HasExtension(Source, { TEXT("wav"), TEXT("mp3"), TEXT("ogg"), TEXT("flac") }))
		{
			OutError = FString::Printf(TEXT("The source file of %s is missing. Reimport it, or pick the audio file instead."), *Asset->GetName());
			return false;
		}
		Out.Path = Source;
	}
	else
	{
		const FString File = CastChecked<UFileMediaSource>(Asset)->GetFullPath();
		if (!FPaths::FileExists(File) || !HasExtension(File, { TEXT("mp4"), TEXT("mov"), TEXT("webm") }))
		{
			OutError = FString::Printf(TEXT("%s points to a file that is missing or not a video."), *Asset->GetName());
			return false;
		}
		Out.Path = File;
	}
	AllowUpload(Out.Path);
	return true;
}

bool UScenarioSubsystem::IsUploadAllowed(const FString& Path) const
{
	return UploadAllowList.Contains(FPaths::ConvertRelativePathToFull(Path));
}

void UScenarioSubsystem::AllowUpload(const FString& Path)
{
	UploadAllowList.Add(FPaths::ConvertRelativePathToFull(Path));
}

void UScenarioSubsystem::AskTelemetryOnce()
{
	UScenarioEditorSettings& Settings = UScenarioEditorSettings::GetMutable();
	if (Settings.bTelemetryAsked)
	{
		return;
	}
	// Asked once, after the first finished generation; ignoring the question leaves telemetry off.
	Settings.bTelemetryAsked = true;
	Settings.SaveConfig();
	auto Answer = [](bool bShare)
	{
		UScenarioEditorSettings& Mutable = UScenarioEditorSettings::GetMutable();
		Mutable.bTelemetry = bShare;
		Mutable.SaveConfig();
		ScenarioTelemetry::SetEnabled(bShare);
	};
	FScenarioEditorModule::Toast(LOCTEXT("TelemetryAsk", "Share anonymous usage statistics?"),
		LOCTEXT("TelemetryAskSub", "Lane, job state and versions only: never prompts, files, names or ids. Change it any time in Editor Preferences > Plugins > Scenario."), true,
		{ LOCTEXT("TelemetryYes", "Share"), LOCTEXT("TelemetryNo", "No thanks") },
		{ [Answer]() { Answer(true); }, [Answer]() { Answer(false); } });
}

void UScenarioSubsystem::ShowWelcomeIfNeeded()
{
	UScenarioEditorSettings& Settings = UScenarioEditorSettings::GetMutable();
	if (Settings.bWelcomeShown)
	{
		return;
	}
	Settings.bWelcomeShown = true;
	Settings.SaveConfig();
	FScenarioEditorModule::Toast(LOCTEXT("Installed", "Scenario is installed"), LOCTEXT("InstalledSub", "Every Scenario model, inside Unreal."), true,
		{ LOCTEXT("OpenScenario", "Open Scenario") }, { []() { FScenarioEditorModule::OpenStudio(ScenarioPages::Create); } });
}

#undef LOCTEXT_NAMESPACE
