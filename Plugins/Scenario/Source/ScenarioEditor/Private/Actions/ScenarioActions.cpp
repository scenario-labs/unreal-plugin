// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioActions.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "ScenarioActionHelpers.h"
#include "ScenarioCatalog.h"
#include "ScenarioCU.h"
#include "ScenarioEditorModule.h"
#include "ScenarioErrors.h"
#include "ScenarioSchema.h"
#include "ScenarioSettings.h"
#include "ScenarioStyle.h"
#include "SScenarioToast.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "ScenarioActions"

FScenarioCaller FScenarioCaller::Agent(const FString& Client)
{
	const UScenarioEditorSettings& Settings = UScenarioEditorSettings::Get();
	FScenarioCaller Caller;
	Caller.Source = TEXT("agent");
	Caller.Client = Client;
	Caller.bCanChangeProject = Settings.bAllowAgentChanges;
	Caller.bCanRunPython = Settings.bAllowAgentChanges && Settings.bAllowAgentPython;
	return Caller;
}

FScenarioCaller FScenarioCaller::Script(const FString& Client)
{
	FScenarioCaller Caller;
	Caller.Source = TEXT("script");
	Caller.Client = Client;
	// Scripts are the person's own code: they may change the project, never run agent Python.
	Caller.bCanChangeProject = true;
	return Caller;
}

FScenarioActionResult FScenarioActionResult::Ok(const TSharedPtr<FJsonObject>& InData)
{
	FScenarioActionResult Result;
	Result.bOk = true;
	Result.Data = InData.IsValid() ? InData : MakeShared<FJsonObject>();
	return Result;
}

FScenarioActionResult FScenarioActionResult::Fail(const FString& Type, const FString& Message)
{
	FScenarioActionResult Result;
	Result.Error = FString::Printf(TEXT("%s: %s"), *Type, *Message);
	return Result;
}

FScenarioActionResult FScenarioActionResult::Fail(const FScenarioError& Error)
{
	FScenarioActionResult Result;
	Result.Error = Error.ToString();
	return Result;
}

FScenarioActions& FScenarioActions::Get()
{
	static FScenarioActions Instance;
	return Instance;
}

FScenarioActions::FScenarioActions()
{
	RegisterScenarioActions();
	RegisterLevelActions();
	RegisterKitActions();
}

void FScenarioActions::Register(FScenarioAction Action)
{
	if (const int32* Existing = IndexByName.Find(Action.Name))
	{
		Actions[*Existing] = MoveTemp(Action);
		return;
	}
	IndexByName.Add(Action.Name, Actions.Num());
	Actions.Add(MoveTemp(Action));
}

const FScenarioAction* FScenarioActions::Find(const FString& Name) const
{
	const int32* Index = IndexByName.Find(Name);
	return Index ? &Actions[*Index] : nullptr;
}

void FScenarioActions::Reset()
{
	TArray<FString> Pending;
	ScenarioActionHelpers::SpendApprovals().GetKeys(Pending);
	for (const FString& Id : Pending)
	{
		DeclineSpendApproval(Id);
	}
	ScenarioActionHelpers::SpendApprovals().Reset();
	ScenarioActionHelpers::Uploads().Reset();
	ScenarioActionHelpers::Applications().Reset();
	ScenarioActionHelpers::References().Reset();
}

bool FScenarioActions::DeclineSpendApproval(const FString& ApprovalId)
{
	ScenarioActionHelpers::FSpendApproval* Approval = ScenarioActionHelpers::SpendApprovals().Find(ApprovalId);
	if (!Approval || Approval->State != TEXT("pending"))
	{
		return false;
	}
	const TFunction<void()> Decline = Approval->Decline;
	if (Decline)
	{
		Decline();
	}
	// The question may already be gone (no UI): record the answer here too.
	if (ScenarioActionHelpers::FSpendApproval* After = ScenarioActionHelpers::SpendApprovals().Find(ApprovalId))
	{
		if (After->State == TEXT("pending"))
		{
			After->State = TEXT("declined");
		}
	}
	return true;
}

