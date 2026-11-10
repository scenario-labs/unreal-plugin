// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Opt-in usage events. Nothing is sent unless the person agreed, and nothing is sent while no
 * collection endpoint is set: version 0.2 has none. Events carry only the allow-listed fields
 * documented in docs/PRIVACY.md, never prompts, file contents, names, ids or credentials.
 */
namespace ScenarioTelemetry
{
	/** Mirrors the person's preference; the editor module keeps it current. */
	SCENARIOCORE_API void SetEnabled(bool bEnabled);
	SCENARIOCORE_API bool IsEnabled();

	/** Collection endpoint. Empty in version 0.2. */
	SCENARIOCORE_API FString Endpoint();

	/** The event body: name, plugin, engine and OS versions, and only the allow-listed fields. */
	SCENARIOCORE_API TSharedRef<FJsonObject> BuildEvent(const FString& Name, const TMap<FString, FString>& Fields);

	/** Sends one event when enabled and an endpoint exists; otherwise does nothing. Fire and forget. */
	SCENARIOCORE_API void Event(const FString& Name, const TMap<FString, FString>& Fields);
}
