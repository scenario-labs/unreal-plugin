// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "ScenarioCatalog.h"
#include "ScenarioCredentialStore.h"
#include "ScenarioFakes.h"
#include "ScenarioJson.h"
#include "ScenarioSchema.h"
#include "ScenarioSession.h"
#include "ScenarioTrained.h"

namespace ScenarioTrainedTests
{
	const TCHAR* const Lora = TEXT("model_test_style_lora");
	const TCHAR* const Composition = TEXT("model_test_scene_composition");
	const TCHAR* const Flux2Lora = TEXT("model_test_flux2_lora");
	const TCHAR* const Unknown = TEXT("model_test_unknown_lora");
	const TCHAR* const Flux1 = TEXT("model_bfl-flux-1-dev");
	const TCHAR* const Flux2 = TEXT("model_bfl-flux-2-dev");

	/** Recorded replies (Tests/Fixtures/trained_models.json), or null with a warning when the plugin ships without them. */
	TSharedPtr<FJsonObject> Fixture(FAutomationTestBase& Test)
	{
		const TSharedPtr<FJsonObject> Json = ScenarioFixtures::LoadJson(TEXT("trained_models.json"));
		if (!Json.IsValid())
		{
			Test.AddWarning(TEXT("Fixture trained_models.json not found; trained model checks skipped."));
		}
		return Json;
	}

	/** The "model" object of one recorded reply. */
	TSharedPtr<FJsonObject> Record(const TSharedPtr<FJsonObject>& Json, const TCHAR* Group, const FString& Id)
	{
		return ScenarioJson::GetObject(ScenarioJson::GetObject(ScenarioJson::GetObject(Json, Group), Id), TEXT("model"));
	}

	/** The two recorded bases, in the data's order (FLUX 2 first, as in Resources/Trained.json). */
	TArray<FScenarioTrainedBase> Bases(const TSharedPtr<FJsonObject>& Json)
	{
		TArray<FScenarioTrainedBase> Out;
		for (const TCHAR* Id : { Flux2, Flux1 })
		{
			FScenarioTrainedBase Base;
			if (FScenarioTrainedBase::FromModel(Record(Json, TEXT("bases"), Id), Base))
			{
				Out.Add(Base);
			}
		}
		return Out;
	}

	FScenarioTrainedData Data()
	{
		FScenarioTrainedData Out;
		Out.BaseProbes = { Flux2, Flux1 };
		Out.BaseNames.Add(TEXT("flux.1-dev"), Flux1);
		Out.BaseNames.Add(TEXT("flux.2-dev"), Flux2);
		Out.DefaultScale = 0.75;
		return Out;
	}

	FScenarioTrainedInfo Derive(const TSharedPtr<FJsonObject>& Json, const FString& Id)
	{
		return ScenarioTrained::Derive(FScenarioModelSummary::FromJson(Record(Json, TEXT("trained"), Id)), Bases(Json), Data());
	}

	/** A record made in the test: only what the trigger words rule reads. */
	TSharedPtr<FJsonObject> Samples(const FString& ParametersJson, const FString& Extra = FString())
	{
		return ScenarioJson::ParseObject(FString::Printf(TEXT("{\"id\":\"model_test_x\",\"type\":\"flux.1-lora\"%s,\"parameters\":%s}"), *Extra, *ParametersJson));
	}

	/** Answers GET /models/{id} with the recorded replies, and the model list with them (bases public, trained models private). */
	void Serve(FScenarioFakeBackend& Backend, const TSharedPtr<FJsonObject>& Json)
	{
		TArray<TSharedPtr<FJsonValue>> Public;
		TArray<TSharedPtr<FJsonValue>> Private;
		for (const TCHAR* Group : { TEXT("bases"), TEXT("trained") })
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ScenarioJson::Fields(ScenarioJson::GetObject(Json, Group)))
			{
				const TSharedPtr<FJsonObject> Reply = Pair.Value->AsObject();
				Backend.On(TEXT("GET /models/") + Pair.Key, [Reply](const FScenarioRequest&) { return FScenarioReply::Ok(Reply); });
				(FCString::Strcmp(Group, TEXT("bases")) == 0 ? Public : Private).Add(MakeShared<FJsonValueObject>(ScenarioJson::GetObject(Reply, TEXT("model"))));
			}
		}
		Backend.On(TEXT("GET /models"), [Public, Private](const FScenarioRequest& Request)
		{
			TSharedRef<FJsonObject> Page = MakeShared<FJsonObject>();
			Page->SetArrayField(TEXT("models"), Request.QueryValue(TEXT("privacy")) == TEXT("private") ? Private : Public);
			return FScenarioReply::Ok(Page);
		});
	}

	/** A signed-in session on a fake Scenario that prices every run at 6 CU. */
	struct FRig
	{
		TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
		TSharedPtr<FScenarioSession> Session;
		double Time = 1000000.0;
		int32 Jobs = 0;

		FRig(const TSharedPtr<FJsonObject>& Json, bool bSpendingDisabled = false)
		{
			Backend->OnJson(TEXT("GET /teams"), TEXT("{\"teams\":[{\"id\":\"team_test\",\"name\":\"Studio\",\"projects\":[{\"id\":\"proj_test\",\"name\":\"Cove\"}]}]}"));
			Backend->OnJson(TEXT("GET /usages"), TEXT("{\"consumption\":[]}"));
			Backend->OnJson(TEXT("GET /jobs"), TEXT("{\"jobs\":[]}"));
			Serve(*Backend, Json);
			Backend->On(TEXT("POST /generate/custom/*"), [this](const FScenarioRequest& Request)
			{
				if (Request.Call == EScenarioCall::DryRun)
				{
					return FScenarioReply::Ok(ScenarioJson::ParseObject(TEXT("{\"creativeUnitsCost\":6,\"creativeUnitsDiscount\":0}")));
				}
				return FScenarioReply::Ok(ScenarioJson::ParseObject(FString::Printf(TEXT("{\"job\":{\"jobId\":\"job_test_%d\",\"status\":\"queued\"}}"), ++Jobs)));
			});
			FScenarioSessionConfig Config;
			Config.StateDir = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("Scenario"), FGuid::NewGuid().ToString());
			Config.Backend = Backend;
			Config.Transport = MakeShared<FScenarioFakeTransport>();
			Config.Store = MakeShared<FScenarioMemoryCredentialStore>();
			Config.bUseTicker = false;
			Config.bAutoDownload = false;
			// The fake backend never reaches Scenario, so SCENARIO_NO_SPEND from tools/test.sh does not apply.
			Config.SpendingDisabled = bSpendingDisabled;
			Session = MakeShared<FScenarioSession>(Config);
			Session->Clock = [this]() { return Time; };
			Session->Start();
		}

		/** The form's payload for a model: what the Studio and estimate_cost send to the session. */
		static FScenarioRunRequest Run(const FString& ModelId, const FString& Parameters = TEXT("{\"prompt\":\"a red barn\"}"), const FString& Source = TEXT("person"))
		{
			FScenarioRunRequest Request;
			Request.TargetId = ModelId;
			Request.TargetName = ModelId;
			Request.Lane = TEXT("image");
			Request.Parameters = ScenarioJson::ParseObject(Parameters);
			Request.Source = Source;
			return Request;
		}

		TScenarioResult<FScenarioQuote> Price(const FScenarioRunRequest& Request)
		{
			TScenarioResult<FScenarioQuote> Out = TScenarioResult<FScenarioQuote>::Fail(EScenarioErrorKind::Local, TEXT("no answer"));
			Session->Price(Request, false, [&Out](const TScenarioResult<FScenarioQuote>& Result) { Out = Result; });
			return Out;
		}

		TScenarioResult<FString> Submit(const FScenarioRunRequest& Request, const FString& QuoteId = FString(), const FString& Approved = FString())
		{
			TScenarioResult<FString> Out = TScenarioResult<FString>::Fail(EScenarioErrorKind::Local, TEXT("no answer"));
			Session->Submit(Request, QuoteId, Approved, [&Out](const TScenarioResult<FString>& Result) { Out = Result; });
			return Out;
		}

		int32 Calls(EScenarioCall Call) const
		{
			int32 Count = 0;
			for (const FScenarioRequest& Seen : Backend->Calls)
			{
				Count += FScenarioFakeBackend::Matches(TEXT("POST /generate/custom/*"), Seen) && Seen.Call == Call ? 1 : 0;
			}
			return Count;
		}

		const FScenarioRequest* LastRun() const
		{
			return Backend->Last(TEXT("POST /generate/custom/*"));
		}
	};

	double FirstNumber(const TSharedPtr<FJsonObject>& Object, const FString& Field)
	{
		const TArray<TSharedPtr<FJsonValue>> Items = ScenarioJson::GetArray(Object, Field);
		return Items.Num() > 0 && Items[0].IsValid() && Items[0]->Type == EJson::Number ? Items[0]->AsNumber() : -1.0;
	}
}

