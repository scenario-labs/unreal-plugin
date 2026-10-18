// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioTypes.h"

struct SCENARIOCORE_API FScenarioHttpResponse
{
	/** 0 when no HTTP response arrived. */
	int32 Status = 0;
	/** Lower-case header names. */
	TMap<FString, FString> Headers;
	TArray<uint8> Body;
	/** Transport or Timeout when no response arrived, None otherwise. */
	EScenarioErrorKind TransportError = EScenarioErrorKind::None;

	FString BodyText() const;
	FString Header(const FString& Name) const;
};

using FScenarioHttpCallback = TFunction<void(const FScenarioHttpResponse&)>;
using FScenarioProgressCallback = TFunction<void(uint64 BytesSent, uint64 BytesReceived)>;

/**
 * The network as the core sees it. The editor uses FScenarioHttpTransport;
 * tests inject a fake. Callbacks run on the game thread.
 */
class SCENARIOCORE_API IScenarioHttpTransport
{
public:
	virtual ~IScenarioHttpTransport() = default;

	virtual void Send(const FString& Verb, const FString& Url, const TMap<FString, FString>& Headers, const TArray<uint8>& Body,
		float TimeoutSeconds, FScenarioHttpCallback OnDone, FScenarioProgressCallback OnProgress = nullptr) = 0;

	/** Calls OnDone after the delay, on the game thread. */
	virtual void Delay(float Seconds, TFunction<void()> OnDone) = 0;
};

/** FHttpModule-backed transport. */
class SCENARIOCORE_API FScenarioHttpTransport : public IScenarioHttpTransport
{
public:
	virtual void Send(const FString& Verb, const FString& Url, const TMap<FString, FString>& Headers, const TArray<uint8>& Body,
		float TimeoutSeconds, FScenarioHttpCallback OnDone, FScenarioProgressCallback OnProgress = nullptr) override;

	virtual void Delay(float Seconds, TFunction<void()> OnDone) override;
};

namespace ScenarioUrl
{
	/** Lower-case host of an absolute URL, without port or credentials. */
	SCENARIOCORE_API FString Host(const FString& Url);
	SCENARIOCORE_API bool IsHttps(const FString& Url);
	SCENARIOCORE_API bool IsLoopbackHost(const FString& Host);
	/** application/x-www-form-urlencoded component encoding. */
	SCENARIOCORE_API FString Encode(const FString& Text);
	SCENARIOCORE_API FString Decode(const FString& Text);
	/** "a=1&b=2" to a map (values decoded). */
	SCENARIOCORE_API TMap<FString, FString> ParseQuery(const FString& Query);
	SCENARIOCORE_API FString BuildQuery(const TArray<TPair<FString, FString>>& Pairs);
}
