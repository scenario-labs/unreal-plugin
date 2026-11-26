// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioApi.h"

#include "Misc/Paths.h"
#include "ScenarioCU.h"
#include "ScenarioJson.h"

namespace
{
	template <typename T>
	TScenarioResult<T> FailFrom(const FScenarioReply& Reply)
	{
		return TScenarioResult<T>::Fail(Reply.Error);
	}

	template <typename T>
	TScenarioResult<T> Missing(const TCHAR* Field)
	{
		return TScenarioResult<T>::Fail(EScenarioErrorKind::Protocol, FString::Printf(TEXT("Scenario's reply had no '%s'."), Field));
	}

	TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Item : Items)
		{
			Values.Add(MakeShared<FJsonValueString>(Item));
		}
		return Values;
	}

	/** First non-empty string among the fields. */
	FString FirstString(const TSharedPtr<FJsonObject>& Object, std::initializer_list<const TCHAR*> Fields)
	{
		for (const TCHAR* Field : Fields)
		{
			const FString Value = ScenarioJson::GetString(Object, Field);
			if (!Value.IsEmpty())
			{
				return Value;
			}
		}
		return FString();
	}

	/** Reads an upload once: the asset id once imported, the import's failure, or Timeout while it is still importing. A read, so it can be repeated. */
	void ReadUpload(const TSharedRef<IScenarioBackend>& Backend, const FString& UploadId, TFunction<void(const TScenarioResult<FString>&)> OnDone)
	{
		Backend->Send(FScenarioRequest::Get(TEXT("/uploads/") + FScenarioRequest::Segment(UploadId)), [OnDone](const FScenarioReply& Reply)
		{
			const TSharedPtr<FJsonObject> Upload = Reply.bOk ? ScenarioJson::GetObject(Reply.Value, TEXT("upload")) : nullptr;
			const FString AssetId = ScenarioJson::GetString(Upload, TEXT("entityId"));
			const FString Status = ScenarioJson::GetString(Upload, TEXT("status"));
			if (!AssetId.IsEmpty())
			{
				OnDone(TScenarioResult<FString>::Ok(AssetId));
				return;
			}
			if (Status == TEXT("failed"))
			{
				const FString Reason = ScenarioJson::GetString(Upload, TEXT("errorMessage"), TEXT("no reason given"));
				OnDone(TScenarioResult<FString>::Fail(EScenarioErrorKind::Scenario, FString::Printf(TEXT("Scenario could not import the file: %s."), *Reason)));
				return;
			}
			if (!Reply.bOk && Reply.Error.Kind != EScenarioErrorKind::NotFound)
			{
				OnDone(FailFrom<FString>(Reply));
				return;
			}
			// Not imported yet (or not readable yet right after the complete): the uploader reads again.
			FScenarioError Error;
			Error.Kind = EScenarioErrorKind::Timeout;
			Error.Message = TEXT("Scenario is still processing the upload.");
			Error.Detail = Status;
			OnDone(TScenarioResult<FString>::Fail(Error));
		});
	}
}

EScenarioJobPhase FScenarioJobInfo::PhaseFromStatus(const FString& Status)
{
	const FString Lower = Status.ToLower();
	if (Lower == TEXT("success") || Lower == TEXT("succeeded") || Lower == TEXT("completed"))
	{
		return EScenarioJobPhase::Success;
	}
	if (Lower == TEXT("failure") || Lower == TEXT("failed") || Lower == TEXT("error"))
	{
		return EScenarioJobPhase::Failure;
	}
	if (Lower == TEXT("canceled") || Lower == TEXT("cancelled"))
	{
		return EScenarioJobPhase::Canceled;
	}
	if (Lower == TEXT("queued") || Lower == TEXT("pending") || Lower.IsEmpty())
	{
		return EScenarioJobPhase::Queued;
	}
	return EScenarioJobPhase::Running;
}

FScenarioJobInfo FScenarioJobInfo::FromJson(const TSharedPtr<FJsonObject>& Object)
{
	FScenarioJobInfo Job;
	if (!Object.IsValid())
	{
		return Job;
	}
	Job.JobId = FirstString(Object, { TEXT("jobId"), TEXT("job_id"), TEXT("id") });
	Job.JobType = ScenarioJson::GetString(Object, TEXT("jobType"));
	Job.RawStatus = ScenarioJson::GetString(Object, TEXT("status"));
	Job.Phase = PhaseFromStatus(Job.RawStatus);
	double Progress = ScenarioJson::GetNumber(Object, TEXT("progress"), 0.0);
	// Some replies give a percentage.
	if (Progress > 1.0)
	{
		Progress /= 100.0;
	}
	Job.Progress = FMath::Clamp(Progress, 0.0, 1.0);
	// The REST job keeps its outputs, error and workflow under metadata, and its input under metadata.input.
	const TSharedPtr<FJsonObject> Metadata = ScenarioJson::GetObject(Object, TEXT("metadata"));
	const TSharedPtr<FJsonObject> Input = ScenarioJson::GetObject(Metadata, TEXT("input"));
	Job.ModelId = FirstString(Object, { TEXT("modelId"), TEXT("model_id") });
	if (Job.ModelId.IsEmpty())
	{
		Job.ModelId = FirstString(Input, { TEXT("modelId") });
	}
	Job.WorkflowId = FirstString(Object, { TEXT("workflowId"), TEXT("workflow_id") });
	if (Job.WorkflowId.IsEmpty())
	{
		Job.WorkflowId = FirstString(Metadata, { TEXT("workflowId") });
	}
	for (const TSharedPtr<FJsonObject>& Source : { Object, Metadata })
	{
		if (Job.AssetIds.Num() == 0)
		{
			Job.AssetIds = ScenarioJson::GetStringArray(Source, TEXT("assetIds"));
		}
	}
	if (Job.AssetIds.Num() == 0)
	{
		Job.AssetIds = ScenarioJson::GetStringArray(Object, TEXT("asset_ids"));
	}
	const TSharedPtr<FJsonObject> Billing = ScenarioJson::GetObject(Object, TEXT("billing"));
	if (ScenarioJson::HasNumber(Object, TEXT("cuCost")))
	{
		Job.ChargedCU = ScenarioJson::GetNumber(Object, TEXT("cuCost"));
	}
	else if (ScenarioJson::HasNumber(Billing, TEXT("cuCost")))
	{
		Job.ChargedCU = ScenarioJson::GetNumber(Billing, TEXT("cuCost"));
	}
	// cuCost is the main action alone; cuCostDetails lists only the add-ons charged beside it (quality gate...).
	const TSharedPtr<FJsonObject> Details = ScenarioJson::GetObject(Billing, TEXT("cuCostDetails"));
	if (Details.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Details->Values)
		{
			if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Number)
			{
				Job.ChargedCU = FMath::Max(Job.ChargedCU, 0.0) + Pair.Value->AsNumber();
			}
		}
	}
	Job.CreatedAt = ScenarioParseIsoTime(FirstString(Object, { TEXT("createdAt"), TEXT("created_at") }));
	for (const TSharedPtr<FJsonObject>& Source : { Object, Metadata })
	{
		const TSharedPtr<FJsonValue> ErrorValue = Source.IsValid() ? Source->TryGetField(TEXT("error")) : nullptr;
		if (Job.Error.IsEmpty() && ErrorValue.IsValid() && ErrorValue->Type != EJson::Null)
		{
			Job.Error = ErrorValue->Type == EJson::String ? ErrorValue->AsString() : ScenarioJson::WriteValue(ErrorValue);
		}
		if (Job.Hint.IsEmpty())
		{
			Job.Hint = ScenarioJson::GetString(Source, TEXT("hint"));
		}
	}
	return Job;
}

