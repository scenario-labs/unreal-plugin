// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioHttp.h"
#include "ScenarioMcpClient.h"

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
	/** JSON-RPC method of an MCP request, or empty. */
	FString RpcMethod() const;
	/** Tool name of a tools/call request, or empty. */
	FString ToolName() const;
	TSharedPtr<FJsonObject> ToolArguments() const;
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
	void EnqueueSse(int32 Status, const FString& Body, const TMap<FString, FString>& Headers = {});
	void EnqueueTransportError(EScenarioErrorKind Kind = EScenarioErrorKind::Transport);

	/** Used when the queue is empty. */
	TFunction<FScenarioHttpResponse(const FScenarioFakeRequest&)> Responder;

	TArray<FScenarioFakeRequest> Requests;
	TArray<float> Delays;
	/** Requests to tools/call, in order (initialize and notifications left out). */
	TArray<FScenarioFakeRequest> ToolCalls() const;

	static FScenarioHttpResponse Json(int32 Status, const FString& Body, const TMap<FString, FString>& Headers = {});
	/** A JSON-RPC result envelope for a tools/call, with the value as structuredContent. */
	static FString ToolResult(int64 Id, const FString& StructuredJson, bool bIsError = false);
	static FString InitializeResult(int64 Id);

private:
	TArray<FScenarioHttpResponse> Queue;
};

/**
 * A Scenario MCP server in a box: answers initialize and the notification,
 * and serves tools/call from per-tool handlers. Plug it into the fake
 * transport's Responder.
 */
class SCENARIOCORE_API FScenarioFakeMcpServer : public TSharedFromThis<FScenarioFakeMcpServer>
{
public:
	using FToolHandler = TFunction<FScenarioHttpResponse(const TSharedPtr<FJsonObject>& Arguments, int64 RequestId)>;

	/** Returns structured content JSON for the tool. */
	void On(const FString& Tool, TFunction<FString(const TSharedPtr<FJsonObject>& Arguments)> Handler);
	/** Full control over the HTTP response. */
	void OnRaw(const FString& Tool, FToolHandler Handler);

	FScenarioHttpResponse Handle(const FScenarioFakeRequest& Request);
	void Attach(FScenarioFakeTransport& Transport);

	FString SessionId = TEXT("fake-session");
	TMap<FString, int32> CallCounts;

private:
	TMap<FString, FToolHandler> Handlers;
};

/** IScenarioBackend answering tool calls from handlers, synchronously. */
class SCENARIOCORE_API FScenarioFakeBackend : public IScenarioBackend
{
public:
	using FHandler = TFunction<FScenarioReply(const TSharedPtr<FJsonObject>& Arguments)>;

	virtual void CallTool(const FString& Tool, const TSharedPtr<FJsonObject>& Arguments, FScenarioReplyCallback OnDone, float TimeoutSeconds = 60.0f) override;
	virtual bool HasCredentials() const override { return bSignedIn; }
	virtual bool IsEstimateAlarmRaised() const override { return false; }

	void On(const FString& Tool, FHandler Handler) { Handlers.Add(Tool, MoveTemp(Handler)); }
	/** Answers the tool with this JSON object text. */
	void OnJson(const FString& Tool, const FString& Json);

	bool bSignedIn = true;
	TArray<TPair<FString, TSharedPtr<FJsonObject>>> Calls;
	int32 CountOf(const FString& Tool) const;

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
