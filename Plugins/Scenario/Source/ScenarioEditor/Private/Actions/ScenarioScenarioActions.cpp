// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

// Scenario actions: status, models, prices, spending, jobs, library and references.

#include "Containers/Ticker.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "ScenarioActionHelpers.h"
#include "ScenarioActions.h"
#include "ScenarioAssets.h"
#include "ScenarioCatalog.h"
#include "ScenarioCU.h"
#include "ScenarioErrors.h"
#include "ScenarioLanes.h"
#include "ScenarioSchema.h"
#include "ScenarioSettings.h"

using namespace ScenarioActionHelpers;

namespace
{
	using FRunFn = TFunction<void(const TSharedPtr<FJsonObject>&, const FScenarioCaller&, FScenarioActionDone)>;

	FScenarioAction Make(const FString& Name, EScenarioActionKind Kind, const FString& Description, const TSharedPtr<FJsonObject>& Schema, FRunFn Run)
	{
		FScenarioAction Action;
		Action.Name = Name;
		Action.Kind = Kind;
		Action.Description = Description;
		Action.InputSchema = Schema;
		Action.Run = MoveTemp(Run);
		return Action;
	}

	/** A ledger row by local job id or Scenario job id. */
	const FScenarioJobRow* FindRow(FScenarioSession& Session, const FString& Id)
	{
		if (const FScenarioJobRow* Row = Session.Ledger().Find(Id))
		{
			return Row;
		}
		return Session.Ledger().FindByJob(Id);
	}

	FString ModelName(FScenarioSession& Session, const FString& ModelId)
	{
		const FScenarioCatalogModel* Model = Session.Catalog().Find(ModelId);
		return Model ? Model->Summary.Name : ModelId;
	}

	/** Names in Values the fields do not know: they would be dropped silently. */
	TArray<FString> UnknownNames(const TArray<FScenarioField>& Fields, const TSharedPtr<FJsonObject>& Values)
	{
		TArray<FString> Unknown;
		if (Values.IsValid())
		{
			for (const auto& Pair : ScenarioJson::Fields(Values))
			{
				if (!ScenarioSchema::Find(Fields, Pair.Key))
				{
					Unknown.Add(Pair.Key);
				}
			}
		}
		return Unknown;
	}

	/** Schema defaults and the plugin's presets, then the caller's values on top. */
	TSharedRef<FJsonObject> MergedValues(const TArray<FScenarioField>& Fields, const TSharedPtr<FJsonObject>& Presets, const TSharedPtr<FJsonObject>& Values)
	{
		TSharedRef<FJsonObject> Merged = ScenarioSchema::InitialValues(Fields, Presets);
		if (Values.IsValid())
		{
			for (const auto& Pair : ScenarioJson::Fields(Values))
			{
				Merged->SetField(Pair.Key, Pair.Value);
			}
		}
		return Merged;
	}

