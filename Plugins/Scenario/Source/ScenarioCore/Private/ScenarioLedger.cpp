// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioLedger.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScenarioApi.h"
#include "ScenarioCoreModule.h"
#include "ScenarioErrors.h"
#include "ScenarioJson.h"

namespace
{
	struct FStateName
	{
		EScenarioJobState State;
		const TCHAR* Name;
	};

	const FStateName StateNames[] = {
		{ EScenarioJobState::Submitting, TEXT("submitting") },
		{ EScenarioJobState::Queued, TEXT("queued") },
		{ EScenarioJobState::Running, TEXT("running") },
		{ EScenarioJobState::Success, TEXT("success") },
		{ EScenarioJobState::Failed, TEXT("failed") },
		{ EScenarioJobState::Blocked, TEXT("blocked") },
		{ EScenarioJobState::CancelRequested, TEXT("cancel_requested") },
		{ EScenarioJobState::Canceled, TEXT("canceled") },
		{ EScenarioJobState::Unknown, TEXT("unknown") },
		{ EScenarioJobState::Rejected, TEXT("rejected") },
		{ EScenarioJobState::Dismissed, TEXT("dismissed") },
	};

	TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Item : Items)
		{
			Values.Add(MakeShared<FJsonValueString>(Item));
		}
		return Values;
	}
}

const TCHAR* LexToString(EScenarioJobState State)
{
	for (const FStateName& Entry : StateNames)
	{
		if (Entry.State == State)
		{
			return Entry.Name;
		}
	}
	return TEXT("unknown");
}

EScenarioJobState ScenarioJobStateFromString(const FString& Text)
{
	for (const FStateName& Entry : StateNames)
	{
		if (Text == Entry.Name)
		{
			return Entry.State;
		}
	}
	return EScenarioJobState::Unknown;
}

bool FScenarioJobRow::IsActive() const
{
	return (State == EScenarioJobState::Queued || State == EScenarioJobState::Running || State == EScenarioJobState::CancelRequested) && !JobId.IsEmpty();
}

bool FScenarioJobRow::IsUnresolved() const
{
	return State == EScenarioJobState::Submitting || State == EScenarioJobState::Unknown;
}

bool FScenarioJobRow::IsFinal() const
{
	return State == EScenarioJobState::Success || State == EScenarioJobState::Failed || State == EScenarioJobState::Blocked
		|| State == EScenarioJobState::Canceled || State == EScenarioJobState::Rejected || State == EScenarioJobState::Dismissed;
}

TSharedRef<FJsonObject> FScenarioJobRow::ToJson() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("local_id"), LocalId);
	Object->SetStringField(TEXT("job_id"), JobId);
	Object->SetStringField(TEXT("state"), LexToString(State));
	Object->SetStringField(TEXT("kind"), Kind);
	Object->SetStringField(TEXT("lane"), Lane);
	Object->SetStringField(TEXT("model_id"), ModelId);
	Object->SetStringField(TEXT("model_name"), ModelName);
	Object->SetStringField(TEXT("workflow_id"), WorkflowId);
	Object->SetStringField(TEXT("prompt"), Prompt);
	Object->SetObjectField(TEXT("parameters"), Parameters.IsValid() ? Parameters : MakeShared<FJsonObject>());
	Object->SetArrayField(TEXT("input_asset_ids"), Strings(InputAssetIds));
	Object->SetStringField(TEXT("fingerprint"), Fingerprint);
	Object->SetNumberField(TEXT("quoted_cu"), QuotedCU);
	Object->SetStringField(TEXT("quoted_exact"), QuotedExact);
	if (ChargedCU >= 0.0)
	{
		Object->SetNumberField(TEXT("charged_cu"), ChargedCU);
	}
	else
	{
		Object->SetField(TEXT("charged_cu"), MakeShared<FJsonValueNull>());
	}
	Object->SetNumberField(TEXT("progress"), Progress);
	Object->SetArrayField(TEXT("asset_ids"), Strings(AssetIds));
	TArray<TSharedPtr<FJsonValue>> FileValues;
	for (const FScenarioJobFile& File : Files)
	{
		TSharedRef<FJsonObject> FileObject = MakeShared<FJsonObject>();
		FileObject->SetStringField(TEXT("asset_id"), File.AssetId);
		FileObject->SetStringField(TEXT("kind"), File.Kind);
		FileObject->SetStringField(TEXT("role"), File.Role);
		FileObject->SetStringField(TEXT("mime_type"), File.MimeType);
		FileObject->SetStringField(TEXT("local_path"), File.LocalPath);
		FileObject->SetNumberField(TEXT("size"), static_cast<double>(File.Size));
		FileObject->SetStringField(TEXT("sha256"), File.Sha256);
		FileObject->SetArrayField(TEXT("imported_objects"), Strings(File.ImportedObjects));
		FileObject->SetStringField(TEXT("error"), File.Error);
		FileValues.Add(MakeShared<FJsonValueObject>(FileObject));
	}
	Object->SetArrayField(TEXT("files"), FileValues);
	Object->SetBoolField(TEXT("imported"), bImported);
	Object->SetStringField(TEXT("source"), Source);
	Object->SetStringField(TEXT("client"), Client);
	Object->SetStringField(TEXT("team_id"), TeamId);
	Object->SetStringField(TEXT("project_id"), ProjectId);
	Object->SetStringField(TEXT("destination"), Destination);
	Object->SetStringField(TEXT("error"), Error);
	Object->SetNumberField(TEXT("created_at"), CreatedAt);
	Object->SetNumberField(TEXT("updated_at"), UpdatedAt);
	Object->SetNumberField(TEXT("revision"), Revision);
	return Object;
}

