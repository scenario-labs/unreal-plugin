// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioScriptLibrary.h"

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
