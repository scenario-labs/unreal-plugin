// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioProvenance.h"

#include "Editor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScenarioCoreModule.h"
#include "ScenarioCU.h"
#include "ScenarioJson.h"
#include "ScenarioLedger.h"
#include "ScenarioTypes.h"
#include "Subsystems/EditorAssetSubsystem.h"

const TArray<FName>& ScenarioProvenance::Keys()
{
	static const TArray<FName> List = {
		TEXT("Scenario.AssetId"), TEXT("Scenario.JobId"), TEXT("Scenario.ModelId"), TEXT("Scenario.ModelName"),
		TEXT("Scenario.WorkflowId"), TEXT("Scenario.Prompt"), TEXT("Scenario.Parameters"), TEXT("Scenario.InputAssetIds"),
		TEXT("Scenario.QuotedCU"), TEXT("Scenario.ChargedCU"), TEXT("Scenario.CreatedAt"), TEXT("Scenario.TeamId"),
		TEXT("Scenario.ProjectId"), TEXT("Scenario.Role"), TEXT("Scenario.Lane"), TEXT("Scenario.PluginVersion"), TEXT("Scenario.EngineVersion"),
	};
	return List;
}

TMap<FName, FString> ScenarioProvenance::Values(const FScenarioJobRow& Row, const FScenarioJobFile& File, bool bOmitPrompt)
{
	TMap<FName, FString> Out;
	auto Put = [&Out](const TCHAR* Key, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Out.Add(FName(Key), Value);
		}
	};
	Put(TEXT("Scenario.AssetId"), File.AssetId);
	Put(TEXT("Scenario.JobId"), Row.JobId);
	Put(TEXT("Scenario.ModelId"), Row.ModelId);
	Put(TEXT("Scenario.ModelName"), Row.ModelName);
	Put(TEXT("Scenario.WorkflowId"), Row.WorkflowId);
	if (!bOmitPrompt)
	{
		Put(TEXT("Scenario.Prompt"), Row.Prompt);
	}
	// Non-file parameters only: asset ids go in InputAssetIds.
	if (Row.Parameters.IsValid())
	{
		TSharedRef<FJsonObject> Compact = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Row.Parameters->Values)
		{
			if (bOmitPrompt && Pair.Key == TEXT("prompt"))
			{
				continue;
			}
			const FString Text = ScenarioJson::ValueText(Pair.Value);
			if (!Text.Contains(TEXT("asset_")))
			{
				Compact->SetField(Pair.Key, Pair.Value);
			}
		}
		Put(TEXT("Scenario.Parameters"), ScenarioJson::Canonical(Compact));
	}
	Put(TEXT("Scenario.InputAssetIds"), FString::Join(Row.InputAssetIds, TEXT(",")));
	Put(TEXT("Scenario.QuotedCU"), Row.QuotedExact);
	if (Row.ChargedCU >= 0.0)
	{
		Put(TEXT("Scenario.ChargedCU"), ScenarioCU::Exact(Row.ChargedCU));
	}
	Put(TEXT("Scenario.CreatedAt"), ScenarioIsoTime(Row.CreatedAt));
	Put(TEXT("Scenario.TeamId"), Row.TeamId);
	Put(TEXT("Scenario.ProjectId"), Row.ProjectId);
	Put(TEXT("Scenario.Role"), File.Role);
	Put(TEXT("Scenario.Lane"), Row.Lane);
	Put(TEXT("Scenario.PluginVersion"), ScenarioVersion::PluginVersion());
	Put(TEXT("Scenario.EngineVersion"), ScenarioVersion::EngineVersion());
	return Out;
}

void ScenarioProvenance::Write(UObject* Asset, const TMap<FName, FString>& Values)
{
	UEditorAssetSubsystem* Assets = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	if (!Asset || !Assets)
	{
		return;
	}
	for (const TPair<FName, FString>& Pair : Values)
	{
		Assets->SetMetadataTag(Asset, Pair.Key, Pair.Value);
	}
	Asset->MarkPackageDirty();
}

TMap<FName, FString> ScenarioProvenance::Read(UObject* Asset)
{
	TMap<FName, FString> Out;
	UEditorAssetSubsystem* Assets = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	if (!Asset || !Assets)
	{
		return Out;
	}
	for (const FName& Key : Keys())
	{
		const FString Value = Assets->GetMetadataTag(Asset, Key);
		if (!Value.IsEmpty())
		{
			Out.Add(Key, Value);
		}
	}
	return Out;
}

FScenarioProvenanceIndex::FScenarioProvenanceIndex(const FString& InPath)
	: Path(InPath)
{
}

void FScenarioProvenanceIndex::Load()
{
	Entries.Reset();
	FString Text;
	if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path))
	{
		return;
	}
	const TSharedPtr<FJsonObject> Root = ScenarioJson::ParseObject(Text);
	const TSharedPtr<FJsonObject> Assets = ScenarioJson::GetObject(Root, TEXT("assets"));
	if (!Assets.IsValid())
	{
		return;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ScenarioJson::Fields(Assets))
	{
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
		{
			continue;
		}
		TMap<FName, FString>& Entry = Entries.Add(Pair.Key);
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Pair.Value->AsObject()->Values)
		{
			Entry.Add(FName(*Field.Key), ScenarioJson::ValueText(Field.Value));
		}
	}
}

void FScenarioProvenanceIndex::Save() const
{
	if (Path.IsEmpty())
	{
		return;
	}
	TSharedRef<FJsonObject> Assets = MakeShared<FJsonObject>();
	for (const TPair<FString, TMap<FName, FString>>& Entry : Entries)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		for (const TPair<FName, FString>& Field : Entry.Value)
		{
			// The index never stores prompts: it is a lookup, the asset holds the full record.
			if (Field.Key != FName(TEXT("Scenario.Prompt")) && Field.Key != FName(TEXT("Scenario.Parameters")))
			{
				Object->SetStringField(Field.Key.ToString(), Field.Value);
			}
		}
		Assets->SetObjectField(Entry.Key, Object);
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 1);
	Root->SetObjectField(TEXT("assets"), Assets);
	const FString Temp = Path + TEXT(".tmp");
	if (FFileHelper::SaveStringToFile(ScenarioJson::Write(Root, true), *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		IFileManager::Get().Move(*Path, *Temp, true);
	}
}

void FScenarioProvenanceIndex::Add(const FString& ObjectPath, const TMap<FName, FString>& Values)
{
	Entries.Add(ObjectPath, Values);
	Save();
}

void FScenarioProvenanceIndex::Remove(const FString& ObjectPath)
{
	if (Entries.Remove(ObjectPath) > 0)
	{
		Save();
	}
}

void FScenarioProvenanceIndex::Rename(const FString& OldPath, const FString& NewPath)
{
	TMap<FName, FString> Values;
	if (Entries.RemoveAndCopyValue(OldPath, Values))
	{
		Entries.Add(NewPath, Values);
		Save();
	}
}

TArray<FString> FScenarioProvenanceIndex::ForJob(const FString& JobId) const
{
	TArray<FString> Out;
	for (const TPair<FString, TMap<FName, FString>>& Entry : Entries)
	{
		const FString* Value = Entry.Value.Find(TEXT("Scenario.JobId"));
		if (Value && *Value == JobId)
		{
			Out.Add(Entry.Key);
		}
	}
	return Out;
}
