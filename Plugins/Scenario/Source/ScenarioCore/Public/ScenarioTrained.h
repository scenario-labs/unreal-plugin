// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FScenarioModelSummary;

/**
 * Trained models (LoRAs and compositions). A trained model is never the model
 * called: it runs on a base model, with the trained model in the base's loras
 * input (a LoRA) or its modelId input (a composition). The bases come from the
 * schemas of the base models listed in Resources/Trained.json: a base takes a
 * trained model when its loras or modelId input lists the trained model's type
 * in modelTypes. Nothing is guessed for a type no base lists.
 */
enum class EScenarioTrainedKind : uint8
{
	None,
	Lora,
	Composition,
};

/** Resources/Trained.json: which base models to read, and how compliantModelIds names them. */
struct SCENARIOCORE_API FScenarioTrainedData
{
	/** Base models whose schemas say which trained types they take, in order of preference. */
	TArray<FString> BaseProbes;
	/** A compliantModelIds entry (a base name such as flux.1-dev) to a model id. Only orders the candidates. */
	TMap<FString, FString> BaseNames;
	/** The LoRA weight when the trained model's record has none of its own. */
	double DefaultScale = 0.75;

	static const FScenarioTrainedData& Get();
	static FScenarioTrainedData FromJson(const TSharedPtr<FJsonObject>& Json);
};

/** A base model read from its schema: the trained types its loras and modelId inputs take. */
struct SCENARIOCORE_API FScenarioTrainedBase
{
	FString Id;
	FString Name;
	TArray<FString> LoraTypes;
	TArray<FString> ModelIdTypes;
	int32 MaxLoras = 0;
	double ScaleMin = 0.0;
	double ScaleMax = 2.0;
	double ScaleStep = 0.05;
	FString PromptField = TEXT("prompt");

	/** From a model record (GET /models/{id}). False when it takes no trained model. */
	static bool FromModel(const TSharedPtr<FJsonObject>& Model, FScenarioTrainedBase& Out);
	TSharedRef<FJsonObject> ToJson() const;
	static bool FromJson(const TSharedPtr<FJsonObject>& Json, FScenarioTrainedBase& Out);
};

/** How one trained model runs. */
struct SCENARIOCORE_API FScenarioTrainedInfo
{
	FString Id;
	FString Name;
	/** The record's type, for example flux.1-lora. */
	FString Type;
	EScenarioTrainedKind Kind = EScenarioTrainedKind::None;
	/** Bases that take it, best first; the first one runs it. */
	TArray<FString> BaseIds;
	TArray<FString> BaseNames;
	/** LoRA weight: the record's own, else the default, within the base's range. */
	double Scale = 0.75;
	double ScaleMin = 0.0;
	double ScaleMax = 2.0;
	double ScaleStep = 0.05;
	FString PromptField = TEXT("prompt");
	/** Words that call the trained look up, empty when the record has none. */
	FString TriggerWords;

	bool IsRunnable() const { return Kind != EScenarioTrainedKind::None && BaseIds.Num() > 0; }
	FString BaseId() const { return BaseIds.Num() > 0 ? BaseIds[0] : FString(); }
	FString BaseName() const { return BaseNames.Num() > 0 ? BaseNames[0] : BaseId(); }
	/** "LoRA via FLUX.1 Dev", "Composition via FLUX.1 Dev", or "No base model for <type>". */
	FString Label() const;
	/** Why it cannot run, for forms and agents; empty when it can. */
	FString Problem() const;
	/** For model_schema: kind, base, weight and its range, trigger words. */
	TSharedRef<FJsonObject> ToJson() const;
};

namespace ScenarioTrained
{
	/** The trained form's LoRA weight (a number, sent to the base as lorasScale [weight]). */
	inline constexpr const TCHAR* WeightField = TEXT("lorasScale");
	/** The trained form's switch that puts the trigger words before the prompt (true when left out). */
	inline constexpr const TCHAR* TriggerField = TEXT("addTriggerWords");

	/** A type that needs a base model: set and not custom. */
	SCENARIOCORE_API bool IsTrainedType(const FString& Type);
	/** lora, composition or empty. */
	SCENARIOCORE_API FString KindName(EScenarioTrainedKind Kind);

	/** Kind and bases from the type, ordered by compliantModelIds; the weight; the trigger words. */
	SCENARIOCORE_API FScenarioTrainedInfo Derive(const FScenarioModelSummary& Model, const TArray<FScenarioTrainedBase>& Bases, const FScenarioTrainedData& Data);

	/** parameters.conceptPrompt, else promptEmbedding, else a capitalized word every sample prompt holds (two or more). */
	SCENARIOCORE_API FString TriggerWords(const TSharedPtr<FJsonObject>& Record);
	/** The weight concepts[] gives the record's own id, or -1. */
	SCENARIOCORE_API double OwnScale(const TSharedPtr<FJsonObject>& Record);
	/** The prompt with the trigger words in front, once, unless it already holds them (any case). */
	SCENARIOCORE_API FString WithTriggerWords(const FString& Prompt, const FString& Words);

	/**
	 * The trained model's form: the base's schema without its own trained
	 * inputs (modelId, loras, lorasScale), plus the LoRA weight and the trigger
	 * words switch, and a "trained" object describing how it runs.
	 */
	SCENARIOCORE_API TSharedRef<FJsonObject> FormSchema(const FScenarioTrainedInfo& Info, const TSharedPtr<FJsonObject>& BaseSchema);
	/** A payload of the trained form as the base's parameters: loras and lorasScale, or modelId; trigger words in the prompt. */
	SCENARIOCORE_API TSharedRef<FJsonObject> BaseParameters(const FScenarioTrainedInfo& Info, const TSharedPtr<FJsonObject>& Payload);
	/** The base's parameters back as values of the trained form (Use settings). */
	SCENARIOCORE_API TSharedRef<FJsonObject> FormValues(const TSharedPtr<FJsonObject>& Parameters);
	/** The trained model the base's parameters carry (loras[0], else modelId), or empty. */
	SCENARIOCORE_API FString AttachedId(const TSharedPtr<FJsonObject>& Parameters);
	/** True for a payload of a trained form (its weight as a number, or the trigger words switch): never sent as it is. */
	SCENARIOCORE_API bool HasFormFields(const TSharedPtr<FJsonObject>& Payload);
}
