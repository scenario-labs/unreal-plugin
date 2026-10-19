// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioApi.h"

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
	Job.ModelId = FirstString(Object, { TEXT("modelId"), TEXT("model_id") });
	Job.WorkflowId = FirstString(Object, { TEXT("workflowId"), TEXT("workflow_id") });
	Job.AssetIds = ScenarioJson::GetStringArray(Object, TEXT("assetIds"));
	if (Job.AssetIds.Num() == 0)
	{
		Job.AssetIds = ScenarioJson::GetStringArray(Object, TEXT("asset_ids"));
	}
	if (ScenarioJson::HasNumber(Object, TEXT("cuCost")))
	{
		Job.ChargedCU = ScenarioJson::GetNumber(Object, TEXT("cuCost"));
	}
	else if (const TSharedPtr<FJsonObject> Billing = ScenarioJson::GetObject(Object, TEXT("billing")))
	{
		if (ScenarioJson::HasNumber(Billing, TEXT("cuCost")))
		{
			Job.ChargedCU = ScenarioJson::GetNumber(Billing, TEXT("cuCost"));
		}
	}
	Job.CreatedAt = ScenarioParseIsoTime(FirstString(Object, { TEXT("createdAt"), TEXT("created_at") }));
	const TSharedPtr<FJsonValue> ErrorValue = Object->TryGetField(TEXT("error"));
	if (ErrorValue.IsValid() && ErrorValue->Type != EJson::Null)
	{
		Job.Error = ErrorValue->Type == EJson::String ? ErrorValue->AsString() : ScenarioJson::WriteValue(ErrorValue);
	}
	Job.Hint = ScenarioJson::GetString(Object, TEXT("hint"));
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
	Asset.Width = static_cast<int32>(ScenarioJson::GetNumber(Metadata, TEXT("width"), ScenarioJson::GetNumber(Object, TEXT("width"))));
	Asset.Height = static_cast<int32>(ScenarioJson::GetNumber(Metadata, TEXT("height"), ScenarioJson::GetNumber(Object, TEXT("height"))));
	Asset.ParentJobId = FirstString(Metadata, { TEXT("parentJobId"), TEXT("jobId") });
	Asset.ModelId = FirstString(Metadata, { TEXT("modelId") });
	if (Asset.ModelId.IsEmpty())
	{
		// assets_list flattens the essentials of metadata to the top level.
		Asset.ModelId = ScenarioJson::GetString(Object, TEXT("modelId"));
	}
	Asset.Prompt = ScenarioJson::GetString(Metadata, TEXT("prompt"));
	if (Asset.Prompt.IsEmpty() && !ScenarioJson::GetString(Object, TEXT("prompt")).StartsWith(TEXT("asset_")))
	{
		// A prompt kept as a text asset comes back as its asset id: not a title.
		Asset.Prompt = ScenarioJson::GetString(Object, TEXT("prompt"));
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
	// search with include_urls carries a thumbnail URL or object.
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
	if (Out.Num() == 0)
	{
		Add(ScenarioJson::GetString(Reply, TEXT("recommended_prompt")));
	}
	return Out;
}

void FScenarioApi::Estimate(const FString& ModelId, const TSharedPtr<FJsonObject>& Parameters, TCallback<FScenarioEstimate> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("model_id"), ModelId);
	Arguments->SetObjectField(TEXT("parameters"), Parameters.IsValid() ? Parameters : Args());
	Arguments->SetBoolField(TEXT("dry_run"), true);
	BackendRef->CallTool(TEXT("model_run"), Arguments, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FScenarioEstimate>(Reply));
			return;
		}
		FScenarioEstimate Estimate;
		if (!FindEstimate(Reply.Value, Estimate))
		{
			OnDone(TScenarioResult<FScenarioEstimate>::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario returned no price for this request.")));
			return;
		}
		OnDone(TScenarioResult<FScenarioEstimate>::Ok(Estimate));
	});
}

