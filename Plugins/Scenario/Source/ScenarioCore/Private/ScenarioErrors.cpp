// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioErrors.h"

#include "Internationalization/Regex.h"
#include "Misc/DateTime.h"

const TCHAR* LexToString(EScenarioErrorKind Kind)
{
	switch (Kind)
	{
	case EScenarioErrorKind::None: return TEXT("None");
	case EScenarioErrorKind::Auth: return TEXT("Auth");
	case EScenarioErrorKind::Forbidden: return TEXT("Forbidden");
	case EScenarioErrorKind::Quota: return TEXT("Quota");
	case EScenarioErrorKind::Moderated: return TEXT("Moderated");
	case EScenarioErrorKind::RateLimit: return TEXT("RateLimit");
	case EScenarioErrorKind::NotFound: return TEXT("NotFound");
	case EScenarioErrorKind::Invalid: return TEXT("Invalid");
	case EScenarioErrorKind::Transport: return TEXT("Transport");
	case EScenarioErrorKind::Timeout: return TEXT("Timeout");
	case EScenarioErrorKind::Http5xx: return TEXT("ServerError");
	case EScenarioErrorKind::Protocol: return TEXT("Protocol");
	case EScenarioErrorKind::Local: return TEXT("Local");
	case EScenarioErrorKind::EstimateJob: return TEXT("EstimateReturnedJob");
	case EScenarioErrorKind::Scenario: return TEXT("Scenario");
	case EScenarioErrorKind::Cancelled: return TEXT("Cancelled");
	}
	return TEXT("Unknown");
}

FString FScenarioError::ToString() const
{
	return FString::Printf(TEXT("%s: %s"), LexToString(Kind), *Message);
}

FScenarioReply FScenarioReply::Ok(const TSharedPtr<FJsonObject>& InValue)
{
	FScenarioReply Reply;
	Reply.bOk = true;
	Reply.Value = InValue.IsValid() ? InValue : MakeShared<FJsonObject>();
	return Reply;
}

FScenarioReply FScenarioReply::Fail(EScenarioErrorKind Kind, const FString& Message, const FString& Detail)
{
	FScenarioReply Reply;
	Reply.bOk = false;
	Reply.Error.Kind = Kind;
	Reply.Error.Message = Message;
	Reply.Error.Detail = Detail;
	return Reply;
}

FScenarioReply FScenarioReply::Fail(const FScenarioError& InError)
{
	FScenarioReply Reply;
	Reply.bOk = false;
	Reply.Error = InError;
	return Reply;
}

double ScenarioNow()
{
	const FTimespan Since = FDateTime::UtcNow() - FDateTime(1970, 1, 1);
	return Since.GetTotalSeconds();
}

double ScenarioParseIsoTime(const FString& Text)
{
	FDateTime Parsed;
	if (Text.Len() < 19 || !FDateTime::ParseIso8601(*Text, Parsed))
	{
		return -1.0;
	}
	return (Parsed - FDateTime(1970, 1, 1)).GetTotalSeconds();
}

FString ScenarioIsoTime(double UnixSeconds)
{
	const FDateTime Time = FDateTime(1970, 1, 1) + FTimespan::FromSeconds(UnixSeconds);
	return Time.ToIso8601();
}

namespace
{
	const TCHAR* const ModerationWords[] = { TEXT("moderat"), TEXT("nsfw"), TEXT("sensitive content"), TEXT("content policy"), TEXT("flagged"), TEXT("safety system") };
	const TCHAR* const QuotaWords[] = { TEXT("insufficient"), TEXT("not enough credit"), TEXT("not enough creative"), TEXT("out of credit"), TEXT("quota") };
	const TCHAR* const AuthWords[] = { TEXT("unauthorized"), TEXT("unauthenticated"), TEXT("invalid api key"), TEXT("invalid credentials"), TEXT("authentication"), TEXT("invalid_token") };

	template <int32 N>
	bool ContainsAny(const FString& Lower, const TCHAR* const (&Words)[N])
	{
		for (const TCHAR* Word : Words)
		{
			if (Lower.Contains(Word))
			{
				return true;
			}
		}
		return false;
	}
}

bool FScenarioErrors::IsAmbiguousKind(EScenarioErrorKind Kind)
{
	return Kind == EScenarioErrorKind::Transport || Kind == EScenarioErrorKind::Timeout
		|| Kind == EScenarioErrorKind::Http5xx || Kind == EScenarioErrorKind::Protocol;
}

bool FScenarioErrors::IsRetryableKind(EScenarioErrorKind Kind)
{
	return Kind == EScenarioErrorKind::RateLimit || Kind == EScenarioErrorKind::Transport
		|| Kind == EScenarioErrorKind::Timeout || Kind == EScenarioErrorKind::Http5xx;
}

bool FScenarioErrors::IsModerated(const FString& Text)
{
	return ContainsAny(Text.ToLower(), ModerationWords);
}

