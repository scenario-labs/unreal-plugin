// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScenarioCredentialStore.h"
#include "ScenarioFakes.h"
#include "ScenarioHash.h"
#include "ScenarioHttp.h"
#include "ScenarioJson.h"
#include "ScenarioOAuth.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

namespace ScenarioAuthTests
{
	const TCHAR* Metadata = TEXT("{\"issuer\":\"https://auth.example.test\",\"authorization_endpoint\":\"https://auth.example.test/oauth/authorize\",")
		TEXT("\"token_endpoint\":\"https://auth.example.test/oauth/token\",")
		TEXT("\"device_authorization_endpoint\":\"https://auth.example.test/oauth/device\",\"revocation_endpoint\":\"https://auth.example.test/oauth/revoke\"}");

	/** A small authorization server: discovery, device code, token. */
	struct FAuthServer
	{
		TArray<FString> TokenReplies;
		int32 TokenCalls = 0;
		TArray<FString> TokenBodies;

		FScenarioHttpResponse Handle(const FScenarioFakeRequest& Request)
		{
			if (Request.Url.EndsWith(TEXT("/.well-known/oauth-authorization-server")))
			{
				return FScenarioFakeTransport::Json(200, Metadata);
			}
			if (Request.Url.EndsWith(TEXT("/oauth/device")))
			{
				return FScenarioFakeTransport::Json(200, TEXT("{\"device_code\":\"dev_1\",\"user_code\":\"ABCD-EFGH\",\"verification_uri\":\"https://auth.example.test/device\",\"interval\":5,\"expires_in\":600}"));
			}
			if (Request.Url.EndsWith(TEXT("/oauth/token")))
			{
				TokenBodies.Add(Request.BodyText());
				const FString Reply = TokenReplies.IsValidIndex(TokenCalls) ? TokenReplies[TokenCalls] : FString(TEXT("400:{\"error\":\"invalid_grant\"}"));
				++TokenCalls;
				FString Status;
				FString Body;
				Reply.Split(TEXT(":"), &Status, &Body);
				return FScenarioFakeTransport::Json(FCString::Atoi(*Status), Body);
			}
			FScenarioHttpResponse Missing;
			Missing.Status = 404;
			return Missing;
		}
	};

	FScenarioOAuthApp TestApp()
	{
		FScenarioOAuthApp App;
		App.Issuer = TEXT("https://auth.example.test");
		App.ClientId = TEXT("client_test");
		return App;
	}

