// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioCU.h"

#include "ScenarioJson.h"

namespace
{
	FString TrimDecimals(const FString& Text)
	{
		FString Out = Text;
		if (Out.Contains(TEXT(".")))
		{
			while (Out.EndsWith(TEXT("0")))
			{
				Out.LeftChopInline(1);
			}
			if (Out.EndsWith(TEXT(".")))
			{
				Out.LeftChopInline(1);
			}
		}
		return Out;
	}

	FString WithSeparators(const FString& IntegerPart)
	{
		FString Digits = IntegerPart;
		FString Sign;
		if (Digits.StartsWith(TEXT("-")))
		{
			Sign = TEXT("-");
			Digits.RightChopInline(1);
		}
		FString Out;
		for (int32 Index = 0; Index < Digits.Len(); ++Index)
		{
			if (Index > 0 && (Digits.Len() - Index) % 3 == 0)
			{
				Out += TEXT(",");
			}
			Out += Digits[Index];
		}
		return Sign + Out;
	}
}

FString ScenarioCU::Format(double Value, bool bWithUnit)
{
	FString Text;
	const double Abs = FMath::Abs(Value);
	if (Abs < 10000.0)
	{
		const FString Fixed = TrimDecimals(FString::Printf(TEXT("%.3f"), Value));
		FString IntegerPart = Fixed;
		FString Fraction;
		int32 Dot = INDEX_NONE;
		if (Fixed.FindChar(TEXT('.'), Dot))
		{
			IntegerPart = Fixed.Left(Dot);
			Fraction = Fixed.Mid(Dot);
		}
		Text = WithSeparators(IntegerPart) + Fraction;
	}
	else
	{
		static const TCHAR* const Suffixes[] = { TEXT("K"), TEXT("M"), TEXT("B"), TEXT("T") };
		double Scaled = Value / 1000.0;
		int32 SuffixIndex = 0;
		while (FMath::Abs(Scaled) >= 1000.0 && SuffixIndex < 3)
		{
			Scaled /= 1000.0;
			++SuffixIndex;
		}
		const double ScaledAbs = FMath::Abs(Scaled);
		const int32 Decimals = ScaledAbs >= 100.0 ? 0 : (ScaledAbs >= 10.0 ? 1 : 2);
		Text = TrimDecimals(FString::Printf(TEXT("%.*f"), Decimals, Scaled)) + Suffixes[SuffixIndex];
	}
	return bWithUnit ? Text + TEXT(" CU") : Text;
}

FString ScenarioCU::Exact(double Value)
{
	return ScenarioJson::NumberText(Value);
}

bool ScenarioCU::Parse(const FString& Text, double& OutValue)
{
	FString Clean = Text.TrimStartAndEnd();
	Clean.RemoveFromEnd(TEXT("CU"));
	Clean = Clean.TrimStartAndEnd().Replace(TEXT(","), TEXT(""));
	if (Clean.IsEmpty() || !Clean.IsNumeric())
	{
		return false;
	}
	OutValue = FCString::Atod(*Clean);
	return true;
}