EScenarioErrorKind FScenarioErrors::Classify(const FString& Detail, int32 HttpStatus)
{
	const FString Lower = Detail.ToLower();
	if (ContainsAny(Lower, ModerationWords))
	{
		return EScenarioErrorKind::Moderated;
	}
	switch (HttpStatus)
	{
	case 401: return EScenarioErrorKind::Auth;
	case 402: return EScenarioErrorKind::Quota;
	case 403: return EScenarioErrorKind::Forbidden;
	case 404: return EScenarioErrorKind::NotFound;
	case 429: return EScenarioErrorKind::RateLimit;
	default: break;
	}
	if (HttpStatus >= 500)
	{
		return EScenarioErrorKind::Http5xx;
	}
	if (ContainsAny(Lower, QuotaWords))
	{
		return EScenarioErrorKind::Quota;
	}
	if (HttpStatus == 400 || HttpStatus == 422)
	{
		return EScenarioErrorKind::Invalid;
	}
	if (ContainsAny(Lower, AuthWords))
	{
		return EScenarioErrorKind::Auth;
	}
	if (Lower.Contains(TEXT("rate limit")) || Lower.Contains(TEXT("too many requests")))
	{
		return EScenarioErrorKind::RateLimit;
	}
	if (Lower.Contains(TEXT("forbidden")) || Lower.Contains(TEXT("not allowed")) || Lower.Contains(TEXT("blocked by your team")))
	{
		return EScenarioErrorKind::Forbidden;
	}
	if (Lower.Contains(TEXT("not found")))
	{
		return EScenarioErrorKind::NotFound;
	}
	return EScenarioErrorKind::Scenario;
}

FString FScenarioErrors::MessageFor(EScenarioErrorKind Kind, const FString& Detail)
{
	FString Base;
	switch (Kind)
	{
	case EScenarioErrorKind::Auth: Base = TEXT("Your Scenario session ended or the API key was rejected. Sign in again."); break;
	case EScenarioErrorKind::Forbidden: Base = TEXT("This account cannot do that in the selected Scenario project."); break;
	case EScenarioErrorKind::Quota: Base = TEXT("Not enough Creative Units for this run."); break;
	case EScenarioErrorKind::Moderated: Base = TEXT("Blocked by Scenario's content rules. Change the prompt or the reference."); break;
	case EScenarioErrorKind::RateLimit: Base = TEXT("Scenario is limiting requests right now. Try again in a moment."); break;
	case EScenarioErrorKind::NotFound: Base = TEXT("Scenario could not find that item."); break;
	case EScenarioErrorKind::Invalid: Base = TEXT("Scenario refused the settings."); break;
	case EScenarioErrorKind::Transport: Base = TEXT("Could not reach Scenario. Check the connection and try again."); break;
	case EScenarioErrorKind::Timeout: Base = TEXT("Scenario took too long to answer."); break;
	case EScenarioErrorKind::Http5xx: Base = TEXT("Scenario had a server error. Try again in a moment."); break;
	case EScenarioErrorKind::Protocol: Base = TEXT("Scenario returned a reply the plugin could not read."); break;
	case EScenarioErrorKind::Local: Base = TEXT("The request was not sent."); break;
	case EScenarioErrorKind::EstimateJob: Base = TEXT("A price check returned a job."); break;
	case EScenarioErrorKind::Cancelled: Base = TEXT("Cancelled."); break;
	default: Base = TEXT("Scenario reported an error."); break;
	}
	FString Clean = Scrub(Detail).TrimStartAndEnd();
	if (Clean.IsEmpty() || Kind == EScenarioErrorKind::Transport || Kind == EScenarioErrorKind::Timeout)
	{
		return Base;
	}
	if (Clean.Len() > 300)
	{
		Clean = Clean.Left(300) + TEXT("...");
	}
	return FString::Printf(TEXT("%s (%s)"), *Base, *Clean);
}

FString FScenarioErrors::Scrub(const FString& Text)
{
	FString Out = Text;
	// Signed CDN and storage URLs: keep scheme, host and path, drop the query.
	{
		const FRegexPattern Pattern(TEXT("(https?://[^\\s\"'?]+)\\?[^\\s\"']*"));
		FRegexMatcher Matcher(Pattern, Out);
		FString Rebuilt;
		int32 Last = 0;
		while (Matcher.FindNext())
		{
			Rebuilt += Out.Mid(Last, Matcher.GetMatchBeginning() - Last);
			Rebuilt += Matcher.GetCaptureGroup(1) + TEXT("?...");
			Last = Matcher.GetMatchEnding();
		}
		Rebuilt += Out.Mid(Last);
		Out = Rebuilt;
	}
	// Authorization headers and bearer or basic tokens.
	{
		const FRegexPattern Pattern(TEXT("(?i)(bearer|basic)\\s+[A-Za-z0-9._~+/=-]{8,}"));
		FRegexMatcher Matcher(Pattern, Out);
		FString Rebuilt;
		int32 Last = 0;
		while (Matcher.FindNext())
		{
			Rebuilt += Out.Mid(Last, Matcher.GetMatchBeginning() - Last);
			Rebuilt += Matcher.GetCaptureGroup(1) + TEXT(" <redacted>");
			Last = Matcher.GetMatchEnding();
		}
		Rebuilt += Out.Mid(Last);
		Out = Rebuilt;
	}
	return Out;
}