	/** An unsigned JWT carrying these claims. */
	FString MakeJwt(const FString& ClaimsJson)
	{
		auto Part = [](const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			return ScenarioHash::Base64Url(TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
		};
		return Part(TEXT("{\"alg\":\"RS256\",\"typ\":\"JWT\"}")) + TEXT(".") + Part(ClaimsJson) + TEXT(".signature");
	}

	/** The ID token the test provider issues for an account. */
	FString IdToken(const FString& Sub, const FString& Email)
	{
		return MakeJwt(FString::Printf(TEXT("{\"iss\":\"https://auth.example.test\",\"aud\":\"client_test\",\"sub\":\"%s\",\"email\":\"%s\"}"), *Sub, *Email));
	}

	/** A token endpoint reply: status, then the JSON body. */
	FString TokenReply(const FString& Access, const FString& Refresh, const FString& IdTokenValue = FString())
	{
		FString Body = FString::Printf(TEXT("{\"access_token\":\"%s\",\"expires_in\":3600,\"token_type\":\"Bearer\""), *Access);
		if (!Refresh.IsEmpty())
		{
			Body += FString::Printf(TEXT(",\"refresh_token\":\"%s\""), *Refresh);
		}
		if (!IdTokenValue.IsEmpty())
		{
			Body += FString::Printf(TEXT(",\"id_token\":\"%s\""), *IdTokenValue);
		}
		return TEXT("200:") + Body + TEXT("}");
	}

	/** A memory store that refuses writes to one account. */
	class FRefusingStore : public FScenarioMemoryCredentialStore
	{
	public:
		FString RefusedAccount;
		virtual bool Write(const FString& Account, const FString& Secret) override
		{
			return Account != RefusedAccount && FScenarioMemoryCredentialStore::Write(Account, Secret);
		}
	};
}

using namespace ScenarioAuthTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioOAuthHelpersTest, "Scenario.Core.Auth.Helpers", SCENARIO_TEST_FLAGS)
bool FScenarioOAuthHelpersTest::RunTest(const FString& Parameters)
{
	// RFC 7636 appendix B.
	TestEqual(TEXT("pkce vector"), FScenarioOAuth::MakeCodeChallenge(TEXT("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")), FString(TEXT("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM")));

	FScenarioOAuthMetadata Parsed;
	TestTrue(TEXT("metadata"), FScenarioOAuthMetadata::FromJson(ScenarioJson::ParseObject(Metadata), Parsed));
	TestFalse(TEXT("http endpoints refused"), FScenarioOAuthMetadata::FromJson(ScenarioJson::ParseObject(
		TEXT("{\"authorization_endpoint\":\"http://a.test/authorize\",\"token_endpoint\":\"https://a.test/token\"}")), Parsed));

	FScenarioOAuthMetadata Good;
	FScenarioOAuthMetadata::FromJson(ScenarioJson::ParseObject(Metadata), Good);
	const FString Url = FScenarioOAuth::BuildAuthorizeUrl(Good, TEXT("client_test"), TEXT("http://127.0.0.1:38917/callback"), TEXT("challenge"), TEXT("state1"), FScenarioOAuth::DefaultScope);
	TestTrue(TEXT("authorize endpoint"), Url.StartsWith(TEXT("https://auth.example.test/oauth/authorize?")));
	TestTrue(TEXT("code flow"), Url.Contains(TEXT("response_type=code")));
	TestTrue(TEXT("client id"), Url.Contains(TEXT("client_id=client_test")));
	TestTrue(TEXT("S256"), Url.Contains(TEXT("code_challenge_method=S256")));
	TestTrue(TEXT("redirect encoded"), Url.Contains(TEXT("redirect_uri=http%3A%2F%2F127.0.0.1%3A38917%2Fcallback")));
	TestTrue(TEXT("scope"), Url.Contains(TEXT("scope=openid%20profile%20email%20offline_access")));
	TestFalse(TEXT("no resource"), Url.Contains(TEXT("resource=")));

	FString Code;
	TestFalse(TEXT("state mismatch refused"), FScenarioOAuth::ReadCallback(TEXT("code=abc&state=other"), TEXT("state1"), FString(), Code).IsEmpty());
	TestTrue(TEXT("no code on mismatch"), Code.IsEmpty());
	TestEqual(TEXT("cancelled"), FScenarioOAuth::ReadCallback(TEXT("error=access_denied&state=state1"), TEXT("state1"), FString(), Code), FString(TEXT("Sign-in was cancelled in the browser.")));
	TestFalse(TEXT("wrong issuer refused"), FScenarioOAuth::ReadCallback(TEXT("code=abc&state=state1&iss=https%3A%2F%2Fevil.test"), TEXT("state1"), TEXT("https://auth.example.test"), Code).IsEmpty());
	TestTrue(TEXT("good callback"), FScenarioOAuth::ReadCallback(TEXT("code=abc&state=state1&iss=https%3A%2F%2Fauth.example.test"), TEXT("state1"), TEXT("https://auth.example.test"), Code).IsEmpty());
	TestEqual(TEXT("code read"), Code, FString(TEXT("abc")));

	// ID token claims; "?>" encodes with the URL-safe characters.
	const TSharedPtr<FJsonObject> Claims = FScenarioOAuth::ReadTokenClaims(MakeJwt(TEXT("{\"sub\":\"user_1\",\"name\":\"?>?>\"}")));
	TestEqual(TEXT("claims read"), ScenarioJson::GetString(Claims, TEXT("sub")), FString(TEXT("user_1")));
	TestEqual(TEXT("url-safe payload"), ScenarioJson::GetString(Claims, TEXT("name")), FString(TEXT("?>?>")));
	TestFalse(TEXT("opaque token"), FScenarioOAuth::ReadTokenClaims(TEXT("at_opaque")).IsValid());
	TestFalse(TEXT("not base64"), FScenarioOAuth::ReadTokenClaims(TEXT("a.!!!.c")).IsValid());
	TArray<uint8> Bytes;
	TestTrue(TEXT("padded base64url"), ScenarioHash::FromBase64Url(TEXT("_-8="), Bytes) && Bytes.Num() == 2 && Bytes[0] == 0xFF && Bytes[1] == 0xEF);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioOAuthAppTest, "Scenario.Core.Auth.App", SCENARIO_TEST_FLAGS)