bool FScenarioJobRow::FromJson(const TSharedPtr<FJsonObject>& Object, FScenarioJobRow& Out)
{
	if (!Object.IsValid() || ScenarioJson::GetString(Object, TEXT("local_id")).IsEmpty())
	{
		return false;
	}
	Out = FScenarioJobRow();
	Out.LocalId = ScenarioJson::GetString(Object, TEXT("local_id"));
	Out.JobId = ScenarioJson::GetString(Object, TEXT("job_id"));
	Out.State = ScenarioJobStateFromString(ScenarioJson::GetString(Object, TEXT("state")));
	Out.Kind = ScenarioJson::GetString(Object, TEXT("kind"), TEXT("model"));
	Out.Lane = ScenarioJson::GetString(Object, TEXT("lane"));
	Out.ModelId = ScenarioJson::GetString(Object, TEXT("model_id"));
	Out.ModelName = ScenarioJson::GetString(Object, TEXT("model_name"));
	Out.WorkflowId = ScenarioJson::GetString(Object, TEXT("workflow_id"));
	Out.Prompt = ScenarioJson::GetString(Object, TEXT("prompt"));
	Out.Parameters = ScenarioJson::GetObject(Object, TEXT("parameters"));
	if (!Out.Parameters.IsValid())
	{
		Out.Parameters = MakeShared<FJsonObject>();
	}
	Out.InputAssetIds = ScenarioJson::GetStringArray(Object, TEXT("input_asset_ids"));
	Out.Fingerprint = ScenarioJson::GetString(Object, TEXT("fingerprint"));
	Out.QuotedCU = ScenarioJson::GetNumber(Object, TEXT("quoted_cu"));
	Out.QuotedExact = ScenarioJson::GetString(Object, TEXT("quoted_exact"));
	Out.ChargedCU = ScenarioJson::HasNumber(Object, TEXT("charged_cu")) ? ScenarioJson::GetNumber(Object, TEXT("charged_cu")) : -1.0;
	Out.Progress = ScenarioJson::GetNumber(Object, TEXT("progress"));
	Out.AssetIds = ScenarioJson::GetStringArray(Object, TEXT("asset_ids"));
	for (const TSharedPtr<FJsonValue>& Value : ScenarioJson::GetArray(Object, TEXT("files")))
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> FileObject = Value->AsObject();
		FScenarioJobFile File;
		File.AssetId = ScenarioJson::GetString(FileObject, TEXT("asset_id"));
		File.Kind = ScenarioJson::GetString(FileObject, TEXT("kind"));
		File.Role = ScenarioJson::GetString(FileObject, TEXT("role"));
		File.MimeType = ScenarioJson::GetString(FileObject, TEXT("mime_type"));
		File.LocalPath = ScenarioJson::GetString(FileObject, TEXT("local_path"));
		File.Size = static_cast<int64>(ScenarioJson::GetNumber(FileObject, TEXT("size")));
		File.Sha256 = ScenarioJson::GetString(FileObject, TEXT("sha256"));
		File.ImportedObjects = ScenarioJson::GetStringArray(FileObject, TEXT("imported_objects"));
		File.Error = ScenarioJson::GetString(FileObject, TEXT("error"));
		Out.Files.Add(MoveTemp(File));
	}
	Out.bImported = ScenarioJson::GetBool(Object, TEXT("imported"));
	Out.Source = ScenarioJson::GetString(Object, TEXT("source"), TEXT("person"));
	Out.Client = ScenarioJson::GetString(Object, TEXT("client"));
	Out.TeamId = ScenarioJson::GetString(Object, TEXT("team_id"));
	Out.ProjectId = ScenarioJson::GetString(Object, TEXT("project_id"));
	Out.Destination = ScenarioJson::GetString(Object, TEXT("destination"));
	Out.Error = ScenarioJson::GetString(Object, TEXT("error"));
	Out.CreatedAt = ScenarioJson::GetNumber(Object, TEXT("created_at"));
	Out.UpdatedAt = ScenarioJson::GetNumber(Object, TEXT("updated_at"));
	Out.Revision = static_cast<int32>(ScenarioJson::GetNumber(Object, TEXT("revision")));
	return true;
}

