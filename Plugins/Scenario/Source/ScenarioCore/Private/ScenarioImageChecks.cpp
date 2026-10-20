// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioImageChecks.h"

#include "Dom/JsonObject.h"
#include "ScenarioJson.h"

namespace
{
	constexpr int32 DarkEnd = 8;
	constexpr int32 LightEnd = 255 - DarkEnd;
	constexpr int32 Clipped = 250;
	constexpr int32 Crushed = 5;
	constexpr double EndShare = 0.99;
	constexpr double UniformDeviation = 2.0;
	constexpr double ClippedLimit = 0.05;
	constexpr double CrushedLimit = 0.25;

	double Round(double Value, double Step)
	{
		return FMath::RoundToDouble(Value / Step) * Step;
	}
}

int32 ScenarioImageChecks::Luma(const FColor& Pixel)
{
	return FMath::Clamp(FMath::RoundToInt(0.2126 * Pixel.R + 0.7152 * Pixel.G + 0.0722 * Pixel.B), 0, 255);
}

FScenarioFrameCheck ScenarioImageChecks::CheckFrame(TArrayView<const FColor> Pixels)
{
	FScenarioFrameCheck Check;
	Check.Pixels = Pixels.Num();
	if (Pixels.Num() == 0)
	{
		Check.bUniform = true;
		return Check;
	}
	uint64 Histogram[256] = {};
	double Sum = 0.0;
	double SumSquares = 0.0;
	for (const FColor& Pixel : Pixels)
	{
		const int32 Y = Luma(Pixel);
		++Histogram[Y];
		Sum += Y;
		SumSquares += static_cast<double>(Y) * Y;
	}
	const double Count = static_cast<double>(Pixels.Num());
	Check.MeanLuma = Sum / Count;
	Check.StdDevLuma = FMath::Sqrt(FMath::Max(0.0, SumSquares / Count - Check.MeanLuma * Check.MeanLuma));

	uint64 Dark = 0;
	uint64 Light = 0;
	uint64 ClippedCount = 0;
	uint64 CrushedCount = 0;
	uint64 Running = 0;
	bool bLowSet = false;
	bool bHighSet = false;
	for (int32 Level = 0; Level < 256; ++Level)
	{
		const uint64 N = Histogram[Level];
		Dark += Level <= DarkEnd ? N : 0;
		Light += Level >= LightEnd ? N : 0;
		ClippedCount += Level >= Clipped ? N : 0;
		CrushedCount += Level <= Crushed ? N : 0;
		Running += N;
		if (!bLowSet && Running >= Count * 0.01)
		{
			Check.Low1 = Level;
			bLowSet = true;
		}
		if (!bHighSet && Running >= Count * 0.99)
		{
			Check.High99 = Level;
			bHighSet = true;
		}
	}
	Check.ClippedShare = ClippedCount / Count;
	Check.CrushedShare = CrushedCount / Count;
	Check.bAllBlack = Dark >= Count * EndShare;
	Check.bAllWhite = !Check.bAllBlack && Light >= Count * EndShare;
	// A flat frame of one mid color: nothing loaded, a wall against the lens, a missing sky.
	Check.bUniform = !Check.bAllBlack && !Check.bAllWhite && Check.StdDevLuma < UniformDeviation;
	Check.bClippedHighlights = !Check.bAllWhite && !Check.bUniform && Check.ClippedShare > ClippedLimit;
	Check.bCrushedShadows = !Check.bAllBlack && !Check.bUniform && Check.CrushedShare > CrushedLimit;
	return Check;
}

TArray<FString> FScenarioFrameCheck::Issues() const
{
	TArray<FString> Out;
	if (bAllBlack)
	{
		Out.Add(TEXT("All black: no light reaches the camera, the camera is inside a mesh, or exposure is far too low."));
	}
	if (bAllWhite)
	{
		Out.Add(TEXT("All white: exposure is far too high, or the camera faces a light or an unlit white surface."));
	}
	if (bUniform)
	{
		Out.Add(TEXT("Uniform: one flat color with no detail (the camera may face a wall, fog or an empty sky)."));
	}
	if (bClippedHighlights)
	{
		Out.Add(FString::Printf(TEXT("Clipped highlights: %.0f%% of the pixels are pure white; lower the exposure or the light intensity."), ClippedShare * 100.0));
	}
	if (bCrushedShadows)
	{
		Out.Add(FString::Printf(TEXT("Crushed shadows: %.0f%% of the pixels are black; add fill light or raise the exposure."), CrushedShare * 100.0));
	}
	return Out;
}

TSharedRef<FJsonObject> FScenarioFrameCheck::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("verdict"), IsOk() ? TEXT("ok") : TEXT("check"));
	Json->SetField(TEXT("issues"), ScenarioJson::StringArrayValue(Issues()));
	Json->SetNumberField(TEXT("mean_luma"), Round(MeanLuma, 0.1));
	Json->SetNumberField(TEXT("luma_deviation"), Round(StdDevLuma, 0.1));
	Json->SetNumberField(TEXT("clipped_percent"), Round(ClippedShare * 100.0, 0.1));
	Json->SetNumberField(TEXT("crushed_percent"), Round(CrushedShare * 100.0, 0.1));
	Json->SetNumberField(TEXT("luma_p1"), Low1);
	Json->SetNumberField(TEXT("luma_p99"), High99);
	return Json;
}
