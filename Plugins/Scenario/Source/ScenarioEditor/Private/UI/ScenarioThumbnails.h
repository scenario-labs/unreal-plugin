// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FAssetThumbnailPool;
struct FSlateBrush;
struct FSlateDynamicImageBrush;

/**
 * Small preview brushes for job outputs and library assets. Files are read
 * and decoded on a worker thread, then turned into a brush on the game
 * thread; nothing blocks the editor. Remote images are fetched once per
 * session. URLs are never logged (they are signed).
 */
class FScenarioThumbnails
{
public:
	static FScenarioThumbnails& Get();
	void Shutdown();

	/** The brush for a local image file, or null while it loads (or when it cannot be read). */
	const FSlateBrush* ForFile(const FString& Path);
	/** The brush for a remote image, keyed by asset id, or null while it loads. */
	const FSlateBrush* ForUrl(const FString& Key, const FString& Url);

	/** The pool behind real asset thumbnails (meshes, materials) of imported results. */
	TSharedPtr<FAssetThumbnailPool> AssetPool();

	/** Fires on the game thread when a brush becomes available. */
	FSimpleMulticastDelegate OnLoaded;

	static constexpr int32 MaxSize = 256;

private:
	void Decode(const FString& Key, TArray<uint8> Bytes);
	void Finish(const FString& Key, int32 Width, int32 Height, TArray<uint8> Bgra);

	TMap<FString, TSharedPtr<FSlateDynamicImageBrush>> Brushes;
	TSharedPtr<FAssetThumbnailPool> Pool;
	TSet<FString> Pending;
	TSet<FString> Failed;
	int32 InFlight = 0;
	TArray<TPair<FString, FString>> Queue;
	void Pump();
};
