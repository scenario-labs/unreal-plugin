// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_SCENARIO_TOOLSETS

#include "ScenarioActions.h"
#include "ScenarioAgentSkill.h"
#include "ScenarioFakes.h"
#include "ScenarioJson.h"
#include "ScenarioLedger.h"
#include "ScenarioSession.h"
#include "ScenarioSubsystem.h"
#include "ScenarioToolset.h"
#include "ScenarioTypes.h"
#include "ToolsetRegistry/ToolCallAsyncResult.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	const TCHAR* ToolsetName = TEXT("ScenarioToolsets.ScenarioToolset");

	/** "ModelId" -> "model_id", "bIncludeLevels" -> "include_levels". */
	FString SnakeCase(const FString& Name)
	{
		FString Source = Name;
		if (Source.Len() > 1 && Source[0] == TEXT('b') && FChar::IsUpper(Source[1]))
		{
			Source.RightChopInline(1);
		}
		FString Out;
		for (int32 Index = 0; Index < Source.Len(); ++Index)
		{
			const TCHAR Char = Source[Index];
			if (FChar::IsUpper(Char) && Index > 0)
			{
				Out.AppendChar(TEXT('_'));
			}
			Out.AppendChar(FChar::ToLower(Char));
		}
		return Out;
	}

	TSharedPtr<FJsonObject> Parse(const UScenarioToolResult* Result)
	{
		return Result ? ScenarioJson::ParseObject(Result->Value) : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioToolsetsRegistrationTest, "Scenario.Toolsets.Registration", SCENARIO_TEST_FLAGS)
