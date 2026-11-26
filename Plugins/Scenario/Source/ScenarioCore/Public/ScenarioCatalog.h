// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioApi.h"
#include "ScenarioTrained.h"

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
 *
 * It also knows how trained models run (ScenarioTrained.h): the base models
 * of Resources/Trained.json are read once and cached with the rows, and each
 * trained model's record is read when its form is asked for.
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

	// Trained models

	/** The base models read from Resources/Trained.json's list, in its order. */
	const TArray<FScenarioTrainedBase>& Bases() const { return BaseModels; }
	bool HasFreshBases() const;
	/**
	 * Reads the listed base models' schemas when they are older than a day (free reads).
	 * A base Scenario no longer has, or that takes no trained model, is left out; one
	 * that does not answer keeps its last reading and the list is read again next time.
	 * A trained model read before is forgotten when its base no longer takes it.
	 */
	void LoadBases(TFunction<void(const FScenarioError& Error)> OnDone = nullptr);
	/** How a trained row runs, from its type and the bases read so far (no request). */
	FScenarioTrainedInfo TrainedInfo(const FScenarioCatalogModel& Model) const;
	/**
	 * A model's form, the shape ScenarioSchema reads: its own inputs, or for a
	 * trained model its base's form with the trained input (ScenarioTrained::FormSchema).
	 * A trained model with no base is an error that says so.
	 */
	void GetSchema(const FString& ModelId, FScenarioReplyCallback OnDone);
	/** How a trained model runs, once GetSchema read it in this workspace; null otherwise. */
	const FScenarioTrainedInfo* FindTrained(const FString& ModelId) const;
	/** True for a model GetSchema has not read yet in this workspace that may be a trained model: a trained row, or a model the catalog does not list. */
	bool NeedsResolve(const FString& ModelId) const;

	DECLARE_MULTICAST_DELEGATE(FOnChanged);
	FOnChanged OnChanged;

private:
	void FetchPage(const FString& Privacy, const FString& Token, int32 PageIndex, int32 Generation, TSharedRef<TArray<FScenarioCatalogModel>> Collected, TFunction<void(const FScenarioError&)> OnDone);
	bool SaveCache() const;
	void ResolveTrained(const FScenarioModelSummary& Summary, int32 Generation, FScenarioReplyCallback OnDone);

	TSharedRef<FScenarioApi> Api;
	FString CachePath;
	FString Workspace;
	TArray<FScenarioCatalogModel> Models;
	TMap<FString, int32> IndexById;
	double FetchedAtSeconds = 0.0;
	bool bRefreshing = false;
	int32 RefreshGeneration = 0;
	TArray<TFunction<void(const FScenarioError&)>> Waiters;

	TArray<FScenarioTrainedBase> BaseModels;
	double BasesFetchedAt = 0.0;
	bool bLoadingBases = false;
	TArray<TFunction<void(const FScenarioError&)>> BaseWaiters;
	/** Trained models read in this workspace: how they run, and their form. */
	struct FTrainedEntry
	{
		FScenarioTrainedInfo Info;
		TSharedPtr<FJsonObject> Form;
	};
	TMap<FString, FTrainedEntry> Trained;
	/** Rows with a trained type whose record lists inputs of its own: they run as themselves. */
	TSet<FString> OwnInputs;
	/** Bumped on workspace switch, so a reply for the old workspace is not kept. */
	int32 TrainedGeneration = 0;
};
