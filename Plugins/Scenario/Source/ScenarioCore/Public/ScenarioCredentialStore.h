// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

/**
 * Where secrets live: the OS credential store, never the project.
 * macOS Keychain, Windows Credential Manager, otherwise a 0600 file in the
 * user's config folder. Accounts used by the plugin:
 *   api-key            the API key id (not secret, kept beside its secret)
 *   api-secret         the API secret
 *   oauth-refresh      the OAuth refresh token
 *   oauth-client       the registered OAuth client (JSON, not secret)
 *   agent-token        the bearer token of the local MCP server
 */
class SCENARIOCORE_API IScenarioCredentialStore
{
public:
	virtual ~IScenarioCredentialStore() = default;

	virtual bool Write(const FString& Account, const FString& Secret) = 0;
	virtual bool Read(const FString& Account, FString& OutSecret) const = 0;
	virtual bool Delete(const FString& Account) = 0;
	/** "macOS Keychain", "Windows Credential Manager" or the file path. */
	virtual FString Describe() const = 0;
	/** False for the file fallback, so the Connection view can say so. */
	virtual bool IsOsStore() const = 0;
};

namespace ScenarioCredentials
{
	/** Service name in the Keychain and target prefix in Credential Manager. */
	SCENARIOCORE_API const TCHAR* ServiceName();

	/** The OS store when available, else the file store. SCENARIO_CREDENTIAL_STORE=file forces the file. */
	SCENARIOCORE_API TSharedRef<IScenarioCredentialStore> CreatePlatformStore();

	/** ~/.config/scenario/unreal (or %APPDATA%\Scenario\Unreal on Windows). */
	SCENARIOCORE_API FString UserConfigDir();
}

/** JSON file with owner-only permissions. The fallback, and the store used by tests. */
class SCENARIOCORE_API FScenarioFileCredentialStore : public IScenarioCredentialStore
{
public:
	explicit FScenarioFileCredentialStore(const FString& InPath);

	virtual bool Write(const FString& Account, const FString& Secret) override;
	virtual bool Read(const FString& Account, FString& OutSecret) const override;
	virtual bool Delete(const FString& Account) override;
	virtual FString Describe() const override { return Path; }
	virtual bool IsOsStore() const override { return false; }

private:
	TMap<FString, FString> Load() const;
	bool Store(const TMap<FString, FString>& Values) const;
	FString Path;
};

/** In-memory store for tests and for sessions that must not touch the OS store. */
class SCENARIOCORE_API FScenarioMemoryCredentialStore : public IScenarioCredentialStore
{
public:
	virtual bool Write(const FString& Account, const FString& Secret) override { Values.Add(Account, Secret); return true; }
	virtual bool Read(const FString& Account, FString& OutSecret) const override;
	virtual bool Delete(const FString& Account) override { Values.Remove(Account); return true; }
	virtual FString Describe() const override { return TEXT("memory"); }
	virtual bool IsOsStore() const override { return false; }

private:
	TMap<FString, FString> Values;
};

/** API key credentials found for this session, and where they came from. */
struct SCENARIOCORE_API FScenarioApiKeyCredentials
{
	FString Key;
	FString Secret;
	/** environment, store, or empty when none. */
	FString Source;

	bool IsValid() const { return !Key.IsEmpty() && !Secret.IsEmpty(); }

	/** Environment first (SCENARIO_API_KEY, SCENARIO_API_SECRET), then the store. */
	static FScenarioApiKeyCredentials Load(const IScenarioCredentialStore& Store);
	/** Accepts the two values, or a single "key:secret" pair in Key. Returns an error message or empty. */
	static FString Save(IScenarioCredentialStore& Store, const FString& Key, const FString& Secret);
	static void Clear(IScenarioCredentialStore& Store);
};