FScenarioLedger::FScenarioLedger(const FString& InPath)
	: Path(InPath)
{
}

void FScenarioLedger::Load()
{
	Jobs.Reset();
	if (Path.IsEmpty() || !FPaths::FileExists(Path))
	{
		return;
	}
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (FFileHelper::LoadFileToString(Text, *Path))
	{
		Root = ScenarioJson::ParseObject(Text);
	}
	if (!Root.IsValid())
	{
		const FString Aside = Path + TEXT(".unreadable");
		UE_LOG(LogScenario, Warning, TEXT("The Scenario job list could not be read and was set aside as %s."), *FPaths::GetCleanFilename(Aside));
		IFileManager::Get().Move(*Aside, *Path, true);
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : ScenarioJson::GetArray(Root, TEXT("jobs")))
	{
		FScenarioJobRow Row;
		if (Value.IsValid() && Value->Type == EJson::Object && FScenarioJobRow::FromJson(Value->AsObject(), Row))
		{
			if (Row.State == EScenarioJobState::Submitting)
			{
				Row.State = EScenarioJobState::Unknown;
				Row.Error = TEXT("The editor closed while this request was being sent. It may have started on Scenario.");
				++Row.Revision;
			}
			Jobs.Add(MoveTemp(Row));
		}
	}
}

bool FScenarioLedger::Save() const
{
	if (Path.IsEmpty())
	{
		return true;
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), FormatVersion);
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FScenarioJobRow& Row : Jobs)
	{
		Rows.Add(MakeShared<FJsonValueObject>(Row.ToJson()));
	}
	Root->SetArrayField(TEXT("jobs"), Rows);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	const FString Temp = Path + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(ScenarioJson::Write(Root, true), *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogScenario, Warning, TEXT("Could not write the Scenario job list."));
		return false;
	}
	// Move with replace is a rename on the same volume, so a crash leaves either the old or the new file.
	return IFileManager::Get().Move(*Path, *Temp, true);
}

FString FScenarioLedger::AddIntent(const FScenarioJobRow& Template, double Now)
{
	FScenarioJobRow Row = Template;
	++Counter;
	Row.LocalId = FString::Printf(TEXT("local_%lld_%d"), static_cast<long long>(Now * 1000.0), Counter);
	Row.JobId.Reset();
	Row.State = EScenarioJobState::Submitting;
	if (Row.Prompt.IsEmpty() && Row.Parameters.IsValid())
	{
		Row.Prompt = ScenarioJson::GetString(Row.Parameters, TEXT("prompt"));
	}
	if (!Row.Parameters.IsValid())
	{
		Row.Parameters = MakeShared<FJsonObject>();
	}
	Row.CreatedAt = Now;
	Row.UpdatedAt = Now;
	Row.Revision = 1;
	Jobs.Insert(MoveTemp(Row), 0);
	Trim();
	const FString LocalId = Jobs[0].LocalId;
	Save();
	OnChanged.Broadcast(LocalId);
	return LocalId;
}

void FScenarioLedger::Touch(FScenarioJobRow& Row, double Now)
{
	Row.UpdatedAt = Now;
	++Row.Revision;
	Save();
	OnChanged.Broadcast(Row.LocalId);
}

void FScenarioLedger::MarkSubmitted(const FString& LocalId, const FString& JobId, const FString& ServerStatus, double Now)
{
	if (FScenarioJobRow* Row = Find(LocalId))
	{
		Row->JobId = JobId;
		Row->State = StateForStatus(ServerStatus, FString());
		Row->Error.Reset();
		Touch(*Row, Now);
	}
}

void FScenarioLedger::MarkRejected(const FString& LocalId, const FString& Message, double Now)
{
	SetState(LocalId, EScenarioJobState::Rejected, Message, Now);
}

void FScenarioLedger::MarkUnknown(const FString& LocalId, const FString& Message, double Now)
{
	SetState(LocalId, EScenarioJobState::Unknown, Message, Now);
}

