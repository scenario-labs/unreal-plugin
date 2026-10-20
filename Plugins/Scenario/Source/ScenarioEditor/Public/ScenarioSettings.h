// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ScenarioSettings.generated.h"

UENUM()
enum class EScenarioMeshCollision : uint8
{
	None UMETA(DisplayName = "None"),
	Simple UMETA(DisplayName = "Simple box"),
	ComplexAsSimple UMETA(DisplayName = "Mesh shape (complex as simple)"),
};

/**
 * Project Settings > Plugins > Scenario. Shared by the team and committed with
 * the project (DefaultEditor.ini). Never holds a secret.
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Scenario"))
class SCENARIOEDITOR_API UScenarioProjectSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UScenarioProjectSettings();

	static const UScenarioProjectSettings& Get() { return *GetDefault<UScenarioProjectSettings>(); }

	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("Scenario"); }

	/** Content folder for everything Scenario imports. */
	UPROPERTY(config, EditAnywhere, Category = "Folders", meta = (ContentDir))
	FDirectoryPath OutputRoot;

	/** Adds a folder per day (2026-10-18) under each kind folder. */
	UPROPERTY(config, EditAnywhere, Category = "Folders")
	bool bDateFolders = true;

	/** Keeps the prompt out of the metadata stored in imported assets (it is committed with them). */
	UPROPERTY(config, EditAnywhere, Category = "Provenance")
	bool bOmitPromptFromAssets = false;

	/**
	 * Collision for imported static meshes. Mesh shape (the default) blocks
	 * where the mesh is, so the player walks through gates, arches and doors;
	 * Simple box is cheaper but closes them. Neither suits meshes that simulate
	 * physics: add simple shapes in the Static Mesh editor for those.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Import|Meshes")
	EScenarioMeshCollision MeshCollision = EScenarioMeshCollision::ComplexAsSimple;

	/**
	 * Builds imported static meshes as Nanite. A mesh with a translucent
	 * material slot is left without Nanite, which does not draw translucency.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Import|Meshes")
	bool bNanite = true;

	/**
	 * Smoothness maps (glossiness) become a roughness map at import:
	 * roughness = 1 - smoothness, read as linear data. The smoothness map is
	 * kept next to it. Turn off when a model's smoothness map already holds
	 * roughness values.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Import|Textures")
	bool bInvertSmoothness = true;

	/** Largest size for imported textures (0 keeps the source size). Panoramas always keep their size. */
	UPROPERTY(config, EditAnywhere, Category = "Import|Textures", meta = (ClampMin = "0", ClampMax = "16384"))
	int32 MaxTextureSize = 0;

	/** Saves imported asset packages right after the import (new files only). */
	UPROPERTY(config, EditAnywhere, Category = "Import")
	bool bSaveImportedAssets = true;

	/** Only these models can be used in this project (empty allows every model). */
	UPROPERTY(config, EditAnywhere, Category = "Models")
	TArray<FString> AllowedModels;

	/** These models are hidden in this project. */
	UPROPERTY(config, EditAnywhere, Category = "Models")
	TArray<FString> BlockedModels;

	/** Long package path of the output root, "/Game/Scenario" by default. */
	FString RootPath() const;
	bool IsModelAllowed(const FString& ModelId) const;
};

/**
 * Editor Preferences > Plugins > Scenario. Per user, never committed.
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Scenario"))
class SCENARIOEDITOR_API UScenarioEditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	static const UScenarioEditorSettings& Get() { return *GetDefault<UScenarioEditorSettings>(); }
	static UScenarioEditorSettings& GetMutable() { return *GetMutableDefault<UScenarioEditorSettings>(); }

	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("Scenario"); }

	/** Asks before a generation that costs more than this many Creative Units. 0 never asks. */
	UPROPERTY(config, EditAnywhere, Category = "Spending", meta = (ClampMin = "0"))
	float ConfirmAboveCU = 0.0f;

	/** Imports results as soon as they are downloaded. Off: results wait for Import on their card. */
	UPROPERTY(config, EditAnywhere, Category = "Results")
	bool bAutoImport = false;

	/** Shows the quick composer bar at the bottom of the level viewport. */
	UPROPERTY(config, EditAnywhere, Category = "Composer")
	bool bShowComposer = false;

	/** Starts the local MCP server for agents when the editor starts. */
	UPROPERTY(config, EditAnywhere, Category = "Agents")
	bool bAgentServer = true;

	/** First port tried for the local MCP server (the next nine are tried when it is taken). */
	UPROPERTY(config, EditAnywhere, Category = "Agents", meta = (ClampMin = "1024", ClampMax = "65000"))
	int32 AgentPort = 9877;

	/** Agents must send the bearer token. Turn off only for a client that cannot send headers. */
	UPROPERTY(config, EditAnywhere, Category = "Agents")
	bool bAgentToken = true;

	/** Agents may import and place results, and save what the plugin created. Off: their changes become proposals you apply. */
	UPROPERTY(config, EditAnywhere, Category = "Agents")
	bool bAllowAgentChanges = false;

	/** Agents may run Unreal Python (needs the setting above and the Python plugin). */
	UPROPERTY(config, EditAnywhere, Category = "Agents", meta = (EditCondition = "bAllowAgentChanges"))
	bool bAllowAgentPython = false;

	/** Each paid agent call waits for your approval in the editor. */
	UPROPERTY(config, EditAnywhere, Category = "Agents")
	bool bApproveAgentSpend = false;

	/** Anonymous usage statistics (asked once). Version 0.1 sends nothing. */
	UPROPERTY(config, EditAnywhere, Category = "Privacy")
	bool bTelemetry = false;

	UPROPERTY(config)
	bool bTelemetryAsked = false;

	UPROPERTY(config)
	bool bWelcomeShown = false;

	UPROPERTY(config)
	bool bAgentCardDismissed = false;

	/** Favorite model ids, kept first in the picker. */
	UPROPERTY(config)
	TArray<FString> FavoriteModels;

	/** Last model used per lane ("lane=model"). */
	UPROPERTY(config)
	TArray<FString> LastModels;

	FString LastModelFor(const FString& Lane) const;
	void SetLastModel(const FString& Lane, const FString& ModelId);
	bool IsFavorite(const FString& ModelId) const { return FavoriteModels.Contains(ModelId); }
	void ToggleFavorite(const FString& ModelId);
};
