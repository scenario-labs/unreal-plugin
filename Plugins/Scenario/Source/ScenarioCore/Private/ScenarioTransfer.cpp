// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioTransfer.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "ScenarioCoreModule.h"
#include "ScenarioErrors.h"
#include "ScenarioHash.h"

bool FScenarioDownloader::bAllowLoopbackForTests = false;
bool FScenarioUploader::bAllowLoopbackForTests = false;

namespace
{
	int64 FileSize(const FString& Path)
	{
		const int64 Size = IFileManager::Get().FileSize(*Path);
		return Size < 0 ? 0 : Size;
	}

	/** Appends bytes to a file. Runs on a worker thread. */
	bool AppendBytes(const FString& Path, const TArray<uint8>& Bytes, bool bTruncate)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*Path, bTruncate ? 0 : FILEWRITE_Append));
		if (!Writer)
		{
			return false;
		}
		Writer->Serialize(const_cast<uint8*>(Bytes.GetData()), Bytes.Num());
		return Writer->Close();
	}

	/** SHA-256 of a file, read in 1 MiB blocks. Runs on a worker thread. */
	FString HashFile(const FString& Path)
	{
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
		if (!Reader)
		{
			return FString();
		}
		// Streaming SHA-256 would avoid holding the file; files here are tens of MB at most.
		TArray<uint8> All;
		All.SetNumUninitialized(Reader->TotalSize());
		Reader->Serialize(All.GetData(), All.Num());
		Reader->Close();
		return ScenarioHash::ToHex(ScenarioHash::Sha256(All));
	}

	/** "bytes 0-8388607/52428800" to the total, or -1. */
	int64 TotalFromContentRange(const FString& Value)
	{
		FString Left;
		FString Right;
		if (Value.Split(TEXT("/"), &Left, &Right) && Right.IsNumeric())
		{
			return FCString::Atoi64(*Right);
		}
		return -1;
	}
}

struct FScenarioDownloader::FState
{
	FString Url;
	FString Destination;
	FString PartPath;
	int64 Received = 0;
	int64 Total = -1;
	uint32 Generation = 0;
	int32 TransportRetries = 0;
	FScenarioTransferProgress OnProgress;
	FScenarioDownloadCallback OnDone;
};

FScenarioDownloader::FScenarioDownloader(TSharedRef<IScenarioHttpTransport> InTransport)
	: Transport(InTransport)
{
}

bool FScenarioDownloader::IsAllowedUrl(const FString& Url)
{
	const FString Host = ScenarioUrl::Host(Url);
	if (bAllowLoopbackForTests && ScenarioUrl::IsLoopbackHost(Host) && Url.StartsWith(TEXT("http://")))
	{
		return true;
	}
	if (!ScenarioUrl::IsHttps(Url))
	{
		return false;
	}
	return Host == TEXT("cdn.cloud.scenario.com") || Host == TEXT("cdn.scenario.com");
}

void FScenarioDownloader::Download(const FString& Url, const FString& DestinationPath, FScenarioTransferProgress OnProgress, FScenarioDownloadCallback OnDone)
{
	if (!IsAllowedUrl(Url))
	{
		FScenarioDownloadResult Result;
		Result.Error.Kind = EScenarioErrorKind::Local;
		Result.Error.Message = FString::Printf(TEXT("Refused to download from %s: only Scenario's file servers are allowed."), *ScenarioUrl::Host(Url));
		OnDone(Result);
		return;
	}
	TSharedRef<FState> State = MakeShared<FState>();
	State->Url = Url;
	State->Destination = DestinationPath;
	State->PartPath = DestinationPath + TEXT(".part");
	State->Received = FileSize(State->PartPath);
	State->Generation = Generation;
	State->OnProgress = MoveTemp(OnProgress);
	State->OnDone = MoveTemp(OnDone);
	NextChunk(State);
}

