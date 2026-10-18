// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioCredentialStore.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScenarioCoreModule.h"
#include "ScenarioJson.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

// Platform stores, defined in Private/Platform.
TSharedPtr<IScenarioCredentialStore> ScenarioCreateOsCredentialStore();

const TCHAR* ScenarioCredentials::ServiceName()
{
	return TEXT("Scenario for Unreal");
}

FString ScenarioCredentials::UserConfigDir()
{
#if PLATFORM_WINDOWS
	const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
	return FPaths::Combine(AppData.IsEmpty() ? FPlatformProcess::UserSettingsDir() : AppData, TEXT("Scenario"), TEXT("Unreal"));
#else
	FString Base = FPlatformMisc::GetEnvironmentVariable(TEXT("XDG_CONFIG_HOME"));
	if (Base.IsEmpty())
	{
		Base = FPaths::Combine(FPlatformMisc::GetEnvironmentVariable(TEXT("HOME")), TEXT(".config"));
	}
	return FPaths::Combine(Base, TEXT("scenario"), TEXT("unreal"));
#endif
}

TSharedRef<IScenarioCredentialStore> ScenarioCredentials::CreatePlatformStore()
{
	if (FPlatformMisc::GetEnvironmentVariable(TEXT("SCENARIO_CREDENTIAL_STORE")) != TEXT("file"))
	{
		if (TSharedPtr<IScenarioCredentialStore> Os = ScenarioCreateOsCredentialStore())
		{
			return Os.ToSharedRef();
		}
	}
	return MakeShared<FScenarioFileCredentialStore>(FPaths::Combine(UserConfigDir(), TEXT("credentials.json")));
}

FScenarioFileCredentialStore::FScenarioFileCredentialStore(const FString& InPath)
	: Path(InPath)
{
}

TMap<FString, FString> FScenarioFileCredentialStore::Load() const
{
	TMap<FString, FString> Values;
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return Values;
	}
	const TSharedPtr<FJsonObject> Root = ScenarioJson::ParseObject(Text);
	if (!Root.IsValid())
	{
		return Values;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
	{
		if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String)
		{
			Values.Add(Pair.Key, Pair.Value->AsString());
		}
	}
	return Values;
}

bool FScenarioFileCredentialStore::Store(const TMap<FString, FString>& Values) const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Values)
	{
		Root->SetStringField(Pair.Key, Pair.Value);
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	const FString Temp = Path + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(ScenarioJson::Write(Root, true), *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return false;
	}
#if PLATFORM_MAC || PLATFORM_LINUX
	chmod(TCHAR_TO_UTF8(*Temp), S_IRUSR | S_IWUSR);
	chmod(TCHAR_TO_UTF8(*FPaths::GetPath(Path)), S_IRUSR | S_IWUSR | S_IXUSR);
#endif
	return IFileManager::Get().Move(*Path, *Temp, true);
}

bool FScenarioFileCredentialStore::Write(const FString& Account, const FString& Secret)
{
	TMap<FString, FString> Values = Load();
	Values.Add(Account, Secret);
	return Store(Values);
}

bool FScenarioFileCredentialStore::Read(const FString& Account, FString& OutSecret) const
{
	const TMap<FString, FString> Values = Load();
	if (const FString* Found = Values.Find(Account))
	{
		OutSecret = *Found;
		return true;
	}
	return false;
}

bool FScenarioFileCredentialStore::Delete(const FString& Account)
{
	TMap<FString, FString> Values = Load();
	if (Values.Remove(Account) == 0)
	{
		return true;
	}
	return Store(Values);
}

bool FScenarioMemoryCredentialStore::Read(const FString& Account, FString& OutSecret) const
{
	if (const FString* Found = Values.Find(Account))
	{
		OutSecret = *Found;
		return true;
	}
	return false;
}

FScenarioApiKeyCredentials FScenarioApiKeyCredentials::Load(const IScenarioCredentialStore& Store)
{
	FScenarioApiKeyCredentials Out;
	const FString EnvKey = FPlatformMisc::GetEnvironmentVariable(TEXT("SCENARIO_API_KEY")).TrimStartAndEnd();
	const FString EnvSecret = FPlatformMisc::GetEnvironmentVariable(TEXT("SCENARIO_API_SECRET")).TrimStartAndEnd();
	if (!EnvKey.IsEmpty() && !EnvSecret.IsEmpty())
	{
		Out.Key = EnvKey;
		Out.Secret = EnvSecret;
		Out.Source = TEXT("environment");
		return Out;
	}
	FString Key;
	FString Secret;
	if (Store.Read(TEXT("api-key"), Key) && Store.Read(TEXT("api-secret"), Secret) && !Key.IsEmpty() && !Secret.IsEmpty())
	{
		Out.Key = Key;
		Out.Secret = Secret;
		Out.Source = TEXT("store");
	}
	return Out;
}

FString FScenarioApiKeyCredentials::Save(IScenarioCredentialStore& Store, const FString& InKey, const FString& InSecret)
{
	FString Key = InKey.TrimStartAndEnd();
	FString Secret = InSecret.TrimStartAndEnd();
	// One "key:secret" pair pasted in the first field.
	if (Secret.IsEmpty() && Key.Contains(TEXT(":")))
	{
		// Split from a copy: writing the left part into Key while splitting Key would corrupt the right part.
		const FString Pair = Key;
		Pair.Split(TEXT(":"), &Key, &Secret);
		Key.TrimStartAndEndInline();
		Secret.TrimStartAndEndInline();
	}
	if (Key.IsEmpty() || Secret.IsEmpty())
	{
		return TEXT("Enter both the API key and the secret.");
	}
	if (Key.Contains(TEXT(" ")) || Secret.Contains(TEXT(" ")))
	{
		return TEXT("The key and secret cannot contain spaces.");
	}
	if (!Store.Write(TEXT("api-secret"), Secret) || !Store.Write(TEXT("api-key"), Key))
	{
		return FString::Printf(TEXT("Could not save the key in %s."), *Store.Describe());
	}
	return FString();
}

void FScenarioApiKeyCredentials::Clear(IScenarioCredentialStore& Store)
{
	Store.Delete(TEXT("api-secret"));
	Store.Delete(TEXT("api-key"));
}
