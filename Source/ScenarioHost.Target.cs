// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

using UnrealBuildTool;

public class ScenarioHostTarget : TargetRules
{
	public ScenarioHostTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ScenarioHost");
	}
}