void FScenarioDownloader::NextChunk(TSharedRef<FState> State)
{
	if (State->Generation != Generation)
	{
		FScenarioDownloadResult Result;
		Result.Error.Kind = EScenarioErrorKind::Cancelled;
		Result.Error.Message = TEXT("Download stopped.");
		Result.PartialBytes = State->Received;
		Result.TotalBytes = State->Total;
		State->OnDone(Result);
		return;
	}
	if (State->Total >= 0 && State->Received >= State->Total)
	{
		Finish(State, FScenarioDownloadResult());
		return;
	}
	TMap<FString, FString> Headers;
	Headers.Add(TEXT("Range"), FString::Printf(TEXT("bytes=%lld-%lld"), static_cast<long long>(State->Received), static_cast<long long>(State->Received + ChunkBytes - 1)));
	Headers.Add(TEXT("User-Agent"), ScenarioVersion::UserAgent());
	TWeakPtr<FScenarioDownloader> WeakThis = AsShared();
	const int64 Base = State->Received;
	Transport->Send(TEXT("GET"), State->Url, Headers, TArray<uint8>(), 120.0f, [WeakThis, State, Base](const FScenarioHttpResponse& Response)
	{
		const TSharedPtr<FScenarioDownloader> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		FScenarioDownloadResult Failure;
		Failure.PartialBytes = State->Received;
		Failure.TotalBytes = State->Total;
		if (Response.Status == 0)
		{
			// Reads are safe to retry: a short back-off, twice, then report a resumable stop.
			if (State->TransportRetries < 2)
			{
				++State->TransportRetries;
				This->Transport->Delay(1.0f + State->TransportRetries, [WeakThis, State]()
				{
					if (const TSharedPtr<FScenarioDownloader> Again = WeakThis.Pin())
					{
						Again->NextChunk(State);
					}
				});
				return;
			}
			Failure.Error.Kind = Response.TransportError == EScenarioErrorKind::Timeout ? EScenarioErrorKind::Timeout : EScenarioErrorKind::Transport;
			Failure.Error.Message = State->Total > 0
				? FString::Printf(TEXT("Download stopped at %d%%."), static_cast<int32>(100.0 * State->Received / State->Total))
				: TEXT("Download stopped.");
			State->OnDone(Failure);
			return;
		}
		if (Response.Status == 416 && State->Received > 0)
		{
			// The .part already holds the whole file.
			State->Total = State->Received;
			This->Finish(State, FScenarioDownloadResult());
			return;
		}
		if (Response.Status == 401 || Response.Status == 403 || Response.Status == 410)
		{
			Failure.bUrlExpired = true;
			Failure.Error.Kind = EScenarioErrorKind::Forbidden;
			Failure.Error.HttpStatus = Response.Status;
			Failure.Error.Message = TEXT("The download link expired. Getting a fresh one resumes the download.");
			State->OnDone(Failure);
			return;
		}
		if (Response.Status == 404)
		{
			Failure.Error.Kind = EScenarioErrorKind::NotFound;
			Failure.Error.HttpStatus = 404;
			Failure.Error.Message = TEXT("Scenario no longer has this file.");
			State->OnDone(Failure);
			return;
		}
		if (Response.Status != 200 && Response.Status != 206)
		{
			Failure.Error.Kind = Response.Status >= 500 ? EScenarioErrorKind::Http5xx : EScenarioErrorKind::Scenario;
			Failure.Error.HttpStatus = Response.Status;
			Failure.Error.Message = FString::Printf(TEXT("The file server answered HTTP %d."), Response.Status);
			State->OnDone(Failure);
			return;
		}
		State->TransportRetries = 0;
		const bool bWhole = Response.Status == 200;
		if (bWhole)
		{
			// The server ignored the range: this is the whole file.
			State->Total = Response.Body.Num();
		}
		else
		{
			const int64 Total = TotalFromContentRange(Response.Header(TEXT("content-range")));
			if (Total >= 0)
			{
				State->Total = Total;
			}
		}
		TArray<uint8> Bytes = Response.Body;
		const FString PartPath = State->PartPath;
		Async(EAsyncExecution::ThreadPool, [WeakThis, State, Bytes = MoveTemp(Bytes), PartPath, bWhole, Base]()
		{
			const bool bWritten = AppendBytes(PartPath, Bytes, bWhole || Base == 0);
			const int64 NewSize = bWritten ? FileSize(PartPath) : -1;
			AsyncTask(ENamedThreads::GameThread, [WeakThis, State, bWritten, NewSize, Count = Bytes.Num(), bWhole]()
			{
				const TSharedPtr<FScenarioDownloader> This = WeakThis.Pin();
				if (!This.IsValid())
				{
					return;
				}
				if (!bWritten)
				{
					FScenarioDownloadResult Failure;
					Failure.Error.Kind = EScenarioErrorKind::Local;
					Failure.Error.Message = TEXT("Could not write the download to disk. Check free space and permissions.");
					State->OnDone(Failure);
					return;
				}
				State->Received = NewSize;
				if (State->OnProgress)
				{
					State->OnProgress(State->Received, State->Total);
				}
				if (bWhole || Count == 0 || (State->Total >= 0 && State->Received >= State->Total))
				{
					State->Total = State->Received;
					This->Finish(State, FScenarioDownloadResult());
					return;
				}
				This->NextChunk(State);
			});
		});
	});
}

