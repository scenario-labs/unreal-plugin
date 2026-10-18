// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioApi.h"
#include "ScenarioHttp.h"

struct SCENARIOCORE_API FScenarioDownloadResult
{
	bool bOk = false;
	FString Path;
	int64 Size = 0;
	FString Sha256;
	FScenarioError Error;
	/** The signed URL expired or was refused: ask Scenario for a fresh one and resume. */
	bool bUrlExpired = false;
	/** Bytes kept in the .part file for a resume. */
	int64 PartialBytes = 0;
	int64 TotalBytes = 0;
};

using FScenarioDownloadCallback = TFunction<void(const FScenarioDownloadResult&)>;
using FScenarioTransferProgress = TFunction<void(int64 Done, int64 Total)>;

/**
 * Downloads Scenario files into a .part file in ranged chunks, then renames it.
 * Only https URLs on Scenario's CDN hosts are fetched, with no credentials.
 * A stopped download resumes from the bytes already on disk.
 */
class SCENARIOCORE_API FScenarioDownloader : public TSharedFromThis<FScenarioDownloader>
{
public:
	static constexpr int64 ChunkBytes = 8 * 1024 * 1024;

	explicit FScenarioDownloader(TSharedRef<IScenarioHttpTransport> InTransport);

	/** Hosts files may come from. */
	static bool IsAllowedUrl(const FString& Url);
	/** Tests serve fixtures from 127.0.0.1 over http. Never set in the editor. */
	static bool bAllowLoopbackForTests;

	void Download(const FString& Url, const FString& DestinationPath, FScenarioTransferProgress OnProgress, FScenarioDownloadCallback OnDone);

	/** Stops every download at the next chunk boundary; the .part files stay for a resume. */
	void CancelAll() { ++Generation; }

private:
	struct FState;
	void NextChunk(TSharedRef<FState> State);
	void Finish(TSharedRef<FState> State, FScenarioDownloadResult Result);

	TSharedRef<IScenarioHttpTransport> Transport;
	uint32 Generation = 0;
};

struct SCENARIOCORE_API FScenarioUploadResult
{
	bool bOk = false;
	FString AssetId;
	FScenarioError Error;
};

using FScenarioUploadCallback = TFunction<void(const FScenarioUploadResult&)>;

/**
 * Uploads a local file through Scenario's multipart flow: upload_asset, a PUT
 * per part to the presigned storage URL, then upload_asset_complete. Parts are
 * read from disk on a worker thread. A part PUT is retried (it overwrites the
 * same part); the start and complete calls are not.
 */
class SCENARIOCORE_API FScenarioUploader : public TSharedFromThis<FScenarioUploader>
{
public:
	FScenarioUploader(TSharedRef<IScenarioHttpTransport> InTransport, TSharedRef<FScenarioApi> InApi);

	static bool IsAllowedPartUrl(const FString& Url);
	static bool bAllowLoopbackForTests;

	/** Kind is image, video, audio or 3d. */
	void Upload(const FString& FilePath, const FString& Kind, const FString& ContentType, FScenarioTransferProgress OnProgress, FScenarioUploadCallback OnDone);

private:
	struct FState;
	void PutPart(TSharedRef<FState> State, int32 PartIndex, int32 Attempt);
	void Complete(TSharedRef<FState> State, int32 Attempt);

	TSharedRef<IScenarioHttpTransport> Transport;
	TSharedRef<FScenarioApi> Api;
};
