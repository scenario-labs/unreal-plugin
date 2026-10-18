// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioLocalHttpServer.h"

#include "Async/Async.h"
#include "Common/TcpSocketBuilder.h"
#include "HAL/Event.h"
#include "HAL/RunnableThread.h"
#include "IPAddress.h"
#include "ScenarioCoreModule.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

FString FScenarioLocalRequest::Header(const FString& Name) const
{
	const FString* Found = Headers.Find(Name.ToLower());
	return Found ? *Found : FString();
}

FString FScenarioLocalRequest::BodyText() const
{
	if (Body.Num() == 0)
	{
		return FString();
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Body.Num());
	return FString(Converted.Length(), Converted.Get());
}

namespace
{
	TArray<uint8> Utf8Bytes(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	const TCHAR* Reason(int32 Status)
	{
		switch (Status)
		{
		case 200: return TEXT("OK");
		case 202: return TEXT("Accepted");
		case 204: return TEXT("No Content");
		case 400: return TEXT("Bad Request");
		case 401: return TEXT("Unauthorized");
		case 403: return TEXT("Forbidden");
		case 404: return TEXT("Not Found");
		case 405: return TEXT("Method Not Allowed");
		case 408: return TEXT("Request Timeout");
		case 413: return TEXT("Payload Too Large");
		case 421: return TEXT("Misdirected Request");
		case 500: return TEXT("Internal Server Error");
		case 503: return TEXT("Service Unavailable");
		default: return TEXT("Status");
		}
	}
}

FScenarioLocalResponse FScenarioLocalResponse::Text(int32 Status, const FString& Body, const FString& ContentType)
{
	FScenarioLocalResponse Response;
	Response.Status = Status;
	Response.ContentType = ContentType;
	Response.Body = Utf8Bytes(Body);
	return Response;
}

FScenarioLocalResponse FScenarioLocalResponse::Json(int32 Status, const FString& Body)
{
	return Text(Status, Body, TEXT("application/json"));
}

FScenarioLocalHttpServer::FScenarioLocalHttpServer(const FString& InName, FScenarioLocalHandler InHandler)
	: Name(InName)
	, Handler(MoveTemp(InHandler))
{
}

FScenarioLocalHttpServer::~FScenarioLocalHttpServer()
{
	Stop();
}

bool FScenarioLocalHttpServer::Start(int32 FirstPort, int32 Tries)
{
	Stop();
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Sockets)
	{
		return false;
	}
	for (int32 Offset = 0; Offset < FMath::Max(1, Tries); ++Offset)
	{
		const int32 TryPort = FirstPort == 0 ? 0 : FirstPort + Offset;
		FIPv4Endpoint Endpoint(FIPv4Address(127, 0, 0, 1), static_cast<uint16>(TryPort));
		FTcpSocketBuilder Builder = FTcpSocketBuilder(*FString::Printf(TEXT("Scenario %s"), *Name))
			.AsNonBlocking()
			.BoundToEndpoint(Endpoint)
			.Listening(16);
#if !PLATFORM_WINDOWS
		// SO_REUSEADDR lets the port be bound again right after a restart (TIME_WAIT). On Windows it would allow port stealing.
		Builder = Builder.AsReusable();
#endif
		FSocket* Socket = Builder.Build();
		if (Socket)
		{
			Listener = Socket;
			TSharedRef<FInternetAddr> Bound = Sockets->CreateInternetAddr();
			Socket->GetAddress(*Bound);
			Port = Bound->GetPort();
			break;
		}
	}
	if (!Listener)
	{
		UE_LOG(LogScenario, Warning, TEXT("Scenario %s: no free local port from %d."), *Name, FirstPort);
		return false;
	}
	bStopping = false;
	WeakSelf = AsShared();
	Thread = FRunnableThread::Create(this, *FString::Printf(TEXT("Scenario%sListener"), *Name), 0, TPri_BelowNormal);
	UE_LOG(LogScenario, Log, TEXT("Scenario %s listening on 127.0.0.1:%d."), *Name, Port);
	return true;
}