void FScenarioDownloader::Finish(TSharedRef<FState> State, FScenarioDownloadResult)
{
	const FString PartPath = State->PartPath;
	const FString Destination = State->Destination;
	Async(EAsyncExecution::ThreadPool, [State, PartPath, Destination]()
	{
		FScenarioDownloadResult Result;
		Result.Path = Destination;
		IFileManager::Get().Delete(*Destination, false, true, true);
		if (!IFileManager::Get().Move(*Destination, *PartPath, true))
		{
			Result.Error.Kind = EScenarioErrorKind::Local;
			Result.Error.Message = TEXT("Could not finish writing the download.");
		}
		else
		{
			Result.bOk = true;
			Result.Size = FileSize(Destination);
			Result.Sha256 = HashFile(Destination);
		}
		AsyncTask(ENamedThreads::GameThread, [State, Result]()
		{
			State->OnDone(Result);
		});
	});
}

struct FScenarioUploader::FState
{
	FString FilePath;
	FString FileName;
	FString Kind;
	FString ContentType;
	int64 Size = 0;
	FScenarioUploadTicket Ticket;
	int64 Sent = 0;
	FScenarioTransferProgress OnProgress;
	FScenarioUploadCallback OnDone;
};

FScenarioUploader::FScenarioUploader(TSharedRef<IScenarioHttpTransport> InTransport, TSharedRef<FScenarioApi> InApi)
	: Transport(InTransport)
	, Api(InApi)
{
}

bool FScenarioUploader::IsAllowedPartUrl(const FString& Url)
{
	const FString Host = ScenarioUrl::Host(Url);
	if (bAllowLoopbackForTests && ScenarioUrl::IsLoopbackHost(Host) && Url.StartsWith(TEXT("http://")))
	{
		return true;
	}
	return ScenarioUrl::IsHttps(Url) && Host.EndsWith(TEXT(".amazonaws.com"));
}

void FScenarioUploader::Upload(const FString& FilePath, const FString& Kind, const FString& ContentType, FScenarioTransferProgress OnProgress, FScenarioUploadCallback OnDone)
{
	TSharedRef<FState> State = MakeShared<FState>();
	State->FilePath = FilePath;
	State->FileName = FPaths::GetCleanFilename(FilePath);
	State->Kind = Kind;
	State->ContentType = ContentType;
	State->Size = FileSize(FilePath);
	State->OnProgress = MoveTemp(OnProgress);
	State->OnDone = MoveTemp(OnDone);
	if (State->Size <= 0)
	{
		FScenarioUploadResult Result;
		Result.Error.Kind = EScenarioErrorKind::Local;
		Result.Error.Message = TEXT("The file to upload is missing or empty.");
		State->OnDone(Result);
		return;
	}
	TWeakPtr<FScenarioUploader> WeakThis = AsShared();
	Api->UploadStart(State->FileName, ContentType, Kind, State->Size, [WeakThis, State](const TScenarioResult<FScenarioUploadTicket>& Started)
	{
		const TSharedPtr<FScenarioUploader> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		if (!Started.bOk)
		{
			FScenarioUploadResult Result;
			Result.Error = Started.Error;
			State->OnDone(Result);
			return;
		}
		State->Ticket = Started.Value;
		if (!State->Ticket.AssetId.IsEmpty() && State->Ticket.Parts.Num() == 0)
		{
			FScenarioUploadResult Result;
			Result.bOk = true;
			Result.AssetId = State->Ticket.AssetId;
			State->OnDone(Result);
			return;
		}
		if (State->Ticket.PartSize <= 0)
		{
			State->Ticket.PartSize = FMath::DivideAndRoundUp<int64>(State->Size, State->Ticket.Parts.Num());
		}
		for (const TPair<int32, FString>& Part : State->Ticket.Parts)
		{
			if (!IsAllowedPartUrl(Part.Value))
			{
				FScenarioUploadResult Result;
				Result.Error.Kind = EScenarioErrorKind::Protocol;
				Result.Error.Message = TEXT("Scenario sent an upload address outside its storage service; nothing was uploaded.");
				State->OnDone(Result);
				return;
			}
		}
		This->PutPart(State, 0, 0);
	});
}

