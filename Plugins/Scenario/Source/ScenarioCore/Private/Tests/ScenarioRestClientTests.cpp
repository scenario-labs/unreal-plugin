// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ScenarioApi.h"
#include "ScenarioFakes.h"
#include "ScenarioJson.h"
#include "ScenarioRestClient.h"

namespace ScenarioRestTests
{
	/** Auth provider whose token changes on refresh. */
	class FTestAuth : public IScenarioAuthProvider
	{
	public:
		explicit FTestAuth(bool bInWorkspace, bool bInRefreshWorks = true)
			: bWorkspace(bInWorkspace)
			, bRefreshWorks(bInRefreshWorks)
		{
		}
		virtual bool HasCredentials() const override { return true; }
		virtual FString AuthorizationHeader() const override { return TEXT("Bearer ") + Token; }
		virtual bool NeedsWorkspaceArguments() const override { return bWorkspace; }
		virtual uint32 GetSignInGeneration() const override { return Generation; }
		virtual void Refresh(TFunction<void(bool)> OnDone) override
		{
			++Refreshes;
			if (bRefreshWorks)
			{
				Token = TEXT("fresh-token");
			}
			if (DuringRefresh)
			{
				DuringRefresh();
			}
			OnDone(bRefreshWorks);
		}
		bool bWorkspace = false;
		bool bRefreshWorks = true;
		int32 Refreshes = 0;
		uint32 Generation = 0;
		/** Runs while the token is renewed, before the request is resent. */
		TFunction<void()> DuringRefresh;
		FString Token = TEXT("old-token");
	};

	FScenarioHttpResponse Status(int32 Code, const FString& Body = TEXT("{}"), const TMap<FString, FString>& Headers = {})
	{
		return FScenarioFakeTransport::Json(Code, Body, Headers);
	}

	struct FRig
	{
		TSharedRef<FScenarioFakeTransport> Transport = MakeShared<FScenarioFakeTransport>();
		TSharedRef<FScenarioRestClient> Client = MakeShared<FScenarioRestClient>(Transport, TEXT("https://api.example.test/v1/"));
		TSharedRef<FTestAuth> Auth;

		explicit FRig(bool bWorkspace = false)
			: Auth(MakeShared<FTestAuth>(bWorkspace))
		{
			Client->SetAuth(Auth);
			Client->bSpendingDisabled = false;
		}

		FScenarioReply Send(const FScenarioRequest& Request)
		{
			FScenarioReply Out = FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("no callback"));
			bool bCalled = false;
			Client->Send(Request, [&Out, &bCalled](const FScenarioReply& Reply)
			{
				Out = Reply;
				bCalled = true;
			});
			check(bCalled);
			return Out;
		}

		static FScenarioRequest Paid()
		{
			return FScenarioRequest::Post(TEXT("/generate/custom/model_x"), ScenarioJson::ParseObject(TEXT("{\"prompt\":\"a chest\"}")), EScenarioCall::Paid);
		}
	};

	/** Runs an API call against the fake backend and returns its result. */
	template <typename T, typename FCall>
	TScenarioResult<T> Run(FCall&& Call)
	{
		TScenarioResult<T> Out = TScenarioResult<T>::Fail(EScenarioErrorKind::Local, TEXT("no callback"));
		Call([&Out](const TScenarioResult<T>& Result) { Out = Result; });
		return Out;
	}
}

