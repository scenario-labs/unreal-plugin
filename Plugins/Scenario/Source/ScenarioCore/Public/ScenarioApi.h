// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioMcpClient.h"
#include "ScenarioTypes.h"

template <typename T>
struct TScenarioResult
{
	bool bOk = false;
	T Value;
	FScenarioError Error;

	static TScenarioResult Ok(T InValue)
	{
		TScenarioResult Result;
		Result.bOk = true;
		Result.Value = MoveTemp(InValue);
		return Result;
	}

	static TScenarioResult Fail(const FScenarioError& InError)
	{
		TScenarioResult Result;
		Result.Error = InError;
		return Result;
	}

	static TScenarioResult Fail(EScenarioErrorKind Kind, const FString& Message)
	{
		TScenarioResult Result;
		Result.Error.Kind = Kind;
		Result.Error.Message = Message;
		return Result;
	}
};

struct SCENARIOCORE_API FScenarioEstimate
{
	double CU = 0.0;
	double Discount = 0.0;
	/** The exact decimal string of CU, as carried by quotes and approvals. */
	FString Exact;
};

struct SCENARIOCORE_API FScenarioSubmitted
{
	FString JobId;
	FString Status;
};

enum class EScenarioJobPhase : uint8
{
	Queued,
	Running,
	Success,
	Failure,
	Canceled,
};

struct SCENARIOCORE_API FScenarioJobInfo
{
	FString JobId;
	FString JobType;
	FString RawStatus;
	EScenarioJobPhase Phase = EScenarioJobPhase::Queued;
	/** 0 to 1. */
	double Progress = 0.0;
	FString ModelId;
	FString WorkflowId;
	TArray<FString> AssetIds;
	/** Charged CU, or -1 while unknown. */
	double ChargedCU = -1.0;
	double CreatedAt = -1.0;
	FString Error;
	FString Hint;

	bool IsTerminal() const { return Phase == EScenarioJobPhase::Success || Phase == EScenarioJobPhase::Failure || Phase == EScenarioJobPhase::Canceled; }

	static EScenarioJobPhase PhaseFromStatus(const FString& Status);
	static FScenarioJobInfo FromJson(const TSharedPtr<FJsonObject>& Object);
};

struct SCENARIOCORE_API FScenarioAssetInfo
{
	FString AssetId;
	FString MimeType;
	/** image, video, audio, 3d, image-hdr, document, json... */
	FString Kind;
	/** metadata.type, for example "inference-txt2img" or "texture-albedo". */
	FString Type;
	FString Name;
	int32 Width = 0;
	int32 Height = 0;
	FString ParentJobId;
	FString ModelId;
	FString Prompt;
	TSharedPtr<FJsonObject> Raw;

	static FScenarioAssetInfo FromJson(const TSharedPtr<FJsonObject>& Object);
};

struct SCENARIOCORE_API FScenarioProject
{
	FString Id;
	FString Name;
};

struct SCENARIOCORE_API FScenarioTeam
{
	FString Id;
	FString Name;
	FString Plan;
	TArray<FScenarioProject> Projects;
};

struct SCENARIOCORE_API FScenarioModelSummary
{
	FString Id;
	FString Name;
	FString ShortDescription;
	FString Type;
	FString Privacy;
	TArray<FString> Capabilities;
	TArray<FString> Tags;
	FString ThumbnailUrl;
	/** complianceMetadata.modelProvider (OpenAI, Meshy...), empty for trained models. */
	FString Provider;
	/** trained when ready; training or failed for trained models that cannot run yet. */
	FString Status;
	double CreatedAt = -1.0;

	static FScenarioModelSummary FromJson(const TSharedPtr<FJsonObject>& Object);
};

struct SCENARIOCORE_API FScenarioModelPage
{
	TArray<FScenarioModelSummary> Models;
	FString NextPageToken;
};

struct SCENARIOCORE_API FScenarioAssetPage
{
	TArray<FScenarioAssetInfo> Assets;
	FString NextPageToken;
};

struct SCENARIOCORE_API FScenarioWaitResult
{
	TArray<FScenarioJobInfo> Jobs;
	TArray<FString> PendingJobIds;
};

struct SCENARIOCORE_API FScenarioUploadTicket
{
	FString UploadId;
	/** Part number and presigned URL. */
	TArray<TPair<int32, FString>> Parts;
	int64 PartSize = 0;
	/** Set when the server finished the upload in one step. */
	FString AssetId;
};

/** Typed wrappers over the Scenario tools the plugin uses. All callbacks run on the game thread. */
class SCENARIOCORE_API FScenarioApi
{
public:
	template <typename T>
	using TCallback = TFunction<void(const TScenarioResult<T>&)>;

	explicit FScenarioApi(TSharedRef<IScenarioBackend> InBackend);

