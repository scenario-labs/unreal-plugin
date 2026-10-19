// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FScenarioError;

/** Who calls an action, and what they may do. */
struct SCENARIOEDITOR_API FScenarioCaller
{
	/** person, agent or script. */
	FString Source = TEXT("agent");
	/** The agent client from initialize (claude-code, codex...) or the script's name. */
	FString Client;
	/** Import, place, save and remove. Agents: "Change this project" on the Agent page. */
	bool bCanChangeProject = false;
	/** Agents: "Allow agents to run Python". */
	bool bCanRunPython = false;

	static FScenarioCaller Agent(const FString& Client);
	static FScenarioCaller Script(const FString& Client);
};

/** The result of one action: data for the caller, or a one-line error. */
struct SCENARIOEDITOR_API FScenarioActionResult
{
	bool bOk = false;
	/** "Type: message", no secrets and no paths outside the project. */
	FString Error;
	TSharedPtr<FJsonObject> Data;
	/** Optional PNG returned to the agent as an image (screenshots). */
	TArray<uint8> Png;
	/** Set on spends: the CU of the quote, for the activity log. */
	FString SpentExact;

	static FScenarioActionResult Ok(const TSharedPtr<FJsonObject>& InData);
	static FScenarioActionResult Fail(const FString& Type, const FString& Message);
	static FScenarioActionResult Fail(const FScenarioError& Error);
};

using FScenarioActionDone = TFunction<void(const FScenarioActionResult&)>;

enum class EScenarioActionKind : uint8
{
	/** Reads the level, the project or Scenario. Always allowed. */
	Read,
	/** Editor state only (selection, captures under Saved). Always allowed. */
	Editor,
	/** Costs Creative Units or sends data to Scenario. Needs a quote where it costs. */
	Spend,
	/** Changes the project (imports, actors, saves). Needs bCanChangeProject. */
	Change,
	/** Runs Python. Needs bCanChangeProject and bCanRunPython. */
	Python,
};

/** One operation shared by the Studio, agents (MCP tools), the script library and the commandlet. */
struct SCENARIOEDITOR_API FScenarioAction
{
	/** Verb-first snake case; the MCP tool name. */
	FString Name;
	/** Short: what it does and what it returns. Spending tools say so first. */
	FString Description;
	/** JSON Schema of the arguments. */
	TSharedPtr<FJsonObject> InputSchema;
	EScenarioActionKind Kind = EScenarioActionKind::Read;
	bool bDestructive = false;
	/** Charges Creative Units: asks the person first when "approve each agent spend" is on. */
	bool bCosts = false;
	/** A Change action that turns into a proposal for the person instead of failing when changes are off. */
	bool bProposeWhenDenied = false;
	/** Seconds a caller waits before giving up. */
	double TimeLimit = 120.0;
	/** Runs on the game thread. Calls Done exactly once, now or later. */
	TFunction<void(const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, FScenarioActionDone Done)> Run;
};

DECLARE_MULTICAST_DELEGATE_FiveParams(FOnScenarioActionCalled, const FString& /*Name*/, const TSharedPtr<FJsonObject>& /*Args*/, const FScenarioCaller& /*Caller*/, const FScenarioActionResult& /*Result*/, double /*Seconds*/);

/**
 * The action layer. Every operation an agent or a script can run is registered
 * here once, with its permission kind; Call checks the caller's permissions,
 * runs the action on the game thread and reports to the activity log.
 * The money contract itself lives in FScenarioSession, under every caller.
 */
class SCENARIOEDITOR_API FScenarioActions
{
public:
	static FScenarioActions& Get();

	void Register(FScenarioAction Action);
	const TArray<FScenarioAction>& All() const { return Actions; }
	const FScenarioAction* Find(const FString& Name) const;

	/** Runs an action. Done is called exactly once, on the game thread. */
	void Call(const FString& Name, const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, FScenarioActionDone Done);

	/** Every finished call, for the activity log and toasts. */
	FOnScenarioActionCalled OnCalled;

	/** Adds the agent server's fields (port, token, clients) to scenario_status. */
	TFunction<void(const TSharedRef<FJsonObject>& Status)> StatusExtension;

	/** Drops pending uploads, applications and approvals (module shutdown). */
	void Reset();

private:
	FScenarioActions();
	void RegisterScenarioActions();
	void RegisterLevelActions();
	void RegisterKitActions();

	TArray<FScenarioAction> Actions;
	TMap<FString, int32> IndexByName;
};
