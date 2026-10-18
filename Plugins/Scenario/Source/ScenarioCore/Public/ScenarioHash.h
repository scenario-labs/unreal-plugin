// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

/** SHA-256 (FIPS 180-4) and random helpers. The engine ships SHA-1 only, and PKCE needs SHA-256. */
namespace ScenarioHash
{
	SCENARIOCORE_API TArray<uint8> Sha256(const uint8* Data, int64 Length);
	SCENARIOCORE_API TArray<uint8> Sha256(const TArray<uint8>& Data);
	/** SHA-256 of the UTF-8 bytes, as lowercase hex. */
	SCENARIOCORE_API FString Sha256Hex(const FString& Text);
	SCENARIOCORE_API FString ToHex(const TArray<uint8>& Bytes);

	/** Base64 with the URL-safe alphabet and no padding (RFC 4648 section 5). */
	SCENARIOCORE_API FString Base64Url(const TArray<uint8>& Bytes);

	/** Random bytes from the operating system's generator. */
	SCENARIOCORE_API TArray<uint8> RandomBytes(int32 Count);

	/** URL-safe random token with the given number of random bytes. */
	SCENARIOCORE_API FString RandomToken(int32 ByteCount = 32);
}
