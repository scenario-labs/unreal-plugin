// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioToolset.h"

#if WITH_SCENARIO_TOOLSETS

#include "Containers/Ticker.h"
#include "ScenarioActions.h"
#include "ScenarioJson.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ScenarioToolset)

namespace
{
	/** Arguments for one registry action, by its snake_case names. Empty strings are left out. */
	struct FArgs
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		FString Error;

		FArgs& Str(const TCHAR* Key, const FString& Value)
		{
			const FString Trimmed = Value.TrimStartAndEnd();
			if (!Trimmed.IsEmpty())
			{
				Json->SetStringField(Key, Trimmed);
			}
			return *this;
		}

		/**
		 * An optional value whose default is a word (all, auto, default, *): Epic's registry fills
		 * a left-out parameter only from its default, and an empty default makes it required.
		 */
		FArgs& Opt(const TCHAR* Key, const FString& Value, const TCHAR* Unset)
		{
			const FString Trimmed = Value.TrimStartAndEnd();
			return Trimmed.Equals(Unset, ESearchCase::IgnoreCase) ? *this : Str(Key, Trimmed);
		}

		FArgs& Num(const TCHAR* Key, int32 Value)
		{
			Json->SetNumberField(Key, Value);
			return *this;
		}

		FArgs& Bool(const TCHAR* Key, bool bValue)
		{
			Json->SetBoolField(Key, bValue);
			return *this;
		}

		/** JSON object text from the caller; anything else is a caller error. */
		FArgs& Object(const TCHAR* Key, const FString& Text, const TCHAR* ToolParam)
		{
			const FString Trimmed = Text.TrimStartAndEnd();
			if (Trimmed.IsEmpty() || Trimmed == TEXT("{}"))
			{
				return *this;
			}
			const TSharedPtr<FJsonObject> Parsed = ScenarioJson::ParseObject(Trimmed);
			if (!Parsed.IsValid())
			{
				Error = FString::Printf(TEXT("Invalid: %s must be a JSON object as text, for example {\"prompt\": \"a stone well\"}."), ToolParam);
				return *this;
			}
			Json->SetObjectField(Key, Parsed);
			return *this;
		}

