// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScenarioApi.h"
#include "ScenarioFakes.h"
#include "ScenarioJson.h"
#include "ScenarioLedger.h"
#include "ScenarioQuotes.h"

namespace ScenarioMoneyTests
{
	FString TempPath(const FString& Name)
	{
		return FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("Scenario"), FGuid::NewGuid().ToString() + TEXT("_") + Name);
	}

	FScenarioJobInfo ServerJob(const FString& JobId, const FString& ModelId, double CreatedAt, const FString& Status = TEXT("success"))
	{
		FScenarioJobInfo Job;
		Job.JobId = JobId;
		Job.ModelId = ModelId;
		Job.CreatedAt = CreatedAt;
		Job.RawStatus = Status;
		Job.Phase = FScenarioJobInfo::PhaseFromStatus(Status);
		return Job;
	}
}

using namespace ScenarioMoneyTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioQuotesTest, "Scenario.Core.Money.Quotes", SCENARIO_TEST_FLAGS)
bool FScenarioQuotesTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Params = ScenarioJson::ParseObject(TEXT("{\"prompt\":\"chest\",\"numSamples\":1}"));
	const TSharedPtr<FJsonObject> Reordered = ScenarioJson::ParseObject(TEXT("{\"numSamples\":1.0,\"prompt\":\"chest\"}"));
	const TSharedPtr<FJsonObject> Edited = ScenarioJson::ParseObject(TEXT("{\"prompt\":\"chest!\",\"numSamples\":1}"));
	const FString Scope = FScenarioQuotes::WorkspaceScope(TEXT("team_a"), TEXT("proj_b"));
	const FString Fingerprint = FScenarioQuotes::Fingerprint(TEXT("model_x"), Params, Scope);
	TestEqual(TEXT("key order does not matter"), Fingerprint, FScenarioQuotes::Fingerprint(TEXT("model_x"), Reordered, Scope));
	TestNotEqual(TEXT("an edit changes the fingerprint"), Fingerprint, FScenarioQuotes::Fingerprint(TEXT("model_x"), Edited, Scope));
	TestNotEqual(TEXT("another model"), Fingerprint, FScenarioQuotes::Fingerprint(TEXT("model_y"), Params, Scope));
	TestNotEqual(TEXT("another workspace"), Fingerprint, FScenarioQuotes::Fingerprint(TEXT("model_x"), Params, FScenarioQuotes::WorkspaceScope(TEXT("team_a"), TEXT("proj_c"))));
	TestEqual(TEXT("api key scope"), FScenarioQuotes::WorkspaceScope(FString(), FString()), FString(TEXT("key")));

	FScenarioQuotes Quotes;
	const double Now = 1000.0;
	const FScenarioQuote Stored = Quotes.Store(EScenarioQuoteKind::Model, TEXT("model_x"), Params, Scope, 11.0, 0.0, Now);
	TestTrue(TEXT("quote id"), Stored.Id.StartsWith(TEXT("q_")));
	TestEqual(TEXT("exact"), Stored.Exact, FString(TEXT("11")));
	TestNotNull(TEXT("usable while fresh"), Quotes.Usable(Fingerprint, Now + 60.0));
	TestNull(TEXT("expired after 120 s"), Quotes.Usable(Fingerprint, Now + 121.0));
	TestNotNull(TEXT("found by id"), Quotes.FindById(Stored.Id, Now + 1.0));

	FScenarioQuote Taken;
	TestTrue(TEXT("consume once"), Quotes.Consume(Fingerprint, Now + 2.0, Taken));
	TestFalse(TEXT("single use"), Quotes.Consume(Fingerprint, Now + 3.0, Taken));
	TestFalse(TEXT("gone by id too"), Quotes.ConsumeById(Stored.Id, Now + 3.0, Taken));

	const FScenarioQuote Late = Quotes.Store(EScenarioQuoteKind::Model, TEXT("model_x"), Params, Scope, 11.0, 0.0, Now);
	TestFalse(TEXT("an expired quote cannot be consumed"), Quotes.ConsumeById(Late.Id, Now + 500.0, Taken));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioLedgerIntentTest, "Scenario.Core.Money.LedgerIntent", SCENARIO_TEST_FLAGS)
