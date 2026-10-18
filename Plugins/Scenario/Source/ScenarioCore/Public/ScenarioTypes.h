// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/** Why a call to Scenario failed. The money rules depend on the kind (see FScenarioErrors). */
enum class EScenarioErrorKind : uint8
{
	None,
	Auth,
	Forbidden,
	Quota,
	Moderated,
	RateLimit,
	NotFound,
	Invalid,
	Transport,
	Timeout,
	Http5xx,
	Protocol,
	Local,
	EstimateJob,
	Scenario,
	Cancelled,
};

SCENARIOCORE_API const TCHAR* LexToString(EScenarioErrorKind Kind);

struct SCENARIOCORE_API FScenarioError
{
	EScenarioErrorKind Kind = EScenarioErrorKind::None;
	/** Plain sentence for people. Never contains secrets or signed URLs. */
	FString Message;
	/** Scenario's own text, shortened, for Details views and agents. */
	FString Detail;
	int32 HttpStatus = 0;
	double RetryAfterSeconds = -1.0;
	/** A paid request whose outcome is unknown: Scenario may have started it. */
	bool bAmbiguous = false;
	/** The request never left the machine (failed before dispatch). */
	bool bNotSent = false;
	bool bSessionExpired = false;
	/** Set when a price check unexpectedly returned a job. */
	FString JobId;

	/** "Kind: message", the one-line form used in logs and agent results. */
	FString ToString() const;
};

/** Result of any call to Scenario: a JSON object, or an error. */
struct SCENARIOCORE_API FScenarioReply
{
	bool bOk = false;
	TSharedPtr<FJsonObject> Value;
	FScenarioError Error;

	static FScenarioReply Ok(const TSharedPtr<FJsonObject>& InValue = nullptr);
	static FScenarioReply Fail(EScenarioErrorKind Kind, const FString& Message, const FString& Detail = FString());
	static FScenarioReply Fail(const FScenarioError& InError);
};

using FScenarioReplyCallback = TFunction<void(const FScenarioReply&)>;

/** Seconds since the Unix epoch, with sub-second precision (UTC). */
SCENARIOCORE_API double ScenarioNow();

/** "2026-10-18T09:10:52.353Z" to Unix seconds, or -1 when it cannot be read. */
SCENARIOCORE_API double ScenarioParseIsoTime(const FString& Text);

/** Unix seconds to "2026-10-18T09:10:52Z". */
SCENARIOCORE_API FString ScenarioIsoTime(double UnixSeconds);
