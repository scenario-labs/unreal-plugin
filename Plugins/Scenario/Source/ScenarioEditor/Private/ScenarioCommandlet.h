// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ScenarioCommandlet.generated.h"

/**
 * Scenario from the command line, for CI. Credentials come from
 * SCENARIO_API_KEY and SCENARIO_API_SECRET; SCENARIO_NO_SPEND=1 refuses paid calls.
 *
 * The name is module-qualified: ScenarioEditor loads after engine init, later than
 * the commandlet lookup, and "ScenarioEditor." makes the engine load it first.
 *
 * One action:
 *   UnrealEditor-Cmd Game.uproject -run=ScenarioEditor.Scenario -Action=list_models -Args="{\"lane\":\"image\"}"
 *
 * Price, run, wait, import and save in one go. -MaxCU is required: the run is
 * sent only when the quote is at most that many Creative Units.
 *   UnrealEditor-Cmd Game.uproject -run=ScenarioEditor.Scenario -Generate -Model=<model id>
 *     -Params=@icon.json -MaxCU=5 -Destination=/Game/UI/Icons -Import -Save
 *
 * Every step prints one line of JSON. Exit code 0 on success.
 */
UCLASS()
class UScenarioCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UScenarioCommandlet();
	virtual int32 Main(const FString& Params) override;
};
