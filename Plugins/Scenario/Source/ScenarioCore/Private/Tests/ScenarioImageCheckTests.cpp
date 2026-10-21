// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "ScenarioFakes.h"
#include "ScenarioImageChecks.h"

namespace
{
	TArray<FColor> Solid(const FColor& Color, int32 Count = 64 * 64)
	{
		TArray<FColor> Pixels;
		Pixels.Init(Color, Count);
		return Pixels;
	}

	/** A horizontal ramp from Low to High luminance, Width x Height. */
	TArray<FColor> Ramp(int32 Low, int32 High, int32 Width = 256, int32 Height = 16)
	{
		TArray<FColor> Pixels;
		Pixels.Reserve(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const uint8 V = static_cast<uint8>(Low + (High - Low) * X / FMath::Max(1, Width - 1));
				Pixels.Add(FColor(V, V, V, 255));
			}
		}
		return Pixels;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioFrameCheckTest, "Scenario.Core.Images.FrameCheck", SCENARIO_TEST_FLAGS)
bool FScenarioFrameCheckTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("luma of white"), ScenarioImageChecks::Luma(FColor::White), 255);
	TestEqual(TEXT("luma of black"), ScenarioImageChecks::Luma(FColor::Black), 0);
	TestEqual(TEXT("green weighs most"), ScenarioImageChecks::Luma(FColor(0, 255, 0)), 182);

	const FScenarioFrameCheck Black = ScenarioImageChecks::CheckFrame(Solid(FColor(2, 2, 2)));
	TestTrue(TEXT("black frame flagged"), Black.bAllBlack);
	TestFalse(TEXT("black is not also uniform"), Black.bUniform);
	TestFalse(TEXT("black is not ok"), Black.IsOk());
	TestEqual(TEXT("one issue for black"), Black.Issues().Num(), 1);

	const FScenarioFrameCheck White = ScenarioImageChecks::CheckFrame(Solid(FColor::White));
	TestTrue(TEXT("white frame flagged"), White.bAllWhite);
	TestFalse(TEXT("white is not clipped twice"), White.bClippedHighlights);

	const FScenarioFrameCheck Grey = ScenarioImageChecks::CheckFrame(Solid(FColor(120, 130, 125)));
	TestTrue(TEXT("flat grey is uniform"), Grey.bUniform);
	TestFalse(TEXT("flat grey is not black"), Grey.bAllBlack || Grey.bAllWhite);

	const FScenarioFrameCheck Good = ScenarioImageChecks::CheckFrame(Ramp(20, 230));
	TestTrue(TEXT("a full ramp is ok"), Good.IsOk());
	TestEqual(TEXT("no issues on a good frame"), Good.Issues().Num(), 0);
	TestTrue(TEXT("p1 near the dark end"), Good.Low1 <= 25);
	TestTrue(TEXT("p99 near the light end"), Good.High99 >= 225);

	// A third of the frame burnt out: clipped highlights.
	TArray<FColor> Burnt = Ramp(30, 200);
	for (int32 Index = 0; Index < Burnt.Num() / 3; ++Index)
	{
		Burnt[Index] = FColor::White;
	}
	const FScenarioFrameCheck Clipped = ScenarioImageChecks::CheckFrame(Burnt);
	TestTrue(TEXT("clipped highlights flagged"), Clipped.bClippedHighlights);
	TestFalse(TEXT("clipped frame is not all white"), Clipped.bAllWhite);

	// Half the frame at 0: crushed shadows.
	TArray<FColor> Dark = Ramp(40, 220);
	for (int32 Index = 0; Index < Dark.Num() / 2; ++Index)
	{
		Dark[Index] = FColor::Black;
	}
	const FScenarioFrameCheck Crushed = ScenarioImageChecks::CheckFrame(Dark);
	TestTrue(TEXT("crushed shadows flagged"), Crushed.bCrushedShadows);
	TestFalse(TEXT("crushed frame is not all black"), Crushed.bAllBlack);

	const TSharedRef<FJsonObject> Json = Crushed.ToJson();
	TestEqual(TEXT("verdict in JSON"), Json->GetStringField(TEXT("verdict")), FString(TEXT("check")));
	TestEqual(TEXT("issues in JSON"), Json->GetArrayField(TEXT("issues")).Num(), 1);

	const FScenarioFrameCheck Empty = ScenarioImageChecks::CheckFrame(TArrayView<const FColor>());
	TestFalse(TEXT("an empty frame is not ok"), Empty.IsOk());
	return true;
}

#endif