void FScenarioLedger::MarkCancelRequested(const FString& LocalId, double Now)
{
	const FScenarioJobRow* Row = Find(LocalId);
	if (Row && Row->IsActive())
	{
		SetState(LocalId, EScenarioJobState::CancelRequested, FString(), Now);
	}
}

void FScenarioLedger::Dismiss(const FString& LocalId, double Now)
{
	SetState(LocalId, EScenarioJobState::Dismissed, FString(), Now);
}

void FScenarioLedger::MarkNotSent(const FString& LocalId, double Now)
{
	const FScenarioJobRow* Row = Find(LocalId);
	if (Row && Row->IsUnresolved())
	{
		SetState(LocalId, EScenarioJobState::Rejected, TEXT("Marked as not sent."), Now);
	}
}

void FScenarioLedger::SetError(const FString& LocalId, const FString& Message, double Now)
{
	if (FScenarioJobRow* Row = Find(LocalId))
	{
		Row->Error = Message;
		Touch(*Row, Now);
	}
}

void FScenarioLedger::SetFiles(const FString& LocalId, const TArray<FScenarioJobFile>& Files, double Now)
{
	if (FScenarioJobRow* Row = Find(LocalId))
	{
		Row->Files = Files;
		Touch(*Row, Now);
	}
}

void FScenarioLedger::MarkImported(const FString& LocalId, double Now)
{
	if (FScenarioJobRow* Row = Find(LocalId))
	{
		Row->bImported = true;
		Row->Error.Reset();
		Touch(*Row, Now);
	}
}

FString FScenarioLedger::AddRecovered(const FScenarioJobInfo& Job, const FString& Lane, double Now)
{
	if (const FScenarioJobRow* Existing = FindByJob(Job.JobId))
	{
		return Existing->LocalId;
	}
	FScenarioJobRow Row;
	++Counter;
	Row.LocalId = FString::Printf(TEXT("local_%lld_%d"), static_cast<long long>(Now * 1000.0), Counter);
	Row.JobId = Job.JobId;
	Row.Kind = Job.WorkflowId.IsEmpty() ? TEXT("model") : TEXT("workflow");
	Row.Lane = Lane;
	Row.ModelId = Job.ModelId;
	Row.WorkflowId = Job.WorkflowId;
	Row.State = StateForStatus(Job.RawStatus, Job.Error);
	Row.AssetIds = Job.AssetIds;
	Row.ChargedCU = Job.ChargedCU;
	Row.Progress = Job.Progress;
	Row.Parameters = MakeShared<FJsonObject>();
	Row.Source = TEXT("recovered");
	Row.CreatedAt = Job.CreatedAt > 0.0 ? Job.CreatedAt : Now;
	Row.UpdatedAt = Now;
	Row.Revision = 1;
	Jobs.Insert(MoveTemp(Row), 0);
	Trim();
	const FString LocalId = Jobs[0].LocalId;
	Save();
	OnChanged.Broadcast(LocalId);
	return LocalId;
}

FScenarioJobRow* FScenarioLedger::ApplyJob(const FScenarioJobInfo& Job, double Now)
{
	FScenarioJobRow* Row = FindByJob(Job.JobId);
	if (!Row)
	{
		return nullptr;
	}
	EScenarioJobState State = StateForStatus(Job.RawStatus, Job.Error);
	// The state changes only when Scenario reports it.
	if (Row->State == EScenarioJobState::CancelRequested && (State == EScenarioJobState::Queued || State == EScenarioJobState::Running))
	{
		State = EScenarioJobState::CancelRequested;
	}
	// A dismissed row stays dismissed.
	if (Row->State == EScenarioJobState::Dismissed)
	{
		State = EScenarioJobState::Dismissed;
	}
	const FString ErrorText = Job.Hint.IsEmpty() ? Job.Error : Job.Error + TEXT(" ") + Job.Hint;
	const bool bChanged = Row->State != State || Row->Progress != Job.Progress || (Job.AssetIds.Num() > 0 && Row->AssetIds != Job.AssetIds)
		|| (Job.ChargedCU >= 0.0 && Row->ChargedCU != Job.ChargedCU) || (!Job.Error.IsEmpty() && Row->Error != ErrorText);
	if (!bChanged)
	{
		return Row;
	}
	Row->State = State;
	Row->Progress = Job.Progress;
	if (Job.AssetIds.Num() > 0)
	{
		Row->AssetIds = Job.AssetIds;
	}
	if (Job.ChargedCU >= 0.0)
	{
		Row->ChargedCU = Job.ChargedCU;
	}
	if (!Job.Error.IsEmpty())
	{
		Row->Error = ErrorText;
	}
	Touch(*Row, Now);
	return Row;
}

