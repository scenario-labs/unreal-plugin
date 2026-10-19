// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class AScenarioGameRules;

/** The game's HUD: title, objective, progress, timer and messages. Reads the rules every frame. */
class SScenarioGameHud : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioGameHud) {}
		SLATE_ARGUMENT(AScenarioGameRules*, Rules)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TWeakObjectPtr<AScenarioGameRules> Rules;
};
