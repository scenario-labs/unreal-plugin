// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

using UnrealBuildTool;

// The only runtime module: three small actors for game prototypes (pickup, goal, rules with a HUD).
// Everything else in the plugin is editor-only and never reaches a packaged game.
public class ScenarioGameplay : ModuleRules
{
	public ScenarioGameplay(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
		});
	}
}
