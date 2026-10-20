// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * A frame check for screenshots an agent will judge: a luminance histogram
 * of 8-bit sRGB pixels, read for the usual capture failures.
 */
struct SCENARIOCORE_API FScenarioFrameCheck
{
	int32 Pixels = 0;
	/** Mean and standard deviation of the luminance, 0 to 255. */
	double MeanLuma = 0.0;
	double StdDevLuma = 0.0;
	/** Share of pixels at 250 or more (clipped) and at 5 or less (crushed), 0 to 1. */
	double ClippedShare = 0.0;
	double CrushedShare = 0.0;
	/** Luminance below which 1% and 99% of the pixels sit. */
	int32 Low1 = 0;
	int32 High99 = 0;

	bool bAllBlack = false;
	bool bAllWhite = false;
	bool bUniform = false;
	bool bClippedHighlights = false;
	bool bCrushedShadows = false;

	/** True when no flag is set. */
	bool IsOk() const { return !bAllBlack && !bAllWhite && !bUniform && !bClippedHighlights && !bCrushedShadows; }
	/** One sentence per flag, in plain words; empty when the frame looks usable. */
	TArray<FString> Issues() const;
	TSharedRef<FJsonObject> ToJson() const;
};

namespace ScenarioImageChecks
{
	/** Rec. 709 luminance of an 8-bit pixel, 0 to 255 (alpha ignored). */
	SCENARIOCORE_API int32 Luma(const FColor& Pixel);

	/**
	 * Histogram check of a frame. Thresholds: all black or all white when 99%
	 * of the pixels are within 8 levels of the end; uniform when the deviation
	 * is under 2 levels; clipped highlights above 5% of pixels at 250 or more;
	 * crushed shadows above 25% at 5 or less.
	 */
	SCENARIOCORE_API FScenarioFrameCheck CheckFrame(TArrayView<const FColor> Pixels);
}
