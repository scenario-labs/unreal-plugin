// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioMcpClient.h"

#include "Misc/Base64.h"
#include "ScenarioCoreModule.h"
#include "ScenarioErrors.h"
#include "ScenarioJson.h"
#include "ScenarioRpc.h"

const TCHAR* FScenarioMcpClient::DefaultEndpoint = TEXT("https://mcp.scenario.com/mcp?toolsets=full");
const TCHAR* FScenarioMcpClient::ProtocolVersion = TEXT("2025-03-26");

FScenarioApiKeyAuth::FScenarioApiKeyAuth(const FString& InKey, const FString& InSecret)
	: Key(InKey.TrimStartAndEnd())
	, Secret(InSecret.TrimStartAndEnd())
{
}

bool FScenarioApiKeyAuth::HasCredentials() const
{
	return !Key.IsEmpty() && !Secret.IsEmpty();
}

FString FScenarioApiKeyAuth::AuthorizationHeader() const
{
	return TEXT("Basic ") + FBase64::Encode(Key + TEXT(":") + Secret);
}

FScenarioMcpClient::FScenarioMcpClient(TSharedRef<IScenarioHttpTransport> InTransport, const FString& InEndpoint)
	: Transport(InTransport)
	, Endpoint(InEndpoint)
{
	bSpendingDisabled = FPlatformMisc::GetEnvironmentVariable(TEXT("SCENARIO_NO_SPEND")) == TEXT("1");
}

void FScenarioMcpClient::SetAuth(TSharedPtr<IScenarioAuthProvider> InAuth)
{
	Auth = InAuth;
	ResetSession();
}

void FScenarioMcpClient::SetWorkspace(const FString& InTeamId, const FString& InProjectId)
{
	TeamId = InTeamId;
	ProjectId = InProjectId;
}

bool FScenarioMcpClient::HasCredentials() const
{
	return Auth.IsValid() && Auth->HasCredentials();
}

void FScenarioMcpClient::ResetSession()
{
	SessionId.Reset();
	bInitialized = false;
	bInitializing = false;
	++SessionGeneration;
	TArray<FInitWaiter> Waiters = MoveTemp(InitWaiters);
	InitWaiters.Reset();
	FScenarioReply Reset = FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("The Scenario connection was reset. Try again."));
	Reset.Error.bNotSent = true;
	for (FInitWaiter& Waiter : Waiters)
	{
		Waiter(Reset);
	}
}

const TArray<FString>& FScenarioMcpClient::ReadTools()
{
	static const TArray<FString> Tools = {
		TEXT("model_schema_get"), TEXT("models_list"), TEXT("model_get"), TEXT("job_get"), TEXT("jobs_list"), TEXT("jobs_wait"), TEXT("asset_get"),
		TEXT("asset_download"), TEXT("assets_list"), TEXT("recommend"), TEXT("search"), TEXT("usage"), TEXT("teams_list"),
		TEXT("projects_list"), TEXT("workflows_list"), TEXT("workflow_get"), TEXT("collections_list"),
		// Runs read-class catalog tools only; Scenario refuses any other.
		TEXT("scenario_tool_execute_read"),
	};
	return Tools;
}

const TArray<FString>& FScenarioMcpClient::SubmitTools()
{
	static const TArray<FString> Tools = {
		TEXT("model_run"), TEXT("prompt_spark"), TEXT("upload_asset"), TEXT("upload_asset_complete"), TEXT("job_cancel"), TEXT("workflow_run"),
	};
	return Tools;
}

bool FScenarioMcpClient::IsDryRun(const FString& Tool, const TSharedPtr<FJsonObject>& Arguments)
{
	return (Tool == TEXT("model_run") || Tool == TEXT("workflow_run") || Tool == TEXT("prompt_spark"))
		&& ScenarioJson::GetBool(Arguments, TEXT("dry_run"), false);
}

bool FScenarioMcpClient::IsSubmission(const FString& Tool, const TSharedPtr<FJsonObject>& Arguments)
{
	if (IsDryRun(Tool, Arguments))
	{
		return false;
	}
	// Anything not known to be a read is treated as able to spend.
	return SubmitTools().Contains(Tool) || !ReadTools().Contains(Tool);
}