FScenarioAssetInfo FScenarioAssetInfo::FromJson(const TSharedPtr<FJsonObject>& Object)
{
	FScenarioAssetInfo Asset;
	if (!Object.IsValid())
	{
		return Asset;
	}
	Asset.Raw = Object;
	Asset.AssetId = FirstString(Object, { TEXT("id"), TEXT("assetId"), TEXT("asset_id") });
	Asset.MimeType = FirstString(Object, { TEXT("mimeType"), TEXT("mime_type") });
	Asset.Kind = ScenarioJson::GetString(Object, TEXT("kind"));
	Asset.Name = ScenarioJson::GetString(Object, TEXT("name"));
	const TSharedPtr<FJsonObject> Metadata = ScenarioJson::GetObject(Object, TEXT("metadata"));
	Asset.Type = ScenarioJson::GetString(Metadata, TEXT("type"));
	if (Asset.Type.IsEmpty())
	{
		Asset.Type = ScenarioJson::GetString(Object, TEXT("type"));
	}
	const TSharedPtr<FJsonObject> Properties = ScenarioJson::GetObject(Object, TEXT("properties"));
	Asset.Width = static_cast<int32>(ScenarioJson::GetNumber(Metadata, TEXT("width"), ScenarioJson::GetNumber(Properties, TEXT("width"), ScenarioJson::GetNumber(Object, TEXT("width")))));
	Asset.Height = static_cast<int32>(ScenarioJson::GetNumber(Metadata, TEXT("height"), ScenarioJson::GetNumber(Properties, TEXT("height"), ScenarioJson::GetNumber(Object, TEXT("height")))));
	Asset.ParentJobId = FirstString(Metadata, { TEXT("parentJobId"), TEXT("jobId") });
	Asset.ModelId = FirstString(Metadata, { TEXT("modelId") });
	if (Asset.ModelId.IsEmpty())
	{
		Asset.ModelId = ScenarioJson::GetString(Object, TEXT("modelId"));
	}
	for (const FString& Prompt : { ScenarioJson::GetString(Metadata, TEXT("prompt")), ScenarioJson::GetString(Object, TEXT("prompt")) })
	{
		// A prompt kept as a text asset comes back as its asset id: not a title.
		if (!Prompt.IsEmpty() && !Prompt.StartsWith(TEXT("asset_")))
		{
			Asset.Prompt = Prompt;
			break;
		}
	}
	if (Asset.Kind.IsEmpty())
	{
		const FString Mime = Asset.MimeType.ToLower();
		if (Mime.StartsWith(TEXT("image/")))
		{
			Asset.Kind = Mime.Contains(TEXT("exr")) || Mime.Contains(TEXT("hdr")) ? TEXT("image-hdr") : TEXT("image");
		}
		else if (Mime.StartsWith(TEXT("video/")))
		{
			Asset.Kind = TEXT("video");
		}
		else if (Mime.StartsWith(TEXT("audio/")))
		{
			Asset.Kind = TEXT("audio");
		}
		else if (Mime.StartsWith(TEXT("model/")))
		{
			Asset.Kind = TEXT("3d");
		}
	}
	return Asset;
}

FScenarioModelSummary FScenarioModelSummary::FromJson(const TSharedPtr<FJsonObject>& Object)
{
	FScenarioModelSummary Model;
	if (!Object.IsValid())
	{
		return Model;
	}
	Model.Id = FirstString(Object, { TEXT("id"), TEXT("modelId") });
	Model.Name = ScenarioJson::GetString(Object, TEXT("name"));
	Model.ShortDescription = FirstString(Object, { TEXT("shortDescription"), TEXT("short_description") });
	Model.Type = ScenarioJson::GetString(Object, TEXT("type"));
	Model.Privacy = ScenarioJson::GetString(Object, TEXT("privacy"));
	Model.Capabilities = ScenarioJson::GetStringArray(Object, TEXT("capabilities"));
	Model.Tags = ScenarioJson::GetStringArray(Object, TEXT("tags"));
	Model.CreatedAt = ScenarioParseIsoTime(ScenarioJson::GetString(Object, TEXT("createdAt")));
	Model.Provider = ScenarioJson::GetString(ScenarioJson::GetObject(Object, TEXT("complianceMetadata")), TEXT("modelProvider"));
	Model.Status = ScenarioJson::GetString(Object, TEXT("status"));
	// Model rows and search hits carry a thumbnail URL or object.
	Model.ThumbnailUrl = ScenarioJson::GetString(Object, TEXT("thumbnail"));
	if (Model.ThumbnailUrl.IsEmpty())
	{
		const TSharedPtr<FJsonObject> Thumbnail = ScenarioJson::GetObject(Object, TEXT("thumbnail"));
		Model.ThumbnailUrl = ScenarioJson::GetString(Thumbnail, TEXT("url"));
	}
	return Model;
}