void FScenarioApi::Submit(const FString& ModelId, const TSharedPtr<FJsonObject>& Parameters, TCallback<FScenarioSubmitted> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("model_id"), ModelId);
	Arguments->SetObjectField(TEXT("parameters"), Parameters.IsValid() ? Parameters : Args());
	Arguments->SetBoolField(TEXT("wait"), false);
	BackendRef->CallTool(TEXT("model_run"), Arguments, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FScenarioSubmitted>(Reply));
			return;
		}
		FScenarioSubmitted Submitted;
		Submitted.JobId = FScenarioMcpClient::CreatedJobId(Reply.Value);
		if (Submitted.JobId.IsEmpty())
		{
			FScenarioError Error;
			Error.Kind = EScenarioErrorKind::Protocol;
			Error.Message = TEXT("Scenario accepted the request but returned no job id. Check recent jobs before generating again.");
			Error.bAmbiguous = true;
			OnDone(TScenarioResult<FScenarioSubmitted>::Fail(Error));
			return;
		}
		Submitted.Status = ScenarioJson::GetString(Reply.Value, TEXT("status"), TEXT("queued"));
		OnDone(TScenarioResult<FScenarioSubmitted>::Ok(Submitted));
	}, 90.0f);
}

void FScenarioApi::EstimateWorkflow(const FString& WorkflowId, const TSharedPtr<FJsonObject>& Inputs, TCallback<FScenarioEstimate> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("workflow_id"), WorkflowId);
	Arguments->SetObjectField(TEXT("inputs"), Inputs.IsValid() ? Inputs : Args());
	Arguments->SetBoolField(TEXT("dry_run"), true);
	BackendRef->CallTool(TEXT("workflow_run"), Arguments, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FScenarioEstimate>(Reply));
			return;
		}
		FScenarioEstimate Estimate;
		if (!FindEstimate(Reply.Value, Estimate))
		{
			OnDone(TScenarioResult<FScenarioEstimate>::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario returned no price for this workflow.")));
			return;
		}
		OnDone(TScenarioResult<FScenarioEstimate>::Ok(Estimate));
	});
}

void FScenarioApi::SubmitWorkflow(const FString& WorkflowId, const TSharedPtr<FJsonObject>& Inputs, TCallback<FScenarioSubmitted> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("workflow_id"), WorkflowId);
	Arguments->SetObjectField(TEXT("inputs"), Inputs.IsValid() ? Inputs : Args());
	BackendRef->CallTool(TEXT("workflow_run"), Arguments, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FScenarioSubmitted>(Reply));
			return;
		}
		FScenarioSubmitted Submitted;
		Submitted.JobId = FScenarioMcpClient::CreatedJobId(Reply.Value);
		if (Submitted.JobId.IsEmpty())
		{
			FScenarioError Error;
			Error.Kind = EScenarioErrorKind::Protocol;
			Error.Message = TEXT("Scenario accepted the workflow but returned no job id. Check recent jobs before running it again.");
			Error.bAmbiguous = true;
			OnDone(TScenarioResult<FScenarioSubmitted>::Fail(Error));
			return;
		}
		const TSharedPtr<FJsonObject> Job = ScenarioJson::GetObject(Reply.Value, TEXT("job"));
		Submitted.Status = ScenarioJson::GetString(Job, TEXT("status"), ScenarioJson::GetString(Reply.Value, TEXT("status"), TEXT("queued")));
		OnDone(TScenarioResult<FScenarioSubmitted>::Ok(Submitted));
	}, 90.0f);
}

void FScenarioApi::EstimatePromptSpark(const FString& ModelId, const FString& Prompt, const TArray<FString>& ImageIds, int32 NumResults, TCallback<FScenarioEstimate> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("model_id"), ModelId);
	if (!Prompt.IsEmpty())
	{
		Arguments->SetStringField(TEXT("prompt"), Prompt);
	}
	if (ImageIds.Num() > 0)
	{
		Arguments->SetArrayField(TEXT("images"), StringValues(ImageIds));
	}
	Arguments->SetNumberField(TEXT("num_results"), FMath::Clamp(NumResults, 1, 5));
	Arguments->SetBoolField(TEXT("dry_run"), true);
	BackendRef->CallTool(TEXT("prompt_spark"), Arguments, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FScenarioEstimate>(Reply));
			return;
		}
		FScenarioEstimate Estimate;
		if (!FindEstimate(Reply.Value, Estimate))
		{
			OnDone(TScenarioResult<FScenarioEstimate>::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario returned no price for Prompt Spark.")));
			return;
		}
		OnDone(TScenarioResult<FScenarioEstimate>::Ok(Estimate));
	});
}