bool FScenarioMcpClient::IsRetryable(const FString& Tool, const TSharedPtr<FJsonObject>& Arguments)
{
	// A free price check is a read.
	return IsDryRun(Tool, Arguments) || ReadTools().Contains(Tool);
}

FString FScenarioMcpClient::CreatedJobId(const TSharedPtr<FJsonObject>& Value)
{
	if (!Value.IsValid())
	{
		return FString();
	}
	for (const TCHAR* Key : { TEXT("job_id"), TEXT("jobId") })
	{
		const FString Id = ScenarioJson::GetString(Value, Key);
		if (!Id.IsEmpty())
		{
			return Id;
		}
	}
	for (const TCHAR* Key : { TEXT("job"), TEXT("inference") })
	{
		const TSharedPtr<FJsonObject> Inner = ScenarioJson::GetObject(Value, Key);
		if (Inner.IsValid())
		{
			FString Id = ScenarioJson::GetString(Inner, TEXT("id"));
			if (Id.IsEmpty())
			{
				Id = ScenarioJson::GetString(Inner, TEXT("jobId"));
			}
			if (!Id.IsEmpty())
			{
				return Id;
			}
		}
	}
	return FString();
}

double FScenarioMcpClient::RetryDelay(int32 AttemptIndex, double RetryAfterSeconds)
{
	if (RetryAfterSeconds >= 0.0)
	{
		return RetryAfterSeconds > MaxRetryAfterSeconds ? -1.0 : RetryAfterSeconds;
	}
	const double Base = FMath::Min(8.0, 0.5 * FMath::Pow(2.0, static_cast<double>(AttemptIndex)));
	return Base * (0.5 + 0.5 * FMath::FRand());
}

void FScenarioMcpClient::CallTool(const FString& Tool, const TSharedPtr<FJsonObject>& InArguments, FScenarioReplyCallback OnDone, float TimeoutSeconds)
{
	if (!HasCredentials())
	{
		FScenarioReply Reply = FScenarioReply::Fail(EScenarioErrorKind::Auth, TEXT("Sign in to Scenario first."));
		Reply.Error.bNotSent = true;
		OnDone(Reply);
		return;
	}
	TSharedPtr<FJsonObject> Arguments = InArguments.IsValid() ? ScenarioJson::CloneObject(InArguments) : MakeShared<FJsonObject>();
	const bool bSubmit = IsSubmission(Tool, Arguments);
	// Cancelling can only lower spend, so the switch lets it through.
	if (bSubmit && bSpendingDisabled && Tool != TEXT("job_cancel"))
	{
		FScenarioReply Reply = FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("Spending is turned off (SCENARIO_NO_SPEND=1), so nothing was sent."));
		Reply.Error.bNotSent = true;
		OnDone(Reply);
		return;
	}
	if (Auth->NeedsWorkspaceArguments() && Tool != TEXT("teams_list"))
	{
		// A catalog read takes the workspace inside the parameters of the tool it runs.
		TSharedPtr<FJsonObject> Target = Arguments;
		if (Tool == TEXT("scenario_tool_execute_read"))
		{
			Target = ScenarioJson::GetObject(Arguments, TEXT("parameters"));
			if (!Target.IsValid())
			{
				Target = MakeShared<FJsonObject>();
				Arguments->SetObjectField(TEXT("parameters"), Target);
			}
		}
		if (!TeamId.IsEmpty() && !Target->HasField(TEXT("team_id")))
		{
			Target->SetStringField(TEXT("team_id"), TeamId);
		}
		if (!ProjectId.IsEmpty() && !Target->HasField(TEXT("project_id")) && Tool != TEXT("projects_list"))
		{
			Target->SetStringField(TEXT("project_id"), ProjectId);
		}
	}
	const int32 Attempts = IsRetryable(Tool, Arguments) ? ReadAttempts : 1;
	Attempt(Tool, Arguments, TimeoutSeconds, bSubmit, 0, Attempts, MoveTemp(OnDone));
}

