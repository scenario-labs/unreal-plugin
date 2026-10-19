// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "ScenarioLanes.h"
#include "ScenarioQuotes.h"
#include "ScenarioSchema.h"
#include "ScenarioSession.h"

class AActor;

enum class EScenarioGenerateState : uint8
{
	SignedOut,
	NeedsWorkspace,
	NoModel,
	LoadingModel,
	ModelError,
	NeedsInput,
	Uploading,
	Pricing,
	Priced,
	PriceFailed,
	SpendingOff,
	Blocked,
	Submitting,
};

/** How the line under Generate reads. */
enum class EScenarioHintTone : uint8
{
	None,
	Busy,
	Info,
	Success,
	Warning,
	Error,
};

/** What a link on the line under Generate does. */
enum class EScenarioHintAction : uint8
{
	None,
	ChangeModel,
	OpenJobs,
	CheckNow,
	ImportAndPlace,
	ImportOnly,
	Show,
	Details,
};

/** The line under Generate: why it is disabled, or how the last request is going. */
struct FScenarioHint
{
	FText Text;
	EScenarioHintTone Tone = EScenarioHintTone::None;
	EScenarioHintAction Primary = EScenarioHintAction::None;
	EScenarioHintAction Secondary = EScenarioHintAction::None;
	/** The job the actions apply to. */
	FString LocalId;
};

/** A workflow the project can run. */
struct FScenarioWorkflowItem
{
	FString Id;
	FString Name;
	FString Description;
	/** The team's own workflow; false for Scenario's featured ones. */
	bool bTeam = false;
};

/** A file input of the form: a local file being uploaded, or a Scenario asset. */
struct FScenarioReference
{
	int32 Id = 0;
	FString Field;
	FString Label;
	FString LocalPath;
	FString AssetId;
	float Progress = 0.0f;
	FString Error;
	bool bUploading = false;
};

/**
 * The Create form's state, shared by the Studio's Create view and the
 * viewport composer: lane, model, values, references, price and the
 * Generate button. Prices are checked automatically (debounced) while the
 * form changes; Generate only sends a request whose fresh quote matches it.
 */
class FScenarioCreateModel : public TSharedFromThis<FScenarioCreateModel>
{
public:
	/** The Create form shared by the Studio and the composer. */
	static TSharedRef<FScenarioCreateModel> Get();

	void Initialize();
	void Shutdown();

	// Lane, mode and model

	const FString& GetLaneId() const { return LaneId; }
	const FScenarioLane* GetLane() const;
	void SetLane(const FString& InLane);
	const FString& GetMode() const { return Mode; }
	void SetMode(const FString& InMode);
	const FString& GetModelId() const { return ModelId; }
	FString GetModelName() const;
	FString GetModelNote() const;
	FString GetModelBadge() const;
	/** Picks a model; switches lane when the model belongs to another one. */
	void SetModel(const FString& InModelId, const FString& InName = FString());
	TArray<FScenarioLaneModel> QuickPicks() const;
	/** Runs a workflow instead of a model (the Workflow lane). */
	void SetWorkflow(const FString& WorkflowId, const FString& Name, const FString& Description);
	bool IsWorkflow() const { return bWorkflow; }
	/** Opens the Workflow lane on the last workflow used, or on none. */
	void EnterWorkflowLane();
	/** The lane shown in the lane row: a lane id, or "workflow". */
	FString GetLaneKey() const { return bWorkflow ? FString(TEXT("workflow")) : LaneId; }

	/** The project's workflows (team first, then featured), loaded once per project. */
	const TArray<FScenarioWorkflowItem>& GetWorkflows() const { return Workflows; }
	bool IsLoadingWorkflows() const { return bLoadingWorkflows; }
	const FString& GetWorkflowsError() const { return WorkflowsError; }
	void LoadWorkflows(bool bForce = false);
	/** Modality of the current lane for the picker (image, 3d, video, audio). */
	FString PickerModality() const;

	// Form

	const TArray<FScenarioField>& GetFields() const { return Fields; }
	bool IsLoadingSchema() const { return bLoadingSchema; }
	const FString& GetSchemaError() const { return SchemaError; }
	TSharedPtr<FJsonValue> GetValue(const FString& Name) const;
	void SetValue(const FString& Name, const TSharedPtr<FJsonValue>& Value);
	FString GetPrompt() const;
	void SetPrompt(const FString& Prompt);
	void ApplyValues(const TSharedPtr<FJsonObject>& InValues);
	TArray<FString> Problems() const;
	bool IsUnrealReadyModel() const;
	bool IsUnrealReady() const { return bUnrealReady; }
	void SetUnrealReady(bool bOn);

	// References

