// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioOpenWorld.h"

#include "Dom/JsonObject.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScenarioJson.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionHelpers.h"

namespace
{
	FString Join(const TArray<FString>& Names)
	{
		return FString::Join(Names, TEXT(", "));
	}
}

ScenarioOpenWorld::FState ScenarioOpenWorld::Read(UWorld* World, bool bCount)
{
	FState State;
	if (!World)
	{
		return State;
	}
	UWorldPartition* Partition = World->GetWorldPartition();
	State.bWorldPartition = Partition != nullptr;
	State.bExternalActors = World->PersistentLevel && World->PersistentLevel->IsUsingExternalActors();
	for (TActorIterator<AActor> It(World); bCount && It; ++It)
	{
		State.LoadedActors += It->IsListedInSceneOutliner() ? 1 : 0;
	}
	if (!Partition)
	{
		return State;
	}
	if (bCount && Partition->IsInitialized())
	{
		// Every actor the level holds on disk, loaded or not, from its actor descriptors (nothing is loaded).
		int32 Total = 0;
		FWorldPartitionHelpers::ForEachActorDescInstance(Partition, AActor::StaticClass(), [&Total](const FWorldPartitionActorDescInstance*)
		{
			++Total;
			return true;
		});
		State.TotalActors = Total;
	}
	if (const UDataLayerManager* Manager = UDataLayerManager::GetDataLayerManager(World))
	{
		for (const UDataLayerInstance* Layer : Manager->GetDataLayerInstances())
		{
			if (!Layer)
			{
				continue;
			}
			FDataLayer Entry;
			Entry.Name = Layer->GetDataLayerShortName();
			Entry.bLoadedInEditor = Layer->IsLoadedInEditor();
			Entry.bVisible = Layer->IsVisible();
			Entry.bReadOnly = Layer->IsReadOnly();
			Entry.bCurrent = Layer->IsInActorEditorContext();
			State.DataLayers.Add(Entry);
		}
	}
	return State;
}

TArray<FString> ScenarioOpenWorld::SummaryNotes(const FState& State)
{
	TArray<FString> Notes;
	if (State.bWorldPartition)
	{
		Notes.Add(State.TotalActors >= 0
			? FString::Printf(TEXT("World Partition level: only loaded actors are listed (%d of %d). Load a region in the World Partition editor to see more."), State.LoadedActors, State.TotalActors)
			: TEXT("World Partition level: only loaded actors are listed. Load a region in the World Partition editor to see more."));
	}
	if (State.bExternalActors)
	{
		Notes.Add(TEXT("One file per actor: each placed actor is saved in its own file; save_changes with include_levels saves them."));
	}
	TArray<FString> Current;
	TArray<FString> Unloaded;
	for (const FDataLayer& Layer : State.DataLayers)
	{
		if (Layer.bCurrent)
		{
			Current.Add(Layer.Name);
		}
		if (!Layer.bLoadedInEditor)
		{
			Unloaded.Add(Layer.Name);
		}
	}
	if (Current.Num() > 0)
	{
		Notes.Add(FString::Printf(TEXT("New actors go into the current data layers: %s."), *Join(Current)));
	}
	if (Unloaded.Num() > 0)
	{
		Notes.Add(FString::Printf(TEXT("Data layers not loaded in the editor (their actors are not listed): %s."), *Join(Unloaded)));
	}
	return Notes;
}

FString ScenarioOpenWorld::PlacementNote(const FState& State)
{
	if (!State.bWorldPartition && !State.bExternalActors)
	{
		return FString();
	}
	TArray<FString> Current;
	for (const FDataLayer& Layer : State.DataLayers)
	{
		if (Layer.bCurrent)
		{
			Current.Add(Layer.Name);
		}
	}
	FString Note = State.bWorldPartition ? TEXT("World Partition level: ") : TEXT("This level: ");
	Note += Current.Num() > 0 ? FString::Printf(TEXT("new actors go into the current data layers (%s)"), *Join(Current)) : FString(TEXT("new actors go into no data layer"));
	Note += State.bExternalActors ? TEXT(" and are saved in their own files (one file per actor).") : TEXT(".");
	return Note;
}

TSharedRef<FJsonObject> ScenarioOpenWorld::ToJson(const FState& State)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("world_partition"), State.bWorldPartition);
	Json->SetBoolField(TEXT("one_file_per_actor"), State.bExternalActors);
	Json->SetNumberField(TEXT("loaded_actors"), State.LoadedActors);
	if (State.TotalActors >= 0)
	{
		Json->SetNumberField(TEXT("actors_on_disk"), State.TotalActors);
	}
	TArray<TSharedPtr<FJsonValue>> Layers;
	for (const FDataLayer& Layer : State.DataLayers)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Layer.Name);
		Row->SetBoolField(TEXT("loaded"), Layer.bLoadedInEditor);
		Row->SetBoolField(TEXT("visible"), Layer.bVisible);
		if (Layer.bReadOnly)
		{
			Row->SetBoolField(TEXT("read_only"), true);
		}
		if (Layer.bCurrent)
		{
			Row->SetBoolField(TEXT("current"), true);
		}
		Layers.Add(MakeShared<FJsonValueObject>(Row));
	}
	if (Layers.Num() > 0)
	{
		Json->SetArrayField(TEXT("data_layers"), Layers);
	}
	return Json;
}

void ScenarioOpenWorld::DescribePlacedActor(const FState& State, bool bExternalPackage, const TArray<FString>& ActorDataLayers, TArray<FString>& OutChecks, TArray<FString>& OutWarnings)
{
	if (!State.bWorldPartition && !State.bExternalActors)
	{
		return;
	}
	if (bExternalPackage)
	{
		OutChecks.AddUnique(TEXT("Saved in its own actor file (one file per actor): save_changes with include_levels writes it."));
	}
	if (ActorDataLayers.Num() == 0)
	{
		if (State.DataLayers.Num() > 0)
		{
			OutChecks.AddUnique(TEXT("In no data layer: it loads with the level's main content."));
		}
		return;
	}
	OutChecks.AddUnique(FString::Printf(TEXT("Data layers: %s (the current ones in the Data Layers outliner)."), *Join(ActorDataLayers)));
	for (const FDataLayer& Layer : State.DataLayers)
	{
		if (!ActorDataLayers.Contains(Layer.Name))
		{
			continue;
		}
		if (Layer.bReadOnly)
		{
			OutWarnings.AddUnique(FString::Printf(TEXT("Data layer %s is read-only: the actor may not save with it."), *Layer.Name));
		}
		if (!Layer.bLoadedInEditor || !Layer.bVisible)
		{
			OutWarnings.AddUnique(FString::Printf(TEXT("Data layer %s is %s in the editor, so the placed actor does not show; make it visible in the Data Layers outliner."),
				*Layer.Name, Layer.bLoadedInEditor ? TEXT("hidden") : TEXT("not loaded")));
		}
	}
}

void ScenarioOpenWorld::DescribePlacedActor(AActor* Actor, TArray<FString>& OutChecks, TArray<FString>& OutWarnings)
{
	if (!Actor)
	{
		return;
	}
	const FState State = Read(Actor->GetWorld(), false);
	TArray<FString> Layers;
	if (State.bWorldPartition)
	{
		for (const UDataLayerInstance* Layer : Actor->GetDataLayerInstances())
		{
			if (Layer)
			{
				Layers.Add(Layer->GetDataLayerShortName());
			}
		}
	}
	DescribePlacedActor(State, Actor->IsPackageExternal(), Layers, OutChecks, OutWarnings);
}