bool FScenarioToolsetsRegistrationTest::RunTest(const FString&)
{
	if (!TestTrue(TEXT("Epic's Toolset Registry is available in the editor"), UToolsetRegistry::IsAvailable()))
	{
		return false;
	}
	TestTrue(TEXT("The Scenario toolset is registered"), UToolsetRegistry::IsToolsetClassRegistered(UScenarioToolset::StaticClass()));
	TestTrue(TEXT("Registered under its module-qualified name"), UToolsetRegistry::IsToolsetRegistered(ToolsetName));

	const TSharedPtr<FJsonObject> Schema = ScenarioJson::ParseObject(UToolsetRegistry::GetToolsetJsonSchema(UScenarioToolset::StaticClass()));
	if (!TestTrue(TEXT("Schema parses"), Schema.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Toolset name"), ScenarioJson::GetString(Schema, TEXT("name")), FString(ToolsetName));
	const FString Description = ScenarioJson::GetString(Schema, TEXT("description"));
	TestTrue(TEXT("Description from the class comment names Creative Units"), Description.Contains(TEXT("Creative Units")));
	TestTrue(TEXT("Description names pending_approval"), Description.Contains(TEXT("pending_approval")));

	// The agent skill is found by Epic's AgentSkillToolset, which is not exported: go through the registry.
	const TStrongObjectPtr<UToolCallAsyncResult> Skills(UToolsetRegistry::ExecuteTool(TEXT("ToolsetRegistry.AgentSkillToolset"), TEXT("ListSkills"), TEXT("{}")));
	if (TestTrue(TEXT("ListSkills completes at once"), Skills.IsValid() && Skills->bIsComplete))
	{
		TestTrue(TEXT("ListSkills has no error"), Skills->Error.IsEmpty());
		TestTrue(TEXT("The Scenario skill is listed"), Skills->GetValueAsJsonString().Contains(TEXT("/Script/ScenarioToolsets.ScenarioAgentSkill")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioToolsetsSchemaTest, "Scenario.Toolsets.Schema", SCENARIO_TEST_FLAGS)
bool FScenarioToolsetsSchemaTest::RunTest(const FString&)
{
	const TSharedPtr<FJsonObject> Schema = ScenarioJson::ParseObject(UToolsetRegistry::GetToolsetJsonSchema(UScenarioToolset::StaticClass()));
	if (!TestTrue(TEXT("Schema parses"), Schema.IsValid()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>> Tools = ScenarioJson::GetArray(Schema, TEXT("tools"));
	const TMap<FString, FString>& Map = UScenarioToolset::ActionsByTool();
	TestEqual(TEXT("Tool count"), Tools.Num(), 19);
	TestEqual(TEXT("Every tool maps to one action"), Map.Num(), Tools.Num());

	// Only what the action itself needs is required; everything else can be left out.
	const TMap<FString, FString> Required = {
		{ TEXT("GetStatus"), TEXT("") },
		{ TEXT("ListModels"), TEXT("") },
		{ TEXT("GetModelSchema"), TEXT("modelId") },
		{ TEXT("RecommendModels"), TEXT("prompt") },
		{ TEXT("EstimateCost"), TEXT("modelId") },
		{ TEXT("Generate"), TEXT("quoteId,approvedCost") },
		{ TEXT("ListWorkflows"), TEXT("") },
		{ TEXT("GetWorkflowSchema"), TEXT("workflowId") },
		{ TEXT("EstimateWorkflow"), TEXT("workflowId") },
		{ TEXT("RunWorkflow"), TEXT("quoteId,approvedCost") },
		{ TEXT("GetJobStatus"), TEXT("jobId") },
		{ TEXT("WaitForJob"), TEXT("jobId") },
		{ TEXT("ListJobs"), TEXT("") },
		{ TEXT("CancelJob"), TEXT("jobId") },
		{ TEXT("CheckUnknownJob"), TEXT("") },
		{ TEXT("SearchAssets"), TEXT("") },
		{ TEXT("PrepareResult"), TEXT("jobId") },
		{ TEXT("ApplyResult"), TEXT("applicationId") },
		{ TEXT("SaveChanges"), TEXT("") },
	};

	int32 Paid = 0;
	for (const TSharedPtr<FJsonValue>& Value : Tools)
	{
		const TSharedPtr<FJsonObject> Tool = Value.IsValid() ? Value->AsObject() : nullptr;
		const FString FullName = ScenarioJson::GetString(Tool, TEXT("name"));
		FString Prefix, ToolName;
		if (!TestTrue(FString::Printf(TEXT("%s is ToolsetName.Tool"), *FullName), FullName.Split(TEXT("."), &Prefix, &ToolName, ESearchCase::CaseSensitive, ESearchDir::FromEnd)))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s prefix"), *ToolName), Prefix, FString(ToolsetName));
		const FString Description = ScenarioJson::GetString(Tool, TEXT("description"));
		TestFalse(FString::Printf(TEXT("%s has a description"), *ToolName), Description.IsEmpty());
		TestTrue(FString::Printf(TEXT("%s description is short"), *ToolName), Description.Len() < 700);

		const FString* ActionName = Map.Find(ToolName);
		if (!TestNotNull(FString::Printf(TEXT("%s maps to an action"), *ToolName), ActionName))
		{
			continue;
		}
		const FScenarioAction* Action = FScenarioActions::Get().Find(*ActionName);
		if (!TestNotNull(FString::Printf(TEXT("Action %s exists"), **ActionName), Action))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s runs no Python"), *ToolName), Action->Kind != EScenarioActionKind::Python);
		TestFalse(FString::Printf(TEXT("%s removes nothing"), *ToolName), Action->bDestructive);
		if (Action->bCosts)
		{
			++Paid;
			TestTrue(FString::Printf(TEXT("%s says it spends"), *ToolName), Description.Contains(TEXT("SPENDS")));
		}

		// Every parameter is one the action reads.
		const TSharedPtr<FJsonObject> Input = ScenarioJson::GetObject(Tool, TEXT("inputSchema"));
		const TSharedPtr<FJsonObject> Properties = ScenarioJson::GetObject(Input, TEXT("properties"));
		const TSharedPtr<FJsonObject> ActionProperties = ScenarioJson::GetObject(Action->InputSchema, TEXT("properties"));
		const TArray<FString> RequiredNow = ScenarioJson::GetStringArray(Input, TEXT("required"));
		if (Properties.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Property : Properties->Values)
			{
				const TSharedPtr<FJsonObject> PropertySchema = Property.Value.IsValid() ? Property.Value->AsObject() : nullptr;
				TestTrue(FString::Printf(TEXT("%s.%s is required or has a default"), *ToolName, *Property.Key),
					RequiredNow.Contains(Property.Key) || (PropertySchema.IsValid() && PropertySchema->HasField(TEXT("default"))));
			}
		}
		if (const FString* Expected = Required.Find(ToolName))
		{
			TestEqual(FString::Printf(TEXT("%s required parameters"), *ToolName), FString::Join(RequiredNow, TEXT(",")), *Expected);
		}
		if (Properties.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Property : Properties->Values)
			{
				const FString Snake = SnakeCase(Property.Key);
				TestTrue(FString::Printf(TEXT("%s.%s is %s in %s"), *ToolName, *Property.Key, *Snake, **ActionName), ActionProperties.IsValid() && ActionProperties->HasField(Snake));
			}
		}
	}
	TestEqual(TEXT("Paid tools: Generate and RunWorkflow"), Paid, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioToolsetsSpendGateTest, "Scenario.Toolsets.SpendGate", SCENARIO_TEST_FLAGS)
bool FScenarioToolsetsSpendGateTest::RunTest(const FString&)
{
	UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
	if (!TestNotNull(TEXT("Subsystem"), Subsystem))
	{
		return false;
	}
	FScenarioSession& Session = Subsystem->Session();
	FScenarioActions& Actions = FScenarioActions::Get();
	const int32 LedgerBefore = Session.Ledger().Rows().Num();

	TSharedRef<FJsonObject> Parameters = MakeShared<FJsonObject>();
	Parameters->SetStringField(TEXT("prompt"), TEXT("toolset spend gate test"));
	const FScenarioQuote Quote = Session.Quotes().Store(EScenarioQuoteKind::Model, TEXT("model_toolset_gate_test"), Parameters, TEXT("test"), 4.0, 0.0, ScenarioNow());
	const int32 QuotesBefore = Session.Quotes().Num();

	// Under the kill switch the gate refuses before asking anyone.
	if (Session.IsSpendingDisabled())
	{
		const UScenarioToolResult* Refused = UScenarioToolset::Generate(Quote.Id, Quote.Exact);
		TestTrue(TEXT("Refused at once"), Refused && Refused->bIsComplete);
		TestTrue(TEXT("Refused with the kill switch message"), Refused && Refused->Error.Contains(TEXT("SCENARIO_NO_SPEND")));
	}

	Actions.bTestAskEvenWhenSpendingIsOff = true;
	ON_SCOPE_EXIT
	{
		Actions.bTestAskEvenWhenSpendingIsOff = false;
	};

	// Wrong cost and unknown quotes never reach the person.
	const UScenarioToolResult* WrongCost = UScenarioToolset::Generate(Quote.Id, TEXT("0.01"));
	TestTrue(TEXT("Wrong approved cost is an error"), WrongCost && WrongCost->Error.Contains(TEXT("exact cost")));
	const UScenarioToolResult* Unknown = UScenarioToolset::Generate(TEXT("q_unknown"), Quote.Exact);
	TestTrue(TEXT("Unknown quote is an error"), Unknown && Unknown->Error.Contains(TEXT("unknown, used or expired")));
	const UScenarioToolResult* WrongKind = UScenarioToolset::RunWorkflow(Quote.Id, Quote.Exact);
	TestTrue(TEXT("A model quote does not run a workflow"), WrongKind && WrongKind->Error.Contains(TEXT("Generate")));

	const UScenarioToolResult* First = UScenarioToolset::Generate(Quote.Id, Quote.Exact);
	if (!TestTrue(TEXT("Generate answers at once"), First && First->bIsComplete))
	{
		return false;
	}
	FString ApprovalId;
	if (First->Error.IsEmpty())
	{
		UE_LOG(LogTemp, Display, TEXT("Scenario.Toolsets.SpendGate: the approval toast was shown; checked pending_approval."));
		const TSharedPtr<FJsonObject> Pending = Parse(First);
		TestEqual(TEXT("State"), ScenarioJson::GetString(Pending, TEXT("state")), FString(TEXT("pending_approval")));
		ApprovalId = ScenarioJson::GetString(Pending, TEXT("approval_id"));
		TestFalse(TEXT("Approval id"), ApprovalId.IsEmpty());
		TestEqual(TEXT("Cost shown is the quote's"), ScenarioJson::GetString(Pending, TEXT("cu_cost_exact")), Quote.Exact);
		TestFalse(TEXT("Texts use the toolset's tool names"), First->Value.Contains(TEXT("job_status")));

		const UScenarioToolResult* Again = UScenarioToolset::Generate(Quote.Id, Quote.Exact);
		TestEqual(TEXT("Asking again returns the same approval"), ScenarioJson::GetString(Parse(Again), TEXT("approval_id")), ApprovalId);

		const UScenarioToolResult* Status = UScenarioToolset::GetJobStatus(ApprovalId);
		TestTrue(TEXT("GetJobStatus follows the approval"), Status && Status->Error.IsEmpty() && Status->Value.Contains(ApprovalId));

		TestTrue(TEXT("Decline from code"), Actions.DeclineSpendApproval(ApprovalId));
		const UScenarioToolResult* After = UScenarioToolset::GetJobStatus(ApprovalId);
		TestEqual(TEXT("Declined"), ScenarioJson::GetString(Parse(After), TEXT("state")), FString(TEXT("declined")));
		TestFalse(TEXT("A declined approval cannot be declined again"), Actions.DeclineSpendApproval(ApprovalId));
	}
	else
	{
		// No UI to ask in: the gate declines, and nothing is sent either way.
		TestTrue(TEXT("Declined without a UI"), First->Error.Contains(TEXT("Declined")));
		UE_LOG(LogTemp, Display, TEXT("Scenario.Toolsets.SpendGate: no approval toast in this editor; checked the no-UI path."));
	}

	TestEqual(TEXT("Nothing was written to the job list"), Session.Ledger().Rows().Num(), LedgerBefore);
	TestEqual(TEXT("The quote was not consumed"), Session.Quotes().Num(), QuotesBefore);
	TestNotNull(TEXT("The quote is still usable"), Session.Quotes().FindById(Quote.Id, ScenarioNow()));

	FScenarioQuote Taken;
	Session.Quotes().ConsumeById(Quote.Id, ScenarioNow(), Taken);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioToolsetsReadsTest, "Scenario.Toolsets.Reads", SCENARIO_TEST_FLAGS)
bool FScenarioToolsetsReadsTest::RunTest(const FString&)
{
	const UScenarioToolResult* Status = UScenarioToolset::GetStatus();
	if (TestTrue(TEXT("GetStatus answers at once"), Status && Status->bIsComplete && Status->Error.IsEmpty()))
	{
		const TSharedPtr<FJsonObject> Json = Parse(Status);
		TestTrue(TEXT("Status has signed_in"), Json.IsValid() && Json->HasField(TEXT("signed_in")));
		TestTrue(TEXT("Every spend waits for the person"), ScenarioJson::GetBool(ScenarioJson::GetObject(Json, TEXT("permissions")), TEXT("person_approves_each_spend")));
		TestFalse(TEXT("No Python through the registry"), ScenarioJson::GetBool(ScenarioJson::GetObject(Json, TEXT("permissions")), TEXT("run_python"), true));
	}

	const UScenarioToolResult* Missing = UScenarioToolset::GetJobStatus(TEXT("job_toolset_missing"));
	TestTrue(TEXT("An unknown job is an error"), Missing && Missing->bIsComplete && !Missing->Error.IsEmpty());

	const UScenarioToolResult* Jobs = UScenarioToolset::ListJobs();
	TestTrue(TEXT("ListJobs works without sign-in"), Jobs && Jobs->bIsComplete && Jobs->Error.IsEmpty());

	const UScenarioToolResult* NoLocation = UScenarioToolset::PrepareResult(TEXT("job_x"), TEXT("place"), TEXT("at_location"));
	TestTrue(TEXT("at_location needs x,y,z"), NoLocation && NoLocation->Error.Contains(TEXT("x,y,z")));

	const UScenarioToolResult* BadJson = UScenarioToolset::EstimateCost(TEXT("model_x"), TEXT("auto"), TEXT("not json"));
	TestTrue(TEXT("Parameters must be a JSON object"), BadJson && BadJson->Error.Contains(TEXT("JSON object")));

	UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
	if (Subsystem && !Subsystem->Session().IsReady())
	{
		const UScenarioToolResult* Models = UScenarioToolset::ListModels();
		TestTrue(TEXT("ListModels explains sign-in when not ready"), Models && Models->bIsComplete && !Models->Error.IsEmpty());
	}

	// The same read through Epic's registry, as call_tool runs it.
	const TStrongObjectPtr<UToolCallAsyncResult> ViaRegistry(UToolsetRegistry::ExecuteTool(ToolsetName, TEXT("GetStatus"), TEXT("{}")));
	if (TestTrue(TEXT("Registry GetStatus completes"), ViaRegistry.IsValid() && ViaRegistry->bIsComplete))
	{
		TestTrue(TEXT("Registry GetStatus has no error"), ViaRegistry->Error.IsEmpty());
		TestTrue(TEXT("Registry GetStatus returns the status object"), ViaRegistry->GetValueAsJsonString().Contains(TEXT("plugin_version")));
	}

	// Left-out optional parameters take their defaults, and a paid call without a usable quote stops at the quote.
	const TStrongObjectPtr<UToolCallAsyncResult> Paid(UToolsetRegistry::ExecuteTool(ToolsetName, TEXT("Generate"), TEXT("{\"quoteId\":\"q_none\",\"approvedCost\":\"1\"}")));
	if (TestTrue(TEXT("Registry Generate completes"), Paid.IsValid() && Paid->bIsComplete))
	{
		// Under SCENARIO_NO_SPEND the gate refuses first; either way nothing is sent and no default is missing.
		TestTrue(FString::Printf(TEXT("Registry Generate stops before sending: %s"), *Paid->Error.Left(160)),
			Paid->Error.Contains(TEXT("unknown, used or expired")) || Paid->Error.Contains(TEXT("SCENARIO_NO_SPEND")));
		TestFalse(TEXT("Registry Generate needs no other parameter"), Paid->Error.Contains(TEXT("needs a default")));
	}
	const TStrongObjectPtr<UToolCallAsyncResult> Defaults(UToolsetRegistry::ExecuteTool(ToolsetName, TEXT("ListJobs"), TEXT("{}")));
	TestTrue(TEXT("Registry ListJobs runs on defaults"), Defaults.IsValid() && Defaults->bIsComplete && Defaults->Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioToolsetsSkillTest, "Scenario.Toolsets.Skill", SCENARIO_TEST_FLAGS)
bool FScenarioToolsetsSkillTest::RunTest(const FString&)
{
	const UScenarioAgentSkill* Skill = GetDefault<UScenarioAgentSkill>();
	TestFalse(TEXT("Description"), Skill->Description.IsEmpty());
	const FString Prompt = Skill->GetDetails().Instructions;
	const FTCHARToUTF8 Utf8(*Prompt);
	TestTrue(FString::Printf(TEXT("Under 3 KB (%d bytes)"), Utf8.Length()), Utf8.Length() < 3072);
	TestFalse(TEXT("State filled in"), Prompt.Contains(TEXT("{State}")));
	TestTrue(TEXT("Names pending_approval"), Prompt.Contains(TEXT("pending_approval")));
	TestTrue(TEXT("Says never to repeat a paid call"), Prompt.Contains(TEXT("Never repeat a paid call")));
	TestTrue(TEXT("Says to save"), Prompt.Contains(TEXT("SaveChanges")));
	int32 Dash = INDEX_NONE;
	TestFalse(TEXT("No long dash"), Prompt.FindChar(static_cast<TCHAR>(0x2014), Dash));
	return true;
}

#endif
