// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FScenarioMcpServer;

/** One agent call, for the activity log. */
struct SCENARIOAGENT_API FScenarioAgentActivity
{
	double Time = 0.0;
	FString Client;
	FString Tool;
	bool bOk = false;
	FString Error;
	/** CU of the quote when the call spent, else empty. */
	FString Spent;
	double Seconds = 0.0;
};

/** The local MCP server for agents, its token, the activity log and the Agents section of the Connection page. */
class SCENARIOAGENT_API FScenarioAgentModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FScenarioAgentModule& Get();
	static bool IsAvailable();

	TSharedPtr<FScenarioMcpServer> Server() const { return ServerPtr; }
	/** Starts the server on the configured port (or the next nine). False with OutError when it cannot listen. */
	bool StartServer(FString& OutError);
	void StopServer();

	/** The bearer token, created once per user and kept in the credential store. */
	FString Token();
	/** A new token: every connected client must be connected again. */
	void RotateToken();

	/** Newest last, at most 200. */
	const TArray<FScenarioAgentActivity>& Activity() const { return Log; }
	FSimpleMulticastDelegate OnActivity;

private:
	void OnActionCalled(const FString& Name, const TSharedPtr<class FJsonObject>& Args, const struct FScenarioCaller& Caller, const struct FScenarioActionResult& Result, double Seconds);

	TSharedPtr<FScenarioMcpServer> ServerPtr;
	TArray<FScenarioAgentActivity> Log;
	FString CachedToken;
	FDelegateHandle CalledHandle;
};