void FScenarioApi::RunPromptSpark(const FString& ModelId, const FString& Prompt, const TArray<FString>& ImageIds, int32 NumResults, FScenarioReplyCallback OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("model_id"), ModelId);
	if (!Prompt.IsEmpty())
	{
		Arguments->SetStringField(TEXT("prompt"), Prompt);
	}
	if (ImageIds.Num() > 0)
	{
		Arguments->SetArrayField(TEXT("images"), StringValues(ImageIds));
	}
	Arguments->SetNumberField(TEXT("num_results"), FMath::Clamp(NumResults, 1, 5));
	BackendRef->CallTool(TEXT("prompt_spark"), Arguments, MoveTemp(OnDone), 120.0f);
}

void FScenarioApi::GetJob(const FString& JobId, TCallback<FScenarioJobInfo> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("job_id"), JobId);
	BackendRef->CallTool(TEXT("job_get"), Arguments, [OnDone](const FScenarioReply& Reply)
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

void FScenarioApi::WaitJobs(const TArray<FString>& JobIds, TCallback<FScenarioWaitResult> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	TArray<FString> Ids = JobIds;
	if (Ids.Num() > 32)
	{
		Ids.SetNum(32);
	}
	Arguments->SetArrayField(TEXT("job_ids"), StringValues(Ids));
	BackendRef->CallTool(TEXT("jobs_wait"), Arguments, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FScenarioWaitResult>(Reply));
			return;
		}
		FScenarioWaitResult Result;
		for (const TCHAR* Field : { TEXT("jobs"), TEXT("rows"), TEXT("results") })
		{
			for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Reply.Value, Field))
			{
				if (Row.IsValid() && Row->Type == EJson::Object)
				{
					Result.Jobs.Add(FScenarioJobInfo::FromJson(Row->AsObject()));
				}
			}
			if (Result.Jobs.Num() > 0)
			{
				break;
			}
		}
		Result.PendingJobIds = ScenarioJson::GetStringArray(Reply.Value, TEXT("pending_job_ids"));
		OnDone(TScenarioResult<FScenarioWaitResult>::Ok(Result));
	}, 200.0f);
}

void FScenarioApi::ListJobs(int32 Limit, TCallback<TArray<FScenarioJobInfo>> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetNumberField(TEXT("limit"), FMath::Clamp(Limit, 1, 50));
	BackendRef->CallTool(TEXT("jobs_list"), Arguments, [OnDone](const FScenarioReply& Reply)
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
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("job_id"), JobId);
	BackendRef->CallTool(TEXT("job_cancel"), Arguments, MoveTemp(OnDone));
}

void FScenarioApi::GetAsset(const FString& AssetId, TCallback<FScenarioAssetInfo> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("asset_id"), AssetId);
	BackendRef->CallTool(TEXT("asset_get"), Arguments, [OnDone, AssetId](const FScenarioReply& Reply)
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
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("asset_id"), AssetId);
	if (!Format.IsEmpty())
	{
		Arguments->SetStringField(TEXT("format"), Format);
	}
	BackendRef->CallTool(TEXT("asset_download"), Arguments, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FString>(Reply));
			return;
		}
		FString Url = FirstString(Reply.Value, { TEXT("url"), TEXT("download_url"), TEXT("downloadUrl") });
		if (Url.IsEmpty())
		{
			OnDone(Missing<FString>(TEXT("url")));
			return;
		}
		OnDone(TScenarioResult<FString>::Ok(Url));
	}, 120.0f);
}

void FScenarioApi::GetSchema(const FString& ModelId, FScenarioReplyCallback OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("model_id"), ModelId);
	BackendRef->CallTool(TEXT("model_schema_get"), Arguments, MoveTemp(OnDone));
}

void FScenarioApi::ListModels(const FString& Privacy, const FString& Modality, int32 Limit, const FString& PageToken, TCallback<FScenarioModelPage> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	TSharedPtr<FJsonObject> Filters = Args();
	if (!Privacy.IsEmpty())
	{
		Filters->SetStringField(TEXT("privacy"), Privacy);
	}
	if (!Modality.IsEmpty())
	{
		Filters->SetStringField(TEXT("modality"), Modality);
	}
	if (Privacy == TEXT("private"))
	{
		// Only ready models; this also leaves out deleted ones.
		Filters->SetStringField(TEXT("status"), TEXT("trained"));
	}
	if (Filters->Values.Num() > 0)
	{
		Arguments->SetObjectField(TEXT("filters"), Filters);
	}
	Arguments->SetNumberField(TEXT("limit"), FMath::Clamp(Limit, 1, 100));
	if (!PageToken.IsEmpty())
	{
		Arguments->SetStringField(TEXT("page_token"), PageToken);
	}
	BackendRef->CallTool(TEXT("models_list"), Arguments, [OnDone](const FScenarioReply& Reply)
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
				if (!Model.Id.IsEmpty())
				{
					Page.Models.Add(MoveTemp(Model));
				}
			}
		}
		Page.NextPageToken = ScenarioJson::GetString(Reply.Value, TEXT("nextPaginationToken"));
		OnDone(TScenarioResult<FScenarioModelPage>::Ok(Page));
	}, 90.0f);
}

