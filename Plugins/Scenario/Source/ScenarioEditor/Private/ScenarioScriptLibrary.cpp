// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioScriptLibrary.h"

#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include "ScenarioActions.h"
#include "ScenarioJson.h"
#include "ScenarioSubsystem.h"

namespace
{
	TMap<FString, FScenarioScriptCall>& Calls()
	{
		static TMap<FString, FScenarioScriptCall> Map;
		return Map;
	}

	TArray<FString>& CallOrder()
	{
		static TArray<FString> Order;
		return Order;
	}

	/** Scripts get the data only: a screenshot's file path is in it, the image bytes are not. */
	FString ResultText(const FScenarioActionResult& Result)
	{
		return Result.Data.IsValid() ? ScenarioJson::Write(Result.Data) : FString(TEXT("{}"));
	}
}

FString UScenarioScriptLibrary::StartAction(const FString& Name, const FString& ArgumentsJson, const FString& ScriptName)
{
	static int32 Counter = 0;
	const FString CallId = FString::Printf(TEXT("call_%d"), ++Counter);
	FScenarioScriptCall& Call = Calls().Add(CallId);
	Call.CallId = CallId;
	CallOrder().Add(CallId);
	while (CallOrder().Num() > 200)
	{
		Calls().Remove(CallOrder()[0]);
		CallOrder().RemoveAt(0);
	}

	const FString Trimmed = ArgumentsJson.TrimStartAndEnd();
	const TSharedPtr<FJsonObject> Args = Trimmed.IsEmpty() ? MakeShared<FJsonObject>() : ScenarioJson::ParseObject(Trimmed);
	if (!Args.IsValid())
	{
		Call.bDone = true;
		Call.Error = TEXT("Invalid: ArgumentsJson is not a JSON object.");
		return CallId;
	}
	FScenarioActions::Get().Call(Name, Args, FScenarioCaller::Script(ScriptName.IsEmpty() ? TEXT("script") : ScriptName), [CallId](const FScenarioActionResult& Result)
	{
		if (FScenarioScriptCall* Entry = Calls().Find(CallId))
		{
			Entry->bDone = true;
			Entry->bOk = Result.bOk;
			Entry->Error = Result.Error;
			Entry->ResultJson = ResultText(Result);
		}
	});
	return CallId;
}

FScenarioScriptCall UScenarioScriptLibrary::GetCall(const FString& CallId)
{
	if (const FScenarioScriptCall* Call = Calls().Find(CallId))
	{
		return *Call;
	}
	FScenarioScriptCall Missing;
	Missing.CallId = CallId;
	Missing.bDone = true;
	Missing.Error = TEXT("NotFound: unknown call id (the last 200 calls are kept).");
	return Missing;
}

TArray<FString> UScenarioScriptLibrary::ListActions()
{
	TArray<FString> Names;
	for (const FScenarioAction& Action : FScenarioActions::Get().All())
	{
		Names.Add(Action.Name);
	}
	return Names;
}

FString UScenarioScriptLibrary::DescribeAction(const FString& Name)
{
	const FScenarioAction* Action = FScenarioActions::Get().Find(Name);
	if (!Action)
	{
		return FString();
	}
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Action->Name);
	Json->SetStringField(TEXT("description"), Action->Description);
	if (Action->InputSchema.IsValid())
	{
		Json->SetObjectField(TEXT("arguments"), Action->InputSchema);
	}
	return ScenarioJson::Write(Json, true);
}

bool UScenarioScriptLibrary::IsReady()
{
	const UScenarioSubsystem* S = UScenarioSubsystem::Get();
	return S && S->Session().IsReady();
}

