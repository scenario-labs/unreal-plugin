// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct SCENARIOCORE_API FScenarioLaneModel
{
	FString Id;
	FString Name;
	FString Note;
	/** Lane mode the model belongs to (text, image, edit, generate, hdri), empty for every mode. */
	FString Mode;
	/** Adds the Unreal-ready constraints to the prompt by default. */
	bool bUnrealReady = false;
};

struct SCENARIOCORE_API FScenarioLane
{
	FString Id;
	FString Title;
	/** Scenario capability used for recommendations (txt2img, txt23d...). */
	FString Capability;
	/** texture, mesh, material, sound, media, sky. */
	FString Importer;
	FString Folder;
	/** Download format to request (glb for meshes), empty for the original file. */
	FString DownloadFormat;
	FString Hint;
	TArray<FString> Modes;
	TArray<FString> StarterPrompts;
	TArray<FScenarioLaneModel> Models;
	bool bRecommend = true;
	bool bNeedsCapture = false;

	/** Models for one mode (all when Mode is empty). */
	TArray<FScenarioLaneModel> ModelsFor(const FString& Mode) const;
};

/**
 * The generation lanes and their default models, read from the plugin's
 * Resources/Lanes.json (the same file the free contract check reads).
 * Compiled-in fallbacks keep the Image lane usable if the file is missing.
 */
class SCENARIOCORE_API FScenarioLanes
{
public:
	static const FScenarioLanes& Get();

	/** Parses a Lanes.json document. Exposed for tests. */
	bool LoadFromString(const FString& Text);

	const TArray<FScenarioLane>& All() const { return Lanes; }
	const FScenarioLane* Find(const FString& Id) const;
	/** The lane a model belongs to, first match. */
	const FScenarioLane* LaneForModel(const FString& ModelId) const;
	FString DefaultModel(const FString& LaneId, const FString& Mode = FString()) const;

	/** Values set on top of the schema defaults for this model. */
	TSharedPtr<FJsonObject> Presets(const FString& ModelId) const;
	/** Allowed values per field name that Unreal can import (audioFormat...). */
	const TMap<FString, TArray<FString>>& Restrict() const { return RestrictMap; }
	const FString& UnrealReadyPrompt() const { return UnrealReady; }
	FString ToolModel(const FString& Tool) const;

private:
	void LoadDefaults();

	TArray<FScenarioLane> Lanes;
	TMap<FString, TSharedPtr<FJsonObject>> PresetMap;
	TMap<FString, TArray<FString>> RestrictMap;
	TMap<FString, FString> Tools;
	FString UnrealReady;
};
