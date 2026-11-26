// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioRestClient.h"

#include "Misc/Base64.h"
#include "ScenarioCoreModule.h"
#include "ScenarioErrors.h"
#include "ScenarioJson.h"

const TCHAR* FScenarioRestClient::DefaultApiUrl = TEXT("https://api.cloud.scenario.com/v1");

FScenarioRequest FScenarioRequest::Get(const FString& Path)
{
	FScenarioRequest Request;
	Request.Path = Path;
	return Request;
}

FScenarioRequest FScenarioRequest::Post(const FString& Path, const TSharedPtr<FJsonObject>& Body, EScenarioCall Call)
{
	FScenarioRequest Request;
	Request.Verb = TEXT("POST");
	Request.Path = Path;
	Request.Body = Body.IsValid() ? Body : MakeShared<FJsonObject>();
	Request.Call = Call;
	return Request;
}

FScenarioRequest& FScenarioRequest::Param(const FString& Name, const FString& Value)
{
	if (!Value.IsEmpty())
	{
		Query.Emplace(Name, Value);
	}
	return *this;
}

FScenarioRequest& FScenarioRequest::Param(const FString& Name, int32 Value)
{
	return Param(Name, FString::FromInt(Value));
}

FScenarioRequest& FScenarioRequest::WithScope(EScenarioScope InScope)
{
	Scope = InScope;
	return *this;
}

FScenarioRequest& FScenarioRequest::WithTimeout(float Seconds)
{
	TimeoutSeconds = Seconds;
	return *this;
}

FString FScenarioRequest::Segment(const FString& Id)
{
	return ScenarioUrl::Encode(Id);
}

FString FScenarioRequest::QueryValue(const FString& Name) const
{
	for (const TPair<FString, FString>& Pair : Query)
	{
		if (Pair.Key == Name)
		{
			return Pair.Value;
		}
	}
	return FString();
}

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

FScenarioRestClient::FScenarioRestClient(TSharedRef<IScenarioHttpTransport> InTransport, const FString& InApiUrl)
	: Transport(InTransport)
	, ApiUrl(InApiUrl)
{
	ApiUrl.RemoveFromEnd(TEXT("/"));
	bSpendingDisabled = FPlatformMisc::GetEnvironmentVariable(TEXT("SCENARIO_NO_SPEND")) == TEXT("1");
}

void FScenarioRestClient::SetAuth(TSharedPtr<IScenarioAuthProvider> InAuth)
{
	Auth = InAuth;
}

void FScenarioRestClient::SetWorkspace(const FString& InTeamId, const FString& InProjectId)
{
	TeamId = InTeamId;
	ProjectId = InProjectId;
}

bool FScenarioRestClient::HasCredentials() const
{
	return Auth.IsValid() && Auth->HasCredentials();
}

FString FScenarioRestClient::UrlFor(const FScenarioRequest& Request) const
{
	TArray<TPair<FString, FString>> Query = Request.Query;
	if (Auth.IsValid() && Auth->NeedsWorkspaceArguments() && Request.Scope != EScenarioScope::None)
	{
		if (!TeamId.IsEmpty() && Request.QueryValue(TEXT("teamId")).IsEmpty())
		{
			Query.Emplace(TEXT("teamId"), TeamId);
		}
		if (Request.Scope == EScenarioScope::Project && !ProjectId.IsEmpty() && Request.QueryValue(TEXT("projectId")).IsEmpty())
		{
			Query.Emplace(TEXT("projectId"), ProjectId);
		}
	}
	const FString Url = ApiUrl + (Request.Path.StartsWith(TEXT("/")) ? Request.Path : TEXT("/") + Request.Path);
	return Query.Num() > 0 ? Url + TEXT("?") + ScenarioUrl::BuildQuery(Query) : Url;
}