		FArgs& List(const TCHAR* Key, const FString& CommaSeparated, const TCHAR* Unset)
		{
			if (CommaSeparated.TrimStartAndEnd().Equals(Unset, ESearchCase::IgnoreCase))
			{
				return *this;
			}
			TArray<FString> Parts;
			CommaSeparated.ParseIntoArray(Parts, TEXT(","), true);
			TArray<FString> Clean;
			for (const FString& Part : Parts)
			{
				const FString Trimmed = Part.TrimStartAndEnd();
				if (!Trimmed.IsEmpty())
				{
					Clean.Add(Trimmed);
				}
			}
			if (Clean.Num() > 0)
			{
				Json->SetField(Key, ScenarioJson::StringArrayValue(Clean));
			}
			return *this;
		}
	};

	bool IsWordChar(TCHAR Char)
	{
		return FChar::IsAlnum(Char) || Char == TEXT('_');
	}

	/**
	 * The registry actions' texts name their own tools (wait_for_job, prepare_result_application...).
	 * Agents here see the toolset's names, so the texts use those.
	 */
	FString WithToolNames(const FString& Text)
	{
		static const TArray<TPair<FString, FString>> Names = []()
		{
			TArray<TPair<FString, FString>> Pairs;
			for (const TPair<FString, FString>& Pair : UScenarioToolset::ActionsByTool())
			{
				Pairs.Emplace(Pair.Value, Pair.Key);
			}
			Pairs.Emplace(TEXT("list_assets"), TEXT("SearchAssets"));
			Pairs.Emplace(TEXT("recover_local_job"), TEXT("CheckUnknownJob"));
			// Longest first, so no name replaces part of a longer one.
			Pairs.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B) { return A.Key.Len() > B.Key.Len(); });
			return Pairs;
		}();
		FString Out;
		Out.Reserve(Text.Len());
		int32 Index = 0;
		while (Index < Text.Len())
		{
			bool bReplaced = false;
			if (Text[Index] >= TEXT('a') && Text[Index] <= TEXT('z') && (Index == 0 || !IsWordChar(Text[Index - 1])))
			{
				for (const TPair<FString, FString>& Pair : Names)
				{
					const int32 End = Index + Pair.Key.Len();
					if (End <= Text.Len() && FCString::Strncmp(*Text + Index, *Pair.Key, Pair.Key.Len()) == 0 && (End == Text.Len() || !IsWordChar(Text[End])))
					{
						Out += Pair.Value;
						Index = End;
						bReplaced = true;
						break;
					}
				}
			}
			if (!bReplaced)
			{
				Out.AppendChar(Text[Index]);
				++Index;
			}
		}
		return Out;
	}

	/** Runs one registry action as the toolset's caller and completes the result once. */
	UScenarioToolResult* Run(const FString& ActionName, const FArgs& Args)
	{
		UScenarioToolResult* Result = NewObject<UScenarioToolResult>();
		if (!Args.Error.IsEmpty())
		{
			Result->SetError(Args.Error);
			return Result;
		}
		const FScenarioAction* Action = FScenarioActions::Get().Find(ActionName);
		if (!Action)
		{
			Result->SetError(FString::Printf(TEXT("NotFound: the Scenario plugin has no action %s."), *ActionName));
			return Result;
		}
		const TStrongObjectPtr<UScenarioToolResult> Keep(Result);
		const TSharedRef<bool> bAnswered = MakeShared<bool>(false);
		const TSharedRef<FTSTicker::FDelegateHandle> Timer = MakeShared<FTSTicker::FDelegateHandle>();
		const double Limit = Action->TimeLimit;
		*Timer = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Keep, bAnswered, ActionName, Limit](float)
		{
			if (!*bAnswered)
			{
				*bAnswered = true;
				Keep->SetError(WithToolNames(FString::Printf(TEXT("Timeout: %s took longer than %.0f s. It may still finish: check job_status or list_local_jobs before trying again."), *ActionName, Limit)));
			}
			return false;
		}), static_cast<float>(Limit));

		FScenarioActions::Get().Call(ActionName, Args.Json, UScenarioToolset::Caller(), [Keep, bAnswered, Timer](const FScenarioActionResult& Answer)
		{
			FTSTicker::GetCoreTicker().RemoveTicker(*Timer);
			if (*bAnswered)
			{
				return;
			}
			*bAnswered = true;
			if (Answer.bOk)
			{
				Keep->SetValue(WithToolNames(ScenarioJson::Write(Answer.Data.IsValid() ? Answer.Data.ToSharedRef() : MakeShared<FJsonObject>())));
				return;
			}
			FString Error = Answer.Error.IsEmpty() ? FString(TEXT("Error: the tool failed without a message.")) : Answer.Error;
			if (Answer.Data.IsValid())
			{
				Error += TEXT("\n") + ScenarioJson::Write(Answer.Data.ToSharedRef());
			}
			Keep->SetError(WithToolNames(Error));
		});
		return Result;
	}
}

TSharedPtr<FJsonValue> UScenarioToolResult::GetValueAsJson() const
{
	if (!bIsComplete || !Error.IsEmpty())
	{
		return nullptr;
	}
	const TSharedPtr<FJsonObject> Object = ScenarioJson::ParseObject(Value);
	if (Object.IsValid())
	{
		return MakeShared<FJsonValueObject>(Object);
	}
	return MakeShared<FJsonValueString>(Value);
}

