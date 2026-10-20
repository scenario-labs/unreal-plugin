// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"

// Epic's Toolset Registry exists from Unreal 5.8. This guard also hides the classes
// from the header tool of older engines; WITH_SCENARIO_TOOLSETS guards the code.
#if !UE_VERSION_OLDER_THAN(5, 8, 0)

#include "Dom/JsonObject.h"
#include "ScenarioActions.h"
#include "ToolsetRegistry/ToolCallAsyncResult.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "ScenarioToolset.generated.h"

/** Completes with the JSON object a Scenario tool returns. */
UCLASS()
class UScenarioToolResult : public UToolCallAsyncResult
{
	GENERATED_BODY()

public:
	/** The tool's answer: a JSON object (sent as an object, not as text). */
	UPROPERTY()
	FString Value;

	bool SetValue(const FString& InJson)
	{
		return MaybeBroadcastSuccessfulCompletion(FString(InJson), Value);
	}

	virtual TSharedPtr<FJsonValue> GetValueAsJson() const override;
};

/**
 * Scenario: price and run Scenario models and workflows (image, video, 3D, material, audio, sky),
 * follow jobs and import results into this project. Costs are in Creative Units (CU). Generate and
 * RunWorkflow answer pending_approval: they are sent only after the person approves in the editor.
 * Skill: /Script/ScenarioToolsets.ScenarioAgentSkill.
 */
