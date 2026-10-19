// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "ScenarioApi.h"
#include "ScenarioCatalog.h"
#include "ScenarioFakes.h"
#include "ScenarioJson.h"
#include "ScenarioLanes.h"
#include "ScenarioSchema.h"

namespace ScenarioCatalogTests
{
	TArray<FScenarioField> FieldsFromFixture(FAutomationTestBase& Test, const FString& Name, const TMap<FString, TArray<FString>>& Restrict = {})
	{
		const TSharedPtr<FJsonObject> Schema = ScenarioFixtures::LoadJson(Name);
		if (!Schema.IsValid())
		{
			Test.AddWarning(FString::Printf(TEXT("Fixture %s not found; schema checks skipped."), *Name));
			return {};
		}
		return ScenarioSchema::FieldsFromSchema(Schema, Restrict);
	}
}

using namespace ScenarioCatalogTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioSchemaModelTest, "Scenario.Core.Schema.Model", SCENARIO_TEST_FLAGS)
bool FScenarioSchemaModelTest::RunTest(const FString& Parameters)
{
	const TArray<FScenarioField> Fields = FieldsFromFixture(*this, TEXT("schema_flare.json"));
	if (Fields.Num() == 0)
	{
		return true;
	}
	TestEqual(TEXT("prompt first"), Fields[0].Name, FString(TEXT("prompt")));
	TestEqual(TEXT("prompt type"), Fields[0].Type, EScenarioFieldType::Prompt);
	TestTrue(TEXT("prompt required"), Fields[0].bRequired);
	TestTrue(TEXT("prompt spark offered"), Fields[0].bPromptSpark);
	TestEqual(TEXT("prompt name"), ScenarioSchema::PromptName(Fields), FString(TEXT("prompt")));

	const FScenarioField* References = ScenarioSchema::Find(Fields, TEXT("referenceImages"));
	TestNotNull(TEXT("references"), References);
	if (References)
	{
		TestEqual(TEXT("file list"), References->Type, EScenarioFieldType::Files);
		TestEqual(TEXT("ten references max"), References->MaxItems, 10);
		TestEqual(TEXT("image kind"), References->Kind, FString(TEXT("image")));
		TestTrue(TEXT("references change the price"), References->bCostImpact);
	}
	const FScenarioField* Count = ScenarioSchema::Find(Fields, TEXT("numOutputs"));
	TestTrue(TEXT("step 1 numbers are integers"), Count && Count->Type == EScenarioFieldType::Integer);
	const FScenarioField* Width = ScenarioSchema::Find(Fields, TEXT("width"));
	TestTrue(TEXT("step 16 numbers are integers"), Width && Width->Type == EScenarioFieldType::Integer);
	const FScenarioField* Quality = ScenarioSchema::Find(Fields, TEXT("quality"));
	TestTrue(TEXT("enum"), Quality && Quality->Type == EScenarioFieldType::Enum && Quality->Options.Num() >= 3);
	TestEqual(TEXT("label"), Width ? Width->Label : FString(), FString(TEXT("Width")));

	const TSharedRef<FJsonObject> Values = ScenarioSchema::InitialValues(Fields, ScenarioJson::ParseObject(TEXT("{\"quality\":\"low\"}")));
	TestEqual(TEXT("default kept"), ScenarioJson::GetNumber(Values, TEXT("numOutputs")), 1.0);
	TestEqual(TEXT("preset wins"), ScenarioJson::GetString(Values, TEXT("quality")), FString(TEXT("low")));
	TestTrue(TEXT("missing prompt blocks"), ScenarioSchema::Validate(Fields, Values).Num() > 0);

	Values->SetStringField(TEXT("prompt"), TEXT("  a carved wooden treasure chest  "));
	Values->SetStringField(TEXT("numOutputs"), TEXT("2"));
	Values->SetStringField(TEXT("referenceImages"), TEXT("asset_ref1"));
	TestEqual(TEXT("valid"), ScenarioSchema::Validate(Fields, Values).Num(), 0);
	Values->SetNumberField(TEXT("numOutputs"), 99.0);
	TestTrue(TEXT("max enforced"), ScenarioSchema::Validate(Fields, Values).Num() == 1);
	Values->SetNumberField(TEXT("numOutputs"), 2.4);

	const TSharedRef<FJsonObject> Payload = ScenarioSchema::Payload(Fields, Values);
	TestEqual(TEXT("prompt trimmed"), ScenarioJson::GetString(Payload, TEXT("prompt")), FString(TEXT("a carved wooden treasure chest")));
	TestEqual(TEXT("integers rounded"), ScenarioJson::GetNumber(Payload, TEXT("numOutputs")), 2.0);
	TestEqual(TEXT("single file becomes an array"), ScenarioJson::GetArray(Payload, TEXT("referenceImages")).Num(), 1);
	TestFalse(TEXT("empty fields dropped"), Payload->HasField(TEXT("mask")));
	TestEqual(TEXT("input asset ids"), ScenarioSchema::InputAssetIds(Fields, Payload), TArray<FString>{ TEXT("asset_ref1") });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioSchemaRestrictTest, "Scenario.Core.Schema.Restrict", SCENARIO_TEST_FLAGS)
