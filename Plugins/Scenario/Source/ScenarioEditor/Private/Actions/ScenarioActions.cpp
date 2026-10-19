// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioActions.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "ScenarioActionHelpers.h"
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
	ScenarioActionHelpers::Uploads().Reset();
	ScenarioActionHelpers::Applications().Reset();
	ScenarioActionHelpers::References().Reset();
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
		Finish(FScenarioActionResult::Fail(TEXT("Permission"), TEXT("Changing the project is off for agents. Use prepare_result_application and apply_result_application: the person gets a proposal to apply, or can turn on \"Allow agents to change this project\".")));
		return;
	}

	const auto Run = Action->Run;
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

	void AskPerson(const FText& Title, const FText& Detail, double Timeout, TFunction<void(bool)> OnAnswer)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OnAnswer(false);
			return;
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
			return;
		}
		State->Item = Item;
		State->Timer = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Answer](float)
		{
			(*Answer)(false);
			return false;
		}), static_cast<float>(Timeout));
	}

	FString NewId(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower());
	}
}

#undef LOCTEXT_NAMESPACE
