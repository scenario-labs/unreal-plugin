// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FScenarioJobRow;
struct FScenarioJobFile;

/**
 * Where a generated asset came from. Stored as package metadata on the asset
 * (editor-only, stripped when cooking, no plugin class referenced) and in an
 * index under Saved/Scenario so the Content Browser filter and the
 * "In this project" view can find assets without loading them.
 */
namespace ScenarioProvenance
{
	/** Metadata keys, all prefixed "Scenario.". */
	SCENARIOEDITOR_API const TArray<FName>& Keys();

	/** Key/value pairs for one imported file of a job. */
	SCENARIOEDITOR_API TMap<FName, FString> Values(const FScenarioJobRow& Row, const FScenarioJobFile& File, bool bOmitPrompt);

	/** Writes the metadata on a loaded asset (marks its package dirty). */
	SCENARIOEDITOR_API void Write(UObject* Asset, const TMap<FName, FString>& Values);

	/** Reads it back; empty when the asset has none. */
	SCENARIOEDITOR_API TMap<FName, FString> Read(UObject* Asset);
}

/** Saved/Scenario/provenance-index.json: object path to its provenance. */
class SCENARIOEDITOR_API FScenarioProvenanceIndex
{
public:
	explicit FScenarioProvenanceIndex(const FString& InPath);

	void Load();
	void Save() const;

	void Add(const FString& ObjectPath, const TMap<FName, FString>& Values);
	void Remove(const FString& ObjectPath);
	/** Follows a rename or move. */
	void Rename(const FString& OldPath, const FString& NewPath);

	bool Contains(const FString& ObjectPath) const { return Entries.Contains(ObjectPath); }
	const TMap<FName, FString>* Find(const FString& ObjectPath) const { return Entries.Find(ObjectPath); }
	const TMap<FString, TMap<FName, FString>>& All() const { return Entries; }
	/** Object paths created from one Scenario job. */
	TArray<FString> ForJob(const FString& JobId) const;

private:
	FString Path;
	TMap<FString, TMap<FName, FString>> Entries;
};