void FScenarioMcpClient::Attempt(const FString& Tool, TSharedPtr<FJsonObject> Arguments, float TimeoutSeconds, bool bSubmit, int32 AttemptIndex, int32 Attempts, FScenarioReplyCallback OnDone)
{
	TWeakPtr<FScenarioMcpClient> WeakThis = AsShared();
	CallOnce(Tool, Arguments, TimeoutSeconds, bSubmit, [WeakThis, Tool, Arguments, TimeoutSeconds, bSubmit, AttemptIndex, Attempts, OnDone](const FScenarioReply& Reply)
	{
		const TSharedPtr<FScenarioMcpClient> This = WeakThis.Pin();
		if (Reply.bOk)
		{
			if (IsDryRun(Tool, Arguments))
			{
				const FString JobId = CreatedJobId(Reply.Value);
				if (!JobId.IsEmpty())
				{
					if (This.IsValid())
					{
						This->bEstimateAlarm = true;
					}
					UE_LOG(LogScenario, Warning, TEXT("A price check for %s returned job %s. Automatic price checks are off for this session."), *Tool, *JobId);
					FScenarioReply Alarm = FScenarioReply::Fail(EScenarioErrorKind::EstimateJob,
						FString::Printf(TEXT("Scenario returned a job for a price check, so it may have started a paid run. Automatic price checks are off for this session. Check job %s."), *JobId));
					Alarm.Error.JobId = JobId;
					OnDone(Alarm);
					return;
				}
			}
			OnDone(Reply);
			return;
		}
		if (This.IsValid() && AttemptIndex + 1 < Attempts && FScenarioErrors::IsRetryableKind(Reply.Error.Kind))
		{
			const double Delay = RetryDelay(AttemptIndex, Reply.Error.RetryAfterSeconds);
			if (Delay >= 0.0)
			{
				UE_LOG(LogScenario, Verbose, TEXT("Retrying %s in %.1f s (%s)."), *Tool, Delay, LexToString(Reply.Error.Kind));
				This->Transport->Delay(static_cast<float>(Delay), [WeakThis, Tool, Arguments, TimeoutSeconds, bSubmit, AttemptIndex, Attempts, OnDone]()
				{
					if (const TSharedPtr<FScenarioMcpClient> Again = WeakThis.Pin())
					{
						Again->Attempt(Tool, Arguments, TimeoutSeconds, bSubmit, AttemptIndex + 1, Attempts, OnDone);
					}
				});
				return;
			}
		}
		OnDone(Reply);
	});
}

namespace
{
	FString FirstText(const TArray<TSharedPtr<FJsonValue>>& Content)
	{
		for (const TSharedPtr<FJsonValue>& Item : Content)
		{
			if (Item.IsValid() && Item->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Object = Item->AsObject();
				if (ScenarioJson::GetString(Object, TEXT("type")) == TEXT("text"))
				{
					return ScenarioJson::GetString(Object, TEXT("text"));
				}
			}
		}
		return FString();
	}

	FScenarioReply ErrorFromDetail(const FString& RawDetail, int32 Status = 0)
	{
		const FString Detail = FScenarioErrors::Scrub(RawDetail);
		const EScenarioErrorKind Kind = FScenarioErrors::Classify(Detail, Status);
		FScenarioReply Reply = FScenarioReply::Fail(Kind, FScenarioErrors::MessageFor(Kind, Detail), Detail.Left(2000));
		Reply.Error.HttpStatus = Status;
		return Reply;
	}
}

