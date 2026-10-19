// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

using System.IO;
using UnrealBuildTool;

public class ScenarioEditor : ModuleRules
{
	public ScenarioEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Every file compiles on its own: helpers in anonymous namespaces may share names across files.
		bUseUnity = false;
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private", "UI"));
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private", "Actions"));
		// Interfaces only, checked at runtime: the Python plugin stays optional.
		PrivateIncludePathModuleNames.AddRange(new string[] { "Settings", "PythonScriptPlugin" });

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
			"EditorWidgets",
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
