// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Framework/Docking/TabManager.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "ScenarioActions.h"
#include "ScenarioAssets.h"
#include "ScenarioFakes.h"
#include "ScenarioJson.h"
#include "ScenarioLedger.h"
#include "ScenarioProvenance.h"
#include "ScopedTransaction.h"
#include "Tests/AutomationCommon.h"

namespace
{
	const TCHAR* TestFolder = TEXT("/Game/ScenarioTests");

	FString TestDir()
	{
		return FPaths::ProjectSavedDir() / TEXT("ScenarioTests");
	}

	/** A small RGBA PNG written to disk, so imports run without network or fixtures. */
	bool WritePng(const FString& Path, int32 Size)
	{
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
		TArray<FColor> Pixels;
		Pixels.SetNum(Size * Size);
		for (int32 Index = 0; Index < Pixels.Num(); ++Index)
		{
			Pixels[Index] = FColor(static_cast<uint8>(Index % 255), 128, 64, 255);
		}
		if (!Wrapper.IsValid() || !Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Size, Size, ERGBFormat::BGRA, 8))
		{
			return false;
		}
		const TArray64<uint8>& Bytes = Wrapper->GetCompressed();
		return FFileHelper::SaveArrayToFile(TArrayView<const uint8>(Bytes.GetData(), static_cast<int32>(Bytes.Num())), *Path);
	}

	void DeleteAssets(const TArray<UObject*>& Objects)
	{
		TArray<UObject*> Valid;
		for (UObject* Object : Objects)
		{
			if (IsValid(Object))
			{
				Valid.Add(Object);
			}
		}
		if (Valid.Num() > 0)
		{
			ObjectTools::ForceDeleteObjects(Valid, false);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioEditorRegistryTest, "Scenario.Editor.Actions.Registry", SCENARIO_TEST_FLAGS)
bool FScenarioEditorRegistryTest::RunTest(const FString& Parameters)
{
	const TArray<FScenarioAction>& Actions = FScenarioActions::Get().All();
	TestEqual(TEXT("39 actions"), Actions.Num(), 39);
	TSet<FString> Names;
	for (const FScenarioAction& Entry : Actions)
	{
		const FScenarioAction* Action = &Entry;
		TestFalse(FString::Printf(TEXT("%s is unique"), *Action->Name), Names.Contains(Action->Name));
		Names.Add(Action->Name);
		bool bSnake = !Action->Name.IsEmpty();
		for (const TCHAR Char : Action->Name)
		{
			bSnake &= FChar::IsLower(Char) || FChar::IsDigit(Char) || Char == TEXT('_');
		}
		TestTrue(FString::Printf(TEXT("%s is snake_case"), *Action->Name), bSnake);
		TestTrue(FString::Printf(TEXT("%s has an object schema"), *Action->Name),
			Action->InputSchema.IsValid() && ScenarioJson::GetString(Action->InputSchema, TEXT("type")) == TEXT("object"));
		TestTrue(FString::Printf(TEXT("%s has a description"), *Action->Name), Action->Description.Len() > 20);
	}
	for (const TCHAR* Paid : { TEXT("generate"), TEXT("run_workflow"), TEXT("approve_prompt") })
	{
		const FScenarioAction* Action = FScenarioActions::Get().Find(Paid);
		TestTrue(FString::Printf(TEXT("%s costs"), Paid), Action && Action->bCosts && Action->Kind == EScenarioActionKind::Spend);
	}
	for (const TCHAR* Free : { TEXT("estimate_cost"), TEXT("estimate_workflow"), TEXT("estimate_prompt"), TEXT("list_models") })
	{
		const FScenarioAction* Action = FScenarioActions::Get().Find(Free);
		TestTrue(FString::Printf(TEXT("%s is free"), Free), Action && !Action->bCosts);
	}
	const FScenarioAction* Remove = FScenarioActions::Get().Find(TEXT("remove_scenario_items"));
	TestTrue(TEXT("remove is destructive"), Remove && Remove->bDestructive && Remove->Kind == EScenarioActionKind::Change);
	const FScenarioAction* Python = FScenarioActions::Get().Find(TEXT("execute_python"));
	TestTrue(TEXT("python has its own kind"), Python && Python->Kind == EScenarioActionKind::Python);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioEditorTextureRolesTest, "Scenario.Editor.Import.TextureRoles", SCENARIO_TEST_FLAGS)
bool FScenarioEditorTextureRolesTest::RunTest(const FString& Parameters)
{
	auto Make = []() { return NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient); };

	UTexture2D* Normal = Make();
	ScenarioAssets::ConfigureTexture(Normal, TEXT("normal"), 0);
	TestFalse(TEXT("normal is linear"), Normal->SRGB);
	TestEqual(TEXT("normal compression"), static_cast<int32>(Normal->CompressionSettings), static_cast<int32>(TC_Normalmap));

	for (const TCHAR* Role : { TEXT("roughness"), TEXT("metallic"), TEXT("height"), TEXT("ao") })
	{
		UTexture2D* Mask = Make();
		ScenarioAssets::ConfigureTexture(Mask, Role, 0);
		TestFalse(FString::Printf(TEXT("%s is linear"), Role), Mask->SRGB);
		TestEqual(FString::Printf(TEXT("%s uses mask compression"), Role), static_cast<int32>(Mask->CompressionSettings), static_cast<int32>(TC_Masks));
	}

	UTexture2D* Albedo = Make();
	Albedo->SRGB = false;
	ScenarioAssets::ConfigureTexture(Albedo, TEXT("albedo"), 1024);
	TestTrue(TEXT("albedo is sRGB"), Albedo->SRGB);
	TestEqual(TEXT("max size applied"), Albedo->MaxTextureSize, 1024);

	UTexture2D* Panorama = Make();
	ScenarioAssets::ConfigureTexture(Panorama, TEXT("panorama"), 1024);
	TestEqual(TEXT("panorama keeps its size"), Panorama->MaxTextureSize, 0);
	TestEqual(TEXT("panorama has no mips"), static_cast<int32>(Panorama->MipGenSettings), static_cast<int32>(TMGS_NoMipmaps));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioEditorProvenanceTest, "Scenario.Editor.Provenance.RoundTrip", SCENARIO_TEST_FLAGS)
