// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioHttp.h"
#include "ScenarioTypes.h"

/** What the plugin needs from Scenario. v1 implements it over the hosted Scenario MCP. */
class SCENARIOCORE_API IScenarioBackend
{
public:
	virtual ~IScenarioBackend() = default;

	/** One tool call. OK carries the tool's structured result. Callbacks run on the game thread. */
	virtual void CallTool(const FString& Tool, const TSharedPtr<FJsonObject>& Arguments, FScenarioReplyCallback OnDone, float TimeoutSeconds = 60.0f) = 0;

	virtual bool HasCredentials() const = 0;

	/** True once a price check returned a job: automatic price checks stay off for the session. */
	virtual bool IsEstimateAlarmRaised() const = 0;
};

/** Supplies the Authorization header. Implemented by the API key and OAuth sign-ins. */
class SCENARIOCORE_API IScenarioAuthProvider
{
public:
	virtual ~IScenarioAuthProvider() = default;

	virtual bool HasCredentials() const = 0;
	/** "Basic ..." or "Bearer ...". */
	virtual FString AuthorizationHeader() const = 0;
	/** OAuth callers pass team_id and project_id on each call; an API key is already scoped. */
	virtual bool NeedsWorkspaceArguments() const = 0;
	/** One silent refresh after a 401. OnDone(true) when a new token is ready. */
	virtual void Refresh(TFunction<void(bool bRefreshed)> OnDone) { OnDone(false); }
};

/** API key and secret, sent as HTTP Basic. */
class SCENARIOCORE_API FScenarioApiKeyAuth : public IScenarioAuthProvider
{
public:
	FScenarioApiKeyAuth(const FString& InKey, const FString& InSecret);

	virtual bool HasCredentials() const override;
	virtual FString AuthorizationHeader() const override;
	virtual bool NeedsWorkspaceArguments() const override { return false; }

	const FString& GetKey() const { return Key; }

private:
	FString Key;
	FString Secret;
};

/**
 * Scenario MCP client: JSON-RPC 2.0 over HTTPS with JSON or SSE replies.
 *
 * Money rules (spec section 7.1): a paid call is sent once and never retried;
 * a transport failure on a paid call is ambiguous (Scenario may have started
 * the job); a price check that returns a job id is an alarm. Only the reads in
 * ReadTools() are retried.
 */
class SCENARIOCORE_API FScenarioMcpClient : public IScenarioBackend, public TSharedFromThis<FScenarioMcpClient>
{
public:
	static const TCHAR* DefaultEndpoint;
	static const TCHAR* ProtocolVersion;
	static constexpr int32 ReadAttempts = 3;
	static constexpr double MaxRetryAfterSeconds = 30.0;

	explicit FScenarioMcpClient(TSharedRef<IScenarioHttpTransport> InTransport, const FString& InEndpoint = DefaultEndpoint);

	void SetAuth(TSharedPtr<IScenarioAuthProvider> InAuth);
	TSharedPtr<IScenarioAuthProvider> GetAuth() const { return Auth; }
	/** Workspace for OAuth calls. Ignored for API keys. */
	void SetWorkspace(const FString& InTeamId, const FString& InProjectId);
	const FString& GetTeamId() const { return TeamId; }
	const FString& GetProjectId() const { return ProjectId; }
	const FString& GetEndpoint() const { return Endpoint; }

	/** SCENARIO_NO_SPEND=1 refuses every paid call before any request is made. Tests can set it directly. */
	bool bSpendingDisabled = false;

	virtual void CallTool(const FString& Tool, const TSharedPtr<FJsonObject>& Arguments, FScenarioReplyCallback OnDone, float TimeoutSeconds = 60.0f) override;
	virtual bool HasCredentials() const override;
	virtual bool IsEstimateAlarmRaised() const override { return bEstimateAlarm; }
	void ClearEstimateAlarm() { bEstimateAlarm = false; }

	/** Forgets the MCP session (next call initializes again). */
	void ResetSession();

	static const TArray<FString>& ReadTools();
	static const TArray<FString>& SubmitTools();
	/** True when the call can spend Creative Units or create something. */
	static bool IsSubmission(const FString& Tool, const TSharedPtr<FJsonObject>& Arguments);
	static bool IsRetryable(const FString& Tool, const TSharedPtr<FJsonObject>& Arguments);
	static bool IsDryRun(const FString& Tool, const TSharedPtr<FJsonObject>& Arguments);
	/** The job id a reply carries, or empty. A price check must carry none. */
	static FString CreatedJobId(const TSharedPtr<FJsonObject>& Value);
	/** Back-off before retry number Attempt+1, or -1 to stop (Retry-After too long). */
	static double RetryDelay(int32 Attempt, double RetryAfterSeconds);

private:
	using FInitWaiter = TFunction<void(const FScenarioReply&)>;

	void Attempt(const FString& Tool, TSharedPtr<FJsonObject> Arguments, float TimeoutSeconds, bool bSubmit, int32 AttemptIndex, int32 Attempts, FScenarioReplyCallback OnDone);
	void CallOnce(const FString& Tool, TSharedPtr<FJsonObject> Arguments, float TimeoutSeconds, bool bSubmit, FScenarioReplyCallback OnDone);
	void Rpc(const FString& Method, TSharedPtr<FJsonObject> Params, float TimeoutSeconds, FScenarioReplyCallback OnDone);
	void EnsureInitialized(float TimeoutSeconds, FInitWaiter OnReady);
	void FinishInitialize(const FScenarioReply& Result);
	void Post(const FString& Method, TSharedPtr<FJsonObject> Params, int64 RequestId, float TimeoutSeconds, bool bAllowSessionSignal, bool bAllowRefresh, FScenarioReplyCallback OnDone);
	FScenarioReply ReadResponse(const FScenarioHttpResponse& Response, int64 RequestId, bool bAllowSessionSignal, const FString& SessionUsed);
	int64 TakeId() { return NextId++; }

	TSharedRef<IScenarioHttpTransport> Transport;
	FString Endpoint;
	TSharedPtr<IScenarioAuthProvider> Auth;
	FString TeamId;
	FString ProjectId;
	FString SessionId;
	bool bInitialized = false;
	bool bInitializing = false;
	/** Bumped by ResetSession and SetAuth so an old initialize cannot mark a new session ready. */
	int32 SessionGeneration = 0;
	TArray<FInitWaiter> InitWaiters;
	bool bEstimateAlarm = false;
	int64 NextId = 1;
};