void FScenarioActions::ProposeSpend(const FString& Name, const FScenarioAction& Action, const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, FScenarioActionDone Finish)
{
	using namespace ScenarioActionHelpers;
	FScenarioActionResult Fail;
	UScenarioSubsystem* S = Subsystem(Fail, false);
	if (!S)
	{
		Finish(Fail);
		return;
	}
	FScenarioSession& Session = S->Session();
	if (Session.IsSpendingDisabled() && !bTestAskEvenWhenSpendingIsOff)
	{
		Finish(FScenarioActionResult::Fail(TEXT("Local"), TEXT("Spending is turned off (SCENARIO_NO_SPEND=1), so nothing was sent and the person was not asked.")));
		return;
	}

	// The quote rules hold before the person is asked: a doomed request never reaches them.
	const FString QuoteId = Str(Args, TEXT("quote_id"));
	const FString Approved = Str(Args, TEXT("approved_cost"));
	if (QuoteId.IsEmpty() || Approved.IsEmpty())
	{
		Finish(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("quote_id and approved_cost are required: price first, then pass the quote's cu_cost_exact.")));
		return;
	}
	const FScenarioQuote* Quote = Session.Quotes().FindById(QuoteId, ScenarioNow());
	if (!Quote)
	{
		Finish(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("This quote is unknown, used or expired (quotes last 120 seconds). Price the request again.")));
		return;
	}
	const EScenarioQuoteKind Expected = Name == TEXT("run_workflow") ? EScenarioQuoteKind::Workflow : Name == TEXT("approve_prompt") ? EScenarioQuoteKind::PromptSpark : EScenarioQuoteKind::Model;
	if (Quote->Kind != Expected)
	{
		const TCHAR* Right = Quote->Kind == EScenarioQuoteKind::Workflow ? TEXT("run_workflow") : Quote->Kind == EScenarioQuoteKind::PromptSpark ? TEXT("approve_prompt") : TEXT("generate");
		Finish(FScenarioActionResult::Fail(TEXT("Invalid"), FString::Printf(TEXT("This quote is for %s."), Right)));
		return;
	}
	if (Approved != Quote->Exact)
	{
		Finish(FScenarioActionResult::Fail(TEXT("Invalid"), FString::Printf(TEXT("approved_cost must be the quote's exact cost \"%s\"."), *Quote->Exact)));
		return;
	}

	// One question per quote: asking again returns the same approval.
	for (const TPair<FString, FSpendApproval>& Pair : SpendApprovals())
	{
		if (Pair.Value.QuoteId == QuoteId && (Pair.Value.State == TEXT("pending") || Pair.Value.State == TEXT("sending")))
		{
			TSharedRef<FJsonObject> Json = ApprovalJson(Pair.Value);
			Json->SetStringField(TEXT("state"), TEXT("pending_approval"));
			Finish(FScenarioActionResult::Ok(Json));
			return;
		}
	}

	FSpendApproval Approval;
	Approval.Id = NewId(TEXT("spend"));
	Approval.Action = Name;
	Approval.QuoteId = QuoteId;
	Approval.CostExact = Quote->Exact;
	if (const FScenarioCatalogModel* Model = Quote->Kind == EScenarioQuoteKind::Model ? Session.Catalog().Find(Quote->TargetId) : nullptr)
	{
		Approval.Target = Model->Summary.Name;
	}
	if (Approval.Target.IsEmpty())
	{
		Approval.Target = Quote->Kind == EScenarioQuoteKind::PromptSpark ? FString(TEXT("Prompt Spark")) : Quote->TargetId;
	}
	Approval.CreatedAt = ScenarioNow();
	// The person answers while the quote is still valid, or the request goes back to the agent.
	const double Timeout = FMath::Clamp(Quote->ExpiresAt() - Approval.CreatedAt, 1.0, 110.0);
	Approval.Deadline = Approval.CreatedAt + Timeout;
	const FString Id = Approval.Id;
	SpendApprovals().Add(Id, Approval);

	const auto Run = Action.Run;
	const FText Who = FText::FromString(Caller.Client.IsEmpty() ? TEXT("An agent") : Caller.Client);
	const FText Title = FText::Format(LOCTEXT("ProposeSpend", "{0} asks to spend {1} CU"), Who, FText::FromString(Approval.CostExact));
	const FText Detail = FText::Format(LOCTEXT("ProposeSpendDetail", "{0} on {1}. Approve sends it to Scenario once. Decline, or no answer within {2} s, sends nothing."),
		FText::FromString(Name), FText::FromString(Approval.Target), FText::AsNumber(FMath::RoundToInt(Timeout)));
	const TFunction<void()> Decline = AskPerson(Title, Detail, Timeout, [this, Id, Name, Run, Args, Caller](bool bApproved)
	{
		FSpendApproval* Entry = SpendApprovals().Find(Id);
		if (!Entry || Entry->State != TEXT("pending"))
		{
			return;
		}
		if (!bApproved)
		{
			Entry->State = ScenarioNow() >= Entry->Deadline - 0.5 ? TEXT("expired") : TEXT("declined");
			return;
		}
		Entry->State = TEXT("sending");
		const double Started = FPlatformTime::Seconds();
		// Run once: the session sends a paid call once and never retries it.
		Run(Args, Caller, [this, Id, Name, Args, Caller, Started](const FScenarioActionResult& Result)
		{
			if (FSpendApproval* Done = SpendApprovals().Find(Id))
			{
				Done->State = Result.bOk ? TEXT("sent") : TEXT("failed");
				Done->JobId = Result.bOk ? ScenarioJson::GetString(Result.Data, TEXT("job_id")) : FString();
				Done->Error = Result.Error;
			}
			OnCalled.Broadcast(Name, Args, Caller, Result, FPlatformTime::Seconds() - Started);
		});
	});
	if (FSpendApproval* Entry = SpendApprovals().Find(Id))
	{
		Entry->Decline = Decline;
		if (Entry->State != TEXT("pending"))
		{
			// The editor could not show the question (no UI): nothing is sent.
			Finish(FScenarioActionResult::Fail(TEXT("Declined"), TEXT("The editor cannot show the approval here (no UI), so nothing was sent.")));
			return;
		}
		TSharedRef<FJsonObject> Json = ApprovalJson(*Entry);
		Json->SetStringField(TEXT("state"), TEXT("pending_approval"));
		Finish(FScenarioActionResult::Ok(Json));
	}
}