bool FScenarioLedgerIntentTest::RunTest(const FString& Parameters)
{
	const FString Path = TempPath(TEXT("jobs.json"));
	{
		FScenarioLedger Ledger(Path);
		FScenarioJobRow Template;
		Template.ModelId = TEXT("model_x");
		Template.Fingerprint = TEXT("fp1");
		Template.Parameters = ScenarioJson::ParseObject(TEXT("{\"prompt\":\"a chest\"}"));
		const FString LocalId = Ledger.AddIntent(Template, 1000.0);
		TestTrue(TEXT("intent is written before the request"), FPaths::FileExists(Path));
		TestEqual(TEXT("prompt taken from parameters"), Ledger.Find(LocalId)->Prompt, FString(TEXT("a chest")));
		TestTrue(TEXT("an unsent intent blocks the same request"), Ledger.Blocks(TEXT("fp1")));
		// The editor stops here, before Scenario answers.
	}
	{
		FScenarioLedger Ledger(Path);
		Ledger.Load();
		TestEqual(TEXT("one row"), Ledger.Rows().Num(), 1);
		const FScenarioJobRow& Row = Ledger.Rows()[0];
		TestEqual(TEXT("submitting becomes unknown on load"), Row.State, EScenarioJobState::Unknown);
		TestTrue(TEXT("still blocks"), Ledger.Blocks(TEXT("fp1")));
		TestFalse(TEXT("another request is free"), Ledger.Blocks(TEXT("fp2")));

		Ledger.MarkNotSent(Row.LocalId, 1100.0);
		TestEqual(TEXT("marked not sent"), Ledger.Rows()[0].State, EScenarioJobState::Rejected);
		TestFalse(TEXT("unblocked"), Ledger.Blocks(TEXT("fp1")));
	}
	IFileManager::Get().Delete(*Path);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioLedgerLifecycleTest, "Scenario.Core.Money.LedgerLifecycle", SCENARIO_TEST_FLAGS)
