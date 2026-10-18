// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioCoreModule.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogScenario);

FString ScenarioVersion::PluginVersion()
{
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scenario")))
	{
		return Plugin->GetDescriptor().VersionName;
	}
	return TEXT("0.0.0");
}

FString ScenarioVersion::EngineVersion()
{
	const FEngineVersion& Version = FEngineVersion::Current();
	return FString::Printf(TEXT("%d.%d.%d"), Version.GetMajor(), Version.GetMinor(), Version.GetPatch());
}

FString ScenarioVersion::UserAgent()
{
	return FString::Printf(TEXT("ScenarioUnreal/%s (Unreal %s; %s)"), *PluginVersion(), *EngineVersion(), ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()));
}

IMPLEMENT_MODULE(FScenarioCoreModule, ScenarioCore);
