// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioOutputLog.h"

#include "Misc/OutputDeviceRedirector.h"
#include "ScenarioTypes.h"

FScenarioOutputLog& FScenarioOutputLog::Get()
{
	static FScenarioOutputLog Instance;
	return Instance;
}

void FScenarioOutputLog::Register()
{
	if (!bRegistered && GLog)
	{
		GLog->AddOutputDevice(this);
		bRegistered = true;
	}
}

void FScenarioOutputLog::Unregister()
{
	if (bRegistered && GLog)
	{
		GLog->RemoveOutputDevice(this);
	}
	bRegistered = false;
}

void FScenarioOutputLog::Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category)
{
	const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
	const bool bProblem = Level == ELogVerbosity::Warning || Level == ELogVerbosity::Error || Level == ELogVerbosity::Fatal;
	if (!bProblem)
	{
		const FString Name = Category.ToString();
		if (!Name.StartsWith(TEXT("LogScenario")) && !Name.StartsWith(TEXT("LogInterchange")))
		{
			return;
		}
	}
	FEntry Entry;
	Entry.Time = ScenarioNow();
	Entry.Verbosity = Level;
	Entry.Category = Category;
	Entry.Text = FString(Text).Left(1000);

	FScopeLock Guard(&Lock);
	if (Entries.Num() < MaxEntries)
	{
		Entries.Add(MoveTemp(Entry));
	}
	else
	{
		Entries[Next] = MoveTemp(Entry);
	}
	Next = (Next + 1) % MaxEntries;
}

TArray<FScenarioOutputLog::FEntry> FScenarioOutputLog::Recent(bool bWarnings, bool bAll, const FString& Category, int32 Limit, double SinceSeconds) const
{
	TArray<FEntry> Ordered;
	{
		FScopeLock Guard(&Lock);
		const int32 Count = Entries.Num();
		const int32 Start = Count < MaxEntries ? 0 : Next;
		for (int32 Offset = 0; Offset < Count; ++Offset)
		{
			Ordered.Add(Entries[(Start + Offset) % Count]);
		}
	}
	const double Since = SinceSeconds > 0.0 ? ScenarioNow() - SinceSeconds : 0.0;
	TArray<FEntry> Out;
	for (int32 Index = Ordered.Num() - 1; Index >= 0 && Out.Num() < Limit; --Index)
	{
		const FEntry& Entry = Ordered[Index];
		const bool bError = Entry.Verbosity == ELogVerbosity::Error || Entry.Verbosity == ELogVerbosity::Fatal;
		const bool bWarning = Entry.Verbosity == ELogVerbosity::Warning;
		if (Entry.Time < Since || (!bAll && !bError && !(bWarnings && bWarning)))
		{
			continue;
		}
		if (!Category.IsEmpty() && !Entry.Category.ToString().StartsWith(Category))
		{
			continue;
		}
		Out.Insert(Entry, 0);
	}
	return Out;
}