UCLASS()
class UScenarioToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Sign-in state, Scenario team and project, permissions and job counts. Call first.
	 * @return Status object.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* GetStatus();

	/**
	 * Scenario models for a lane, the lane's picks first. At most 40 rows.
	 * @param Lane image, video, 3d, material, audio, environment, render, or all.
	 * @param Modality image, video, 3d, audio, or all.
	 * @param Query Text matched against name, provider and description; * for no filter.
	 * @param Limit Rows, default 20.
	 * @return models with id, name, capabilities and tags.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* ListModels(const FString& Lane = TEXT("all"), const FString& Modality = TEXT("all"), const FString& Query = TEXT("*"), int32 Limit = 20);

	/**
	 * A model's parameters: types, defaults, options, limits, which change the price. File inputs take Scenario asset ids.
	 * @param ModelId Id from ListModels.
	 * @return parameters, plugin defaults and, for 3D picks, unreal_ready_prompt to append.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* GetModelSchema(const FString& ModelId);

	/**
	 * Scenario's model recommendation for a prompt.
	 * @param Prompt What to make.
	 * @param Lane A lane as in ListModels, or auto.
	 * @return The recommendation.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* RecommendModels(const FString& Prompt, const FString& Lane = TEXT("auto"));

	/**
	 * Free price check of a model run. Quotes are single use and last 120 s.
	 * @param ModelId Id from ListModels.
	 * @param Lane Where results land; auto infers it from the model.
	 * @param Parameters JSON object text of values by parameter name, for example {"prompt": "a stone well"}. Plugin defaults fill the rest.
	 * @return quote_id and cu_cost_exact, which Generate needs.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* EstimateCost(const FString& ModelId, const FString& Lane = TEXT("auto"), const FString& Parameters = TEXT("{}"));

	/**
	 * SPENDS Creative Units once the person approves in the editor. Runs the request priced by EstimateCost.
	 * @param QuoteId quote_id from EstimateCost.
	 * @param ApprovedCost The quote's cu_cost_exact, character for character.
	 * @param Destination Content folder for the import, for example /Game/Props; default is the plugin's folder.
	 * @return state pending_approval and approval_id; GetJobStatus with it returns the job once sent.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* Generate(const FString& QuoteId, const FString& ApprovedCost, const FString& Destination = TEXT("default"));

	/**
	 * Ready workflows: the team's own, Scenario's featured ones, or both.
	 * @param Source all, team or featured.
	 * @param Limit Rows per source, default 20.
	 * @return workflows with workflow_id, name and input count.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* ListWorkflows(const FString& Source = TEXT("all"), int32 Limit = 20);

	/**
	 * A workflow's inputs, in the same form as GetModelSchema.
	 * @param WorkflowId Id from ListWorkflows.
	 * @return parameters.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* GetWorkflowSchema(const FString& WorkflowId);

	/**
	 * Free price check of a workflow run.
	 * @param WorkflowId Id from ListWorkflows.
	 * @param Inputs JSON object text of values by input name.
	 * @return quote_id and cu_cost_exact, which RunWorkflow needs.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* EstimateWorkflow(const FString& WorkflowId, const FString& Inputs = TEXT("{}"));

	/**
	 * SPENDS Creative Units once the person approves in the editor. Runs the workflow priced by EstimateWorkflow.
	 * @param QuoteId quote_id from EstimateWorkflow.
	 * @param ApprovedCost The quote's cu_cost_exact, character for character.
	 * @param Destination Content folder for the import; default is the plugin's folder.
	 * @param WorkflowName Name for the job list; auto uses the workflow's.
	 * @return state pending_approval and approval_id.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* RunWorkflow(const FString& QuoteId, const FString& ApprovedCost, const FString& Destination = TEXT("default"), const FString& WorkflowName = TEXT("auto"));

	/**
	 * A job's state, progress, cost, files and the next step. Also takes an approval_id from Generate or RunWorkflow.
	 * @param JobId job_id or approval_id.
	 * @return The job, or the approval while the person has not answered.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* GetJobStatus(const FString& JobId);

	/**
	 * Waits until a job is finished and its files are downloaded, then returns its status.
	 * @param JobId job_id or approval_id.
	 * @param TimeoutSeconds Default 45, max 160. Returns still_running when not done: call again.
	 * @return The job.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* WaitForJob(const FString& JobId, int32 TimeoutSeconds = 45);

	/**
	 * This project's jobs, newest first, from people, agents and scripts. Survives restarts.
	 * @param State all, active, unresolved or finished.
	 * @param Limit Rows, default 20, max 50.
	 * @return jobs.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* ListJobs(const FString& State = TEXT("all"), int32 Limit = 20);

	/**
	 * Asks Scenario to cancel a queued or running job. Scenario decides on refunds.
	 * @param JobId job_id.
	 * @return The job.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* CancelJob(const FString& JobId);

	/**
	 * Matches jobs whose outcome is unknown against Scenario's job list. Use this, never a second paid call.
	 * @param JobId A job_id to report on, or all.
	 * @return linked and still_unresolved counts.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* CheckUnknownJob(const FString& JobId = TEXT("all"));

	/**
	 * Assets in the Scenario project, newest first, or matching a text search. Their ids go into file parameters.
	 * @param Query Text to search; * lists the newest.
	 * @param Kind image, video, 3d, audio, or all.
	 * @param Limit Rows, default 20, max 50.
	 * @param Offset Rows to skip.
	 * @return assets with asset_id.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* SearchAssets(const FString& Query = TEXT("*"), const FString& Kind = TEXT("all"), int32 Limit = 20, int32 Offset = 0);

	/**
	 * Plans how a finished job lands. Changes nothing.
	 * @param JobId A delivered job.
	 * @param Purpose import, place, material, environment, media_plate or mesh_edit.
	 * @param Placement For place: auto, in_front_of_camera, at_pivot, at_location or on_selection.
	 * @param Location World position in cm as x,y,z, for example 0,250,100. Read for placement at_location only.
	 * @param ActorIds Comma-separated actor names or labels (material, media_plate, mesh_edit, at_pivot), or selection.
	 * @param Slot Material slot.
	 * @return application_id and the review text.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* PrepareResult(const FString& JobId, const FString& Purpose = TEXT("place"), const FString& Placement = TEXT("auto"), const FString& Location = TEXT("none"), const FString& ActorIds = TEXT("selection"), int32 Slot = 0);

	/**
	 * Applies a prepared plan once, in one undo step. Without "Change this project" the person gets a proposal and the answer is pending_approval.
	 * @param ApplicationId application_id from PrepareResult.
	 * @return What changed and the import checks.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* ApplyResult(const FString& ApplicationId);

	/**
	 * Saves the packages the plugin created or changed this session. Nothing is on disk before this.
	 * @param bIncludeLevels Also save the open level and its World Partition actors.
	 * @return Each file written.
	 */
	UFUNCTION(meta = (AICallable))
	static UScenarioToolResult* SaveChanges(bool bIncludeLevels = false);

	/** The registry action each tool runs, by tool name (tests and renaming). */
	static const TMap<FString, FString>& ActionsByTool();

	/** The caller every tool runs as: an agent whose paid calls wait for the person. */
	static FScenarioCaller Caller();
};

#endif