	IScenarioBackend& Backend() const { return *BackendRef; }
	TSharedRef<IScenarioBackend> BackendShared() const { return BackendRef; }

	/** Free price check: model_run with dry_run. */
	void Estimate(const FString& ModelId, const TSharedPtr<FJsonObject>& Parameters, TCallback<FScenarioEstimate> OnDone);
	/** Paid. Sent exactly once. */
	void Submit(const FString& ModelId, const TSharedPtr<FJsonObject>& Parameters, TCallback<FScenarioSubmitted> OnDone);

	void EstimateWorkflow(const FString& WorkflowId, const TSharedPtr<FJsonObject>& Inputs, TCallback<FScenarioEstimate> OnDone);
	void SubmitWorkflow(const FString& WorkflowId, const TSharedPtr<FJsonObject>& Inputs, TCallback<FScenarioSubmitted> OnDone);

	/** Prompt Spark price (free) and run (paid). The run returns Scenario's reply (prompts, calls and recommended_call). */
	void EstimatePromptSpark(const FString& ModelId, const FString& Prompt, const TArray<FString>& ImageIds, int32 NumResults, TCallback<FScenarioEstimate> OnDone);
	void RunPromptSpark(const FString& ModelId, const FString& Prompt, const TArray<FString>& ImageIds, int32 NumResults, FScenarioReplyCallback OnDone);

	void GetJob(const FString& JobId, TCallback<FScenarioJobInfo> OnDone);
	void WaitJobs(const TArray<FString>& JobIds, TCallback<FScenarioWaitResult> OnDone);
	void ListJobs(int32 Limit, TCallback<TArray<FScenarioJobInfo>> OnDone);
	void CancelJob(const FString& JobId, FScenarioReplyCallback OnDone);

	void GetAsset(const FString& AssetId, TCallback<FScenarioAssetInfo> OnDone);
	/** Signed download URL; Format converts images and 3D (png, jpg, webp, glb, fbx, obj), empty keeps the original. */
	void DownloadUrl(const FString& AssetId, const FString& Format, TCallback<FString> OnDone);

	void GetSchema(const FString& ModelId, FScenarioReplyCallback OnDone);
	/** models_list page. Privacy "public" or "private"; Modality image, video, 3d, audio or empty. */
	void ListModels(const FString& Privacy, const FString& Modality, int32 Limit, const FString& PageToken, TCallback<FScenarioModelPage> OnDone);
	/** search target=models with URLs, used for thumbnails and text search. */
	void SearchModels(const FString& Query, bool bPublic, int32 Limit, TCallback<TArray<FScenarioModelSummary>> OnDone);
	void SearchAssets(const FString& Query, const FString& Kind, int32 Limit, int32 Offset, TCallback<TArray<FScenarioAssetInfo>> OnDone);
	/** The project's assets, newest first, read from the project itself (assets_list). Kind image, 3d, audio, video or empty. */
	void ListAssets(const FString& Kind, int32 Limit, const FString& PageToken, TCallback<FScenarioAssetPage> OnDone);
	void Recommend(const FString& Capability, const FString& Prompt, int32 Limit, FScenarioReplyCallback OnDone);

	void ListTeams(TCallback<TArray<FScenarioTeam>> OnDone);
	void ListProjects(const FString& TeamId, TCallback<TArray<FScenarioProject>> OnDone);

	/** CU used (after discounts) between the two ISO dates. The project id is needed even with an API key (checked 2026-10-18). */
	void UsageTotal(const FString& ProjectId, const FString& StartDate, const FString& EndDate, TCallback<double> OnDone);

	/** Tags filters the list ("featured" for Scenario's picks in the public catalog). */
	void ListWorkflows(const FString& Privacy, const FString& Status, int32 Limit, const FString& PageToken, const TArray<FString>& Tags, FScenarioReplyCallback OnDone);
	void GetWorkflow(const FString& WorkflowId, FScenarioReplyCallback OnDone);
	void ListCollections(FScenarioReplyCallback OnDone);

	/** Multipart upload: start (creates an upload, costs no CU), then PUT parts, then complete. */
	void UploadStart(const FString& FileName, const FString& ContentType, const FString& Kind, int64 FileSize, TCallback<FScenarioUploadTicket> OnDone);
	void UploadComplete(const FString& UploadId, TCallback<FString> OnDone);

	/** Finds the CU price in a dry-run reply, at the top level or one level down. */
	static bool FindEstimate(const TSharedPtr<FJsonObject>& Value, FScenarioEstimate& Out);

	/** The prompt variants of a Prompt Spark reply: prompts, else the prompts of calls, else recommended_prompt. */
	static TArray<FString> SparkPrompts(const TSharedPtr<FJsonObject>& Reply);

private:
	static TSharedPtr<FJsonObject> Args();
	TSharedRef<IScenarioBackend> BackendRef;
};