	const TArray<FScenarioReference>& GetReferences() const { return References; }
	TArray<FScenarioReference> ReferencesFor(const FString& Field) const;
	/** Uploads a local file for a file field. Returns an error or empty. A single-file field keeps the newest. */
	FString AddReferenceFile(const FString& Field, const FString& Path, const FString& Label);
	FString AddReferenceAsset(const FString& Field, const FString& AssetId, const FString& Label);
	void RemoveReference(const FString& Field, int32 IndexInField);
	/** Adds the active viewport as a reference image. Returns an error or empty. */
	FString AddViewportCapture(const FString& Field);
	/** Adds the Content Browser selection (textures, meshes, sounds). Returns an error or empty. */
	FString AddContentSelection(const FString& Field);
	/** Adds the selected actors exported as one GLB (3D edits). Returns an error or empty. */
	FString AddSelectedActors(const FString& Field);
	/**
	 * Uses a Scenario asset as the input of the first file field of its kind
	 * (Next steps, "Use as reference"). Waits for the model's form when it is loading.
	 */
	void UseReference(const FString& AssetId, const FString& Kind, const FString& Label);
	/** Uploads a local file into the first file field of its kind. Returns an error or empty. */
	FString UseFile(const FString& Path, const FString& Label);
	/** The first file field that takes this kind (image, 3d, audio, video), or null. */
	const FScenarioField* FileFieldFor(const FString& Kind) const;

	// Destination

	FString Destination;
	FString DestinationText() const;
	FString PlacementText() const;

	// Price and Generate

	FScenarioRunRequest BuildRequest() const;
	EScenarioGenerateState GetState() const;
	/** "Generate", "Generate · 30 CU", "Generate · ~30 CU", "Sending...", "Retry price". */
	FText GetButtonText() const;
	/** True while the price on the button is being checked again (shown dimmed). */
	bool IsPriceStale() const;
	FScenarioHint GetHint() const;
	FText GetButtonTooltip() const;
	bool CanClickGenerate() const;
	/** Re-checks the price now (the Retry button). */
	void Reprice();
	void Generate();
	const FScenarioError& GetPriceError() const { return PriceError; }
	const FString& GetLastError() const { return LastError; }
	const FString& GetLastLocalId() const { return LastLocalId; }
	/** True while the job this form sent last is queued or running at Scenario. */
	bool IsSentJobRunning() const;
	/** One line on the job this form sent last ("Sent. Generating with X: 42%"), empty when there is none. */
	FText GetSentJobText() const;
	/** The quote for the form as it is now, or null. */
	const FScenarioQuote* CurrentQuote() const;

	// Prompt tools (each priced and clicked separately)

	void PriceSpark(TFunction<void(const FString& PriceText)> OnDone);
	void RunSpark(TFunction<void(const TArray<FString>& Variants, const FString& Error)> OnDone);

	/** Suggest: Scenario's recommendation for the prompt. */
	void Suggest(TFunction<void(const TArray<TPair<FString, FString>>& Models, const FString& Error)> OnDone);

	FSimpleMulticastDelegate OnChanged;

private:
	void LoadSchema();
	void SchemaLoaded(const TArray<FScenarioField>& NewFields);
	void Changed(bool bReprice);
	void SchedulePrice(float Delay);
	bool OnDebounce(float DeltaSeconds);
	void PriceNow(bool bAutomatic);
	void Submit();
	void OnSessionChanged();
	void Subscribe();
	FScenarioRunRequest SparkRequest() const;
	/** Values with the references written into their file fields. */
	TSharedRef<FJsonObject> ValuesWithReferences() const;
	FString Fingerprint() const;
	FScenarioReference* FindReference(int32 Id);
	const FScenarioField* FirstProblemField() const;

	FString LaneId = TEXT("image");
	FString Mode;
	FString ModelId;
	FString ModelNameOverride;
	FString WorkflowDescription;
	bool bWorkflow = false;
	bool bUnrealReady = true;
	TArray<FScenarioField> Fields;
	TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
	bool bLoadingSchema = false;
	FString SchemaError;
	int32 SchemaGeneration = 0;
	TArray<FScenarioReference> References;

	TSharedPtr<FJsonObject> PendingValues;
	struct FPendingReference
	{
		FString AssetId;
		FString Path;
		FString Kind;
		FString Label;
	};
	TArray<FPendingReference> PendingReferences;
	void AttachPendingReferences();
	int32 NextReferenceId = 1;

	/** The request last priced, and its price: shown on the button until the form changes. */
	FString PricedFingerprint;
	FString ExpectedExact;
	double ExpectedCU = 0.0;
	bool bPricing = false;
	int32 PriceGeneration = 0;
	FScenarioError PriceError;
	bool bGenerateAfterPrice = false;
	bool bSubmitting = false;
	/** The button says "Waiting for Scenario..." until the job is confirmed plus 2 s. */
	double WaitingUntil = 0.0;
	FString LastLocalId;
	FString LastError;
	mutable FString CachedFingerprint;
	mutable bool bFingerprintDirty = true;
	/** Connection and scope seen last, so ledger updates do not trigger price checks. */
	FString SessionKey;
	/** A free price of the model with the lane's example prompt, shown as "~30 CU" before a prompt is typed. */
	double EstimateCU = -1.0;
	FString EstimateModel;
	bool bEstimating = false;
	void Estimate();
	/** The lane the last request was sent from: its status line shows there only. */
	FString SentLane;

	TArray<FScenarioWorkflowItem> Workflows;
	bool bLoadingWorkflows = false;
	FString WorkflowsError;
	FString WorkflowsFor;

	FTSTicker::FDelegateHandle DebounceHandle;
	FDelegateHandle SessionHandle;
	FDelegateHandle CatalogHandle;
};