	/** Prices a request and answers with the quote, the way estimate_cost, estimate_workflow and estimate_prompt all do. */
	void PriceAndAnswer(FScenarioSession& Session, const FScenarioRunRequest& Request, const TArray<FString>& Warnings, const FString& NextTool, FScenarioActionDone Done)
	{
		const TSharedPtr<FJsonObject> Sent = Request.Parameters;
		Session.Price(Request, false, [Sent, Warnings, NextTool, Done](const TScenarioResult<FScenarioQuote>& Result)
		{
			if (!Result.bOk)
			{
				if (Result.Error.Kind == EScenarioErrorKind::EstimateJob)
				{
					Done(FScenarioActionResult::Fail(TEXT("EstimateJob"), FString::Printf(TEXT("The price check returned a job (%s). Automatic price checks are stopped for this session; check Jobs before anything else."), *Result.Error.JobId)));
					return;
				}
				Done(FScenarioActionResult::Fail(Result.Error));
				return;
			}
			const FScenarioQuote& Quote = Result.Value;
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("quote_id"), Quote.Id);
			Json->SetStringField(TEXT("cu_cost_exact"), Quote.Exact);
			Json->SetNumberField(TEXT("cu_cost"), Quote.CU);
			if (Quote.Discount > 0.0)
			{
				Json->SetNumberField(TEXT("discount_cu"), Quote.Discount);
			}
			Json->SetNumberField(TEXT("expires_in_seconds"), FMath::Max(0.0, FMath::RoundToDouble(Quote.ExpiresAt() - ScenarioNow())));
			if (Sent.IsValid())
			{
				Json->SetObjectField(TEXT("priced_parameters"), Sent);
			}
			if (Warnings.Num() > 0)
			{
				Json->SetField(TEXT("warnings"), ScenarioJson::StringArrayValue(Warnings));
			}
			Json->SetStringField(TEXT("next"), FString::Printf(TEXT("%s with quote_id and approved_cost \"%s\" (single use, same project)."), *NextTool, *Quote.Exact));
			Done(FScenarioActionResult::Ok(Json));
		});
	}

	/** Spends a quote: generate, run_workflow and approve_prompt. */
	void SpendQuote(EScenarioQuoteKind Kind, const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, FScenarioActionDone Done)
	{
		FScenarioActionResult Fail;
		UScenarioSubsystem* S = Subsystem(Fail, true);
		if (!S)
		{
			Done(Fail);
			return;
		}
		FScenarioSession& Session = S->Session();
		const FString QuoteId = Str(Args, TEXT("quote_id"));
		const FString Approved = Str(Args, TEXT("approved_cost"));
		if (QuoteId.IsEmpty() || Approved.IsEmpty())
		{
			Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("quote_id and approved_cost are required: price first, then pass the quote's cu_cost_exact.")));
			return;
		}
		const FScenarioQuote* Quote = Session.Quotes().FindById(QuoteId, ScenarioNow());
		if (!Quote)
		{
			Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("This quote is unknown, used or expired (quotes last 120 seconds). Price the request again.")));
			return;
		}
		if (Quote->Kind != Kind)
		{
			const TCHAR* Right = Quote->Kind == EScenarioQuoteKind::Workflow ? TEXT("run_workflow") : Quote->Kind == EScenarioQuoteKind::PromptSpark ? TEXT("approve_prompt") : TEXT("generate");
			Done(FScenarioActionResult::Fail(TEXT("Invalid"), FString::Printf(TEXT("This quote is for %s."), Right)));
			return;
		}

		FScenarioRunRequest Request;
		Request.Kind = Kind;
		Request.TargetId = Quote->TargetId;
		Request.Parameters = ScenarioJson::CloneObject(Quote->Parameters);
		Request.Source = Caller.Source;
		Request.Client = Caller.Client;
		Request.Destination = Str(Args, TEXT("destination"));
		if (!Request.Destination.IsEmpty() && !Request.Destination.StartsWith(TEXT("/")))
		{
			Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("destination must be a content folder such as /Game/Props.")));
			return;
		}
		if (Kind == EScenarioQuoteKind::Workflow)
		{
			Request.TargetName = Str(Args, TEXT("workflow_name"), Quote->TargetId);
		}
		else
		{
			Request.TargetName = Kind == EScenarioQuoteKind::PromptSpark ? FString(TEXT("Prompt Spark")) : ModelName(Session, Quote->TargetId);
			const FScenarioLane* Lane = FScenarioLanes::Get().LaneForModel(Quote->TargetId);
			Request.Lane = Str(Args, TEXT("lane"), Lane ? Lane->Id : FString());
		}
		const FString Exact = Quote->Exact;
		TSharedRef<FScenarioSession> Shared = S->SessionShared();
		Session.Submit(Request, QuoteId, Approved, [Shared, Exact, Kind, Done](const TScenarioResult<FString>& Result)
		{
			if (!Result.bOk)
			{
				Done(FScenarioActionResult::Fail(Result.Error));
				return;
			}
			const FScenarioJobRow* Row = Shared->Ledger().Find(Result.Value);
			TSharedRef<FJsonObject> Json = Row ? JobJson(*Row, false) : MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("job_id"), Result.Value);
			if (Kind == EScenarioQuoteKind::PromptSpark)
			{
				Json->SetStringField(TEXT("next"), TEXT("Call read_prompt_result."));
			}
			FScenarioActionResult Ok = FScenarioActionResult::Ok(Json);
			Ok.SpentExact = Exact;
			Done(Ok);
		});
	}

	TSharedRef<FJsonObject> ModelRow(const FScenarioCatalogModel& Model, bool bLanePick)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("id"), Model.Id());
		Json->SetStringField(TEXT("name"), Model.Summary.Name);
		Json->SetStringField(TEXT("modality"), Model.Modality);
		TArray<FString> Chips = Model.Chips;
		if (Chips.Num() > 4)
		{
			Chips.SetNum(4);
		}
		Json->SetField(TEXT("does"), ScenarioJson::StringArrayValue(Chips));
		if (!Model.Summary.Provider.IsEmpty())
		{
			Json->SetStringField(TEXT("provider"), Model.Summary.Provider);
		}
		TArray<FString> Tags;
		if (bLanePick)
		{
			Tags.Add(TEXT("lane_pick"));
		}
		if (Model.bFeatured)
		{
			Tags.Add(TEXT("featured"));
		}
		if (Model.bTeam)
		{
			Tags.Add(TEXT("team"));
		}
		if (Model.bLegacy)
		{
			Tags.Add(TEXT("legacy"));
		}
		if (Tags.Num() > 0)
		{
			Json->SetField(TEXT("tags"), ScenarioJson::StringArrayValue(Tags));
		}
		if (!Model.ReplacedBy.IsEmpty())
		{
			Json->SetStringField(TEXT("replaced_by"), Model.ReplacedBy);
		}
		if (!Model.Summary.ShortDescription.IsEmpty())
		{
			Json->SetStringField(TEXT("about"), Model.Summary.ShortDescription.Left(140));
		}
		return Json;
	}

	TSharedRef<FJsonObject> AssetRow(FScenarioSession& Session, const FScenarioAssetInfo& Asset)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("asset_id"), Asset.AssetId);
		Json->SetStringField(TEXT("kind"), Asset.Kind);
		if (!Asset.Type.IsEmpty())
		{
			Json->SetStringField(TEXT("type"), Asset.Type);
		}
		if (!Asset.Name.IsEmpty())
		{
			Json->SetStringField(TEXT("name"), Asset.Name);
		}
		if (Asset.Width > 0)
		{
			Json->SetStringField(TEXT("size"), FString::Printf(TEXT("%dx%d"), Asset.Width, Asset.Height));
		}
		if (!Asset.ModelId.IsEmpty())
		{
			Json->SetStringField(TEXT("model_id"), Asset.ModelId);
		}
		if (!Asset.Prompt.IsEmpty())
		{
			Json->SetStringField(TEXT("prompt"), Asset.Prompt.Left(160));
		}
		if (!Asset.ParentJobId.IsEmpty())
		{
			Json->SetStringField(TEXT("scenario_job_id"), Asset.ParentJobId);
			if (const FScenarioJobRow* Row = Session.Ledger().FindByJob(Asset.ParentJobId))
			{
				Json->SetStringField(TEXT("job_id"), Row->LocalId);
			}
		}
		return Json;
	}

	void SearchLibrary(const TSharedPtr<FJsonObject>& Args, const FString& Query, FScenarioActionDone Done)
	{
		FScenarioActionResult Fail;
		UScenarioSubsystem* S = Subsystem(Fail, true);
		if (!S)
		{
			Done(Fail);
			return;
		}
		TSharedRef<FScenarioSession> Session = S->SessionShared();
		const int32 Limit = Int(Args, TEXT("limit"), 20, 1, 50);
		const int32 Offset = Int(Args, TEXT("offset"), 0, 0, 10000);
		Session->Api().SearchAssets(Query, Str(Args, TEXT("kind")), Limit, Offset, [Session, Limit, Offset, Done](const TScenarioResult<TArray<FScenarioAssetInfo>>& Result)
		{
			if (!Result.bOk)
			{
				Done(FScenarioActionResult::Fail(Result.Error));
				return;
			}
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FScenarioAssetInfo& Asset : Result.Value)
			{
				Rows.Add(MakeShared<FJsonValueObject>(AssetRow(*Session, Asset)));
			}
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetArrayField(TEXT("assets"), Rows);
			if (Rows.Num() == Limit)
			{
				Json->SetNumberField(TEXT("next_offset"), Offset + Limit);
			}
			Json->SetStringField(TEXT("note"), TEXT("Asset ids go straight into file fields; nothing is uploaded again."));
			Done(FScenarioActionResult::Ok(Json));
		});
	}

	/** Model form fields, then the run request, for estimate_cost. */
	void ModelRequest(const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, TFunction<void(UScenarioSubsystem*, const FScenarioRunRequest&, const TArray<FString>&)> OnReady, FScenarioActionDone Done)
	{
		FScenarioActionResult Fail;
		UScenarioSubsystem* S = Subsystem(Fail, true);
		if (!S)
		{
			Done(Fail);
			return;
		}
		const FString ModelId = Str(Args, TEXT("model_id"));
		if (ModelId.IsEmpty())
		{
			Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("model_id is required (see list_models).")));
			return;
		}
		if (!UScenarioProjectSettings::Get().IsModelAllowed(ModelId))
		{
			Done(FScenarioActionResult::Fail(TEXT("Forbidden"), TEXT("This project does not allow that model (Project Settings > Plugins > Scenario > Models).")));
			return;
		}
		const TSharedPtr<FJsonObject> Values = Obj(Args, TEXT("parameters"));
		TWeakObjectPtr<UScenarioSubsystem> Weak(S);
		S->GetModelFields(ModelId, [Weak, ModelId, Values, Args, Caller, OnReady, Done](const TArray<FScenarioField>& Fields, const FScenarioError& Error)
		{
			UScenarioSubsystem* Sub = Weak.Get();
			if (!Sub)
			{
				Done(FScenarioActionResult::Fail(TEXT("Unavailable"), TEXT("The editor is shutting down.")));
				return;
			}
			if (Error.Kind != EScenarioErrorKind::None)
			{
				Done(FScenarioActionResult::Fail(Error));
				return;
			}
			const TSharedRef<FJsonObject> Merged = MergedValues(Fields, FScenarioLanes::Get().Presets(ModelId), Values);
			const TArray<FString> Problems = ScenarioSchema::Validate(Fields, Merged);
			if (Problems.Num() > 0)
			{
				Done(FScenarioActionResult::Fail(TEXT("Invalid"), FString::Join(Problems, TEXT(" ")) + TEXT(" See model_schema.")));
				return;
			}
			TArray<FString> Warnings;
			for (const FString& Name : UnknownNames(Fields, Values))
			{
				Warnings.Add(FString::Printf(TEXT("Ignored unknown parameter \"%s\"."), *Name));
			}
			FScenarioRunRequest Request;
			Request.Kind = EScenarioQuoteKind::Model;
			Request.TargetId = ModelId;
			Request.TargetName = ModelName(Sub->Session(), ModelId);
			const FScenarioLane* Lane = FScenarioLanes::Get().LaneForModel(ModelId);
			Request.Lane = Str(Args, TEXT("lane"), Lane ? Lane->Id : FString());
			Request.Parameters = ScenarioSchema::Payload(Fields, Merged);
			Request.Source = Caller.Source;
			Request.Client = Caller.Client;
			OnReady(Sub, Request, Warnings);
		});
	}

	TSharedRef<FJsonObject> FieldsJson(const TArray<FScenarioField>& Fields)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FScenarioField& Field : Fields)
		{
			Rows.Add(MakeShared<FJsonValueObject>(FieldJson(Field)));
		}
		Json->SetArrayField(TEXT("parameters"), Rows);
		return Json;
	}
}

