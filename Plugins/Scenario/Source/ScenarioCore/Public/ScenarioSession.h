// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "ScenarioApi.h"
#include "ScenarioLedger.h"
#include "ScenarioQuotes.h"

class FScenarioCatalog;
class FScenarioDownloader;
class FScenarioOAuth;
class FScenarioUploader;
class IScenarioCredentialStore;
class IScenarioHttpTransport;
struct FScenarioDeviceCode;

/** The Scenario team and project calls run in. */
struct SCENARIOCORE_API FScenarioWorkspace
{
	FString TeamId;
	FString TeamName;
	FString ProjectId;
	FString ProjectName;

	bool IsSet() const { return !ProjectId.IsEmpty(); }
	/** "Team / Project", or the ids when the names are unknown. */
	FString Label() const;
};

enum class EScenarioAuthMode : uint8
{
	None,
	ApiKey,
	OAuth,
};

enum class EScenarioConnection : uint8
{
	SignedOut,
	Connecting,
	/** Signed in, but the account has several projects and none is chosen yet. */
	NeedsWorkspace,
	Ready,
};

/** One paid request: a model run, a workflow run or a Prompt Spark call. */
struct SCENARIOCORE_API FScenarioRunRequest
{
	EScenarioQuoteKind Kind = EScenarioQuoteKind::Model;
	/** Model id, or workflow id for workflows. Prompt Spark uses the target model's id. */
	FString TargetId;
	FString TargetName;
	/** Lane id (image, 3d...), empty when unknown. */
	FString Lane;
	/** The payload as sent: run parameters, workflow inputs, or {prompt, images, num_results} for Prompt Spark. */
	TSharedPtr<FJsonObject> Parameters;
	/** person, agent or script. */
	FString Source = TEXT("person");
	FString Client;
	/** Content Browser folder for this run, empty for the default. */
	FString Destination;

	/** The id the quote is bound to: the target, prefixed for Prompt Spark so it never matches a model run. */
	FString QuoteTarget() const;
};

struct SCENARIOCORE_API FScenarioSessionConfig
{
	/** Folder for the job list, catalog cache, workspace and downloads (the project's Saved/Scenario). */
	FString StateDir;
	FString Endpoint;
	TSharedPtr<IScenarioHttpTransport> Transport;
	TSharedPtr<IScenarioCredentialStore> Store;
	/** Tests inject a backend; sign-in is then the backend's business. */
	TSharedPtr<IScenarioBackend> Backend;
	double PollIntervalSeconds = 3.0;
	int32 MaxConcurrentDownloads = 3;
	bool bAutoDownload = true;
	/** Registers an FTSTicker for polling. Tests call Poll() themselves. */
	bool bUseTicker = true;
	/** With an injected backend: whether paid calls are refused. Unset: SCENARIO_NO_SPEND decides. */
	TOptional<bool> SpendingDisabled;
};

DECLARE_MULTICAST_DELEGATE(FOnScenarioSessionChanged);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnScenarioJobFilesReady, const FString& /*LocalId*/);

/**
 * Everything between the editor and Scenario: sign-in, workspace, prices,
 * paid runs under the money contract, job polling and downloads.
 *
 * Money rules (spec section 7.1), enforced here for every caller (Studio,
 * composer, agents, scripts):
 *  - a run is sent only with a fresh single-use quote for the exact request;
 *  - an agent must also pass the quote's exact cost string;
 *  - the intent is written to the job list before the request leaves;
 *  - an unresolved identical request blocks a new one;
 *  - a paid call is never retried; an ambiguous failure becomes Unknown.
 */
class SCENARIOCORE_API FScenarioSession : public TSharedFromThis<FScenarioSession>
{
public:
	explicit FScenarioSession(const FScenarioSessionConfig& InConfig);
	~FScenarioSession();

	/** Loads the job list, catalog cache and saved workspace, then restores the sign-in. */
	void Start();
	void Shutdown();

	// Sign-in

