// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioHttp.h"
#include "ScenarioTypes.h"

/** What a request can do to the person's Creative Units, which decides retries and SCENARIO_NO_SPEND. */
enum class EScenarioCall : uint8
{
	/** Reads only: retried after a network failure, 429 or 5xx. */
	Read,
	/** A free price check: retried like a read; a reply that names a job raises the alarm. */
	DryRun,
	/** Can spend or create something: sent once, refused under SCENARIO_NO_SPEND. */
	Paid,
	/** Can only lower spend (cancelling a job): sent once, allowed under SCENARIO_NO_SPEND. */
	Cancel,
};

/** What a signed-in person's request names of the workspace (an API key is already scoped to its project). */
enum class EScenarioScope : uint8
{
	/** teamId and projectId. */
	Project,
	/** teamId only. */
	Team,
	/** Nothing: the request names its own scope, or must not carry one. */
	None,
};

/** One call to Scenario's REST API. */
struct SCENARIOCORE_API FScenarioRequest
{
	FString Verb = TEXT("GET");
	/** Path under the API address, for example /models/model_abc. */
	FString Path;
	TArray<TPair<FString, FString>> Query;
	TSharedPtr<FJsonObject> Body;
	EScenarioCall Call = EScenarioCall::Read;
	EScenarioScope Scope = EScenarioScope::Project;
	float TimeoutSeconds = 60.0f;

	static FScenarioRequest Get(const FString& Path);
	static FScenarioRequest Post(const FString& Path, const TSharedPtr<FJsonObject>& Body, EScenarioCall Call);
	/** Adds a query parameter; an empty value is left out. */
	FScenarioRequest& Param(const FString& Name, const FString& Value);
	FScenarioRequest& Param(const FString& Name, int32 Value);
	FScenarioRequest& WithScope(EScenarioScope InScope);
	FScenarioRequest& WithTimeout(float Seconds);

	/** Path segment encoding, for ids put in a path. */
	static FString Segment(const FString& Id);
	/** A query parameter's value, or empty. */
	FString QueryValue(const FString& Name) const;
	bool IsSpend() const { return Call == EScenarioCall::Paid; }
	bool IsRetryable() const { return Call == EScenarioCall::Read || Call == EScenarioCall::DryRun; }
};

/** What the plugin needs from Scenario: one REST request at a time. */
class SCENARIOCORE_API IScenarioBackend
{
public:
	virtual ~IScenarioBackend() = default;

	/** OK carries the reply's JSON object (empty for an empty body). Callbacks run on the game thread. */
	virtual void Send(const FScenarioRequest& Request, FScenarioReplyCallback OnDone) = 0;

	virtual bool HasCredentials() const = 0;

	/** True for an API key, scoped to its project: it cannot list teams. */
	virtual bool UsesApiKey() const = 0;

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
	/** A signed-in person names the team and project on each request; an API key is already scoped. */
	virtual bool NeedsWorkspaceArguments() const = 0;
	/** One silent refresh after the credentials were refused. OnDone(true) when a new token is ready. */
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
 * Scenario's REST API over HTTPS, JSON in and out.
 *
 * Money rules (spec section 7.1): a paid request is sent once and never
 * retried; a network failure, timeout, 408 or 5xx on it is ambiguous
 * (Scenario may have started the job); a price check that returns a job id is
 * an alarm. Credentials the API's gateway refuses (401, or 403 with
 * x-amzn-errortype AccessDeniedException) never reached a handler, so they are
 * renewed and the request resent once, paid or not.
 */
class SCENARIOCORE_API FScenarioRestClient : public IScenarioBackend, public TSharedFromThis<FScenarioRestClient>
{
public:
	static const TCHAR* DefaultApiUrl;
	static constexpr int32 ReadAttempts = 3;
	static constexpr double MaxRetryAfterSeconds = 30.0;

	explicit FScenarioRestClient(TSharedRef<IScenarioHttpTransport> InTransport, const FString& InApiUrl = DefaultApiUrl);

	void SetAuth(TSharedPtr<IScenarioAuthProvider> InAuth);
	TSharedPtr<IScenarioAuthProvider> GetAuth() const { return Auth; }
	/** Workspace a signed-in person's requests name. Ignored for API keys. */
	void SetWorkspace(const FString& InTeamId, const FString& InProjectId);
	const FString& GetTeamId() const { return TeamId; }
	const FString& GetProjectId() const { return ProjectId; }
	const FString& GetApiUrl() const { return ApiUrl; }

	/** SCENARIO_NO_SPEND=1 refuses every paid request before it is sent. Tests can set it directly. */
	bool bSpendingDisabled = false;

	virtual void Send(const FScenarioRequest& Request, FScenarioReplyCallback OnDone) override;
	virtual bool HasCredentials() const override;
	virtual bool UsesApiKey() const override { return Auth.IsValid() && !Auth->NeedsWorkspaceArguments(); }
	virtual bool IsEstimateAlarmRaised() const override { return bEstimateAlarm; }
	void ClearEstimateAlarm() { bEstimateAlarm = false; }

	/** The full URL of a request, with the workspace a signed-in person names. */
	FString UrlFor(const FScenarioRequest& Request) const;

	/** The job id a reply carries, or empty. A price check must carry none. */
	static FString CreatedJobId(const TSharedPtr<FJsonObject>& Value);
	/** Back-off before retry number Attempt+1, or -1 to stop (Retry-After too long). */
	static double RetryDelay(int32 Attempt, double RetryAfterSeconds);
	/** The gateway refused the credentials before any handler ran. */
	static bool IsGatewayRefusal(const FScenarioHttpResponse& Response);
	/** An HTTP answer to a reply or an error, as a paid or free request sees it. */
	static FScenarioReply ReadResponse(const FScenarioHttpResponse& Response, bool bPaid);

private:
	void Attempt(const FScenarioRequest& Request, int32 AttemptIndex, int32 Attempts, FScenarioReplyCallback OnDone);
	void SendOnce(const FScenarioRequest& Request, bool bAllowRefresh, FScenarioReplyCallback OnDone);

	TSharedRef<IScenarioHttpTransport> Transport;
	FString ApiUrl;
	TSharedPtr<IScenarioAuthProvider> Auth;
	FString TeamId;
	FString ProjectId;
	bool bEstimateAlarm = false;
};