void FScenarioApi::SearchModels(const FString& Query, bool bPublic, int32 Limit, TCallback<TArray<FScenarioModelSummary>> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("target"), TEXT("models"));
	Arguments->SetStringField(TEXT("query"), Query);
	Arguments->SetBoolField(TEXT("public"), bPublic);
	Arguments->SetBoolField(TEXT("include_urls"), true);
	Arguments->SetNumberField(TEXT("limit"), FMath::Clamp(Limit, 1, 100));
	BackendRef->CallTool(TEXT("search"), Arguments, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<TArray<FScenarioModelSummary>>(Reply));
			return;
		}
		TArray<FScenarioModelSummary> Models;
		for (const TCHAR* Field : { TEXT("results"), TEXT("hits"), TEXT("models") })
		{
			for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Reply.Value, Field))
			{
				if (Row.IsValid() && Row->Type == EJson::Object)
				{
					FScenarioModelSummary Model = FScenarioModelSummary::FromJson(Row->AsObject());
					if (!Model.Id.IsEmpty())
					{
						Models.Add(MoveTemp(Model));
					}
				}
			}
			if (Models.Num() > 0)
			{
				break;
			}
		}
		OnDone(TScenarioResult<TArray<FScenarioModelSummary>>::Ok(Models));
	});
}

void FScenarioApi::SearchAssets(const FString& Query, const FString& Kind, int32 Limit, int32 Offset, TCallback<TArray<FScenarioAssetInfo>> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("target"), TEXT("assets"));
	if (!Query.IsEmpty())
	{
		Arguments->SetStringField(TEXT("query"), Query);
	}
	TSharedPtr<FJsonObject> Filters = Args();
	if (!Kind.IsEmpty())
	{
		Filters->SetStringField(TEXT("kind"), Kind);
	}
	Arguments->SetObjectField(TEXT("filters"), Filters);
	if (Query.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Sort;
		Sort.Add(MakeShared<FJsonValueString>(TEXT("createdAt:desc")));
		Arguments->SetArrayField(TEXT("sort_by"), Sort);
		if (Kind.IsEmpty())
		{
			// search needs a query or a filter: every asset has a kind.
			Arguments->SetStringField(TEXT("filter"), TEXT("kind EXISTS"));
		}
	}
	Arguments->SetBoolField(TEXT("include_urls"), true);
	Arguments->SetNumberField(TEXT("limit"), FMath::Clamp(Limit, 1, 100));
	Arguments->SetNumberField(TEXT("offset"), FMath::Max(0, Offset));
	BackendRef->CallTool(TEXT("search"), Arguments, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<TArray<FScenarioAssetInfo>>(Reply));
			return;
		}
		TArray<FScenarioAssetInfo> Assets;
		for (const TCHAR* Field : { TEXT("results"), TEXT("hits"), TEXT("assets") })
		{
			for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Reply.Value, Field))
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
			if (Assets.Num() > 0)
			{
				break;
			}
		}
		OnDone(TScenarioResult<TArray<FScenarioAssetInfo>>::Ok(Assets));
	});
}

