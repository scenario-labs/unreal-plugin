// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioRestClient.h"
#include "ScenarioTypes.h"

class IScenarioCredentialStore;
class IScenarioHttpTransport;
class FScenarioLocalHttpServer;

struct SCENARIOCORE_API FScenarioOAuthMetadata
{
	FString Issuer;
	FString AuthorizationEndpoint;
	FString TokenEndpoint;
	FString DeviceAuthorizationEndpoint;
	FString RevocationEndpoint;

	bool IsValid() const { return !AuthorizationEndpoint.IsEmpty() && !TokenEndpoint.IsEmpty(); }
	static bool FromJson(const TSharedPtr<FJsonObject>& Object, FScenarioOAuthMetadata& Out);
};

/** What the device code sign-in shows to the person. */
struct SCENARIOCORE_API FScenarioDeviceCode
{
	FString UserCode;
	FString VerificationUri;
	FString VerificationUriComplete;
	double ExpiresAt = 0.0;
};

/** The identity provider and the registered OAuth application Unreal signs in as. */
struct SCENARIOCORE_API FScenarioOAuthApp
{
	FString Issuer;
	FString ClientId;
	/** Why the SCENARIO_OAUTH_* variables were refused; sign-in fails with it. Empty when usable. */
	FString Problem;

	FString MetadataUrl() const { return Issuer + TEXT("/.well-known/oauth-authorization-server"); }
	/** Production, or the development instance named by both variables together (HTTPS issuer). */
	static FScenarioOAuthApp Select(const FString& EnvIssuer, const FString& EnvClientId);
	/** Select() with SCENARIO_OAUTH_ISSUER and SCENARIO_OAUTH_CLIENT_ID. */
	static FScenarioOAuthApp FromEnvironment();
};

DECLARE_MULTICAST_DELEGATE(FOnScenarioOAuthChanged);

/**
 * Sign in with Scenario as "Scenario for Unreal Engine", the plugin's
 * registered public OAuth application on Scenario's identity provider.
 * Authorization code with PKCE and a loopback redirect by default, the device
 * code grant as the fallback, refresh tokens in the credential store, the
 * access token in memory only.
 */
class SCENARIOCORE_API FScenarioOAuth : public IScenarioAuthProvider, public TSharedFromThis<FScenarioOAuth>
{
public:
	static const TCHAR* ProductionIssuer;
	/** The production application's client id: a public client, so not a secret. */
	static const TCHAR* ProductionClientId;
	/** Who the person is (their id and email, shown in the editor), and a refresh token. */
	static const TCHAR* DefaultScope;
	/** Loopback ports tried in order; the provider accepts a loopback redirect on any port. */
	static const TArray<int32>& RedirectPorts();
	static constexpr double BrowserTimeoutSeconds = 300.0;

	FScenarioOAuth(TSharedRef<IScenarioHttpTransport> InTransport, TSharedRef<IScenarioCredentialStore> InStore);

	// IScenarioAuthProvider
	virtual bool HasCredentials() const override;
	virtual FString AuthorizationHeader() const override;
	virtual bool NeedsWorkspaceArguments() const override { return true; }
	virtual void Refresh(TFunction<void(bool bRefreshed)> OnDone) override;

	/**
	 * Loads the refresh token saved by an earlier session. True when one exists.
	 * A token saved for another application (an older version's own client, or
	 * another provider) is deleted without being sent anywhere.
	 */
	bool Restore();
	bool HasRefreshToken() const { return !RefreshToken.IsEmpty(); }
	bool IsAccessTokenFresh() const;
	/** The signed-in account's email, from the ID token; empty when unknown. Shown in the editor, never logged. */
	const FString& GetAccountEmail() const { return AccountEmail; }

	/**
	 * Starts the browser sign-in. OnUrl receives the authorization URL to open;
	 * OnDone fires once with OK or an error (Cancelled when Cancel() was called).
	 */
	void BeginBrowserSignIn(TFunction<void(const FString& Url)> OnUrl, FScenarioReplyCallback OnDone);
	/** Starts the device code sign-in. OnCode receives the code to show. */
	void BeginDeviceSignIn(TFunction<void(const FScenarioDeviceCode& Code)> OnCode, FScenarioReplyCallback OnDone);
	void Cancel();
	bool IsSigningIn() const { return bSigningIn; }

	/**
	 * Revokes the refresh token (best effort), removes it from the store and forgets the access token.
	 * Editors on this machine share the saved session, so the others end theirs at their next refresh.
	 */
	void SignOut();

	/** Uses an access token from the environment (SCENARIO_ACCESS_TOKEN) for CI. True when one is set. */
	bool UseEnvironmentToken();

	FOnScenarioOAuthChanged OnChanged;

	// Exposed for tests.
	static FString MakeCodeChallenge(const FString& Verifier);
	static FString BuildAuthorizeUrl(const FScenarioOAuthMetadata& Metadata, const FString& ClientId, const FString& RedirectUri,
		const FString& Challenge, const FString& State, const FString& Scope);
	/** Reads ?code=&state=&iss= or ?error= from a callback query. Returns an error message or empty, and the code. */
	static FString ReadCallback(const FString& Query, const FString& ExpectedState, const FString& ExpectedIssuer, FString& OutCode);
	/** The claims of a JWT, without checking its signature; null when unreadable. */
	static TSharedPtr<FJsonObject> ReadTokenClaims(const FString& Jwt);

	FScenarioOAuthApp App;
	FString Scope;

private:
	void Discover(TFunction<void(const FScenarioReply&)> OnDone);
	/**
	 * Takes the refresh token another editor on this machine saved since this one last read or wrote it, when it
	 * belongs to the same application, provider and account. True when taken.
	 */
	bool AdoptStoredSession();
	/** Forgets the session and deletes the saved one; with bKeepNewer, not when another editor has replaced it since. */
	void ForgetSession(bool bKeepNewer = false);
	void SendRefresh(bool bRetried);
	void FinishRefresh(bool bOk);
	void PostForm(const FString& Url, const TArray<TPair<FString, FString>>& Fields, FScenarioReplyCallback OnDone);
	void ExchangeCode(const FString& Code, const FString& RedirectUri, FScenarioReplyCallback OnDone);
	bool ApplyTokenResponse(const TSharedPtr<FJsonObject>& Token);
	void PollDevice(const FString& DeviceCode, double Interval, double ExpiresAt, uint32 Attempt);
	void FinishSignIn(const FScenarioReply& Result);
	void StopCallbackServer();

	TSharedRef<IScenarioHttpTransport> Transport;
	TSharedRef<IScenarioCredentialStore> Store;
	FScenarioOAuthMetadata Metadata;
	FString AccessToken;
	double AccessExpiresAt = 0.0;
	FString RefreshToken;
	/** The refresh token last read from or written to the store, to tell another editor's changes from this one's. */
	FString StoredRefreshToken;
	/** The account's id (sub) and email, from the ID token. */
	FString AccountId;
	FString AccountEmail;

	bool bSigningIn = false;
	uint32 SignInGeneration = 0;
	FScenarioReplyCallback PendingDone;
	FString PendingState;
	FString PendingVerifier;
	FString PendingRedirectUri;
	TSharedPtr<FScenarioLocalHttpServer, ESPMode::ThreadSafe> CallbackServer;

	bool bRefreshing = false;
	TArray<TFunction<void(bool)>> RefreshWaiters;
};