bool FScenarioEditorProvenanceTest::RunTest(const FString& Parameters)
{
	UTexture2D* Asset = NewObject<UTexture2D>(GetTransientPackage(), TEXT("T_ScenarioProvenanceTest"), RF_Transient);
	FScenarioJobRow Row;
	Row.LocalId = TEXT("local_1_1");
	Row.JobId = TEXT("job_test");
	Row.ModelId = TEXT("model_test");
	Row.Prompt = TEXT("a bronze shield");
	Row.QuotedExact = TEXT("4");
	FScenarioJobFile File;
	File.AssetId = TEXT("asset_test");

	const TMap<FName, FString> Values = ScenarioProvenance::Values(Row, File, false);
	ScenarioProvenance::Write(Asset, Values);
	const TMap<FName, FString> Read = ScenarioProvenance::Read(Asset);
	TestTrue(TEXT("metadata read back"), Read.Num() > 0);
	for (const TPair<FName, FString>& Pair : Values)
	{
		TestEqual(FString::Printf(TEXT("%s round trip"), *Pair.Key.ToString()), Read.FindRef(Pair.Key), Pair.Value);
	}
	bool bHasPrompt = false;
	for (const TPair<FName, FString>& Pair : ScenarioProvenance::Values(Row, File, true))
	{
		bHasPrompt |= Pair.Value.Contains(TEXT("bronze shield"));
	}
	TestFalse(TEXT("prompt left out when asked"), bHasPrompt);

	const FString IndexPath = TestDir() / TEXT("provenance-index.json");
	IFileManager::Get().Delete(*IndexPath, false, true, true);
	{
		FScenarioProvenanceIndex Index(IndexPath);
		Index.Add(TEXT("/Game/A.A"), Values);
		Index.Rename(TEXT("/Game/A.A"), TEXT("/Game/B.B"));
		Index.Save();
	}
	FScenarioProvenanceIndex Reloaded(IndexPath);
	Reloaded.Load();
	TestFalse(TEXT("old path gone after rename"), Reloaded.Contains(TEXT("/Game/A.A")));
	TestTrue(TEXT("new path kept"), Reloaded.Contains(TEXT("/Game/B.B")));
	TestEqual(TEXT("found by job"), Reloaded.ForJob(TEXT("job_test")).Num(), 1);
	Reloaded.Remove(TEXT("/Game/B.B"));
	TestFalse(TEXT("removed"), Reloaded.Contains(TEXT("/Game/B.B")));
	IFileManager::Get().Delete(*IndexPath, false, true, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioEditorStudioTabTest, "Scenario.Editor.Studio.Registered", SCENARIO_TEST_FLAGS)
bool FScenarioEditorStudioTabTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Studio tab spawner registered"), FGlobalTabmanager::Get()->HasTabSpawner(TEXT("ScenarioStudio")));
	return true;
}