void FScenarioUploader::PutPart(TSharedRef<FState> State, int32 PartIndex, int32 Attempt)
{
	if (PartIndex >= State->Ticket.Parts.Num())
	{
		Complete(State, 0);
		return;
	}
	const int64 Offset = static_cast<int64>(State->Ticket.Parts[PartIndex].Key - 1) * State->Ticket.PartSize;
	const int64 Length = FMath::Min(State->Ticket.PartSize, State->Size - Offset);
	if (Length <= 0)
	{
		PutPart(State, PartIndex + 1, 0);
		return;
	}
	TWeakPtr<FScenarioUploader> WeakThis = AsShared();
	const FString Path = State->FilePath;
	Async(EAsyncExecution::ThreadPool, [WeakThis, State, PartIndex, Attempt, Path, Offset, Length]()
	{
		TArray<uint8> Bytes;
		bool bRead = false;
		if (TUniquePtr<FArchive> Reader = TUniquePtr<FArchive>(IFileManager::Get().CreateFileReader(*Path)))
		{
			Reader->Seek(Offset);
			Bytes.SetNumUninitialized(Length);
			Reader->Serialize(Bytes.GetData(), Length);
			bRead = Reader->Close();
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, State, PartIndex, Attempt, bRead, Bytes = MoveTemp(Bytes)]()
		{
			const TSharedPtr<FScenarioUploader> This = WeakThis.Pin();
			if (!This.IsValid())
			{
				return;
			}
			if (!bRead)
			{
				FScenarioUploadResult Result;
				Result.Error.Kind = EScenarioErrorKind::Local;
				Result.Error.Message = TEXT("Could not read the file to upload.");
				State->OnDone(Result);
				return;
			}
			// The full presigned URL only: no checksum headers, or storage refuses the signature.
			// The plugin's User-Agent replaces Unreal's default, which names the project.
			TMap<FString, FString> Headers;
			Headers.Add(TEXT("User-Agent"), ScenarioVersion::UserAgent());
			const int64 PartBytes = Bytes.Num();
			This->Transport->Send(TEXT("PUT"), State->Ticket.Parts[PartIndex].Value, Headers, Bytes, 300.0f,
				[WeakThis, State, PartIndex, Attempt, PartBytes](const FScenarioHttpResponse& Response)
			{
				const TSharedPtr<FScenarioUploader> Self = WeakThis.Pin();
				if (!Self.IsValid())
				{
					return;
				}
				if (Response.Status >= 200 && Response.Status < 300)
				{
					State->Sent += PartBytes;
					if (State->OnProgress)
					{
						State->OnProgress(State->Sent, State->Size);
					}
					Self->PutPart(State, PartIndex + 1, 0);
					return;
				}
				const bool bRetryable = Response.Status == 0 || Response.Status >= 500 || Response.Status == 429;
				if (bRetryable && Attempt < 2)
				{
					Self->Transport->Delay(1.0f + 2.0f * Attempt, [WeakThis, State, PartIndex, Attempt]()
					{
						if (const TSharedPtr<FScenarioUploader> Again = WeakThis.Pin())
						{
							Again->PutPart(State, PartIndex, Attempt + 1);
						}
					});
					return;
				}
				FScenarioUploadResult Result;
				Result.Error.Kind = Response.Status == 0 ? EScenarioErrorKind::Transport : EScenarioErrorKind::Scenario;
				Result.Error.HttpStatus = Response.Status;
				Result.Error.Message = Response.Status == 403
					? TEXT("The upload link was refused or expired. Try the upload again.")
					: FString::Printf(TEXT("Uploading part %d failed."), PartIndex + 1);
				State->OnDone(Result);
			});
		});
	});
}

void FScenarioUploader::Complete(TSharedRef<FState> State, int32 Attempt)
{
	TWeakPtr<FScenarioUploader> WeakThis = AsShared();
	Api->UploadComplete(State->Ticket.UploadId, [WeakThis, State, Attempt](const TScenarioResult<FString>& Completed)
	{
		const TSharedPtr<FScenarioUploader> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		if (Completed.bOk)
		{
			FScenarioUploadResult Result;
			Result.bOk = true;
			Result.AssetId = Completed.Value;
			State->OnDone(Result);
			return;
		}
		// Completing again resumes the same upload (Scenario's documented recovery); it never creates a second asset.
		if (Completed.Error.Kind == EScenarioErrorKind::Timeout && Attempt < 3)
		{
			This->Transport->Delay(3.0f, [WeakThis, State, Attempt]()
			{
				if (const TSharedPtr<FScenarioUploader> Again = WeakThis.Pin())
				{
					Again->Complete(State, Attempt + 1);
				}
			});
			return;
		}
		FScenarioUploadResult Result;
		Result.Error = Completed.Error;
		State->OnDone(Result);
	});
}