void FScenarioActions::Call(const FString& Name, const TSharedPtr<FJsonObject>& InArgs, const FScenarioCaller& Caller, FScenarioActionDone Done)
{
	if (!IsInGameThread())
	{
		AsyncTask(ENamedThreads::GameThread, [this, Name, InArgs, Caller, Done = MoveTemp(Done)]() mutable
		{
			Call(Name, InArgs, Caller, MoveTemp(Done));
		});
		return;
	}

	const TSharedPtr<FJsonObject> Args = InArgs.IsValid() ? InArgs : MakeShared<FJsonObject>();
	const double Started = FPlatformTime::Seconds();
	TSharedRef<bool> bFinished = MakeShared<bool>(false);
	FScenarioActionDone Finish = [this, Name, Args, Caller, Started, bFinished, Done = MoveTemp(Done)](const FScenarioActionResult& Result)
	{
		if (*bFinished)
		{
			return;
		}
		*bFinished = true;
		OnCalled.Broadcast(Name, Args, Caller, Result, FPlatformTime::Seconds() - Started);
		Done(Result);
	};

	const FScenarioAction* Action = Find(Name);
	if (!Action || !Action->Run)
	{
		Finish(FScenarioActionResult::Fail(TEXT("NotFound"), FString::Printf(TEXT("No tool named %s."), *Name)));
		return;
	}

	if (Action->Kind == EScenarioActionKind::Python && !(Caller.bCanChangeProject && Caller.bCanRunPython))
	{
		Finish(FScenarioActionResult::Fail(TEXT("Permission"), TEXT("Running Python is off. The person can turn on \"Allow agents to run Python\" in Scenario > Connection > Agents.")));
		return;
	}
	if (Action->Kind == EScenarioActionKind::Change && !Caller.bCanChangeProject && !Action->bProposeWhenDenied)
	{
		Finish(FScenarioActionResult::Fail(TEXT("Permission"), TEXT("Changing the project is off for agents. Use prepare_result_application and apply_result_application: the person gets a proposal to approve, or can turn on \"Change this project\" on the Agent page.")));
		return;
	}

	const auto Run = Action->Run;
	if (Action->bCosts && Caller.bSpendWaitsForApproval)
	{
		ProposeSpend(Name, *Action, Args, Caller, Finish);
		return;
	}
	if (Action->bCosts && Caller.Source == TEXT("agent") && UScenarioEditorSettings::Get().bApproveAgentSpend)
	{
		const FText Title = FText::Format(LOCTEXT("ApproveSpend", "{0} wants to run {1}"), FText::FromString(Caller.Client.IsEmpty() ? TEXT("An agent") : Caller.Client), FText::FromString(Name));
		const FText Detail = FText::Format(LOCTEXT("ApproveSpendDetail", "Cost: {0} CU. Approve to send it to Scenario."), FText::FromString(ScenarioActionHelpers::Str(Args, TEXT("approved_cost"), TEXT("?"))));
		ScenarioActionHelpers::AskPerson(Title, Detail, 100.0, [Run, Args, Caller, Finish](bool bApproved)
		{
			if (!bApproved)
			{
				Finish(FScenarioActionResult::Fail(TEXT("Declined"), TEXT("The person did not approve this spend in the editor. Nothing was sent.")));
				return;
			}
			Run(Args, Caller, Finish);
		});
		return;
	}
	Run(Args, Caller, Finish);
}