FScenarioApi::FScenarioApi(TSharedRef<IScenarioBackend> InBackend)
	: BackendRef(InBackend)
{
}

TSharedPtr<FJsonObject> FScenarioApi::Args()
{
	return MakeShared<FJsonObject>();
}

bool FScenarioApi::FindEstimate(const TSharedPtr<FJsonObject>& Value, FScenarioEstimate& Out)
{
	if (!Value.IsValid())
	{
		return false;
	}
	auto ReadFrom = [&Out](const TSharedPtr<FJsonObject>& Object) -> bool
	{
		for (const TCHAR* Key : { TEXT("creativeUnitsCost"), TEXT("creative_units_cost"), TEXT("cuCost"), TEXT("cu_cost") })
		{
			if (ScenarioJson::HasNumber(Object, Key))
			{
				Out.CU = ScenarioJson::GetNumber(Object, Key);
				Out.Discount = ScenarioJson::GetNumber(Object, TEXT("creativeUnitsDiscount"), 0.0);
				Out.Exact = ScenarioCU::Exact(Out.CU);
				return true;
			}
		}
		return false;
	};
	if (ReadFrom(Value))
	{
		return true;
	}
	for (const TCHAR* Key : { TEXT("estimate"), TEXT("cost"), TEXT("result"), TEXT("data") })
	{
		if (const TSharedPtr<FJsonObject> Inner = ScenarioJson::GetObject(Value, Key))
		{
			if (ReadFrom(Inner))
			{
				return true;
			}
		}
	}
	return false;
}

TArray<FString> FScenarioApi::SparkPrompts(const TSharedPtr<FJsonObject>& Reply)
{
	TArray<FString> Out;
	auto Add = [&Out](const FString& Prompt)
	{
		const FString Clean = Prompt.TrimStartAndEnd();
		if (!Clean.IsEmpty())
		{
			Out.AddUnique(Clean);
		}
	};
	for (const TSharedPtr<FJsonValue>& Item : ScenarioJson::GetArray(Reply, TEXT("prompts")))
	{
		if (Item->Type == EJson::String)
		{
			Add(Item->AsString());
		}
		else if (Item->Type == EJson::Object)
		{
			Add(ScenarioJson::GetString(Item->AsObject(), TEXT("prompt")));
		}
	}
	if (Out.Num() == 0)
	{
		for (const TSharedPtr<FJsonValue>& Call : ScenarioJson::GetArray(Reply, TEXT("calls")))
		{
			if (Call->Type == EJson::Object)
			{
				Add(ScenarioJson::GetString(ScenarioJson::GetObject(Call->AsObject(), TEXT("parameters")), TEXT("prompt")));
			}
		}
	}
	return Out;
}


TSharedPtr<FJsonObject> FScenarioApi::SchemaFromModel(const TSharedPtr<FJsonObject>& Model)
{
	// The inputs keep the API's names (allowedValues, maxLength, required {always}); ScenarioSchema reads both spellings.
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("model_id"), ScenarioJson::GetString(Model, TEXT("id")));
	Schema->SetStringField(TEXT("name"), ScenarioJson::GetString(Model, TEXT("name")));
	Schema->SetField(TEXT("capabilities"), ScenarioJson::StringArrayValue(ScenarioJson::GetStringArray(Model, TEXT("capabilities"))));
	TArray<TSharedPtr<FJsonValue>> Parameters;
	for (const TSharedPtr<FJsonValue>& Input : ScenarioJson::GetArray(Model, TEXT("inputs")))
	{
		if (Input.IsValid() && Input->Type == EJson::Object && !ScenarioJson::GetString(Input->AsObject(), TEXT("name")).IsEmpty())
		{
			Parameters.Add(Input);
		}
	}
	Schema->SetArrayField(TEXT("parameters"), Parameters);
	return Schema;
}

bool FScenarioApi::HasModality(const TArray<FString>& Capabilities, const FString& Modality)
{
	// The capabilities that produce each modality: the API has no modality filter.
	static const TMap<FString, TArray<FString>> Produces = {
		{ TEXT("image"), { TEXT("txt2img"), TEXT("img2img"), TEXT("inpaint"), TEXT("outpaint"), TEXT("reference"), TEXT("video2img"), TEXT("txt2img_texture"), TEXT("img2img_texture"), TEXT("reference_texture") } },
		{ TEXT("video"), { TEXT("txt2video"), TEXT("img2video"), TEXT("video2video"), TEXT("audio2video") } },
		{ TEXT("3d"), { TEXT("3d23d"), TEXT("img23d"), TEXT("txt23d"), TEXT("video23d") } },
		{ TEXT("audio"), { TEXT("txt2audio"), TEXT("audio2audio"), TEXT("video2audio") } },
	};
	const TArray<FString>* Wanted = Produces.Find(Modality.ToLower());
	if (!Wanted)
	{
		return true;
	}
	for (const FString& Capability : Capabilities)
	{
		if (Wanted->Contains(Capability))
		{
			return true;
		}
	}
	return false;
}

