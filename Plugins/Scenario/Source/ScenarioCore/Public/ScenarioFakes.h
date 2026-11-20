// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioHttp.h"
#include "ScenarioRestClient.h"

/**
 * Test doubles shared by the automation tests of the three modules.
 * They never touch the network.
 */

/** One request seen by the fake transport. */
struct SCENARIOCORE_API FScenarioFakeRequest
{
	FString Verb;
	FString Url;
	TMap<FString, FString> Headers;
	TArray<uint8> Body;
	float Timeout = 0.0f;

	FString BodyText() const;
	TSharedPtr<FJsonObject> BodyJson() const;
	/** The URL's path, without the query. */
	FString Path() const;
	/** A decoded query parameter, or empty. */
	FString Query(const FString& Name) const;
};

/**
 * Scripted HTTP: each Send takes the next response from the queue (or asks
 * the Responder), answers synchronously, and records the request. Delay runs
 * the callback at once and records the delay.
 */
class SCENARIOCORE_API FScenarioFakeTransport : public IScenarioHttpTransport
{
public:
	virtual void Send(const FString& Verb, const FString& Url, const TMap<FString, FString>& Headers, const TArray<uint8>& Body,
		float TimeoutSeconds, FScenarioHttpCallback OnDone, FScenarioProgressCallback OnProgress = nullptr) override;
	virtual void Delay(float Seconds, TFunction<void()> OnDone) override;

	void Enqueue(const FScenarioHttpResponse& Response) { Queue.Add(Response); }
	void EnqueueJson(int32 Status, const FString& Body, const TMap<FString, FString>& Headers = {});
	void EnqueueTransportError(EScenarioErrorKind Kind = EScenarioErrorKind::Transport);

	/** Used when the queue is empty. */
	TFunction<FScenarioHttpResponse(const FScenarioFakeRequest&)> Responder;

	TArray<FScenarioFakeRequest> Requests;
	TArray<float> Delays;

	static FScenarioHttpResponse Json(int32 Status, const FString& Body, const TMap<FString, FString>& Headers = {});

private:
	TArray<FScenarioHttpResponse> Queue;
};

/**
 * IScenarioBackend answering requests from handlers, synchronously. A route is
 * the verb and the path, where a segment written * stands for any one segment
 * (the model id of a run, say). An exact route wins over a pattern.
 */
class SCENARIOCORE_API FScenarioFakeBackend : public IScenarioBackend
{
public:
	using FHandler = TFunction<FScenarioReply(const FScenarioRequest& Request)>;

	virtual void Send(const FScenarioRequest& Request, FScenarioReplyCallback OnDone) override;
	virtual bool HasCredentials() const override { return bSignedIn; }
	virtual bool UsesApiKey() const override { return bApiKey; }
	virtual bool IsEstimateAlarmRaised() const override { return false; }

	void On(const FString& Route, FHandler Handler) { Handlers.Add(Route, MoveTemp(Handler)); }
	/** Answers the route with this JSON object text. */
	void OnJson(const FString& Route, const FString& Json);

	bool bSignedIn = true;
	/** Answers as an API key would be answered; off by default so GET /teams is used. */
	bool bApiKey = false;
	TArray<FScenarioRequest> Calls;
	int32 CountOf(const FString& Route) const;
	/** The last request that matched the route, or null. */
	const FScenarioRequest* Last(const FString& Route) const;
	static bool Matches(const FString& Route, const FScenarioRequest& Request);

private:
	TMap<FString, FHandler> Handlers;
};

namespace ScenarioFixtures
{
	/** Folder of the recorded fixtures (Plugins/Scenario/Tests/Fixtures), empty when the plugin ships without them. */
	SCENARIOCORE_API FString Dir();
	/** Reads a fixture file. False when it is missing. */
	SCENARIOCORE_API bool Load(const FString& Name, FString& OutText);
	SCENARIOCORE_API TSharedPtr<FJsonObject> LoadJson(const FString& Name);
}

/** Flags shared by every Scenario automation test: editor only, engine filter (runs headless with -nullrhi). */
#define SCENARIO_TEST_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
