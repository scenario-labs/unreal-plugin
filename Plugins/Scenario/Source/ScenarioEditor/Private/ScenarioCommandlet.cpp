// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioCommandlet.h"

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "ScenarioActions.h"
#include "ScenarioCU.h"
#include "ScenarioJson.h"
#include "ScenarioSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogScenarioCommandlet, Display, All);

namespace
{
	/** Pumps the game thread (tickers, HTTP, queued tasks) until Done or the deadline. */
	bool Pump(const TFunction<bool()>& Done, double Seconds)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		double Last = FPlatformTime::Seconds();
		while (!Done())
		{
			const double Now = FPlatformTime::Seconds();
			if (Now >= Deadline)
			{
				return false;
			}
			const float Delta = static_cast<float>(Now - Last);
			Last = Now;
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FHttpModule::Get().GetHttpManager().Tick(Delta);
			FTSTicker::GetCoreTicker().Tick(Delta);
			FPlatformProcess::Sleep(0.02f);
		}
		return true;
	}

	/** Runs one action to completion. Prints its result line. */
	bool RunAction(const FString& Name, const TSharedPtr<FJsonObject>& Args, FScenarioActionResult& Out, double Seconds = 180.0)
	{
		bool bFinished = false;
		FScenarioActions::Get().Call(Name, Args, FScenarioCaller::Script(TEXT("commandlet")), [&bFinished, &Out](const FScenarioActionResult& Result)
		{
			Out = Result;
			bFinished = true;
		});
		if (!Pump([&bFinished]() { return bFinished; }, Seconds))
		{
			Out = FScenarioActionResult::Fail(TEXT("Timeout"), FString::Printf(TEXT("%s did not finish in %.0f s."), *Name, Seconds));
		}
		TSharedRef<FJsonObject> Line = MakeShared<FJsonObject>();
		Line->SetStringField(TEXT("step"), Name);
		Line->SetBoolField(TEXT("ok"), Out.bOk);
		if (!Out.bOk)
		{
			Line->SetStringField(TEXT("error"), Out.Error);
		}
		if (Out.Data.IsValid())
		{
			Line->SetObjectField(TEXT("result"), Out.Data);
		}
		UE_LOG(LogScenarioCommandlet, Display, TEXT("%s"), *ScenarioJson::Write(Line));
		return Out.bOk;
	}

	/** -Params=@file.json or inline JSON. */
	TSharedPtr<FJsonObject> ReadJsonParam(const FString& Params, const TCHAR* Name)
	{
		FString Value;
		if (!FParse::Value(*Params, Name, Value, false))
		{
			return MakeShared<FJsonObject>();
		}
		if (Value.StartsWith(TEXT("@")))
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Value.RightChop(1)))
			{
				return nullptr;
			}
			Value = Text;
		}
		return ScenarioJson::ParseObject(Value);
	}

	bool WaitReady()
	{
		UScenarioSubsystem* S = UScenarioSubsystem::Get();
		if (!S)
		{
			UE_LOG(LogScenarioCommandlet, Error, TEXT("The Scenario editor subsystem is not running."));
			return false;
		}
		FScenarioSession& Session = S->Session();
		Pump([&Session]() { return Session.IsReady() || (Session.GetConnection() == EScenarioConnection::SignedOut && Session.GetLastError().Kind != EScenarioErrorKind::None); }, 60.0);
		if (!Session.IsReady())
		{
			UE_LOG(LogScenarioCommandlet, Error, TEXT("Not signed in to Scenario: set SCENARIO_API_KEY and SCENARIO_API_SECRET. %s"), *Session.GetLastError().ToString());
			return false;
		}
		return true;
	}
}