FString FScenarioUploadTicket::CleanFileName(const FString& Name)
{
	FString Stem = FPaths::GetBaseFilename(Name).ToLower();
	FString Extension = FPaths::GetExtension(Name).ToLower();
	FString Slug;
	bool bDash = false;
	for (const TCHAR Char : Stem)
	{
		if ((Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('0') && Char <= TEXT('9')))
		{
			if (bDash && !Slug.IsEmpty())
			{
				Slug.AppendChar(TEXT('-'));
			}
			Slug.AppendChar(Char);
			bDash = false;
		}
		else
		{
			bDash = true;
		}
	}
	if (Slug.IsEmpty())
	{
		Slug = TEXT("file");
	}
	FString CleanExtension;
	for (const TCHAR Char : Extension)
	{
		if ((Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('0') && Char <= TEXT('9')))
		{
			CleanExtension.AppendChar(Char);
		}
	}
	return CleanExtension.IsEmpty() ? Slug : Slug + TEXT(".") + CleanExtension;
}

namespace
{
	FString ModelPath(const FString& ModelId)
	{
		return TEXT("/generate/custom/") + FScenarioRequest::Segment(ModelId);
	}

	FString WorkflowRunPath(const FString& WorkflowId)
	{
		return TEXT("/workflows/") + FScenarioRequest::Segment(WorkflowId) + TEXT("/run");
	}

	TSharedPtr<FJsonObject> OrEmpty(const TSharedPtr<FJsonObject>& Object)
	{
		return Object.IsValid() ? ScenarioJson::CloneObject(Object) : MakeShared<FJsonObject>();
	}

	/** Prompt Spark v3 (contextual-v2): prompt variants written for a target model, from text and up to 15 images. */
	TSharedPtr<FJsonObject> SparkBody(const FString& ModelId, const FString& Prompt, const TArray<FString>& ImageIds, int32 NumResults)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("mode"), TEXT("contextual-v2"));
		Body->SetStringField(TEXT("modelId"), ModelId);
		if (!Prompt.IsEmpty())
		{
			Body->SetStringField(TEXT("prompt"), Prompt);
		}
		if (ImageIds.Num() > 0)
		{
			Body->SetArrayField(TEXT("images"), StringValues(ImageIds));
		}
		Body->SetNumberField(TEXT("numResults"), FMath::Clamp(NumResults, 1, 5));
		return Body;
	}

	template <typename T>
	void ReadEstimate(const FScenarioReply& Reply, const TCHAR* What, TFunction<void(const TScenarioResult<T>&)> OnDone)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<T>(Reply));
			return;
		}
		FScenarioEstimate Estimate;
		if (!FScenarioApi::FindEstimate(Reply.Value, Estimate))
		{
			OnDone(TScenarioResult<T>::Fail(EScenarioErrorKind::Protocol, FString::Printf(TEXT("Scenario returned no price for %s."), What)));
			return;
		}
		OnDone(TScenarioResult<T>::Ok(Estimate));
	}

	void ReadSubmitted(const FScenarioReply& Reply, const TCHAR* Unknown, TFunction<void(const TScenarioResult<FScenarioSubmitted>&)> OnDone)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FScenarioSubmitted>(Reply));
			return;
		}
		FScenarioSubmitted Submitted;
		Submitted.JobId = FScenarioRestClient::CreatedJobId(Reply.Value);
		if (Submitted.JobId.IsEmpty())
		{
			FScenarioError Error;
			Error.Kind = EScenarioErrorKind::Protocol;
			Error.Message = Unknown;
			Error.bAmbiguous = true;
			OnDone(TScenarioResult<FScenarioSubmitted>::Fail(Error));
			return;
		}
		const TSharedPtr<FJsonObject> Job = ScenarioJson::GetObject(Reply.Value, TEXT("job"));
		Submitted.Status = ScenarioJson::GetString(Job, TEXT("status"), ScenarioJson::GetString(Reply.Value, TEXT("status"), TEXT("queued")));
		OnDone(TScenarioResult<FScenarioSubmitted>::Ok(Submitted));
	}

	FScenarioProject ProjectFrom(const TSharedPtr<FJsonObject>& Object)
	{
		FScenarioProject Project;
		Project.Id = FirstString(Object, { TEXT("id"), TEXT("projectId") });
		Project.Name = ScenarioJson::GetString(Object, TEXT("name"));
		return Project;
	}
}

void FScenarioApi::Estimate(const FString& ModelId, const TSharedPtr<FJsonObject>& Parameters, TCallback<FScenarioEstimate> OnDone)
{
	FScenarioRequest Request = FScenarioRequest::Post(ModelPath(ModelId), OrEmpty(Parameters), EScenarioCall::DryRun).Param(TEXT("dryRun"), TEXT("true"));
	BackendRef->Send(Request, [OnDone](const FScenarioReply& Reply)
	{
		ReadEstimate<FScenarioEstimate>(Reply, TEXT("this request"), OnDone);
	});
}

void FScenarioApi::Submit(const FString& ModelId, const TSharedPtr<FJsonObject>& Parameters, TCallback<FScenarioSubmitted> OnDone)
{
	FScenarioRequest Request = FScenarioRequest::Post(ModelPath(ModelId), OrEmpty(Parameters), EScenarioCall::Paid).WithTimeout(90.0f);
	BackendRef->Send(Request, [OnDone](const FScenarioReply& Reply)
	{
		ReadSubmitted(Reply, TEXT("Scenario accepted the request but returned no job id. Check recent jobs before generating again."), OnDone);
	});
}

void FScenarioApi::EstimateWorkflow(const FString& WorkflowId, const TSharedPtr<FJsonObject>& Inputs, TCallback<FScenarioEstimate> OnDone)
{
	FScenarioRequest Request = FScenarioRequest::Post(WorkflowRunPath(WorkflowId), OrEmpty(Inputs), EScenarioCall::DryRun).Param(TEXT("dryRun"), TEXT("true"));
	Request.Verb = TEXT("PUT");
	BackendRef->Send(Request, [OnDone](const FScenarioReply& Reply)
	{
		ReadEstimate<FScenarioEstimate>(Reply, TEXT("this workflow"), OnDone);
	});
}

