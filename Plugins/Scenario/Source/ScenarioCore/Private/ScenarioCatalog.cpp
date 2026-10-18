// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioCatalog.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScenarioCoreModule.h"
#include "ScenarioJson.h"

namespace
{
	constexpr int32 CacheVersion = 1;
	constexpr int32 PageSize = 100;
	/** 728 public models fit in 8 pages on 2026-10-18; the guard stops a runaway token loop. */
	constexpr int32 MaxPages = 40;

	bool HasTag(const TArray<FString>& Tags, std::initializer_list<const TCHAR*> Needles)
	{
		for (const FString& Tag : Tags)
		{
			for (const TCHAR* Needle : Needles)
			{
				if (Tag.Contains(Needle, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
		}
		return false;
	}

	bool HasCapability(const TArray<FString>& Capabilities, std::initializer_list<const TCHAR*> Names)
	{
		for (const TCHAR* Name : Names)
		{
			if (Capabilities.Contains(Name))
			{
				return true;
			}
		}
		return false;
	}

	/** Output side of a capability: "img2video" gives "video", "inpaint" gives "img". */
	FString OutputOf(const FString& Capability)
	{
		FString Base = Capability;
		Base.RemoveFromEnd(TEXT("_texture"));
		if (Base == TEXT("inpaint") || Base == TEXT("outpaint") || Base == TEXT("reference"))
		{
			return TEXT("img");
		}
		FString Left;
		FString Right;
		if (Base.Split(TEXT("2"), &Left, &Right, ESearchCase::CaseSensitive, ESearchDir::FromStart))
		{
			return Right;
		}
		return FString();
	}

	TSharedRef<FJsonObject> RowToJson(const FScenarioCatalogModel& Model)
	{
		const FScenarioModelSummary& S = Model.Summary;
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("id"), S.Id);
		Row->SetStringField(TEXT("name"), S.Name);
		Row->SetStringField(TEXT("shortDescription"), S.ShortDescription);
		Row->SetStringField(TEXT("type"), S.Type);
		Row->SetStringField(TEXT("privacy"), S.Privacy);
		Row->SetStringField(TEXT("status"), S.Status);
		Row->SetField(TEXT("capabilities"), ScenarioJson::StringArrayValue(S.Capabilities));
		Row->SetField(TEXT("tags"), ScenarioJson::StringArrayValue(S.Tags));
		if (S.CreatedAt > 0.0)
		{
			Row->SetStringField(TEXT("createdAt"), ScenarioIsoTime(S.CreatedAt));
		}
		if (!S.Provider.IsEmpty())
		{
			TSharedRef<FJsonObject> Compliance = MakeShared<FJsonObject>();
			Compliance->SetStringField(TEXT("modelProvider"), S.Provider);
			Row->SetObjectField(TEXT("complianceMetadata"), Compliance);
		}
		Row->SetBoolField(TEXT("team"), Model.bTeam);
		return Row;
	}

	bool ParseCache(const FString& Text, TArray<FScenarioCatalogModel>& OutModels, double& OutFetchedAt, FString& OutWorkspace)
	{
		const TSharedPtr<FJsonObject> Root = ScenarioJson::ParseObject(Text);
		if (!Root.IsValid() || ScenarioJson::GetNumber(Root, TEXT("version")) != CacheVersion)
		{
			return false;
		}
		OutFetchedAt = ScenarioJson::GetNumber(Root, TEXT("fetchedAt"));
		OutWorkspace = ScenarioJson::GetString(Root, TEXT("workspace"));
		for (const TSharedPtr<FJsonValue>& Value : ScenarioJson::GetArray(Root, TEXT("models")))
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				continue;
			}
			FScenarioCatalogModel Model;
			if (FScenarioCatalog::ParseRow(Value->AsObject(), ScenarioJson::GetBool(Value->AsObject(), TEXT("team")), Model))
			{
				OutModels.Add(MoveTemp(Model));
			}
		}
		return OutModels.Num() > 0;
	}
}

