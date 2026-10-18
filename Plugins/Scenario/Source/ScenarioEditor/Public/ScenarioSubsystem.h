// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "ScenarioLedger.h"
#include "ScenarioSchema.h"
#include "ScenarioSession.h"
#include "ScenarioSubsystem.generated.h"

class SNotificationItem;

class AActor;
class FScenarioProvenanceIndex;
class UMaterialInterface;

/** What an action changed. Every caller (Studio, composer, agents, scripts) gets one back. */
struct SCENARIOEDITOR_API FScenarioChangeReport
{
	bool bOk = false;
	FString Error;
	/** Object paths of assets created. */
	TArray<FString> CreatedAssets;
	/** "Label (ActorName)" of actors spawned. */
	TArray<FString> SpawnedActors;
	/** "Label (ActorName)" of existing actors changed (material slot, sky). */
	TArray<FString> ModifiedActors;
	/** Files written to disk by a save. */
	TArray<FString> SavedFiles;
	/** Import checks: one line each (scale, triangles, bones...). */
	TArray<FString> Checks;
	TArray<FString> Warnings;

	bool ChangedAnything() const { return CreatedAssets.Num() + SpawnedActors.Num() + ModifiedActors.Num() + SavedFiles.Num() > 0; }
	/** One line for a toast or a card. */
	FString Summary() const;
	TSharedRef<FJsonObject> ToJson() const;
	void Append(const FScenarioChangeReport& Other);
	static FScenarioChangeReport Fail(const FString& Error);
};

UENUM()
enum class EScenarioPlacement : uint8
{
	/** Import only (Content Browser). */
	None,
	/** The lane's natural target: the selected actor's material, a plane or mesh in front of the camera, a sky. */
	Auto,
	InFrontOfCamera,
	AtPivot,
	/** On the selected actors (material slot, Media Plate, sky light). */
	OnSelection,
	AtLocation,
};

struct SCENARIOEDITOR_API FScenarioPlaceOptions
{
	EScenarioPlacement Placement = EScenarioPlacement::None;
	/** Actor names or labels; empty uses the editor selection when the placement needs targets. */
	TArray<FString> TargetActors;
	int32 MaterialSlot = 0;
	FVector Location = FVector::ZeroVector;
	/** File indices of the job to import; empty imports all. */
	TArray<int32> Files;
};

/** A Content Browser asset as a Scenario input. */
struct SCENARIOEDITOR_API FScenarioUploadSource
{
	/** Set when the asset came from Scenario: it is reused, nothing is uploaded. */
	FString AssetId;
	/** Otherwise the file to upload: the imported source file, or an export (PNG, GLB). */
	FString Path;
	/** image, 3d, audio or video. */
	FString Kind;
	FString Label;
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnScenarioImported, const FString& /*LocalId*/, const FScenarioChangeReport& /*Report*/);

/**
 * The editor side of Scenario: one session per editor, the importers and the
 * placement. All actions run on the game thread; imports of large files go
 * through Interchange's asynchronous path.
 */
UCLASS()
class SCENARIOEDITOR_API UScenarioSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	/** The subsystem, or null before the editor is up. */
	static UScenarioSubsystem* Get();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	FScenarioSession& Session() const { return *SessionPtr; }
	TSharedRef<FScenarioSession> SessionShared() const { return SessionPtr.ToSharedRef(); }

	/** Saved/Scenario of this project. */
	static FString StateDir();

	// Forms

	/** Form fields of a model, cached for the session. */
	void GetModelFields(const FString& ModelId, TFunction<void(const TArray<FScenarioField>& Fields, const FScenarioError& Error)> OnDone);
	void GetWorkflowFields(const FString& WorkflowId, TFunction<void(const TArray<FScenarioField>& Fields, const TSharedPtr<FJsonObject>& Workflow, const FScenarioError& Error)> OnDone);

	// Results

	/** Imports a finished job's files, then places them as asked, in one undo step. */
	void ImportJob(const FString& LocalId, const FScenarioPlaceOptions& Options, TFunction<void(const FScenarioChangeReport&)> OnDone = nullptr);
	bool IsImporting(const FString& LocalId) const { return Importing.Contains(LocalId); }
	/** The last import report of a job in this session. */
	const FScenarioChangeReport* LastReport(const FString& LocalId) const { return Reports.Find(LocalId); }
	/** Places assets that are already imported (a second Place, or an agent's apply). */
	FScenarioChangeReport PlaceJob(const FString& LocalId, const FScenarioPlaceOptions& Options);
	/** Content folder a job imports into. */
	FString DestinationFor(const FScenarioJobRow& Row) const;
	/** Deletes the job's imported assets when nothing references them. */
	FScenarioChangeReport RemoveImported(const FString& LocalId);

	FOnScenarioImported OnImported;

	// Captures

	/** Renders the active level viewport's camera (no editor gizmos) to a PNG under Saved/Scenario/Captures. */
	FString CaptureViewport(int32 Width, int32 Height, FString& OutError, FString* OutDescription = nullptr);
	/** Exports the selected actors (or one actor) to a GLB under Saved/Scenario/Captures, for 3D edits. */
	FString ExportActorsToGlb(const TArray<AActor*>& Actors, FString& OutError);
	/**
	 * What to send for a Content Browser asset: its Scenario asset id, its source
	 * file, or an export (textures to PNG, meshes to GLB). False with OutError
	 * when the asset type cannot be a Scenario input.
	 */
	bool ResolveUploadSource(UObject* Asset, FScenarioUploadSource& Out, FString& OutError);
	/** Files the plugin produced or the person picked this session: the only files agents may upload. */
	bool IsUploadAllowed(const FString& Path) const;
	void AllowUpload(const FString& Path);

	// Saving

	/** Saves the packages the plugin created or changed this session (dirty ones only). */
	FScenarioChangeReport SaveChanges(bool bIncludeLevels);
	void TrackPackage(UPackage* Package);

	FScenarioProvenanceIndex& Provenance() const { return *ProvenancePtr; }

	/** Starts the first-run toast and the welcome card. */
	void ShowWelcomeIfNeeded();
	/** After the first finished generation: asks once whether to share anonymous usage statistics. */
	void AskTelemetryOnce();

private:
	void OnFilesReady(const FString& LocalId);
	void OnLedgerChanged(const FString& LocalId);
	void Notify(const FScenarioJobRow& Row, EScenarioJobState Previous);
	/**
	 * The corner notification of a job the person started: "Generating" from the
	 * moment Scenario accepts it, then Ready with Import, or Failed. Returns
	 * false when the job has none.
	 */
	bool UpdateProgress(const FScenarioJobRow& Row);
	void FinishImport(const FString& LocalId, const FScenarioPlaceOptions& Options, FScenarioChangeReport Report, TFunction<void(const FScenarioChangeReport&)> OnDone);

	TSharedPtr<FScenarioSession> SessionPtr;
	TSharedPtr<FScenarioProvenanceIndex> ProvenancePtr;
	TMap<FString, TArray<FScenarioField>> FieldCache;
	TSet<FString> Importing;
	TMap<FString, FScenarioChangeReport> Reports;
	TMap<FString, EScenarioJobState> SeenStates;
	TMap<FString, TWeakPtr<SNotificationItem>> ProgressItems;
	TSet<FString> UploadAllowList;
	TSet<FString> TouchedPackages;
	FDelegateHandle LedgerHandle;
	FDelegateHandle FilesHandle;
};
