// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioApi.h"

/** One model in the picker, with what the picker derives from Scenario's tags. */
struct SCENARIOCORE_API FScenarioCatalogModel
{
	FScenarioModelSummary Summary;
	/** image, video, 3d, audio or text. */
	FString Modality;
	/** Picker chips this model appears under (Generate, Edit, Upscale...). */
	TArray<FString> Chips;
	bool bFeatured = false;
	bool bLegacy = false;
	/** Model id Scenario suggests instead of a deprecated one. */
	FString ReplacedBy;
	bool bTrained = false;
	bool bTeam = false;

	const FString& Id() const { return Summary.Id; }
	/** Badge text shown instead of a thumbnail (Scenario returns no model thumbnails). */
	FString Badge() const;

	static FScenarioCatalogModel FromSummary(const FScenarioModelSummary& Summary, bool bTeam);
	static FString ModalityOf(const TArray<FString>& Capabilities);
	static TArray<FString> ChipsFor(const FString& Modality, const TArray<FString>& Capabilities, const TArray<FString>& Tags, bool bTrained);
};

struct SCENARIOCORE_API FScenarioCatalogQuery
{
	FString Text;
	/** Empty for every modality. */
	FString Modality;
	/** Empty for every chip. */
	FString Chip;
	bool bShowLegacy = false;
	bool bTeamModels = true;
	/** Kept first, in this order (favorites, then the lane's quick picks). */
	TArray<FString> PinnedIds;
	int32 Limit = 0;
};

/**
 * The model catalog: every public model plus the workspace's trained models,
 * cached for 24 hours in Saved/Scenario/catalog.json and refreshed in the
 * background. Search and filters run locally on the cached rows.
 */
class SCENARIOCORE_API FScenarioCatalog : public TSharedFromThis<FScenarioCatalog>
{
public:
	static constexpr double MaxAgeSeconds = 24.0 * 3600.0;

	FScenarioCatalog(TSharedRef<FScenarioApi> InApi, const FString& InCachePath);

	/** Reads the cache file. Returns false when there is none or it cannot be read. */
	bool LoadCache();
	bool IsLoaded() const { return Models.Num() > 0; }
	bool IsStale() const;
	bool IsRefreshing() const { return bRefreshing; }
	double FetchedAt() const { return FetchedAtSeconds; }
	const FString& WorkspaceKey() const { return Workspace; }

	/** Fetches every page (public, then private trained), replaces the rows and saves the cache. */
	void Refresh(const FString& InWorkspaceKey, TFunction<void(const FScenarioError& Error)> OnDone = nullptr);
	/** Refresh only when stale or when the workspace changed. */
	void RefreshIfNeeded(const FString& InWorkspaceKey);

	/** Drops the workspace's private models (on workspace switch or sign-out). */
	void ClearPrivate();

	const TArray<FScenarioCatalogModel>& All() const { return Models; }
	const FScenarioCatalogModel* Find(const FString& ModelId) const;
	TArray<FScenarioCatalogModel> Query(const FScenarioCatalogQuery& Query) const;
	/** Chips present for one modality, in the web app's order. */
	TArray<FString> ChipsForModality(const FString& Modality) const;

	/** Replaces the rows (tests and fixtures). */
	void SetModels(TArray<FScenarioCatalogModel> InModels);

	/** Parses one models_list row, keeping the fields the picker needs. */
	static bool ParseRow(const TSharedPtr<FJsonObject>& Row, bool bTeam, FScenarioCatalogModel& Out);
	static int32 MatchScore(const FScenarioCatalogModel& Model, const FString& Text);

	DECLARE_MULTICAST_DELEGATE(FOnChanged);
	FOnChanged OnChanged;

private:
	void FetchPage(const FString& Privacy, const FString& Token, int32 PageIndex, int32 Generation, TSharedRef<TArray<FScenarioCatalogModel>> Collected, TFunction<void(const FScenarioError&)> OnDone);
	bool SaveCache() const;

	TSharedRef<FScenarioApi> Api;
	FString CachePath;
	FString Workspace;
	TArray<FScenarioCatalogModel> Models;
	TMap<FString, int32> IndexById;
	double FetchedAtSeconds = 0.0;
	bool bRefreshing = false;
	int32 RefreshGeneration = 0;
	TArray<TFunction<void(const FScenarioError&)>> Waiters;
};