FString FScenarioCatalogModel::Badge() const
{
	if (bTeam || bTrained)
	{
		return TEXT("Trained");
	}
	return Summary.Provider.IsEmpty() ? FString(TEXT("Scenario")) : Summary.Provider;
}

FString FScenarioCatalogModel::ModalityOf(const TArray<FString>& Capabilities)
{
	bool bImage = false;
	bool bVideo = false;
	bool bAudio = false;
	bool b3D = false;
	bool bText = false;
	for (const FString& Capability : Capabilities)
	{
		const FString Output = OutputOf(Capability);
		bImage |= Output == TEXT("img");
		bVideo |= Output == TEXT("video");
		bAudio |= Output == TEXT("audio");
		b3D |= Output == TEXT("3d");
		bText |= Output == TEXT("txt");
	}
	// A model that outputs several kinds is listed under its richest one.
	if (b3D) return TEXT("3d");
	if (bVideo) return TEXT("video");
	if (bAudio) return TEXT("audio");
	if (bImage) return TEXT("image");
	if (bText) return TEXT("text");
	return TEXT("image");
}

TArray<FString> FScenarioCatalogModel::ChipsFor(const FString& Modality, const TArray<FString>& Caps, const TArray<FString>& Tags, bool bTrained)
{
	TArray<FString> Chips;
	const bool bTool = HasTag(Tags, { TEXT("sc:tool"), TEXT("tool") });
	if (Modality == TEXT("image"))
	{
		if (HasCapability(Caps, { TEXT("txt2img"), TEXT("txt2img_texture") }) && !bTool) Chips.Add(TEXT("Generate"));
		if (HasCapability(Caps, { TEXT("img2img"), TEXT("inpaint") }) || HasTag(Tags, { TEXT("edit") })) Chips.Add(TEXT("Edit"));
		if (HasCapability(Caps, { TEXT("outpaint") }) || HasTag(Tags, { TEXT("expand"), TEXT("outpaint") })) Chips.Add(TEXT("Expand"));
		if (HasTag(Tags, { TEXT("upscale"), TEXT("enhance") })) Chips.Add(TEXT("Upscale"));
		if (HasTag(Tags, { TEXT("vector"), TEXT("svg") })) Chips.Add(TEXT("Vectorize"));
		if (HasTag(Tags, { TEXT("remove-background"), TEXT("remove background"), TEXT("background removal") })) Chips.Add(TEXT("Remove background"));
		if (HasCapability(Caps, { TEXT("txt2img_texture"), TEXT("img2img_texture") }) || HasTag(Tags, { TEXT("sc:texture"), TEXT("PBR") })) Chips.Add(TEXT("Textures"));
	}
	else if (Modality == TEXT("video"))
	{
		if (HasCapability(Caps, { TEXT("txt2video"), TEXT("img2video"), TEXT("audio2video") })) Chips.Add(TEXT("Generate"));
		if (HasCapability(Caps, { TEXT("video2video") }) && !HasTag(Tags, { TEXT("upscale") })) Chips.Add(TEXT("Edit"));
		if (HasTag(Tags, { TEXT("lipsync"), TEXT("lip sync") })) Chips.Add(TEXT("Lipsync"));
		if (HasTag(Tags, { TEXT("upscale") })) Chips.Add(TEXT("Upscale"));
		if (HasTag(Tags, { TEXT("reframe") })) Chips.Add(TEXT("Reframe"));
		if (HasTag(Tags, { TEXT("remove-background"), TEXT("remove background") })) Chips.Add(TEXT("Remove background"));
	}
	else if (Modality == TEXT("audio"))
	{
		if (HasTag(Tags, { TEXT("tts"), TEXT("speech"), TEXT("voice") })) Chips.Add(TEXT("Speech"));
		if (HasTag(Tags, { TEXT("music") })) Chips.Add(TEXT("Music"));
		if (HasTag(Tags, { TEXT("sfx"), TEXT("sound effect") }) || HasCapability(Caps, { TEXT("video2audio") })) Chips.Add(TEXT("SFX"));
	}
	else if (Modality == TEXT("3d"))
	{
		if (HasCapability(Caps, { TEXT("txt23d"), TEXT("img23d"), TEXT("video23d") }) && !HasTag(Tags, { TEXT("splat") })) Chips.Add(TEXT("Generate"));
		if (HasTag(Tags, { TEXT("remesh"), TEXT("retopo"), TEXT("low poly") })) Chips.Add(TEXT("Remesh"));
		if (HasTag(Tags, { TEXT("retexture"), TEXT("texturing") })) Chips.Add(TEXT("Retexture"));
		if (HasTag(Tags, { TEXT("uv") })) Chips.Add(TEXT("UV unwrap"));
		if (HasTag(Tags, { TEXT("rig") })) Chips.Add(TEXT("Rigging"));
		if (HasTag(Tags, { TEXT("animation"), TEXT("motion") })) Chips.Add(TEXT("Animate"));
		if (HasTag(Tags, { TEXT("parts"), TEXT("segment") })) Chips.Add(TEXT("Parts"));
		if (HasTag(Tags, { TEXT("splat") })) Chips.Add(TEXT("Splat"));
		if (HasCapability(Caps, { TEXT("3d23d") }) && Chips.Num() == 0) Chips.Add(TEXT("Edit"));
	}
	if (bTool)
	{
		Chips.Add(TEXT("Tools"));
	}
	if (bTrained)
	{
		Chips.Add(TEXT("Trained"));
	}
	return Chips;
}