void FScenarioApi::SubmitWorkflow(const FString& WorkflowId, const TSharedPtr<FJsonObject>& Inputs, TCallback<FScenarioSubmitted> OnDone)
{
	FScenarioRequest Request = FScenarioRequest::Post(WorkflowRunPath(WorkflowId), OrEmpty(Inputs), EScenarioCall::Paid).WithTimeout(90.0f);
	Request.Verb = TEXT("PUT");
	BackendRef->Send(Request, [OnDone](const FScenarioReply& Reply)
	{
		ReadSubmitted(Reply, TEXT("Scenario accepted the workflow but returned no job id. Check recent jobs before running it again."), OnDone);
	});
}

void FScenarioApi::EstimatePromptSpark(const FString& ModelId, const FString& Prompt, const TArray<FString>& ImageIds, int32 NumResults, TCallback<FScenarioEstimate> OnDone)
{
	FScenarioRequest Request = FScenarioRequest::Post(TEXT("/generate/prompt"), SparkBody(ModelId, Prompt, ImageIds, NumResults), EScenarioCall::DryRun).Param(TEXT("dryRun"), TEXT("true"));
	BackendRef->Send(Request, [OnDone](const FScenarioReply& Reply)
	{
		ReadEstimate<FScenarioEstimate>(Reply, TEXT("Prompt Spark"), OnDone);
	});
}

void FScenarioApi::RunPromptSpark(const FString& ModelId, const FString& Prompt, const TArray<FString>& ImageIds, int32 NumResults, FScenarioReplyCallback OnDone)
{
	// Answers at once with the prompts and the finished job.
	FScenarioRequest Request = FScenarioRequest::Post(TEXT("/generate/prompt"), SparkBody(ModelId, Prompt, ImageIds, NumResults), EScenarioCall::Paid).WithTimeout(120.0f);
	BackendRef->Send(Request, [OnDone](const FScenarioReply& Reply)
	{
		if (Reply.bOk && SparkPrompts(Reply.Value).Num() == 0)
		{
			FScenarioReply Empty = FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Prompt Spark returned no prompt."));
			Empty.Error.JobId = FScenarioRestClient::CreatedJobId(Reply.Value);
			OnDone(Empty);
			return;
		}
		OnDone(Reply);
	});
}

void FScenarioApi::GetJob(const FString& JobId, TCallback<FScenarioJobInfo> OnDone)
{
	BackendRef->Send(FScenarioRequest::Get(TEXT("/jobs/") + FScenarioRequest::Segment(JobId)), [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FScenarioJobInfo>(Reply));
			return;
		}
		const TSharedPtr<FJsonObject> Job = ScenarioJson::GetObject(Reply.Value, TEXT("job"));
		if (!Job.IsValid())
		{
			OnDone(Missing<FScenarioJobInfo>(TEXT("job")));
			return;
		}
		OnDone(TScenarioResult<FScenarioJobInfo>::Ok(FScenarioJobInfo::FromJson(Job)));
	});
}

void FScenarioApi::ListJobs(int32 Limit, TCallback<TArray<FScenarioJobInfo>> OnDone)
{
	BackendRef->Send(FScenarioRequest::Get(TEXT("/jobs")).Param(TEXT("pageSize"), FMath::Clamp(Limit, 1, 100)), [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<TArray<FScenarioJobInfo>>(Reply));
			return;
		}
		if (!Reply.Value->HasField(TEXT("jobs")))
		{
			OnDone(Missing<TArray<FScenarioJobInfo>>(TEXT("jobs")));
			return;
		}
		TArray<FScenarioJobInfo> Jobs;
		for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Reply.Value, TEXT("jobs")))
		{
			if (Row.IsValid() && Row->Type == EJson::Object)
			{
				Jobs.Add(FScenarioJobInfo::FromJson(Row->AsObject()));
			}
		}
		OnDone(TScenarioResult<TArray<FScenarioJobInfo>>::Ok(Jobs));
	});
}

void FScenarioApi::CancelJob(const FString& JobId, FScenarioReplyCallback OnDone)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("action"), TEXT("cancel"));
	BackendRef->Send(FScenarioRequest::Post(TEXT("/jobs/") + FScenarioRequest::Segment(JobId) + TEXT("/action"), Body, EScenarioCall::Cancel), MoveTemp(OnDone));
}

void FScenarioApi::GetAsset(const FString& AssetId, TCallback<FScenarioAssetInfo> OnDone)
{
	BackendRef->Send(FScenarioRequest::Get(TEXT("/assets/") + FScenarioRequest::Segment(AssetId)), [OnDone, AssetId](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FScenarioAssetInfo>(Reply));
			return;
		}
		const TSharedPtr<FJsonObject> Asset = ScenarioJson::GetObject(Reply.Value, TEXT("asset"));
		if (!Asset.IsValid())
		{
			OnDone(Missing<FScenarioAssetInfo>(TEXT("asset")));
			return;
		}
		FScenarioAssetInfo Info = FScenarioAssetInfo::FromJson(Asset);
		if (Info.AssetId.IsEmpty())
		{
			Info.AssetId = AssetId;
		}
		OnDone(TScenarioResult<FScenarioAssetInfo>::Ok(Info));
	});
}

void FScenarioApi::DownloadUrl(const FString& AssetId, const FString& Format, TCallback<FString> OnDone)
{
	// A signed link, converted on the spot when a format is named (the API matches formats in lower case).
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	if (!Format.IsEmpty())
	{
		Body->SetStringField(TEXT("targetFormat"), Format.ToLower());
	}
	FScenarioRequest Request = FScenarioRequest::Post(TEXT("/assets/") + FScenarioRequest::Segment(AssetId) + TEXT("/download"), Body, EScenarioCall::Read).WithTimeout(120.0f);
	BackendRef->Send(Request, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FString>(Reply));
			return;
		}
		const FString Url = FirstString(Reply.Value, { TEXT("url"), TEXT("downloadUrl") });
		if (Url.IsEmpty())
		{
			OnDone(Missing<FString>(TEXT("url")));
			return;
		}
		OnDone(TScenarioResult<FString>::Ok(Url));
	});
}

