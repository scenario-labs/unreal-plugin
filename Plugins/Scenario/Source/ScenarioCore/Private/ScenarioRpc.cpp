// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioRpc.h"

#include "ScenarioJson.h"

TArray<TSharedPtr<FJsonValue>> ScenarioRpc::StreamMessages(const FString& Source)
{
	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<FString> DataLines;
	FString Normalized = Source.Replace(TEXT("\r\n"), TEXT("\n")).Replace(TEXT("\r"), TEXT("\n"));
	TArray<FString> Lines;
	Normalized.ParseIntoArray(Lines, TEXT("\n"), false);
	Lines.Add(FString());
	for (const FString& Line : Lines)
	{
		if (Line.IsEmpty())
		{
			if (DataLines.Num() > 0)
			{
				const FString Value = FString::Join(DataLines, TEXT("\n"));
				if (Value.TrimStartAndEnd() != TEXT("[DONE]"))
				{
					// A malformed progress event must not hide a later result.
					if (const TSharedPtr<FJsonValue> Parsed = ScenarioJson::ParseValue(Value))
					{
						Messages.Add(Parsed);
					}
				}
				DataLines.Reset();
			}
		}
		else if (Line.StartsWith(TEXT("data:")))
		{
			FString Data = Line.Mid(5);
			if (Data.StartsWith(TEXT(" ")))
			{
				Data.RightChopInline(1);
			}
			DataLines.Add(Data);
		}
	}
	return Messages;
}

namespace
{
	bool SameId(const TSharedPtr<FJsonValue>& Id, int64 RequestId)
	{
		if (!Id.IsValid())
		{
			return false;
		}
		if (Id->Type == EJson::Number)
		{
			return static_cast<int64>(Id->AsNumber()) == RequestId;
		}
		if (Id->Type == EJson::String)
		{
			return Id->AsString() == FString::Printf(TEXT("%lld"), static_cast<long long>(RequestId));
		}
		return false;
	}
}

FScenarioReply ScenarioRpc::ParseReply(const FString& Body, const FString& ContentType, int64 RequestId)
{
	FString Source = Body;
	if (Source.StartsWith(TEXT("\uFEFF")))
	{
		Source.RightChopInline(1);
	}
	if (Source.TrimStartAndEnd().IsEmpty())
	{
		if (RequestId < 0)
		{
			return FScenarioReply::Ok();
		}
		return FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario sent an empty reply."));
	}
	TArray<TSharedPtr<FJsonValue>> Messages;
	const FString Trimmed = Source.TrimStart();
	const bool bStream = ContentType.ToLower().Contains(TEXT("text/event-stream"))
		|| Trimmed.StartsWith(TEXT("data:")) || Trimmed.StartsWith(TEXT("event:")) || Trimmed.StartsWith(TEXT(":"));
	if (bStream)
	{
		Messages = StreamMessages(Source);
	}
	else
	{
		const TSharedPtr<FJsonValue> Payload = ScenarioJson::ParseValue(Source);
		if (!Payload.IsValid())
		{
			return FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario returned invalid JSON."));
		}
		if (Payload->Type == EJson::Array)
		{
			Messages = Payload->AsArray();
		}
		else
		{
			Messages.Add(Payload);
		}
	}
	if (RequestId < 0)
	{
		return FScenarioReply::Ok();
	}
	TSharedPtr<FJsonObject> Found;
	for (const TSharedPtr<FJsonValue>& Message : Messages)
	{
		if (!Message.IsValid() || Message->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Object = Message->AsObject();
		if (SameId(Object->TryGetField(TEXT("id")), RequestId) && (Object->HasField(TEXT("result")) || Object->HasField(TEXT("error"))))
		{
			// The last matching message wins (progress first, result last).
			Found = Object;
		}
	}
	if (!Found.IsValid())
	{
		return FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario did not return a complete reply for this request."));
	}
	return FScenarioReply::Ok(Found);
}