	EScenarioConnection GetConnection() const { return Connection; }
	EScenarioAuthMode GetAuthMode() const { return AuthMode; }
	bool IsSignedIn() const { return Connection == EScenarioConnection::Ready || Connection == EScenarioConnection::NeedsWorkspace; }
	bool IsReady() const { return Connection == EScenarioConnection::Ready; }
	/** The last sign-in or connection problem, for the sign-in screen. */
	const FScenarioError& GetLastError() const { return LastError; }
	/** Where credentials live ("macOS Keychain"...), for the Connection view. */
	FString DescribeCredentialStore() const;
	bool IsCredentialStoreSecure() const;
	/** environment, store or empty: where the API key came from. */
	const FString& GetApiKeySource() const { return ApiKeySource; }

	void SignInWithBrowser(TFunction<void(const FString& Url)> OnUrl, FScenarioReplyCallback OnDone);
	void SignInWithDeviceCode(TFunction<void(const FScenarioDeviceCode& Code)> OnCode, FScenarioReplyCallback OnDone);
	/** Checks the key with a free call, then saves it in the credential store. */
	void SignInWithApiKey(const FString& Key, const FString& Secret, FScenarioReplyCallback OnDone);
	void CancelSignIn();
	bool IsSigningIn() const;
	void SignOut();

	// Workspace

	const FScenarioWorkspace& GetWorkspace() const { return Workspace; }
	const TArray<FScenarioTeam>& GetTeams() const { return Teams; }
	void RefreshTeams(FScenarioReplyCallback OnDone = nullptr);
	void SetWorkspace(const FScenarioWorkspace& InWorkspace);
	/** Quote scope of the current workspace. */
	FString Scope() const;

	/** Creative Units used this month by the project, or -1 while unknown. Scenario has no balance read. */
	double GetUsageThisMonth() const { return UsageThisMonth; }
	void RefreshUsage();

	// Money

	/**
	 * Free price check. Reuses a fresh quote for the same request.
	 * bAutomatic marks checks made while a form is edited: they stop after an estimate alarm.
	 */
	void Price(const FScenarioRunRequest& Request, bool bAutomatic, TFunction<void(const TScenarioResult<FScenarioQuote>&)> OnDone);
	/** The fresh quote for this request, or null. */
	const FScenarioQuote* QuoteFor(const FScenarioRunRequest& Request) const;
	FString Fingerprint(const FScenarioRunRequest& Request) const;

	/**
	 * Paid. QuoteId and ApprovedExact are required for agents (empty for the
	 * person's click, which uses the fresh quote of the exact request).
	 * OnDone gets the local job id; for Prompt Spark the reply is in GetSparkResult.
	 */
	void Submit(const FScenarioRunRequest& Request, const FString& QuoteId, const FString& ApprovedExact, TFunction<void(const TScenarioResult<FString>&)> OnDone);

	/** Why a submit would be refused now, or empty (no network call). */
	FString CanSubmit(const FScenarioRunRequest& Request, const FString& QuoteId, const FString& ApprovedExact) const;
	bool IsSpendingDisabled() const;
	bool IsEstimateAlarmRaised() const;

	/** Prompt Spark replies by local job id (prompts, calls, recommended_call), kept for this session. */
	TSharedPtr<FJsonObject> GetSparkResult(const FString& LocalId) const;

	// Jobs

	FScenarioLedger& Ledger() { return *LedgerPtr; }
	const FScenarioLedger& Ledger() const { return *LedgerPtr; }

	/** One polling pass: job_get for every active job that is due. Called by the ticker. */
	void Poll(double Now);
	void CancelJob(const FString& LocalId, FScenarioReplyCallback OnDone = nullptr);
	/** Looks for unresolved requests in Scenario's recent jobs. OnDone gets the count linked. */
	void CheckUnresolved(TFunction<void(int32 Linked, const FScenarioError& Error)> OnDone = nullptr);
	void MarkNotSent(const FString& LocalId);
	void Dismiss(const FString& LocalId);
	/** Links a Scenario job that is not in the list (Results > cloud history). */
	void Recover(const FScenarioJobInfo& Job, const FString& Lane);

