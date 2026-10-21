// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

SCENARIOCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogScenario, Log, All);

/** Version of the plugin, sent in the User-Agent and stored in provenance. */
namespace ScenarioVersion
{
	SCENARIOCORE_API FString PluginVersion();
	SCENARIOCORE_API FString EngineVersion();
	/** "ScenarioUnreal/0.2.0 (Unreal 5.8.3; Mac)" */
	SCENARIOCORE_API FString UserAgent();
}

class FScenarioCoreModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};
