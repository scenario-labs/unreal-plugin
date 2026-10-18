// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioSettings.h"

UScenarioProjectSettings::UScenarioProjectSettings()
{
	OutputRoot.Path = TEXT("/Game/Scenario");
}

FString UScenarioProjectSettings::RootPath() const
{
	FString Path = OutputRoot.Path.TrimStartAndEnd();
	if (Path.IsEmpty())
	{
		return TEXT("/Game/Scenario");
	}
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (!Path.StartsWith(TEXT("/")))
	{
		Path = TEXT("/Game/") + Path;
	}
	while (Path.EndsWith(TEXT("/")))
	{
		Path.LeftChopInline(1);
	}
	return Path;
}

bool UScenarioProjectSettings::IsModelAllowed(const FString& ModelId) const
{
	if (BlockedModels.Contains(ModelId))
	{
		return false;
	}
	return AllowedModels.Num() == 0 || AllowedModels.Contains(ModelId);
}

FString UScenarioEditorSettings::LastModelFor(const FString& Lane) const
{
	const FString Prefix = Lane + TEXT("=");
	for (const FString& Entry : LastModels)
	{
		if (Entry.StartsWith(Prefix))
		{
			return Entry.RightChop(Prefix.Len());
		}
	}
	return FString();
}

void UScenarioEditorSettings::SetLastModel(const FString& Lane, const FString& ModelId)
{
	const FString Prefix = Lane + TEXT("=");
	LastModels.RemoveAll([&Prefix](const FString& Entry) { return Entry.StartsWith(Prefix); });
	LastModels.Add(Prefix + ModelId);
	SaveConfig();
}

void UScenarioEditorSettings::ToggleFavorite(const FString& ModelId)
{
	if (FavoriteModels.Remove(ModelId) == 0)
	{
		FavoriteModels.Add(ModelId);
	}
	SaveConfig();
}
