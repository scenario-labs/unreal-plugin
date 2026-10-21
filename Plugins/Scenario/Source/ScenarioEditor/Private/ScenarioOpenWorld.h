// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class AActor;
class FJsonObject;
class UWorld;

/**
 * Open worlds (World Partition, one file per actor, data layers) as the
 * plugin reports them to people and agents. The text is built from a plain
 * state so it can be tested without a level.
 */
namespace ScenarioOpenWorld
{
	struct FDataLayer
	{
		FString Name;
		bool bLoadedInEditor = true;
		bool bVisible = true;
		bool bReadOnly = false;
		/** In the Data Layers outliner's current set: new actors go into it. */
		bool bCurrent = false;
	};

	struct FState
	{
		bool bWorldPartition = false;
		/** The level stores each actor in its own file (One File Per Actor). */
		bool bExternalActors = false;
		/** Actors in memory now. */
		int32 LoadedActors = 0;
		/** Actors the level holds on disk, loaded or not (-1 when unknown). */
		int32 TotalActors = -1;
		TArray<FDataLayer> DataLayers;
	};

	/** The open-world state of a world (empty state for null). bCount also counts loaded actors and actors on disk. */
	FState Read(UWorld* World, bool bCount = true);

	/** Sentences for scene_summary: what the actor list covers and how new actors are stored. Empty for a plain level. */
	TArray<FString> SummaryNotes(const FState& State);

	/** One sentence for a placement review in an open world: where a new actor goes and how it is saved. Empty for a plain level. */
	FString PlacementNote(const FState& State);

	/** The state as JSON for scene_summary. */
	TSharedRef<FJsonObject> ToJson(const FState& State);

	/**
	 * Where a newly placed actor ended up: its own file, its data layers, and a
	 * warning when one of them is unloaded, hidden or read-only (the actor is
	 * placed but may not show, or may not save). Empty for a plain level.
	 */
	void DescribePlacedActor(const FState& State, bool bExternalPackage, const TArray<FString>& ActorDataLayers, TArray<FString>& OutChecks, TArray<FString>& OutWarnings);

	/** DescribePlacedActor for a live actor in its world. */
	void DescribePlacedActor(AActor* Actor, TArray<FString>& OutChecks, TArray<FString>& OutWarnings);
}
