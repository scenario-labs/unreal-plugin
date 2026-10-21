// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Modules/ModuleManager.h"

#if WITH_SCENARIO_TOOLSETS
#include "Editor.h"
#include "ScenarioToolset.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#endif

/**
 * Registers the Scenario toolset with Epic's Toolset Registry (Unreal 5.8 and later,
 * when the ToolsetRegistry plugin is enabled). The agent skill needs no registration:
 * the registry finds every UAgentSkill class. Without the registry this module is empty.
 */
class FScenarioToolsetsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if WITH_SCENARIO_TOOLSETS
		// No editor (commandlets): nothing to register, and no warning from the registry.
		if (GEditor && UToolsetRegistry::IsAvailable())
		{
			UToolsetRegistry::RegisterToolsetClass(UScenarioToolset::StaticClass());
			bRegistered = true;
		}
#endif
	}

	virtual void ShutdownModule() override
	{
#if WITH_SCENARIO_TOOLSETS
		if (bRegistered && UObjectInitialized())
		{
			UToolsetRegistry::UnregisterToolsetClass(UScenarioToolset::StaticClass());
		}
		bRegistered = false;
#endif
	}

private:
	bool bRegistered = false;
};

IMPLEMENT_MODULE(FScenarioToolsetsModule, ScenarioToolsets)