FString FScenarioRestClient::CreatedJobId(const TSharedPtr<FJsonObject>& Value)
{
	if (!Value.IsValid())
	{
		return FString();
	}
	for (const TCHAR* Key : { TEXT("jobId"), TEXT("job_id") })
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
			FString Id = ScenarioJson::GetString(Inner, TEXT("jobId"));
			if (Id.IsEmpty())
			{
				Id = ScenarioJson::GetString(Inner, TEXT("id"));
			}
			if (!Id.IsEmpty())
			{
				return Id;
			}
		}
	}
	return FString();
}

double FScenarioRestClient::RetryDelay(int32 AttemptIndex, double RetryAfterSeconds)
{
	if (RetryAfterSeconds >= 0.0)
	{
		return RetryAfterSeconds > MaxRetryAfterSeconds ? -1.0 : RetryAfterSeconds;
	}
	const double Base = FMath::Min(8.0, 0.5 * FMath::Pow(2.0, static_cast<double>(AttemptIndex)));
	return Base * (0.5 + 0.5 * FMath::FRand());
}

bool FScenarioRestClient::IsGatewayRefusal(const FScenarioHttpResponse& Response)
{
	return Response.Status == 401 || (Response.Status == 403 && Response.Header(TEXT("x-amzn-errortype")).StartsWith(TEXT("AccessDeniedException")));
}

void FScenarioRestClient::Send(const FScenarioRequest& Request, FScenarioReplyCallback OnDone)
{
	if (!HasCredentials())
	{
		FScenarioReply Reply = FScenarioReply::Fail(EScenarioErrorKind::Auth, TEXT("Sign in to Scenario first."));
		Reply.Error.bNotSent = true;
		OnDone(Reply);
		return;
	}
	if (Request.IsSpend() && bSpendingDisabled)
	{
		FScenarioReply Reply = FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("Spending is turned off (SCENARIO_NO_SPEND=1), so nothing was sent."));
		Reply.Error.bNotSent = true;
		OnDone(Reply);
		return;
	}
	const FString DryRun = Request.QueryValue(TEXT("dryRun"));
	if (Request.Call == EScenarioCall::DryRun && DryRun != TEXT("true") && DryRun != TEXT("api"))
	{
		// Without dryRun the API runs the operation: a price check that lacks it would spend.
		FScenarioReply Reply = FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("This price check is not marked as a dry run, so nothing was sent."));
		Reply.Error.bNotSent = true;
		OnDone(Reply);
		return;
	}
	Attempt(Request, UrlFor(Request), 0, Request.IsRetryable() ? ReadAttempts : 1, MoveTemp(OnDone));
}

void FScenarioRestClient::Attempt(const FScenarioRequest& Request, const FString& BoundUrl, int32 AttemptIndex, int32 Attempts, FScenarioReplyCallback OnDone)
{
	TWeakPtr<FScenarioRestClient> WeakThis = AsShared();
	SendOnce(Request, true, [WeakThis, Request, BoundUrl, AttemptIndex, Attempts, OnDone](const FScenarioReply& Reply)
	{
		const TSharedPtr<FScenarioRestClient> This = WeakThis.Pin();
		if (Reply.bOk)
		{
			if (Request.Call == EScenarioCall::DryRun)
			{
				const FString JobId = CreatedJobId(Reply.Value);
				if (!JobId.IsEmpty())
				{
					if (This.IsValid())
					{
						This->bEstimateAlarm = true;
					}
					UE_LOG(LogScenario, Warning, TEXT("A price check (%s) returned job %s. Automatic price checks are off for this session."), *Request.Path, *JobId);
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
				UE_LOG(LogScenario, Verbose, TEXT("Retrying %s %s in %.1f s (%s)."), *Request.Verb, *Request.Path, Delay, LexToString(Reply.Error.Kind));
				This->Transport->Delay(static_cast<float>(Delay), [WeakThis, Request, BoundUrl, AttemptIndex, Attempts, OnDone]()
				{
					if (const TSharedPtr<FScenarioRestClient> Again = WeakThis.Pin())
					{
						if (!Again->UrlFor(Request).Equals(BoundUrl, ESearchCase::CaseSensitive))
						{
							// The team or project changed during the back-off: the answer would belong to the new one.
							FScenarioReply Moved = FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("The team or project changed while the request was retried, so it was stopped."));
							Moved.Error.bNotSent = true;
							OnDone(Moved);
							return;
						}
						Again->Attempt(Request, BoundUrl, AttemptIndex + 1, Attempts, OnDone);
					}
				});
				return;
			}
		}
		OnDone(Reply);
	});
}