FScenarioCatalogModel FScenarioCatalogModel::FromSummary(const FScenarioModelSummary& Summary, bool bInTeam)
{
	FScenarioCatalogModel Model;
	Model.Summary = Summary;
	Model.bTeam = bInTeam;
	Model.Modality = ModalityOf(Summary.Capabilities);
	const FString Type = Summary.Type.ToLower();
	Model.bTrained = bInTeam || Type.Contains(TEXT("lora")) || Type.Contains(TEXT("composition"));
	for (const FString& Tag : Summary.Tags)
	{
		const FString Lower = Tag.ToLower();
		if (Lower == TEXT("sc:featured") || Lower == TEXT("featured"))
		{
			Model.bFeatured = true;
		}
		else if (Lower == TEXT("sc:legacy") || Lower == TEXT("deprecated") || Lower == TEXT("legacy"))
		{
			Model.bLegacy = true;
		}
		else if (Lower.StartsWith(TEXT("deprecated:")))
		{
			Model.bLegacy = true;
			Model.ReplacedBy = Tag.Mid(11);
		}
	}
	Model.Chips = ChipsFor(Model.Modality, Summary.Capabilities, Summary.Tags, Model.bTrained);
	return Model;
}

FScenarioCatalog::FScenarioCatalog(TSharedRef<FScenarioApi> InApi, const FString& InCachePath)
	: Api(InApi)
	, CachePath(InCachePath)
{
}

bool FScenarioCatalog::ParseRow(const TSharedPtr<FJsonObject>& Row, bool bInTeam, FScenarioCatalogModel& Out)
{
	const FScenarioModelSummary Summary = FScenarioModelSummary::FromJson(Row);
	if (Summary.Id.IsEmpty() || Summary.Capabilities.Num() == 0)
	{
		return false;
	}
	// Private rows that are not ready (training, failed) are not usable.
	if (!Summary.Status.IsEmpty() && Summary.Status != TEXT("trained"))
	{
		return false;
	}
	Out = FScenarioCatalogModel::FromSummary(Summary, bInTeam);
	return true;
}

