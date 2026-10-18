// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioTypes.h"

/**
 * Error classification shared by the core.
 *
 * Money rules (spec section 7.1): a paid call is sent once and never retried.
 * A transport failure, timeout, 5xx or unreadable reply on a paid call is
 * "ambiguous" (Scenario may have started the job). Only allow-listed reads are
 * retried, and only for the retryable kinds below.
 */
namespace FScenarioErrors
{
	/** Failures where Scenario may still have received and run the request. */
	SCENARIOCORE_API bool IsAmbiguousKind(EScenarioErrorKind Kind);

	/** Failures worth retrying, for allow-listed reads only. */
	SCENARIOCORE_API bool IsRetryableKind(EScenarioErrorKind Kind);

	SCENARIOCORE_API bool IsModerated(const FString& Text);

	/** Kind of a failure from the server's text and the HTTP status (0 when the error came inside a 200 reply). */
	SCENARIOCORE_API EScenarioErrorKind Classify(const FString& Detail, int32 HttpStatus = 0);

	/** The sentence shown to people, plus Scenario's detail when it adds something. */
	SCENARIOCORE_API FString MessageFor(EScenarioErrorKind Kind, const FString& Detail = FString());

	/** Removes signed URL query strings and anything that looks like a credential from text meant for logs. */
	SCENARIOCORE_API FString Scrub(const FString& Text);
}