bool UScenarioScriptLibrary::FindPanoramaSun(UTexture2D* Panorama, float& AzimuthDegrees, float& ElevationDegrees, float& Strength)
{
	AzimuthDegrees = 0.0f;
	ElevationDegrees = 0.0f;
	Strength = 0.0f;
#if WITH_EDITORONLY_DATA
	FImage Source;
	if (!Panorama || !Panorama->Source.IsValid() || !Panorama->Source.GetMipImage(Source, 0))
	{
		return false;
	}
	// Linear light at 256 x 128: a box filter keeps the sun's energy, and a cell is about 1.4 degrees.
	constexpr int32 Width = 256;
	constexpr int32 Height = 128;
	FImage Small;
	FImageCore::ResizeImageAllocDest(Source, Small, Width, Height, ERawImageFormat::RGBA32F, EGammaSpace::Linear,
		FImageCore::EResizeImageFilter::Box | FImageCore::EResizeImageFilter::Flag_WrapX);
	const TArrayView64<FLinearColor> Pixels = Small.AsRGBA32F();
	auto Luminance = [&Pixels](int32 X, int32 Y)
	{
		const FLinearColor& C = Pixels[(int64)Y * Width + ((X % Width) + Width) % Width];
		return FMath::Max(0.0f, 0.2126f * C.R + 0.7152f * C.G + 0.0722f * C.B);
	};

	// The brightest cell of the upper half (the sky), and the sky's median.
	TArray<float> Sky;
	Sky.Reserve(Width * Height / 2);
	int32 BestX = 0;
	int32 BestY = 0;
	float Best = -1.0f;
	for (int32 Y = 0; Y < Height / 2; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			const float L = Luminance(X, Y);
			Sky.Add(L);
			if (L > Best)
			{
				Best = L;
				BestX = X;
				BestY = Y;
			}
		}
	}
	if (Best <= 0.0f)
	{
		return false;
	}
	Sky.Sort();
	const float Median = Sky[Sky.Num() / 2];

	// Centre of the bright region around that cell (a clipped sun is a disc of equal cells), wrapping in azimuth.
	double SumX = 0.0;
	double SumY = 0.0;
	double SumW = 0.0;
	for (int32 DY = -6; DY <= 6; ++DY)
	{
		const int32 Y = BestY + DY;
		if (Y < 0 || Y >= Height / 2)
		{
			continue;
		}
		for (int32 DX = -8; DX <= 8; ++DX)
		{
			const float L = Luminance(BestX + DX, Y);
			if (L >= 0.9f * Best)
			{
				SumX += L * (BestX + DX + 0.5);
				SumY += L * (Y + 0.5);
				SumW += L;
			}
		}
	}
	const double U = SumX / SumW / Width;
	const double V = SumY / SumW / Height;
	// The sky material reads u = atan2(d.y, d.x) / 2pi + 0.5 and v = acos(d.z) / pi.
	AzimuthDegrees = (float)FRotator::NormalizeAxis((U - 0.5) * 360.0);
	ElevationDegrees = (float)(90.0 - V * 180.0);
	Strength = Best / FMath::Max(Median, 1.0e-4f);
	return true;
#else
	return false;
#endif
}

UScenarioRunActionAsync* UScenarioRunActionAsync::RunScenarioAction(const FString& Name, const FString& ArgumentsJson)
{
	UScenarioRunActionAsync* Node = NewObject<UScenarioRunActionAsync>();
	Node->ActionName = Name;
	Node->Arguments = ArgumentsJson;
	// Editor utilities have no game instance to keep the node alive while it waits.
	Node->AddToRoot();
	return Node;
}

void UScenarioRunActionAsync::Activate()
{
	const FString Trimmed = Arguments.TrimStartAndEnd();
	const TSharedPtr<FJsonObject> Args = Trimmed.IsEmpty() ? MakeShared<FJsonObject>() : ScenarioJson::ParseObject(Trimmed);
	if (!Args.IsValid())
	{
		OnFinished.Broadcast(false, TEXT("{}"), TEXT("Invalid: ArgumentsJson is not a JSON object."));
		RemoveFromRoot();
		SetReadyToDestroy();
		return;
	}
	TWeakObjectPtr<UScenarioRunActionAsync> Weak(this);
	FScenarioActions::Get().Call(ActionName, Args, FScenarioCaller::Script(TEXT("blueprint")), [Weak](const FScenarioActionResult& Result)
	{
		if (UScenarioRunActionAsync* Node = Weak.Get())
		{
			Node->OnFinished.Broadcast(Result.bOk, ResultText(Result), Result.Error);
			Node->RemoveFromRoot();
			Node->SetReadyToDestroy();
		}
	});
}
