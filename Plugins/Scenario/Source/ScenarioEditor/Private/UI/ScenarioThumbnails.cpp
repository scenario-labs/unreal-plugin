// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioThumbnails.h"

#include "Async/Async.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "HttpModule.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/** Concurrent remote fetches; local files are cheap and skip the queue. */
	constexpr int32 MaxInFlight = 4;
	bool bShutDown = false;
}

FScenarioThumbnails& FScenarioThumbnails::Get()
{
	static FScenarioThumbnails Instance;
	return Instance;
}

void FScenarioThumbnails::Shutdown()
{
	bShutDown = true;
	Brushes.Reset();
	Queue.Reset();
	OnLoaded.Clear();
}

const FSlateBrush* FScenarioThumbnails::ForFile(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return nullptr;
	}
	if (const TSharedPtr<FSlateDynamicImageBrush>* Found = Brushes.Find(Path))
	{
		return Found->Get();
	}
	if (Pending.Contains(Path) || Failed.Contains(Path) || bShutDown)
	{
		return nullptr;
	}
	const FString Extension = FPaths::GetExtension(Path).ToLower();
	if (Extension != TEXT("png") && Extension != TEXT("jpg") && Extension != TEXT("jpeg") && Extension != TEXT("webp") && Extension != TEXT("bmp"))
	{
		Failed.Add(Path);
		return nullptr;
	}
	Pending.Add(Path);
	Async(EAsyncExecution::ThreadPool, [Path]()
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			Bytes.Reset();
		}
		AsyncTask(ENamedThreads::GameThread, [Path, Bytes = MoveTemp(Bytes)]() mutable
		{
			if (!bShutDown)
			{
				FScenarioThumbnails::Get().Decode(Path, MoveTemp(Bytes));
			}
		});
	});
	return nullptr;
}

const FSlateBrush* FScenarioThumbnails::ForUrl(const FString& Key, const FString& Url)
{
	if (Key.IsEmpty() || Url.IsEmpty())
	{
		return nullptr;
	}
	if (const TSharedPtr<FSlateDynamicImageBrush>* Found = Brushes.Find(Key))
	{
		return Found->Get();
	}
	if (Pending.Contains(Key) || Failed.Contains(Key) || bShutDown)
	{
		return nullptr;
	}
	Pending.Add(Key);
	Queue.Add({ Key, Url });
	Pump();
	return nullptr;
}

void FScenarioThumbnails::Pump()
{
	while (InFlight < MaxInFlight && Queue.Num() > 0)
	{
		const TPair<FString, FString> Item = Queue[0];
		Queue.RemoveAt(0);
		++InFlight;
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Item.Value);
		Request->SetVerb(TEXT("GET"));
		Request->SetTimeout(30.0f);
		const FString Key = Item.Key;
		Request->OnProcessRequestComplete().BindLambda([Key](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (bShutDown)
			{
				return;
			}
			FScenarioThumbnails& Self = FScenarioThumbnails::Get();
			--Self.InFlight;
			TArray<uint8> Bytes;
			if (bConnected && Response.IsValid() && Response->GetResponseCode() == 200)
			{
				Bytes = Response->GetContent();
			}
			Self.Decode(Key, MoveTemp(Bytes));
			Self.Pump();
		});
		Request->ProcessRequest();
	}
}

void FScenarioThumbnails::Decode(const FString& Key, TArray<uint8> Bytes)
{
	if (Bytes.Num() == 0)
	{
		Pending.Remove(Key);
		Failed.Add(Key);
		return;
	}
	Async(EAsyncExecution::ThreadPool, [Key, Bytes = MoveTemp(Bytes)]()
	{
		FImage Source;
		int32 Width = 0;
		int32 Height = 0;
		TArray<uint8> Bgra;
		if (FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Source) && Source.SizeX > 0 && Source.SizeY > 0)
		{
			const float Scale = FMath::Min(1.0f, static_cast<float>(MaxSize) / FMath::Max(Source.SizeX, Source.SizeY));
			Width = FMath::Max(1, FMath::RoundToInt(Source.SizeX * Scale));
			Height = FMath::Max(1, FMath::RoundToInt(Source.SizeY * Scale));
			FImage Thumb;
			Source.ResizeTo(Thumb, Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			Bgra = MoveTemp(Thumb.RawData);
		}
		AsyncTask(ENamedThreads::GameThread, [Key, Width, Height, Bgra = MoveTemp(Bgra)]() mutable
		{
			if (!bShutDown)
			{
				FScenarioThumbnails::Get().Finish(Key, Width, Height, MoveTemp(Bgra));
			}
		});
	});
}

void FScenarioThumbnails::Finish(const FString& Key, int32 Width, int32 Height, TArray<uint8> Bgra)
{
	Pending.Remove(Key);
	if (Bgra.Num() != Width * Height * 4 || Width <= 0)
	{
		Failed.Add(Key);
		return;
	}
	static int32 Counter = 0;
	const FName Name(*FString::Printf(TEXT("ScenarioThumb_%d"), ++Counter));
	TSharedPtr<FSlateDynamicImageBrush> Brush = FSlateDynamicImageBrush::CreateWithImageData(Name, FVector2D(Width, Height), Bgra);
	if (!Brush.IsValid())
	{
		Failed.Add(Key);
		return;
	}
	Brushes.Add(Key, Brush);
	OnLoaded.Broadcast();
}