bool FScenarioLedgerLifecycleTest::RunTest(const FString& Parameters)
{
	FScenarioLedger Ledger;
	FScenarioJobRow Template;
	Template.ModelId = TEXT("model_x");
	Template.Fingerprint = TEXT("fp");
	const FString LocalId = Ledger.AddIntent(Template, 1000.0);
	Ledger.MarkSubmitted(LocalId, TEXT("job_1"), TEXT("queued"), 1001.0);
	TestEqual(TEXT("queued"), Ledger.Find(LocalId)->State, EScenarioJobState::Queued);
	TestTrue(TEXT("active"), Ledger.Find(LocalId)->IsActive());
	TestFalse(TEXT("a sent request no longer blocks"), Ledger.Blocks(TEXT("fp")));

	const int32 Revision = Ledger.Find(LocalId)->Revision;
	FScenarioJobInfo Running = ServerJob(TEXT("job_1"), TEXT("model_x"), 1000.5, TEXT("in-progress"));
	Running.Progress = 0.4;
	TestNotNull(TEXT("applied"), Ledger.ApplyJob(Running, 1002.0));
	TestEqual(TEXT("running"), Ledger.Find(LocalId)->State, EScenarioJobState::Running);
	TestTrue(TEXT("revision moved"), Ledger.Find(LocalId)->Revision > Revision);
	const int32 Same = Ledger.Find(LocalId)->Revision;
	Ledger.ApplyJob(Running, 1003.0);
	TestEqual(TEXT("no change, no new revision"), Ledger.Find(LocalId)->Revision, Same);

	FScenarioJobInfo Done = ServerJob(TEXT("job_1"), TEXT("model_x"), 1000.5, TEXT("success"));
	Done.AssetIds = { TEXT("asset_a") };
	Done.ChargedCU = 11.0;
	Ledger.ApplyJob(Done, 1010.0);
	TestEqual(TEXT("success"), Ledger.Find(LocalId)->State, EScenarioJobState::Success);
	TestEqual(TEXT("charged"), Ledger.Find(LocalId)->ChargedCU, 11.0);
	TestEqual(TEXT("assets"), Ledger.Find(LocalId)->AssetIds.Num(), 1);

	TestEqual(TEXT("moderated failure is Blocked"), FScenarioLedger::StateForStatus(TEXT("failure"), TEXT("NSFW content detected")), EScenarioJobState::Blocked);
	TestEqual(TEXT("plain failure"), FScenarioLedger::StateForStatus(TEXT("failure"), TEXT("GPU out of memory")), EScenarioJobState::Failed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioLedgerReconcileTest, "Scenario.Core.Money.LedgerReconcile", SCENARIO_TEST_FLAGS)
bool FScenarioLedgerReconcileTest::RunTest(const FString& Parameters)
{
	FScenarioLedger Ledger;
	FScenarioJobRow Template;
	Template.ModelId = TEXT("model_x");
	Template.Fingerprint = TEXT("fp");
	const FString LocalId = Ledger.AddIntent(Template, 1000.0);
	Ledger.MarkUnknown(LocalId, TEXT("Timed out."), 1060.0);

	// Two candidates in the window: ambiguous, nothing linked.
	TArray<FScenarioJobInfo> Two = { ServerJob(TEXT("job_a"), TEXT("model_x"), 1002.0), ServerJob(TEXT("job_b"), TEXT("model_x"), 1010.0) };
	TestEqual(TEXT("two candidates link nothing"), Ledger.Reconcile(Two, 1100.0), 0);
	TestEqual(TEXT("still unknown"), Ledger.Find(LocalId)->State, EScenarioJobState::Unknown);

	// Outside the window or another model: ignored.
	TArray<FScenarioJobInfo> Far = { ServerJob(TEXT("job_c"), TEXT("model_x"), 1000.0 + 400.0), ServerJob(TEXT("job_d"), TEXT("model_y"), 1003.0), ServerJob(TEXT("job_e"), TEXT("model_x"), 990.0) };
	TestEqual(TEXT("nothing in the window"), Ledger.Reconcile(Far, 1100.0), 0);

	TArray<FScenarioJobInfo> One = { ServerJob(TEXT("job_f"), TEXT("model_x"), 1004.0, TEXT("success")), ServerJob(TEXT("job_g"), TEXT("model_y"), 1004.0) };
	TestEqual(TEXT("one match links"), Ledger.Reconcile(One, 1100.0), 1);
	TestEqual(TEXT("linked job"), Ledger.Find(LocalId)->JobId, FString(TEXT("job_f")));
	TestEqual(TEXT("state from Scenario"), Ledger.Find(LocalId)->State, EScenarioJobState::Success);
	TestFalse(TEXT("unblocked"), Ledger.Blocks(TEXT("fp")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioLedgerFileTest, "Scenario.Core.Money.LedgerFile", SCENARIO_TEST_FLAGS)
bool FScenarioLedgerFileTest::RunTest(const FString& Parameters)
{
	const FString Path = TempPath(TEXT("broken.json"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	FFileHelper::SaveStringToFile(TEXT("{ not json"), *Path);
	FScenarioLedger Ledger(Path);
	Ledger.Load();
	TestEqual(TEXT("empty after a bad file"), Ledger.Rows().Num(), 0);
	TestTrue(TEXT("bad file kept aside"), FPaths::FileExists(Path + TEXT(".unreadable")));

	FScenarioJobRow Template;
	Template.ModelId = TEXT("model_x");
	Template.Parameters = ScenarioJson::ParseObject(TEXT("{\"prompt\":\"round trip\",\"seed\":7}"));
	Template.QuotedCU = 2.75;
	Template.QuotedExact = TEXT("2.75");
	const FString LocalId = Ledger.AddIntent(Template, 1000.0);
	Ledger.MarkSubmitted(LocalId, TEXT("job_9"), TEXT("queued"), 1001.0);

	FScenarioLedger Again(Path);
	Again.Load();
	const FScenarioJobRow* Row = Again.FindByJob(TEXT("job_9"));
	TestNotNull(TEXT("round trip"), Row);
	if (Row)
	{
		TestEqual(TEXT("quote kept exactly"), Row->QuotedExact, FString(TEXT("2.75")));
		TestEqual(TEXT("parameters kept"), ScenarioJson::GetNumber(Row->Parameters, TEXT("seed")), 7.0);
		TestEqual(TEXT("state kept"), Row->State, EScenarioJobState::Queued);
	}
	IFileManager::Get().Delete(*Path);
	IFileManager::Get().Delete(*(Path + TEXT(".unreadable")));
	return true;
}

#endif