void FScenarioMcpClient::CallOnce(const FString& Tool, TSharedPtr<FJsonObject> Arguments, float TimeoutSeconds, bool bSubmit, FScenarioReplyCallback OnDone)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), Tool);
	Params->SetObjectField(TEXT("arguments"), Arguments);
	Rpc(TEXT("tools/call"), Params, TimeoutSeconds, [bSubmit, OnDone](const FScenarioReply& Rpc)
	{
		if (!Rpc.bOk)
		{
			FScenarioReply Out = Rpc;
			if (bSubmit && FScenarioErrors::IsAmbiguousKind(Out.Error.Kind) && !Out.Error.bNotSent)
			{
				Out.Error.bAmbiguous = true;
				Out.Error.Message = TEXT("The request status is unknown: Scenario may have accepted it. Check recent jobs before sending it again.");
			}
			OnDone(Out);
			return;
		}
		const TSharedPtr<FJsonObject> Result = Rpc.Value;
		const TSharedPtr<FJsonObject> Structured = ScenarioJson::GetObject(Result, TEXT("structuredContent"));
		const TArray<TSharedPtr<FJsonValue>> Content = ScenarioJson::GetArray(Result, TEXT("content"));
		if (ScenarioJson::GetBool(Result, TEXT("isError"), false))
		{
			FString Detail = FirstText(Content);
			if (Structured.IsValid() && Structured->Values.Num() > 0)
			{
				Detail = ScenarioJson::Write(Structured);
			}
			OnDone(ErrorFromDetail(Detail));
			return;
		}
		if (Structured.IsValid())
		{
			if (bSubmit && Structured->Values.Num() == 0)
			{
				FScenarioReply Empty = FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario returned an empty reply. Check recent jobs before sending it again."));
				Empty.Error.bAmbiguous = true;
				OnDone(Empty);
				return;
			}
			OnDone(FScenarioReply::Ok(Structured));
			return;
		}
		for (const TSharedPtr<FJsonValue>& Item : Content)
		{
			if (!Item.IsValid() || Item->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Object = Item->AsObject();
			if (ScenarioJson::GetString(Object, TEXT("type")) != TEXT("text"))
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Parsed = ScenarioJson::ParseObject(ScenarioJson::GetString(Object, TEXT("text")));
			if (Parsed.IsValid())
			{
				if (bSubmit && Parsed->Values.Num() == 0)
				{
					break;
				}
				OnDone(FScenarioReply::Ok(Parsed));
				return;
			}
		}
		FScenarioReply None = FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario returned no usable result."));
		None.Error.bAmbiguous = bSubmit;
		OnDone(None);
	});
}

void FScenarioMcpClient::Rpc(const FString& Method, TSharedPtr<FJsonObject> Params, float TimeoutSeconds, FScenarioReplyCallback OnDone)
{
	TWeakPtr<FScenarioMcpClient> WeakThis = AsShared();
	EnsureInitialized(TimeoutSeconds, [WeakThis, Method, Params, TimeoutSeconds, OnDone](const FScenarioReply& Init)
	{
		const TSharedPtr<FScenarioMcpClient> This = WeakThis.Pin();
		if (!Init.bOk || !This.IsValid())
		{
			FScenarioReply Out = Init.bOk ? FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("The Scenario client was closed.")) : Init;
			Out.Error.bNotSent = true;
			OnDone(Out);
			return;
		}
		const int64 RequestId = This->TakeId();
		auto HandleEnvelope = [OnDone](const FScenarioReply& Reply)
		{
			if (!Reply.bOk)
			{
				OnDone(Reply);
				return;
			}
			const TSharedPtr<FJsonObject> Envelope = Reply.Value;
			if (Envelope->HasField(TEXT("error")))
			{
				const TSharedPtr<FJsonValue> RpcError = Envelope->TryGetField(TEXT("error"));
				const FString Detail = RpcError->Type == EJson::String ? RpcError->AsString() : ScenarioJson::WriteValue(RpcError);
				OnDone(ErrorFromDetail(Detail));
				return;
			}
			const TSharedPtr<FJsonObject> Result = ScenarioJson::GetObject(Envelope, TEXT("result"));
			if (!Result.IsValid())
			{
				OnDone(FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario returned an unexpected reply structure.")));
				return;
			}
			OnDone(FScenarioReply::Ok(Result));
		};
		This->Post(Method, Params, RequestId, TimeoutSeconds, true, true, [WeakThis, Method, Params, RequestId, TimeoutSeconds, HandleEnvelope](const FScenarioReply& Reply)
		{
			const TSharedPtr<FScenarioMcpClient> Self = WeakThis.Pin();
			if (Reply.bOk || !Reply.Error.bSessionExpired || !Self.IsValid())
			{
				HandleEnvelope(Reply);
				return;
			}
			// The server did not dispatch the request: renew the session and send it again.
			UE_LOG(LogScenario, Log, TEXT("Scenario MCP session expired; starting a new one."));
			Self->ResetSession();
			Self->EnsureInitialized(TimeoutSeconds, [WeakThis, Method, Params, RequestId, TimeoutSeconds, HandleEnvelope](const FScenarioReply& Again)
			{
				const TSharedPtr<FScenarioMcpClient> Renewed = WeakThis.Pin();
				if (!Again.bOk || !Renewed.IsValid())
				{
					FScenarioReply Out = Again.bOk ? FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("The Scenario client was closed.")) : Again;
					Out.Error.bNotSent = true;
					HandleEnvelope(Out);
					return;
				}
				Renewed->Post(Method, Params, RequestId, TimeoutSeconds, false, true, HandleEnvelope);
			});
		});
	});
}

