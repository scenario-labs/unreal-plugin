// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioTypes.h"

/**
 * Reads a Scenario MCP HTTP reply, plain JSON or a server-sent event stream,
 * and returns the final JSON-RPC message that answers one request id.
 */
namespace ScenarioRpc
{
	/**
	 * OK(envelope) where the envelope holds "result" or "error". A notification
	 * (RequestId < 0) with an empty body is OK with an empty object.
	 */
	SCENARIOCORE_API FScenarioReply ParseReply(const FString& Body, const FString& ContentType, int64 RequestId);

	/** The JSON messages carried by a server-sent event stream; malformed events are skipped. */
	SCENARIOCORE_API TArray<TSharedPtr<FJsonValue>> StreamMessages(const FString& Source);
}