int32 FScenarioLedger::Reconcile(const TArray<FScenarioJobInfo>& ServerJobs, double Now)
{
	int32 Linked = 0;
	for (FScenarioJobRow& Row : Jobs)
	{
		if (!Row.IsUnresolved())
		{
			continue;
		}
		const FScenarioJobInfo* Candidate = nullptr;
		int32 Count = 0;
		for (const FScenarioJobInfo& Job : ServerJobs)
		{
			const FString& Target = Row.Kind == TEXT("workflow") ? Job.WorkflowId : Job.ModelId;
			const FString& Wanted = Row.Kind == TEXT("workflow") ? Row.WorkflowId : Row.ModelId;
			if (Job.JobId.IsEmpty() || Target != Wanted || FindByJob(Job.JobId) != nullptr || Job.CreatedAt < 0.0)
			{
				continue;
			}
			const double Delta = Job.CreatedAt - Row.CreatedAt;
			if (Delta >= -5.0 && Delta <= ReconcileWindowSeconds)
			{
				Candidate = &Job;
				++Count;
			}
		}
		if (Count == 1)
		{
			Row.JobId = Candidate->JobId;
			Row.Error.Reset();
			Row.State = StateForStatus(Candidate->RawStatus, Candidate->Error);
			Row.AssetIds = Candidate->AssetIds;
			Row.ChargedCU = Candidate->ChargedCU;
			Row.UpdatedAt = Now;
			++Row.Revision;
			++Linked;
			OnChanged.Broadcast(Row.LocalId);
		}
	}
	if (Linked > 0)
	{
		Save();
	}
	return Linked;
}

FScenarioJobRow* FScenarioLedger::Find(const FString& LocalId)
{
	return Jobs.FindByPredicate([&LocalId](const FScenarioJobRow& Row) { return Row.LocalId == LocalId; });
}

const FScenarioJobRow* FScenarioLedger::Find(const FString& LocalId) const
{
	return Jobs.FindByPredicate([&LocalId](const FScenarioJobRow& Row) { return Row.LocalId == LocalId; });
}

FScenarioJobRow* FScenarioLedger::FindByJob(const FString& JobId)
{
	if (JobId.IsEmpty())
	{
		return nullptr;
	}
	return Jobs.FindByPredicate([&JobId](const FScenarioJobRow& Row) { return Row.JobId == JobId; });
}

TArray<FScenarioJobRow> FScenarioLedger::Active() const
{
	return Jobs.FilterByPredicate([](const FScenarioJobRow& Row) { return Row.IsActive(); });
}

TArray<FScenarioJobRow> FScenarioLedger::Unresolved() const
{
	return Jobs.FilterByPredicate([](const FScenarioJobRow& Row) { return Row.IsUnresolved(); });
}

bool FScenarioLedger::Blocks(const FString& InFingerprint) const
{
	return Jobs.ContainsByPredicate([&InFingerprint](const FScenarioJobRow& Row) { return Row.IsUnresolved() && Row.Fingerprint == InFingerprint; });
}

EScenarioJobState FScenarioLedger::StateForStatus(const FString& Status, const FString& ErrorText)
{
	switch (FScenarioJobInfo::PhaseFromStatus(Status))
	{
	case EScenarioJobPhase::Success: return EScenarioJobState::Success;
	case EScenarioJobPhase::Failure: return FScenarioErrors::IsModerated(ErrorText) ? EScenarioJobState::Blocked : EScenarioJobState::Failed;
	case EScenarioJobPhase::Canceled: return EScenarioJobState::Canceled;
	case EScenarioJobPhase::Queued: return EScenarioJobState::Queued;
	default: return EScenarioJobState::Running;
	}
}

void FScenarioLedger::SetState(const FString& LocalId, EScenarioJobState State, const FString& Message, double Now)
{
	if (FScenarioJobRow* Row = Find(LocalId))
	{
		Row->State = State;
		Row->Error = Message;
		Touch(*Row, Now);
	}
}

void FScenarioLedger::Trim()
{
	while (Jobs.Num() > MaxRows)
	{
		bool bDropped = false;
		for (int32 Index = Jobs.Num() - 1; Index >= 0; --Index)
		{
			if (Jobs[Index].IsFinal())
			{
				Jobs.RemoveAt(Index);
				bDropped = true;
				break;
			}
		}
		if (!bDropped)
		{
			return;
		}
	}
}
