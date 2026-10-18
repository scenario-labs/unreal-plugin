// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "Containers/Queue.h"

class FSocket;
class FRunnableThread;

struct SCENARIOCORE_API FScenarioLocalRequest
{
	FString Method;
	/** Path without the query, for example "/mcp". */
	FString Path;
	FString Query;
	/** Lower-case names. Repeated headers keep every value, joined with "\n". */
	TMap<FString, FString> Headers;
	TArray<uint8> Body;
	/** Number of Host headers seen (must be exactly one). */
	int32 HostCount = 0;

	FString Header(const FString& Name) const;
	FString BodyText() const;
};

struct SCENARIOCORE_API FScenarioLocalResponse
{
	int32 Status = 200;
	FString ContentType = TEXT("application/json");
	TArray<uint8> Body;
	TMap<FString, FString> Headers;

	static FScenarioLocalResponse Text(int32 Status, const FString& Body, const FString& ContentType = TEXT("text/plain; charset=utf-8"));
	static FScenarioLocalResponse Json(int32 Status, const FString& Body);
};

/** Called on the game thread. Respond may be called later (asynchronous tools), exactly once. */
using FScenarioLocalRespond = TFunction<void(const FScenarioLocalResponse&)>;
using FScenarioLocalHandler = TFunction<void(const FScenarioLocalRequest&, FScenarioLocalRespond)>;

/**
 * Minimal HTTP/1.1 listener bound to the loopback address only.
 *
 * A background thread accepts connections and reads requests (body limit,
 * idle timeout); each request is handed to the game thread; the reply is
 * written back on the connection's thread and the connection is closed.
 * Used by the OAuth callback and by the local MCP server.
 */
class SCENARIOCORE_API FScenarioLocalHttpServer : public FRunnable, public TSharedFromThis<FScenarioLocalHttpServer, ESPMode::ThreadSafe>
{
public:
	static constexpr int32 DefaultMaxBodyBytes = 10 * 1024 * 1024;
	static constexpr double DefaultIdleSeconds = 60.0;

	FScenarioLocalHttpServer(const FString& InName, FScenarioLocalHandler InHandler);
	virtual ~FScenarioLocalHttpServer() override;

	/** Binds 127.0.0.1 on the first free port in [FirstPort, FirstPort + Tries). Port 0 picks any free port. */
	bool Start(int32 FirstPort, int32 Tries = 1);
	void Stop();
	bool IsRunning() const { return Listener != nullptr; }
	int32 GetPort() const { return Port; }

	int32 MaxBodyBytes = DefaultMaxBodyBytes;
	double IdleSeconds = DefaultIdleSeconds;

	/** Host header check: exactly one Host naming 127.0.0.1, localhost or [::1] with this port (or no port). */
	static bool IsLoopbackHostHeader(const FString& Host, int32 Port);
	/** Origin check: absent, "null" refused, otherwise must be a loopback origin. */
	static bool IsAllowedOrigin(const FString& Origin);
	/** Parses a request head and body from raw bytes. Returns false while incomplete; sets OutError on a bad request. */
	static bool ParseRequest(const TArray<uint8>& Data, int32 MaxBody, FScenarioLocalRequest& Out, FString& OutError, int32& OutStatus);

	// FRunnable
	virtual uint32 Run() override;
	virtual void Exit() override {}

private:
	void ServeConnection(FSocket* Connection);
	static void SendAll(FSocket* Connection, const FScenarioLocalResponse& Response);

	FString Name;
	FScenarioLocalHandler Handler;
	TWeakPtr<FScenarioLocalHttpServer, ESPMode::ThreadSafe> WeakSelf;
	FSocket* Listener = nullptr;
	FRunnableThread* Thread = nullptr;
	FThreadSafeBool bStopping = false;
	int32 Port = 0;
	FThreadSafeCounter OpenConnections;
};