void FScenarioRestClient::SendOnce(const FScenarioRequest& Request, bool bAllowRefresh, FScenarioReplyCallback OnDone)
{
	if (!Auth.IsValid() || !Auth->HasCredentials())
	{
		FScenarioReply Reply = FScenarioReply::Fail(EScenarioErrorKind::Auth, TEXT("Sign in to Scenario first."));
		Reply.Error.bNotSent = true;
		OnDone(Reply);
		return;
	}
	TArray<uint8> Body;
	TMap<FString, FString> Headers;
	Headers.Add(TEXT("Authorization"), Auth->AuthorizationHeader());
	Headers.Add(TEXT("Accept"), TEXT("application/json"));
	Headers.Add(TEXT("User-Agent"), ScenarioVersion::UserAgent());
	if (Request.Body.IsValid())
	{
		const FTCHARToUTF8 Utf8(*ScenarioJson::Write(Request.Body));
		Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		Headers.Add(TEXT("Content-Type"), TEXT("application/json"));
	}

	TWeakPtr<FScenarioRestClient> WeakThis = AsShared();
	const FString Url = UrlFor(Request);
	// The sign-in that sends the request: a renewal resends it only for that one.
	const TWeakPtr<IScenarioAuthProvider> SentAuth = Auth;
	const uint32 SentGeneration = Auth->GetSignInGeneration();
	auto SameSignIn = [SentAuth, SentGeneration](const FScenarioRestClient& Client)
	{
		return Client.Auth.IsValid() && Client.Auth == SentAuth.Pin() && Client.Auth->GetSignInGeneration() == SentGeneration;
	};
	Transport->Send(Request.Verb, Url, Headers, Body, FMath::Clamp(Request.TimeoutSeconds, 1.0f, 180.0f),
		[WeakThis, Request, Url, bAllowRefresh, SameSignIn, OnDone](const FScenarioHttpResponse& Response)
	{
		const TSharedPtr<FScenarioRestClient> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		if (bAllowRefresh && IsGatewayRefusal(Response) && SameSignIn(*This))
		{
			This->Auth->Refresh([WeakThis, Request, Url, Response, SameSignIn, OnDone](bool bRefreshed)
			{
				const TSharedPtr<FScenarioRestClient> Self = WeakThis.Pin();
				if (!Self.IsValid())
				{
					return;
				}
				if (bRefreshed && !SameSignIn(*Self))
				{
					// Someone signed in or out during the renewal: the request was approved under the earlier sign-in.
					FScenarioReply Switched = FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("The Scenario sign-in changed while the session was renewed, so nothing was sent."));
					Switched.Error.bNotSent = true;
					OnDone(Switched);
					return;
				}
				if (bRefreshed && !Self->UrlFor(Request).Equals(Url, ESearchCase::CaseSensitive))
				{
					// The team or project changed during the renewal: the request was priced and approved for the old one.
					FScenarioReply Moved = FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("The team or project changed while the session was renewed, so nothing was sent."));
					Moved.Error.bNotSent = true;
					OnDone(Moved);
					return;
				}
				if (bRefreshed)
				{
					Self->SendOnce(Request, false, OnDone);
					return;
				}
				OnDone(ReadReply(Request, Response));
			});
			return;
		}
		OnDone(ReadReply(Request, Response));
	});
}

FScenarioReply FScenarioRestClient::ReadReply(const FScenarioRequest& Request, const FScenarioHttpResponse& Response)
{
	FScenarioReply Reply = ReadResponse(Response, Request.IsSpend());
	// A price comes only from a reply the API marks as a dry run (HTTP 269). A reply with a job goes on to the alarm.
	if (Request.Call == EScenarioCall::DryRun && Reply.bOk && Response.Status != 269 && CreatedJobId(Reply.Value).IsEmpty())
	{
		return FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario did not answer this price check as a dry run, so no price was taken."));
	}
	return Reply;
}