using namespace ScenarioRestTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioRestRequestTest, "Scenario.Core.Rest.Request", SCENARIO_TEST_FLAGS)
bool FScenarioRestRequestTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Transport->EnqueueJson(200, TEXT("{\"model\":{\"id\":\"model_x\"}}"));
	const FScenarioReply Read = Rig.Send(FScenarioRequest::Get(TEXT("/models/model_x")).Param(TEXT("pageSize"), 5).Param(TEXT("empty"), FString()));
	TestTrue(TEXT("read ok"), Read.bOk && ScenarioJson::GetObject(Read.Value, TEXT("model")).IsValid());
	const FScenarioFakeRequest& Get = Rig.Transport->Requests.Last();
	TestEqual(TEXT("verb"), Get.Verb, FString(TEXT("GET")));
	TestEqual(TEXT("url under the API, trailing slash dropped"), Get.Url, FString(TEXT("https://api.example.test/v1/models/model_x?pageSize=5")));
	TestEqual(TEXT("bearer sent"), Get.Headers.FindRef(TEXT("Authorization")), FString(TEXT("Bearer old-token")));
	TestEqual(TEXT("JSON accepted"), Get.Headers.FindRef(TEXT("Accept")), FString(TEXT("application/json")));
	TestTrue(TEXT("user agent names the plugin"), Get.Headers.FindRef(TEXT("User-Agent")).StartsWith(TEXT("ScenarioUnreal/")));
	TestFalse(TEXT("no content type without a body"), Get.Headers.Contains(TEXT("Content-Type")));

	Rig.Transport->EnqueueJson(200, TEXT("{\"job\":{\"jobId\":\"job_1\",\"status\":\"queued\"}}"));
	TestTrue(TEXT("paid ok"), Rig.Send(FRig::Paid()).bOk);
	const FScenarioFakeRequest& Post = Rig.Transport->Requests.Last();
	TestEqual(TEXT("post"), Post.Verb, FString(TEXT("POST")));
	TestEqual(TEXT("JSON body"), ScenarioJson::GetString(Post.BodyJson(), TEXT("prompt")), FString(TEXT("a chest")));
	TestEqual(TEXT("content type"), Post.Headers.FindRef(TEXT("Content-Type")), FString(TEXT("application/json")));

	Rig.Transport->Enqueue(Status(204, FString()));
	TestTrue(TEXT("empty body is an empty object"), Rig.Send(FScenarioRequest::Get(TEXT("/x"))).bOk);
	TestEqual(TEXT("an id passes as it is"), FScenarioRequest::Segment(TEXT("model_abc-1")), FString(TEXT("model_abc-1")));
	TestFalse(TEXT("a slash never survives in a segment"), FScenarioRequest::Segment(TEXT("a/b")).Contains(TEXT("/")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioRestPaidNeverRetriedTest, "Scenario.Core.Rest.PaidNeverRetried", SCENARIO_TEST_FLAGS)
bool FScenarioRestPaidNeverRetriedTest::RunTest(const FString& Parameters)
{
	{
		FRig Rig;
		Rig.Transport->Responder = [](const FScenarioFakeRequest&) { return Status(503, TEXT("upstream error")); };
		const FScenarioReply Reply = Rig.Send(FRig::Paid());
		TestFalse(TEXT("503 fails"), Reply.bOk);
		TestTrue(TEXT("503 on a paid call is ambiguous"), Reply.Error.bAmbiguous);
		TestEqual(TEXT("sent once"), Rig.Transport->Requests.Num(), 1);
		TestEqual(TEXT("no back-off"), Rig.Transport->Delays.Num(), 0);
	}
	{
		FRig Rig;
		Rig.Transport->EnqueueTransportError(EScenarioErrorKind::Timeout);
		const FScenarioReply Reply = Rig.Send(FRig::Paid());
		TestTrue(TEXT("timeout on a paid call is ambiguous"), Reply.Error.bAmbiguous);
		TestEqual(TEXT("timeout kind"), Reply.Error.Kind, EScenarioErrorKind::Timeout);
		TestEqual(TEXT("timeout sent once"), Rig.Transport->Requests.Num(), 1);
	}
	{
		FRig Rig;
		Rig.Transport->Enqueue(Status(408, TEXT("{\"reason\":\"Request timeout\"}")));
		const FScenarioReply Reply = Rig.Send(FRig::Paid());
		TestTrue(TEXT("408 on a paid call is ambiguous"), Reply.Error.bAmbiguous);
		TestFalse(TEXT("408 may have started"), Reply.Error.bNotSent);
	}
	{
		FRig Rig;
		Rig.Transport->Responder = [](const FScenarioFakeRequest&) { return Status(429, TEXT("{\"reason\":\"Too many requests\"}"), { { TEXT("Retry-After"), TEXT("3") } }); };
		const FScenarioReply Reply = Rig.Send(FRig::Paid());
		TestEqual(TEXT("429 kind"), Reply.Error.Kind, EScenarioErrorKind::RateLimit);
		TestTrue(TEXT("429 was not sent"), Reply.Error.bNotSent);
		TestFalse(TEXT("429 is not ambiguous"), Reply.Error.bAmbiguous);
		TestEqual(TEXT("retry-after read"), Reply.Error.RetryAfterSeconds, 3.0);
		TestEqual(TEXT("429 paid call not retried"), Rig.Transport->Requests.Num(), 1);
	}
	{
		FRig Rig;
		Rig.Transport->Enqueue(Status(200, TEXT("<html>proxy</html>")));
		const FScenarioReply Reply = Rig.Send(FRig::Paid());
		TestEqual(TEXT("unreadable reply"), Reply.Error.Kind, EScenarioErrorKind::Protocol);
		TestTrue(TEXT("an unreadable success on a paid call is ambiguous"), Reply.Error.bAmbiguous);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioRestReadRetryTest, "Scenario.Core.Rest.ReadRetry", SCENARIO_TEST_FLAGS)
bool FScenarioRestReadRetryTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	int32 Calls = 0;
	Rig.Transport->Responder = [&Calls](const FScenarioFakeRequest&)
	{
		return ++Calls < 3 ? Status(503, TEXT("busy")) : Status(200, TEXT("{\"job\":{\"jobId\":\"job_1\",\"status\":\"success\"}}"));
	};
	TestTrue(TEXT("read succeeds on the third try"), Rig.Send(FScenarioRequest::Get(TEXT("/jobs/job_1"))).bOk);
	TestEqual(TEXT("three attempts"), Calls, 3);
	TestEqual(TEXT("two back-offs"), Rig.Transport->Delays.Num(), 2);

	FRig Long;
	Long.Transport->Responder = [](const FScenarioFakeRequest&) { return Status(429, TEXT("{\"reason\":\"slow\"}"), { { TEXT("Retry-After"), TEXT("120") } }); };
	TestFalse(TEXT("long Retry-After stops"), Long.Send(FScenarioRequest::Get(TEXT("/jobs/job_1"))).bOk);

	// The project changes during the back-off: the read is not retried in the new project.
	FRig Moved(true);
	Moved.Client->SetWorkspace(TEXT("team_1"), TEXT("proj_1"));
	TSharedRef<FScenarioRestClient> MovedClient = Moved.Client;
	Moved.Transport->Responder = [MovedClient](const FScenarioFakeRequest&)
	{
		MovedClient->SetWorkspace(TEXT("team_1"), TEXT("proj_2"));
		return Status(503, TEXT("busy"));
	};
	const FScenarioReply Stopped = Moved.Send(FScenarioRequest::Get(TEXT("/assets")));
	TestEqual(TEXT("stopped after a workspace change"), Stopped.Error.Kind, EScenarioErrorKind::Local);
	TestEqual(TEXT("no retry in the new project"), Moved.Transport->Requests.Num(), 1);
	TestEqual(TEXT("no retry past the cap"), Long.Transport->Requests.Num(), 1);

	FRig DryRun;
	int32 Prices = 0;
	DryRun.Transport->Responder = [&Prices](const FScenarioFakeRequest&)
	{
		// A dry run answers 269, not 200.
		return ++Prices == 1 ? Status(500, TEXT("oops")) : Status(269, TEXT("{\"creativeUnitsCost\":11,\"creativeUnitsDiscount\":0}"));
	};
	FScenarioRequest Price = FRig::Paid();
	Price.Call = EScenarioCall::DryRun;
	const FScenarioReply Priced = DryRun.Send(Price.Param(TEXT("dryRun"), TEXT("true")));
	TestTrue(TEXT("a price check is a read and is retried"), Priced.bOk);
	TestEqual(TEXT("269 carries the price"), ScenarioJson::GetNumber(Priced.Value, TEXT("creativeUnitsCost")), 11.0);
	TestEqual(TEXT("two price attempts"), Prices, 2);

	FRig Plain;
	Plain.Transport->Responder = [](const FScenarioFakeRequest&) { return Status(200, TEXT("{\"creativeUnitsCost\":11}")); };
	const FScenarioReply NotDry = Plain.Send(Price);
	TestEqual(TEXT("a price without 269 is not taken"), NotDry.Error.Kind, EScenarioErrorKind::Protocol);
	TestEqual(TEXT("and not retried"), Plain.Transport->Requests.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioRestEstimateAlarmTest, "Scenario.Core.Rest.EstimateAlarm", SCENARIO_TEST_FLAGS)
bool FScenarioRestEstimateAlarmTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Transport->Enqueue(Status(200, TEXT("{\"job\":{\"jobId\":\"job_surprise\",\"status\":\"queued\"}}")));
	FScenarioRequest Price = FRig::Paid();
	Price.Call = EScenarioCall::DryRun;
	const FScenarioReply Reply = Rig.Send(Price.Param(TEXT("dryRun"), TEXT("true")));
	TestFalse(TEXT("price check with a job fails"), Reply.bOk);
	TestEqual(TEXT("alarm kind"), Reply.Error.Kind, EScenarioErrorKind::EstimateJob);
	TestEqual(TEXT("job id surfaced"), Reply.Error.JobId, FString(TEXT("job_surprise")));
	TestTrue(TEXT("alarm raised for the session"), Rig.Client->IsEstimateAlarmRaised());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioRestRefreshTest, "Scenario.Core.Rest.Refresh", SCENARIO_TEST_FLAGS)
bool FScenarioRestRefreshTest::RunTest(const FString& Parameters)
{
	// The API's gateway refuses an expired token with 403 and AccessDeniedException before any handler runs.
	const TMap<FString, FString> Denied = { { TEXT("x-amzn-errortype"), TEXT("AccessDeniedException:http://internal.amazon.com/coral/com.amazon.coral.service/") } };
	FRig Rig(true);
	int32 Handled = 0;
	Rig.Transport->Responder = [&Handled, Denied](const FScenarioFakeRequest& Request)
	{
		if (Request.Headers.FindRef(TEXT("Authorization")) != TEXT("Bearer fresh-token"))
		{
			return Status(403, TEXT("{\"message\":\"User is not authorized to access this resource with an explicit deny\"}"), Denied);
		}
		++Handled;
		return Status(200, TEXT("{\"job\":{\"jobId\":\"job_2\",\"status\":\"queued\"}}"));
	};
	TestTrue(TEXT("refreshed then sent"), Rig.Send(FRig::Paid()).bOk);
	TestEqual(TEXT("one refresh"), Rig.Auth->Refreshes, 1);
	TestEqual(TEXT("paid call ran once"), Handled, 1);

	FRig Dead(true);
	Dead.Auth->bRefreshWorks = false;
	Dead.Transport->Responder = [](const FScenarioFakeRequest&) { return Status(401, TEXT("{\"message\":\"Unauthorized\"}")); };
	const FScenarioReply Reply = Dead.Send(FRig::Paid());
	TestEqual(TEXT("auth error"), Reply.Error.Kind, EScenarioErrorKind::Auth);
	TestTrue(TEXT("refused request was not sent"), Reply.Error.bNotSent);
	TestFalse(TEXT("refused request is not ambiguous"), Reply.Error.bAmbiguous);
	TestEqual(TEXT("one resend at most"), Dead.Transport->Requests.Num(), 1);

	// A real permission refusal has no gateway header: nothing to renew.
	FRig Forbidden(true);
	Forbidden.Transport->Responder = [](const FScenarioFakeRequest&) { return Status(403, TEXT("{\"reason\":\"Not allowed in this project\"}")); };
	const FScenarioReply Refused = Forbidden.Send(FScenarioRequest::Get(TEXT("/models")));
	TestEqual(TEXT("forbidden"), Refused.Error.Kind, EScenarioErrorKind::Forbidden);
	TestEqual(TEXT("no refresh for a real refusal"), Forbidden.Auth->Refreshes, 0);
	TestTrue(TEXT("the reason reaches the message"), Refused.Error.Message.Contains(TEXT("Not allowed in this project")));

	// The workspace changes while the token is renewed: the paid call is not resent to the new project.
	FRig Moved(true);
	Moved.Client->SetWorkspace(TEXT("team_1"), TEXT("proj_1"));
	TSharedRef<FScenarioRestClient> MovedClient = Moved.Client;
	Moved.Auth->DuringRefresh = [MovedClient]() { MovedClient->SetWorkspace(TEXT("team_1"), TEXT("proj_2")); };
	Moved.Transport->Responder = [Denied](const FScenarioFakeRequest& Request)
	{
		return Request.Headers.FindRef(TEXT("Authorization")) == TEXT("Bearer fresh-token")
			? Status(200, TEXT("{\"job\":{\"jobId\":\"job_3\",\"status\":\"queued\"}}"))
			: Status(403, TEXT("{}"), Denied);
	};
	const FScenarioReply Stopped = Moved.Send(FRig::Paid());
	TestTrue(TEXT("not resent after a workspace change"), !Stopped.bOk && Stopped.Error.bNotSent);
	TestEqual(TEXT("only the refused request went out"), Moved.Transport->Requests.Num(), 1);

	// Someone signs in again while the token is renewed: the paid call is not resent under that sign-in.
	FRig Switched(true);
	TSharedRef<FTestAuth> SwitchedAuth = Switched.Auth;
	Switched.Auth->DuringRefresh = [SwitchedAuth]() { ++SwitchedAuth->Generation; };
	Switched.Transport->Responder = [Denied](const FScenarioFakeRequest& Request)
	{
		return Request.Headers.FindRef(TEXT("Authorization")) == TEXT("Bearer fresh-token")
			? Status(200, TEXT("{\"job\":{\"jobId\":\"job_4\",\"status\":\"queued\"}}"))
			: Status(403, TEXT("{}"), Denied);
	};
	const FScenarioReply NotResent = Switched.Send(FRig::Paid());
	TestTrue(TEXT("not resent after another sign-in"), !NotResent.bOk && NotResent.Error.bNotSent);
	TestEqual(TEXT("only the first request went out"), Switched.Transport->Requests.Num(), 1);

	// The sign-in changed before the refusal came back: no renewal for it.
	FRig Replaced(true);
	TSharedRef<FScenarioRestClient> ReplacedClient = Replaced.Client;
	Replaced.Transport->Responder = [ReplacedClient](const FScenarioFakeRequest&)
	{
		ReplacedClient->SetAuth(MakeShared<FTestAuth>(true));
		return Status(401, TEXT("{\"message\":\"Unauthorized\"}"));
	};
	const FScenarioReply Unrenewed = Replaced.Send(FRig::Paid());
	TestEqual(TEXT("refusal returned as is"), Unrenewed.Error.Kind, EScenarioErrorKind::Auth);
	TestEqual(TEXT("the old sign-in is not renewed"), Replaced.Auth->Refreshes, 0);
	TestEqual(TEXT("nothing resent"), Replaced.Transport->Requests.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioRestNoSpendTest, "Scenario.Core.Rest.NoSpend", SCENARIO_TEST_FLAGS)
bool FScenarioRestNoSpendTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Client->bSpendingDisabled = true;
	Rig.Transport->Responder = [](const FScenarioFakeRequest& Request)
	{
		return Request.Path() == TEXT("/v1/jobs/job_1/action") ? Status(200) : Status(269, TEXT("{\"creativeUnitsCost\":3}"));
	};
	const FScenarioReply Paid = Rig.Send(FRig::Paid());
	TestTrue(TEXT("paid call refused locally"), !Paid.bOk && Paid.Error.bNotSent);
	TestEqual(TEXT("nothing sent for the paid call"), Rig.Transport->Requests.Num(), 0);
	FScenarioRequest Price = FRig::Paid();
	Price.Call = EScenarioCall::DryRun;
	const FScenarioReply Unmarked = Rig.Send(Price);
	TestTrue(TEXT("a price check without dryRun is refused"), !Unmarked.bOk && Unmarked.Error.bNotSent);
	TestEqual(TEXT("and nothing was sent"), Rig.Transport->Requests.Num(), 0);
	TestTrue(TEXT("price checks still work"), Rig.Send(Price.Param(TEXT("dryRun"), TEXT("true"))).bOk);
	TestTrue(TEXT("cancel still works"), Rig.Send(FScenarioRequest::Post(TEXT("/jobs/job_1/action"), nullptr, EScenarioCall::Cancel)).bOk);
	TestFalse(TEXT("uploads refused"), Rig.Send(FScenarioRequest::Post(TEXT("/uploads"), nullptr, EScenarioCall::Paid)).bOk);
	TestEqual(TEXT("two requests went out"), Rig.Transport->Requests.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioRestWorkspaceTest, "Scenario.Core.Rest.Workspace", SCENARIO_TEST_FLAGS)
bool FScenarioRestWorkspaceTest::RunTest(const FString& Parameters)
{
	FRig Rig(true);
	Rig.Client->SetWorkspace(TEXT("team_a"), TEXT("proj_b"));
	Rig.Transport->Responder = [](const FScenarioFakeRequest&) { return Status(200); };
	Rig.Send(FScenarioRequest::Get(TEXT("/models")));
	Rig.Send(FScenarioRequest::Get(TEXT("/usages")).WithScope(EScenarioScope::Team));
	Rig.Send(FScenarioRequest::Get(TEXT("/teams")).WithScope(EScenarioScope::None));
	Rig.Send(FScenarioRequest::Get(TEXT("/projects")).Param(TEXT("teamId"), TEXT("team_c")).WithScope(EScenarioScope::None));
	const TArray<FScenarioFakeRequest>& Sent = Rig.Transport->Requests;
	if (!TestEqual(TEXT("four requests"), Sent.Num(), 4))
	{
		return false;
	}
	TestEqual(TEXT("project scope: team"), Sent[0].Query(TEXT("teamId")), FString(TEXT("team_a")));
	TestEqual(TEXT("project scope: project"), Sent[0].Query(TEXT("projectId")), FString(TEXT("proj_b")));
	TestEqual(TEXT("team scope: team"), Sent[1].Query(TEXT("teamId")), FString(TEXT("team_a")));
	TestTrue(TEXT("team scope: no project"), Sent[1].Query(TEXT("projectId")).IsEmpty());
	TestTrue(TEXT("teams: no workspace"), Sent[2].Query(TEXT("teamId")).IsEmpty() && Sent[2].Query(TEXT("projectId")).IsEmpty());
	TestEqual(TEXT("the request's own team wins"), Sent[3].Query(TEXT("teamId")), FString(TEXT("team_c")));

	FRig Key(false);
	Key.Client->SetWorkspace(TEXT("team_a"), TEXT("proj_b"));
	Key.Transport->Responder = [](const FScenarioFakeRequest&) { return Status(200); };
	Key.Send(FScenarioRequest::Get(TEXT("/models")));
	TestEqual(TEXT("API keys send no workspace"), Key.Transport->Requests.Last().Url, FString(TEXT("https://api.example.test/v1/models")));
	TestTrue(TEXT("a key says so"), Key.Client->UsesApiKey());
	TestFalse(TEXT("a person does not"), Rig.Client->UsesApiKey());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioRestErrorsTest, "Scenario.Core.Rest.Errors", SCENARIO_TEST_FLAGS)
bool FScenarioRestErrorsTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Transport->Enqueue(Status(400, TEXT("{\"reason\":\"Prompt flagged by content moderation\",\"name\":\"BadRequestError\"}")));
	const FScenarioReply Moderated = Rig.Send(FRig::Paid());
	TestEqual(TEXT("moderation"), Moderated.Error.Kind, EScenarioErrorKind::Moderated);
	TestFalse(TEXT("a clear refusal is not ambiguous"), Moderated.Error.bAmbiguous);
	TestTrue(TEXT("a refusal was not sent"), Moderated.Error.bNotSent);

	Rig.Transport->Enqueue(Status(429, TEXT("{\"reason\":\"Your plan does not allow more generations\",\"name\":\"PlanLimitReachedError\",\"details\":{\"plan\":\"free\"}}")));
	const FScenarioReply Plan = Rig.Send(FScenarioRequest::Get(TEXT("/models")));
	TestEqual(TEXT("a plan limit is not a pace"), Plan.Error.Kind, EScenarioErrorKind::Quota);
	TestEqual(TEXT("a plan limit is not retried"), Rig.Transport->Delays.Num(), 0);

	Rig.Transport->Enqueue(Status(400, TEXT("{\"error\":\"Bad width\",\"apiError\":\"invalid\"}")));
	const FScenarioReply Invalid = Rig.Send(FRig::Paid());
	TestEqual(TEXT("invalid"), Invalid.Error.Kind, EScenarioErrorKind::Invalid);
	TestTrue(TEXT("the API's error text reaches the message"), Invalid.Error.Message.Contains(TEXT("Bad width")));

	Rig.Transport->Enqueue(Status(500, TEXT("see https://bucket.example.test/file?X-Amz-Signature=secret")));
	const FScenarioReply Scrubbed = Rig.Send(FRig::Paid());
	TestFalse(TEXT("signed queries are scrubbed"), Scrubbed.Error.Detail.Contains(TEXT("secret")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioApiEstimateTest, "Scenario.Core.Api.Estimate", SCENARIO_TEST_FLAGS)
bool FScenarioApiEstimateTest::RunTest(const FString& Parameters)
{
	FScenarioEstimate Estimate;
	TestTrue(TEXT("top level"), FScenarioApi::FindEstimate(ScenarioJson::ParseObject(TEXT("{\"creativeUnitsCost\":11}")), Estimate) && Estimate.CU == 11.0);
	TestEqual(TEXT("exact text"), Estimate.Exact, FString(TEXT("11")));
	TestTrue(TEXT("nested"), FScenarioApi::FindEstimate(ScenarioJson::ParseObject(TEXT("{\"estimate\":{\"cuCost\":2.75,\"creativeUnitsDiscount\":0.25}}")), Estimate) && Estimate.CU == 2.75);
	TestFalse(TEXT("no price"), FScenarioApi::FindEstimate(ScenarioJson::ParseObject(TEXT("{\"job\":{}}")), Estimate));

	TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
	Backend->OnJson(TEXT("POST /generate/custom/*"), TEXT("{\"creativeUnitsCost\":12,\"costDetails\":{\"custom-generation\":12}}"));
	Backend->OnJson(TEXT("PUT /workflows/*/run"), TEXT("{\"creativeUnitsCost\":40}"));
	Backend->OnJson(TEXT("POST /generate/prompt"), TEXT("{\"creativeUnitsCost\":1}"));
	FScenarioApi Api(Backend);
	const TScenarioResult<FScenarioEstimate> Model = Run<FScenarioEstimate>([&Api](auto Done) { Api.Estimate(TEXT("model_x"), ScenarioJson::ParseObject(TEXT("{\"prompt\":\"a chest\"}")), Done); });
	TestTrue(TEXT("api estimate"), Model.bOk && Model.Value.CU == 12.0);
	const FScenarioRequest* Sent = Backend->Last(TEXT("POST /generate/custom/*"));
	if (!TestNotNull(TEXT("model run priced"), Sent))
	{
		return false;
	}
	TestEqual(TEXT("dryRun=true, the literal the API reads"), Sent->QueryValue(TEXT("dryRun")), FString(TEXT("true")));
	TestEqual(TEXT("a price check"), Sent->Call, EScenarioCall::DryRun);
	TestEqual(TEXT("inputs are the body, unwrapped"), ScenarioJson::GetString(Sent->Body, TEXT("prompt")), FString(TEXT("a chest")));

	const TScenarioResult<FScenarioEstimate> Workflow = Run<FScenarioEstimate>([&Api](auto Done) { Api.EstimateWorkflow(TEXT("workflow_y"), nullptr, Done); });
	TestTrue(TEXT("workflow priced"), Workflow.bOk && Workflow.Value.CU == 40.0);
	TestEqual(TEXT("a workflow runs with PUT"), Backend->Calls.Last().Verb, FString(TEXT("PUT")));

	const TScenarioResult<FScenarioEstimate> Spark = Run<FScenarioEstimate>([&Api](auto Done) { Api.EstimatePromptSpark(TEXT("model_x"), TEXT("a chest"), { TEXT("asset_1") }, 9, Done); });
	TestTrue(TEXT("Prompt Spark priced"), Spark.bOk);
	const TSharedPtr<FJsonObject> SparkBody = Backend->Calls.Last().Body;
	TestEqual(TEXT("Prompt Spark v3"), ScenarioJson::GetString(SparkBody, TEXT("mode")), FString(TEXT("contextual-v2")));
	TestEqual(TEXT("target model"), ScenarioJson::GetString(SparkBody, TEXT("modelId")), FString(TEXT("model_x")));
	TestEqual(TEXT("results capped"), ScenarioJson::GetNumber(SparkBody, TEXT("numResults")), 5.0);
	TestEqual(TEXT("images"), ScenarioJson::GetStringArray(SparkBody, TEXT("images")).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioApiSubmitTest, "Scenario.Core.Api.Submit", SCENARIO_TEST_FLAGS)
bool FScenarioApiSubmitTest::RunTest(const FString& Parameters)
{
	TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
	Backend->OnJson(TEXT("POST /generate/custom/*"), TEXT("{\"job\":{\"jobId\":\"job_9\",\"status\":\"queued\"},\"creativeUnitsCost\":12}"));
	Backend->OnJson(TEXT("PUT /workflows/*/run"), TEXT("{\"workflow\":{\"id\":\"workflow_y\"},\"job\":{\"jobId\":\"job_10\",\"status\":\"pending\"}}"));
	FScenarioApi Api(Backend);
	const TScenarioResult<FScenarioSubmitted> Model = Run<FScenarioSubmitted>([&Api](auto Done) { Api.Submit(TEXT("model_x"), nullptr, Done); });
	TestTrue(TEXT("job id from job.jobId"), Model.bOk && Model.Value.JobId == TEXT("job_9"));
	TestEqual(TEXT("status"), Model.Value.Status, FString(TEXT("queued")));
	TestEqual(TEXT("paid"), Backend->Calls.Last().Call, EScenarioCall::Paid);
	TestTrue(TEXT("no dry run on the paid call"), Backend->Calls.Last().QueryValue(TEXT("dryRun")).IsEmpty());

	const TScenarioResult<FScenarioSubmitted> Workflow = Run<FScenarioSubmitted>([&Api](auto Done) { Api.SubmitWorkflow(TEXT("workflow_y"), nullptr, Done); });
	TestTrue(TEXT("workflow job"), Workflow.bOk && Workflow.Value.JobId == TEXT("job_10"));

	Backend->OnJson(TEXT("POST /generate/custom/*"), TEXT("{\"creativeUnitsCost\":12}"));
	const TScenarioResult<FScenarioSubmitted> NoJob = Run<FScenarioSubmitted>([&Api](auto Done) { Api.Submit(TEXT("model_x"), nullptr, Done); });
	TestTrue(TEXT("a success without a job is ambiguous"), !NoJob.bOk && NoJob.Error.bAmbiguous);

	Backend->OnJson(TEXT("POST /jobs/*/action"), TEXT("{\"job\":{\"jobId\":\"job_9\",\"status\":\"canceled\"}}"));
	bool bCancelled = false;
	Api.CancelJob(TEXT("job_9"), [&bCancelled](const FScenarioReply& Reply) { bCancelled = Reply.bOk; });
	TestTrue(TEXT("cancel"), bCancelled);
	TestEqual(TEXT("cancel action"), ScenarioJson::GetString(Backend->Calls.Last().Body, TEXT("action")), FString(TEXT("cancel")));
	TestEqual(TEXT("cancel lowers spend"), Backend->Calls.Last().Call, EScenarioCall::Cancel);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioApiJobTest, "Scenario.Core.Api.Job", SCENARIO_TEST_FLAGS)
bool FScenarioApiJobTest::RunTest(const FString& Parameters)
{
	// The REST job keeps outputs and errors under metadata; cuCost is the main action alone.
	const FScenarioJobInfo Done = FScenarioJobInfo::FromJson(ScenarioJson::ParseObject(TEXT("{\"jobId\":\"job_1\",\"jobType\":\"custom\",\"status\":\"success\",\"progress\":1,")
		TEXT("\"billing\":{\"cuCost\":11,\"cuCostDetails\":{\"quality-gate\":1.75}},")
		TEXT("\"metadata\":{\"assetIds\":[\"asset_a\",\"asset_b\"],\"input\":{\"modelId\":\"model_x\"},\"workflowId\":\"workflow_y\"}}")));
	TestEqual(TEXT("success"), Done.Phase, EScenarioJobPhase::Success);
	TestEqual(TEXT("outputs from metadata"), Done.AssetIds.Num(), 2);
	TestEqual(TEXT("model from the input"), Done.ModelId, FString(TEXT("model_x")));
	TestEqual(TEXT("workflow from metadata"), Done.WorkflowId, FString(TEXT("workflow_y")));
	TestEqual(TEXT("main action plus add-ons"), Done.ChargedCU, 12.75);

	const FScenarioJobInfo Failed = FScenarioJobInfo::FromJson(ScenarioJson::ParseObject(TEXT("{\"jobId\":\"job_2\",\"status\":\"failure\",\"billing\":{\"cuCost\":0},\"metadata\":{\"error\":\"Out of memory\",\"hint\":\"Lower the resolution\"}}")));
	TestEqual(TEXT("failure"), Failed.Phase, EScenarioJobPhase::Failure);
	TestEqual(TEXT("error from metadata"), Failed.Error, FString(TEXT("Out of memory")));
	TestEqual(TEXT("hint from metadata"), Failed.Hint, FString(TEXT("Lower the resolution")));
	TestEqual(TEXT("charged nothing"), Failed.ChargedCU, 0.0);
	TestEqual(TEXT("warming up is running"), FScenarioJobInfo::PhaseFromStatus(TEXT("warming-up")), EScenarioJobPhase::Running);

	TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
	Backend->OnJson(TEXT("GET /jobs/*"), TEXT("{\"job\":{\"jobId\":\"job_1\",\"status\":\"in-progress\",\"progress\":0.4}}"));
	Backend->OnJson(TEXT("GET /jobs"), TEXT("{\"jobs\":[{\"jobId\":\"job_1\",\"status\":\"queued\"}],\"nextPaginationToken\":\"t\"}"));
	FScenarioApi Api(Backend);
	const TScenarioResult<FScenarioJobInfo> One = Run<FScenarioJobInfo>([&Api](auto Done) { Api.GetJob(TEXT("job_1"), Done); });
	TestTrue(TEXT("job read"), One.bOk && One.Value.Phase == EScenarioJobPhase::Running && One.Value.Progress == 0.4);
	const TScenarioResult<TArray<FScenarioJobInfo>> Recent = Run<TArray<FScenarioJobInfo>>([&Api](auto Done) { Api.ListJobs(500, Done); });
	TestTrue(TEXT("jobs listed"), Recent.bOk && Recent.Value.Num() == 1);
	TestEqual(TEXT("page size capped at the API's 100"), Backend->Calls.Last().QueryValue(TEXT("pageSize")), FString(TEXT("100")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioApiCatalogTest, "Scenario.Core.Api.Catalog", SCENARIO_TEST_FLAGS)
bool FScenarioApiCatalogTest::RunTest(const FString& Parameters)
{
	TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
	Backend->OnJson(TEXT("GET /models"), TEXT("{\"models\":[")
		TEXT("{\"id\":\"model_img\",\"name\":\"Img\",\"capabilities\":[\"txt2img\"]},")
		TEXT("{\"id\":\"model_mesh\",\"name\":\"Mesh\",\"capabilities\":[\"img23d\"],\"complianceMetadata\":{\"modelProvider\":\"Meshy\"}}")
		TEXT("],\"nextPaginationToken\":\"next\"}"));
	Backend->OnJson(TEXT("GET /models/*"), TEXT("{\"model\":{\"id\":\"model_img\",\"name\":\"Img\",\"capabilities\":[\"txt2img\"],\"inputs\":[")
		TEXT("{\"name\":\"prompt\",\"type\":\"string\",\"required\":{\"always\":true},\"promptSpark\":true},")
		TEXT("{\"name\":\"numOutputs\",\"type\":\"number\",\"min\":1,\"max\":4,\"costImpact\":true},{\"type\":\"string\"}]}}"));
	FScenarioApi Api(Backend);
	const TScenarioResult<FScenarioModelPage> Meshes = Run<FScenarioModelPage>([&Api](auto Done) { Api.ListModels(TEXT("public"), TEXT("3d"), 900, FString(), Done); });
	if (!TestTrue(TEXT("3D only"), Meshes.bOk && Meshes.Value.Models.Num() == 1 && Meshes.Value.Models[0].Id == TEXT("model_mesh")))
	{
		return false;
	}
	TestEqual(TEXT("provider"), Meshes.Value.Models[0].Provider, FString(TEXT("Meshy")));
	TestEqual(TEXT("next page"), Meshes.Value.NextPageToken, FString(TEXT("next")));
	TestEqual(TEXT("page size capped at the API's 500"), Backend->Calls.Last().QueryValue(TEXT("pageSize")), FString(TEXT("500")));
	TestTrue(TEXT("public has no status filter"), Backend->Calls.Last().QueryValue(TEXT("status")).IsEmpty());
	Run<FScenarioModelPage>([&Api](auto Done) { Api.ListModels(TEXT("private"), FString(), 100, TEXT("tok"), Done); });
	TestEqual(TEXT("trained models only"), Backend->Calls.Last().QueryValue(TEXT("status")), FString(TEXT("trained")));
	TestEqual(TEXT("page token"), Backend->Calls.Last().QueryValue(TEXT("paginationToken")), FString(TEXT("tok")));
	TestTrue(TEXT("no modality matches all"), FScenarioApi::HasModality({ TEXT("txt2audio") }, FString()));
	TestFalse(TEXT("audio is not video"), FScenarioApi::HasModality({ TEXT("txt2audio") }, TEXT("video")));

	TSharedPtr<FJsonObject> Schema;
	Api.GetSchema(TEXT("model_img"), [&Schema](const FScenarioReply& Reply) { Schema = Reply.bOk ? Reply.Value : nullptr; });
	if (!TestTrue(TEXT("schema read"), Schema.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("model id"), ScenarioJson::GetString(Schema, TEXT("model_id")), FString(TEXT("model_img")));
	TestEqual(TEXT("inputs become parameters, unnamed ones dropped"), ScenarioJson::GetArray(Schema, TEXT("parameters")).Num(), 2);

	Backend->OnJson(TEXT("GET /models/model_lora"), TEXT("{\"model\":{\"id\":\"model_lora\",\"type\":\"flux.1-lora\",\"capabilities\":[\"txt2img\"]}}"));
	FScenarioReply Lora = FScenarioReply::Ok();
	Api.GetSchema(TEXT("model_lora"), [&Lora](const FScenarioReply& Reply) { Lora = Reply; });
	TestTrue(TEXT("a model without inputs is an explicit error, not an empty form"), !Lora.bOk && Lora.Error.Kind == EScenarioErrorKind::Local);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioApiListAssetsTest, "Scenario.Core.Api.ListAssets", SCENARIO_TEST_FLAGS)
bool FScenarioApiListAssetsTest::RunTest(const FString& Parameters)
{
	TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
	Backend->OnJson(TEXT("GET /assets"), TEXT("{\"assets\":[")
		TEXT("{\"id\":\"asset_mesh\",\"kind\":\"3d\",\"metadata\":{\"modelId\":\"model_x\",\"prompt\":\"asset_prompt_text\"},\"url\":\"https://cdn.example.test/m\"},")
		TEXT("{\"id\":\"asset_image\",\"kind\":\"image\",\"metadata\":{\"prompt\":\"a mossy arch\",\"type\":\"inference-txt2img\"},\"properties\":{\"size\":10,\"width\":1024,\"height\":768}}")
		TEXT("],\"nextPaginationToken\":\"next_1\"}"));
	Backend->OnJson(TEXT("POST /search/assets"), TEXT("{\"hits\":[{\"id\":\"asset_found\",\"mimeType\":\"model/gltf-binary\"}],\"limit\":20,\"offset\":0}"));
	Backend->OnJson(TEXT("POST /assets/*/download"), TEXT("{\"url\":\"https://cdn.example.test/m.glb?sig=1\",\"mimeType\":\"model/gltf-binary\"}"));
	FScenarioApi Api(Backend);
	const TScenarioResult<FScenarioAssetPage> Page = Run<FScenarioAssetPage>([&Api](auto Done) { Api.ListAssets(TEXT("3d"), 20, TEXT("page_0"), Done); });
	if (!TestTrue(TEXT("listed"), Page.bOk) || !TestEqual(TEXT("two assets"), Page.Value.Assets.Num(), 2))
	{
		return false;
	}
	const FScenarioRequest& Sent = *Backend->Last(TEXT("GET /assets"));
	TestEqual(TEXT("passes the page token"), Sent.QueryValue(TEXT("paginationToken")), FString(TEXT("page_0")));
	TestEqual(TEXT("filters 3D types"), Sent.QueryValue(TEXT("types")), FString(TEXT("uploaded-3d,img23d,txt23d,3d23d")));
	TestEqual(TEXT("newest first"), Sent.QueryValue(TEXT("sortDirection")), FString(TEXT("desc")));
	TestEqual(TEXT("next token"), Page.Value.NextPageToken, FString(TEXT("next_1")));
	TestEqual(TEXT("model id from metadata"), Page.Value.Assets[0].ModelId, FString(TEXT("model_x")));
	TestTrue(TEXT("a prompt kept as a text asset is not shown"), Page.Value.Assets[0].Prompt.IsEmpty());
	TestEqual(TEXT("prompt from metadata"), Page.Value.Assets[1].Prompt, FString(TEXT("a mossy arch")));
	TestEqual(TEXT("width from properties"), Page.Value.Assets[1].Width, 1024);

	const TScenarioResult<TArray<FScenarioAssetInfo>> Found = Run<TArray<FScenarioAssetInfo>>([&Api](auto Done) { Api.SearchAssets(FString(), TEXT("3d"), 20, 0, Done); });
	TestTrue(TEXT("search hits"), Found.bOk && Found.Value.Num() == 1 && Found.Value[0].Kind == TEXT("3d"));
	TestEqual(TEXT("kind filter"), ScenarioJson::GetString(Backend->Calls.Last().Body, TEXT("filter")), FString(TEXT("kind = \"3d\"")));
	TestEqual(TEXT("search is a read"), Backend->Calls.Last().Call, EScenarioCall::Read);

	const TScenarioResult<FString> Url = Run<FString>([&Api](auto Done) { Api.DownloadUrl(TEXT("asset_mesh"), TEXT("GLB"), Done); });
	TestTrue(TEXT("download link"), Url.bOk && Url.Value.StartsWith(TEXT("https://cdn.example.test/")));
	TestEqual(TEXT("format in lower case"), ScenarioJson::GetString(Backend->Calls.Last().Body, TEXT("targetFormat")), FString(TEXT("glb")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioApiWorkspaceTest, "Scenario.Core.Api.Workspace", SCENARIO_TEST_FLAGS)
bool FScenarioApiWorkspaceTest::RunTest(const FString& Parameters)
{
	TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
	Backend->OnJson(TEXT("GET /teams"), TEXT("{\"teams\":[{\"id\":\"team_1\",\"name\":\"Studio\",\"plan\":\"cu-pro\",\"projects\":[{\"id\":\"proj_1\",\"name\":\"Cove\"}]}]}"));
	int32 ProjectPages = 0;
	Backend->On(TEXT("GET /projects"), [&ProjectPages](const FScenarioRequest& Request)
	{
		// Pages are filtered after paging: an empty page can still have a next one.
		++ProjectPages;
		if (Request.QueryValue(TEXT("teamId")).IsEmpty())
		{
			return FScenarioReply::Ok(ScenarioJson::ParseObject(TEXT("{\"projects\":[{\"id\":\"proj_key\",\"name\":\"Key project\",\"teamId\":\"team_key\",\"teamName\":\"Key team\",\"keys\":[{\"id\":\"k\"}]}]}")));
		}
		if (Request.QueryValue(TEXT("paginationToken")).IsEmpty())
		{
			return FScenarioReply::Ok(ScenarioJson::ParseObject(TEXT("{\"projects\":[],\"nextPaginationToken\":\"p2\"}")));
		}
		return FScenarioReply::Ok(ScenarioJson::ParseObject(TEXT("{\"projects\":[{\"id\":\"proj_2\",\"name\":\"Two\"}]}")));
	});
	Backend->OnJson(TEXT("GET /usages"), TEXT("{\"consumption\":[{\"userId\":\"u1\",\"value\":40,\"discount\":2,\"total\":42},{\"userId\":\"u2\",\"value\":2.5}],\"usages\":[]}"));
	FScenarioApi Api(Backend);

	const TScenarioResult<TArray<FScenarioTeam>> Teams = Run<TArray<FScenarioTeam>>([&Api](auto Done) { Api.ListTeams(Done); });
	TestTrue(TEXT("teams with projects"), Teams.bOk && Teams.Value.Num() == 1 && Teams.Value[0].Projects.Num() == 1 && Teams.Value[0].Plan == TEXT("cu-pro"));
	const FScenarioRequest* TeamsSent = Backend->Last(TEXT("GET /teams"));
	if (!TestNotNull(TEXT("teams listed"), TeamsSent))
	{
		return false;
	}
	TestEqual(TEXT("teams carry no workspace"), TeamsSent->Scope, EScenarioScope::None);

	Backend->bApiKey = true;
	const TScenarioResult<TArray<FScenarioTeam>> KeyTeams = Run<TArray<FScenarioTeam>>([&Api](auto Done) { Api.ListTeams(Done); });
	TestTrue(TEXT("a key reads its own project"), KeyTeams.bOk && KeyTeams.Value.Num() == 1 && KeyTeams.Value[0].Id == TEXT("team_key")
		&& KeyTeams.Value[0].Projects.Num() == 1 && KeyTeams.Value[0].Projects[0].Id == TEXT("proj_key"));
	TestEqual(TEXT("a key never lists teams"), Backend->CountOf(TEXT("GET /teams")), 1);
	Backend->bApiKey = false;

	ProjectPages = 0;
	const TScenarioResult<TArray<FScenarioProject>> Projects = Run<TArray<FScenarioProject>>([&Api](auto Done) { Api.ListProjects(TEXT("team_1"), Done); });
	TestTrue(TEXT("projects past an empty page"), Projects.bOk && Projects.Value.Num() == 1 && Projects.Value[0].Id == TEXT("proj_2"));
	TestEqual(TEXT("two pages"), ProjectPages, 2);
	TestEqual(TEXT("projects name the team only"), Backend->Calls.Last().Scope, EScenarioScope::None);

	const TScenarioResult<double> Usage = Run<double>([&Api](auto Done) { Api.UsageTotal(TEXT("proj_1"), TEXT("2026-11-01T00:00:00Z"), TEXT("2026-11-20T00:00:00Z"), Done); });
	TestTrue(TEXT("usage summed after discounts"), Usage.bOk && Usage.Value == 42.5);
	const FScenarioRequest& Asked = Backend->Calls.Last();
	TestEqual(TEXT("usage for the project"), Asked.QueryValue(TEXT("projectIds")), FString(TEXT("proj_1")));
	TestEqual(TEXT("usage names the team only"), Asked.Scope, EScenarioScope::Team);
	TestEqual(TEXT("dates"), Asked.QueryValue(TEXT("startDate")), FString(TEXT("2026-11-01T00:00:00Z")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioApiUploadTest, "Scenario.Core.Api.Upload", SCENARIO_TEST_FLAGS)
bool FScenarioApiUploadTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("clean name"), FScenarioUploadTicket::CleanFileName(TEXT("My Crate (final) v2.PNG")), FString(TEXT("my-crate-final-v2.png")));
	TestEqual(TEXT("nothing usable"), FScenarioUploadTicket::CleanFileName(TEXT("(  ).glb")), FString(TEXT("file.glb")));

	TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
	Backend->OnJson(TEXT("POST /uploads"), TEXT("{\"upload\":{\"id\":\"upload_1\",\"status\":\"pending\",\"parts\":[")
		TEXT("{\"number\":2,\"url\":\"https://bucket.example.test/2\"},{\"number\":1,\"url\":\"https://bucket.example.test/1\"}]}}"));
	FScenarioApi Api(Backend);
	const TScenarioResult<FScenarioUploadTicket> Ticket = Run<FScenarioUploadTicket>([&Api](auto Done) { Api.UploadStart(TEXT("Big Mesh.glb"), TEXT("model/gltf-binary"), TEXT("3d"), 7000000, Done); });
	if (!TestTrue(TEXT("upload started"), Ticket.bOk))
	{
		return false;
	}
	TestEqual(TEXT("parts in order"), Ticket.Value.Parts[0].Key, 1);
	TestEqual(TEXT("6 MB parts"), Ticket.Value.PartSize, FScenarioUploadTicket::DefaultPartSize);
	const TSharedPtr<FJsonObject> Body = Backend->Calls.Last().Body;
	TestEqual(TEXT("two parts asked"), ScenarioJson::GetNumber(Body, TEXT("parts")), 2.0);
	TestEqual(TEXT("clean file name sent"), ScenarioJson::GetString(Body, TEXT("fileName")), FString(TEXT("big-mesh.glb")));
	TestEqual(TEXT("an upload is sent once"), Backend->Calls.Last().Call, EScenarioCall::Paid);

	// The complete races the upload's own state: a refusal saying so is harmless, the read decides.
	int32 Completes = 0;
	Backend->On(TEXT("POST /uploads/*/action"), [&Completes](const FScenarioRequest&)
	{
		++Completes;
		FScenarioReply Race = FScenarioReply::Fail(EScenarioErrorKind::Invalid, TEXT("Scenario refused the settings."), TEXT("Unable to complete upload with status: complete"));
		Race.Error.bNotSent = true;
		return Race;
	});
	int32 Reads = 0;
	Backend->On(TEXT("GET /uploads/*"), [&Reads](const FScenarioRequest&)
	{
		return FScenarioReply::Ok(ScenarioJson::ParseObject(++Reads == 1 ? TEXT("{\"upload\":{\"status\":\"complete\"}}") : TEXT("{\"upload\":{\"status\":\"imported\",\"entityId\":\"asset_new\"}}")));
	});
	const TScenarioResult<FString> Pending = Run<FString>([&Api](auto Done) { Api.UploadComplete(TEXT("upload_1"), Done); });
	TestEqual(TEXT("complete is not the end: still importing"), Pending.Error.Kind, EScenarioErrorKind::Timeout);
	const TScenarioResult<FString> Imported = Run<FString>([&Api](auto Done) { Api.UploadStatus(TEXT("upload_1"), Done); });
	TestTrue(TEXT("the asset once imported"), Imported.bOk && Imported.Value == TEXT("asset_new"));
	TestEqual(TEXT("the complete is sent once, the upload is read again"), Completes, 1);

	Backend->OnJson(TEXT("GET /uploads/*"), TEXT("{\"upload\":{\"status\":\"failed\",\"errorMessage\":\"Unsupported file\"}}"));
	const TScenarioResult<FString> Failed = Run<FString>([&Api](auto Done) { Api.UploadComplete(TEXT("upload_1"), Done); });
	TestTrue(TEXT("a failed import says why"), !Failed.bOk && Failed.Error.Message.Contains(TEXT("Unsupported file")));
	return true;
}

#endif