void FScenarioApi::GetSchema(const FString& ModelId, FScenarioReplyCallback OnDone)
{
	BackendRef->Send(FScenarioRequest::Get(TEXT("/models/") + FScenarioRequest::Segment(ModelId)), [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(Reply);
			return;
		}
		const TSharedPtr<FJsonObject> Model = ScenarioJson::GetObject(Reply.Value, TEXT("model"));
		if (!Model.IsValid())
		{
			OnDone(FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario's reply had no 'model'.")));
			return;
		}
		const TSharedPtr<FJsonObject> Schema = SchemaFromModel(Model);
		if (ScenarioJson::GetArray(Schema, TEXT("parameters")).Num() == 0)
		{
			// LoRAs and compositions list no inputs: they run through their base model, which the plugin does not wire yet.
			OnDone(FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("This model lists no inputs of its own (a LoRA or a composition, for example), so the plugin cannot run it yet.")));
			return;
		}
		OnDone(FScenarioReply::Ok(Schema));
	});
}

void FScenarioApi::ListModels(const FString& Privacy, const FString& Modality, int32 Limit, const FString& PageToken, TCallback<FScenarioModelPage> OnDone)
{
	FScenarioRequest Request = FScenarioRequest::Get(TEXT("/models"))
		.Param(TEXT("privacy"), Privacy)
		.Param(TEXT("pageSize"), FMath::Clamp(Limit, 1, 500))
		.Param(TEXT("paginationToken"), PageToken)
		.WithTimeout(90.0f);
	if (Privacy == TEXT("private"))
	{
		// Only ready models; this also leaves out deleted ones.
		Request.Param(TEXT("status"), TEXT("trained"));
	}
	BackendRef->Send(Request, [OnDone, Modality](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FScenarioModelPage>(Reply));
			return;
		}
		if (!Reply.Value->HasField(TEXT("models")))
		{
			OnDone(Missing<FScenarioModelPage>(TEXT("models")));
			return;
		}
		FScenarioModelPage Page;
		for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Reply.Value, TEXT("models")))
		{
			if (Row.IsValid() && Row->Type == EJson::Object)
			{
				FScenarioModelSummary Model = FScenarioModelSummary::FromJson(Row->AsObject());
				if (!Model.Id.IsEmpty() && HasModality(Model.Capabilities, Modality))
				{
					Page.Models.Add(MoveTemp(Model));
				}
			}
		}
		Page.NextPageToken = ScenarioJson::GetString(Reply.Value, TEXT("nextPaginationToken"));
		OnDone(TScenarioResult<FScenarioModelPage>::Ok(Page));
	});
}

void FScenarioApi::SearchAssets(const FString& Query, const FString& Kind, int32 Limit, int32 Offset, TCallback<TArray<FScenarioAssetInfo>> OnDone)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	if (!Query.IsEmpty())
	{
		Body->SetStringField(TEXT("query"), Query);
	}
	if (!Kind.IsEmpty())
	{
		Body->SetStringField(TEXT("filter"), FString::Printf(TEXT("kind = %s"), *ScenarioJson::EscapeString(Kind)));
	}
	else if (Query.IsEmpty())
	{
		// Search needs a query or a filter: every asset has a kind.
		Body->SetStringField(TEXT("filter"), TEXT("kind EXISTS"));
	}
	if (Query.IsEmpty())
	{
		Body->SetField(TEXT("sortBy"), ScenarioJson::StringArrayValue({ TEXT("createdAt:desc") }));
	}
	Body->SetNumberField(TEXT("limit"), FMath::Clamp(Limit, 1, 100));
	Body->SetNumberField(TEXT("offset"), FMath::Max(0, Offset));
	BackendRef->Send(FScenarioRequest::Post(TEXT("/search/assets"), Body, EScenarioCall::Read), [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<TArray<FScenarioAssetInfo>>(Reply));
			return;
		}
		TArray<FScenarioAssetInfo> Assets;
		for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Reply.Value, TEXT("hits")))
		{
			if (Row.IsValid() && Row->Type == EJson::Object)
			{
				FScenarioAssetInfo Asset = FScenarioAssetInfo::FromJson(Row->AsObject());
				if (!Asset.AssetId.IsEmpty())
				{
					Assets.Add(MoveTemp(Asset));
				}
			}
		}
		OnDone(TScenarioResult<TArray<FScenarioAssetInfo>>::Ok(Assets));
	});
}

void FScenarioApi::ListAssets(const FString& Kind, int32 Limit, const FString& PageToken, TCallback<FScenarioAssetPage> OnDone)
{
	// Search ranks the whole team's index before it narrows to the project, so a small project in a
	// large team can get an empty first page. The asset list reads the project itself; it filters by
	// how an asset was made, not by kind.
	static const TMap<FString, FString> Types = {
		{ TEXT("image"), TEXT("txt2img,img2img,uploaded") },
		{ TEXT("3d"), TEXT("uploaded-3d,img23d,txt23d,3d23d") },
		{ TEXT("video"), TEXT("img2video,txt2video,uploaded-video") },
		{ TEXT("audio"), TEXT("txt2audio,audio2audio,uploaded-audio") },
	};
	FScenarioRequest Request = FScenarioRequest::Get(TEXT("/assets"))
		.Param(TEXT("pageSize"), FMath::Clamp(Limit, 1, 100))
		.Param(TEXT("paginationToken"), PageToken)
		.Param(TEXT("sortBy"), TEXT("createdAt"))
		.Param(TEXT("sortDirection"), TEXT("desc"));
	if (const FString* Group = Types.Find(Kind))
	{
		Request.Param(TEXT("types"), *Group);
	}
	BackendRef->Send(Request, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FScenarioAssetPage>(Reply));
			return;
		}
		FScenarioAssetPage Page;
		for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Reply.Value, TEXT("assets")))
		{
			if (Row.IsValid() && Row->Type == EJson::Object)
			{
				FScenarioAssetInfo Asset = FScenarioAssetInfo::FromJson(Row->AsObject());
				if (!Asset.AssetId.IsEmpty())
				{
					Page.Assets.Add(MoveTemp(Asset));
				}
			}
		}
		Page.NextPageToken = ScenarioJson::GetString(Reply.Value, TEXT("nextPaginationToken"));
		OnDone(TScenarioResult<FScenarioAssetPage>::Ok(Page));
	});
}

