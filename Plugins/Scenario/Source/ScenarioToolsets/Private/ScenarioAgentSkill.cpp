// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioAgentSkill.h"

#if WITH_SCENARIO_TOOLSETS

#include "ScenarioLanes.h"
#include "ScenarioSession.h"
#include "ScenarioSettings.h"
#include "ScenarioSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ScenarioAgentSkill)

UScenarioAgentSkill::UScenarioAgentSkill()
{
	Description = TEXT("Generate images, video, 3D models, PBR materials, audio and skies with Scenario and import them into this project (toolset ScenarioToolsets.ScenarioToolset). Read before pricing or generating.");

	Instructions = TEXT(
		"Scenario makes assets; the toolset ScenarioToolsets.ScenarioToolset prices them, runs them and lands them here.\n"
		"\n"
		"State now:\n"
		"{State}\n"
		"\n"
		"Spending (Creative Units, CU):\n"
		"- Price first: EstimateCost or EstimateWorkflow is free and returns quote_id and cu_cost_exact. Tell the person the cost.\n"
		"- Then Generate or RunWorkflow with that quote_id and ApprovedCost equal to cu_cost_exact, character for character. A quote is single use, lasts 120 s and matches one exact request: change a parameter, price again.\n"
		"- The answer is pending_approval with an approval_id: the person approves or declines in the editor. Follow it with WaitForJob or GetJobStatus; it becomes the job once sent. Declined or expired means nothing was sent.\n"
		"- Never repeat a paid call to retry. After an error, a timeout or an unknown outcome, look with GetJobStatus, ListJobs or CheckUnknownJob. An unknown job blocks the identical request until it is checked.\n"
		"\n"
		"Landing results: WaitForJob until delivered, PrepareResult (changes nothing, read its review), ApplyResult (one undo step), then SaveChanges. Nothing is on disk before SaveChanges. When ApplyResult answers pending_approval, the person applies it in the editor.\n"
		"\n"
		"Hand over: lighting, materials beyond the imported one, Blueprints, level layout and Python belong to the engine's other toolsets and skills. Scenario tools make, import and place assets only."
	);
}

FString UScenarioAgentSkill::StateText()
{
	TArray<FString> Lines;
	UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
	if (!Subsystem)
	{
		Lines.Add(TEXT("- Scenario is not loaded in this editor."));
		return FString::Join(Lines, TEXT("\n"));
	}
	const FScenarioSession& Session = Subsystem->Session();
	if (Session.IsReady())
	{
		Lines.Add(FString::Printf(TEXT("- Signed in; project: %s."), *Session.GetWorkspace().Label()));
	}
	else if (Session.IsSignedIn())
	{
		Lines.Add(TEXT("- Signed in, no project chosen: the person picks one in the Scenario Studio (Window > Scenario Studio). Only GetStatus and job tools work until then."));
	}
	else
	{
		Lines.Add(TEXT("- Not signed in: the person signs in from the Scenario Studio (Window > Scenario Studio). Only GetStatus and job tools work until then."));
	}

	const UScenarioEditorSettings& Settings = UScenarioEditorSettings::Get();
	Lines.Add(Settings.bAllowAgentChanges
		? FString(TEXT("- Change this project: on. ApplyResult and SaveChanges run directly."))
		: FString(TEXT("- Change this project: off. ApplyResult becomes a proposal the person applies.")));

	Lines.Add(Session.IsSpendingDisabled()
		? FString(TEXT("- Spending is off (SCENARIO_NO_SPEND=1): prices work, paid calls are refused."))
		: FString(TEXT("- Every paid call waits for the person's approval in the editor.")));

	TArray<FString> LaneIds;
	for (const FScenarioLane& Lane : FScenarioLanes::Get().All())
	{
		LaneIds.Add(Lane.Id);
	}
	if (LaneIds.Num() > 0)
	{
		Lines.Add(FString::Printf(TEXT("- Lanes for ListModels and EstimateCost: %s."), *FString::Join(LaneIds, TEXT(", "))));
	}
	return FString::Join(Lines, TEXT("\n"));
}

FString UScenarioAgentSkill::GeneratePrompt_Implementation(const FString& InitialInstructions) const
{
	return InitialInstructions.Replace(TEXT("{State}"), *StateText());
}

#endif