void FScenarioApi::ListAssets(const FString& Kind, int32 Limit, const FString& PageToken, TCallback<FScenarioAssetPage> OnDone)
{
	// search ranks the whole team's index before it narrows to the project, so a small project in a
	// large team can get an empty first page. assets_list reads the project itself.
	static const TMap<FString, TArray<FString>> Types = {
		{ TEXT("image"), { TEXT("txt2img"), TEXT("img2img"), TEXT("uploaded") } },
		{ TEXT("3d"), { TEXT("uploaded-3d"), TEXT("img23d"), TEXT("txt23d"), TEXT("3d23d") } },
		{ TEXT("video"), { TEXT("img2video"), TEXT("txt2video"), TEXT("uploaded-video") } },
		{ TEXT("audio"), { TEXT("txt2audio"), TEXT("audio2audio"), TEXT("uploaded-audio") } },
	};
	TSharedPtr<FJsonObject> Parameters = Args();
	if (const TArray<FString>* Group = Types.Find(Kind))
	{
		TArray<TSharedPtr<FJsonValue>> List;
		for (const FString& Type : *Group)
		{
			List.Add(MakeShared<FJsonValueString>(Type));
		}
		TSharedPtr<FJsonObject> Filters = Args();
		Filters->SetArrayField(TEXT("types"), List);
		Parameters->SetObjectField(TEXT("filters"), Filters);
	}
	Parameters->SetNumberField(TEXT("limit"), FMath::Clamp(Limit, 1, 100));
	if (!PageToken.IsEmpty())
	{
		Parameters->SetStringField(TEXT("page_token"), PageToken);
	}
	Parameters->SetBoolField(TEXT("include_urls"), true);
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("name"), TEXT("assets_list"));
	Arguments->SetObjectField(TEXT("parameters"), Parameters);
	BackendRef->CallTool(TEXT("scenario_tool_execute_read"), Arguments, [OnDone](const FScenarioReply& Reply)
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

void FScenarioApi::Recommend(const FString& Capability, const FString& Prompt, int32 Limit, FScenarioReplyCallback OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	if (!Capability.IsEmpty())
	{
		Arguments->SetStringField(TEXT("capability"), Capability);
	}
	if (!Prompt.IsEmpty())
	{
		Arguments->SetStringField(TEXT("prompt"), Prompt.Left(4000));
	}
	Arguments->SetStringField(TEXT("priority"), TEXT("quality"));
	Arguments->SetNumberField(TEXT("limit"), FMath::Clamp(Limit, 1, 10));
	BackendRef->CallTool(TEXT("recommend"), Arguments, MoveTemp(OnDone));
}

void FScenarioApi::ListTeams(TCallback<TArray<FScenarioTeam>> OnDone)
{
	BackendRef->CallTool(TEXT("teams_list"), Args(), [OnDone](const FScenarioReply& Reply)
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
					FScenarioProject Project;
					Project.Id = FirstString(ProjectRow->AsObject(), { TEXT("id"), TEXT("projectId") });
					Project.Name = ScenarioJson::GetString(ProjectRow->AsObject(), TEXT("name"));
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
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("team_id"), TeamId);
	Arguments->SetNumberField(TEXT("limit"), 100);
	BackendRef->CallTool(TEXT("projects_list"), Arguments, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<TArray<FScenarioProject>>(Reply));
			return;
		}
		TArray<FScenarioProject> Projects;
		for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Reply.Value, TEXT("projects")))
		{
			if (Row.IsValid() && Row->Type == EJson::Object)
			{
				FScenarioProject Project;
				Project.Id = FirstString(Row->AsObject(), { TEXT("id"), TEXT("projectId") });
				Project.Name = ScenarioJson::GetString(Row->AsObject(), TEXT("name"));
				if (!Project.Id.IsEmpty())
				{
					Projects.Add(Project);
				}
			}
		}
		OnDone(TScenarioResult<TArray<FScenarioProject>>::Ok(Projects));
	});
}

void FScenarioApi::UsageTotal(const FString& ProjectId, const FString& StartDate, const FString& EndDate, TCallback<double> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	if (!ProjectId.IsEmpty())
	{
		Arguments->SetStringField(TEXT("project_id"), ProjectId);
	}
	if (!StartDate.IsEmpty() && !EndDate.IsEmpty())
	{
		Arguments->SetStringField(TEXT("start_date"), StartDate);
		Arguments->SetStringField(TEXT("end_date"), EndDate);
	}
	BackendRef->CallTool(TEXT("usage"), Arguments, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<double>(Reply));
			return;
		}
		const TSharedPtr<FJsonObject> Totals = ScenarioJson::GetObject(Reply.Value, TEXT("totals"));
		if (!ScenarioJson::HasNumber(Totals, TEXT("totalCU")))
		{
			OnDone(Missing<double>(TEXT("totals.totalCU")));
			return;
		}
		OnDone(TScenarioResult<double>::Ok(ScenarioJson::GetNumber(Totals, TEXT("totalCU"))));
	});
}

