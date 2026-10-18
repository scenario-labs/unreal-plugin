// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/Paths.h"
#include "ScenarioCredentialStore.h"
#include "ScenarioFakes.h"
#include "ScenarioJson.h"
#include "ScenarioSession.h"
#include "ScenarioTransfer.h"

namespace ScenarioSessionTests
{
	struct FRig
	{
		TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
		TSharedRef<FScenarioFakeTransport> Transport = MakeShared<FScenarioFakeTransport>();
		TSharedPtr<FScenarioSession> Session;
		double Time = 1000000.0;
		int32 JobCounter = 0;
		/** State of the job list when the paid call arrived, to prove the intent came first. */
		FString StateAtSubmit;

		FRig()
		{
			Backend->OnJson(TEXT("teams_list"), TEXT("{\"teams\":[{\"id\":\"team_1\",\"name\":\"Studio\",\"projects\":[{\"id\":\"proj_1\",\"name\":\"Cove\"}]}]}"));
			Backend->OnJson(TEXT("models_list"), TEXT("{\"models\":[]}"));
			Backend->OnJson(TEXT("usage"), TEXT("{\"totals\":{\"totalCU\":42.5}}"));
			Backend->OnJson(TEXT("jobs_list"), TEXT("{\"jobs\":[]}"));
			Backend->On(TEXT("model_run"), [this](const TSharedPtr<FJsonObject>& Arguments)
			{
				if (ScenarioJson::GetBool(Arguments, TEXT("dry_run")))
				{
					return FScenarioReply::Ok(ScenarioJson::ParseObject(TEXT("{\"creativeUnitsCost\":11,\"creativeUnitsDiscount\":0}")));
				}
				const TArray<FScenarioJobRow>& Rows = Session->Ledger().Rows();
				StateAtSubmit = Rows.Num() > 0 ? LexToString(Rows[0].State) : TEXT("none");
				return FScenarioReply::Ok(ScenarioJson::ParseObject(FString::Printf(TEXT("{\"job_id\":\"job_%d\",\"status\":\"queued\"}"), ++JobCounter)));
			});

			FScenarioSessionConfig Config;
			Config.StateDir = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("Scenario"), FGuid::NewGuid().ToString());
			Config.Backend = Backend;
			Config.Transport = Transport;
			Config.Store = MakeShared<FScenarioMemoryCredentialStore>();
			Config.bUseTicker = false;
			Config.bAutoDownload = false;
			// The fake backend never reaches Scenario, so SCENARIO_NO_SPEND from tools/test.sh does not apply.
			Config.SpendingDisabled = false;
			Session = MakeShared<FScenarioSession>(Config);
			Session->Clock = [this]() { return Time; };
		}

		FScenarioRunRequest Request(const FString& Prompt, const FString& Source = TEXT("person")) const
		{
			FScenarioRunRequest Run;
			Run.TargetId = TEXT("model_flare");
			Run.TargetName = TEXT("Flare");
			Run.Lane = TEXT("image");
			Run.Parameters = ScenarioJson::ParseObject(FString::Printf(TEXT("{\"prompt\":\"%s\",\"numOutputs\":1}"), *Prompt));
			Run.Source = Source;
			return Run;
		}

		int32 PaidCalls() const
		{
			int32 Count = 0;
			for (const TPair<FString, TSharedPtr<FJsonObject>>& Call : Backend->Calls)
			{
				Count += Call.Key == TEXT("model_run") && !ScenarioJson::GetBool(Call.Value, TEXT("dry_run")) ? 1 : 0;
			}
			return Count;
		}

		/** Price, synchronously (the fake backend answers inline). */
		FScenarioQuote Price(const FScenarioRunRequest& Run)
		{
			FScenarioQuote Quote;
			Session->Price(Run, false, [&Quote](const TScenarioResult<FScenarioQuote>& Result)
			{
				if (Result.bOk)
				{
					Quote = Result.Value;
				}
			});
			return Quote;
		}

		TScenarioResult<FString> Submit(const FScenarioRunRequest& Run, const FString& QuoteId = FString(), const FString& Approved = FString())
		{
			TScenarioResult<FString> Out;
			Session->Submit(Run, QuoteId, Approved, [&Out](const TScenarioResult<FString>& Result) { Out = Result; });
			return Out;
		}
	};
}