void FScenarioApi::ListTeams(TCallback<TArray<FScenarioTeam>> OnDone)
{
	if (BackendRef->UsesApiKey())
	{
		// A key is scoped to one project and cannot list teams: its project names the team.
		BackendRef->Send(FScenarioRequest::Get(TEXT("/projects")).Param(TEXT("pageSize"), 100), [OnDone](const FScenarioReply& Reply)
		{
			if (!Reply.bOk)
			{
				OnDone(FailFrom<TArray<FScenarioTeam>>(Reply));
				return;
			}
			TArray<FScenarioTeam> Teams;
			for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Reply.Value, TEXT("projects")))
			{
				if (!Row.IsValid() || Row->Type != EJson::Object)
				{
					continue;
				}
				// Only the names and ids are kept: the row also lists the project's members and keys.
				const FScenarioProject Project = ProjectFrom(Row->AsObject());
				if (Project.Id.IsEmpty())
				{
					continue;
				}
				FScenarioTeam Team;
				Team.Id = ScenarioJson::GetString(Row->AsObject(), TEXT("teamId"));
				Team.Name = ScenarioJson::GetString(Row->AsObject(), TEXT("teamName"));
				Team.Projects.Add(Project);
				Teams.Add(MoveTemp(Team));
				break;
			}
			OnDone(TScenarioResult<TArray<FScenarioTeam>>::Ok(Teams));
		});
		return;
	}
	// Unscoped on purpose: with a stale projectId the gateway refuses the whole read.
	BackendRef->Send(FScenarioRequest::Get(TEXT("/teams")).WithScope(EScenarioScope::None), [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<TArray<FScenarioTeam>>(Reply));
			return;
		}
		TArray<FScenarioTeam> Teams;
		for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Reply.Value, TEXT("teams")))
		{
			if (!Row.IsValid() || Row->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Object = Row->AsObject();
			FScenarioTeam Team;
			Team.Id = FirstString(Object, { TEXT("id"), TEXT("teamId") });
			Team.Name = ScenarioJson::GetString(Object, TEXT("name"));
			Team.Plan = ScenarioJson::GetString(Object, TEXT("plan"));
			for (const TSharedPtr<FJsonValue>& ProjectRow : ScenarioJson::GetArray(Object, TEXT("projects")))
			{
				if (ProjectRow.IsValid() && ProjectRow->Type == EJson::Object)
				{
					const FScenarioProject Project = ProjectFrom(ProjectRow->AsObject());
					if (!Project.Id.IsEmpty())
					{
						Team.Projects.Add(Project);
					}
				}
			}
			if (!Team.Id.IsEmpty())
			{
				Teams.Add(MoveTemp(Team));
			}
		}
		OnDone(TScenarioResult<TArray<FScenarioTeam>>::Ok(Teams));
	});
}

void FScenarioApi::ListProjects(const FString& TeamId, TCallback<TArray<FScenarioProject>> OnDone)
{
	TSharedRef<TArray<FScenarioProject>> Collected = MakeShared<TArray<FScenarioProject>>();
	TSharedRef<TFunction<void(const FString&, int32)>> Page = MakeShared<TFunction<void(const FString&, int32)>>();
	TWeakPtr<IScenarioBackend> WeakBackend = BackendRef;
	TWeakPtr<TFunction<void(const FString&, int32)>> WeakPage = Page;
	*Page = [WeakBackend, WeakPage, TeamId, Collected, OnDone](const FString& Token, int32 Index)
	{
		const TSharedPtr<IScenarioBackend> Backend = WeakBackend.Pin();
		// The pending request keeps the pager alive until its reply.
		const TSharedPtr<TFunction<void(const FString&, int32)>> Keep = WeakPage.Pin();
		if (!Backend.IsValid() || !Keep.IsValid())
		{
			return;
		}
		// The team only: a projectId would make the API list that project's team instead.
		FScenarioRequest Request = FScenarioRequest::Get(TEXT("/projects")).Param(TEXT("teamId"), TeamId).Param(TEXT("pageSize"), 100)
			.Param(TEXT("paginationToken"), Token).WithScope(EScenarioScope::None);
		Backend->Send(Request, [Keep, Collected, OnDone, Index](const FScenarioReply& Reply)
		{
			if (!Reply.bOk)
			{
				OnDone(FailFrom<TArray<FScenarioProject>>(Reply));
				return;
			}
			for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Reply.Value, TEXT("projects")))
			{
				if (Row.IsValid() && Row->Type == EJson::Object)
				{
					const FScenarioProject Project = ProjectFrom(Row->AsObject());
					if (!Project.Id.IsEmpty())
					{
						Collected->Add(Project);
					}
				}
			}
			// Pages are filtered after paging, so a short page is not the end.
			const FString Next = ScenarioJson::GetString(Reply.Value, TEXT("nextPaginationToken"));
			if (Next.IsEmpty() || Index + 1 >= 20)
			{
				OnDone(TScenarioResult<TArray<FScenarioProject>>::Ok(*Collected));
				return;
			}
			(*Keep)(Next, Index + 1);
		});
	};
	(*Page)(FString(), 0);
}