void FScenarioActions::RegisterScenarioActions()
{
	const TArray<FString> Lanes = []()
	{
		TArray<FString> Ids;
		for (const FScenarioLane& Lane : FScenarioLanes::Get().All())
		{
			Ids.Add(Lane.Id);
		}
		return Ids;
	}();

	// scenario_status
	Register(Make(TEXT("scenario_status"), EScenarioActionKind::Read,
		TEXT("Sign-in state, Scenario team and project, plugin and engine versions, permissions in force, and job counts. Call first."),
		Object({}),
		[this](const TSharedPtr<FJsonObject>&, const FScenarioCaller& Caller, FScenarioActionDone Done)
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scenario"));
			Json->SetStringField(TEXT("plugin_version"), Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("unknown"));
			Json->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString(EVersionComponent::Patch));
			UScenarioSubsystem* S = UScenarioSubsystem::Get();
			if (S)
			{
				FScenarioSession& Session = S->Session();
				Json->SetBoolField(TEXT("signed_in"), Session.IsSignedIn());
				Json->SetBoolField(TEXT("ready"), Session.IsReady());
				Json->SetStringField(TEXT("auth"), Session.GetAuthMode() == EScenarioAuthMode::OAuth ? TEXT("oauth") : Session.GetAuthMode() == EScenarioAuthMode::ApiKey ? TEXT("api_key") : TEXT("none"));
				if (Session.GetWorkspace().IsSet())
				{
					TSharedRef<FJsonObject> Workspace = MakeShared<FJsonObject>();
					Workspace->SetStringField(TEXT("team"), Session.GetWorkspace().TeamName);
					Workspace->SetStringField(TEXT("project"), Session.GetWorkspace().ProjectName);
					Json->SetObjectField(TEXT("workspace"), Workspace);
				}
				Json->SetBoolField(TEXT("spending_disabled"), Session.IsSpendingDisabled());
				if (Session.IsEstimateAlarmRaised())
				{
					Json->SetBoolField(TEXT("estimate_alarm"), true);
				}
				if (Session.GetUsageThisMonth() >= 0.0)
				{
					Json->SetStringField(TEXT("project_cu_this_month"), ScenarioCU::Exact(Session.GetUsageThisMonth()));
				}
				Json->SetNumberField(TEXT("active_jobs"), Session.Ledger().Active().Num());
				Json->SetNumberField(TEXT("unresolved_jobs"), Session.Ledger().Unresolved().Num());
				if (Session.GetLastError().Kind != EScenarioErrorKind::None)
				{
					Json->SetStringField(TEXT("last_error"), Session.GetLastError().ToString());
				}
			}
			const UScenarioEditorSettings& Settings = UScenarioEditorSettings::Get();
			TSharedRef<FJsonObject> Permissions = MakeShared<FJsonObject>();
			Permissions->SetBoolField(TEXT("change_project"), Caller.bCanChangeProject);
			Permissions->SetBoolField(TEXT("run_python"), Caller.bCanRunPython);
			Permissions->SetBoolField(TEXT("person_approves_each_spend"), Settings.bApproveAgentSpend);
			Permissions->SetStringField(TEXT("spending"), TEXT("Every paid call needs a quote_id from an estimate tool and approved_cost equal to its cu_cost_exact. No CU cap is set by the plugin."));
			Json->SetObjectField(TEXT("permissions"), Permissions);
			if (StatusExtension)
			{
				StatusExtension(Json);
			}
			Done(FScenarioActionResult::Ok(Json));
		}));

	// list_models
	Register(Make(TEXT("list_models"), EScenarioActionKind::Read,
		TEXT("Scenario models filtered by lane, modality, capability chip or text. The lane's picks come first. At most 40 rows."),
		Object({
			{ TEXT("lane"), TEXT("string"), TEXT("Plugin lane; its picks come first."), FString(), Lanes },
			{ TEXT("modality"), TEXT("string"), FString(), FString(), { TEXT("image"), TEXT("video"), TEXT("3d"), TEXT("audio") } },
			{ TEXT("chip"), TEXT("string"), TEXT("Capability as in the web app: Generate, Edit, Upscale, Remove background, Texture...") },
			{ TEXT("query"), TEXT("string"), TEXT("Text search on name, provider and description.") },
			{ TEXT("source"), TEXT("string"), FString(), FString(), { TEXT("all"), TEXT("catalog"), TEXT("trained") } },
			{ TEXT("include_legacy"), TEXT("boolean") },
			{ TEXT("limit"), TEXT("integer"), TEXT("Default 20, max 40.") },
		}),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, true);
			if (!S)
			{
				Done(Fail);
				return;
			}
			TSharedRef<FScenarioSession> Session = S->SessionShared();
			auto Answer = [Session, Args, Done]()
			{
				const FString LaneId = Str(Args, TEXT("lane"));
				const FString Source = Str(Args, TEXT("source"), TEXT("all"));
				const int32 Limit = Int(Args, TEXT("limit"), 20, 1, 40);
				FScenarioCatalogQuery Query;
				Query.Text = Str(Args, TEXT("query"));
				Query.Modality = Str(Args, TEXT("modality"));
				Query.Chip = Str(Args, TEXT("chip"));
				Query.bShowLegacy = Bool(Args, TEXT("include_legacy"));
				Query.bTeamModels = Source != TEXT("catalog");
				TSet<FString> Picks;
				if (const FScenarioLane* Lane = FScenarioLanes::Get().Find(LaneId))
				{
					for (const FScenarioLaneModel& Pick : Lane->Models)
					{
						Query.PinnedIds.AddUnique(Pick.Id);
						Picks.Add(Pick.Id);
					}
					if (Query.Modality.IsEmpty() && Lane->Id != TEXT("material") && Lane->Id != TEXT("environment") && Lane->Id != TEXT("render"))
					{
						Query.Modality = Lane->Id == TEXT("3d") ? TEXT("3d") : Lane->Id;
					}
				}
				const UScenarioProjectSettings& Project = UScenarioProjectSettings::Get();
				TArray<TSharedPtr<FJsonValue>> Rows;
				int32 Matched = 0;
				for (const FScenarioCatalogModel& Model : Session->Catalog().Query(Query))
				{
					if (!Project.IsModelAllowed(Model.Id()) || (Source == TEXT("trained") && !Model.bTeam && !Model.bTrained))
					{
						continue;
					}
					++Matched;
					if (Rows.Num() < Limit)
					{
						Rows.Add(MakeShared<FJsonValueObject>(ModelRow(Model, Picks.Contains(Model.Id()))));
					}
				}
				TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetArrayField(TEXT("models"), Rows);
				Json->SetNumberField(TEXT("matched"), Matched);
				if (Matched > Rows.Num())
				{
					Json->SetStringField(TEXT("note"), TEXT("More models match: narrow with query, chip or modality."));
				}
				Done(FScenarioActionResult::Ok(Json));
			};
			FScenarioCatalog& Catalog = Session->Catalog();
			if (!Catalog.IsLoaded())
			{
				Catalog.LoadCache();
			}
			if (!Catalog.IsLoaded())
			{
				Catalog.Refresh(Session->Scope(), [Answer, Done](const FScenarioError& Error)
				{
					if (Error.Kind != EScenarioErrorKind::None)
					{
						Done(FScenarioActionResult::Fail(Error));
						return;
					}
					Answer();
				});
				return;
			}
			Catalog.RefreshIfNeeded(Session->Scope());
			Answer();
		}));

	// model_schema
	Register(Make(TEXT("model_schema"), EScenarioActionKind::Read,
		TEXT("A model's parameters: types, defaults, options, limits, which change the price, and file inputs (they take Scenario asset ids)."),
		Object({ { TEXT("model_id"), TEXT("string") } }, { TEXT("model_id") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, true);
			if (!S)
			{
				Done(Fail);
				return;
			}
			const FString ModelId = Str(Args, TEXT("model_id"));
			S->GetModelFields(ModelId, [ModelId, Done](const TArray<FScenarioField>& Fields, const FScenarioError& Error)
			{
				if (Error.Kind != EScenarioErrorKind::None)
				{
					Done(FScenarioActionResult::Fail(Error));
					return;
				}
				TSharedRef<FJsonObject> Json = FieldsJson(Fields);
				Json->SetStringField(TEXT("model_id"), ModelId);
				if (const TSharedPtr<FJsonObject> Presets = FScenarioLanes::Get().Presets(ModelId))
				{
					Json->SetObjectField(TEXT("plugin_defaults"), Presets);
				}
				if (const FScenarioLane* Lane = FScenarioLanes::Get().LaneForModel(ModelId))
				{
					Json->SetStringField(TEXT("lane"), Lane->Id);
					for (const FScenarioLaneModel& Pick : Lane->Models)
					{
						if (Pick.Id == ModelId && Pick.bUnrealReady)
						{
							Json->SetStringField(TEXT("unreal_ready_prompt"), FScenarioLanes::Get().UnrealReadyPrompt());
						}
					}
				}
				Json->SetStringField(TEXT("files"), TEXT("File fields take Scenario asset ids: from upload_reference, search_assets or a finished job's files."));
				Done(FScenarioActionResult::Ok(Json));
			});
		}));

	// recommend_models
	Register(Make(TEXT("recommend_models"), EScenarioActionKind::Read,
		TEXT("Scenario's model recommendation for a prompt and capability (txt2img, img23d, txt2audio...), or for a plugin lane."),
		Object({
			{ TEXT("prompt"), TEXT("string") },
			{ TEXT("capability"), TEXT("string") },
			{ TEXT("lane"), TEXT("string"), FString(), FString(), Lanes },
			{ TEXT("limit"), TEXT("integer") },
		}, { TEXT("prompt") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, true);
			if (!S)
			{
				Done(Fail);
				return;
			}
			FString Capability = Str(Args, TEXT("capability"));
			if (Capability.IsEmpty())
			{
				const FScenarioLane* Lane = FScenarioLanes::Get().Find(Str(Args, TEXT("lane"), TEXT("image")));
				Capability = Lane ? Lane->Capability : TEXT("txt2img");
			}
			S->Session().Api().Recommend(Capability, Str(Args, TEXT("prompt")), Int(Args, TEXT("limit"), 5, 1, 10), [Done](const FScenarioReply& Reply)
			{
				if (!Reply.bOk)
				{
					Done(FScenarioActionResult::Fail(Reply.Error));
					return;
				}
				TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetObjectField(TEXT("recommendation"), Reply.Value.IsValid() ? Reply.Value : MakeShared<FJsonObject>());
				Done(FScenarioActionResult::Ok(Json));
			});
		}));

	// estimate_cost
	Register(Make(TEXT("estimate_cost"), EScenarioActionKind::Read,
		TEXT("Free price check of a model run (dry run). Returns quote_id and cu_cost_exact; generate needs both. Quotes are single use and last 120 s."),
		Object({
			{ TEXT("model_id"), TEXT("string") },
			{ TEXT("parameters"), TEXT("object"), TEXT("Values by parameter name (model_schema). Plugin defaults fill the rest.") },
			{ TEXT("lane"), TEXT("string"), TEXT("Where results land; inferred from the model when omitted."), FString(), Lanes },
		}, { TEXT("model_id") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, FScenarioActionDone Done)
		{
			ModelRequest(Args, Caller, [Done](UScenarioSubsystem* S, const FScenarioRunRequest& Request, const TArray<FString>& Warnings)
			{
				PriceAndAnswer(S->Session(), Request, Warnings, TEXT("generate"), Done);
			}, Done);
		}));

	// generate
	{
		FScenarioAction Action = Make(TEXT("generate"), EScenarioActionKind::Spend,
			TEXT("SPENDS Creative Units. Runs the request priced by estimate_cost. Needs quote_id and approved_cost equal to its cu_cost_exact. Returns job_id; then wait_for_job."),
			Object({
				{ TEXT("quote_id"), TEXT("string") },
				{ TEXT("approved_cost"), TEXT("string"), TEXT("The quote's cu_cost_exact, verbatim.") },
				{ TEXT("destination"), TEXT("string"), TEXT("Content folder for the import, for example /Game/Props. Default: the plugin's folder.") },
				{ TEXT("lane"), TEXT("string"), FString(), FString(), Lanes },
			}, { TEXT("quote_id"), TEXT("approved_cost") }),
			[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, FScenarioActionDone Done)
			{
				SpendQuote(EScenarioQuoteKind::Model, Args, Caller, Done);
			});
		Action.bCosts = true;
		Register(MoveTemp(Action));
	}

	// job_status
	Register(Make(TEXT("job_status"), EScenarioActionKind::Read,
		TEXT("State, progress, cost, files and whether they are downloaded and imported, with the next step. Takes job_id or job_ids."),
		Object({ { TEXT("job_id"), TEXT("string") }, { TEXT("job_ids"), TEXT("array") } }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, false);
			if (!S)
			{
				Done(Fail);
				return;
			}
			TArray<FString> Ids = Strings(Args, TEXT("job_ids"));
			Ids.Append(Strings(Args, TEXT("job_id")));
			if (Ids.Num() == 0)
			{
				Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("job_id or job_ids is required.")));
				return;
			}
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FString& Id : Ids)
			{
				const FScenarioJobRow* Row = FindRow(S->Session(), Id);
				if (!Row)
				{
					Done(FScenarioActionResult::Fail(TEXT("NotFound"), FString::Printf(TEXT("No job %s in this project. list_local_jobs lists them; recover_cloud_job links a Scenario job."), *Id)));
					return;
				}
				Rows.Add(MakeShared<FJsonValueObject>(JobJson(*Row, true)));
			}
			if (Rows.Num() == 1)
			{
				Done(FScenarioActionResult::Ok(Rows[0]->AsObject()));
				return;
			}
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetArrayField(TEXT("jobs"), Rows);
			Done(FScenarioActionResult::Ok(Json));
		}));

	// wait_for_job
	{
		FScenarioAction Action = Make(TEXT("wait_for_job"), EScenarioActionKind::Read,
			TEXT("Waits until a job is finished and its files are downloaded, up to timeout_seconds (default 45, max 160; agent clients often cut calls at 60 s). Returns job_status, with still_running when it is not done yet."),
			Object({ { TEXT("job_id"), TEXT("string") }, { TEXT("timeout_seconds"), TEXT("integer") } }, { TEXT("job_id") }),
			[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
			{
				FScenarioActionResult Fail;
				UScenarioSubsystem* S = Subsystem(Fail, false);
				if (!S)
				{
					Done(Fail);
					return;
				}
				const FScenarioJobRow* First = FindRow(S->Session(), Str(Args, TEXT("job_id")));
				if (!First)
				{
					Done(FScenarioActionResult::Fail(TEXT("NotFound"), TEXT("No such job in this project.")));
					return;
				}
				const FString LocalId = First->LocalId;
				const double Deadline = FPlatformTime::Seconds() + Int(Args, TEXT("timeout_seconds"), 45, 1, 160);
				TWeakObjectPtr<UScenarioSubsystem> Weak(S);
				auto Check = [Weak, LocalId, Deadline, Done](float) -> bool
				{
					UScenarioSubsystem* Sub = Weak.Get();
					const FScenarioJobRow* Row = Sub ? Sub->Session().Ledger().Find(LocalId) : nullptr;
					if (!Row)
					{
						Done(FScenarioActionResult::Fail(TEXT("NotFound"), TEXT("The job left the list.")));
						return false;
					}
					const bool bSettled = (Row->IsFinal() && Row->State != EScenarioJobState::Success) || IsDelivered(*Row) || Row->IsUnresolved()
						|| (Row->Kind == TEXT("prompt_spark") && Row->State == EScenarioJobState::Success);
					if (Row->State == EScenarioJobState::Success && !IsDelivered(*Row) && Row->Kind != TEXT("prompt_spark") && !Sub->Session().IsDownloading(LocalId))
					{
						Sub->Session().DownloadFiles(LocalId);
					}
					if (bSettled || FPlatformTime::Seconds() >= Deadline)
					{
						TSharedRef<FJsonObject> Json = JobJson(*Row, false);
						if (!bSettled)
						{
							Json->SetBoolField(TEXT("still_running"), true);
							Json->SetStringField(TEXT("next"), TEXT("Call wait_for_job again; nothing is lost."));
						}
						Done(FScenarioActionResult::Ok(Json));
						return false;
					}
					return true;
				};
				if (Check(0.0f))
				{
					FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(Check), 0.5f);
				}
			});
		Action.TimeLimit = 170.0;
		Register(MoveTemp(Action));
	}

	// list_local_jobs
	Register(Make(TEXT("list_local_jobs"), EScenarioActionKind::Read,
		TEXT("This project's jobs, newest first: every request made from this editor by people, agents and scripts. Survives restarts."),
		Object({
			{ TEXT("state"), TEXT("string"), FString(), FString(), { TEXT("all"), TEXT("active"), TEXT("unresolved"), TEXT("finished") } },
			{ TEXT("limit"), TEXT("integer"), TEXT("Default 20, max 50.") },
		}),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, false);
			if (!S)
			{
				Done(Fail);
				return;
			}
			const FString Filter = Str(Args, TEXT("state"), TEXT("all"));
			const int32 Limit = Int(Args, TEXT("limit"), 20, 1, 50);
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FScenarioJobRow& Row : S->Session().Ledger().Rows())
			{
				const bool bKeep = Filter == TEXT("active") ? Row.IsActive() : Filter == TEXT("unresolved") ? Row.IsUnresolved() : Filter == TEXT("finished") ? Row.IsFinal() : Row.State != EScenarioJobState::Dismissed;
				if (bKeep)
				{
					Rows.Add(MakeShared<FJsonValueObject>(JobJson(Row, false)));
					if (Rows.Num() >= Limit)
					{
						break;
					}
				}
			}
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetArrayField(TEXT("jobs"), Rows);
			Done(FScenarioActionResult::Ok(Json));
		}));

	// list_generations
	Register(Make(TEXT("list_generations"), EScenarioActionKind::Read,
		TEXT("Recent jobs on Scenario for this project, from any tool, marked when they are already in this Unreal project."),
		Object({ { TEXT("limit"), TEXT("integer"), TEXT("Default 20, max 50.") } }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, true);
			if (!S)
			{
				Done(Fail);
				return;
			}
			TSharedRef<FScenarioSession> Session = S->SessionShared();
			Session->Api().ListJobs(Int(Args, TEXT("limit"), 20, 1, 50), [Session, Done](const TScenarioResult<TArray<FScenarioJobInfo>>& Result)
			{
				if (!Result.bOk)
				{
					Done(FScenarioActionResult::Fail(Result.Error));
					return;
				}
				TArray<TSharedPtr<FJsonValue>> Rows;
				for (const FScenarioJobInfo& Job : Result.Value)
				{
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("scenario_job_id"), Job.JobId);
					Row->SetStringField(TEXT("type"), Job.JobType);
					Row->SetStringField(TEXT("status"), Job.RawStatus);
					if (!Job.ModelId.IsEmpty())
					{
						Row->SetStringField(TEXT("model_id"), Job.ModelId);
					}
					if (!Job.WorkflowId.IsEmpty())
					{
						Row->SetStringField(TEXT("workflow_id"), Job.WorkflowId);
					}
					Row->SetNumberField(TEXT("assets"), Job.AssetIds.Num());
					if (Job.ChargedCU >= 0.0)
					{
						Row->SetStringField(TEXT("charged_cu"), ScenarioCU::Exact(Job.ChargedCU));
					}
					if (Job.CreatedAt > 0.0)
					{
						Row->SetStringField(TEXT("created_at"), ScenarioIsoTime(Job.CreatedAt));
					}
					if (const FScenarioJobRow* Local = Session->Ledger().FindByJob(Job.JobId))
					{
						Row->SetStringField(TEXT("job_id"), Local->LocalId);
					}
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				}
				TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetArrayField(TEXT("generations"), Rows);
				Json->SetStringField(TEXT("note"), TEXT("Rows without job_id are not in this project: recover_cloud_job links one."));
				Done(FScenarioActionResult::Ok(Json));
			});
		}));

	// list_workflows
	Register(Make(TEXT("list_workflows"), EScenarioActionKind::Read,
		TEXT("Ready workflows: the team's own, Scenario's featured ones, or both."),
		Object({
			{ TEXT("source"), TEXT("string"), FString(), FString(), { TEXT("all"), TEXT("team"), TEXT("featured") } },
			{ TEXT("limit"), TEXT("integer"), TEXT("Per source. Default 20, max 50.") },
		}),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, true);
			if (!S)
			{
				Done(Fail);
				return;
			}
			const FString Source = Str(Args, TEXT("source"), TEXT("all"));
			const int32 Limit = Int(Args, TEXT("limit"), 20, 1, 50);
			struct FCollect
			{
				TArray<TSharedPtr<FJsonValue>> Rows;
				int32 Pending = 0;
				FString Error;
			};
			TSharedRef<FCollect> Collect = MakeShared<FCollect>();
			auto Add = [Collect, Done](const FString& Tag, const FScenarioReply& Reply)
			{
				if (!Reply.bOk)
				{
					Collect->Error = Reply.Error.ToString();
				}
				for (const TSharedPtr<FJsonValue>& Value : ScenarioJson::GetArray(Reply.Value, TEXT("workflows")))
				{
					const TSharedPtr<FJsonObject> W = Value->AsObject();
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("workflow_id"), ScenarioJson::GetString(W, TEXT("id")));
					Row->SetStringField(TEXT("name"), ScenarioJson::GetString(W, TEXT("name")));
					const FString About = ScenarioJson::GetString(W, TEXT("shortDescription"), ScenarioJson::GetString(W, TEXT("description")));
					if (!About.IsEmpty())
					{
						Row->SetStringField(TEXT("about"), About.Left(160));
					}
					Row->SetNumberField(TEXT("inputs"), ScenarioJson::GetArray(W, TEXT("inputs")).Num());
					Row->SetStringField(TEXT("source"), Tag);
					Collect->Rows.Add(MakeShared<FJsonValueObject>(Row));
				}
				if (--Collect->Pending == 0)
				{
					if (Collect->Rows.Num() == 0 && !Collect->Error.IsEmpty())
					{
						FScenarioActionResult Failed;
						Failed.Error = Collect->Error;
						Done(Failed);
						return;
					}
					TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
					Json->SetArrayField(TEXT("workflows"), Collect->Rows);
					Done(FScenarioActionResult::Ok(Json));
				}
			};
			FScenarioApi& Api = S->Session().Api();
			const bool bTeam = Source != TEXT("featured");
			const bool bFeatured = Source != TEXT("team");
			Collect->Pending = (bTeam ? 1 : 0) + (bFeatured ? 1 : 0);
			if (bTeam)
			{
				Api.ListWorkflows(TEXT("private"), TEXT("ready"), Limit, FString(), {}, [Add](const FScenarioReply& Reply) { Add(TEXT("team"), Reply); });
			}
			if (bFeatured)
			{
				Api.ListWorkflows(TEXT("public"), TEXT("ready"), Limit, FString(), { TEXT("featured") }, [Add](const FScenarioReply& Reply) { Add(TEXT("featured"), Reply); });
			}
		}));

	// workflow_schema
	Register(Make(TEXT("workflow_schema"), EScenarioActionKind::Read,
		TEXT("A workflow's inputs, in the same form as model_schema."),
		Object({ { TEXT("workflow_id"), TEXT("string") } }, { TEXT("workflow_id") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, true);
			if (!S)
			{
				Done(Fail);
				return;
			}
			S->GetWorkflowFields(Str(Args, TEXT("workflow_id")), [Done](const TArray<FScenarioField>& Fields, const TSharedPtr<FJsonObject>& Workflow, const FScenarioError& Error)
			{
				if (Error.Kind != EScenarioErrorKind::None)
				{
					Done(FScenarioActionResult::Fail(Error));
					return;
				}
				TSharedRef<FJsonObject> Json = FieldsJson(Fields);
				Json->SetStringField(TEXT("name"), ScenarioJson::GetString(Workflow, TEXT("name")));
				Json->SetStringField(TEXT("about"), ScenarioJson::GetString(Workflow, TEXT("description")).Left(400));
				Done(FScenarioActionResult::Ok(Json));
			});
		}));

	// estimate_workflow
	Register(Make(TEXT("estimate_workflow"), EScenarioActionKind::Read,
		TEXT("Free price check of a workflow run. Returns quote_id and cu_cost_exact for run_workflow."),
		Object({ { TEXT("workflow_id"), TEXT("string") }, { TEXT("inputs"), TEXT("object"), TEXT("Values by input name (workflow_schema).") } }, { TEXT("workflow_id") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, true);
			if (!S)
			{
				Done(Fail);
				return;
			}
			const FString WorkflowId = Str(Args, TEXT("workflow_id"));
			const TSharedPtr<FJsonObject> Values = Obj(Args, TEXT("inputs"));
			TWeakObjectPtr<UScenarioSubsystem> Weak(S);
			S->GetWorkflowFields(WorkflowId, [Weak, WorkflowId, Values, Caller, Done](const TArray<FScenarioField>& Fields, const TSharedPtr<FJsonObject>& Workflow, const FScenarioError& Error)
			{
				UScenarioSubsystem* Sub = Weak.Get();
				if (!Sub || Error.Kind != EScenarioErrorKind::None)
				{
					Done(Sub ? FScenarioActionResult::Fail(Error) : FScenarioActionResult::Fail(TEXT("Unavailable"), TEXT("The editor is shutting down.")));
					return;
				}
				const TSharedRef<FJsonObject> Merged = MergedValues(Fields, nullptr, Values);
				const TArray<FString> Problems = ScenarioSchema::Validate(Fields, Merged);
				if (Problems.Num() > 0)
				{
					Done(FScenarioActionResult::Fail(TEXT("Invalid"), FString::Join(Problems, TEXT(" ")) + TEXT(" See workflow_schema.")));
					return;
				}
				TArray<FString> Warnings;
				for (const FString& Name : UnknownNames(Fields, Values))
				{
					Warnings.Add(FString::Printf(TEXT("Ignored unknown input \"%s\"."), *Name));
				}
				FScenarioRunRequest Request;
				Request.Kind = EScenarioQuoteKind::Workflow;
				Request.TargetId = WorkflowId;
				Request.TargetName = ScenarioJson::GetString(Workflow, TEXT("name"), WorkflowId);
				Request.Parameters = ScenarioSchema::Payload(Fields, Merged);
				Request.Source = Caller.Source;
				Request.Client = Caller.Client;
				PriceAndAnswer(Sub->Session(), Request, Warnings, TEXT("run_workflow"), Done);
			});
		}));

	// run_workflow
	{
		FScenarioAction Action = Make(TEXT("run_workflow"), EScenarioActionKind::Spend,
			TEXT("SPENDS Creative Units. Runs the workflow priced by estimate_workflow, with its quote_id and approved_cost. Returns job_id."),
			Object({
				{ TEXT("quote_id"), TEXT("string") },
				{ TEXT("approved_cost"), TEXT("string") },
				{ TEXT("destination"), TEXT("string") },
				{ TEXT("workflow_name"), TEXT("string"), TEXT("For the job list.") },
			}, { TEXT("quote_id"), TEXT("approved_cost") }),
			[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, FScenarioActionDone Done)
			{
				SpendQuote(EScenarioQuoteKind::Workflow, Args, Caller, Done);
			});
		Action.bCosts = true;
		Register(MoveTemp(Action));
	}

	// list_assets, search_assets
	Register(Make(TEXT("list_assets"), EScenarioActionKind::Read,
		TEXT("Assets in the Scenario project, newest first. Their ids go straight into file fields."),
		Object({
			{ TEXT("kind"), TEXT("string"), FString(), FString(), { TEXT("image"), TEXT("video"), TEXT("3d"), TEXT("audio") } },
			{ TEXT("limit"), TEXT("integer") },
			{ TEXT("offset"), TEXT("integer") },
		}),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			SearchLibrary(Args, FString(), Done);
		}));
	Register(Make(TEXT("search_assets"), EScenarioActionKind::Read,
		TEXT("Text search in the Scenario project's assets."),
		Object({
			{ TEXT("query"), TEXT("string") },
			{ TEXT("kind"), TEXT("string"), FString(), FString(), { TEXT("image"), TEXT("video"), TEXT("3d"), TEXT("audio") } },
			{ TEXT("limit"), TEXT("integer") },
			{ TEXT("offset"), TEXT("integer") },
		}, { TEXT("query") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			SearchLibrary(Args, Str(Args, TEXT("query")), Done);
		}));

	// list_collections
	Register(Make(TEXT("list_collections"), EScenarioActionKind::Read,
		TEXT("Collections in the Scenario project."),
		Object({}),
		[](const TSharedPtr<FJsonObject>&, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, true);
			if (!S)
			{
				Done(Fail);
				return;
			}
			S->Session().Api().ListCollections([Done](const FScenarioReply& Reply)
			{
				Done(Reply.bOk ? FScenarioActionResult::Ok(Reply.Value) : FScenarioActionResult::Fail(Reply.Error));
			});
		}));

	// estimate_prompt
	Register(Make(TEXT("estimate_prompt"), EScenarioActionKind::Read,
		TEXT("Free price of Prompt Spark: prompt variants written for a target model, optionally from reference images. Returns a quote for approve_prompt."),
		Object({
			{ TEXT("model_id"), TEXT("string"), TEXT("The model the prompt is for.") },
			{ TEXT("prompt"), TEXT("string") },
			{ TEXT("image_ids"), TEXT("array"), TEXT("Scenario asset ids of reference images.") },
			{ TEXT("num_results"), TEXT("integer"), TEXT("1 to 4, default 3.") },
		}, { TEXT("model_id"), TEXT("prompt") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, true);
			if (!S)
			{
				Done(Fail);
				return;
			}
			FScenarioRunRequest Request;
			Request.Kind = EScenarioQuoteKind::PromptSpark;
			Request.TargetId = Str(Args, TEXT("model_id"));
			Request.TargetName = TEXT("Prompt Spark");
			Request.Source = Caller.Source;
			Request.Client = Caller.Client;
			const TSharedRef<FJsonObject> Parameters = MakeShared<FJsonObject>();
			Parameters->SetStringField(TEXT("prompt"), Str(Args, TEXT("prompt")));
			const TArray<FString> Images = Strings(Args, TEXT("image_ids"));
			if (Images.Num() > 0)
			{
				Parameters->SetField(TEXT("images"), ScenarioJson::StringArrayValue(Images));
			}
			Parameters->SetNumberField(TEXT("num_results"), Int(Args, TEXT("num_results"), 3, 1, 4));
			Request.Parameters = Parameters;
			PriceAndAnswer(S->Session(), Request, {}, TEXT("approve_prompt"), Done);
		}));

	// approve_prompt
	{
		FScenarioAction Action = Make(TEXT("approve_prompt"), EScenarioActionKind::Spend,
			TEXT("SPENDS Creative Units. Runs the Prompt Spark call priced by estimate_prompt. Returns job_id for read_prompt_result."),
			Object({ { TEXT("quote_id"), TEXT("string") }, { TEXT("approved_cost"), TEXT("string") } }, { TEXT("quote_id"), TEXT("approved_cost") }),
			[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, FScenarioActionDone Done)
			{
				SpendQuote(EScenarioQuoteKind::PromptSpark, Args, Caller, Done);
			});
		Action.bCosts = true;
		Register(MoveTemp(Action));
	}

	// read_prompt_result
	Register(Make(TEXT("read_prompt_result"), EScenarioActionKind::Read,
		TEXT("The prompt variants of a Prompt Spark job (this editor session only)."),
		Object({ { TEXT("job_id"), TEXT("string") } }, { TEXT("job_id") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, false);
			if (!S)
			{
				Done(Fail);
				return;
			}
			const FScenarioJobRow* Row = FindRow(S->Session(), Str(Args, TEXT("job_id")));
			const TSharedPtr<FJsonObject> Result = Row ? S->Session().GetSparkResult(Row->LocalId) : nullptr;
			if (!Result.IsValid())
			{
				Done(FScenarioActionResult::Fail(TEXT("NotFound"), TEXT("No Prompt Spark result for this job in this editor session.")));
				return;
			}
			Done(FScenarioActionResult::Ok(Result));
		}));

	// capture_reference
	Register(Make(TEXT("capture_reference"), EScenarioActionKind::Editor,
		TEXT("Captures the level viewport to a PNG, or exports actors to a GLB, as a local reference for upload_reference. Writes only under Saved/Scenario."),
		Object({
			{ TEXT("source"), TEXT("string"), FString(), FString(), { TEXT("viewport"), TEXT("actors") } },
			{ TEXT("actor_ids"), TEXT("array"), TEXT("Names or labels; default the selection.") },
			{ TEXT("width"), TEXT("integer"), TEXT("Viewport capture, default 1280.") },
			{ TEXT("height"), TEXT("integer"), TEXT("Viewport capture, default 720.") },
		}),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, false);
			if (!S)
			{
				Done(Fail);
				return;
			}
			FString Error;
			FString Path;
			FString Description;
			FString Kind = TEXT("image");
			if (Str(Args, TEXT("source"), TEXT("viewport")) == TEXT("actors"))
			{
				const TArray<AActor*> Actors = ScenarioPlacement::ResolveTargets(Strings(Args, TEXT("actor_ids")));
				if (Actors.Num() == 0)
				{
					Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("No actors: pass actor_ids or select actors.")));
					return;
				}
				Path = S->ExportActorsToGlb(Actors, Error);
				Kind = TEXT("3d");
				TArray<FString> Names;
				for (const AActor* Actor : Actors)
				{
					Names.Add(ScenarioPlacement::Describe(Actor));
				}
				Description = FString::Printf(TEXT("GLB of %s"), *FString::Join(Names, TEXT(", ")));
			}
			else
			{
				Path = S->CaptureViewport(Int(Args, TEXT("width"), 1280, 256, 4096), Int(Args, TEXT("height"), 720, 256, 4096), Error, &Description);
			}
			if (Path.IsEmpty())
			{
				Done(FScenarioActionResult::Fail(TEXT("Capture"), Error));
				return;
			}
			const FString Id = NewId(TEXT("ref"));
			References().Add(Id, Path);
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("reference_id"), Id);
			Json->SetStringField(TEXT("kind"), Kind);
			Json->SetStringField(TEXT("file"), ProjectRelative(Path));
			Json->SetStringField(TEXT("captured"), Description);
			Json->SetStringField(TEXT("next"), TEXT("upload_reference with this reference_id to get a Scenario asset id."));
			Done(FScenarioActionResult::Ok(Json));
		}));

	// upload_reference
	Register(Make(TEXT("upload_reference"), EScenarioActionKind::Spend,
		TEXT("Uploads a captured reference (reference_id) or a file the person chose in the editor to Scenario. Free. Returns upload_id; poll reference_upload_status."),
		Object({ { TEXT("reference_id"), TEXT("string") }, { TEXT("path"), TEXT("string"), TEXT("Project-relative path of a file the plugin produced or the person chose.") } }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, true);
			if (!S)
			{
				Done(Fail);
				return;
			}
			FString Path;
			if (const FString* Found = References().Find(Str(Args, TEXT("reference_id"))))
			{
				Path = *Found;
			}
			else if (!Str(Args, TEXT("path")).IsEmpty())
			{
				Path = Str(Args, TEXT("path"));
				if (FPaths::IsRelative(Path))
				{
					Path = FPaths::Combine(FPaths::ProjectDir(), Path);
				}
				Path = FPaths::ConvertRelativePathToFull(Path);
			}
			if (Path.IsEmpty())
			{
				Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("reference_id (from capture_reference) or path is required.")));
				return;
			}
			if (Caller.Source == TEXT("agent") && !S->IsUploadAllowed(Path))
			{
				Done(FScenarioActionResult::Fail(TEXT("Permission"), TEXT("Agents can upload only files the plugin produced (capture_reference) or files the person chose in the editor this session.")));
				return;
			}
			const FString Handle = NewId(TEXT("up"));
			FUpload& Upload = Uploads().Add(Handle);
			Upload.Handle = Handle;
			Upload.Path = Path;
			S->Session().Upload(Path, [Handle](const TScenarioResult<FString>& Result)
			{
				if (FUpload* Entry = Uploads().Find(Handle))
				{
					Entry->State = Result.bOk ? TEXT("imported") : TEXT("failed");
					Entry->AssetId = Result.bOk ? Result.Value : FString();
					Entry->Error = Result.bOk ? FString() : Result.Error.ToString();
					Entry->Progress = Result.bOk ? 1.0f : Entry->Progress;
				}
			}, [Handle](float Progress)
			{
				if (FUpload* Entry = Uploads().Find(Handle))
				{
					Entry->Progress = Progress;
				}
			});
			const FUpload& Now = Uploads().FindChecked(Handle);
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("upload_id"), Handle);
			Json->SetStringField(TEXT("state"), Now.State);
			if (!Now.AssetId.IsEmpty())
			{
				Json->SetStringField(TEXT("asset_id"), Now.AssetId);
			}
			if (!Now.Error.IsEmpty())
			{
				Json->SetStringField(TEXT("error"), Now.Error);
			}
			Done(FScenarioActionResult::Ok(Json));
		}));

	// reference_upload_status
	Register(Make(TEXT("reference_upload_status"), EScenarioActionKind::Read,
		TEXT("State of an upload_reference call: uploading, imported (with asset_id) or failed."),
		Object({ { TEXT("upload_id"), TEXT("string") } }, { TEXT("upload_id") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			const FUpload* Upload = Uploads().Find(Str(Args, TEXT("upload_id")));
			if (!Upload)
			{
				Done(FScenarioActionResult::Fail(TEXT("NotFound"), TEXT("Unknown upload_id (uploads are kept for this editor session).")));
				return;
			}
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("upload_id"), Upload->Handle);
			Json->SetStringField(TEXT("state"), Upload->State);
			Json->SetNumberField(TEXT("progress"), FMath::RoundToDouble(Upload->Progress * 100.0) / 100.0);
			if (!Upload->AssetId.IsEmpty())
			{
				Json->SetStringField(TEXT("asset_id"), Upload->AssetId);
			}
			if (!Upload->Error.IsEmpty())
			{
				Json->SetStringField(TEXT("error"), Upload->Error);
			}
			Done(FScenarioActionResult::Ok(Json));
		}));

	// recover_local_job
	Register(Make(TEXT("recover_local_job"), EScenarioActionKind::Editor,
		TEXT("Resolves jobs whose outcome is unknown: \"check\" matches them against Scenario's job list; \"dismiss\" hides a finished job. Only the person can mark a request as not sent."),
		Object({ { TEXT("job_id"), TEXT("string") }, { TEXT("action"), TEXT("string"), FString(), FString(), { TEXT("check"), TEXT("dismiss") } } }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, true);
			if (!S)
			{
				Done(Fail);
				return;
			}
			TSharedRef<FScenarioSession> Session = S->SessionShared();
			const FString Action = Str(Args, TEXT("action"), TEXT("check"));
			const FString Id = Str(Args, TEXT("job_id"));
			if (Action == TEXT("not_sent"))
			{
				Done(FScenarioActionResult::Fail(TEXT("Permission"), TEXT("Only the person can mark a request as not sent (Scenario > Jobs), because it allows the same paid request again.")));
				return;
			}
			if (Action == TEXT("dismiss"))
			{
				const FScenarioJobRow* Row = FindRow(*Session, Id);
				if (!Row || !Row->IsFinal())
				{
					Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("Only a finished job can be dismissed.")));
					return;
				}
				Session->Dismiss(Row->LocalId);
				TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetStringField(TEXT("dismissed"), Row->LocalId);
				Done(FScenarioActionResult::Ok(Json));
				return;
			}
			Session->CheckUnresolved([Session, Id, Done](int32 Linked, const FScenarioError& Error)
			{
				if (Error.Kind != EScenarioErrorKind::None)
				{
					Done(FScenarioActionResult::Fail(Error));
					return;
				}
				TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetNumberField(TEXT("linked"), Linked);
				Json->SetNumberField(TEXT("still_unresolved"), Session->Ledger().Unresolved().Num());
				if (const FScenarioJobRow* Row = Id.IsEmpty() ? nullptr : FindRow(*Session, Id))
				{
					Json->SetObjectField(TEXT("job"), JobJson(*Row, false));
				}
				if (Session->Ledger().Unresolved().Num() > 0)
				{
					Json->SetStringField(TEXT("next"), TEXT("No clear match yet. Ask the person to check Scenario > Jobs; never resend the same request meanwhile."));
				}
				Done(FScenarioActionResult::Ok(Json));
			});
		}));

	// recover_cloud_job
	Register(Make(TEXT("recover_cloud_job"), EScenarioActionKind::Editor,
		TEXT("Links a Scenario job (scenario_job_id from list_generations) into this project's job list and downloads its files. Free."),
		Object({ { TEXT("scenario_job_id"), TEXT("string") }, { TEXT("lane"), TEXT("string"), FString(), FString(), Lanes } }, { TEXT("scenario_job_id") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, true);
			if (!S)
			{
				Done(Fail);
				return;
			}
			TSharedRef<FScenarioSession> Session = S->SessionShared();
			const FString JobId = Str(Args, TEXT("scenario_job_id"));
			if (const FScenarioJobRow* Existing = Session->Ledger().FindByJob(JobId))
			{
				Done(FScenarioActionResult::Ok(JobJson(*Existing, false)));
				return;
			}
			const FString LaneArg = Str(Args, TEXT("lane"));
			Session->Api().GetJob(JobId, [Session, JobId, LaneArg, Done](const TScenarioResult<FScenarioJobInfo>& Result)
			{
				if (!Result.bOk)
				{
					Done(FScenarioActionResult::Fail(Result.Error));
					return;
				}
				const FScenarioLane* Lane = FScenarioLanes::Get().LaneForModel(Result.Value.ModelId);
				Session->Recover(Result.Value, LaneArg.IsEmpty() && Lane ? Lane->Id : LaneArg);
				const FScenarioJobRow* Row = Session->Ledger().FindByJob(JobId);
				Done(Row ? FScenarioActionResult::Ok(JobJson(*Row, false)) : FScenarioActionResult::Fail(TEXT("Local"), TEXT("The job could not be added to the list.")));
			});
		}));

	// cancel_job
	Register(Make(TEXT("cancel_job"), EScenarioActionKind::Editor,
		TEXT("Asks Scenario to cancel a queued or running job. Whether CU are refunded is Scenario's decision."),
		Object({ { TEXT("job_id"), TEXT("string") } }, { TEXT("job_id") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, true);
			if (!S)
			{
				Done(Fail);
				return;
			}
			TSharedRef<FScenarioSession> Session = S->SessionShared();
			const FScenarioJobRow* Row = FindRow(*Session, Str(Args, TEXT("job_id")));
			if (!Row)
			{
				Done(FScenarioActionResult::Fail(TEXT("NotFound"), TEXT("No such job in this project.")));
				return;
			}
			const FString LocalId = Row->LocalId;
			Session->CancelJob(LocalId, [Session, LocalId, Done](const FScenarioReply& Reply)
			{
				if (!Reply.bOk)
				{
					Done(FScenarioActionResult::Fail(Reply.Error));
					return;
				}
				const FScenarioJobRow* After = Session->Ledger().Find(LocalId);
				Done(After ? FScenarioActionResult::Ok(JobJson(*After, false)) : FScenarioActionResult::Ok(nullptr));
			});
		}));
}