bool FScenarioCatalog::LoadCache()
{
	FString Text;
	if (CachePath.IsEmpty() || !FFileHelper::LoadFileToString(Text, *CachePath))
	{
		return false;
	}
	TArray<FScenarioCatalogModel> Parsed;
	double ParsedAt = 0.0;
	FString ParsedWorkspace;
	if (!ParseCache(Text, Parsed, ParsedAt, ParsedWorkspace))
	{
		UE_LOG(LogScenario, Warning, TEXT("Scenario catalog cache unreadable; it will be fetched again."));
		return false;
	}
	FetchedAtSeconds = ParsedAt;
	Workspace = ParsedWorkspace;
	SetModels(MoveTemp(Parsed));
	return true;
}

bool FScenarioCatalog::IsStale() const
{
	return Models.Num() == 0 || ScenarioNow() - FetchedAtSeconds > MaxAgeSeconds;
}

void FScenarioCatalog::RefreshIfNeeded(const FString& InWorkspaceKey)
{
	if (bRefreshing)
	{
		return;
	}
	if (IsStale() || InWorkspaceKey != Workspace)
	{
		Refresh(InWorkspaceKey);
	}
}

void FScenarioCatalog::Refresh(const FString& InWorkspaceKey, TFunction<void(const FScenarioError& Error)> OnDone)
{
	if (OnDone)
	{
		Waiters.Add(MoveTemp(OnDone));
	}
	if (bRefreshing && InWorkspaceKey == Workspace)
	{
		return;
	}
	bRefreshing = true;
	Workspace = InWorkspaceKey;
	const int32 Generation = ++RefreshGeneration;
	TSharedRef<TArray<FScenarioCatalogModel>> Collected = MakeShared<TArray<FScenarioCatalogModel>>();
	TWeakPtr<FScenarioCatalog> WeakSelf = AsShared();

	auto Finish = [WeakSelf, Generation, Collected](const FScenarioError& Error)
	{
		const TSharedPtr<FScenarioCatalog> Self = WeakSelf.Pin();
		if (!Self.IsValid() || Generation != Self->RefreshGeneration)
		{
			return;
		}
		Self->bRefreshing = false;
		if (Collected->Num() > 0)
		{
			Self->FetchedAtSeconds = ScenarioNow();
			Self->SetModels(MoveTemp(*Collected));
			Self->SaveCache();
		}
		TArray<TFunction<void(const FScenarioError&)>> Done = MoveTemp(Self->Waiters);
		for (const TFunction<void(const FScenarioError&)>& Waiter : Done)
		{
			Waiter(Error);
		}
	};

	FetchPage(TEXT("public"), FString(), 0, Generation, Collected, [WeakSelf, Generation, Collected, Finish](const FScenarioError& PublicError)
	{
		const TSharedPtr<FScenarioCatalog> Self = WeakSelf.Pin();
		if (!Self.IsValid() || Generation != Self->RefreshGeneration)
		{
			return;
		}
		if (PublicError.Kind != EScenarioErrorKind::None)
		{
			// Keep the cached rows rather than a partial list.
			Collected->Reset();
			Finish(PublicError);
			return;
		}
		Self->FetchPage(TEXT("private"), FString(), 0, Generation, Collected, [Finish](const FScenarioError& PrivateError)
		{
			if (PrivateError.Kind != EScenarioErrorKind::None)
			{
				// Public models are still usable; trained models come back on the next refresh.
				UE_LOG(LogScenario, Warning, TEXT("Scenario trained models unavailable: %s"), *PrivateError.ToString());
			}
			Finish(FScenarioError());
		});
	});
}