void FScenarioLocalHttpServer::Stop()
{
	if (!Listener && !Thread)
	{
		return;
	}
	bStopping = true;
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	if (Listener)
	{
		Listener->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Listener);
		Listener = nullptr;
	}
	// Connections in flight notice bStopping within their poll interval.
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	while (OpenConnections.GetValue() > 0 && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.01f);
	}
	Port = 0;
}

uint32 FScenarioLocalHttpServer::Run()
{
	while (!bStopping)
	{
		bool bPending = false;
		if (Listener && Listener->WaitForPendingConnection(bPending, FTimespan::FromMilliseconds(100)) && bPending)
		{
			FSocket* Connection = Listener->Accept(TEXT("Scenario local connection"));
			if (Connection)
			{
				OpenConnections.Increment();
				TWeakPtr<FScenarioLocalHttpServer, ESPMode::ThreadSafe> WeakThis = WeakSelf;
				// One short-lived task per connection, so a slow tool never blocks accepting.
				Async(EAsyncExecution::Thread, [WeakThis, Connection]()
				{
					if (const TSharedPtr<FScenarioLocalHttpServer, ESPMode::ThreadSafe> This = WeakThis.Pin())
					{
						This->ServeConnection(Connection);
						This->OpenConnections.Decrement();
					}
					else
					{
						Connection->Close();
						ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Connection);
					}
				});
			}
		}
	}
	return 0;
}

bool FScenarioLocalHttpServer::ParseRequest(const TArray<uint8>& Data, int32 MaxBody, FScenarioLocalRequest& Out, FString& OutError, int32& OutStatus)
{
	OutError.Reset();
	// Find the end of the head.
	int32 HeadEnd = INDEX_NONE;
	for (int32 Index = 0; Index + 3 < Data.Num(); ++Index)
	{
		if (Data[Index] == '\r' && Data[Index + 1] == '\n' && Data[Index + 2] == '\r' && Data[Index + 3] == '\n')
		{
			HeadEnd = Index;
			break;
		}
	}
	if (HeadEnd == INDEX_NONE)
	{
		if (Data.Num() > 64 * 1024)
		{
			OutError = TEXT("Request head too large.");
			OutStatus = 400;
		}
		return false;
	}
	const FUTF8ToTCHAR HeadConverted(reinterpret_cast<const ANSICHAR*>(Data.GetData()), HeadEnd);
	const FString HeadText(HeadConverted.Length(), HeadConverted.Get());
	TArray<FString> Lines;
	HeadText.ParseIntoArray(Lines, TEXT("\r\n"), false);
	if (Lines.Num() == 0)
	{
		OutError = TEXT("Empty request.");
		OutStatus = 400;
		return false;
	}
	TArray<FString> RequestLine;
	Lines[0].ParseIntoArrayWS(RequestLine);
	if (RequestLine.Num() != 3 || !RequestLine[2].StartsWith(TEXT("HTTP/1.")))
	{
		OutError = TEXT("Malformed request line.");
		OutStatus = 400;
		return false;
	}
	Out = FScenarioLocalRequest();
	Out.Method = RequestLine[0].ToUpper();
	FString Target = RequestLine[1];
	if (!Target.Split(TEXT("?"), &Out.Path, &Out.Query))
	{
		Out.Path = Target;
	}
	for (int32 Index = 1; Index < Lines.Num(); ++Index)
	{
		FString HeaderName;
		FString HeaderValue;
		if (!Lines[Index].Split(TEXT(":"), &HeaderName, &HeaderValue))
		{
			OutError = TEXT("Malformed header.");
			OutStatus = 400;
			return false;
		}
		HeaderName = HeaderName.TrimStartAndEnd().ToLower();
		HeaderValue = HeaderValue.TrimStartAndEnd();
		if (HeaderName == TEXT("host"))
		{
			++Out.HostCount;
		}
		if (FString* Existing = Out.Headers.Find(HeaderName))
		{
			*Existing += TEXT("\n") + HeaderValue;
		}
		else
		{
			Out.Headers.Add(HeaderName, HeaderValue);
		}
	}
	if (!Out.Header(TEXT("transfer-encoding")).IsEmpty())
	{
		OutError = TEXT("Chunked requests are not supported.");
		OutStatus = 400;
		return false;
	}
	const FString LengthText = Out.Header(TEXT("content-length"));
	int64 Length = 0;
	if (!LengthText.IsEmpty())
	{
		if (!LengthText.IsNumeric() || LengthText.Contains(TEXT("\n")))
		{
			OutError = TEXT("Bad Content-Length.");
			OutStatus = 400;
			return false;
		}
		Length = FCString::Atoi64(*LengthText);
	}
	if (Length < 0 || Length > MaxBody)
	{
		OutError = TEXT("Request body too large.");
		OutStatus = 413;
		return false;
	}
	const int32 BodyStart = HeadEnd + 4;
	if (Data.Num() - BodyStart < Length)
	{
		return false;
	}
	Out.Body = TArray<uint8>(Data.GetData() + BodyStart, static_cast<int32>(Length));
	return true;
}