bool FScenarioSchemaRestrictTest::RunTest(const FString& Parameters)
{
	TMap<FString, TArray<FString>> Restrict;
	Restrict.Add(TEXT("audioFormat"), { TEXT("wav"), TEXT("mp3"), TEXT("ogg"), TEXT("flac") });
	const TArray<FScenarioField> Fields = FieldsFromFixture(*this, TEXT("schema_sonilo_sfx.json"), Restrict);
	if (Fields.Num() == 0)
	{
		return true;
	}
	const FScenarioField* Format = ScenarioSchema::Find(Fields, TEXT("audioFormat"));
	TestNotNull(TEXT("format field"), Format);
	if (Format)
	{
		for (const TSharedPtr<FJsonValue>& Option : Format->Options)
		{
			TestNotEqual(TEXT("aac is not offered (Unreal cannot import it)"), Option->AsString(), FString(TEXT("aac")));
		}
		TestEqual(TEXT("default moves to wav"), Format->Default->AsString(), FString(TEXT("wav")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioSchemaWorkflowTest, "Scenario.Core.Schema.Workflow", SCENARIO_TEST_FLAGS)
bool FScenarioSchemaWorkflowTest::RunTest(const FString& Parameters)
{
	const TArray<FScenarioField> Fields = FieldsFromFixture(*this, TEXT("workflow_get.json"));
	if (Fields.Num() == 0)
	{
		return true;
	}
	const FScenarioField* Text = ScenarioSchema::Find(Fields, TEXT("text1"));
	const FScenarioField* Image = ScenarioSchema::Find(Fields, TEXT("image1"));
	TestTrue(TEXT("workflow prompt field"), Text && Text->Type == EScenarioFieldType::Prompt);
	TestTrue(TEXT("label from the workflow"), Text && Text->Label == TEXT("Character & Scene Description"));
	TestTrue(TEXT("required {always:false}"), Text && !Text->bRequired);
	TestTrue(TEXT("required {always:true}"), Image && Image->bRequired && Image->Type == EScenarioFieldType::File);
	if (!TestTrue(TEXT("workflow fields"), Fields.Num() > 0))
	{
		return false;
	}
	TestEqual(TEXT("prompt first"), Fields[0].Name, FString(TEXT("text1")));
	const TSharedRef<FJsonObject> Values = ScenarioSchema::InitialValues(Fields);
	TestTrue(TEXT("missing image blocks"), ScenarioSchema::Validate(Fields, Values).Num() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioSchemaHumanizeTest, "Scenario.Core.Schema.Humanize", SCENARIO_TEST_FLAGS)
bool FScenarioSchemaHumanizeTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("camel"), ScenarioSchema::Humanize(TEXT("numOutputs")), FString(TEXT("Num outputs")));
	TestEqual(TEXT("snake"), ScenarioSchema::Humanize(TEXT("face_budget")), FString(TEXT("Face budget")));
	TestEqual(TEXT("acronym kept together"), ScenarioSchema::Humanize(TEXT("enablePBR")), FString(TEXT("Enable pbr")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioCatalogRulesTest, "Scenario.Core.Catalog.Rules", SCENARIO_TEST_FLAGS)
bool FScenarioCatalogRulesTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("image"), FScenarioCatalogModel::ModalityOf({ TEXT("txt2img"), TEXT("img2img") }), FString(TEXT("image")));
	TestEqual(TEXT("video wins over image"), FScenarioCatalogModel::ModalityOf({ TEXT("img2video"), TEXT("video2img") }), FString(TEXT("video")));
	TestEqual(TEXT("3d"), FScenarioCatalogModel::ModalityOf({ TEXT("img23d") }), FString(TEXT("3d")));
	TestEqual(TEXT("audio"), FScenarioCatalogModel::ModalityOf({ TEXT("video2audio") }), FString(TEXT("audio")));
	TestEqual(TEXT("texture capability is image"), FScenarioCatalogModel::ModalityOf({ TEXT("txt2img_texture") }), FString(TEXT("image")));

	const TSharedPtr<FJsonObject> Page = ScenarioFixtures::LoadJson(TEXT("models_list.json"));
	if (!Page.IsValid())
	{
		AddWarning(TEXT("Fixture models_list.json not found; catalog checks skipped."));
		return true;
	}
	TArray<FScenarioCatalogModel> Models;
	for (const TSharedPtr<FJsonValue>& Row : ScenarioJson::GetArray(Page, TEXT("models")))
	{
		FScenarioCatalogModel Model;
		if (FScenarioCatalog::ParseRow(Row->AsObject(), false, Model))
		{
			Models.Add(Model);
		}
	}
	TestEqual(TEXT("all rows parsed"), Models.Num(), ScenarioJson::GetArray(Page, TEXT("models")).Num());

	TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
	TSharedRef<FScenarioCatalog> Catalog = MakeShared<FScenarioCatalog>(MakeShared<FScenarioApi>(Backend), FString());
	Catalog->SetModels(Models);

	const FScenarioCatalogModel* Flare = Catalog->Find(TEXT("model_openai-gpt-image-2-5-flare"));
	TestTrue(TEXT("flare featured"), Flare && Flare->bFeatured);
	TestTrue(TEXT("flare provider badge"), Flare && Flare->Badge() == TEXT("OpenAI"));
	TestTrue(TEXT("flare generate chip"), Flare && Flare->Chips.Contains(TEXT("Generate")));
	const FScenarioCatalogModel* Old = Catalog->Find(TEXT("model_bytedance-seedance-1-pro-fast"));
	TestTrue(TEXT("legacy detected"), Old && Old->bLegacy);
	TestTrue(TEXT("replacement read"), Old && Old->ReplacedBy == TEXT("model_bytedance-seedance-2-5"));
	const FScenarioCatalogModel* Tool = Catalog->Find(TEXT("model_photoroom-background-removal"));
	TestTrue(TEXT("tool chip"), Tool && Tool->Chips.Contains(TEXT("Tools")) && Tool->Chips.Contains(TEXT("Remove background")));

	FScenarioCatalogQuery Video;
	Video.Modality = TEXT("video");
	TestEqual(TEXT("legacy hidden by default"), Catalog->Query(Video).Num(), 0);
	Video.bShowLegacy = true;
	TestEqual(TEXT("legacy shown on request"), Catalog->Query(Video).Num(), 1);

	FScenarioCatalogQuery Pinned;
	Pinned.PinnedIds = { TEXT("model_bytedance-seedance-1-pro-fast") };
	const TArray<FScenarioCatalogModel> PinnedFound = Catalog->Query(Pinned);
	TestTrue(TEXT("a pinned legacy model stays visible and first"), PinnedFound.Num() > 0 && PinnedFound[0].Id() == TEXT("model_bytedance-seedance-1-pro-fast"));

	FScenarioCatalogQuery Search;
	Search.Text = TEXT("flare");
	const TArray<FScenarioCatalogModel> Found = Catalog->Query(Search);
	TestTrue(TEXT("search by name"), Found.Num() == 1 && Found[0].Id() == TEXT("model_openai-gpt-image-2-5-flare"));
	Search.Text = TEXT("elevenlabs speech");
	TestTrue(TEXT("every word must match"), Catalog->Query(Search).Num() >= 1);
	Search.Text = TEXT("zzzz");
	TestEqual(TEXT("no match"), Catalog->Query(Search).Num(), 0);

	TestTrue(TEXT("audio chips"), Catalog->ChipsForModality(TEXT("audio")).Contains(TEXT("SFX")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioCatalogRefreshTest, "Scenario.Core.Catalog.Refresh", SCENARIO_TEST_FLAGS)
bool FScenarioCatalogRefreshTest::RunTest(const FString& Parameters)
{
	TSharedRef<FScenarioFakeBackend> Backend = MakeShared<FScenarioFakeBackend>();
	int32 Pages = 0;
	Backend->On(TEXT("models_list"), [&Pages](const TSharedPtr<FJsonObject>& Arguments)
	{
		const TSharedPtr<FJsonObject> Filters = ScenarioJson::GetObject(Arguments, TEXT("filters"));
		if (ScenarioJson::GetString(Filters, TEXT("privacy")) == TEXT("private"))
		{
			return FScenarioReply::Ok(ScenarioJson::ParseObject(TEXT("{\"models\":[{\"id\":\"model_team_lora\",\"name\":\"Team Props\",\"type\":\"flux.1-lora\",\"privacy\":\"private\",\"status\":\"trained\",\"capabilities\":[\"txt2img\"],\"tags\":[]},{\"id\":\"model_training\",\"name\":\"Not ready\",\"status\":\"training\",\"capabilities\":[\"txt2img\"]}]}")));
		}
		++Pages;
		const FString Token = ScenarioJson::GetString(Arguments, TEXT("page_token"));
		if (Token.IsEmpty())
		{
			return FScenarioReply::Ok(ScenarioJson::ParseObject(TEXT("{\"models\":[{\"id\":\"model_a\",\"name\":\"A\",\"capabilities\":[\"txt2img\"],\"tags\":[],\"createdAt\":\"2026-01-01T00:00:00Z\"}],\"nextPaginationToken\":\"p2\"}")));
		}
		return FScenarioReply::Ok(ScenarioJson::ParseObject(TEXT("{\"models\":[{\"id\":\"model_b\",\"name\":\"B\",\"capabilities\":[\"txt23d\"],\"tags\":[],\"createdAt\":\"2026-02-01T00:00:00Z\"}]}")));
	});
	const FString Path = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("Scenario"), FGuid::NewGuid().ToString() + TEXT("_catalog.json"));
	TSharedRef<FScenarioCatalog> Catalog = MakeShared<FScenarioCatalog>(MakeShared<FScenarioApi>(Backend), Path);
	bool bDone = false;
	Catalog->Refresh(TEXT("key"), [&bDone](const FScenarioError& Error) { bDone = Error.Kind == EScenarioErrorKind::None; });
	TestTrue(TEXT("refresh finished"), bDone);
	TestEqual(TEXT("two public pages"), Pages, 2);
	TestEqual(TEXT("public plus ready trained models"), Catalog->All().Num(), 3);
	const FScenarioCatalogModel* Team = Catalog->Find(TEXT("model_team_lora"));
	TestTrue(TEXT("team model marked"), Team && Team->bTeam && Team->Chips.Contains(TEXT("Trained")));
	TestFalse(TEXT("cache fresh"), Catalog->IsStale());

	TSharedRef<FScenarioCatalog> Cached = MakeShared<FScenarioCatalog>(MakeShared<FScenarioApi>(Backend), Path);
	TestTrue(TEXT("cache loads"), Cached->LoadCache());
	TestEqual(TEXT("cache keeps rows"), Cached->All().Num(), 3);
	TestEqual(TEXT("cache keeps workspace"), Cached->WorkspaceKey(), FString(TEXT("key")));

	Cached->ClearPrivate();
	TestNull(TEXT("team models cleared on workspace switch"), Cached->Find(TEXT("model_team_lora")));
	TestEqual(TEXT("public kept"), Cached->All().Num(), 2);
	IFileManager::Get().Delete(*Path);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioLanesTest, "Scenario.Core.Lanes", SCENARIO_TEST_FLAGS)
bool FScenarioLanesTest::RunTest(const FString& Parameters)
{
	const FScenarioLanes& Lanes = FScenarioLanes::Get();
	for (const TCHAR* Id : { TEXT("image"), TEXT("3d"), TEXT("material"), TEXT("audio"), TEXT("video"), TEXT("environment"), TEXT("render") })
	{
		const FScenarioLane* Lane = Lanes.Find(Id);
		TestNotNull(FString::Printf(TEXT("lane %s"), Id), Lane);
		if (Lane)
		{
			TestTrue(FString::Printf(TEXT("lane %s has models"), Id), Lane->Models.Num() > 0);
			TestTrue(FString::Printf(TEXT("lane %s has a starter prompt"), Id), Lane->StarterPrompts.Num() > 0);
		}
	}
	// Every lane and mode opens on a model.
	for (const FScenarioLane& Lane : Lanes.All())
	{
		TestFalse(FString::Printf(TEXT("lane %s default model"), *Lane.Id), Lanes.DefaultModel(Lane.Id, FString()).IsEmpty());
		for (const FString& Mode : Lane.Modes)
		{
			TestFalse(FString::Printf(TEXT("lane %s mode %s default model"), *Lane.Id, *Mode), Lanes.DefaultModel(Lane.Id, Mode).IsEmpty());
		}
	}
	TestEqual(TEXT("image default is cheap and fast"), Lanes.DefaultModel(TEXT("image")), FString(TEXT("model_openai-gpt-image-2-5-flare")));
	TestTrue(TEXT("3d edit mode has models"), Lanes.Find(TEXT("3d")) && Lanes.Find(TEXT("3d"))->ModelsFor(TEXT("edit")).Num() > 0);
	TestEqual(TEXT("wav preset for sonilo"), ScenarioJson::GetString(Lanes.Presets(TEXT("model_sonilo-v1-1-text-to-sound-effects")), TEXT("audioFormat")), FString(TEXT("wav")));
	TestTrue(TEXT("restrict list"), Lanes.Restrict().Contains(TEXT("audioFormat")));
	TestFalse(TEXT("unreal-ready prompt text"), Lanes.UnrealReadyPrompt().IsEmpty());

	FScenarioLanes Broken;
	TestFalse(TEXT("bad JSON refused"), Broken.LoadFromString(TEXT("{")));
	TestFalse(TEXT("no lanes refused"), Broken.LoadFromString(TEXT("{\"lanes\":[]}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioSparkPromptsTest, "Scenario.Core.Schema.SparkPrompts", SCENARIO_TEST_FLAGS)
bool FScenarioSparkPromptsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Reply = ScenarioFixtures::LoadJson(TEXT("prompt_spark.json"));
	if (!TestTrue(TEXT("fixture"), Reply.IsValid()))
	{
		return false;
	}
	const TArray<FString> Prompts = FScenarioApi::SparkPrompts(Reply);
	TestEqual(TEXT("two variants from prompts"), Prompts.Num(), 2);
	TestTrue(TEXT("first is the recommended one"), Prompts.Num() > 0 && Prompts[0].StartsWith(TEXT("A serene")));

	// Without prompts: the calls, then the recommended prompt.
	TSharedPtr<FJsonObject> CallsOnly = ScenarioJson::ParseObject(TEXT("{\"calls\":[{\"parameters\":{\"prompt\":\"a\"}},{\"parameters\":{\"prompt\":\"b\"}}]}"));
	TestEqual(TEXT("from calls"), FScenarioApi::SparkPrompts(CallsOnly).Num(), 2);
	TSharedPtr<FJsonObject> Recommended = ScenarioJson::ParseObject(TEXT("{\"prompts\":[],\"recommended_prompt\":\"c\"}"));
	TestEqual(TEXT("from recommended_prompt"), FScenarioApi::SparkPrompts(Recommended).Num(), 1);
	TestEqual(TEXT("empty reply"), FScenarioApi::SparkPrompts(ScenarioJson::ParseObject(TEXT("{\"prompts\":[]}"))).Num(), 0);
	return true;
}

#endif