UScenarioCommandlet::UScenarioCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UScenarioCommandlet::Main(const FString& Params)
{
	if (!WaitReady())
	{
		return 1;
	}

	FString ActionName;
	if (FParse::Value(*Params, TEXT("-Action="), ActionName))
	{
		const TSharedPtr<FJsonObject> Args = ReadJsonParam(Params, TEXT("-Args="));
		if (!Args.IsValid())
		{
			UE_LOG(LogScenarioCommandlet, Error, TEXT("-Args is not a JSON object."));
			return 1;
		}
		FScenarioActionResult Result;
		return RunAction(ActionName, Args, Result) ? 0 : 1;
	}

	if (!FParse::Param(*Params, TEXT("Generate")))
	{
		UE_LOG(LogScenarioCommandlet, Display, TEXT("Usage: -run=ScenarioEditor.Scenario -Action=<name> [-Args=<json|@file>], or -Generate -Model=<id> -Params=<json|@file> -MaxCU=<n> [-Destination=/Game/...] [-Import] [-Save]"));
		return 1;
	}

	FString ModelId;
	FString MaxText;
	double MaxCU = 0.0;
	if (!FParse::Value(*Params, TEXT("-Model="), ModelId) || !FParse::Value(*Params, TEXT("-MaxCU="), MaxText) || !ScenarioCU::Parse(MaxText, MaxCU))
	{
		UE_LOG(LogScenarioCommandlet, Error, TEXT("-Generate needs -Model and -MaxCU (the most you accept to spend on this run)."));
		return 1;
	}
	const TSharedPtr<FJsonObject> Parameters = ReadJsonParam(Params, TEXT("-Params="));
	if (!Parameters.IsValid())
	{
		UE_LOG(LogScenarioCommandlet, Error, TEXT("-Params is not a JSON object."));
		return 1;
	}

	// 1. Price
	TSharedRef<FJsonObject> EstimateArgs = MakeShared<FJsonObject>();
	EstimateArgs->SetStringField(TEXT("model_id"), ModelId);
	EstimateArgs->SetObjectField(TEXT("parameters"), Parameters);
	FScenarioActionResult Quote;
	if (!RunAction(TEXT("estimate_cost"), EstimateArgs, Quote))
	{
		return 1;
	}
	const FString Exact = ScenarioJson::GetString(Quote.Data, TEXT("cu_cost_exact"));
	double Cost = 0.0;
	if (!ScenarioCU::Parse(Exact, Cost) || Cost > MaxCU)
	{
		UE_LOG(LogScenarioCommandlet, Error, TEXT("The price is %s CU, above -MaxCU=%s. Nothing was sent."), *Exact, *MaxText);
		return 1;
	}

	// 2. Run
	TSharedRef<FJsonObject> RunArgs = MakeShared<FJsonObject>();
	RunArgs->SetStringField(TEXT("quote_id"), ScenarioJson::GetString(Quote.Data, TEXT("quote_id")));
	RunArgs->SetStringField(TEXT("approved_cost"), Exact);
	FString Destination;
	if (FParse::Value(*Params, TEXT("-Destination="), Destination))
	{
		RunArgs->SetStringField(TEXT("destination"), Destination);
	}
	FScenarioActionResult Run;
	if (!RunAction(TEXT("generate"), RunArgs, Run))
	{
		return 1;
	}
	const FString JobId = ScenarioJson::GetString(Run.Data, TEXT("job_id"));

	// 3. Wait (several waits for long jobs; the job is never sent twice)
	TSharedRef<FJsonObject> WaitArgs = MakeShared<FJsonObject>();
	WaitArgs->SetStringField(TEXT("job_id"), JobId);
	WaitArgs->SetNumberField(TEXT("timeout_seconds"), 160);
	FScenarioActionResult Wait;
	for (int32 Attempt = 0; Attempt < 8; ++Attempt)
	{
		if (!RunAction(TEXT("wait_for_job"), WaitArgs, Wait, 175.0))
		{
			return 1;
		}
		if (!ScenarioJson::GetBool(Wait.Data, TEXT("still_running")))
		{
			break;
		}
	}
	if (!ScenarioJson::GetBool(Wait.Data, TEXT("delivered")))
	{
		UE_LOG(LogScenarioCommandlet, Error, TEXT("Job %s ended as %s."), *JobId, *ScenarioJson::GetString(Wait.Data, TEXT("state")));
		return 1;
	}

	// 4. Import, 5. Save
	if (FParse::Param(*Params, TEXT("Import")))
	{
		TSharedRef<FJsonObject> PrepareArgs = MakeShared<FJsonObject>();
		PrepareArgs->SetStringField(TEXT("job_id"), JobId);
		PrepareArgs->SetStringField(TEXT("purpose"), TEXT("import"));
		FScenarioActionResult Prepared;
		if (!RunAction(TEXT("prepare_result_application"), PrepareArgs, Prepared))
		{
			return 1;
		}
		TSharedRef<FJsonObject> ApplyArgs = MakeShared<FJsonObject>();
		ApplyArgs->SetStringField(TEXT("application_id"), ScenarioJson::GetString(Prepared.Data, TEXT("application_id")));
		FScenarioActionResult Applied;
		if (!RunAction(TEXT("apply_result_application"), ApplyArgs, Applied, 300.0))
		{
			return 1;
		}
		if (FParse::Param(*Params, TEXT("Save")))
		{
			FScenarioActionResult Saved;
			RunAction(TEXT("save_changes"), MakeShared<FJsonObject>(), Saved);
		}
	}
	return 0;
}