bool FScenarioLocalHttpServer::IsLoopbackHostHeader(const FString& Host, int32 InPort)
{
	FString Value = Host.TrimStartAndEnd().ToLower();
	if (Value.IsEmpty() || Value.Contains(TEXT("\n")))
	{
		return false;
	}
	FString Name = Value;
	FString PortText;
	if (Value.StartsWith(TEXT("[")))
	{
		const int32 Close = Value.Find(TEXT("]"));
		if (Close == INDEX_NONE)
		{
			return false;
		}
		Name = Value.Mid(1, Close - 1);
		const FString Rest = Value.Mid(Close + 1);
		if (!Rest.IsEmpty())
		{
			if (!Rest.StartsWith(TEXT(":")))
			{
				return false;
			}
			PortText = Rest.Mid(1);
		}
	}
	else if (Value.Split(TEXT(":"), &Name, &PortText))
	{
		// Name and PortText set by Split.
	}
	if (Name != TEXT("127.0.0.1") && Name != TEXT("localhost") && Name != TEXT("::1"))
	{
		return false;
	}
	return PortText.IsEmpty() || (PortText.IsNumeric() && FCString::Atoi(*PortText) == InPort);
}

bool FScenarioLocalHttpServer::IsAllowedOrigin(const FString& Origin)
{
	const FString Value = Origin.TrimStartAndEnd().ToLower();
	if (Value.IsEmpty())
	{
		return true;
	}
	for (const TCHAR* Prefix : { TEXT("http://127.0.0.1"), TEXT("http://localhost"), TEXT("http://[::1]") })
	{
		if (Value == Prefix || Value.StartsWith(FString(Prefix) + TEXT(":")))
		{
			return true;
		}
	}
	return false;
}

