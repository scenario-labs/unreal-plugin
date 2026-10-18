// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

using UnrealBuildTool;

public class ScenarioHost : ModuleRules
{
	public ScenarioHost(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
	}
}
