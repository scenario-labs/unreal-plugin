// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioMcpClient.h"
#include "ScenarioTypes.h"

class IScenarioCredentialStore;
class IScenarioHttpTransport;
class FScenarioLocalHttpServer;

struct SCENARIOCORE_API FScenarioOAuthMetadata
{
	FString Issuer;
	FString AuthorizationEndpoint;
	FString TokenEndpoint;
	FString RegistrationEndpoint;
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

DECLARE_MULTICAST_DELEGATE(FOnScenarioOAuthChanged);

/**
 * Sign in with Scenario: OAuth 2.1 through the hosted Scenario MCP's
 * authorization server. Authorization code with PKCE and a loopback redirect
 * by default, the device code grant as the fallback, refresh tokens in the
 * credential store, the access token in memory only.
 */
class SCENARIOCORE_API FScenarioOAuth : public IScenarioAuthProvider, public TSharedFromThis<FScenarioOAuth>
{
public:
	static const TCHAR* DefaultMetadataUrl;
	static const TCHAR* DefaultResource;
	/** Fixed loopback ports registered as redirect URIs; the first free one is used. */
	static const TArray<int32>& RedirectPorts();
	static constexpr double BrowserTimeoutSeconds = 300.0;

	FScenarioOAuth(TSharedRef<IScenarioHttpTransport> InTransport, TSharedRef<IScenarioCredentialStore> InStore);

	// IScenarioAuthProvider
	virtual bool HasCredentials() const override;
	virtual FString AuthorizationHeader() const override;
	virtual bool NeedsWorkspaceArguments() const override { return true; }
	virtual void Refresh(TFunction<void(bool bRefreshed)> OnDone) override;

	/** Loads the refresh token saved by an earlier session. True when one exists. */
	bool Restore();
	bool HasRefreshToken() const { return !RefreshToken.IsEmpty(); }
	bool IsAccessTokenFresh() const;

	/**
	 * Starts the browser sign-in. OnUrl receives the authorization URL to open;
	 * OnDone fires once with OK or an error (Cancelled when Cancel() was called).
	 */
	void BeginBrowserSignIn(TFunction<void(const FString& Url)> OnUrl, FScenarioReplyCallback OnDone);
	/** Starts the device code sign-in. OnCode receives the code to show. */
	void BeginDeviceSignIn(TFunction<void(const FScenarioDeviceCode& Code)> OnCode, FScenarioReplyCallback OnDone);
	void Cancel();
	bool IsSigningIn() const { return bSigningIn; }

	/** Revokes the refresh token (best effort), removes it from the store and forgets the access token. */
	void SignOut();

	/** Uses an access token from the environment (SCENARIO_ACCESS_TOKEN) for CI. True when one is set. */
	bool UseEnvironmentToken();

	FOnScenarioOAuthChanged OnChanged;

	// Exposed for tests.
	static FString MakeCodeChallenge(const FString& Verifier);
	static FString BuildAuthorizeUrl(const FScenarioOAuthMetadata& Metadata, const FString& ClientId, const FString& RedirectUri,
		const FString& Challenge, const FString& State, const FString& Resource, const FString& Scope);
	/** Reads ?code=&state=&iss= or ?error= from a callback query. Returns an error message or empty, and the code. */
	static FString ReadCallback(const FString& Query, const FString& ExpectedState, const FString& ExpectedIssuer, FString& OutCode);

	FString MetadataUrl;
	FString Resource;
	/** Empty by default: the authorization server's default scopes are used. */
	FString Scope;

private:
	void Discover(TFunction<void(const FScenarioReply&)> OnDone);
	void EnsureClient(TFunction<void(const FScenarioReply&, const FString& ClientId)> OnDone);
	void PostForm(const FString& Url, const TArray<TPair<FString, FString>>& Fields, FScenarioReplyCallback OnDone);
	void ExchangeCode(const FString& Code, const FString& RedirectUri, FScenarioReplyCallback OnDone);
	bool ApplyTokenResponse(const TSharedPtr<FJsonObject>& Token);
	void PollDevice(const FString& DeviceCode, double Interval, double ExpiresAt, uint32 Attempt);
	void FinishSignIn(const FScenarioReply& Result);
	void StopCallbackServer();

	TSharedRef<IScenarioHttpTransport> Transport;
	TSharedRef<IScenarioCredentialStore> Store;
	FScenarioOAuthMetadata Metadata;
	FString ClientId;
	FString AccessToken;
	double AccessExpiresAt = 0.0;
	FString RefreshToken;

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
