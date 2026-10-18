// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

/**
 * Creative Unit formatting, shared with the other Scenario plugins:
 * up to three decimals without trailing zeros, thousands separators below
 * 10,000, then K, M, B, T with three significant digits.
 */
namespace ScenarioCU
{
	/** 3 -> "3 CU", 2.75 -> "2.75 CU", 1240 -> "1,240 CU", 12345 -> "12.3K CU". */
	SCENARIOCORE_API FString Format(double Value, bool bWithUnit = true);

	/** The exact decimal string carried by quotes and approvals: 11 -> "11", 2.75 -> "2.75". */
	SCENARIOCORE_API FString Exact(double Value);

	SCENARIOCORE_API bool Parse(const FString& Text, double& OutValue);
}
