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
#include "Materials/MaterialInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "ScenarioActions.h"
#include "ScenarioAssets.h"
#include "ScenarioCreateModel.h"
#include "ScenarioEditorModule.h"
#include "ScenarioFakes.h"
#include "ScenarioJson.h"
#include "ScenarioLedger.h"
#include "ScenarioProvenance.h"
#include "ScenarioSettings.h"
#include "ScopedTransaction.h"
#include "StaticMeshCompiler.h"
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

	/** PNG bytes of a Size x Size image of one color. */
	TArray64<uint8> PngOf(const FColor& Color, int32 Size)
	{
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
		TArray<FColor> Pixels;
		Pixels.Init(Color, Size * Size);
		if (!Wrapper.IsValid() || !Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Size, Size, ERGBFormat::BGRA, 8))
		{
			return {};
		}
		return Wrapper->GetCompressed();
	}

	/**
	 * A 1 m glTF cube with base color, metallic-roughness and normal maps, the
	 * shape of a 3D model from Scenario, written here so the test needs no file.
	 */
	bool WriteGlb(const FString& Path)
	{
		TArray<FVector3f> Positions;
		TArray<FVector3f> Normals;
		TArray<FVector2f> Uvs;
		TArray<uint16> Indices;
		const FVector3f Faces[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
		const FVector2f Corners[4] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
		for (const FVector3f& Normal : Faces)
		{
			// Tangent x Bitangent = Normal, so the corners run counterclockwise seen from outside.
			const FVector3f Tangent = FMath::Abs(Normal.Z) > 0.5f ? FVector3f(1, 0, 0) : FVector3f(0, 0, 1);
			const FVector3f Bitangent = FVector3f::CrossProduct(Normal, Tangent);
			const uint16 First = static_cast<uint16>(Positions.Num());
			for (const FVector2f& Corner : Corners)
			{
				Positions.Add((Normal + Tangent * Corner.X + Bitangent * Corner.Y) * 0.5f);
				Normals.Add(Normal);
				Uvs.Add(FVector2f((Corner.X + 1.0f) * 0.5f, (Corner.Y + 1.0f) * 0.5f));
			}
			for (const int32 Offset : { 0, 1, 2, 0, 2, 3 })
			{
				Indices.Add(static_cast<uint16>(First + Offset));
			}
		}

		TArray<uint8> Bin;
		TArray<FString> Views;
		auto AddView = [&Bin, &Views](const void* Data, int64 Size)
		{
			Views.Add(FString::Printf(TEXT("{\"buffer\":0,\"byteOffset\":%d,\"byteLength\":%lld}"), Bin.Num(), Size));
			Bin.Append(static_cast<const uint8*>(Data), static_cast<int32>(Size));
			while (Bin.Num() % 4 != 0)
			{
				Bin.Add(0);
			}
		};
		AddView(Positions.GetData(), Positions.Num() * sizeof(FVector3f));
		AddView(Normals.GetData(), Normals.Num() * sizeof(FVector3f));
		AddView(Uvs.GetData(), Uvs.Num() * sizeof(FVector2f));
		AddView(Indices.GetData(), Indices.Num() * sizeof(uint16));
		// Base color, metallic-roughness (G roughness, B metallic) and a flat normal.
		for (const FColor& Color : { FColor(180, 120, 60), FColor(0, 200, 30), FColor(128, 128, 255) })
		{
			const TArray64<uint8> Png = PngOf(Color, 16);
			if (Png.Num() == 0)
			{
				return false;
			}
			AddView(Png.GetData(), Png.Num());
		}

		FString Json = FString::Printf(TEXT("{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],")
			TEXT("\"nodes\":[{\"mesh\":0,\"name\":\"ScenarioTestCube\"}],")
			TEXT("\"meshes\":[{\"name\":\"ScenarioTestCube\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],")
			TEXT("\"materials\":[{\"name\":\"ScenarioTestCubeMaterial\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicRoughnessTexture\":{\"index\":1}},\"normalTexture\":{\"index\":2}}],")
			TEXT("\"textures\":[{\"source\":0},{\"source\":1},{\"source\":2}],")
			TEXT("\"images\":[{\"bufferView\":4,\"mimeType\":\"image/png\"},{\"bufferView\":5,\"mimeType\":\"image/png\"},{\"bufferView\":6,\"mimeType\":\"image/png\"}],")
			TEXT("\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\",\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},")
			TEXT("{\"bufferView\":1,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\"},{\"bufferView\":2,\"componentType\":5126,\"count\":%d,\"type\":\"VEC2\"},")
			TEXT("{\"bufferView\":3,\"componentType\":5123,\"count\":%d,\"type\":\"SCALAR\"}],")
			TEXT("\"bufferViews\":[%s],\"buffers\":[{\"byteLength\":%d}]}"),
			Positions.Num(), Normals.Num(), Uvs.Num(), Indices.Num(), *FString::Join(Views, TEXT(",")), Bin.Num());
		FTCHARToUTF8 Utf8(*Json);
		TArray<uint8> JsonBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		while (JsonBytes.Num() % 4 != 0)
		{
			JsonBytes.Add(' ');
		}

		TArray<uint8> Glb;
		auto Word = [&Glb](uint32 Value) { Glb.Append(reinterpret_cast<const uint8*>(&Value), 4); };
		Word(0x46546C67); // "glTF"
		Word(2);
		Word(12 + 8 + JsonBytes.Num() + 8 + Bin.Num());
		Word(JsonBytes.Num());
		Word(0x4E4F534A); // "JSON"
		Glb.Append(JsonBytes);
		Word(Bin.Num());
		Word(0x004E4942); // "BIN"
		Glb.Append(Bin);
		return FFileHelper::SaveArrayToFile(Glb, *Path);
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
	TestEqual(TEXT("39 built-in actions and 9 kit actions"), Actions.Num(), 48);
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

	// Mesh imports pass no role: the importer's choice per material slot stays.
	UTexture2D* Kept = Make();
	Kept->SRGB = false;
	ScenarioAssets::ConfigureTexture(Kept, FString(), 0);
	TestFalse(TEXT("an empty role keeps the importer's sRGB"), Kept->SRGB);

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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioEditorStartedTest, "Scenario.Editor.Module.Started", SCENARIO_TEST_FLAGS)
bool FScenarioEditorStartedTest::RunTest(const FString& Parameters)
{
	// The module starts on a tick after load; by the time tests run it has.
	TestTrue(TEXT("editor module started"), FScenarioEditorModule::Get().IsStarted());
	const TSharedRef<FScenarioCreateModel> Create = FScenarioCreateModel::Get();
	TestTrue(TEXT("Create opens on a model"), Create->IsWorkflow() || !Create->GetModelId().IsEmpty());
	return true;
}

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

/** A glTF model: masks end linear, base color stays sRGB, and each collision setting holds after the build. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioEditorImportMeshTest, "Scenario.Editor.Import.Mesh", SCENARIO_TEST_FLAGS)
bool FScenarioEditorImportMeshTest::RunTest(const FString& Parameters)
{
	const FString Glb = TestDir() / TEXT("SM_ScenarioMeshTest.glb");
	if (!TestTrue(TEXT("fixture written"), WriteGlb(Glb)))
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
	ScenarioAssets::ImportFile(Glb, FString(TestFolder) / TEXT("Mesh"), [State](const TArray<UObject*>& Objects, const FString& Error)
	{
		State->Objects = Objects;
		State->Error = Error;
		State->bDone = true;
	});
	ADD_LATENT_AUTOMATION_COMMAND(FUntilCommand([State]() { return State->bDone; }, []() { return true; }, 120.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([this, State, Glb]()
	{
		TestTrue(TEXT("imported without error"), State->Error.IsEmpty());
		UStaticMesh* Mesh = nullptr;
		for (UObject* Object : State->Objects)
		{
			Mesh = Mesh ? Mesh : Cast<UStaticMesh>(Object);
		}
		if (TestNotNull(TEXT("a static mesh came out"), Mesh))
		{
			// The subsystem's order: textures keep the importer's color settings, then the masks turn linear.
			for (UObject* Object : State->Objects)
			{
				if (UTexture* Texture = Cast<UTexture>(Object))
				{
					ScenarioAssets::ConfigureTexture(Texture, FString(), 0);
				}
			}
			ScenarioAssets::FixMeshTextures(State->Objects);
			const UMaterialInstance* Instance = Cast<UMaterialInstance>(Mesh->GetMaterial(0));
			bool bSawMask = false;
			bool bSawBase = false;
			if (TestNotNull(TEXT("the mesh uses a material instance"), Instance))
			{
				for (const FTextureParameterValue& Value : Instance->TextureParameterValues)
				{
					const FString Name = Value.ParameterInfo.Name.ToString();
					if (!Value.ParameterValue)
					{
						continue;
					}
					if (Name.Contains(TEXT("MetallicRoughness")))
					{
						bSawMask = true;
						TestFalse(TEXT("metallic-roughness map is linear"), Value.ParameterValue->SRGB);
					}
					else if (Name.Contains(TEXT("BaseColor")))
					{
						bSawBase = true;
						TestTrue(TEXT("base color map is sRGB"), Value.ParameterValue->SRGB);
					}
				}
			}
			TestTrue(TEXT("a metallic-roughness parameter was set"), bSawMask);
			TestTrue(TEXT("a base color parameter was set"), bSawBase);

			UScenarioProjectSettings* Settings = GetMutableDefault<UScenarioProjectSettings>();
			const EScenarioMeshCollision Before = Settings->MeshCollision;
			const UBodySetup* Imported = Mesh->GetBodySetup();
			AddInfo(FString::Printf(TEXT("Interchange left %d simple shapes."), Imported ? Imported->AggGeom.GetElementCount() : -1));

			Settings->MeshCollision = EScenarioMeshCollision::Simple;
			ScenarioAssets::ConfigureStaticMesh(Mesh);
			FStaticMeshCompilingManager::Get().FinishAllCompilation();
			const UBodySetup* Body = Mesh->GetBodySetup();
			TestTrue(TEXT("Simple box: the body has a simple shape after the build"), Body && Body->AggGeom.GetElementCount() > 0);
			TestTrue(TEXT("Simple box: default trace"), Body && Body->CollisionTraceFlag == CTF_UseDefault);

			Settings->MeshCollision = EScenarioMeshCollision::ComplexAsSimple;
			ScenarioAssets::ConfigureStaticMesh(Mesh);
			FStaticMeshCompilingManager::Get().FinishAllCompilation();
			Body = Mesh->GetBodySetup();
			TestTrue(TEXT("Mesh shape: complex as simple after the build"), Body && Body->CollisionTraceFlag == CTF_UseComplexAsSimple);
			Settings->MeshCollision = Before;
		}
		DeleteAssets(State->Objects);
		IFileManager::Get().Delete(*Glb, false, true, true);
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