using namespace ScenarioTrainedTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioTrainedDeriveTest, "Scenario.Core.Trained.Derive", SCENARIO_TEST_FLAGS)
bool FScenarioTrainedDeriveTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Json = Fixture(*this);
	if (!Json.IsValid())
	{
		return true;
	}
	const TArray<FScenarioTrainedBase> Read = Bases(Json);
	if (!TestEqual(TEXT("both recorded bases take trained models"), Read.Num(), 2))
	{
		return false;
	}
	const FScenarioTrainedBase& Flux1Base = Read[1];
	TestEqual(TEXT("base id"), Flux1Base.Id, FString(Flux1));
	TestTrue(TEXT("its loras input takes FLUX.1 LoRAs"), Flux1Base.LoraTypes.Contains(TEXT("flux.1-lora")));
	TestFalse(TEXT("but not compositions"), Flux1Base.LoraTypes.Contains(TEXT("flux.1-composition")));
	TestTrue(TEXT("its modelId input takes compositions"), Flux1Base.ModelIdTypes.Contains(TEXT("flux.1-composition")));
	TestEqual(TEXT("six LoRAs at most"), Flux1Base.MaxLoras, 6);
	TestEqual(TEXT("weight range max"), Flux1Base.ScaleMax, 2.0);
	TestEqual(TEXT("prompt input"), Flux1Base.PromptField, FString(TEXT("prompt")));
	FScenarioTrainedBase NotABase;
	TestFalse(TEXT("a model without loras or modelId is no base"), FScenarioTrainedBase::FromModel(ScenarioJson::ParseObject(TEXT("{\"id\":\"model_test_plain\",\"inputs\":[{\"name\":\"prompt\",\"type\":\"string\"}]}")), NotABase));

	// A LoRA: a loras input lists its type.
	const FScenarioTrainedInfo LoraInfo = Derive(Json, Lora);
	TestTrue(TEXT("LoRA kind"), LoraInfo.Kind == EScenarioTrainedKind::Lora);
	TestTrue(TEXT("LoRA runnable"), LoraInfo.IsRunnable());
	TestEqual(TEXT("LoRA runs on FLUX.1 Dev"), LoraInfo.BaseId(), FString(Flux1));
	TestEqual(TEXT("LoRA label"), LoraInfo.Label(), FString(TEXT("LoRA via FLUX.1 Dev")));
	TestEqual(TEXT("its own weight from concepts"), LoraInfo.Scale, 0.8);
	TestEqual(TEXT("weight range from the base"), LoraInfo.ScaleMax, 2.0);
	TestEqual(TEXT("trigger words from the sample prompts"), LoraInfo.TriggerWords, FString(TEXT("FAKESTYLE")));

	// A composition: only a modelId input lists its type.
	const FScenarioTrainedInfo CompositionInfo = Derive(Json, Composition);
	TestTrue(TEXT("composition kind"), CompositionInfo.Kind == EScenarioTrainedKind::Composition);
	TestEqual(TEXT("composition runs on FLUX.1 Dev"), CompositionInfo.BaseId(), FString(Flux1));
	TestEqual(TEXT("composition label"), CompositionInfo.Label(), FString(TEXT("Composition via FLUX.1 Dev")));
	TestEqual(TEXT("trigger words from conceptPrompt"), CompositionInfo.TriggerWords, FString(TEXT("test scene words")));

	// A FLUX 2 LoRA without a weight of its own.
	const FScenarioTrainedInfo Flux2Info = Derive(Json, Flux2Lora);
	TestEqual(TEXT("FLUX 2 LoRA runs on FLUX 2"), Flux2Info.BaseId(), FString(Flux2));
	TestEqual(TEXT("default weight"), Flux2Info.Scale, 0.75);
	TestTrue(TEXT("one sample prompt gives no trigger words"), Flux2Info.TriggerWords.IsEmpty());

	// An unknown type: no base lists it, nothing is guessed.
	const FScenarioTrainedInfo UnknownInfo = Derive(Json, Unknown);
	TestTrue(TEXT("unknown type has no kind"), UnknownInfo.Kind == EScenarioTrainedKind::None);
	TestFalse(TEXT("unknown type cannot run"), UnknownInfo.IsRunnable());
	TestEqual(TEXT("no base guessed"), UnknownInfo.BaseIds.Num(), 0);
	TestEqual(TEXT("unknown label"), UnknownInfo.Label(), FString(TEXT("No base model for sd-xl-lora")));
	TestTrue(TEXT("unknown type says why"), UnknownInfo.Problem().Contains(TEXT("sd-xl-lora")));
	TestTrue(TEXT("a runnable model has no problem"), LoraInfo.Problem().IsEmpty());

	// Several bases take the type: compliantModelIds orders them through the name map, else the probe order.
	auto TestBase = [](const TCHAR* Id, const TCHAR* Name)
	{
		FScenarioTrainedBase Base;
		FScenarioTrainedBase::FromModel(ScenarioJson::ParseObject(FString::Printf(
			TEXT("{\"id\":\"%s\",\"name\":\"%s\",\"inputs\":[{\"name\":\"loras\",\"modelTypes\":[\"test-lora\"]},{\"name\":\"lorasScale\",\"min\":0,\"max\":1.5}]}"), Id, Name)), Base);
		return Base;
	};
	const TArray<FScenarioTrainedBase> Two = { TestBase(TEXT("model_test_base_a"), TEXT("Base A")), TestBase(TEXT("model_test_base_b"), TEXT("Base B")) };
	FScenarioTrainedData Order;
	Order.BaseProbes = { TEXT("model_test_base_a"), TEXT("model_test_base_b") };
	Order.BaseNames.Add(TEXT("base-b"), TEXT("model_test_base_b"));
	FScenarioModelSummary Plain = FScenarioModelSummary::FromJson(ScenarioJson::ParseObject(TEXT("{\"id\":\"model_test_y\",\"type\":\"test-lora\"}")));
	TestEqual(TEXT("probe order without compliantModelIds"), ScenarioTrained::Derive(Plain, Two, Order).BaseId(), FString(TEXT("model_test_base_a")));
	FScenarioModelSummary Named = FScenarioModelSummary::FromJson(ScenarioJson::ParseObject(TEXT("{\"id\":\"model_test_y\",\"type\":\"test-lora\",\"compliantModelIds\":[\"base-b\",\"base-unknown\"]}")));
	const FScenarioTrainedInfo Ordered = ScenarioTrained::Derive(Named, Two, Order);
	TestEqual(TEXT("compliantModelIds puts its base first"), Ordered.BaseId(), FString(TEXT("model_test_base_b")));
	TestEqual(TEXT("both candidates kept"), Ordered.BaseIds.Num(), 2);
	FScenarioModelSummary Heavy = FScenarioModelSummary::FromJson(ScenarioJson::ParseObject(TEXT("{\"id\":\"model_test_y\",\"type\":\"test-lora\",\"concepts\":[{\"modelId\":\"model_test_y\",\"scale\":3}]}")));
	TestEqual(TEXT("own weight kept within the base's range"), ScenarioTrained::Derive(Heavy, Two, Order).Scale, 1.5);

	TestFalse(TEXT("custom is no trained type"), ScenarioTrained::IsTrainedType(TEXT("custom")));
	TestFalse(TEXT("no type is no trained type"), ScenarioTrained::IsTrainedType(FString()));
	TestTrue(TEXT("a LoRA type is"), ScenarioTrained::IsTrainedType(TEXT("flux.1-lora")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioTrainedTriggerTest, "Scenario.Core.Trained.TriggerWords", SCENARIO_TEST_FLAGS)
bool FScenarioTrainedTriggerTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioTrained;
	TestEqual(TEXT("conceptPrompt first, spaces collapsed"), TriggerWords(Samples(TEXT("{\"conceptPrompt\":\"  ink   wash \",\"samplePrompts\":[\"a FAKESTYLE barn\",\"FAKESTYLE tower\"]}"))), FString(TEXT("ink wash")));
	TestEqual(TEXT("then promptEmbedding"), TriggerWords(Samples(TEXT("{\"conceptPrompt\":\"\"}"), TEXT(",\"promptEmbedding\":\"embedded words\""))), FString(TEXT("embedded words")));
	TestEqual(TEXT("then a capitalized word every sample holds"), TriggerWords(Samples(TEXT("{\"samplePrompts\":[\"a red barn, FAKESTYLE\",\"FAKESTYLE castle\",\"knight in FAKESTYLE\"]}"))), FString(TEXT("FAKESTYLE")));
	TestEqual(TEXT("a word missing from one sample is skipped"), TriggerWords(Samples(TEXT("{\"samplePrompts\":[\"CASTLE drawn in FAKESTYLE\",\"a FAKESTYLE barn\"]}"))), FString(TEXT("FAKESTYLE")));
	TestEqual(TEXT("digits and underscores count"), TriggerWords(Samples(TEXT("{\"samplePrompts\":[\"STYLE_2B barn\",\"tower, STYLE_2B\"]}"))), FString(TEXT("STYLE_2B")));
	TestTrue(TEXT("one sample is not enough"), TriggerWords(Samples(TEXT("{\"samplePrompts\":[\"a FAKESTYLE barn\"]}"))).IsEmpty());
	TestTrue(TEXT("mixed case is not a trigger"), TriggerWords(Samples(TEXT("{\"samplePrompts\":[\"a Fakestyle barn\",\"Fakestyle tower\"]}"))).IsEmpty());
	TestTrue(TEXT("three letters are not enough"), TriggerWords(Samples(TEXT("{\"samplePrompts\":[\"an ABC barn\",\"ABC tower\"]}"))).IsEmpty());
	TestTrue(TEXT("whole words only"), TriggerWords(Samples(TEXT("{\"samplePrompts\":[\"FAKESTYLE barn\",\"FAKESTYLES tower\"]}"))).IsEmpty());
	TestTrue(TEXT("no word in every sample"), TriggerWords(Samples(TEXT("{\"samplePrompts\":[\"a CASTLE\",\"a TOWER\"]}"))).IsEmpty());
	TestEqual(TEXT("a long conceptPrompt is a description, not trigger words"),
		TriggerWords(Samples(TEXT("{\"conceptPrompt\":\"a very long description of the training subject that keeps going on\",\"samplePrompts\":[\"FAKESTYLE a\",\"FAKESTYLE b\"]}"))), FString(TEXT("FAKESTYLE")));
	TestTrue(TEXT("no parameters, no trigger words"), TriggerWords(ScenarioJson::ParseObject(TEXT("{\"id\":\"model_test_x\"}"))).IsEmpty());

	TestEqual(TEXT("put in front"), WithTriggerWords(TEXT("a red barn"), TEXT("FAKESTYLE")), FString(TEXT("FAKESTYLE, a red barn")));
	TestEqual(TEXT("once: already in the prompt, any case"), WithTriggerWords(TEXT("a fakestyle barn"), TEXT("FAKESTYLE")), FString(TEXT("a fakestyle barn")));
	TestEqual(TEXT("at either end"), WithTriggerWords(TEXT("a red barn, FAKESTYLE"), TEXT("FAKESTYLE")), FString(TEXT("a red barn, FAKESTYLE")));
	TestEqual(TEXT("a longer word does not hold them"), WithTriggerWords(TEXT("a FAKESTYLES barn"), TEXT("FAKESTYLE")), FString(TEXT("FAKESTYLE, a FAKESTYLES barn")));
	TestEqual(TEXT("nor one glued in front"), WithTriggerWords(TEXT("a NOFAKESTYLE barn"), TEXT("FAKESTYLE")), FString(TEXT("FAKESTYLE, a NOFAKESTYLE barn")));
	TestEqual(TEXT("a later whole word does"), WithTriggerWords(TEXT("FAKESTYLES and FAKESTYLE"), TEXT("FAKESTYLE")), FString(TEXT("FAKESTYLES and FAKESTYLE")));
	TestEqual(TEXT("a phrase, whole"), WithTriggerWords(TEXT("the test scene words here"), TEXT("test scene words")), FString(TEXT("the test scene words here")));
	TestEqual(TEXT("a phrase, cut"), WithTriggerWords(TEXT("the test scene wordsmith"), TEXT("test scene words")), FString(TEXT("test scene words, the test scene wordsmith")));
	TestEqual(TEXT("an empty prompt stays empty"), WithTriggerWords(FString(), TEXT("FAKESTYLE")), FString());
	TestEqual(TEXT("no trigger words, no change"), WithTriggerWords(TEXT("a red barn"), FString()), FString(TEXT("a red barn")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioTrainedFormTest, "Scenario.Core.Trained.Form", SCENARIO_TEST_FLAGS)
bool FScenarioTrainedFormTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Json = Fixture(*this);
	if (!Json.IsValid())
	{
		return true;
	}
	const FScenarioTrainedInfo LoraInfo = Derive(Json, Lora);
	const TSharedPtr<FJsonObject> BaseSchema = FScenarioApi::SchemaFromModel(Record(Json, TEXT("bases"), Flux1));
	const TSharedRef<FJsonObject> Form = ScenarioTrained::FormSchema(LoraInfo, BaseSchema);
	TestEqual(TEXT("the form is the trained model's"), ScenarioJson::GetString(Form, TEXT("model_id")), FString(Lora));
	TestEqual(TEXT("it says how it runs"), ScenarioJson::GetString(ScenarioJson::GetObject(Form, TEXT("trained")), TEXT("base_model_id")), FString(Flux1));

	const TArray<FScenarioField> Fields = ScenarioSchema::FieldsFromSchema(Form);
	TestNull(TEXT("the base's loras input is the plugin's"), ScenarioSchema::Find(Fields, TEXT("loras")));
	TestNull(TEXT("so is its modelId input"), ScenarioSchema::Find(Fields, TEXT("modelId")));
	TestEqual(TEXT("the base's prompt stays"), ScenarioSchema::PromptName(Fields), FString(TEXT("prompt")));
	TestNotNull(TEXT("the base's settings stay"), ScenarioSchema::Find(Fields, TEXT("numOutputs")));
	const FScenarioField* Weight = ScenarioSchema::Find(Fields, ScenarioTrained::WeightField);
	if (TestNotNull(TEXT("a LoRA weight"), Weight))
	{
		TestTrue(TEXT("a number"), Weight->Type == EScenarioFieldType::Number);
		TestEqual(TEXT("its own weight by default"), Weight->Default.IsValid() ? Weight->Default->AsNumber() : -1.0, 0.8);
		TestEqual(TEXT("the base's maximum"), Weight->Max.Get(0.0), 2.0);
	}
	const FScenarioField* Trigger = ScenarioSchema::Find(Fields, ScenarioTrained::TriggerField);
	if (TestNotNull(TEXT("a trigger words switch"), Trigger))
	{
		TestTrue(TEXT("a switch"), Trigger->Type == EScenarioFieldType::Boolean);
		TestTrue(TEXT("on by default"), ScenarioJson::Truthy(Trigger->Default));
		TestTrue(TEXT("it shows the words"), Trigger->Description.Contains(TEXT("FAKESTYLE")));
	}
	const TSharedRef<FJsonObject> TooHeavy = ScenarioSchema::InitialValues(Fields);
	TooHeavy->SetStringField(TEXT("prompt"), TEXT("a red barn"));
	TooHeavy->SetNumberField(ScenarioTrained::WeightField, 3.0);
	TestEqual(TEXT("a weight outside the base's range is refused"), ScenarioSchema::Validate(Fields, TooHeavy).Num(), 1);

	// The form's payload as the base's parameters.
	const TSharedRef<FJsonObject> Payload = ScenarioSchema::Payload(Fields, ScenarioJson::ParseObject(TEXT("{\"prompt\":\"a red barn\",\"lorasScale\":1.25,\"addTriggerWords\":true,\"numOutputs\":1}")));
	const TSharedRef<FJsonObject> Sent = ScenarioTrained::BaseParameters(LoraInfo, Payload);
	TestEqual(TEXT("loras carries the LoRA"), FString::Join(ScenarioJson::GetStringArray(Sent, TEXT("loras")), TEXT(",")), FString(Lora));
	TestEqual(TEXT("lorasScale carries its weight"), FirstNumber(Sent, TEXT("lorasScale")), 1.25);
	TestEqual(TEXT("trigger words put in front of the prompt"), ScenarioJson::GetString(Sent, TEXT("prompt")), FString(TEXT("FAKESTYLE, a red barn")));
	TestFalse(TEXT("the switch is not sent"), Sent->HasField(ScenarioTrained::TriggerField));
	TestFalse(TEXT("no modelId for a LoRA"), Sent->HasField(TEXT("modelId")));
	TestEqual(TEXT("the base's settings pass through"), ScenarioJson::GetNumber(Sent, TEXT("numOutputs")), 1.0);
	TestEqual(TEXT("the attached model"), ScenarioTrained::AttachedId(Sent), FString(Lora));

	const TSharedRef<FJsonObject> LeftOut = ScenarioTrained::BaseParameters(LoraInfo, ScenarioJson::ParseObject(TEXT("{\"prompt\":\"a red barn\",\"addTriggerWords\":false}")));
	TestEqual(TEXT("trigger words left out"), ScenarioJson::GetString(LeftOut, TEXT("prompt")), FString(TEXT("a red barn")));
	TestEqual(TEXT("its own weight when none is given"), FirstNumber(LeftOut, TEXT("lorasScale")), 0.8);
	const TSharedRef<FJsonObject> Twice = ScenarioTrained::BaseParameters(LoraInfo, ScenarioJson::ParseObject(TEXT("{\"prompt\":\"FAKESTYLE, a red barn\"}")));
	TestEqual(TEXT("never twice"), ScenarioJson::GetString(Twice, TEXT("prompt")), FString(TEXT("FAKESTYLE, a red barn")));

	const FScenarioTrainedInfo CompositionInfo = Derive(Json, Composition);
	const TArray<FScenarioField> CompositionFields = ScenarioSchema::FieldsFromSchema(ScenarioTrained::FormSchema(CompositionInfo, BaseSchema));
	TestNull(TEXT("a composition has no weight: its concepts carry theirs"), ScenarioSchema::Find(CompositionFields, ScenarioTrained::WeightField));
	const TSharedRef<FJsonObject> CompositionSent = ScenarioTrained::BaseParameters(CompositionInfo, ScenarioJson::ParseObject(TEXT("{\"prompt\":\"a harbor\",\"loras\":[\"model_test_other\"]}")));
	TestEqual(TEXT("modelId carries the composition"), ScenarioJson::GetString(CompositionSent, TEXT("modelId")), FString(Composition));
	TestFalse(TEXT("a composition replaces any LoRAs"), CompositionSent->HasField(TEXT("loras")));
	TestFalse(TEXT("and their weights"), CompositionSent->HasField(TEXT("lorasScale")));
	TestEqual(TEXT("composition trigger words"), ScenarioJson::GetString(CompositionSent, TEXT("prompt")), FString(TEXT("test scene words, a harbor")));
	TestEqual(TEXT("the attached composition"), ScenarioTrained::AttachedId(CompositionSent), FString(Composition));

	// Use settings: back to the trained form's values.
	const TSharedRef<FJsonObject> Back = ScenarioTrained::FormValues(Sent);
	TestEqual(TEXT("the weight comes back as a number"), ScenarioJson::GetNumber(Back, ScenarioTrained::WeightField), 1.25);
	TestFalse(TEXT("loras is not a form value"), Back->HasField(TEXT("loras")));
	TestTrue(TEXT("the form accepts them"), ScenarioSchema::Validate(Fields, Back).Num() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioTrainedCatalogTest, "Scenario.Core.Trained.Catalog", SCENARIO_TEST_FLAGS)
bool FScenarioTrainedCatalogTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Json = Fixture(*this);
	if (!Json.IsValid())
	{
		return true;
	}
	TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
	Serve(*Backend, Json);
	const FString Path = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("Scenario"), FGuid::NewGuid().ToString() + TEXT("_catalog.json"));
	TSharedRef<FScenarioCatalog> Catalog = MakeShared<FScenarioCatalog>(MakeShared<FScenarioApi>(Backend), Path);
	TArray<FScenarioCatalogModel> Rows;
	for (const TCHAR* Id : { Lora, Composition, Flux2Lora, Unknown })
	{
		FScenarioCatalogModel Row;
		if (FScenarioCatalog::ParseRow(Record(Json, TEXT("trained"), Id), true, Row))
		{
			Rows.Add(Row);
		}
	}
	Catalog->SetModels(Rows);
	TestTrue(TEXT("a trained row waits to be read"), Catalog->NeedsResolve(Lora));
	TestTrue(TEXT("so does a model the catalog does not list"), Catalog->NeedsResolve(Flux1));

	FScenarioReply Reply = FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("no answer"));
	Catalog->GetSchema(Lora, [&Reply](const FScenarioReply& In) { Reply = In; });
	if (!TestTrue(TEXT("the LoRA's form"), Reply.bOk))
	{
		AddError(Reply.Error.Message);
		return false;
	}
	TestTrue(TEXT("it is the trained form"), Reply.Value->HasField(TEXT("trained")));
	TestNotNull(TEXT("its weight"), ScenarioSchema::Find(ScenarioSchema::FieldsFromSchema(Reply.Value), ScenarioTrained::WeightField));
	const FScenarioTrainedInfo* Info = Catalog->FindTrained(Lora);
	TestTrue(TEXT("how it runs is kept"), Info && Info->BaseId() == Flux1);
	TestFalse(TEXT("read now"), Catalog->NeedsResolve(Lora));
	TestEqual(TEXT("the recorded bases were read"), Catalog->Bases().Num(), 2);
	TestEqual(TEXT("a listed base missing from Scenario is skipped"), Backend->CountOf(TEXT("GET /models/model_z-image")), 1);

	Catalog->GetSchema(Composition, [&Reply](const FScenarioReply& In) { Reply = In; });
	TestTrue(TEXT("the composition's form"), Reply.bOk);
	TestEqual(TEXT("the bases are read once"), Backend->CountOf(TEXT("GET /models/model_z-image")), 1);

	Catalog->GetSchema(Unknown, [&Reply](const FScenarioReply& In) { Reply = In; });
	TestFalse(TEXT("an unknown type has no form"), Reply.bOk);
	TestTrue(TEXT("and says why"), Reply.Error.Message.Contains(TEXT("sd-xl-lora")));
	TestNull(TEXT("nothing kept for it"), Catalog->FindTrained(Unknown));

	Catalog->GetSchema(Flux1, [&Reply](const FScenarioReply& In) { Reply = In; });
	TestTrue(TEXT("a base model has its own form"), Reply.bOk && !Reply.Value->HasField(TEXT("trained")));
	TestFalse(TEXT("read: it runs as itself"), Catalog->NeedsResolve(Flux1));

	// The picker's label comes from the rows and the bases, with no request.
	const FScenarioCatalogModel* Row = Catalog->Find(Flux2Lora);
	TestTrue(TEXT("picker label"), Row && Catalog->TrainedInfo(*Row).Label() == TEXT("LoRA via FLUX 2 (Dev)"));

	// The cache keeps the bases and what trained rows say about themselves.
	TSharedRef<FScenarioCatalog> Cached = MakeShared<FScenarioCatalog>(MakeShared<FScenarioApi>(Backend), Path);
	TestTrue(TEXT("cache loads"), Cached->LoadCache());
	TestEqual(TEXT("bases cached"), Cached->Bases().Num(), 2);
	TestTrue(TEXT("bases fresh"), Cached->HasFreshBases());
	const FScenarioCatalogModel* CachedLora = Cached->Find(Lora);
	if (TestNotNull(TEXT("trained row cached"), CachedLora))
	{
		TestEqual(TEXT("trigger words cached"), CachedLora->Summary.TriggerWords, FString(TEXT("FAKESTYLE")));
		TestEqual(TEXT("own weight cached"), CachedLora->Summary.OwnScale, 0.8);
		TestEqual(TEXT("compliantModelIds cached"), CachedLora->Summary.CompliantModelIds.Num(), 2);
		TestEqual(TEXT("label from the cache"), Cached->TrainedInfo(*CachedLora).Label(), FString(TEXT("LoRA via FLUX.1 Dev")));
	}

	Catalog->ClearPrivate();
	TestNull(TEXT("trained models are read again in another workspace"), Catalog->FindTrained(Lora));
	TestEqual(TEXT("the public bases stay"), Catalog->Bases().Num(), 2);
	IFileManager::Get().Delete(*Path);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioTrainedSessionTest, "Scenario.Core.Trained.Session", SCENARIO_TEST_FLAGS)
bool FScenarioTrainedSessionTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Json = Fixture(*this);
	if (!Json.IsValid())
	{
		return true;
	}
	FRig Rig(Json);
	if (!TestTrue(TEXT("ready"), Rig.Session->IsReady()))
	{
		return false;
	}

	// The price check goes to the base, with the LoRA, its weight and its trigger words.
	const TScenarioResult<FScenarioQuote> Priced = Rig.Price(FRig::Run(Lora));
	if (!TestTrue(TEXT("priced"), Priced.bOk))
	{
		AddError(Priced.Error.Message);
		return false;
	}
	const FScenarioQuote Quote = Priced.Value;
	const FScenarioRequest* DryRun = Rig.LastRun();
	if (!TestNotNull(TEXT("a dry run"), DryRun))
	{
		return false;
	}
	TestTrue(TEXT("a free dry run"), DryRun->Call == EScenarioCall::DryRun);
	TestEqual(TEXT("the base is the model called"), DryRun->Path, FString(TEXT("/generate/custom/model_bfl-flux-1-dev")));
	TestEqual(TEXT("loras carries the LoRA"), FString::Join(ScenarioJson::GetStringArray(DryRun->Body, TEXT("loras")), TEXT(",")), FString(Lora));
	TestEqual(TEXT("lorasScale carries its own weight"), FirstNumber(DryRun->Body, TEXT("lorasScale")), 0.8);
	TestEqual(TEXT("trigger words in front"), ScenarioJson::GetString(DryRun->Body, TEXT("prompt")), FString(TEXT("FAKESTYLE, a red barn")));
	TestFalse(TEXT("no form-only field is sent"), DryRun->Body->HasField(ScenarioTrained::TriggerField));
	TestEqual(TEXT("the quote is for the base"), Quote.TargetId, FString(Flux1));
	TestEqual(TEXT("and names the trained model"), Quote.TrainedModelId, FString(Lora));

	// The fingerprint covers the trained model and its weight.
	FScenarioSession& Session = *Rig.Session;
	const FString Fp = Session.Fingerprint(FRig::Run(Lora));
	TestEqual(TEXT("the request's fingerprint is the quote's"), Fp, Quote.Fingerprint);
	TestEqual(TEXT("the same weight given or not"), Session.Fingerprint(FRig::Run(Lora, TEXT("{\"prompt\":\"a red barn\",\"lorasScale\":0.8}"))), Fp);
	TestNotEqual(TEXT("another weight, another fingerprint"), Session.Fingerprint(FRig::Run(Lora, TEXT("{\"prompt\":\"a red barn\",\"lorasScale\":1}"))), Fp);
	TestNotEqual(TEXT("trigger words left out, another fingerprint"), Session.Fingerprint(FRig::Run(Lora, TEXT("{\"prompt\":\"a red barn\",\"addTriggerWords\":false}"))), Fp);
	TestNotEqual(TEXT("the base alone, another fingerprint"), Session.Fingerprint(FRig::Run(Flux1, TEXT("{\"prompt\":\"FAKESTYLE, a red barn\"}"))), Fp);
	TestTrue(TEXT("the composition prices"), Rig.Price(FRig::Run(Composition)).bOk);
	TestEqual(TEXT("a composition rides in modelId"), ScenarioJson::GetString(Rig.LastRun()->Body, TEXT("modelId")), FString(Composition));
	TestFalse(TEXT("with no loras"), Rig.LastRun()->Body->HasField(TEXT("loras")));
	TestNotEqual(TEXT("another trained model, another fingerprint"), Session.Fingerprint(FRig::Run(Composition)), Fp);

	// A quote for one weight or one trained model never pays for another.
	const FScenarioRunRequest Heavier = FRig::Run(Lora, TEXT("{\"prompt\":\"a red barn\",\"lorasScale\":1}"), TEXT("agent"));
	TScenarioResult<FString> Refused = Rig.Submit(Heavier, Quote.Id, Quote.Exact);
	TestFalse(TEXT("a quote for weight 0.8 does not pay for 1"), Refused.bOk);
	TestTrue(TEXT("refused before sending"), Refused.Error.bNotSent);
	Refused = Rig.Submit(FRig::Run(Composition, TEXT("{\"prompt\":\"a red barn\"}"), TEXT("agent")), Quote.Id, Quote.Exact);
	TestFalse(TEXT("a quote for one trained model does not pay for another"), Refused.bOk);
	FScenarioRunRequest Relabeled = FRig::Run(Flux1, ScenarioJson::Write(Quote.Parameters), TEXT("agent"));
	Relabeled.TrainedModelId = Composition;
	Refused = Rig.Submit(Relabeled, Quote.Id, Quote.Exact);
	TestFalse(TEXT("the trained model it names must be the one it carries"), Refused.bOk);
	TestEqual(TEXT("nothing paid"), Rig.Calls(EScenarioCall::Paid), 0);

	// The person's click: sent once, to the base, recorded with both models.
	const TScenarioResult<FString> Sent = Rig.Submit(FRig::Run(Lora));
	if (!TestTrue(TEXT("sent"), Sent.bOk))
	{
		AddError(Sent.Error.Message);
		return false;
	}
	TestEqual(TEXT("one paid call"), Rig.Calls(EScenarioCall::Paid), 1);
	const FScenarioRequest* Paid = Rig.LastRun();
	TestEqual(TEXT("paid call to the base"), Paid->Path, FString(TEXT("/generate/custom/model_bfl-flux-1-dev")));
	TestEqual(TEXT("paid body is the priced body"), ScenarioJson::Canonical(Paid->Body), ScenarioJson::Canonical(Quote.Parameters));
	const FScenarioJobRow* Row = Session.Ledger().Find(Sent.Value);
	if (TestNotNull(TEXT("job row"), Row))
	{
		TestEqual(TEXT("the base is the job's model"), Row->ModelId, FString(Flux1));
		TestEqual(TEXT("base name"), Row->ModelName, FString(TEXT("FLUX.1 Dev")));
		TestEqual(TEXT("the trained model is kept beside it"), Row->TrainedModelId, FString(Lora));
		TestEqual(TEXT("with its name"), Row->TrainedModelName, FString(TEXT("Test Painted Style")));
		TestEqual(TEXT("the row's fingerprint is the quote's"), Row->Fingerprint, Quote.Fingerprint);
		FScenarioJobRow Loaded;
		TestTrue(TEXT("the job list keeps it"), FScenarioJobRow::FromJson(Row->ToJson(), Loaded) && Loaded.TrainedModelId == Lora && Loaded.TrainedModelName == Row->TrainedModelName);
	}
	TestNull(TEXT("the quote was used"), Session.Quotes().FindById(Quote.Id, Rig.Time));

	// An agent's generate rebuilds the request from its quote: base, trained model and parameters as priced.
	const TScenarioResult<FScenarioQuote> Again = Rig.Price(FRig::Run(Flux2Lora, TEXT("{\"prompt\":\"a crate\"}"), TEXT("agent")));
	if (TestTrue(TEXT("priced again"), Again.bOk))
	{
		FScenarioRunRequest FromQuote;
		FromQuote.TargetId = Again.Value.TargetId;
		FromQuote.TrainedModelId = Again.Value.TrainedModelId;
		FromQuote.Parameters = ScenarioJson::CloneObject(Again.Value.Parameters);
		FromQuote.Source = TEXT("agent");
		TestEqual(TEXT("FLUX 2 LoRA on FLUX 2"), FromQuote.TargetId, FString(Flux2));
		const TScenarioResult<FString> AgentSent = Rig.Submit(FromQuote, Again.Value.Id, Again.Value.Exact);
		TestTrue(TEXT("the agent's run is sent"), AgentSent.bOk);
		const FScenarioJobRow* AgentRow = Session.Ledger().Find(AgentSent.Value);
		TestTrue(TEXT("recorded with the trained model"), AgentRow && AgentRow->ModelId == Flux2 && AgentRow->TrainedModelId == Flux2Lora);
	}

	// A trained model no base takes is never priced or sent.
	const TScenarioResult<FScenarioQuote> NoBase = Rig.Price(FRig::Run(Unknown));
	TestFalse(TEXT("no price without a base"), NoBase.bOk);
	TestTrue(TEXT("it says why"), NoBase.Error.Message.Contains(TEXT("sd-xl-lora")));
	TestEqual(TEXT("no request names it"), Rig.Backend->CountOf(TEXT("POST /generate/custom/model_test_unknown_lora")), 0);

	// A trained form's payload is never priced as it is, even for a model the catalog does not list.
	const TScenarioResult<FScenarioQuote> Unread = Rig.Price(FRig::Run(TEXT("model_test_missing"), TEXT("{\"prompt\":\"a red barn\",\"lorasScale\":0.8,\"addTriggerWords\":true}")));
	TestFalse(TEXT("an unread trained form is not priced"), Unread.bOk);
	TestEqual(TEXT("nothing asked for it"), Rig.Backend->CountOf(TEXT("POST /generate/custom/model_test_missing")), 0);

	// After a project switch the trained model is read again before it is priced.
	FScenarioWorkspace Other = Session.GetWorkspace();
	Other.ProjectId = TEXT("proj_test_other");
	Session.SetWorkspace(Other);
	TestNull(TEXT("forgotten in the other project"), Session.Catalog().FindTrained(Lora));
	TestTrue(TEXT("read again and priced"), Rig.Price(FRig::Run(Lora)).bOk);
	TestEqual(TEXT("still on its base"), Rig.LastRun()->Path, FString(TEXT("/generate/custom/model_bfl-flux-1-dev")));

	// SCENARIO_NO_SPEND holds for trained models too: priced, never sent.
	FRig Off(Json, true);
	TestTrue(TEXT("priced with spending off"), Off.Price(FRig::Run(Lora)).bOk);
	const TScenarioResult<FString> Blocked = Off.Submit(FRig::Run(Lora));
	TestFalse(TEXT("refused with spending off"), Blocked.bOk);
	TestEqual(TEXT("no paid call with spending off"), Off.Calls(EScenarioCall::Paid), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioTrainedBasesTest, "Scenario.Core.Trained.Bases", SCENARIO_TEST_FLAGS)
bool FScenarioTrainedBasesTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Json = Fixture(*this);
	if (!Json.IsValid())
	{
		return true;
	}
	TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
	Serve(*Backend, Json);
	// Each recorded base answers, does not answer, or is gone, as the test says.
	enum class EAnswer : uint8 { Ok, None, Gone };
	TSharedRef<TMap<FString, EAnswer>> Answers = MakeShared<TMap<FString, EAnswer>>();
	for (const TCHAR* Id : { Flux2, Flux1 })
	{
		const TSharedPtr<FJsonObject> Reply = ScenarioJson::GetObject(ScenarioJson::GetObject(Json, TEXT("bases")), Id);
		const FString BaseId = Id;
		Backend->On(TEXT("GET /models/") + BaseId, [Answers, BaseId, Reply](const FScenarioRequest&)
		{
			switch (Answers->FindRef(BaseId))
			{
			case EAnswer::None: return FScenarioReply::Fail(EScenarioErrorKind::Transport, TEXT("No connection."));
			case EAnswer::Gone: return FScenarioReply::Fail(EScenarioErrorKind::NotFound, TEXT("Model not found."));
			default: return FScenarioReply::Ok(Reply);
			}
		});
	}
	const FString Path = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("Scenario"), FGuid::NewGuid().ToString() + TEXT("_catalog.json"));
	TSharedRef<FScenarioCatalog> Catalog = MakeShared<FScenarioCatalog>(MakeShared<FScenarioApi>(Backend), Path);
	FScenarioCatalogModel Row;
	if (FScenarioCatalog::ParseRow(Record(Json, TEXT("trained"), Lora), true, Row))
	{
		Catalog->SetModels({ Row });
	}

	// FLUX 2 does not answer: FLUX.1 is read, and the list is read again next time.
	Answers->Add(FString(Flux2), EAnswer::None);
	Catalog->LoadBases();
	TestEqual(TEXT("the base that answered"), Catalog->Bases().Num(), 1);
	TestFalse(TEXT("not fresh while a base did not answer"), Catalog->HasFreshBases());
	FScenarioReply Reply = FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("no answer"));
	Catalog->GetSchema(Lora, [&Reply](const FScenarioReply& In) { Reply = In; });
	const FScenarioTrainedInfo* Info = Catalog->FindTrained(Lora);
	TestTrue(TEXT("the LoRA runs on FLUX.1"), Reply.bOk && Info && Info->BaseId() == Flux1);

	// FLUX 2 answers, FLUX.1 does not: FLUX.1 keeps its last reading, and the LoRA its base.
	Answers->Add(FString(Flux2), EAnswer::Ok);
	Answers->Add(FString(Flux1), EAnswer::None);
	Catalog->LoadBases();
	TestTrue(TEXT("a base with no answer is kept"), Catalog->Bases().Num() == 2 && Catalog->Bases()[1].Id == Flux1);
	TestFalse(TEXT("still not fresh"), Catalog->HasFreshBases());
	TestNotNull(TEXT("the LoRA keeps its base"), Catalog->FindTrained(Lora));

	// Scenario says FLUX.1 is gone: it leaves the list, and the LoRA it ran is read again.
	Answers->Add(FString(Flux1), EAnswer::Gone);
	Catalog->LoadBases();
	TestTrue(TEXT("a base Scenario no longer has is left out"), Catalog->Bases().Num() == 1 && Catalog->Bases()[0].Id == Flux2);
	TestTrue(TEXT("fresh once every base answered"), Catalog->HasFreshBases());
	TestNull(TEXT("a trained model whose base left is forgotten"), Catalog->FindTrained(Lora));
	TestTrue(TEXT("and waits to be read again"), Catalog->NeedsResolve(Lora));
	IFileManager::Get().Delete(*Path);

	// Scenario says every base is gone: the empty list is applied, and the LoRA read on the last one is forgotten.
	const FString EmptiedPath = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("Scenario"), FGuid::NewGuid().ToString() + TEXT("_catalog.json"));
	TSharedRef<FScenarioCatalog> Emptied = MakeShared<FScenarioCatalog>(MakeShared<FScenarioApi>(Backend), EmptiedPath);
	Emptied->SetModels({ Row });
	Answers->Add(FString(Flux2), EAnswer::None);
	Answers->Add(FString(Flux1), EAnswer::Ok);
	Emptied->LoadBases();
	Emptied->GetSchema(Lora, [](const FScenarioReply&) {});
	TestNotNull(TEXT("the LoRA runs on FLUX.1 first"), Emptied->FindTrained(Lora));
	Answers->Add(FString(Flux2), EAnswer::Gone);
	Answers->Add(FString(Flux1), EAnswer::Gone);
	FScenarioError Loaded;
	Emptied->LoadBases([&Loaded](const FScenarioError& Error) { Loaded = Error; });
	TestEqual(TEXT("no base left"), Emptied->Bases().Num(), 0);
	TestNull(TEXT("the LoRA on a base that left is forgotten"), Emptied->FindTrained(Lora));
	TestTrue(TEXT("loading says there is no base"), Loaded.Kind != EScenarioErrorKind::None);
	IFileManager::Get().Delete(*EmptiedPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioTrainedResolveTest, "Scenario.Core.Trained.Resolve", SCENARIO_TEST_FLAGS)
bool FScenarioTrainedResolveTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Json = Fixture(*this);
	if (!Json.IsValid())
	{
		return true;
	}

	// A model the catalog does not list yet (first launch) is read before its price check: never priced as itself when trained.
	{
		FRig Rig(Json);
		Rig.Session->Catalog().SetModels({});
		const TScenarioResult<FScenarioQuote> Unlisted = Rig.Price(FRig::Run(Composition));
		TestTrue(TEXT("an unlisted composition is priced"), Unlisted.bOk);
		TestEqual(TEXT("on its base"), Unlisted.Value.TargetId, FString(Flux1));
		TestEqual(TEXT("in modelId"), ScenarioJson::GetString(Rig.LastRun()->Body, TEXT("modelId")), FString(Composition));
		TestEqual(TEXT("never as the model called"), Rig.Backend->CountOf(TEXT("POST /generate/custom/model_test_scene_composition")), 0);
		const int32 Reads = Rig.Backend->CountOf(TEXT("GET /models/model_bfl-flux-1-dev"));
		TestTrue(TEXT("an unlisted model with inputs of its own is priced"), Rig.Price(FRig::Run(Flux1)).bOk);
		TestEqual(TEXT("as itself"), Rig.LastRun()->Path, FString(TEXT("/generate/custom/model_bfl-flux-1-dev")));
		TestTrue(TEXT("priced again"), Rig.Price(FRig::Run(Flux1, TEXT("{\"prompt\":\"a blue barn\"}"))).bOk);
		TestEqual(TEXT("read once"), Rig.Backend->CountOf(TEXT("GET /models/model_bfl-flux-1-dev")), Reads + 1);
	}

	// The project changes while the trained model is read: never priced unresolved. (The fake backend signs in like an API key,
	// whose quote scope ignores the project, so the read that the switch made stale is what refuses here.)
	{
		FRig Rig(Json);
		FScenarioSession& Session = *Rig.Session;
		FScenarioWorkspace Other = Session.GetWorkspace();
		Other.ProjectId = TEXT("proj_test_other");
		const TSharedPtr<FJsonObject> Reply = ScenarioJson::GetObject(ScenarioJson::GetObject(Json, TEXT("trained")), Lora);
		bool bSwitched = false;
		Rig.Backend->On(FString(TEXT("GET /models/")) + Lora, [&Session, &bSwitched, Other, Reply](const FScenarioRequest&)
		{
			if (!bSwitched)
			{
				bSwitched = true;
				Session.SetWorkspace(Other);
			}
			return FScenarioReply::Ok(Reply);
		});
		const TScenarioResult<FScenarioQuote> Moved = Rig.Price(FRig::Run(Lora));
		TestTrue(TEXT("switched during the read"), bSwitched);
		TestFalse(TEXT("not priced"), Moved.bOk);
		TestTrue(TEXT("says why"), Moved.Error.Message.Contains(TEXT("Pick the model again")));
		TestEqual(TEXT("no price check sent"), Rig.Calls(EScenarioCall::DryRun), 0);
		TestTrue(TEXT("priced when asked again"), Rig.Price(FRig::Run(Lora)).bOk);
		TestEqual(TEXT("on its base"), Rig.LastRun()->Path, FString(TEXT("/generate/custom/model_bfl-flux-1-dev")));
	}

	// The base takes 2048 characters of prompt: the trigger words count.
	{
		FRig Rig(Json);
		const FString Long = FString::ChrN(2048, TEXT('a'));
		const TScenarioResult<FScenarioQuote> TooLong = Rig.Price(FRig::Run(Lora, FString::Printf(TEXT("{\"prompt\":\"%s\"}"), *Long)));
		TestFalse(TEXT("too long with the trigger words"), TooLong.bOk);
		TestTrue(TEXT("says why"), TooLong.Error.Message.Contains(TEXT("2048")));
		TestEqual(TEXT("no price check sent"), Rig.Calls(EScenarioCall::DryRun), 0);
		TestTrue(TEXT("the full length without them"), Rig.Price(FRig::Run(Lora, FString::Printf(TEXT("{\"prompt\":\"%s\",\"addTriggerWords\":false}"), *Long))).bOk);
		const FString Held = TEXT("FAKESTYLE ") + FString::ChrN(2038, TEXT('a'));
		TestTrue(TEXT("or with them already in the prompt"), Rig.Price(FRig::Run(Lora, FString::Printf(TEXT("{\"prompt\":\"%s\"}"), *Held))).bOk);
	}

	// A base request with the trained model in its loras is the same call as the trained model's: each pays with a quote that names what it runs.
	{
		FRig Rig(Json);
		FScenarioSession& Session = *Rig.Session;
		const TScenarioResult<FScenarioQuote> First = Rig.Price(FRig::Run(Lora));
		if (!TestTrue(TEXT("priced"), First.bOk))
		{
			return false;
		}
		const FScenarioRunRequest Base = FRig::Run(Flux1, ScenarioJson::Write(First.Value.Parameters));
		TestEqual(TEXT("the same call"), Session.Fingerprint(Base), First.Value.Fingerprint);
		const TScenarioResult<FScenarioQuote> BaseQuote = Rig.Price(Base);
		TestTrue(TEXT("the base request gets its own quote"), BaseQuote.bOk && BaseQuote.Value.Id != First.Value.Id && BaseQuote.Value.TrainedModelId.IsEmpty());
		TestNull(TEXT("which replaces the other"), Session.Quotes().FindById(First.Value.Id, Rig.Time));
		const TScenarioResult<FScenarioQuote> Again = Rig.Price(FRig::Run(Lora));
		TestTrue(TEXT("the trained model's quote names it again"), Again.bOk && Again.Value.TrainedModelId == Lora);
		const TScenarioResult<FString> Sent = Rig.Submit(FRig::Run(Lora));
		const FScenarioJobRow* Row = Sent.bOk ? Session.Ledger().Find(Sent.Value) : nullptr;
		TestTrue(TEXT("sent, recorded with the trained model"), Row && Row->TrainedModelId == Lora);
		TestTrue(TEXT("the base request prices again"), Rig.Price(Base).bOk);
		const TScenarioResult<FString> BaseSent = Rig.Submit(Base);
		const FScenarioJobRow* BaseRow = BaseSent.bOk ? Session.Ledger().Find(BaseSent.Value) : nullptr;
		TestTrue(TEXT("sent as the base request"), BaseRow && BaseRow->ModelId == Flux1 && BaseRow->TrainedModelId.IsEmpty());
		TestEqual(TEXT("two paid calls"), Rig.Calls(EScenarioCall::Paid), 2);
	}
	return true;
}

#endif
