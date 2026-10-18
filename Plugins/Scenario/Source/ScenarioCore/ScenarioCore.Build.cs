// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

using UnrealBuildTool;

public class ScenarioCore : ModuleRules
{
	public ScenarioCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Json",
			"HTTP",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Sockets",
			"Networking",
			"Projects",
		});

		if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			PublicFrameworks.AddRange(new string[] { "Security", "CoreFoundation" });
		}
		else if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("Advapi32.lib");
		}
	}
}
