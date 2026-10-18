// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/** Recent warnings and errors (and Scenario and Interchange messages) for the output_log tool. Any thread. */
class FScenarioOutputLog : public FOutputDevice
{
public:
	struct FEntry
	{
		double Time = 0.0;
		ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
		FName Category;
		FString Text;
	};

	static FScenarioOutputLog& Get();
	void Register();
	void Unregister();

	/** Newest last. bWarnings includes warnings with errors; Category filters by prefix. */
	TArray<FEntry> Recent(bool bWarnings, bool bAll, const FString& Category, int32 Limit, double SinceSeconds) const;

	virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

private:
	static constexpr int32 MaxEntries = 1000;
	mutable FCriticalSection Lock;
	TArray<FEntry> Entries;
	int32 Next = 0;
	bool bRegistered = false;
};
