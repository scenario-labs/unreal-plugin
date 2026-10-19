// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

// The OS credential stores. Each platform block is small on purpose: this is
// the only platform-specific code in the core.

#include "ScenarioCredentialStore.h"

#include "ScenarioCoreModule.h"
#include "Misc/App.h"

#if PLATFORM_MAC

#ifndef __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#endif
THIRD_PARTY_INCLUDES_START
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
THIRD_PARTY_INCLUDES_END

namespace
{
	CFStringRef ToCF(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(Utf8.Get()), Utf8.Length(), kCFStringEncodingUTF8, false);
	}

	CFMutableDictionaryRef BaseQuery(const FString& Account)
	{
		CFMutableDictionaryRef Query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFStringRef Service = ToCF(ScenarioCredentials::ServiceName());
		CFStringRef AccountRef = ToCF(Account);
		CFDictionarySetValue(Query, kSecClass, kSecClassGenericPassword);
		CFDictionarySetValue(Query, kSecAttrService, Service);
		CFDictionarySetValue(Query, kSecAttrAccount, AccountRef);
		CFRelease(Service);
		CFRelease(AccountRef);
		return Query;
	}

	class FMacKeychainStore : public IScenarioCredentialStore
	{
	public:
		virtual bool Write(const FString& Account, const FString& Secret) override
		{
			const FTCHARToUTF8 Utf8(*Secret);
			CFDataRef Data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(Utf8.Get()), Utf8.Length());
			CFMutableDictionaryRef Query = BaseQuery(Account);
			CFMutableDictionaryRef Update = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			CFDictionarySetValue(Update, kSecValueData, Data);
			OSStatus Status = SecItemUpdate(Query, Update);
			if (Status == errSecItemNotFound)
			{
				CFDictionarySetValue(Query, kSecValueData, Data);
				CFStringRef Label = ToCF(FString(ScenarioCredentials::ServiceName()) + TEXT(" (") + Account + TEXT(")"));
				CFDictionarySetValue(Query, kSecAttrLabel, Label);
				CFRelease(Label);
				Status = SecItemAdd(Query, nullptr);
			}
			CFRelease(Update);
			CFRelease(Query);
			CFRelease(Data);
			if (Status != errSecSuccess)
			{
				UE_LOG(LogScenario, Warning, TEXT("Keychain write failed for %s (status %d)."), *Account, static_cast<int32>(Status));
			}
			return Status == errSecSuccess;
		}

		virtual bool Read(const FString& Account, FString& OutSecret) const override
		{
			CFMutableDictionaryRef Query = BaseQuery(Account);
			CFDictionarySetValue(Query, kSecReturnData, kCFBooleanTrue);
			CFDictionarySetValue(Query, kSecMatchLimit, kSecMatchLimitOne);
			CFTypeRef Result = nullptr;
			const OSStatus Status = SecItemCopyMatching(Query, &Result);
			CFRelease(Query);
			if (Status != errSecSuccess || Result == nullptr)
			{
				if (Status == errSecInteractionNotAllowed)
				{
					UE_LOG(LogScenario, Display, TEXT("The saved %s needs keychain approval, which an unattended run cannot ask for. Set SCENARIO_API_KEY and SCENARIO_API_SECRET instead."), *Account);
				}
				else if (Status != errSecItemNotFound && Status != errSecSuccess)
				{
					UE_LOG(LogScenario, Warning, TEXT("Keychain read failed for %s (status %d)."), *Account, static_cast<int32>(Status));
				}
				return false;
			}
			const CFDataRef Data = static_cast<CFDataRef>(Result);
			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(CFDataGetBytePtr(Data)), static_cast<int32>(CFDataGetLength(Data)));
			OutSecret = FString(Converted.Length(), Converted.Get());
			CFRelease(Result);
			return true;
		}

		virtual bool Delete(const FString& Account) override
		{
			CFMutableDictionaryRef Query = BaseQuery(Account);
			const OSStatus Status = SecItemDelete(Query);
			CFRelease(Query);
			return Status == errSecSuccess || Status == errSecItemNotFound;
		}

		virtual FString Describe() const override { return TEXT("macOS Keychain"); }
		virtual bool IsOsStore() const override { return true; }
	};
}

TSharedPtr<IScenarioCredentialStore> ScenarioCreateOsCredentialStore()
{
	if (FApp::IsUnattended())
	{
		// A keychain item trusts the app that wrote it, so UnrealEditor-Cmd reading what the editor saved asks for
		// the login password. Nobody answers that dialog in an unattended run: make such reads fail instead.
		// The setting is per process, and no prompt is wanted anywhere in an unattended process.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		SecKeychainSetUserInteractionAllowed(false);
#pragma clang diagnostic pop
	}
	return MakeShared<FMacKeychainStore>();
}

#elif PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincred.h>
#include "Windows/HideWindowsPlatformTypes.h"

namespace
{
	FString TargetName(const FString& Account)
	{
		return FString(ScenarioCredentials::ServiceName()) + TEXT("/") + Account;
	}

	class FWindowsCredentialStore : public IScenarioCredentialStore
	{
	public:
		virtual bool Write(const FString& Account, const FString& Secret) override
		{
			const FTCHARToUTF8 Utf8(*Secret);
			FString Target = TargetName(Account);
			CREDENTIALW Credential = {};
			Credential.Type = CRED_TYPE_GENERIC;
			Credential.TargetName = const_cast<LPWSTR>(*Target);
			Credential.CredentialBlobSize = static_cast<DWORD>(Utf8.Length());
			Credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<ANSICHAR*>(Utf8.Get()));
			Credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
			FString User = Account;
			Credential.UserName = const_cast<LPWSTR>(*User);
			return CredWriteW(&Credential, 0) != FALSE;
		}

		virtual bool Read(const FString& Account, FString& OutSecret) const override
		{
			PCREDENTIALW Credential = nullptr;
			const FString Target = TargetName(Account);
			if (!CredReadW(*Target, CRED_TYPE_GENERIC, 0, &Credential) || Credential == nullptr)
			{
				return false;
			}
			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Credential->CredentialBlob), static_cast<int32>(Credential->CredentialBlobSize));
			OutSecret = FString(Converted.Length(), Converted.Get());
			CredFree(Credential);
			return true;
		}

		virtual bool Delete(const FString& Account) override
		{
			const FString Target = TargetName(Account);
			return CredDeleteW(*Target, CRED_TYPE_GENERIC, 0) != FALSE || GetLastError() == ERROR_NOT_FOUND;
		}

		virtual FString Describe() const override { return TEXT("Windows Credential Manager"); }
		virtual bool IsOsStore() const override { return true; }
	};
}

TSharedPtr<IScenarioCredentialStore> ScenarioCreateOsCredentialStore()
{
	return MakeShared<FWindowsCredentialStore>();
}

#else

// Linux: libsecret is not linked in v0.1; the 0600 file store is used and the Connection view says so.
TSharedPtr<IScenarioCredentialStore> ScenarioCreateOsCredentialStore()
{
	return nullptr;
}

#endif
