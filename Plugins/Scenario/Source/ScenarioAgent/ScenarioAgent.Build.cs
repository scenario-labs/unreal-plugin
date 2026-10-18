// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

using UnrealBuildTool;

public class ScenarioAgent : ModuleRules
{
	public ScenarioAgent(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Every file compiles on its own: helpers in anonymous namespaces may share names across files.
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"ScenarioCore",
			"ScenarioEditor",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"InputCore",
			"Projects",
			"Slate",
			"SlateCore",
			"UnrealEd",
		});
	}
}