void FScenarioLocalHttpServer::ServeConnection(FSocket* Connection)
{
	TArray<uint8> Data;
	FScenarioLocalRequest Request;
	FString Error;
	int32 ErrorStatus = 400;
	bool bComplete = false;
	const double Deadline = FPlatformTime::Seconds() + IdleSeconds;
	uint8 Buffer[16384];
	while (!bStopping && FPlatformTime::Seconds() < Deadline)
	{
		if (!Connection->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(100)))
		{
			continue;
		}
		int32 Read = 0;
		if (!Connection->Recv(Buffer, sizeof(Buffer), Read) || Read <= 0)
		{
			break;
		}
		Data.Append(Buffer, Read);
		if (ParseRequest(Data, MaxBodyBytes, Request, Error, ErrorStatus))
		{
			bComplete = true;
			break;
		}
		if (!Error.IsEmpty())
		{
			break;
		}
	}
	if (!bComplete)
	{
		if (!Error.IsEmpty())
		{
			SendAll(Connection, FScenarioLocalResponse::Text(ErrorStatus, Error));
		}
		else if (!Data.IsEmpty())
		{
			SendAll(Connection, FScenarioLocalResponse::Text(408, TEXT("Request timed out.")));
		}
		Connection->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Connection);
		return;
	}

	// Hand the request to the game thread and wait for the reply here.
	struct FReplySlot
	{
		FCriticalSection Lock;
		FScenarioLocalResponse Response;
		FEvent* Done = nullptr;
		bool bSet = false;
		bool bAbandoned = false;
	};
	TSharedRef<FReplySlot, ESPMode::ThreadSafe> Slot = MakeShared<FReplySlot, ESPMode::ThreadSafe>();
	Slot->Done = FPlatformProcess::GetSynchEventFromPool(true);
	TWeakPtr<FScenarioLocalHttpServer, ESPMode::ThreadSafe> WeakThis = WeakSelf;
	AsyncTask(ENamedThreads::GameThread, [WeakThis, Request, Slot]()
	{
		const TSharedPtr<FScenarioLocalHttpServer, ESPMode::ThreadSafe> This = WeakThis.Pin();
		FScenarioLocalRespond Respond = [Slot](const FScenarioLocalResponse& Response)
		{
			FScopeLock Guard(&Slot->Lock);
			if (Slot->bSet || Slot->bAbandoned)
			{
				return;
			}
			Slot->Response = Response;
			Slot->bSet = true;
			Slot->Done->Trigger();
		};
		if (!This.IsValid() || !This->Handler)
		{
			Respond(FScenarioLocalResponse::Text(503, TEXT("Server stopping.")));
			return;
		}
		This->Handler(Request, Respond);
	});
	// Tools have their own time limits; this is the backstop.
	const uint32 WaitMs = 200 * 1000;
	bool bAnswered = false;
	const double WaitStart = FPlatformTime::Seconds();
	while (!bStopping && (FPlatformTime::Seconds() - WaitStart) * 1000.0 < WaitMs)
	{
		if (Slot->Done->Wait(100))
		{
			bAnswered = true;
			break;
		}
	}
	{
		FScopeLock Guard(&Slot->Lock);
		bAnswered = Slot->bSet;
		Slot->bAbandoned = !bAnswered;
	}
	if (bAnswered)
	{
		SendAll(Connection, Slot->Response);
	}
	else
	{
		SendAll(Connection, FScenarioLocalResponse::Text(503, bStopping ? TEXT("Server stopping.") : TEXT("The editor did not answer in time.")));
	}
	FPlatformProcess::ReturnSynchEventToPool(Slot->Done);
	Slot->Done = nullptr;
	Connection->Close();
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Connection);
}

void FScenarioLocalHttpServer::SendAll(FSocket* Connection, const FScenarioLocalResponse& Response)
{
	FString Head = FString::Printf(TEXT("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"),
		Response.Status, Reason(Response.Status), *Response.ContentType, Response.Body.Num());
	for (const TPair<FString, FString>& Pair : Response.Headers)
	{
		Head += Pair.Key + TEXT(": ") + Pair.Value + TEXT("\r\n");
	}
	Head += TEXT("\r\n");
	TArray<uint8> Bytes = Utf8Bytes(Head);
	Bytes.Append(Response.Body);
	int32 Offset = 0;
	const double Deadline = FPlatformTime::Seconds() + 10.0;
	while (Offset < Bytes.Num() && FPlatformTime::Seconds() < Deadline)
	{
		int32 Sent = 0;
		if (!Connection->Send(Bytes.GetData() + Offset, Bytes.Num() - Offset, Sent))
		{
			if (!Connection->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(100)))
			{
				continue;
			}
			continue;
		}
		Offset += Sent;
	}
}