bool FScenarioOAuthAppTest::RunTest(const FString& Parameters)
{
	const FScenarioOAuthApp Production = FScenarioOAuthApp::Select(FString(), FString());
	TestEqual(TEXT("production issuer"), Production.Issuer, FString(TEXT("https://clerk.scenario.com")));
	TestEqual(TEXT("production client"), Production.ClientId, FString(TEXT("U67CS7IbpZy0PPe0")));
	TestTrue(TEXT("production usable"), Production.Problem.IsEmpty());
	TestEqual(TEXT("metadata url"), Production.MetadataUrl(), FString(TEXT("https://clerk.scenario.com/.well-known/oauth-authorization-server")));

	const FScenarioOAuthApp Development = FScenarioOAuthApp::Select(TEXT(" https://dev.example.test/ "), TEXT("dev_client"));
	TestEqual(TEXT("development issuer trimmed"), Development.Issuer, FString(TEXT("https://dev.example.test")));
	TestEqual(TEXT("development client"), Development.ClientId, FString(TEXT("dev_client")));
	TestTrue(TEXT("development usable"), Development.Problem.IsEmpty());

	TestFalse(TEXT("issuer alone refused"), FScenarioOAuthApp::Select(TEXT("https://dev.example.test"), FString()).Problem.IsEmpty());
	TestFalse(TEXT("client alone refused"), FScenarioOAuthApp::Select(FString(), TEXT("dev_client")).Problem.IsEmpty());
	TestFalse(TEXT("http issuer refused"), FScenarioOAuthApp::Select(TEXT("http://dev.example.test"), TEXT("dev_client")).Problem.IsEmpty());

	// A refused override fails the sign-in before anything is sent.
	TSharedRef<FScenarioFakeTransport> Transport = MakeShared<FScenarioFakeTransport>();
	TSharedRef<FScenarioOAuth> OAuth = MakeShared<FScenarioOAuth>(Transport, MakeShared<FScenarioMemoryCredentialStore>());
	OAuth->App = FScenarioOAuthApp::Select(TEXT("https://dev.example.test"), FString());
	FScenarioReply Result = FScenarioReply::Ok();
	OAuth->BeginBrowserSignIn([](const FString&) {}, [&Result](const FScenarioReply& Reply) { Result = Reply; });
	TestFalse(TEXT("refused"), Result.bOk);
	TestEqual(TEXT("names the variables"), Result.Error.Message, OAuth->App.Problem);
	TestEqual(TEXT("nothing sent"), Transport->Requests.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioOAuthOtherAppTest, "Scenario.Core.Auth.OtherAppSession", SCENARIO_TEST_FLAGS)
bool FScenarioOAuthOtherAppTest::RunTest(const FString& Parameters)
{
	TSharedRef<FScenarioFakeTransport> Transport = MakeShared<FScenarioFakeTransport>();
	TSharedRef<FScenarioMemoryCredentialStore> Store = MakeShared<FScenarioMemoryCredentialStore>();
	FString Saved;

	// Saved by an older version, which registered its own client with the MCP's sign-in service.
	Store->Write(TEXT("oauth-refresh"), TEXT("rt_old"));
	Store->Write(TEXT("oauth-client"), TEXT("{\"client_id\":\"registered_per_install\",\"registered_at\":1}"));
	TSharedRef<FScenarioOAuth> OAuth = MakeShared<FScenarioOAuth>(Transport, Store);
	OAuth->App = TestApp();
	TestFalse(TEXT("not resumed"), OAuth->Restore());
	TestFalse(TEXT("no credentials"), OAuth->HasCredentials());
	TestFalse(TEXT("old token deleted"), Store->Read(TEXT("oauth-refresh"), Saved));
	TestFalse(TEXT("old client deleted"), Store->Read(TEXT("oauth-client"), Saved));

	// Same client id from another provider (a development instance).
	Store->Write(TEXT("oauth-refresh"), TEXT("rt_dev"));
	Store->Write(TEXT("oauth-client"), TEXT("{\"client_id\":\"client_test\",\"issuer\":\"https://dev.example.test\"}"));
	TestFalse(TEXT("other provider not resumed"), OAuth->Restore());
	TestFalse(TEXT("other provider token deleted"), Store->Read(TEXT("oauth-refresh"), Saved));
	TestEqual(TEXT("nothing sent"), Transport->Requests.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioOAuthDeviceTest, "Scenario.Core.Auth.DeviceCode", SCENARIO_TEST_FLAGS)
bool FScenarioOAuthDeviceTest::RunTest(const FString& Parameters)
{
	TSharedRef<FScenarioFakeTransport> Transport = MakeShared<FScenarioFakeTransport>();
	TSharedRef<FAuthServer> Server = MakeShared<FAuthServer>();
	Server->TokenReplies = {
		TEXT("400:{\"error\":\"authorization_pending\"}"),
		TEXT("400:{\"error\":\"slow_down\"}"),
		TokenReply(TEXT("at_1"), TEXT("rt_1"), IdToken(TEXT("user_1"), TEXT("person@example.test"))),
	};
	Transport->Responder = [Server](const FScenarioFakeRequest& Request) { return Server->Handle(Request); };
	TSharedRef<FScenarioMemoryCredentialStore> Store = MakeShared<FScenarioMemoryCredentialStore>();
	TSharedRef<FScenarioOAuth> OAuth = MakeShared<FScenarioOAuth>(Transport, Store);
	OAuth->App = TestApp();

	FScenarioDeviceCode Shown;
	FScenarioReply Result = FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("not finished"));
	OAuth->BeginDeviceSignIn([&Shown](const FScenarioDeviceCode& Code) { Shown = Code; }, [&Result](const FScenarioReply& Reply) { Result = Reply; });
	TestTrue(TEXT("signed in"), Result.bOk);
	TestEqual(TEXT("code shown"), Shown.UserCode, FString(TEXT("ABCD-EFGH")));
	TestEqual(TEXT("three polls"), Server->TokenCalls, 3);
	TestTrue(TEXT("interval respected"), Transport->Delays.Contains(5.0f));
	TestTrue(TEXT("slow_down adds 5 s"), Transport->Delays.Contains(10.0f));
	TestEqual(TEXT("bearer"), OAuth->AuthorizationHeader(), FString(TEXT("Bearer at_1")));
	TestEqual(TEXT("account email"), OAuth->GetAccountEmail(), FString(TEXT("person@example.test")));
	FString Saved;
	TestTrue(TEXT("refresh token stored"), Store->Read(TEXT("oauth-refresh"), Saved) && Saved == TEXT("rt_1"));
	TestTrue(TEXT("client stored"), Store->Read(TEXT("oauth-client"), Saved) && Saved.Contains(TEXT("client_test")) && Saved.Contains(TEXT("https://auth.example.test")));
	TestTrue(TEXT("account stored"), Saved.Contains(TEXT("user_1")) && Saved.Contains(TEXT("person@example.test")));
	TestTrue(TEXT("device grant used"), Server->TokenBodies.Num() > 0 && Server->TokenBodies[0].Contains(TEXT("device_code")));
	bool bRegistered = false;
	bool bResource = false;
	for (const FScenarioFakeRequest& Request : Transport->Requests)
	{
		bRegistered |= Request.Url.Contains(TEXT("register"));
		bResource |= Request.BodyText().Contains(TEXT("resource="));
		if (Request.Url.EndsWith(TEXT("/oauth/device")))
		{
			TestTrue(TEXT("device request names the app"), Request.BodyText().Contains(TEXT("client_id=client_test")));
			TestTrue(TEXT("device request scopes"), Request.BodyText().Contains(TEXT("scope=openid%20profile%20email%20offline_access")));
		}
	}
	TestFalse(TEXT("no client registration"), bRegistered);
	TestFalse(TEXT("no resource"), bResource);

	// Next session: restore, then refresh on demand.
	Server->TokenCalls = 0;
	Server->TokenReplies = { TEXT("200:{\"access_token\":\"at_2\",\"expires_in\":3600}") };
	TSharedRef<FScenarioOAuth> Next = MakeShared<FScenarioOAuth>(Transport, Store);
	Next->App = TestApp();
	TestTrue(TEXT("restored"), Next->Restore());
	TestTrue(TEXT("credentials without an access token"), Next->HasCredentials() && !Next->IsAccessTokenFresh());
	TestEqual(TEXT("email restored"), Next->GetAccountEmail(), FString(TEXT("person@example.test")));
	bool bRefreshed = false;
	Next->Refresh([&bRefreshed](bool bOk) { bRefreshed = bOk; });
	TestTrue(TEXT("refreshed"), bRefreshed);
	TestEqual(TEXT("new bearer"), Next->AuthorizationHeader(), FString(TEXT("Bearer at_2")));
	TestEqual(TEXT("email kept without an ID token"), Next->GetAccountEmail(), FString(TEXT("person@example.test")));
	TestTrue(TEXT("refresh grant"), Server->TokenBodies.Last().Contains(TEXT("grant_type=refresh_token")));
	TestTrue(TEXT("refresh names the app"), Server->TokenBodies.Last().Contains(TEXT("client_id=client_test")));

	// A rotated refresh token is saved even when its identity cannot be written.
	FString ClientJson;
	Store->Read(TEXT("oauth-client"), ClientJson);
	TSharedRef<FRefusingStore> Refusing = MakeShared<FRefusingStore>();
	Refusing->Write(TEXT("oauth-refresh"), TEXT("rt_1"));
	Refusing->Write(TEXT("oauth-client"), ClientJson);
	Refusing->RefusedAccount = TEXT("oauth-client");
	Server->TokenCalls = 0;
	Server->TokenReplies = { TEXT("200:{\"access_token\":\"at_3\",\"refresh_token\":\"rt_3\",\"expires_in\":3600}") };
	TSharedRef<FScenarioOAuth> Rotating = MakeShared<FScenarioOAuth>(Transport, Refusing);
	Rotating->App = TestApp();
	TestTrue(TEXT("restored before rotation"), Rotating->Restore());
	bool bRotated = false;
	Rotating->Refresh([&bRotated](bool bOk) { bRotated = bOk; });
	TestTrue(TEXT("rotated token saved without its identity"), bRotated && Refusing->Read(TEXT("oauth-refresh"), Saved) && Saved == TEXT("rt_3"));

	// Revoked session: invalid_grant clears the stored token.
	Server->TokenCalls = 0;
	Server->TokenReplies = { TEXT("400:{\"error\":\"invalid_grant\"}") };
	bool bSecond = true;
	Next->Refresh([&bSecond](bool bOk) { bSecond = bOk; });
	TestFalse(TEXT("refresh refused"), bSecond);
	TestFalse(TEXT("signed out"), Next->HasCredentials());
	TestTrue(TEXT("email forgotten"), Next->GetAccountEmail().IsEmpty());
	TestFalse(TEXT("stored token removed"), Store->Read(TEXT("oauth-refresh"), Saved) && !Saved.IsEmpty());
	TestFalse(TEXT("stored client removed"), Store->Read(TEXT("oauth-client"), Saved));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioOAuthSharedSessionTest, "Scenario.Core.Auth.SharedSession", SCENARIO_TEST_FLAGS)
bool FScenarioOAuthSharedSessionTest::RunTest(const FString& Parameters)
{
	// Two editors on one machine share the saved session; each refresh replaces the refresh token.
	TSharedRef<FScenarioFakeTransport> Transport = MakeShared<FScenarioFakeTransport>();
	TSharedRef<FAuthServer> Server = MakeShared<FAuthServer>();
	Transport->Responder = [Server](const FScenarioFakeRequest& Request) { return Server->Handle(Request); };
	TSharedRef<FScenarioMemoryCredentialStore> Store = MakeShared<FScenarioMemoryCredentialStore>();
	auto Editor = [&Transport, &Store]()
	{
		TSharedRef<FScenarioOAuth> OAuth = MakeShared<FScenarioOAuth>(Transport, Store);
		OAuth->App = TestApp();
		return OAuth;
	};
	auto RefreshOf = [](const TSharedRef<FScenarioOAuth>& OAuth)
	{
		bool bResult = false;
		OAuth->Refresh([&bResult](bool bOk) { bResult = bOk; });
		return bResult;
	};
	FString Saved;

	Server->TokenReplies = { TokenReply(TEXT("at_1"), TEXT("rt_1"), IdToken(TEXT("user_1"), TEXT("person@example.test"))) };
	TSharedRef<FScenarioOAuth> First = Editor();
	First->BeginDeviceSignIn([](const FScenarioDeviceCode&) {}, [](const FScenarioReply&) {});
	TSharedRef<FScenarioOAuth> Second = Editor();
	TestTrue(TEXT("second editor restored"), Second->Restore());

	// The first editor refreshes; the second then starts from the token it saved instead of its own, now replaced.
	Server->TokenCalls = 0;
	Server->TokenReplies = { TokenReply(TEXT("at_2"), TEXT("rt_2")), TokenReply(TEXT("at_3"), TEXT("rt_3")) };
	TestTrue(TEXT("first refreshed"), RefreshOf(First));
	TestTrue(TEXT("second refreshed"), RefreshOf(Second));
	TestTrue(TEXT("second sent the newest token"), Server->TokenBodies.Last().Contains(TEXT("refresh_token=rt_2")));
	TestTrue(TEXT("newest token saved"), Store->Read(TEXT("oauth-refresh"), Saved) && Saved == TEXT("rt_3"));

	// Both send the same token; the provider refuses the later one, which retries once with the token the other saved.
	Server->TokenCalls = 0;
	Server->TokenBodies.Reset();
	Server->TokenReplies = { TEXT("400:{\"error\":\"invalid_grant\"}"), TokenReply(TEXT("at_5"), TEXT("rt_5")) };
	Transport->Responder = [Server, Store](const FScenarioFakeRequest& Request)
	{
		if (Request.Url.EndsWith(TEXT("/oauth/token")) && Server->TokenCalls == 0)
		{
			Store->Write(TEXT("oauth-refresh"), TEXT("rt_4"));
		}
		return Server->Handle(Request);
	};
	TestTrue(TEXT("refused, then refreshed with the other's token"), RefreshOf(First));
	TestEqual(TEXT("two token calls"), Server->TokenCalls, 2);
	TestTrue(TEXT("retried with the saved token"), Server->TokenBodies.Num() == 2 && Server->TokenBodies[1].Contains(TEXT("refresh_token=rt_4")));
	TestEqual(TEXT("retried bearer"), First->AuthorizationHeader(), FString(TEXT("Bearer at_5")));
	Transport->Responder = [Server](const FScenarioFakeRequest& Request) { return Server->Handle(Request); };

	// Another account signed in from the other editor: its session is neither taken nor deleted.
	Store->Write(TEXT("oauth-refresh"), TEXT("rt_other"));
	Store->Write(TEXT("oauth-client"), TEXT("{\"client_id\":\"client_test\",\"issuer\":\"https://auth.example.test\",\"sub\":\"user_2\"}"));
	Server->TokenCalls = 0;
	Server->TokenBodies.Reset();
	Server->TokenReplies = { TEXT("400:{\"error\":\"invalid_grant\"}") };
	TestFalse(TEXT("own session refused"), RefreshOf(First));
	TestTrue(TEXT("own token sent"), Server->TokenBodies.Num() == 1 && Server->TokenBodies[0].Contains(TEXT("refresh_token=rt_5")));
	TestFalse(TEXT("signed out"), First->HasCredentials());
	TestTrue(TEXT("other account's session kept"), Store->Read(TEXT("oauth-refresh"), Saved) && Saved == TEXT("rt_other"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioOAuthDeclinedTest, "Scenario.Core.Auth.Declined", SCENARIO_TEST_FLAGS)
bool FScenarioOAuthDeclinedTest::RunTest(const FString& Parameters)
{
	TSharedRef<FScenarioFakeTransport> Transport = MakeShared<FScenarioFakeTransport>();
	TSharedRef<FAuthServer> Server = MakeShared<FAuthServer>();
	Server->TokenReplies = { TEXT("400:{\"error\":\"access_denied\"}") };
	Transport->Responder = [Server](const FScenarioFakeRequest& Request) { return Server->Handle(Request); };
	TSharedRef<FScenarioOAuth> OAuth = MakeShared<FScenarioOAuth>(Transport, MakeShared<FScenarioMemoryCredentialStore>());
	OAuth->App = TestApp();
	FScenarioReply Result = FScenarioReply::Ok();
	OAuth->BeginDeviceSignIn([](const FScenarioDeviceCode&) {}, [&Result](const FScenarioReply& Reply) { Result = Reply; });
	TestFalse(TEXT("declined"), Result.bOk);
	TestEqual(TEXT("message"), Result.Error.Message, FString(TEXT("Sign-in was declined.")));
	TestFalse(TEXT("no credentials"), OAuth->HasCredentials());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioCredentialFileTest, "Scenario.Core.Auth.CredentialFile", SCENARIO_TEST_FLAGS)
bool FScenarioCredentialFileTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("Scenario"), FGuid::NewGuid().ToString());
	const FString Path = FPaths::Combine(Folder, TEXT("credentials.json"));
	{
		FScenarioFileCredentialStore Store(Path);
		TestTrue(TEXT("write"), Store.Write(TEXT("api-secret"), TEXT("s3cr3t")));
		FString Read;
		TestTrue(TEXT("read"), Store.Read(TEXT("api-secret"), Read) && Read == TEXT("s3cr3t"));
		TestFalse(TEXT("missing account"), Store.Read(TEXT("oauth-refresh"), Read));
		TestFalse(TEXT("not an OS store"), Store.IsOsStore());
#if PLATFORM_MAC || PLATFORM_LINUX
		struct stat Info;
		if (stat(TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(Path)), &Info) == 0)
		{
			TestEqual(TEXT("owner-only file"), static_cast<int32>(Info.st_mode & 0777), 0600);
		}
		else
		{
			AddError(TEXT("credential file not found"));
		}
#endif
		TestTrue(TEXT("delete"), Store.Delete(TEXT("api-secret")));
		TestFalse(TEXT("deleted"), Store.Read(TEXT("api-secret"), Read));
	}
	{
		FScenarioMemoryCredentialStore Memory;
		TestEqual(TEXT("pair accepted"), FScenarioApiKeyCredentials::Save(Memory, TEXT("api_key123:secret456"), FString()), FString());
		const FScenarioApiKeyCredentials Loaded = FScenarioApiKeyCredentials::Load(Memory);
		if (FPlatformMisc::GetEnvironmentVariable(TEXT("SCENARIO_API_KEY")).IsEmpty())
		{
			TestEqual(TEXT("key"), Loaded.Key, FString(TEXT("api_key123")));
			TestEqual(TEXT("secret"), Loaded.Secret, FString(TEXT("secret456")));
			TestEqual(TEXT("source"), Loaded.Source, FString(TEXT("store")));
		}
		TestFalse(TEXT("empty refused"), FScenarioApiKeyCredentials::Save(Memory, FString(), FString()).IsEmpty());
		FScenarioApiKeyCredentials::Clear(Memory);
		FString Read;
		TestFalse(TEXT("cleared"), Memory.Read(TEXT("api-secret"), Read));
	}
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

#endif