void FScenarioCatalog::FetchPage(const FString& Privacy, const FString& Token, int32 PageIndex, int32 Generation, TSharedRef<TArray<FScenarioCatalogModel>> Collected, TFunction<void(const FScenarioError&)> OnDone)
{
	TWeakPtr<FScenarioCatalog> WeakSelf = AsShared();
	Api->ListModels(Privacy, FString(), PageSize, Token, [WeakSelf, Privacy, Generation, Collected, OnDone, PageIndex](const TScenarioResult<FScenarioModelPage>& Result)
	{
		const TSharedPtr<FScenarioCatalog> Self = WeakSelf.Pin();
		if (!Self.IsValid() || Generation != Self->RefreshGeneration)
		{
			return;
		}
		if (!Result.bOk)
		{
			OnDone(Result.Error);
			return;
		}
		const bool bTeam = Privacy == TEXT("private");
		for (const FScenarioModelSummary& Summary : Result.Value.Models)
		{
			// Trained models still training (or failed) cannot run yet.
			if (Summary.Capabilities.Num() > 0 && (Summary.Status.IsEmpty() || Summary.Status == TEXT("trained")))
			{
				Collected->Add(FScenarioCatalogModel::FromSummary(Summary, bTeam));
			}
		}
		if (Result.Value.NextPageToken.IsEmpty() || Result.Value.Models.Num() == 0 || PageIndex + 1 >= MaxPages)
		{
			OnDone(FScenarioError());
			return;
		}
		Self->FetchPage(Privacy, Result.Value.NextPageToken, PageIndex + 1, Generation, Collected, OnDone);
	});
}

bool FScenarioCatalog::SaveCache() const
{
	if (CachePath.IsEmpty())
	{
		return false;
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), CacheVersion);
	Root->SetNumberField(TEXT("fetchedAt"), FetchedAtSeconds);
	Root->SetStringField(TEXT("workspace"), Workspace);
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Models.Num());
	for (const FScenarioCatalogModel& Model : Models)
	{
		Rows.Add(MakeShared<FJsonValueObject>(RowToJson(Model)));
	}
	Root->SetArrayField(TEXT("models"), Rows);
	const FString Text = ScenarioJson::Write(Root);
	const FString Temp = CachePath + TEXT(".tmp");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(CachePath), true);
	if (!FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return false;
	}
	return IFileManager::Get().Move(*CachePath, *Temp, true, true);
}

void FScenarioCatalog::ClearPrivate()
{
	TArray<FScenarioCatalogModel> Kept = Models.FilterByPredicate([](const FScenarioCatalogModel& Model) { return !Model.bTeam; });
	// A refresh in flight would bring back the old workspace's models.
	++RefreshGeneration;
	bRefreshing = false;
	Workspace.Reset();
	SetModels(MoveTemp(Kept));
	SaveCache();
}

void FScenarioCatalog::SetModels(TArray<FScenarioCatalogModel> InModels)
{
	Models = MoveTemp(InModels);
	IndexById.Reset();
	for (int32 Index = 0; Index < Models.Num(); ++Index)
	{
		IndexById.Add(Models[Index].Id(), Index);
	}
	OnChanged.Broadcast();
}

const FScenarioCatalogModel* FScenarioCatalog::Find(const FString& ModelId) const
{
	const int32* Index = IndexById.Find(ModelId);
	return Index ? &Models[*Index] : nullptr;
}

int32 FScenarioCatalog::MatchScore(const FScenarioCatalogModel& Model, const FString& Text)
{
	TArray<FString> Words;
	Text.ToLower().ParseIntoArrayWS(Words);
	if (Words.Num() == 0)
	{
		return 1;
	}
	const FString Name = Model.Summary.Name.ToLower();
	const FString Id = Model.Summary.Id.ToLower();
	const FString Provider = Model.Summary.Provider.ToLower();
	const FString Description = Model.Summary.ShortDescription.ToLower();
	const FString Extra = (FString::Join(Model.Summary.Tags, TEXT(" ")) + TEXT(" ") + FString::Join(Model.Summary.Capabilities, TEXT(" ")) + TEXT(" ") + FString::Join(Model.Chips, TEXT(" "))).ToLower();
	int32 Score = 0;
	for (const FString& Word : Words)
	{
		int32 WordScore = 0;
		if (Name.StartsWith(Word))
		{
			WordScore = 6;
		}
		else if (Name.Contains(Word))
		{
			WordScore = 4;
		}
		else if (Id.Contains(Word) || Provider.Contains(Word))
		{
			WordScore = 2;
		}
		else if (Extra.Contains(Word) || Description.Contains(Word))
		{
			WordScore = 1;
		}
		if (WordScore == 0)
		{
			return 0;
		}
		Score += WordScore;
	}
	return Score;
}