/** Import of a PNG written on the fly, through the same path as a downloaded result, with no dialog. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioEditorImportPngTest, "Scenario.Editor.Import.Png", SCENARIO_TEST_FLAGS)
bool FScenarioEditorImportPngTest::RunTest(const FString& Parameters)
{
	const FString Png = TestDir() / TEXT("T_ScenarioImportTest.png");
	if (!TestTrue(TEXT("fixture written"), WritePng(Png, 64)))
	{
		return false;
	}
	struct FState
	{
		bool bDone = false;
		TArray<UObject*> Objects;
		FString Error;
	};
	const TSharedRef<FState> State = MakeShared<FState>();
	ScenarioAssets::ImportFile(Png, TestFolder, [State](const TArray<UObject*>& Objects, const FString& Error)
	{
		State->Objects = Objects;
		State->Error = Error;
		State->bDone = true;
	});
	ADD_LATENT_AUTOMATION_COMMAND(FUntilCommand([State]() { return State->bDone; }, []() { return true; }, 60.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([this, State, Png]()
	{
		TestTrue(TEXT("imported without error"), State->Error.IsEmpty());
		UTexture2D* Texture = nullptr;
		for (UObject* Object : State->Objects)
		{
			Texture = Texture ? Texture : Cast<UTexture2D>(Object);
		}
		TestNotNull(TEXT("a texture came out"), Texture);
		if (Texture)
		{
			TestTrue(TEXT("in the requested folder"), Texture->GetPathName().StartsWith(TestFolder));
		}
		DeleteAssets(State->Objects);
		IFileManager::Get().Delete(*Png, false, true, true);
	}, 0.0f));
	return true;
}

/** A placed mesh carries the Scenario tag and goes away with one undo. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioEditorPlaceUndoTest, "Scenario.Editor.Placement.Undo", SCENARIO_TEST_FLAGS)
bool FScenarioEditorPlaceUndoTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("engine cube"), Cube) || !TestNotNull(TEXT("editor"), GEditor) || !TestNotNull(TEXT("world"), ScenarioPlacement::EditorWorld()))
	{
		return false;
	}
	FScenarioPlaceOptions Options;
	Options.Placement = EScenarioPlacement::AtLocation;
	Options.Location = FVector(123.0, 456.0, 0.0);
	FScenarioChangeReport Report;
	AActor* Actor = nullptr;
	{
		const FScopedTransaction Transaction(NSLOCTEXT("ScenarioTests", "Place", "Scenario test placement"));
		Actor = ScenarioPlacement::SpawnMesh(Cube, Options, TEXT("ScenarioTestCube"), Report);
	}
	if (!TestNotNull(TEXT("actor spawned"), Actor))
	{
		return false;
	}
	TestTrue(TEXT("tagged Scenario"), Actor->Tags.Contains(ScenarioPlacement::ActorTag));
	TestEqual(TEXT("one actor reported"), Report.SpawnedActors.Num(), 1);
	const TWeakObjectPtr<AActor> Weak(Actor);
	GEditor->UndoTransaction();
	TestTrue(TEXT("one undo removes it"), !Weak.IsValid() || Weak->IsPendingKillPending() || !IsValid(Weak.Get()));
	return true;
}

#endif
