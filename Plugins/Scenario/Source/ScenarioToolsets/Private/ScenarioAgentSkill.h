// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"

#if !UE_VERSION_OLDER_THAN(5, 8, 0)

#include "ToolsetRegistry/AgentSkill.h"

#include "ScenarioAgentSkill.generated.h"

/**
 * How to use the Scenario toolset: what the tools cannot say themselves (the
 * money rules, apply then save, when to use other skills), with the editor's
 * state filled in when an agent reads it. Found by Epic's AgentSkillToolset.
 */
UCLASS()
class UScenarioAgentSkill : public UAgentSkill
{
	GENERATED_BODY()

public:
	UScenarioAgentSkill();

	/** The live state lines that replace {State} in the instructions. */
	static FString StateText();

protected:
	virtual FString GeneratePrompt_Implementation(const FString& InitialInstructions) const override;
};

#endif