void FScenarioApi::ListWorkflows(const FString& Privacy, const FString& Status, int32 Limit, const FString& PageToken, const TArray<FString>& Tags, FScenarioReplyCallback OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	if (!Privacy.IsEmpty())
	{
		Arguments->SetStringField(TEXT("privacy"), Privacy);
	}
	if (!Status.IsEmpty())
	{
		Arguments->SetStringField(TEXT("status"), Status);
	}
	Arguments->SetNumberField(TEXT("limit"), FMath::Clamp(Limit, 1, 100));
	if (!PageToken.IsEmpty())
	{
		Arguments->SetStringField(TEXT("page_token"), PageToken);
	}
	if (Tags.Num() > 0)
	{
		Arguments->SetField(TEXT("tags"), ScenarioJson::StringArrayValue(Tags));
	}
	BackendRef->CallTool(TEXT("workflows_list"), Arguments, MoveTemp(OnDone));
}

void FScenarioApi::GetWorkflow(const FString& WorkflowId, FScenarioReplyCallback OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("workflow_id"), WorkflowId);
	BackendRef->CallTool(TEXT("workflow_get"), Arguments, MoveTemp(OnDone));
}

void FScenarioApi::ListCollections(FScenarioReplyCallback OnDone)
{
	// collections_list is in the full toolset (catalog tool) on the hosted MCP.
	BackendRef->CallTool(TEXT("collections_list"), Args(), MoveTemp(OnDone));
}

void FScenarioApi::UploadStart(const FString& FileName, const FString& ContentType, const FString& Kind, int64 FileSize, TCallback<FScenarioUploadTicket> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("file_name"), FileName);
	Arguments->SetStringField(TEXT("content_type"), ContentType);
	Arguments->SetStringField(TEXT("kind"), Kind);
	Arguments->SetNumberField(TEXT("file_size"), static_cast<double>(FileSize));
	BackendRef->CallTool(TEXT("upload_asset"), Arguments, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FScenarioUploadTicket>(Reply));
			return;
		}
		FScenarioUploadTicket Ticket;
		Ticket.UploadId = FirstString(Reply.Value, { TEXT("upload_id"), TEXT("uploadId") });
		Ticket.AssetId = FirstString(Reply.Value, { TEXT("asset_id"), TEXT("assetId") });
		Ticket.PartSize = static_cast<int64>(ScenarioJson::GetNumber(Reply.Value, TEXT("part_size"), ScenarioJson::GetNumber(Reply.Value, TEXT("partSize"))));
		for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Reply.Value, TEXT("parts")))
		{
			if (Row.IsValid() && Row->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Part = Row->AsObject();
				const int32 Number = static_cast<int32>(ScenarioJson::GetNumber(Part, TEXT("part_number"), ScenarioJson::GetNumber(Part, TEXT("partNumber"))));
				const FString Url = FirstString(Part, { TEXT("upload_url"), TEXT("uploadUrl"), TEXT("url") });
				if (Number > 0 && !Url.IsEmpty())
				{
					Ticket.Parts.Emplace(Number, Url);
				}
			}
		}
		Ticket.Parts.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key < B.Key; });
		if (Ticket.AssetId.IsEmpty() && (Ticket.UploadId.IsEmpty() || Ticket.Parts.Num() == 0))
		{
			OnDone(TScenarioResult<FScenarioUploadTicket>::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario did not return upload URLs.")));
			return;
		}
		OnDone(TScenarioResult<FScenarioUploadTicket>::Ok(Ticket));
	});
}

void FScenarioApi::UploadComplete(const FString& UploadId, TCallback<FString> OnDone)
{
	TSharedPtr<FJsonObject> Arguments = Args();
	Arguments->SetStringField(TEXT("upload_id"), UploadId);
	BackendRef->CallTool(TEXT("upload_asset_complete"), Arguments, [OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone(FailFrom<FString>(Reply));
			return;
		}
		const FString AssetId = FirstString(Reply.Value, { TEXT("asset_id"), TEXT("assetId") });
		if (AssetId.IsEmpty())
		{
			FScenarioError Error;
			Error.Kind = EScenarioErrorKind::Timeout;
			Error.Message = TEXT("Scenario is still processing the upload.");
			Error.Detail = ScenarioJson::GetString(Reply.Value, TEXT("status"));
			OnDone(TScenarioResult<FString>::Fail(Error));
			return;
		}
		OnDone(TScenarioResult<FString>::Ok(AssetId));
	}, 150.0f);
}
