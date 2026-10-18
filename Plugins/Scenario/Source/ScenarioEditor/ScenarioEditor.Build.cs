// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

using System.IO;
using UnrealBuildTool;

public class ScenarioEditor : ModuleRules
{
	public ScenarioEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private", "UI"));
		PrivateIncludePathModuleNames.Add("Settings");

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"DeveloperSettings",
			"EditorSubsystem",
			"ScenarioCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"AssetRegistry",
			"AssetTools",
			"ContentBrowser",
			"ContentBrowserData",
			"DesktopPlatform",
			"EditorFramework",
			"GLTFExporter",
			"HTTP",
			"ImageCore",
			"ImageWrapper",
			"InputCore",
			"InterchangeCore",
			"InterchangeEngine",
			"LevelEditor",
			"MaterialEditor",
			"MediaAssets",
			"Projects",
			"PropertyEditor",
			"RenderCore",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"ToolWidgets",
			"UnrealEd",
			"WorkspaceMenuStructure",
		});
	}
}