	/** Downloads every output file of a finished job (resumes .part files). */
	void DownloadFiles(const FString& LocalId);
	bool IsDownloading(const FString& LocalId) const;
	/** 0 to 1 over the job's files, or -1 when not downloading. */
	float DownloadProgress(const FString& LocalId) const;
	FString DownloadDir(const FString& LocalId) const;

	/** Uploads a local file as a Scenario asset, reusing an earlier upload of the same file in this session. */
	void Upload(const FString& Path, TFunction<void(const TScenarioResult<FString>&)> OnDone, TFunction<void(float)> OnProgress = nullptr);

	// Parts

	FScenarioApi& Api() { return *ApiPtr; }
	TSharedRef<FScenarioApi> ApiShared() const { return ApiPtr.ToSharedRef(); }
	FScenarioCatalog& Catalog() { return *CatalogPtr; }
	TSharedRef<FScenarioCatalog> CatalogShared() const { return CatalogPtr.ToSharedRef(); }
	FScenarioQuotes& Quotes() { return QuoteBook; }
	const FScenarioSessionConfig& GetConfig() const { return Config; }

	FOnScenarioSessionChanged OnChanged;
	/** Every output file of a job is on disk. */
	FOnScenarioJobFilesReady OnFilesReady;

	/** Overridable clock for tests. */
	TFunction<double()> Clock;

private:
	double Now() const;
	void SetConnection(EScenarioConnection InConnection, const FScenarioError& Error = FScenarioError());
	void UseApiKey(const FString& Key, const FString& Secret, const FString& Source);
	void UseOAuth();
	void Connected();
	void LoadWorkspace();
	void SaveWorkspace() const;
	void ApplyWorkspaceToClient();
	void AfterJobUpdate(const FString& LocalId);
	void PumpDownloads();
	void StartFileDownload(const FString& LocalId, int32 FileIndex, int32 UrlAttempt);
	void FinishFile(const FString& LocalId, int32 FileIndex, const FScenarioJobFile& File);
	bool Tick(float DeltaSeconds);

	FScenarioSessionConfig Config;
	TSharedPtr<IScenarioHttpTransport> Transport;
	TSharedPtr<IScenarioCredentialStore> Store;
	TSharedPtr<class FScenarioMcpClient> Client;
	TSharedPtr<IScenarioBackend> Backend;
	TSharedPtr<FScenarioApi> ApiPtr;
	TSharedPtr<FScenarioOAuth> OAuth;
	TSharedPtr<FScenarioLedger> LedgerPtr;
	TSharedPtr<FScenarioCatalog> CatalogPtr;
	TSharedPtr<FScenarioDownloader> Downloader;
	TSharedPtr<FScenarioUploader> Uploader;
	FScenarioQuotes QuoteBook;

	EScenarioConnection Connection = EScenarioConnection::SignedOut;
	EScenarioAuthMode AuthMode = EScenarioAuthMode::None;
	FString ApiKeySource;
	FScenarioError LastError;
	FScenarioWorkspace Workspace;
	TArray<FScenarioTeam> Teams;
	double UsageThisMonth = -1.0;
	uint32 AuthGeneration = 0;

	/** Jobs with a job_get in flight, and when each was last polled. */
	TSet<FString> PollsInFlight;
	TMap<FString, double> LastPolled;
	bool bPollPaused = false;

	struct FDownloadJob
	{
		TArray<int64> Done;
		TArray<int64> Total;
		int32 Remaining = 0;
		bool bFailed = false;
	};
	TMap<FString, FDownloadJob> Downloads;
	TArray<TPair<FString, int32>> DownloadQueue;
	int32 ActiveDownloads = 0;

	TMap<FString, TSharedPtr<FJsonObject>> SparkResults;
	/** "path|size|mtime" to asset id, so the same reference is uploaded once per session. */
	TMap<FString, FString> UploadCache;

	FTSTicker::FDelegateHandle TickerHandle;
	double LastUsageRefresh = 0.0;
};
