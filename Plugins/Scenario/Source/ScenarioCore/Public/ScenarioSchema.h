// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

enum class EScenarioFieldType : uint8
{
	Prompt,
	Text,
	Enum,
	Number,
	Integer,
	Boolean,
	/** A list of allowed values (string_array). */
	Multi,
	/** One asset id. */
	File,
	/** A list of asset ids. */
	Files,
};

struct SCENARIOCORE_API FScenarioField
{
	FString Name;
	FString Label;
	FString Description;
	EScenarioFieldType Type = EScenarioFieldType::Text;
	bool bRequired = false;
	TSharedPtr<FJsonValue> Default;
	TOptional<double> Min;
	TOptional<double> Max;
	TOptional<double> Step;
	TArray<TSharedPtr<FJsonValue>> Options;
	/** For files: image, video, audio, 3d. */
	FString Kind;
	int32 MaxItems = 0;
	int32 MaxLength = 0;
	/** Changing it changes the price (shown with a coin in the form). */
	bool bCostImpact = false;
	bool bPromptSpark = false;
	FString Group;
	FString Placeholder;
	/** Shown after numbers: "s", "px", "m", "fps". */
	FString Unit;
	/** Display names of enum and multi values ("basecolor" -> "Base color"). */
	TMap<FString, FString> OptionLabels;

	bool IsFile() const { return Type == EScenarioFieldType::File || Type == EScenarioFieldType::Files; }
	/** Shown above "More settings": prompt, required fields, files, and fields that change the price. */
	bool IsPrimary() const;
};

/**
 * Turns a model schema (model_schema_get "parameters", snake_case) or a
 * workflow's inputs (camelCase, required as {always}) into form fields,
 * validates values, and builds the run payload.
 */
namespace ScenarioSchema
{
	/** Restrict limits enum and multi fields to values Unreal can import. */
	SCENARIOCORE_API TArray<FScenarioField> FieldsFromParameters(const TArray<TSharedPtr<FJsonValue>>& Parameters, const TMap<FString, TArray<FString>>& Restrict = {});
	/** The "parameters" array of a model schema, or the inputs of a workflow record. */
	SCENARIOCORE_API TArray<FScenarioField> FieldsFromSchema(const TSharedPtr<FJsonObject>& Schema, const TMap<FString, TArray<FString>>& Restrict = {});

	SCENARIOCORE_API FString PromptName(const TArray<FScenarioField>& Fields);
	SCENARIOCORE_API const FScenarioField* Find(const TArray<FScenarioField>& Fields, const FString& Name);

	/** camelCase or snake_case to "Sentence case". */
	SCENARIOCORE_API FString Humanize(const FString& Name);
	/** An enum or multi value as people read it: the field's label for it, else "standard" -> "Standard", "HIGH" -> "High", "" -> "Default". */
	SCENARIOCORE_API FString OptionText(const FScenarioField& Field, const TSharedPtr<FJsonValue>& Value);

	/** Schema defaults, then presets on top. */
	SCENARIOCORE_API TSharedRef<FJsonObject> InitialValues(const TArray<FScenarioField>& Fields, const TSharedPtr<FJsonObject>& Presets = nullptr);

	/** Problems that block a price check, as short sentences. Empty means valid. */
	SCENARIOCORE_API TArray<FString> Validate(const TArray<FScenarioField>& Fields, const TSharedPtr<FJsonObject>& Values);

	/** The run payload: known fields only, empty values dropped, integers as integers, lists as arrays. */
	SCENARIOCORE_API TSharedRef<FJsonObject> Payload(const TArray<FScenarioField>& Fields, const TSharedPtr<FJsonObject>& Values);

	/** Asset ids used by file fields in a payload, in field order. */
	SCENARIOCORE_API TArray<FString> InputAssetIds(const TArray<FScenarioField>& Fields, const TSharedPtr<FJsonObject>& Payload);

	SCENARIOCORE_API FString TypeName(EScenarioFieldType Type);
}
