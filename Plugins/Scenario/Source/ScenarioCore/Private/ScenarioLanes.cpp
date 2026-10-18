// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioLanes.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScenarioCoreModule.h"
#include "ScenarioJson.h"

TArray<FScenarioLaneModel> FScenarioLane::ModelsFor(const FString& Mode) const
{
	if (Mode.IsEmpty())
	{
		return Models;
	}
	return Models.FilterByPredicate([&Mode](const FScenarioLaneModel& Model) { return Model.Mode.IsEmpty() || Model.Mode == Mode; });
}

const FScenarioLanes& FScenarioLanes::Get()
{
	static FScenarioLanes Instance;
	static bool bLoaded = false;
	if (!bLoaded)
	{
		bLoaded = true;
		FString Text;
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scenario"));
		const FString Path = Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Lanes.json")) : FString();
		if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path) || !Instance.LoadFromString(Text))
		{
			UE_LOG(LogScenario, Warning, TEXT("Scenario lanes file missing or unreadable; using the built-in Image lane only."));
			Instance.LoadDefaults();
		}
	}
	return Instance;
}

bool FScenarioLanes::LoadFromString(const FString& Text)
{
	const TSharedPtr<FJsonObject> Root = ScenarioJson::ParseObject(Text);
	if (!Root.IsValid())
	{
		return false;
	}
	TArray<FScenarioLane> Parsed;
	for (const TSharedPtr<FJsonValue>& LaneValue : ScenarioJson::GetArray(Root, TEXT("lanes")))
	{
		if (!LaneValue.IsValid() || LaneValue->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Object = LaneValue->AsObject();
		FScenarioLane Lane;
		Lane.Id = ScenarioJson::GetString(Object, TEXT("id"));
		Lane.Title = ScenarioJson::GetString(Object, TEXT("title"), Lane.Id);
		Lane.Capability = ScenarioJson::GetString(Object, TEXT("capability"));
		Lane.Importer = ScenarioJson::GetString(Object, TEXT("importer"));
		Lane.Folder = ScenarioJson::GetString(Object, TEXT("folder"));
		Lane.DownloadFormat = ScenarioJson::GetString(Object, TEXT("download_format"));
		Lane.Hint = ScenarioJson::GetString(Object, TEXT("hint"));
		Lane.Modes = ScenarioJson::GetStringArray(Object, TEXT("modes"));
		Lane.StarterPrompts = ScenarioJson::GetStringArray(Object, TEXT("starter_prompts"));
		Lane.bRecommend = ScenarioJson::GetBool(Object, TEXT("recommend"), true);
		Lane.bNeedsCapture = ScenarioJson::GetBool(Object, TEXT("needs_capture"), false);
		for (const TSharedPtr<FJsonValue>& ModelValue : ScenarioJson::GetArray(Object, TEXT("models")))
		{
			if (!ModelValue.IsValid() || ModelValue->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> ModelObject = ModelValue->AsObject();
			FScenarioLaneModel Model;
			Model.Id = ScenarioJson::GetString(ModelObject, TEXT("id"));
			Model.Name = ScenarioJson::GetString(ModelObject, TEXT("name"), Model.Id);
			Model.Note = ScenarioJson::GetString(ModelObject, TEXT("note"));
			Model.Mode = ScenarioJson::GetString(ModelObject, TEXT("mode"));
			Model.bUnrealReady = ScenarioJson::GetBool(ModelObject, TEXT("unreal_ready"), false);
			if (!Model.Id.IsEmpty())
			{
				Lane.Models.Add(MoveTemp(Model));
			}
		}
		if (!Lane.Id.IsEmpty() && Lane.Models.Num() > 0)
		{
			Parsed.Add(MoveTemp(Lane));
		}
	}
	if (Parsed.Num() == 0)
	{
		return false;
	}
	Lanes = MoveTemp(Parsed);
	PresetMap.Reset();
	if (const TSharedPtr<FJsonObject> Presets = ScenarioJson::GetObject(Root, TEXT("presets")))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Presets->Values)
		{
			if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
			{
				PresetMap.Add(Pair.Key, Pair.Value->AsObject());
			}
		}
	}
	RestrictMap.Reset();
	if (const TSharedPtr<FJsonObject> Restrict = ScenarioJson::GetObject(Root, TEXT("restrict")))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Restrict->Values)
		{
			RestrictMap.Add(Pair.Key, ScenarioJson::GetStringArray(Restrict, Pair.Key));
		}
	}
	Tools.Reset();
	if (const TSharedPtr<FJsonObject> ToolObject = ScenarioJson::GetObject(Root, TEXT("tools")))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ToolObject->Values)
		{
			Tools.Add(Pair.Key, ScenarioJson::GetString(ToolObject, Pair.Key));
		}
	}
	UnrealReady = ScenarioJson::GetString(Root, TEXT("unreal_ready_prompt"));
	return true;
}

void FScenarioLanes::LoadDefaults()
{
	FScenarioLane Image;
	Image.Id = TEXT("image");
	Image.Title = TEXT("Image");
	Image.Capability = TEXT("txt2img");
	Image.Importer = TEXT("texture");
	Image.Folder = TEXT("Images");
	Image.StarterPrompts.Add(TEXT("a carved wooden treasure chest, game prop, studio light"));
	FScenarioLaneModel Flare;
	Flare.Id = TEXT("model_openai-gpt-image-2-5-flare");
	Flare.Name = TEXT("GPT Image 2.5 Flare");
	Image.Models.Add(Flare);
	Lanes = { Image };
}

const FScenarioLane* FScenarioLanes::Find(const FString& Id) const
{
	return Lanes.FindByPredicate([&Id](const FScenarioLane& Lane) { return Lane.Id == Id; });
}

const FScenarioLane* FScenarioLanes::LaneForModel(const FString& ModelId) const
{
	for (const FScenarioLane& Lane : Lanes)
	{
		if (Lane.Models.ContainsByPredicate([&ModelId](const FScenarioLaneModel& Model) { return Model.Id == ModelId; }))
		{
			return &Lane;
		}
	}
	return nullptr;
}

FString FScenarioLanes::DefaultModel(const FString& LaneId, const FString& Mode) const
{
	const FScenarioLane* Lane = Find(LaneId);
	if (!Lane)
	{
		return FString();
	}
	const TArray<FScenarioLaneModel> Models = Lane->ModelsFor(Mode);
	return Models.Num() > 0 ? Models[0].Id : FString();
}

TSharedPtr<FJsonObject> FScenarioLanes::Presets(const FString& ModelId) const
{
	const TSharedPtr<FJsonObject>* Found = PresetMap.Find(ModelId);
	return Found ? ScenarioJson::CloneObject(*Found) : MakeShared<FJsonObject>();
}

FString FScenarioLanes::ToolModel(const FString& Tool) const
{
	const FString* Found = Tools.Find(Tool);
	return Found ? *Found : FString();
}
