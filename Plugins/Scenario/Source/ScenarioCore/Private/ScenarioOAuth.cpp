// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioOAuth.h"

#include "ScenarioCoreModule.h"
#include "ScenarioCredentialStore.h"
#include "ScenarioErrors.h"
#include "ScenarioHash.h"
#include "ScenarioHttp.h"
#include "ScenarioJson.h"
#include "ScenarioLocalHttpServer.h"

const TCHAR* FScenarioOAuth::ProductionIssuer = TEXT("https://clerk.scenario.com");
const TCHAR* FScenarioOAuth::ProductionClientId = TEXT("U67CS7IbpZy0PPe0");
const TCHAR* FScenarioOAuth::DefaultScope = TEXT("openid offline_access");

namespace
{
	const TCHAR* const RefreshAccount = TEXT("oauth-refresh");
	const TCHAR* const ClientAccount = TEXT("oauth-client");
	const TCHAR* const DeviceGrant = TEXT("urn:ietf:params:oauth:grant-type:device_code");

	FString CallbackPage(bool bOk, const FString& Message)
	{
		const FString Title = bOk ? TEXT("Signed in to Scenario") : TEXT("Sign-in did not finish");
		const FString Body = bOk ? TEXT("You can return to Unreal Editor and close this tab.") : Message;
		FString Escaped = Body.Replace(TEXT("&"), TEXT("&amp;")).Replace(TEXT("<"), TEXT("&lt;")).Replace(TEXT(">"), TEXT("&gt;"));
		return FString::Printf(TEXT("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">")
			TEXT("<title>%s</title><style>body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:#111;color:#f2f2f2;")
			TEXT("font:16px/1.5 -apple-system,Segoe UI,Roboto,sans-serif}main{max-width:420px;padding:24px;text-align:center}h1{font-size:20px;margin:0 0 8px}")
			TEXT("p{color:#bdbdbd;margin:0}</style></head><body><main><h1>%s</h1><p>%s</p></main></body></html>"), *Title, *Title, *Escaped);
	}

	FString OAuthErrorText(const TSharedPtr<FJsonObject>& Object, const FString& Fallback)
	{
		const FString Code = ScenarioJson::GetString(Object, TEXT("error"));
		const FString Description = ScenarioJson::GetString(Object, TEXT("error_description"));
		if (!Description.IsEmpty())
		{
			return Description;
		}
		return Code.IsEmpty() ? Fallback : Code;
	}
}

bool FScenarioOAuthMetadata::FromJson(const TSharedPtr<FJsonObject>& Object, FScenarioOAuthMetadata& Out)
{
	if (!Object.IsValid())
	{
		return false;
	}
	Out.Issuer = ScenarioJson::GetString(Object, TEXT("issuer"));
	Out.AuthorizationEndpoint = ScenarioJson::GetString(Object, TEXT("authorization_endpoint"));
	Out.TokenEndpoint = ScenarioJson::GetString(Object, TEXT("token_endpoint"));
	Out.DeviceAuthorizationEndpoint = ScenarioJson::GetString(Object, TEXT("device_authorization_endpoint"));
	Out.RevocationEndpoint = ScenarioJson::GetString(Object, TEXT("revocation_endpoint"));
	// Endpoints must be https so codes and tokens never travel in clear.
	for (const FString* Url : { &Out.AuthorizationEndpoint, &Out.TokenEndpoint })
	{
		if (!ScenarioUrl::IsHttps(*Url))
		{
			return false;
		}
	}
	return Out.IsValid();
}

FScenarioOAuthApp FScenarioOAuthApp::Select(const FString& EnvIssuer, const FString& EnvClientId)
{
	FScenarioOAuthApp Out;
	FString Issuer = EnvIssuer.TrimStartAndEnd();
	Issuer.RemoveFromEnd(TEXT("/"));
	const FString ClientId = EnvClientId.TrimStartAndEnd();
	if (Issuer.IsEmpty() && ClientId.IsEmpty())
	{
		Out.Issuer = FScenarioOAuth::ProductionIssuer;
		Out.ClientId = FScenarioOAuth::ProductionClientId;
		return Out;
	}
	if (Issuer.IsEmpty() || ClientId.IsEmpty() || !ScenarioUrl::IsHttps(Issuer))
	{
		Out.Problem = TEXT("Set SCENARIO_OAUTH_ISSUER (HTTPS) and SCENARIO_OAUTH_CLIENT_ID together, or neither.");
		return Out;
	}
	Out.Issuer = Issuer;
	Out.ClientId = ClientId;
	return Out;
}

FScenarioOAuthApp FScenarioOAuthApp::FromEnvironment()
{
	return Select(FPlatformMisc::GetEnvironmentVariable(TEXT("SCENARIO_OAUTH_ISSUER")), FPlatformMisc::GetEnvironmentVariable(TEXT("SCENARIO_OAUTH_CLIENT_ID")));
}

const TArray<int32>& FScenarioOAuth::RedirectPorts()
{
	static const TArray<int32> Ports = { 38917, 38918, 38919 };
	return Ports;
}

FScenarioOAuth::FScenarioOAuth(TSharedRef<IScenarioHttpTransport> InTransport, TSharedRef<IScenarioCredentialStore> InStore)
	: App(FScenarioOAuthApp::FromEnvironment())
	, Scope(DefaultScope)
	, Transport(InTransport)
	, Store(InStore)
{
}

bool FScenarioOAuth::HasCredentials() const
{
	return !AccessToken.IsEmpty() || !RefreshToken.IsEmpty();
}

FString FScenarioOAuth::AuthorizationHeader() const
{
	// With only a refresh token, the empty bearer gets a 401 and the client refreshes once, then resends.
	return TEXT("Bearer ") + AccessToken;
}

bool FScenarioOAuth::IsAccessTokenFresh() const
{
	return !AccessToken.IsEmpty() && (AccessExpiresAt <= 0.0 || ScenarioNow() < AccessExpiresAt - 30.0);
}

bool FScenarioOAuth::Restore()
{
	FString Saved;
	// With a refused SCENARIO_OAUTH_* pair there is no application to compare with: keep the token.
	if (!App.Problem.IsEmpty() || !Store->Read(RefreshAccount, Saved) || Saved.IsEmpty())
	{
		return false;
	}
	FString ClientJson;
	Store->Read(ClientAccount, ClientJson);
	const TSharedPtr<FJsonObject> Client = ScenarioJson::ParseObject(ClientJson);
	if (ScenarioJson::GetString(Client, TEXT("client_id")) != App.ClientId
		|| ScenarioJson::GetString(Client, TEXT("issuer")) != App.Issuer)
	{
		// Saved by another application, such as the client an older version registered: its provider
		// is not this one, so the token is only forgotten.
		UE_LOG(LogScenario, Log, TEXT("Scenario sign-in: the saved session belongs to another sign-in application; sign in again."));
		ForgetSession();
		return false;
	}
	RefreshToken = Saved;
	return true;
}

bool FScenarioOAuth::UseEnvironmentToken()
{
	const FString Token = FPlatformMisc::GetEnvironmentVariable(TEXT("SCENARIO_ACCESS_TOKEN")).TrimStartAndEnd();
	if (Token.IsEmpty())
	{
		return false;
	}
	AccessToken = Token;
	AccessExpiresAt = 0.0;
	return true;
}

FString FScenarioOAuth::MakeCodeChallenge(const FString& Verifier)
{
	const FTCHARToUTF8 Utf8(*Verifier);
	return ScenarioHash::Base64Url(ScenarioHash::Sha256(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}

FString FScenarioOAuth::BuildAuthorizeUrl(const FScenarioOAuthMetadata& InMetadata, const FString& InClientId, const FString& RedirectUri,
	const FString& Challenge, const FString& State, const FString& InScope)
{
	TArray<TPair<FString, FString>> Query = {
		{ TEXT("response_type"), TEXT("code") },
		{ TEXT("client_id"), InClientId },
		{ TEXT("redirect_uri"), RedirectUri },
		{ TEXT("code_challenge"), Challenge },
		{ TEXT("code_challenge_method"), TEXT("S256") },
		{ TEXT("state"), State },
	};
	if (!InScope.IsEmpty())
	{
		Query.Emplace(TEXT("scope"), InScope);
	}
	const FString Separator = InMetadata.AuthorizationEndpoint.Contains(TEXT("?")) ? TEXT("&") : TEXT("?");
	return InMetadata.AuthorizationEndpoint + Separator + ScenarioUrl::BuildQuery(Query);
}

FString FScenarioOAuth::ReadCallback(const FString& Query, const FString& ExpectedState, const FString& ExpectedIssuer, FString& OutCode)
{
	OutCode.Reset();
	const TMap<FString, FString> Values = ScenarioUrl::ParseQuery(Query);
	const FString* State = Values.Find(TEXT("state"));
	if (!State || ExpectedState.IsEmpty() || *State != ExpectedState)
	{
		return TEXT("The sign-in reply did not match this request. Start the sign-in again from Unreal.");
	}
	if (const FString* Error = Values.Find(TEXT("error")))
	{
		const FString* Description = Values.Find(TEXT("error_description"));
		if (*Error == TEXT("access_denied"))
		{
			return TEXT("Sign-in was cancelled in the browser.");
		}
		return Description && !Description->IsEmpty() ? *Description : *Error;
	}
	if (const FString* Issuer = Values.Find(TEXT("iss")))
	{
		if (!ExpectedIssuer.IsEmpty() && *Issuer != ExpectedIssuer)
		{
			return TEXT("The sign-in reply came from an unexpected server.");
		}
	}
	const FString* Code = Values.Find(TEXT("code"));
	if (!Code || Code->IsEmpty())
	{
		return TEXT("The sign-in reply had no authorization code.");
	}
	OutCode = *Code;
	return FString();
}

void FScenarioOAuth::Discover(TFunction<void(const FScenarioReply&)> OnDone)
{
	if (!App.Problem.IsEmpty() || App.ClientId.IsEmpty())
	{
		OnDone(FScenarioReply::Fail(EScenarioErrorKind::Local, App.Problem.IsEmpty() ? FString(TEXT("Sign-in is not set up in this build. Use an API key instead.")) : App.Problem));
		return;
	}
	if (Metadata.IsValid())
	{
		OnDone(FScenarioReply::Ok());
		return;
	}
	TMap<FString, FString> Headers;
	Headers.Add(TEXT("Accept"), TEXT("application/json"));
	Headers.Add(TEXT("User-Agent"), ScenarioVersion::UserAgent());
	TWeakPtr<FScenarioOAuth> WeakThis = AsShared();
	Transport->Send(TEXT("GET"), App.MetadataUrl(), Headers, TArray<uint8>(), 20.0f, [WeakThis, OnDone](const FScenarioHttpResponse& Response)
	{
		const TSharedPtr<FScenarioOAuth> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		if (Response.Status != 200)
		{
			const EScenarioErrorKind Kind = Response.Status == 0 ? EScenarioErrorKind::Transport : EScenarioErrorKind::Scenario;
			OnDone(FScenarioReply::Fail(Kind, TEXT("Could not reach Scenario's sign-in service.")));
			return;
		}
		FScenarioOAuthMetadata Found;
		if (!FScenarioOAuthMetadata::FromJson(ScenarioJson::ParseObject(Response.BodyText()), Found))
		{
			OnDone(FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario's sign-in service sent unreadable settings.")));
			return;
		}
		This->Metadata = Found;
		OnDone(FScenarioReply::Ok());
	});
}

void FScenarioOAuth::PostForm(const FString& Url, const TArray<TPair<FString, FString>>& Fields, FScenarioReplyCallback OnDone)
{
	const FString Text = ScenarioUrl::BuildQuery(Fields);
	const FTCHARToUTF8 Utf8(*Text);
	TMap<FString, FString> Headers;
	Headers.Add(TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded"));
	Headers.Add(TEXT("Accept"), TEXT("application/json"));
	Headers.Add(TEXT("User-Agent"), ScenarioVersion::UserAgent());
	Transport->Send(TEXT("POST"), Url, Headers, TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), 30.0f,
		[OnDone](const FScenarioHttpResponse& Response)
	{
		if (Response.Status == 0)
		{
			OnDone(FScenarioReply::Fail(Response.TransportError == EScenarioErrorKind::Timeout ? EScenarioErrorKind::Timeout : EScenarioErrorKind::Transport,
				TEXT("Could not reach Scenario's sign-in service.")));
			return;
		}
		const TSharedPtr<FJsonObject> Reply = ScenarioJson::ParseObject(Response.BodyText());
		if (Response.Status < 200 || Response.Status >= 300)
		{
			FScenarioReply Failed = FScenarioReply::Fail(EScenarioErrorKind::Auth, FScenarioErrors::Scrub(OAuthErrorText(Reply, FString::Printf(TEXT("HTTP %d"), Response.Status))));
			Failed.Error.HttpStatus = Response.Status;
			// The OAuth error code, for device polling.
			Failed.Error.Detail = ScenarioJson::GetString(Reply, TEXT("error"));
			OnDone(Failed);
			return;
		}
		if (!Reply.IsValid())
		{
			OnDone(FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario's sign-in service sent an unreadable reply.")));
			return;
		}
		OnDone(FScenarioReply::Ok(Reply));
	});
}

bool FScenarioOAuth::ApplyTokenResponse(const TSharedPtr<FJsonObject>& Token)
{
	const FString NewAccess = ScenarioJson::GetString(Token, TEXT("access_token"));
	if (NewAccess.IsEmpty())
	{
		return false;
	}
	AccessToken = NewAccess;
	const double ExpiresIn = ScenarioJson::GetNumber(Token, TEXT("expires_in"), 0.0);
	AccessExpiresAt = ExpiresIn > 0.0 ? ScenarioNow() + ExpiresIn : 0.0;
	const FString NewRefresh = ScenarioJson::GetString(Token, TEXT("refresh_token"));
	if (!NewRefresh.IsEmpty())
	{
		RefreshToken = NewRefresh;
		// Which application and provider the token belongs to, so a later session only resumes it with them.
		TSharedRef<FJsonObject> Client = MakeShared<FJsonObject>();
		Client->SetStringField(TEXT("client_id"), App.ClientId);
		Client->SetStringField(TEXT("issuer"), App.Issuer);
		// The token is saved even when its identity is not: Restore forgets a token whose saved identity does not match.
		const bool bClientSaved = Store->Write(ClientAccount, ScenarioJson::Write(Client));
		if (!Store->Write(RefreshAccount, RefreshToken) || !bClientSaved)
		{
			UE_LOG(LogScenario, Warning, TEXT("Scenario sign-in: could not save the session in %s; you will need to sign in again next time."), *Store->Describe());
		}
	}
	return true;
}

void FScenarioOAuth::BeginBrowserSignIn(TFunction<void(const FString&)> OnUrl, FScenarioReplyCallback OnDone)
{
	Cancel();
	bSigningIn = true;
	const uint32 Generation = ++SignInGeneration;
	PendingDone = MoveTemp(OnDone);
	TWeakPtr<FScenarioOAuth> WeakThis = AsShared();
	Discover([WeakThis, Generation, OnUrl](const FScenarioReply& Discovered)
	{
		const TSharedPtr<FScenarioOAuth> This = WeakThis.Pin();
		if (!This.IsValid() || This->SignInGeneration != Generation)
		{
			return;
		}
		if (!Discovered.bOk)
		{
			This->FinishSignIn(Discovered);
			return;
		}
		This->PendingVerifier = ScenarioHash::RandomToken(48);
		This->PendingState = ScenarioHash::RandomToken(24);
		TSharedPtr<FScenarioLocalHttpServer, ESPMode::ThreadSafe> Server = MakeShared<FScenarioLocalHttpServer, ESPMode::ThreadSafe>(TEXT("SignIn"),
			[WeakThis, Generation](const FScenarioLocalRequest& Request, FScenarioLocalRespond Respond)
		{
			const TSharedPtr<FScenarioOAuth> Owner = WeakThis.Pin();
			if (Request.Path != TEXT("/callback") || Request.Method != TEXT("GET"))
			{
				Respond(FScenarioLocalResponse::Text(404, TEXT("Not found.")));
				return;
			}
			if (!Owner.IsValid() || Owner->SignInGeneration != Generation || !Owner->bSigningIn)
			{
				Respond(FScenarioLocalResponse::Text(200, CallbackPage(false, TEXT("This sign-in is no longer active. Start again from Unreal.")), TEXT("text/html; charset=utf-8")));
				return;
			}
			FString Code;
			const FString Problem = ReadCallback(Request.Query, Owner->PendingState, Owner->Metadata.Issuer, Code);
			if (!Problem.IsEmpty())
			{
				Respond(FScenarioLocalResponse::Text(200, CallbackPage(false, Problem), TEXT("text/html; charset=utf-8")));
				Owner->FinishSignIn(FScenarioReply::Fail(EScenarioErrorKind::Auth, Problem));
				return;
			}
			Owner->ExchangeCode(Code, Owner->PendingRedirectUri, [WeakThis, Respond](const FScenarioReply& Exchanged)
			{
				Respond(FScenarioLocalResponse::Text(200, CallbackPage(Exchanged.bOk, Exchanged.Error.Message), TEXT("text/html; charset=utf-8")));
				if (const TSharedPtr<FScenarioOAuth> Final = WeakThis.Pin())
				{
					Final->FinishSignIn(Exchanged);
				}
			});
		});
		bool bStarted = false;
		for (const int32 Port : RedirectPorts())
		{
			if (Server->Start(Port, 1))
			{
				bStarted = true;
				break;
			}
		}
		if (!bStarted)
		{
			This->FinishSignIn(FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("The local sign-in ports are busy. Use \"Sign in on another device\" or an API key.")));
			return;
		}
		This->CallbackServer = Server;
		This->PendingRedirectUri = FString::Printf(TEXT("http://127.0.0.1:%d/callback"), Server->GetPort());
		const FString Url = BuildAuthorizeUrl(This->Metadata, This->App.ClientId, This->PendingRedirectUri, MakeCodeChallenge(This->PendingVerifier),
			This->PendingState, This->Scope);
		This->Transport->Delay(static_cast<float>(BrowserTimeoutSeconds), [WeakThis, Generation]()
		{
			const TSharedPtr<FScenarioOAuth> Late = WeakThis.Pin();
			if (Late.IsValid() && Late->SignInGeneration == Generation && Late->bSigningIn)
			{
				Late->FinishSignIn(FScenarioReply::Fail(EScenarioErrorKind::Timeout, TEXT("Sign-in timed out. Start it again, or use \"Sign in on another device\".")));
			}
		});
		OnUrl(Url);
	});
}

void FScenarioOAuth::ExchangeCode(const FString& Code, const FString& RedirectUri, FScenarioReplyCallback OnDone)
{
	TArray<TPair<FString, FString>> Fields = {
		{ TEXT("grant_type"), TEXT("authorization_code") },
		{ TEXT("code"), Code },
		{ TEXT("redirect_uri"), RedirectUri },
		{ TEXT("client_id"), App.ClientId },
		{ TEXT("code_verifier"), PendingVerifier },
	};
	TWeakPtr<FScenarioOAuth> WeakThis = AsShared();
	PostForm(Metadata.TokenEndpoint, Fields, [WeakThis, OnDone](const FScenarioReply& Reply)
	{
		const TSharedPtr<FScenarioOAuth> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		if (!Reply.bOk)
		{
			OnDone(Reply);
			return;
		}
		if (!This->ApplyTokenResponse(Reply.Value))
		{
			OnDone(FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario's sign-in service returned no access token.")));
			return;
		}
		OnDone(FScenarioReply::Ok());
	});
}

void FScenarioOAuth::BeginDeviceSignIn(TFunction<void(const FScenarioDeviceCode&)> OnCode, FScenarioReplyCallback OnDone)
{
	Cancel();
	bSigningIn = true;
	const uint32 Generation = ++SignInGeneration;
	PendingDone = MoveTemp(OnDone);
	TWeakPtr<FScenarioOAuth> WeakThis = AsShared();
	Discover([WeakThis, Generation, OnCode](const FScenarioReply& Discovered)
	{
		const TSharedPtr<FScenarioOAuth> This = WeakThis.Pin();
		if (!This.IsValid() || This->SignInGeneration != Generation)
		{
			return;
		}
		if (!Discovered.bOk)
		{
			This->FinishSignIn(Discovered);
			return;
		}
		if (This->Metadata.DeviceAuthorizationEndpoint.IsEmpty())
		{
			This->FinishSignIn(FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario does not offer sign-in on another device. Use an API key instead.")));
			return;
		}
		TArray<TPair<FString, FString>> Fields = { { TEXT("client_id"), This->App.ClientId } };
		if (!This->Scope.IsEmpty())
		{
			Fields.Emplace(TEXT("scope"), This->Scope);
		}
		This->PostForm(This->Metadata.DeviceAuthorizationEndpoint, Fields, [WeakThis, Generation, OnCode](const FScenarioReply& Reply)
		{
			const TSharedPtr<FScenarioOAuth> Owner = WeakThis.Pin();
			if (!Owner.IsValid() || Owner->SignInGeneration != Generation)
			{
				return;
			}
			if (!Reply.bOk)
			{
				Owner->FinishSignIn(Reply);
				return;
			}
			const FString DeviceCode = ScenarioJson::GetString(Reply.Value, TEXT("device_code"));
			FScenarioDeviceCode Code;
			Code.UserCode = ScenarioJson::GetString(Reply.Value, TEXT("user_code"));
			Code.VerificationUri = ScenarioJson::GetString(Reply.Value, TEXT("verification_uri"));
			Code.VerificationUriComplete = ScenarioJson::GetString(Reply.Value, TEXT("verification_uri_complete"));
			const double ExpiresIn = ScenarioJson::GetNumber(Reply.Value, TEXT("expires_in"), 600.0);
			Code.ExpiresAt = ScenarioNow() + ExpiresIn;
			if (DeviceCode.IsEmpty() || Code.UserCode.IsEmpty() || !ScenarioUrl::IsHttps(Code.VerificationUri))
			{
				Owner->FinishSignIn(FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario's sign-in service sent an incomplete device code.")));
				return;
			}
			OnCode(Code);
			const double Interval = FMath::Max(1.0, ScenarioJson::GetNumber(Reply.Value, TEXT("interval"), 5.0));
			Owner->PollDevice(DeviceCode, Interval, Code.ExpiresAt, 0);
		});
	});
}

void FScenarioOAuth::PollDevice(const FString& DeviceCode, double Interval, double ExpiresAt, uint32 Attempt)
{
	const uint32 Generation = SignInGeneration;
	TWeakPtr<FScenarioOAuth> WeakThis = AsShared();
	Transport->Delay(static_cast<float>(Interval), [WeakThis, Generation, DeviceCode, Interval, ExpiresAt, Attempt]()
	{
		const TSharedPtr<FScenarioOAuth> This = WeakThis.Pin();
		if (!This.IsValid() || This->SignInGeneration != Generation || !This->bSigningIn)
		{
			return;
		}
		if (ScenarioNow() > ExpiresAt)
		{
			This->FinishSignIn(FScenarioReply::Fail(EScenarioErrorKind::Timeout, TEXT("The code expired. Start the sign-in again.")));
			return;
		}
		TArray<TPair<FString, FString>> Fields = {
			{ TEXT("grant_type"), DeviceGrant },
			{ TEXT("device_code"), DeviceCode },
			{ TEXT("client_id"), This->App.ClientId },
		};
		This->PostForm(This->Metadata.TokenEndpoint, Fields, [WeakThis, Generation, DeviceCode, Interval, ExpiresAt, Attempt](const FScenarioReply& Reply)
		{
			const TSharedPtr<FScenarioOAuth> Self = WeakThis.Pin();
			if (!Self.IsValid() || Self->SignInGeneration != Generation || !Self->bSigningIn)
			{
				return;
			}
			if (Reply.bOk)
			{
				if (Self->ApplyTokenResponse(Reply.Value))
				{
					Self->FinishSignIn(FScenarioReply::Ok());
				}
				else
				{
					Self->FinishSignIn(FScenarioReply::Fail(EScenarioErrorKind::Protocol, TEXT("Scenario's sign-in service returned no access token.")));
				}
				return;
			}
			const FString Code = Reply.Error.Detail;
			if (Code == TEXT("authorization_pending"))
			{
				Self->PollDevice(DeviceCode, Interval, ExpiresAt, Attempt + 1);
			}
			else if (Code == TEXT("slow_down"))
			{
				Self->PollDevice(DeviceCode, Interval + 5.0, ExpiresAt, Attempt + 1);
			}
			else if (Reply.Error.Kind == EScenarioErrorKind::Transport || Reply.Error.Kind == EScenarioErrorKind::Timeout)
			{
				// Polling is a read: keep going while the code is valid.
				Self->PollDevice(DeviceCode, Interval * 2.0, ExpiresAt, Attempt + 1);
			}
			else if (Code == TEXT("access_denied"))
			{
				Self->FinishSignIn(FScenarioReply::Fail(EScenarioErrorKind::Auth, TEXT("Sign-in was declined.")));
			}
			else if (Code == TEXT("expired_token"))
			{
				Self->FinishSignIn(FScenarioReply::Fail(EScenarioErrorKind::Timeout, TEXT("The code expired. Start the sign-in again.")));
			}
			else
			{
				Self->FinishSignIn(Reply);
			}
		});
	});
}

void FScenarioOAuth::Refresh(TFunction<void(bool)> OnDone)
{
	if (RefreshToken.IsEmpty())
	{
		OnDone(false);
		return;
	}
	RefreshWaiters.Add(MoveTemp(OnDone));
	if (bRefreshing)
	{
		return;
	}
	bRefreshing = true;
	TWeakPtr<FScenarioOAuth> WeakThis = AsShared();
	auto Finish = [WeakThis](bool bOk)
	{
		const TSharedPtr<FScenarioOAuth> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		This->bRefreshing = false;
		TArray<TFunction<void(bool)>> Waiters = MoveTemp(This->RefreshWaiters);
		This->RefreshWaiters.Reset();
		This->OnChanged.Broadcast();
		for (TFunction<void(bool)>& Waiter : Waiters)
		{
			Waiter(bOk);
		}
	};
	Discover([WeakThis, Finish](const FScenarioReply& Discovered)
	{
		const TSharedPtr<FScenarioOAuth> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		if (!Discovered.bOk)
		{
			Finish(false);
			return;
		}
		TArray<TPair<FString, FString>> Fields = {
			{ TEXT("grant_type"), TEXT("refresh_token") },
			{ TEXT("refresh_token"), This->RefreshToken },
			{ TEXT("client_id"), This->App.ClientId },
		};
		This->PostForm(This->Metadata.TokenEndpoint, Fields, [WeakThis, Finish](const FScenarioReply& Reply)
		{
			const TSharedPtr<FScenarioOAuth> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Reply.bOk && Self->ApplyTokenResponse(Reply.Value))
			{
				Finish(true);
				return;
			}
			if (!Reply.bOk && (Reply.Error.Detail == TEXT("invalid_grant") || Reply.Error.HttpStatus == 400 || Reply.Error.HttpStatus == 401))
			{
				// The session was revoked or expired: sign in again.
				UE_LOG(LogScenario, Log, TEXT("Scenario session expired; sign in again."));
				Self->ForgetSession();
			}
			Finish(false);
		});
	});
}

void FScenarioOAuth::Cancel()
{
	if (bSigningIn)
	{
		FinishSignIn(FScenarioReply::Fail(EScenarioErrorKind::Cancelled, TEXT("Sign-in cancelled.")));
	}
}

void FScenarioOAuth::FinishSignIn(const FScenarioReply& Result)
{
	if (!bSigningIn)
	{
		return;
	}
	bSigningIn = false;
	++SignInGeneration;
	PendingState.Reset();
	PendingVerifier.Reset();
	StopCallbackServer();
	FScenarioReplyCallback Done = MoveTemp(PendingDone);
	PendingDone = nullptr;
	if (Result.bOk)
	{
		UE_LOG(LogScenario, Log, TEXT("Signed in to Scenario."));
		OnChanged.Broadcast();
	}
	if (Done)
	{
		Done(Result);
	}
}

void FScenarioOAuth::StopCallbackServer()
{
	if (!CallbackServer.IsValid())
	{
		return;
	}
	// Let the browser receive its page before the listener goes away.
	TSharedPtr<FScenarioLocalHttpServer, ESPMode::ThreadSafe> Server = CallbackServer;
	CallbackServer.Reset();
	Transport->Delay(1.0f, [Server]()
	{
		Server->Stop();
	});
}

void FScenarioOAuth::SignOut()
{
	Cancel();
	const FString Token = RefreshToken;
	if (!Token.IsEmpty() && !Metadata.RevocationEndpoint.IsEmpty() && ScenarioUrl::IsHttps(Metadata.RevocationEndpoint))
	{
		PostForm(Metadata.RevocationEndpoint, { { TEXT("token"), Token }, { TEXT("token_type_hint"), TEXT("refresh_token") }, { TEXT("client_id"), App.ClientId } },
			[](const FScenarioReply&) {});
	}
	ForgetSession();
	OnChanged.Broadcast();
}

void FScenarioOAuth::ForgetSession()
{
	RefreshToken.Reset();
	AccessToken.Reset();
	AccessExpiresAt = 0.0;
	Store->Delete(RefreshAccount);
	Store->Delete(ClientAccount);
}