void FScenarioMcpClient::EnsureInitialized(float TimeoutSeconds, FInitWaiter OnReady)
{
	if (bInitialized)
	{
		OnReady(FScenarioReply::Ok());
		return;
	}
	InitWaiters.Add(MoveTemp(OnReady));
	if (bInitializing)
	{
		return;
	}
	bInitializing = true;
	const int32 Generation = SessionGeneration;
	TSharedPtr<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
	ClientInfo->SetStringField(TEXT("name"), TEXT("scenario-unreal-plugin"));
	ClientInfo->SetStringField(TEXT("version"), ScenarioVersion::PluginVersion());
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("protocolVersion"), ProtocolVersion);
	Params->SetObjectField(TEXT("capabilities"), MakeShared<FJsonObject>());
	Params->SetObjectField(TEXT("clientInfo"), ClientInfo);
	TWeakPtr<FScenarioMcpClient> WeakThis = AsShared();
	Post(TEXT("initialize"), Params, TakeId(), TimeoutSeconds, false, true, [WeakThis, Generation, TimeoutSeconds](const FScenarioReply& Reply)
	{
		const TSharedPtr<FScenarioMcpClient> This = WeakThis.Pin();
		if (!This.IsValid() || This->SessionGeneration != Generation)
		{
			return;
		}
		if (!Reply.bOk)
		{
			This->FinishInitialize(Reply);
			return;
		}
		if (Reply.Value->HasField(TEXT("error")))
		{
			const FString Detail = ScenarioJson::WriteValue(Reply.Value->TryGetField(TEXT("error")));
			This->FinishInitialize(ErrorFromDetail(Detail));
			return;
		}
		This->Post(TEXT("notifications/initialized"), nullptr, -1, TimeoutSeconds, false, false, [WeakThis, Generation](const FScenarioReply& Notified)
		{
			const TSharedPtr<FScenarioMcpClient> Self = WeakThis.Pin();
			if (!Self.IsValid() || Self->SessionGeneration != Generation)
			{
				return;
			}
			Self->FinishInitialize(Notified);
		});
	});
}

void FScenarioMcpClient::FinishInitialize(const FScenarioReply& Result)
{
	bInitializing = false;
	bInitialized = Result.bOk;
	TArray<FInitWaiter> Waiters = MoveTemp(InitWaiters);
	InitWaiters.Reset();
	for (FInitWaiter& Waiter : Waiters)
	{
		Waiter(Result.bOk ? FScenarioReply::Ok() : Result);
	}
}