using namespace ScenarioSessionTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioSessionConnectTest, "Scenario.Core.Session.Connect", SCENARIO_TEST_FLAGS)
bool FScenarioSessionConnectTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Session->Start();
	TestTrue(TEXT("ready with the only project"), Rig.Session->IsReady());
	TestEqual(TEXT("project chosen"), Rig.Session->GetWorkspace().ProjectId, FString(TEXT("proj_1")));
	TestEqual(TEXT("label"), Rig.Session->GetWorkspace().Label(), FString(TEXT("Studio / Cove")));
	TestEqual(TEXT("usage this month"), Rig.Session->GetUsageThisMonth(), 42.5);
	TestEqual(TEXT("usage asked for the project"), ScenarioJson::GetString(Rig.Backend->Calls.FindByPredicate([](const TPair<FString, TSharedPtr<FJsonObject>>& Call) { return Call.Key == TEXT("usage"); })->Value, TEXT("project_id")), FString(TEXT("proj_1")));

	FRig Offline;
	Offline.Backend->bSignedIn = false;
	Offline.Session->Start();
	TestFalse(TEXT("signed out without credentials"), Offline.Session->IsSignedIn());
	FScenarioQuote Quote = Offline.Price(Offline.Request(TEXT("chest")));
	TestTrue(TEXT("no price while signed out"), Quote.Id.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioSessionMoneyTest, "Scenario.Core.Session.Money", SCENARIO_TEST_FLAGS)
bool FScenarioSessionMoneyTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Session->Start();

	// No quote, no spend.
	TScenarioResult<FString> Refused = Rig.Submit(Rig.Request(TEXT("chest")));
	TestFalse(TEXT("submit without a price is refused"), Refused.bOk);
	TestTrue(TEXT("refusal is not sent"), Refused.Error.bNotSent);
	TestEqual(TEXT("nothing paid"), Rig.PaidCalls(), 0);
	TestEqual(TEXT("no job row"), Rig.Session->Ledger().Rows().Num(), 0);

	// Price once, reuse while fresh.
	const FScenarioQuote Quote = Rig.Price(Rig.Request(TEXT("chest")));
	TestEqual(TEXT("price"), Quote.Exact, FString(TEXT("11")));
	Rig.Price(Rig.Request(TEXT("chest")));
	TestEqual(TEXT("a fresh quote is reused"), Rig.Backend->CountOf(TEXT("model_run")), 1);

	// An edit after pricing is refused.
	TestFalse(TEXT("edited request is refused"), Rig.Submit(Rig.Request(TEXT("chest!"))).bOk);
	TestEqual(TEXT("still nothing paid"), Rig.PaidCalls(), 0);

	// The priced request goes, with the intent written first.
	TScenarioResult<FString> Sent = Rig.Submit(Rig.Request(TEXT("chest")));
	TestTrue(TEXT("priced request is sent"), Sent.bOk);
	TestEqual(TEXT("intent written before the paid call"), Rig.StateAtSubmit, FString(TEXT("submitting")));
	const FScenarioJobRow* Row = Rig.Session->Ledger().Find(Sent.Value);
	if (TestNotNull(TEXT("job row"), Row))
	{
		TestEqual(TEXT("job id"), Row->JobId, FString(TEXT("job_1")));
		TestTrue(TEXT("queued"), Row->State == EScenarioJobState::Queued);
		TestEqual(TEXT("quoted exact"), Row->QuotedExact, FString(TEXT("11")));
		TestEqual(TEXT("workspace on the row"), Row->ProjectId, FString(TEXT("proj_1")));
		TestEqual(TEXT("prompt on the row"), Row->Prompt, FString(TEXT("chest")));
	}

	// Single use: the same quote cannot pay twice.
	TestFalse(TEXT("quote is single use"), Rig.Submit(Rig.Request(TEXT("chest"))).bOk);
	TestEqual(TEXT("one paid call"), Rig.PaidCalls(), 1);

	// Quotes expire after 120 s.
	Rig.Price(Rig.Request(TEXT("barrel")));
	Rig.Time += 121.0;
	TestFalse(TEXT("expired quote refused"), Rig.Submit(Rig.Request(TEXT("barrel"))).bOk);
	TestEqual(TEXT("still one paid call"), Rig.PaidCalls(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioSessionAgentTest, "Scenario.Core.Session.AgentQuote", SCENARIO_TEST_FLAGS)
bool FScenarioSessionAgentTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Session->Start();
	const FScenarioRunRequest Run = Rig.Request(TEXT("crate"), TEXT("agent"));
	const FScenarioQuote Quote = Rig.Price(Run);

	TestFalse(TEXT("agent without quote id"), Rig.Submit(Run).bOk);
	TestFalse(TEXT("agent without approved cost"), Rig.Submit(Run, Quote.Id).bOk);
	const TScenarioResult<FString> Wrong = Rig.Submit(Run, Quote.Id, TEXT("10"));
	TestFalse(TEXT("agent with another cost"), Wrong.bOk);
	TestTrue(TEXT("names the exact cost"), Wrong.Error.Message.Contains(TEXT("\"11\"")));
	TestFalse(TEXT("unknown quote id"), Rig.Submit(Run, TEXT("q_nope"), TEXT("11")).bOk);
	TestEqual(TEXT("nothing paid yet"), Rig.PaidCalls(), 0);

	// A quote for another request cannot pay for this one.
	FScenarioRunRequest Other = Rig.Request(TEXT("other crate"), TEXT("agent"));
	TestFalse(TEXT("quote bound to its request"), Rig.Submit(Other, Quote.Id, TEXT("11")).bOk);

	TestTrue(TEXT("exact quote and cost"), Rig.Submit(Run, Quote.Id, TEXT("11")).bOk);
	TestEqual(TEXT("one paid call"), Rig.PaidCalls(), 1);
	if (!TestEqual(TEXT("one row"), Rig.Session->Ledger().Rows().Num(), 1))
	{
		return false;
	}
	const FScenarioJobRow& Row = Rig.Session->Ledger().Rows()[0];
	TestEqual(TEXT("source recorded"), Row.Source, FString(TEXT("agent")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioSessionUnknownTest, "Scenario.Core.Session.Unknown", SCENARIO_TEST_FLAGS)
bool FScenarioSessionUnknownTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Session->Start();
	const double SubmitTime = Rig.Time;

	// The paid call times out: Scenario may have started the job.
	Rig.Backend->On(TEXT("model_run"), [](const TSharedPtr<FJsonObject>& Arguments)
	{
		if (ScenarioJson::GetBool(Arguments, TEXT("dry_run")))
		{
			return FScenarioReply::Ok(ScenarioJson::ParseObject(TEXT("{\"creativeUnitsCost\":11}")));
		}
		return FScenarioReply::Fail(EScenarioErrorKind::Timeout, TEXT("No reply from Scenario."));
	});
	Rig.Price(Rig.Request(TEXT("chest")));
	const TScenarioResult<FString> Sent = Rig.Submit(Rig.Request(TEXT("chest")));
	TestFalse(TEXT("timeout reported"), Sent.bOk);
	const FScenarioJobRow* Row = Rig.Session->Ledger().Find(Sent.Value);
	if (!TestNotNull(TEXT("row kept"), Row))
	{
		return false;
	}
	TestTrue(TEXT("unknown"), Row->State == EScenarioJobState::Unknown);

	// The identical request is blocked until resolved, even with a fresh price.
	Rig.Price(Rig.Request(TEXT("chest")));
	const TScenarioResult<FString> Again = Rig.Submit(Rig.Request(TEXT("chest")));
	TestFalse(TEXT("identical request blocked"), Again.bOk);
	TestTrue(TEXT("says why"), Again.Error.Message.Contains(TEXT("identical")));
	TestEqual(TEXT("one paid attempt"), Rig.PaidCalls(), 1);

	// Check Scenario: one job of the same model just after the intent is the match.
	Rig.Backend->OnJson(TEXT("jobs_list"), FString::Printf(
		TEXT("{\"jobs\":[{\"jobId\":\"job_found\",\"modelId\":\"model_flare\",\"status\":\"in-progress\",\"createdAt\":\"%s\"},")
		TEXT("{\"jobId\":\"job_old\",\"modelId\":\"model_flare\",\"status\":\"success\",\"createdAt\":\"%s\"}]}"),
		*ScenarioIsoTime(SubmitTime + 2.0), *ScenarioIsoTime(SubmitTime - 3600.0)));
	int32 Linked = -1;
	Rig.Session->CheckUnresolved([&Linked](int32 Count, const FScenarioError&) { Linked = Count; });
	TestEqual(TEXT("linked one"), Linked, 1);
	Row = Rig.Session->Ledger().Find(Sent.Value);
	TestEqual(TEXT("matched job"), Row->JobId, FString(TEXT("job_found")));
	TestTrue(TEXT("running"), Row->State == EScenarioJobState::Running);

	// Mark as not sent releases the block.
	Rig.Price(Rig.Request(TEXT("lamp")));
	Rig.Backend->On(TEXT("model_run"), [](const TSharedPtr<FJsonObject>& Arguments)
	{
		if (ScenarioJson::GetBool(Arguments, TEXT("dry_run")))
		{
			return FScenarioReply::Ok(ScenarioJson::ParseObject(TEXT("{\"creativeUnitsCost\":11}")));
		}
		FScenarioReply Reply = FScenarioReply::Fail(EScenarioErrorKind::Transport, TEXT("Connection reset."));
		return Reply;
	});
	const TScenarioResult<FString> Lost = Rig.Submit(Rig.Request(TEXT("lamp")));
	TestTrue(TEXT("unknown again"), Rig.Session->Ledger().Find(Lost.Value)->State == EScenarioJobState::Unknown);
	Rig.Session->MarkNotSent(Lost.Value);
	Rig.Price(Rig.Request(TEXT("lamp")));
	TestTrue(TEXT("no longer blocked"), Rig.Session->CanSubmit(Rig.Request(TEXT("lamp")), FString(), FString()).IsEmpty());

	// A clear refusal (moderation) is Rejected and does not block.
	Rig.Backend->On(TEXT("model_run"), [](const TSharedPtr<FJsonObject>& Arguments)
	{
		if (ScenarioJson::GetBool(Arguments, TEXT("dry_run")))
		{
			return FScenarioReply::Ok(ScenarioJson::ParseObject(TEXT("{\"creativeUnitsCost\":11}")));
		}
		return FScenarioReply::Fail(EScenarioErrorKind::Moderated, TEXT("Blocked by Scenario's content rules."));
	});
	Rig.Price(Rig.Request(TEXT("rock")));
	const TScenarioResult<FString> Blocked = Rig.Submit(Rig.Request(TEXT("rock")));
	TestTrue(TEXT("rejected"), Rig.Session->Ledger().Find(Blocked.Value)->State == EScenarioJobState::Rejected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioSessionPollTest, "Scenario.Core.Session.Poll", SCENARIO_TEST_FLAGS)
bool FScenarioSessionPollTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Session->Start();
	Rig.Price(Rig.Request(TEXT("chest")));
	const FString LocalId = Rig.Submit(Rig.Request(TEXT("chest"))).Value;

	int32 Polls = 0;
	FString Status = TEXT("in-progress");
	Rig.Backend->On(TEXT("job_get"), [&Polls, &Status](const TSharedPtr<FJsonObject>& Arguments)
	{
		++Polls;
		return FScenarioReply::Ok(ScenarioJson::ParseObject(FString::Printf(
			TEXT("{\"job\":{\"jobId\":\"%s\",\"status\":\"%s\",\"progress\":0.5,\"assetIds\":[\"asset_abc123\"],\"cuCost\":11}}"),
			*ScenarioJson::GetString(Arguments, TEXT("job_id")), *Status)));
	});

	Rig.Session->Poll(Rig.Time + 1.0);
	TestEqual(TEXT("not polled before the interval"), Polls, 0);
	Rig.Session->Poll(Rig.Time + 3.5);
	TestEqual(TEXT("polled once"), Polls, 1);
	TestTrue(TEXT("running"), Rig.Session->Ledger().Find(LocalId)->State == EScenarioJobState::Running);
	Rig.Session->Poll(Rig.Time + 4.0);
	TestEqual(TEXT("interval respected"), Polls, 1);

	Status = TEXT("success");
	Rig.Session->Poll(Rig.Time + 7.0);
	const FScenarioJobRow* Row = Rig.Session->Ledger().Find(LocalId);
	TestTrue(TEXT("success"), Row->State == EScenarioJobState::Success);
	TestEqual(TEXT("asset ids"), Row->AssetIds.Num(), 1);
	TestEqual(TEXT("charged"), Row->ChargedCU, 11.0);
	Rig.Session->Poll(Rig.Time + 20.0);
	TestEqual(TEXT("finished jobs are not polled"), Polls, 2);

	// A job Scenario no longer knows stops being polled.
	Rig.Price(Rig.Request(TEXT("barrel")));
	const FString Lost = Rig.Submit(Rig.Request(TEXT("barrel"))).Value;
	Rig.Backend->On(TEXT("job_get"), [](const TSharedPtr<FJsonObject>&) { return FScenarioReply::Fail(EScenarioErrorKind::NotFound, TEXT("Job not found.")); });
	Rig.Session->Poll(Rig.Time + 30.0);
	TestTrue(TEXT("missing job failed"), Rig.Session->Ledger().Find(Lost)->State == EScenarioJobState::Failed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioSessionWorkspaceTest, "Scenario.Core.Session.Workspace", SCENARIO_TEST_FLAGS)
bool FScenarioSessionWorkspaceTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Backend->OnJson(TEXT("teams_list"), TEXT("{\"teams\":[{\"id\":\"team_1\",\"name\":\"Studio\",\"projects\":[{\"id\":\"proj_1\",\"name\":\"Cove\"},{\"id\":\"proj_2\",\"name\":\"Reef\"}]}]}"));
	Rig.Session->Start();
	// The test backend signs in like an API key, scoped to its first project.
	TestTrue(TEXT("api key picks its project"), Rig.Session->IsReady());
	const FScenarioQuote Quote = Rig.Price(Rig.Request(TEXT("chest")));
	TestFalse(TEXT("quoted"), Quote.Id.IsEmpty());
	Rig.Session->SetWorkspace({ TEXT("team_1"), TEXT("Studio"), TEXT("proj_2"), TEXT("Reef") });
	TestNull(TEXT("a workspace change drops quotes"), Rig.Session->QuoteFor(Rig.Request(TEXT("chest"))));
	return true;
}

#endif