const TMap<FString, FString>& UScenarioToolset::ActionsByTool()
{
	static const TMap<FString, FString> Map = {
		{ TEXT("GetStatus"), TEXT("scenario_status") },
		{ TEXT("ListModels"), TEXT("list_models") },
		{ TEXT("GetModelSchema"), TEXT("model_schema") },
		{ TEXT("RecommendModels"), TEXT("recommend_models") },
		{ TEXT("EstimateCost"), TEXT("estimate_cost") },
		{ TEXT("Generate"), TEXT("generate") },
		{ TEXT("ListWorkflows"), TEXT("list_workflows") },
		{ TEXT("GetWorkflowSchema"), TEXT("workflow_schema") },
		{ TEXT("EstimateWorkflow"), TEXT("estimate_workflow") },
		{ TEXT("RunWorkflow"), TEXT("run_workflow") },
		{ TEXT("GetJobStatus"), TEXT("job_status") },
		{ TEXT("WaitForJob"), TEXT("wait_for_job") },
		{ TEXT("ListJobs"), TEXT("list_local_jobs") },
		{ TEXT("CancelJob"), TEXT("cancel_job") },
		{ TEXT("CheckUnknownJob"), TEXT("recover_local_job") },
		{ TEXT("SearchAssets"), TEXT("search_assets") },
		{ TEXT("PrepareResult"), TEXT("prepare_result_application") },
		{ TEXT("ApplyResult"), TEXT("apply_result_application") },
		{ TEXT("SaveChanges"), TEXT("save_changes") },
	};
	return Map;
}

FScenarioCaller UScenarioToolset::Caller()
{
	// Epic's endpoint has no token: any program on this computer can call it. Reads and prices
	// are free; every paid call waits for the person in the editor, whatever the Agent page says.
	FScenarioCaller Caller = FScenarioCaller::Agent(TEXT("Toolset Registry"));
	Caller.bSpendWaitsForApproval = true;
	// Python and removals are not in this toolset; keep them off even if a tool ever reached them.
	Caller.bCanRunPython = false;
	return Caller;
}

UScenarioToolResult* UScenarioToolset::GetStatus()
{
	return Run(TEXT("scenario_status"), FArgs());
}

UScenarioToolResult* UScenarioToolset::ListModels(const FString& Lane, const FString& Modality, const FString& Query, int32 Limit)
{
	return Run(TEXT("list_models"), FArgs().Opt(TEXT("lane"), Lane, TEXT("all")).Opt(TEXT("modality"), Modality, TEXT("all")).Opt(TEXT("query"), Query, TEXT("*")).Num(TEXT("limit"), Limit));
}

UScenarioToolResult* UScenarioToolset::GetModelSchema(const FString& ModelId)
{
	return Run(TEXT("model_schema"), FArgs().Str(TEXT("model_id"), ModelId));
}

UScenarioToolResult* UScenarioToolset::RecommendModels(const FString& Prompt, const FString& Lane)
{
	return Run(TEXT("recommend_models"), FArgs().Str(TEXT("prompt"), Prompt).Opt(TEXT("lane"), Lane, TEXT("auto")));
}

UScenarioToolResult* UScenarioToolset::EstimateCost(const FString& ModelId, const FString& Lane, const FString& Parameters)
{
	return Run(TEXT("estimate_cost"), FArgs().Str(TEXT("model_id"), ModelId).Opt(TEXT("lane"), Lane, TEXT("auto")).Object(TEXT("parameters"), Parameters, TEXT("Parameters")));
}

UScenarioToolResult* UScenarioToolset::Generate(const FString& QuoteId, const FString& ApprovedCost, const FString& Destination)
{
	return Run(TEXT("generate"), FArgs().Str(TEXT("quote_id"), QuoteId).Str(TEXT("approved_cost"), ApprovedCost).Opt(TEXT("destination"), Destination, TEXT("default")));
}

UScenarioToolResult* UScenarioToolset::ListWorkflows(const FString& Source, int32 Limit)
{
	return Run(TEXT("list_workflows"), FArgs().Str(TEXT("source"), Source).Num(TEXT("limit"), Limit));
}

UScenarioToolResult* UScenarioToolset::GetWorkflowSchema(const FString& WorkflowId)
{
	return Run(TEXT("workflow_schema"), FArgs().Str(TEXT("workflow_id"), WorkflowId));
}

UScenarioToolResult* UScenarioToolset::EstimateWorkflow(const FString& WorkflowId, const FString& Inputs)
{
	return Run(TEXT("estimate_workflow"), FArgs().Str(TEXT("workflow_id"), WorkflowId).Object(TEXT("inputs"), Inputs, TEXT("Inputs")));
}

