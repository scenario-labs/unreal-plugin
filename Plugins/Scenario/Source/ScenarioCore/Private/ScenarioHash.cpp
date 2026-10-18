// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioHash.h"

#include "Misc/Base64.h"
#include "Misc/Guid.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#pragma comment(lib, "Bcrypt.lib")
#elif PLATFORM_MAC || PLATFORM_LINUX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
	constexpr uint32 K[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
	};

	FORCEINLINE uint32 RotR(uint32 Value, uint32 Bits)
	{
		return (Value >> Bits) | (Value << (32 - Bits));
	}

	void Compress(uint32 State[8], const uint8 Block[64])
	{
		uint32 W[64];
		for (int32 Index = 0; Index < 16; ++Index)
		{
			W[Index] = (uint32(Block[Index * 4]) << 24) | (uint32(Block[Index * 4 + 1]) << 16) | (uint32(Block[Index * 4 + 2]) << 8) | uint32(Block[Index * 4 + 3]);
		}
		for (int32 Index = 16; Index < 64; ++Index)
		{
			const uint32 S0 = RotR(W[Index - 15], 7) ^ RotR(W[Index - 15], 18) ^ (W[Index - 15] >> 3);
			const uint32 S1 = RotR(W[Index - 2], 17) ^ RotR(W[Index - 2], 19) ^ (W[Index - 2] >> 10);
			W[Index] = W[Index - 16] + S0 + W[Index - 7] + S1;
		}
		uint32 A = State[0], B = State[1], C = State[2], D = State[3], E = State[4], F = State[5], G = State[6], H = State[7];
		for (int32 Index = 0; Index < 64; ++Index)
		{
			const uint32 S1 = RotR(E, 6) ^ RotR(E, 11) ^ RotR(E, 25);
			const uint32 Ch = (E & F) ^ (~E & G);
			const uint32 T1 = H + S1 + Ch + K[Index] + W[Index];
			const uint32 S0 = RotR(A, 2) ^ RotR(A, 13) ^ RotR(A, 22);
			const uint32 Maj = (A & B) ^ (A & C) ^ (B & C);
			const uint32 T2 = S0 + Maj;
			H = G; G = F; F = E; E = D + T1; D = C; C = B; B = A; A = T1 + T2;
		}
		State[0] += A; State[1] += B; State[2] += C; State[3] += D;
		State[4] += E; State[5] += F; State[6] += G; State[7] += H;
	}
}

TArray<uint8> ScenarioHash::Sha256(const uint8* Data, int64 Length)
{
	uint32 State[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	int64 Offset = 0;
	while (Length - Offset >= 64)
	{
		Compress(State, Data + Offset);
		Offset += 64;
	}
	uint8 Tail[128] = {};
	const int64 Remaining = Length - Offset;
	if (Remaining > 0)
	{
		FMemory::Memcpy(Tail, Data + Offset, Remaining);
	}
	Tail[Remaining] = 0x80;
	const int32 TailBlocks = Remaining + 9 > 64 ? 2 : 1;
	const uint64 BitLength = uint64(Length) * 8;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Tail[TailBlocks * 64 - 1 - Index] = uint8(BitLength >> (Index * 8));
	}
	for (int32 Block = 0; Block < TailBlocks; ++Block)
	{
		Compress(State, Tail + Block * 64);
	}
	TArray<uint8> Digest;
	Digest.SetNumUninitialized(32);
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Digest[Index * 4] = uint8(State[Index] >> 24);
		Digest[Index * 4 + 1] = uint8(State[Index] >> 16);
		Digest[Index * 4 + 2] = uint8(State[Index] >> 8);
		Digest[Index * 4 + 3] = uint8(State[Index]);
	}
	return Digest;
}

TArray<uint8> ScenarioHash::Sha256(const TArray<uint8>& Data)
{
	return Sha256(Data.GetData(), Data.Num());
}

FString ScenarioHash::Sha256Hex(const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text);
	return ToHex(Sha256(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}

FString ScenarioHash::ToHex(const TArray<uint8>& Bytes)
{
	FString Out;
	Out.Reserve(Bytes.Num() * 2);
	for (const uint8 Byte : Bytes)
	{
		Out += FString::Printf(TEXT("%02x"), Byte);
	}
	return Out;
}

FString ScenarioHash::Base64Url(const TArray<uint8>& Bytes)
{
	FString Encoded = FBase64::Encode(Bytes);
	Encoded.ReplaceInline(TEXT("+"), TEXT("-"));
	Encoded.ReplaceInline(TEXT("/"), TEXT("_"));
	while (Encoded.EndsWith(TEXT("=")))
	{
		Encoded.LeftChopInline(1);
	}
	return Encoded;
}

TArray<uint8> ScenarioHash::RandomBytes(int32 Count)
{
	TArray<uint8> Bytes;
	Bytes.SetNumZeroed(Count);
	bool bFilled = false;
#if PLATFORM_WINDOWS
	bFilled = BCryptGenRandom(nullptr, Bytes.GetData(), static_cast<ULONG>(Count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif PLATFORM_MAC || PLATFORM_LINUX
	const int Handle = open("/dev/urandom", O_RDONLY);
	if (Handle >= 0)
	{
		int32 Read = 0;
		while (Read < Count)
		{
			const ssize_t Got = read(Handle, Bytes.GetData() + Read, Count - Read);
			if (Got <= 0)
			{
				break;
			}
			Read += static_cast<int32>(Got);
		}
		close(Handle);
		bFilled = Read == Count;
	}
#endif
	if (!bFilled)
	{
		// Fallback: random GUIDs from the platform (version 4 UUIDs on every desktop OS).
		int32 Offset = 0;
		while (Offset < Count)
		{
			const FGuid Guid = FGuid::NewGuid();
			const uint32 Parts[4] = { Guid.A, Guid.B, Guid.C, Guid.D };
			for (int32 Part = 0; Part < 4 && Offset < Count; ++Part)
			{
				for (int32 Shift = 0; Shift < 32 && Offset < Count; Shift += 8)
				{
					Bytes[Offset++] = uint8(Parts[Part] >> Shift);
				}
			}
		}
	}
	return Bytes;
}

FString ScenarioHash::RandomToken(int32 ByteCount)
{
	return Base64Url(RandomBytes(ByteCount));
}