void FScenarioMcpClient::Post(const FString& Method, TSharedPtr<FJsonObject> Params, int64 RequestId, float TimeoutSeconds, bool bAllowSessionSignal, bool bAllowRefresh, FScenarioReplyCallback OnDone)
{
	if (!Auth.IsValid() || !Auth->HasCredentials())
	{
		FScenarioReply Reply = FScenarioReply::Fail(EScenarioErrorKind::Auth, TEXT("Sign in to Scenario first."));
		Reply.Error.bNotSent = true;
		OnDone(Reply);
		return;
	}
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Payload->SetStringField(TEXT("method"), Method);
	if (RequestId >= 0)
	{
		Payload->SetNumberField(TEXT("id"), static_cast<double>(RequestId));
	}
	if (Params.IsValid() && Params->Values.Num() > 0)
	{
		Payload->SetObjectField(TEXT("params"), Params);
	}
	const FString BodyText = ScenarioJson::Write(Payload);
	const FTCHARToUTF8 Utf8(*BodyText);
	TArray<uint8> Body(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

	TMap<FString, FString> Headers;
	Headers.Add(TEXT("Authorization"), Auth->AuthorizationHeader());
	Headers.Add(TEXT("Content-Type"), TEXT("application/json"));
	Headers.Add(TEXT("Accept"), TEXT("application/json, text/event-stream"));
	Headers.Add(TEXT("MCP-Protocol-Version"), ProtocolVersion);
	Headers.Add(TEXT("User-Agent"), ScenarioVersion::UserAgent());
	const FString SessionUsed = SessionId;
	if (!SessionUsed.IsEmpty())
	{
		Headers.Add(TEXT("Mcp-Session-Id"), SessionUsed);
	}

	TWeakPtr<FScenarioMcpClient> WeakThis = AsShared();
	Transport->Send(TEXT("POST"), Endpoint, Headers, Body, FMath::Clamp(TimeoutSeconds, 1.0f, 180.0f),
		[WeakThis, Method, Params, RequestId, TimeoutSeconds, bAllowSessionSignal, bAllowRefresh, SessionUsed, OnDone](const FScenarioHttpResponse& Response)
	{
		const TSharedPtr<FScenarioMcpClient> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		// A 401 means the request was refused before dispatch, so one refresh and resend is safe even for paid calls.
		if (Response.Status == 401 && bAllowRefresh && This->Auth.IsValid())
		{
			const TSharedPtr<IScenarioAuthProvider> UsedAuth = This->Auth;
			UsedAuth->Refresh([WeakThis, Method, Params, RequestId, TimeoutSeconds, bAllowSessionSignal, SessionUsed, Response, OnDone](bool bRefreshed)
			{
				const TSharedPtr<FScenarioMcpClient> Self = WeakThis.Pin();
				if (!Self.IsValid())
				{
					return;
				}
				if (bRefreshed)
				{
					Self->Post(Method, Params, RequestId, TimeoutSeconds, bAllowSessionSignal, false, OnDone);
					return;
				}
				FScenarioReply Refused = Self->ReadResponse(Response, RequestId, bAllowSessionSignal, SessionUsed);
				Refused.Error.bNotSent = true;
				OnDone(Refused);
			});
			return;
		}
		OnDone(This->ReadResponse(Response, RequestId, bAllowSessionSignal, SessionUsed));
	});
}

FScenarioReply FScenarioMcpClient::ReadResponse(const FScenarioHttpResponse& Response, int64 RequestId, bool bAllowSessionSignal, const FString& SessionUsed)
{
	if (Response.TransportError == EScenarioErrorKind::Timeout)
	{
		return FScenarioReply::Fail(EScenarioErrorKind::Timeout, FScenarioErrors::MessageFor(EScenarioErrorKind::Timeout));
	}
	if (Response.TransportError != EScenarioErrorKind::None || Response.Status == 0)
	{
		return FScenarioReply::Fail(EScenarioErrorKind::Transport, FScenarioErrors::MessageFor(EScenarioErrorKind::Transport));
	}
	const FString NewSession = Response.Header(TEXT("mcp-session-id"));
	if (!NewSession.IsEmpty())
	{
		SessionId = NewSession;
	}
	const FString Body = Response.BodyText();
	if (Response.Status < 200 || Response.Status >= 300)
	{
		if (bAllowSessionSignal && (Response.Status == 400 || Response.Status == 404))
		{
			// "session" covers "Mcp-Session-Id is required" and "No valid session ID";
			// a 404 on a request that carried a session id means it expired.
			if (Body.ToLower().Contains(TEXT("session")) || (Response.Status == 404 && !SessionUsed.IsEmpty()))
			{
				FScenarioReply Expired = FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Session expired."));
				Expired.Error.bSessionExpired = true;
				Expired.Error.bNotSent = true;
				Expired.Error.HttpStatus = Response.Status;
				return Expired;
			}
		}
		FScenarioReply Failed = ErrorFromDetail(Body, Response.Status);
		const FString RetryAfter = Response.Header(TEXT("retry-after")).TrimStartAndEnd();
		if (!RetryAfter.IsEmpty() && FCString::IsNumeric(*RetryAfter))
		{
			Failed.Error.RetryAfterSeconds = FMath::Max(0.0, FCString::Atod(*RetryAfter));
		}
		// Refused requests were not run: 401, 403, 429 and validation errors never start a job.
		const int32 Status = Response.Status;
		Failed.Error.bNotSent = Status == 401 || Status == 403 || Status == 429 || Status == 400 || Status == 422;
		return Failed;
	}
	return ScenarioRpc::ParseReply(Body, Response.Header(TEXT("content-type")), RequestId);
}