namespace ScenarioActionHelpers
{
	TSharedPtr<FJsonObject> Object(const TArray<FProp>& Props, const TArray<FString>& Required)
	{
		TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		for (const FProp& Prop : Props)
		{
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			if (Prop.Type == TEXT("any"))
			{
				// No type: any JSON value.
			}
			else
			{
				P->SetStringField(TEXT("type"), Prop.Type);
			}
			if (!Prop.Description.IsEmpty())
			{
				P->SetStringField(TEXT("description"), Prop.Description);
			}
			if (Prop.Type == TEXT("array"))
			{
				TSharedRef<FJsonObject> Items = MakeShared<FJsonObject>();
				Items->SetStringField(TEXT("type"), Prop.Items.IsEmpty() ? TEXT("string") : Prop.Items);
				P->SetObjectField(TEXT("items"), Items);
			}
			if (Prop.Enum.Num() > 0)
			{
				P->SetField(TEXT("enum"), ScenarioJson::StringArrayValue(Prop.Enum));
			}
			Properties->SetObjectField(Prop.Name, P);
		}
		Schema->SetObjectField(TEXT("properties"), Properties);
		if (Required.Num() > 0)
		{
			Schema->SetField(TEXT("required"), ScenarioJson::StringArrayValue(Required));
		}
		return Schema;
	}

	FString Str(const TSharedPtr<FJsonObject>& Args, const FString& Name, const FString& Default)
	{
		const TSharedPtr<FJsonValue> Value = Args.IsValid() ? Args->TryGetField(Name) : nullptr;
		if (!Value.IsValid() || Value->IsNull())
		{
			return Default;
		}
		const FString Text = ScenarioJson::ValueText(Value).TrimStartAndEnd();
		return Text.IsEmpty() ? Default : Text;
	}

	int32 Int(const TSharedPtr<FJsonObject>& Args, const FString& Name, int32 Default, int32 Min, int32 Max)
	{
		double Value = Default;
		const FString Text = Str(Args, Name);
		if (!Text.IsEmpty() && !LexTryParseString(Value, *Text))
		{
			Value = Default;
		}
		return FMath::Clamp(static_cast<int32>(Value), Min, Max);
	}

	bool Bool(const TSharedPtr<FJsonObject>& Args, const FString& Name, bool Default)
	{
		const TSharedPtr<FJsonValue> Value = Args.IsValid() ? Args->TryGetField(Name) : nullptr;
		return Value.IsValid() && !Value->IsNull() ? ScenarioJson::Truthy(Value) : Default;
	}

