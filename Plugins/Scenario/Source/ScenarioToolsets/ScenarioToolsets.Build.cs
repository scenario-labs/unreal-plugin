// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

using System.IO;
using UnrealBuildTool;

// Registers Scenario with Epic's Toolset Registry, so agents on Epic's Unreal MCP
// endpoint (list_toolsets, describe_toolset, call_tool) can use Scenario. The
// registry ships with Unreal 5.8 as an Experimental plugin; with an older engine
// this module builds empty and the other Scenario modules do not need it.
public class ScenarioToolsets : ModuleRules
{
	public ScenarioToolsets(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"ScenarioCore",
			"ScenarioEditor",
		});

		bool bHasRegistry = Directory.Exists(Path.Combine(EngineDirectory, "Plugins", "Experimental", "ToolsetRegistry"));
		bool bEngineHasIt = Target.Version.MajorVersion > 5 || (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 8);
		if (bEngineHasIt && !bHasRegistry)
		{
			// The headers' engine version guard expects the registry from 5.8 on.
			throw new BuildException("ScenarioToolsets needs Engine/Plugins/Experimental/ToolsetRegistry, which Unreal 5.8 installs. Reinstall the engine's Experimental plugins or remove the ScenarioToolsets module from Scenario.uplugin.");
		}
		if (bHasRegistry)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"ToolsetRegistry",
				"UnrealEd",
			});
		}
		PrivateDefinitions.Add("WITH_SCENARIO_TOOLSETS=" + (bHasRegistry && bEngineHasIt ? "1" : "0"));
	}
}
