// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/** Small JSON helpers. Scenario replies are read defensively: a missing or mistyped field gives the default. */
namespace ScenarioJson
{
	SCENARIOCORE_API TSharedPtr<FJsonObject> ParseObject(const FString& Text);
	SCENARIOCORE_API TSharedPtr<FJsonValue> ParseValue(const FString& Text);

	SCENARIOCORE_API FString Write(const TSharedPtr<FJsonObject>& Object, bool bPretty = false);

	/** The object's fields with FString keys, on every supported engine version (5.8 stores keys as shared strings). */
	SCENARIOCORE_API TArray<TPair<FString, TSharedPtr<FJsonValue>>> Fields(const TSharedPtr<FJsonObject>& Object);
	SCENARIOCORE_API FString WriteValue(const TSharedPtr<FJsonValue>& Value, bool bPretty = false);

	/** Keys sorted at every level, whole numbers written as integers, no whitespace. Used for quote fingerprints. */
	SCENARIOCORE_API FString Canonical(const TSharedPtr<FJsonValue>& Value);
	SCENARIOCORE_API FString Canonical(const TSharedPtr<FJsonObject>& Object);

	SCENARIOCORE_API FString EscapeString(const FString& Text);

	/** 11 as "11", 2.75 as "2.75", 0.1 as "0.1" (shortest form that reads back the same). */
	SCENARIOCORE_API FString NumberText(double Value);

	SCENARIOCORE_API TSharedPtr<FJsonValue> Clone(const TSharedPtr<FJsonValue>& Value);
	SCENARIOCORE_API TSharedPtr<FJsonObject> CloneObject(const TSharedPtr<FJsonObject>& Object);

	SCENARIOCORE_API FString GetString(const TSharedPtr<FJsonObject>& Object, const FString& Field, const FString& Default = FString());
	SCENARIOCORE_API double GetNumber(const TSharedPtr<FJsonObject>& Object, const FString& Field, double Default = 0.0);
	SCENARIOCORE_API bool GetBool(const TSharedPtr<FJsonObject>& Object, const FString& Field, bool Default = false);
	SCENARIOCORE_API bool HasNumber(const TSharedPtr<FJsonObject>& Object, const FString& Field);
	SCENARIOCORE_API TSharedPtr<FJsonObject> GetObject(const TSharedPtr<FJsonObject>& Object, const FString& Field);
	SCENARIOCORE_API TArray<TSharedPtr<FJsonValue>> GetArray(const TSharedPtr<FJsonObject>& Object, const FString& Field);
	SCENARIOCORE_API TArray<FString> GetStringArray(const TSharedPtr<FJsonObject>& Object, const FString& Field);

	/** Value as text: strings as-is, numbers in NumberText form, booleans as true or false, others as JSON. */
	SCENARIOCORE_API FString ValueText(const TSharedPtr<FJsonValue>& Value);

	/** "truthy" the way schemas use it: true, non-zero numbers, "true", "1", "yes". */
	SCENARIOCORE_API bool Truthy(const TSharedPtr<FJsonValue>& Value);

	SCENARIOCORE_API bool IsEmptyValue(const TSharedPtr<FJsonValue>& Value);

	SCENARIOCORE_API TSharedRef<FJsonValue> StringArrayValue(const TArray<FString>& Items);
}