	TArray<FString> Strings(const TSharedPtr<FJsonObject>& Args, const FString& Name)
	{
		TArray<FString> Out;
		const TSharedPtr<FJsonValue> Value = Args.IsValid() ? Args->TryGetField(Name) : nullptr;
		if (!Value.IsValid() || Value->IsNull())
		{
			return Out;
		}
		if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				const FString Text = ScenarioJson::ValueText(Item).TrimStartAndEnd();
				if (!Text.IsEmpty())
				{
					Out.Add(Text);
				}
			}
		}
		else
		{
			const FString Text = ScenarioJson::ValueText(Value).TrimStartAndEnd();
			if (!Text.IsEmpty())
			{
				Out.Add(Text);
			}
		}
		return Out;
	}

	TSharedPtr<FJsonObject> Obj(const TSharedPtr<FJsonObject>& Args, const FString& Name)
	{
		const TSharedPtr<FJsonValue> Value = Args.IsValid() ? Args->TryGetField(Name) : nullptr;
		if (!Value.IsValid())
		{
			return nullptr;
		}
		if (Value->Type == EJson::Object)
		{
			return Value->AsObject();
		}
		// Some clients send nested objects as JSON text.
		if (Value->Type == EJson::String)
		{
			return ScenarioJson::ParseObject(Value->AsString());
		}
		return nullptr;
	}

	UScenarioSubsystem* Subsystem(FScenarioActionResult& OutFail, bool bNeedsReady)
	{
		UScenarioSubsystem* S = UScenarioSubsystem::Get();
		if (!S)
		{
			OutFail = FScenarioActionResult::Fail(TEXT("Unavailable"), TEXT("The Scenario editor subsystem is not running."));
			return nullptr;
		}
		if (bNeedsReady && !S->Session().IsReady())
		{
			OutFail = S->Session().IsSignedIn()
				? FScenarioActionResult::Fail(TEXT("Auth"), TEXT("No Scenario project is chosen. Ask the person to choose one in the Scenario tab."))
				: FScenarioActionResult::Fail(TEXT("Auth"), TEXT("Not signed in to Scenario. Ask the person to sign in from the Scenario tab (toolbar > Scenario)."));
			return nullptr;
		}
		return S;
	}

	bool IsDelivered(const FScenarioJobRow& Row)
	{
		if (Row.State != EScenarioJobState::Success || Row.Files.Num() == 0)
		{
			return false;
		}
		for (const FScenarioJobFile& File : Row.Files)
		{
			if (File.LocalPath.IsEmpty() && File.Error.IsEmpty())
			{
				return false;
			}
		}
		return true;
	}

	FString ProjectRelative(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return Path;
		}
		FString Full = FPaths::ConvertRelativePathToFull(Path);
		const FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		if (FPaths::MakePathRelativeTo(Full, *Project) && !Full.StartsWith(TEXT("..")))
		{
			return Full;
		}
		return FPaths::GetCleanFilename(Path);
	}

	TSharedRef<FJsonObject> JobJson(const FScenarioJobRow& Row, bool bDetail)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("job_id"), Row.LocalId);
		if (!Row.JobId.IsEmpty())
		{
			Json->SetStringField(TEXT("scenario_job_id"), Row.JobId);
		}
		Json->SetStringField(TEXT("state"), LexToString(Row.State));
		Json->SetStringField(TEXT("kind"), Row.Kind);
		if (!Row.Lane.IsEmpty())
		{
			Json->SetStringField(TEXT("lane"), Row.Lane);
		}
		Json->SetStringField(Row.Kind == TEXT("workflow") ? TEXT("workflow_id") : TEXT("model_id"), Row.Kind == TEXT("workflow") ? Row.WorkflowId : Row.ModelId);
		Json->SetNumberField(TEXT("progress"), FMath::RoundToDouble(Row.Progress * 100.0) / 100.0);
		if (!Row.QuotedExact.IsEmpty())
		{
			Json->SetStringField(TEXT("quoted_cu"), Row.QuotedExact);
		}
		if (Row.ChargedCU >= 0.0)
		{
			Json->SetStringField(TEXT("charged_cu"), ScenarioCU::Exact(Row.ChargedCU));
		}
		Json->SetStringField(TEXT("created_at"), ScenarioIsoTime(Row.CreatedAt));
		Json->SetStringField(TEXT("source"), Row.Source);
		const bool bDelivered = IsDelivered(Row);
		Json->SetBoolField(TEXT("delivered"), bDelivered);
		Json->SetBoolField(TEXT("imported"), Row.bImported);
		if (!Row.Error.IsEmpty())
		{
			Json->SetStringField(TEXT("error"), Row.Error);
		}
		TArray<TSharedPtr<FJsonValue>> Files;
		for (int32 Index = 0; Index < Row.Files.Num(); ++Index)
		{
			const FScenarioJobFile& File = Row.Files[Index];
			TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetNumberField(TEXT("index"), Index);
			F->SetStringField(TEXT("asset_id"), File.AssetId);
			F->SetStringField(TEXT("kind"), File.Kind);
			if (!File.Role.IsEmpty())
			{
				F->SetStringField(TEXT("role"), File.Role);
			}
			F->SetBoolField(TEXT("downloaded"), !File.LocalPath.IsEmpty());
			if (File.ImportedObjects.Num() > 0)
			{
				F->SetField(TEXT("imported_objects"), ScenarioJson::StringArrayValue(File.ImportedObjects));
			}
			if (!File.Error.IsEmpty())
			{
				F->SetStringField(TEXT("error"), File.Error);
			}
			Files.Add(MakeShared<FJsonValueObject>(F));
		}
		if (Files.Num() > 0)
		{
			Json->SetArrayField(TEXT("files"), Files);
		}
		else if (Row.AssetIds.Num() > 0)
		{
			Json->SetField(TEXT("asset_ids"), ScenarioJson::StringArrayValue(Row.AssetIds));
		}

		FString Next;
		if (Row.IsUnresolved())
		{
			Next = TEXT("Unknown outcome: call recover_local_job with action \"check\". Do not send the same request again.");
		}
		else if (Row.IsActive() || (Row.State == EScenarioJobState::Success && !bDelivered))
		{
			Next = TEXT("Call wait_for_job.");
		}
		else if (bDelivered && !Row.bImported && Row.Kind != TEXT("prompt_spark"))
		{
			Next = TEXT("Call prepare_result_application, then apply_result_application.");
		}
		else if (Row.Kind == TEXT("prompt_spark") && Row.State == EScenarioJobState::Success)
		{
			Next = TEXT("Call read_prompt_result.");
		}
		if (!Next.IsEmpty())
		{
			Json->SetStringField(TEXT("next"), Next);
		}

		if (bDetail)
		{
			if (!Row.Prompt.IsEmpty())
			{
				Json->SetStringField(TEXT("prompt"), Row.Prompt);
			}
			if (Row.Parameters.IsValid())
			{
				Json->SetObjectField(TEXT("parameters"), Row.Parameters);
			}
			if (!Row.Destination.IsEmpty())
			{
				Json->SetStringField(TEXT("destination"), Row.Destination);
			}
			if (!Row.Client.IsEmpty())
			{
				Json->SetStringField(TEXT("client"), Row.Client);
			}
		}
		return Json;
	}

	TSharedRef<FJsonObject> FieldJson(const FScenarioField& Field)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Field.Name);
		Json->SetStringField(TEXT("type"), ScenarioSchema::TypeName(Field.Type));
		if (Field.bRequired)
		{
			Json->SetBoolField(TEXT("required"), true);
		}
		if (Field.Default.IsValid() && !Field.Default->IsNull())
		{
			Json->SetField(TEXT("default"), Field.Default);
		}
		if (Field.Options.Num() > 0)
		{
			Json->SetArrayField(TEXT("options"), Field.Options);
		}
		if (Field.Min.IsSet())
		{
			Json->SetNumberField(TEXT("min"), Field.Min.GetValue());
		}
		if (Field.Max.IsSet())
		{
			Json->SetNumberField(TEXT("max"), Field.Max.GetValue());
		}
		if (Field.IsFile())
		{
			Json->SetStringField(TEXT("accepts"), Field.Kind.IsEmpty() ? TEXT("asset id") : Field.Kind + TEXT(" asset id"));
			if (Field.Type == EScenarioFieldType::Files && Field.MaxItems > 0)
			{
				Json->SetNumberField(TEXT("max_items"), Field.MaxItems);
			}
		}
		if (Field.MaxLength > 0)
		{
			Json->SetNumberField(TEXT("max_length"), Field.MaxLength);
		}
		if (Field.bCostImpact)
		{
			Json->SetBoolField(TEXT("changes_price"), true);
		}
		if (!Field.Description.IsEmpty())
		{
			Json->SetStringField(TEXT("description"), Field.Description.Left(240));
		}
		return Json;
	}

	TMap<FString, FUpload>& Uploads()
	{
		static TMap<FString, FUpload> Map;
		return Map;
	}

	TMap<FString, FApplication>& Applications()
	{
		static TMap<FString, FApplication> Map;
		return Map;
	}

	TMap<FString, FString>& References()
	{
		static TMap<FString, FString> Map;
		return Map;
	}

	TMap<FString, FSpendApproval>& SpendApprovals()
	{
		static TMap<FString, FSpendApproval> Map;
		return Map;
	}

	TSharedRef<FJsonObject> ApprovalJson(const FSpendApproval& Approval)
	{
		if (Approval.State == TEXT("sent") && !Approval.JobId.IsEmpty())
		{
			UScenarioSubsystem* S = UScenarioSubsystem::Get();
			if (const FScenarioJobRow* Row = S ? S->Session().Ledger().Find(Approval.JobId) : nullptr)
			{
				TSharedRef<FJsonObject> Json = JobJson(*Row, false);
				Json->SetStringField(TEXT("approval_id"), Approval.Id);
				return Json;
			}
		}
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("approval_id"), Approval.Id);
		Json->SetStringField(TEXT("state"), Approval.State);
		Json->SetStringField(TEXT("tool"), Approval.Action);
		Json->SetStringField(TEXT("cu_cost_exact"), Approval.CostExact);
		Json->SetStringField(TEXT("target"), Approval.Target);
		if (!Approval.JobId.IsEmpty())
		{
			Json->SetStringField(TEXT("job_id"), Approval.JobId);
		}
		if (!Approval.Error.IsEmpty())
		{
			Json->SetStringField(TEXT("error"), Approval.Error);
		}
		FString Next;
		if (Approval.State == TEXT("pending"))
		{
			Next = FString::Printf(TEXT("The person sees Approve and Decline in the editor (%.0f s left). Call job_status with this approval_id later; do not call %s again."),
				FMath::Max(0.0, Approval.Deadline - ScenarioNow()), *Approval.Action);
		}
		else if (Approval.State == TEXT("sending"))
		{
			Next = TEXT("Approved and being sent: call job_status again.");
		}
		else if (Approval.State == TEXT("declined"))
		{
			Next = TEXT("The person declined. Nothing was sent. Do not retry; ask the person.");
		}
		else if (Approval.State == TEXT("expired"))
		{
			Next = TEXT("Nobody approved in time and the quote lapsed. Nothing was sent. Price again only if the person still wants it.");
		}
		else if (Approval.State == TEXT("failed"))
		{
			Next = TEXT("Approved but not confirmed. Check list_local_jobs before anything else; never send the same request again.");
		}
		if (!Next.IsEmpty())
		{
			Json->SetStringField(TEXT("next"), Next);
		}
		return Json;
	}

	TFunction<void()> AskPerson(const FText& Title, const FText& Detail, double Timeout, TFunction<void(bool)> OnAnswer)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OnAnswer(false);
			return []() {};
		}
		struct FState
		{
			bool bAnswered = false;
			TWeakPtr<SNotificationItem> Item;
			FTSTicker::FDelegateHandle Timer;
		};
		TSharedRef<FState> State = MakeShared<FState>();
		TSharedRef<TFunction<void(bool)>> Answer = MakeShared<TFunction<void(bool)>>([State, OnAnswer](bool bApproved)
		{
			if (State->bAnswered)
			{
				return;
			}
			State->bAnswered = true;
			FTSTicker::GetCoreTicker().RemoveTicker(State->Timer);
			if (const TSharedPtr<SNotificationItem> Pinned = State->Item.Pin())
			{
				Pinned->SetCompletionState(bApproved ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
				Pinned->ExpireAndFadeout();
			}
			OnAnswer(bApproved);
		});

		FScenarioToastContent Content;
		Content.Title = Title;
		Content.Detail = Detail;
		Content.CloseTooltip = LOCTEXT("DeclineClose", "Decline this spend and close.");
		Content.OnClosed = [Answer]() { (*Answer)(false); };
		FScenarioToastAction Approve;
		Approve.Label = LOCTEXT("Approve", "Approve");
		Approve.Run = [Answer]() { (*Answer)(true); };
		Approve.States = { SNotificationItem::CS_Pending };
		Approve.bCloses = false;
		FScenarioToastAction Decline = Approve;
		Decline.Label = LOCTEXT("Decline", "Decline");
		Decline.Run = [Answer]() { (*Answer)(false); };
		Content.Actions = { Approve, Decline };
		const TSharedPtr<SNotificationItem> Item = SScenarioToast::Show(Content, SNotificationItem::CS_Pending, 0.0f);
		if (!Item.IsValid())
		{
			(*Answer)(false);
			return []() {};
		}
		State->Item = Item;
		State->Timer = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Answer](float)
		{
			(*Answer)(false);
			return false;
		}), static_cast<float>(Timeout));
		return [Answer]() { (*Answer)(false); };
	}

	FString NewId(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower());
	}
}

#undef LOCTEXT_NAMESPACE