void FScenarioApi::UsageTotal(const FString& ProjectId, const FString& StartDate, const FString& EndDate, TCallback<double> OnDone)
{
	FScenarioRequest Request = FScenarioRequest::Get(TEXT("/usages"))
		.Param(TEXT("projectIds"), ProjectId)
		.Param(TEXT("type"), TEXT("consumption"))
		.WithScope(EScenarioScope::Team);
	if (!StartDate.IsEmpty() && !EndDate.IsEmpty())
	{
		Request.Param(TEXT("startDate"), StartDate).Param(TEXT("endDate"), EndDate);
	}
	else
	{
		Request.Param(TEXT("timeRange"), TEXT("last-thirty-one-days"));
	}
	BackendRef->Send(Request, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<double>(Reply));
			return;
		}
		if (!Reply.Value->HasField(TEXT("consumption")))
		{
			OnDone(Missing<double>(TEXT("consumption")));
			return;
		}
		// One row per person; value is already after discounts.
		double Total = 0.0;
		for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Reply.Value, TEXT("consumption")))
		{
			if (Row.IsValid() && Row->Type == EJson::Object)
			{
				Total += ScenarioJson::GetNumber(Row->AsObject(), TEXT("value"));
			}
		}
		OnDone(TScenarioResult<double>::Ok(Total));
	});
}

void FScenarioApi::ListWorkflows(const FString& Privacy, const FString& Status, int32 Limit, const FString& PageToken, const TArray<FString>& Tags, FScenarioReplyCallback OnDone)
{
	FScenarioRequest Request = FScenarioRequest::Get(TEXT("/workflows"))
		.Param(TEXT("privacy"), Privacy)
		.Param(TEXT("status"), Privacy == TEXT("public") ? FString() : Status)
		.Param(TEXT("pageSize"), FMath::Clamp(Limit, 1, 100))
		.Param(TEXT("paginationToken"), PageToken)
		.Param(TEXT("tags"), FString::Join(Tags, TEXT(",")))
		// Without the flow and editor layout: the list only needs names, inputs and thumbnails.
		.Param(TEXT("full"), TEXT("false"));
	BackendRef->Send(Request, MoveTemp(OnDone));
}

void FScenarioApi::GetWorkflow(const FString& WorkflowId, FScenarioReplyCallback OnDone)
{
	BackendRef->Send(FScenarioRequest::Get(TEXT("/workflows/") + FScenarioRequest::Segment(WorkflowId)), MoveTemp(OnDone));
}

void FScenarioApi::ListCollections(FScenarioReplyCallback OnDone)
{
	BackendRef->Send(FScenarioRequest::Get(TEXT("/collections")).Param(TEXT("pageSize"), 100), MoveTemp(OnDone));
}

void FScenarioApi::UploadStart(const FString& FileName, const FString& ContentType, const FString& Kind, int64 FileSize, TCallback<FScenarioUploadTicket> OnDone)
{
	const int64 PartSize = FScenarioUploadTicket::DefaultPartSize;
	const int32 PartCount = static_cast<int32>(FMath::Max<int64>(1, FMath::DivideAndRoundUp<int64>(FileSize, PartSize)));
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("fileName"), FScenarioUploadTicket::CleanFileName(FileName));
	Body->SetStringField(TEXT("contentType"), ContentType);
	Body->SetStringField(TEXT("kind"), Kind);
	Body->SetNumberField(TEXT("fileSize"), static_cast<double>(FileSize));
	Body->SetNumberField(TEXT("parts"), PartCount);
	BackendRef->Send(FScenarioRequest::Post(TEXT("/uploads"), Body, EScenarioCall::Paid), [OnDone, PartSize, PartCount](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FScenarioUploadTicket>(Reply));
			return;
		}
		const TSharedPtr<FJsonObject> Upload = ScenarioJson::GetObject(Reply.Value, TEXT("upload"));
		FScenarioUploadTicket Ticket;
		Ticket.UploadId = ScenarioJson::GetString(Upload, TEXT("id"));
		Ticket.AssetId = ScenarioJson::GetString(Upload, TEXT("entityId"));
		Ticket.PartSize = PartSize;
		for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Upload, TEXT("parts")))
		{
			if (Row.IsValid() && Row->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Part = Row->AsObject();
				const int32 Number = static_cast<int32>(ScenarioJson::GetNumber(Part, TEXT("number")));
				const FString Url = ScenarioJson::GetString(Part, TEXT("url"));
				if (Number > 0 && !Url.IsEmpty())
				{
					Ticket.Parts.Emplace(Number, Url);
				}
			}
		}
		Ticket.Parts.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key < B.Key; });
		if (Ticket.AssetId.IsEmpty() && (Ticket.UploadId.IsEmpty() || Ticket.Parts.Num() != PartCount))
		{
			OnDone(TScenarioResult<FScenarioUploadTicket>::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario did not return upload URLs.")));
			return;
		}
		OnDone(TScenarioResult<FScenarioUploadTicket>::Ok(Ticket));
	});
}

void FScenarioApi::UploadComplete(const FString& UploadId, TCallback<FString> OnDone)
{
	const FString Path = TEXT("/uploads/") + FScenarioRequest::Segment(UploadId);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("action"), TEXT("complete"));
	TWeakPtr<IScenarioBackend> WeakBackend = BackendRef;
	BackendRef->Send(FScenarioRequest::Post(Path + TEXT("/action"), Body, EScenarioCall::Paid), [WeakBackend, UploadId, OnDone](const FScenarioReply& Completed)
	{
		// A second complete, or one read before the upload is visible, is refused harmlessly: read on.
		const FString Said = (Completed.Error.Detail + TEXT(" ") + Completed.Error.Message).ToLower();
		if (!Completed.bOk && !Said.Contains(TEXT("unable to load request upload id")) && !Said.Contains(TEXT("unable to complete upload with status")))
		{
			OnDone(FailFrom<FString>(Completed));
			return;
		}
		if (const TSharedPtr<IScenarioBackend> Backend = WeakBackend.Pin())
		{
			ReadUpload(Backend.ToSharedRef(), UploadId, OnDone);
		}
	});
}

void FScenarioApi::UploadStatus(const FString& UploadId, TCallback<FString> OnDone)
{
	ReadUpload(BackendRef, UploadId, OnDone);
}