UScenarioToolResult* UScenarioToolset::RunWorkflow(const FString& QuoteId, const FString& ApprovedCost, const FString& Destination, const FString& WorkflowName)
{
	return Run(TEXT("run_workflow"), FArgs().Str(TEXT("quote_id"), QuoteId).Str(TEXT("approved_cost"), ApprovedCost).Opt(TEXT("destination"), Destination, TEXT("default")).Opt(TEXT("workflow_name"), WorkflowName, TEXT("auto")));
}

UScenarioToolResult* UScenarioToolset::GetJobStatus(const FString& JobId)
{
	return Run(TEXT("job_status"), FArgs().Str(TEXT("job_id"), JobId));
}

UScenarioToolResult* UScenarioToolset::WaitForJob(const FString& JobId, int32 TimeoutSeconds)
{
	return Run(TEXT("wait_for_job"), FArgs().Str(TEXT("job_id"), JobId).Num(TEXT("timeout_seconds"), TimeoutSeconds));
}

UScenarioToolResult* UScenarioToolset::ListJobs(const FString& State, int32 Limit)
{
	return Run(TEXT("list_local_jobs"), FArgs().Str(TEXT("state"), State).Num(TEXT("limit"), Limit));
}

UScenarioToolResult* UScenarioToolset::CancelJob(const FString& JobId)
{
	return Run(TEXT("cancel_job"), FArgs().Str(TEXT("job_id"), JobId));
}

UScenarioToolResult* UScenarioToolset::CheckUnknownJob(const FString& JobId)
{
	return Run(TEXT("recover_local_job"), FArgs().Opt(TEXT("job_id"), JobId, TEXT("all")).Str(TEXT("action"), TEXT("check")));
}

UScenarioToolResult* UScenarioToolset::SearchAssets(const FString& Query, const FString& Kind, int32 Limit, int32 Offset)
{
	const bool bNewest = Query.TrimStartAndEnd().IsEmpty() || Query.TrimStartAndEnd() == TEXT("*");
	const FArgs Args = FArgs().Opt(TEXT("query"), Query, TEXT("*")).Opt(TEXT("kind"), Kind, TEXT("all")).Num(TEXT("limit"), Limit).Num(TEXT("offset"), Offset);
	return Run(bNewest ? TEXT("list_assets") : TEXT("search_assets"), Args);
}

UScenarioToolResult* UScenarioToolset::PrepareResult(const FString& JobId, const FString& Purpose, const FString& Placement, const FString& Location, const FString& ActorIds, int32 Slot)
{
	FArgs Args;
	Args.Str(TEXT("job_id"), JobId).Str(TEXT("purpose"), Purpose).Str(TEXT("placement"), Placement).List(TEXT("actor_ids"), ActorIds, TEXT("selection")).Num(TEXT("slot"), Slot);
	if (Placement.TrimStartAndEnd().Equals(TEXT("at_location"), ESearchCase::IgnoreCase))
	{
		TArray<FString> Parts;
		Location.ParseIntoArray(Parts, TEXT(","), true);
		TArray<TSharedPtr<FJsonValue>> Xyz;
		for (const FString& Part : Parts)
		{
			const FString Trimmed = Part.TrimStartAndEnd();
			if (Trimmed.IsNumeric())
			{
				Xyz.Add(MakeShared<FJsonValueNumber>(FCString::Atod(*Trimmed)));
			}
		}
		if (Xyz.Num() != 3 || Parts.Num() != 3)
		{
			UScenarioToolResult* Result = NewObject<UScenarioToolResult>();
			Result->SetError(TEXT("Invalid: placement at_location needs Location as x,y,z in cm, for example 0,250,100."));
			return Result;
		}
		Args.Json->SetArrayField(TEXT("location"), Xyz);
	}
	return Run(TEXT("prepare_result_application"), Args);
}

UScenarioToolResult* UScenarioToolset::ApplyResult(const FString& ApplicationId)
{
	return Run(TEXT("apply_result_application"), FArgs().Str(TEXT("application_id"), ApplicationId));
}

UScenarioToolResult* UScenarioToolset::SaveChanges(bool bIncludeLevels)
{
	return Run(TEXT("save_changes"), FArgs().Bool(TEXT("include_levels"), bIncludeLevels));
}

#endif