TArray<FScenarioCatalogModel> FScenarioCatalog::Query(const FScenarioCatalogQuery& InQuery) const
{
	struct FScored
	{
		const FScenarioCatalogModel* Model;
		int32 Pin;
		int32 Score;
	};
	TArray<FScored> Rows;
	for (const FScenarioCatalogModel& Model : Models)
	{
		const int32 Pin = InQuery.PinnedIds.IndexOfByKey(Model.Id());
		if (!InQuery.Modality.IsEmpty() && Model.Modality != InQuery.Modality)
		{
			continue;
		}
		if (!InQuery.Chip.IsEmpty() && !Model.Chips.Contains(InQuery.Chip))
		{
			continue;
		}
		// Pinned models stay visible even when legacy, so a saved favorite never disappears.
		if (Model.bLegacy && !InQuery.bShowLegacy && Pin == INDEX_NONE)
		{
			continue;
		}
		if (Model.bTeam && !InQuery.bTeamModels)
		{
			continue;
		}
		const int32 Score = MatchScore(Model, InQuery.Text);
		if (Score <= 0)
		{
			continue;
		}
		Rows.Add({ &Model, Pin == INDEX_NONE ? MAX_int32 : Pin, Score });
	}
	const bool bText = !InQuery.Text.TrimStartAndEnd().IsEmpty();
	Rows.Sort([bText](const FScored& A, const FScored& B)
	{
		if (A.Pin != B.Pin)
		{
			return A.Pin < B.Pin;
		}
		if (bText && A.Score != B.Score)
		{
			return A.Score > B.Score;
		}
		if (A.Model->bFeatured != B.Model->bFeatured)
		{
			return A.Model->bFeatured;
		}
		return A.Model->Summary.CreatedAt > B.Model->Summary.CreatedAt;
	});
	TArray<FScenarioCatalogModel> Out;
	for (const FScored& Row : Rows)
	{
		if (InQuery.Limit > 0 && Out.Num() >= InQuery.Limit)
		{
			break;
		}
		Out.Add(*Row.Model);
	}
	return Out;
}

TArray<FString> FScenarioCatalog::ChipsForModality(const FString& Modality) const
{
	static const TMap<FString, TArray<FString>> Order = {
		{ TEXT("image"), { TEXT("Generate"), TEXT("Edit"), TEXT("Expand"), TEXT("Upscale"), TEXT("Vectorize"), TEXT("Remove background"), TEXT("Textures"), TEXT("Tools"), TEXT("Trained") } },
		{ TEXT("video"), { TEXT("Generate"), TEXT("Edit"), TEXT("Lipsync"), TEXT("Upscale"), TEXT("Reframe"), TEXT("Remove background"), TEXT("Tools"), TEXT("Trained") } },
		{ TEXT("audio"), { TEXT("Speech"), TEXT("Music"), TEXT("SFX"), TEXT("Tools"), TEXT("Trained") } },
		{ TEXT("3d"), { TEXT("Generate"), TEXT("Remesh"), TEXT("Retexture"), TEXT("UV unwrap"), TEXT("Rigging"), TEXT("Animate"), TEXT("Parts"), TEXT("Splat"), TEXT("Edit"), TEXT("Tools"), TEXT("Trained") } },
	};
	TSet<FString> Present;
	for (const FScenarioCatalogModel& Model : Models)
	{
		if (Model.Modality == Modality && !Model.bLegacy)
		{
			Present.Append(Model.Chips);
		}
	}
	TArray<FString> Out;
	if (const TArray<FString>* Chips = Order.Find(Modality))
	{
		for (const FString& Chip : *Chips)
		{
			if (Present.Contains(Chip))
			{
				Out.Add(Chip);
			}
		}
	}
	return Out;
}
