// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioLocalHttpServer.h"

class FJsonObject;
struct FScenarioAction;

/**
 * The local MCP server: JSON-RPC over POST /mcp on 127.0.0.1, JSON replies
 * (no streaming), GET /health. Every request passes the Host and Origin
 * checks; /mcp also needs the bearer token unless the person turned it off.
 * Tool calls go to FScenarioActions with the agent's permissions.
 */
class SCENARIOAGENT_API FScenarioMcpServer : public TSharedFromThis<FScenarioMcpServer>
{
public:
	static constexpr int32 DefaultPort = 9877;
	static constexpr int32 PortTries = 10;
	static const TCHAR* ServerName;

	/** Binds the first free port in [FirstPort, FirstPort + PortTries). */
	bool Start(int32 FirstPort, FString& OutError);
	void Stop();
	bool IsRunning() const;
	int32 GetPort() const { return Port; }
	FString Url() const;

	FString Token;
	bool bRequireToken = true;

	/** Request handling, callable without sockets (tests). Port is the one the Host header must name. */
	void Handle(const FScenarioLocalRequest& Request, FScenarioLocalRespond Respond);
	void SetPortForTests(int32 InPort) { Port = InPort; }

	/** tools/list result, also used by the size budget test. */
	static TSharedRef<FJsonObject> ToolsList();
	static TSharedRef<FJsonObject> ToolJson(const FScenarioAction& Action);

	struct FClient
	{
		FString Name;
		FString Version;
		double LastSeen = 0.0;
	};
	/** By MCP session id. */
	const TMap<FString, FClient>& Clients() const { return Sessions; }
	/** Last time a client whose name contains Fragment (case-insensitive) talked to the server, or 0. */
	double LastSeen(const FString& Fragment) const;

	FSimpleMulticastDelegate OnClientsChanged;

private:
	void HandleRpc(const TSharedPtr<FJsonObject>& Message, const FString& SessionId, FScenarioLocalRespond Respond);
	static FScenarioLocalResponse RpcResult(const TSharedPtr<class FJsonValue>& Id, const TSharedRef<FJsonObject>& Result, const FString& SessionHeader = FString());
	static FScenarioLocalResponse RpcError(const TSharedPtr<class FJsonValue>& Id, int32 Code, const FString& Message);
	bool TokenMatches(const FString& Authorization) const;

	TSharedPtr<FScenarioLocalHttpServer, ESPMode::ThreadSafe> Listener;
	int32 Port = 0;
	TMap<FString, FClient> Sessions;
};
