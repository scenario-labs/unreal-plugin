// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioHttp.h"

#include "Containers/Ticker.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

FString FScenarioHttpResponse::BodyText() const
{
	if (Body.Num() == 0)
	{
		return FString();
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Body.Num());
	return FString(Converted.Length(), Converted.Get());
}

FString FScenarioHttpResponse::Header(const FString& Name) const
{
	const FString* Found = Headers.Find(Name.ToLower());
	return Found ? *Found : FString();
}

void FScenarioHttpTransport::Send(const FString& Verb, const FString& Url, const TMap<FString, FString>& Headers, const TArray<uint8>& Body,
	float TimeoutSeconds, FScenarioHttpCallback OnDone, FScenarioProgressCallback OnProgress)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetVerb(Verb);
	Request->SetURL(Url);
	for (const TPair<FString, FString>& Pair : Headers)
	{
		Request->SetHeader(Pair.Key, Pair.Value);
	}
	if (Body.Num() > 0 || Verb == TEXT("POST") || Verb == TEXT("PUT"))
	{
		Request->SetContent(Body);
	}
	Request->SetTimeout(FMath::Max(1.0f, TimeoutSeconds));
	const double Started = FPlatformTime::Seconds();
	if (OnProgress)
	{
		Request->OnRequestProgress64().BindLambda([OnProgress](FHttpRequestPtr, uint64 Sent, uint64 Received)
		{
			OnProgress(Sent, Received);
		});
	}
	Request->OnProcessRequestComplete().BindLambda([OnDone, Started, TimeoutSeconds](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		FScenarioHttpResponse Out;
		if (!bConnected || !Response.IsValid())
		{
			const double Elapsed = FPlatformTime::Seconds() - Started;
			Out.TransportError = Elapsed >= TimeoutSeconds - 0.5 ? EScenarioErrorKind::Timeout : EScenarioErrorKind::Transport;
			OnDone(Out);
			return;
		}
		Out.Status = Response->GetResponseCode();
		for (const FString& Line : Response->GetAllHeaders())
		{
			FString Name;
			FString Value;
			if (Line.Split(TEXT(":"), &Name, &Value))
			{
				Out.Headers.Add(Name.TrimStartAndEnd().ToLower(), Value.TrimStartAndEnd());
			}
		}
		Out.Body = Response->GetContent();
		OnDone(Out);
	});
	Request->ProcessRequest();
}

void FScenarioHttpTransport::Delay(float Seconds, TFunction<void()> OnDone)
{
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([OnDone](float)
	{
		OnDone();
		return false;
	}), FMath::Max(0.0f, Seconds));
}

FString ScenarioUrl::Host(const FString& Url)
{
	FString Rest = Url;
	int32 Scheme = Rest.Find(TEXT("://"));
	if (Scheme != INDEX_NONE)
	{
		Rest = Rest.Mid(Scheme + 3);
	}
	for (const TCHAR* Separator : { TEXT("/"), TEXT("?"), TEXT("#") })
	{
		const int32 At = Rest.Find(Separator);
		if (At != INDEX_NONE)
		{
			Rest = Rest.Left(At);
		}
	}
	const int32 AtSign = Rest.Find(TEXT("@"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (AtSign != INDEX_NONE)
	{
		Rest = Rest.Mid(AtSign + 1);
	}
	if (Rest.StartsWith(TEXT("[")))
	{
		const int32 Close = Rest.Find(TEXT("]"));
		return Close != INDEX_NONE ? Rest.Mid(1, Close - 1).ToLower() : Rest.ToLower();
	}
	const int32 Colon = Rest.Find(TEXT(":"));
	if (Colon != INDEX_NONE)
	{
		Rest = Rest.Left(Colon);
	}
	return Rest.ToLower();
}

bool ScenarioUrl::IsHttps(const FString& Url)
{
	return Url.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase);
}

bool ScenarioUrl::IsLoopbackHost(const FString& Host)
{
	return Host == TEXT("127.0.0.1") || Host == TEXT("localhost") || Host == TEXT("::1");
}

FString ScenarioUrl::Encode(const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text);
	FString Out;
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		const uint8 Byte = static_cast<uint8>(Utf8.Get()[Index]);
		if ((Byte >= 'A' && Byte <= 'Z') || (Byte >= 'a' && Byte <= 'z') || (Byte >= '0' && Byte <= '9') || Byte == '-' || Byte == '_' || Byte == '.' || Byte == '~')
		{
			Out += static_cast<TCHAR>(Byte);
		}
		else
		{
			Out += FString::Printf(TEXT("%%%02X"), Byte);
		}
	}
	return Out;
}

FString ScenarioUrl::Decode(const FString& Text)
{
	TArray<uint8> Bytes;
	const FTCHARToUTF8 Utf8(*Text.Replace(TEXT("+"), TEXT(" ")));
	const ANSICHAR* Data = Utf8.Get();
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		if (Data[Index] == '%' && Index + 2 < Utf8.Length() && FChar::IsHexDigit(Data[Index + 1]) && FChar::IsHexDigit(Data[Index + 2]))
		{
			const TCHAR Hex[3] = { static_cast<TCHAR>(Data[Index + 1]), static_cast<TCHAR>(Data[Index + 2]), 0 };
			Bytes.Add(static_cast<uint8>(FParse::HexNumber(Hex)));
			Index += 2;
		}
		else
		{
			Bytes.Add(static_cast<uint8>(Data[Index]));
		}
	}
	Bytes.Add(0);
	return FString(UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData())));
}

TMap<FString, FString> ScenarioUrl::ParseQuery(const FString& Query)
{
	TMap<FString, FString> Out;
	TArray<FString> Parts;
	Query.ParseIntoArray(Parts, TEXT("&"));
	for (const FString& Part : Parts)
	{
		FString Key;
		FString Value;
		if (!Part.Split(TEXT("="), &Key, &Value))
		{
			Key = Part;
		}
		Out.Add(Decode(Key), Decode(Value));
	}
	return Out;
}

FString ScenarioUrl::BuildQuery(const TArray<TPair<FString, FString>>& Pairs)
{
	FString Out;
	for (const TPair<FString, FString>& Pair : Pairs)
	{
		if (!Out.IsEmpty())
		{
			Out += TEXT("&");
		}
		Out += Encode(Pair.Key) + TEXT("=") + Encode(Pair.Value);
	}
	return Out;
}
