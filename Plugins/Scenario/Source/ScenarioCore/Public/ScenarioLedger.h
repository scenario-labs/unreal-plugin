// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FScenarioJobInfo;

enum class EScenarioJobState : uint8
{
	Submitting,
	Queued,
	Running,
	Success,
	Failed,
	Blocked,
	CancelRequested,
	Canceled,
	Unknown,
	Rejected,
	Dismissed,
};

SCENARIOCORE_API const TCHAR* LexToString(EScenarioJobState State);
SCENARIOCORE_API EScenarioJobState ScenarioJobStateFromString(const FString& Text);

/** One output file of a job, as it moves from Scenario to the project. */
struct SCENARIOCORE_API FScenarioJobFile
{
	FString AssetId;
	/** image, video, audio, 3d, image-hdr... */
	FString Kind;
	/** Role for material maps (albedo, normal...), empty otherwise. */
	FString Role;
	FString MimeType;
	/** Absolute path of the finished download, empty until downloaded. */
	FString LocalPath;
	int64 Size = 0;
	FString Sha256;
	/** Unreal object paths created from this file. */
	TArray<FString> ImportedObjects;
	FString Error;
};

/** One generation, from intent to imported assets. */
struct SCENARIOCORE_API FScenarioJobRow
{
	FString LocalId;
	FString JobId;
	EScenarioJobState State = EScenarioJobState::Submitting;
	/** model, workflow or prompt_spark. */
	FString Kind = TEXT("model");
	/** Lane id (image, 3d, material...). */
	FString Lane;
	FString ModelId;
	FString ModelName;
	FString WorkflowId;
	FString Prompt;
	TSharedPtr<FJsonObject> Parameters;
	TArray<FString> InputAssetIds;
	FString Fingerprint;
	double QuotedCU = 0.0;
	FString QuotedExact;
	/** -1 until Scenario reports it. */
	double ChargedCU = -1.0;
	double Progress = 0.0;
	TArray<FString> AssetIds;
	TArray<FScenarioJobFile> Files;
	bool bImported = false;
	/** person, agent or script; Client names the agent client when known. */
	FString Source = TEXT("person");
	FString Client;
	FString TeamId;
	FString ProjectId;
	/** Content Browser folder chosen for this run, empty for the default. */
	FString Destination;
	FString Error;
	double CreatedAt = 0.0;
	double UpdatedAt = 0.0;
	/** Bumped on every change, so views and agents can tell a row moved. */
	int32 Revision = 0;

	bool IsActive() const;
	bool IsUnresolved() const;
	bool IsFinal() const;

	TSharedRef<FJsonObject> ToJson() const;
	static bool FromJson(const TSharedPtr<FJsonObject>& Object, FScenarioJobRow& Out);
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnScenarioLedgerChanged, const FString& /*LocalId*/);

/**
 * Durable job ledger: survives editor restarts.
 *
 * The intent is written with state Submitting BEFORE the paid request is sent.
 * A row still Submitting when the ledger loads means the editor stopped
 * mid-request: it becomes Unknown and blocks the same request until it is
 * matched to a Scenario job or dismissed by the user.
 */
class SCENARIOCORE_API FScenarioLedger
{
public:
	static constexpr int32 MaxRows = 300;
	/** A server job counts as the match of an Unknown row if created this close after the intent. */
	static constexpr double ReconcileWindowSeconds = 180.0;
	static constexpr int32 FormatVersion = 1;

	explicit FScenarioLedger(const FString& InPath = FString());

	void Load();
	bool Save() const;
	const FString& GetPath() const { return Path; }

	FString AddIntent(const FScenarioJobRow& Template, double Now);
	void MarkSubmitted(const FString& LocalId, const FString& JobId, const FString& ServerStatus, double Now);
	/** Scenario refused clearly: nothing ran, nothing was charged. */
	void MarkRejected(const FString& LocalId, const FString& Message, double Now);
	void MarkUnknown(const FString& LocalId, const FString& Message, double Now);
	void MarkCancelRequested(const FString& LocalId, double Now);
	void Dismiss(const FString& LocalId, double Now);
	/** The person says the uncertain request never reached Scenario. */
	void MarkNotSent(const FString& LocalId, double Now);
	void SetError(const FString& LocalId, const FString& Message, double Now);
	void SetFiles(const FString& LocalId, const TArray<FScenarioJobFile>& Files, double Now);
	void MarkImported(const FString& LocalId, double Now);
	/** Links a Scenario job the plugin did not submit (recovery from the cloud history). */
	FString AddRecovered(const FScenarioJobInfo& Job, const FString& Lane, double Now);

	/** Applies a job row from job_get or jobs_wait. Returns the ledger row or null. */
	FScenarioJobRow* ApplyJob(const FScenarioJobInfo& Job, double Now);

	/**
	 * Links unresolved rows to jobs found on Scenario. Only an unambiguous match
	 * (exactly one untracked job of the same model in the window after the
	 * intent) is linked; anything else stays for the person. Returns the count.
	 */
	int32 Reconcile(const TArray<FScenarioJobInfo>& ServerJobs, double Now);

	FScenarioJobRow* Find(const FString& LocalId);
	const FScenarioJobRow* Find(const FString& LocalId) const;
	FScenarioJobRow* FindByJob(const FString& JobId);
	const TArray<FScenarioJobRow>& Rows() const { return Jobs; }
	TArray<FScenarioJobRow> Active() const;
	TArray<FScenarioJobRow> Unresolved() const;

	/** True when an unresolved row holds the same request: sending it again could pay twice. */
	bool Blocks(const FString& Fingerprint) const;

	static EScenarioJobState StateForStatus(const FString& Status, const FString& ErrorText);

	FOnScenarioLedgerChanged OnChanged;

private:
	void Touch(FScenarioJobRow& Row, double Now);
	void SetState(const FString& LocalId, EScenarioJobState State, const FString& Message, double Now);
	void Trim();

	FString Path;
	TArray<FScenarioJobRow> Jobs;
	int32 Counter = 0;
};