FScenarioReply FScenarioRestClient::ReadResponse(const FScenarioHttpResponse& Response, bool bPaid)
{
	auto Unknown = [bPaid](FScenarioReply Reply)
	{
		if (bPaid && FScenarioErrors::IsAmbiguousKind(Reply.Error.Kind) && !Reply.Error.bNotSent)
		{
			Reply.Error.bAmbiguous = true;
			Reply.Error.Message = TEXT("The request status is unknown: Scenario may have accepted it. Check recent jobs before sending it again.");
		}
		return Reply;
	};
	if (Response.TransportError == EScenarioErrorKind::Timeout)
	{
		return Unknown(FScenarioReply::Fail(EScenarioErrorKind::Timeout, FScenarioErrors::MessageFor(EScenarioErrorKind::Timeout)));
	}
	if (Response.TransportError != EScenarioErrorKind::None || Response.Status == 0)
	{
		return Unknown(FScenarioReply::Fail(EScenarioErrorKind::Transport, FScenarioErrors::MessageFor(EScenarioErrorKind::Transport)));
	}
	const FString Text = Response.BodyText();
	if (Response.Status >= 200 && Response.Status < 300)
	{
		// A dry run answers 269; any 2xx is a reply.
		if (Text.TrimStartAndEnd().IsEmpty())
		{
			return FScenarioReply::Ok();
		}
		const TSharedPtr<FJsonObject> Parsed = ScenarioJson::ParseObject(Text);
		if (!Parsed.IsValid())
		{
			return Unknown(FScenarioReply::Fail(EScenarioErrorKind::Protocol, FScenarioErrors::MessageFor(EScenarioErrorKind::Protocol)));
		}
		return FScenarioReply::Ok(Parsed);
	}

	// Errors come as {reason, name, details}; anything else is kept as text.
	const TSharedPtr<FJsonObject> ErrorBody = ScenarioJson::ParseObject(Text);
	FString Detail;
	for (const TCHAR* Field : { TEXT("reason"), TEXT("message"), TEXT("error") })
	{
		Detail = ScenarioJson::GetString(ErrorBody, Field);
		if (!Detail.IsEmpty())
		{
			break;
		}
	}
	if (Detail.IsEmpty() && !ErrorBody.IsValid())
	{
		Detail = Text.Left(400);
	}
	Detail = FScenarioErrors::Scrub(Detail).TrimStartAndEnd();
	const int32 Status = Response.Status;
	EScenarioErrorKind Kind = FScenarioErrors::Classify(Detail, Status);
	if (Status == 408)
	{
		Kind = EScenarioErrorKind::Timeout;
	}
	else if (Status == 429 && ScenarioJson::GetString(ErrorBody, TEXT("name")) == TEXT("PlanLimitReachedError"))
	{
		// The plan's limit, not a pace: waiting a few seconds does not help.
		Kind = EScenarioErrorKind::Quota;
	}
	else if (IsGatewayRefusal(Response) && (Detail.IsEmpty() || Detail == TEXT("Forbidden") || Detail == TEXT("Unauthorized")))
	{
		Kind = EScenarioErrorKind::Auth;
	}
	FScenarioReply Failed = FScenarioReply::Fail(Kind, FScenarioErrors::MessageFor(Kind, Detail), Detail.Left(2000));
	Failed.Error.HttpStatus = Status;
	const FString RetryAfter = Response.Header(TEXT("retry-after")).TrimStartAndEnd();
	if (!RetryAfter.IsEmpty() && FCString::IsNumeric(*RetryAfter))
	{
		Failed.Error.RetryAfterSeconds = FMath::Max(0.0, FCString::Atod(*RetryAfter));
	}
	// A 4xx other than 408 is a refusal: the request was checked and turned down, nothing started.
	Failed.Error.bNotSent = Status >= 400 && Status < 500 && Status != 408;
	return Unknown(Failed);
}
