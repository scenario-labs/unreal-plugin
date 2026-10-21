// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

// Import policy (Interchange pipelines, mesh checks), smoothness to roughness,
// Substrate-aware masters, open worlds and the viewport exposure note.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DataDrivenShaderPlatformInfo.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "ImageCore.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialShared.h"
#include "Misc/App.h"
#include "Misc/EngineVersionComparison.h"
#include "PhysicsEngine/BodySetup.h"
#include "RenderUtils.h"
#include "ScenarioAssets.h"
#include "ScenarioEditorTestFixtures.h"
#include "ScenarioFakes.h"
#include "ScenarioOpenWorld.h"
#include "ScenarioSettings.h"
#include "ScenarioSubsystem.h"
#include "StaticMeshCompiler.h"
#include "Tests/AutomationCommon.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

using namespace ScenarioEditorTests;

namespace
{
	/** A Width x Height PNG: the left half white, the right half grey 128. */
	bool WriteSplitPng(const FString& Path, int32 Width, int32 Height)
	{
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
		TArray<FColor> Pixels;
		Pixels.SetNum(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				Pixels[Y * Width + X] = X < Width / 2 ? FColor(255, 255, 255, 255) : FColor(128, 128, 128, 255);
			}
		}
		if (!Wrapper.IsValid() || !Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		{
			return false;
		}
		const TArray64<uint8>& Bytes = Wrapper->GetCompressed();
		return FFileHelper::SaveArrayToFile(TArrayView<const uint8>(Bytes.GetData(), static_cast<int32>(Bytes.Num())), *Path);
	}

	/** Mip 0 of a texture's source as 8-bit BGRA, without a gamma change. */
	bool SourcePixels(UTexture2D* Texture, FImage& Out)
	{
		FImage Image;
		if (!Texture || !Texture->Source.GetMipImage(Image, 0, 0, 0))
		{
			return false;
		}
		Image.CopyTo(Out, ERawImageFormat::BGRA8, Image.GammaSpace);
		return true;
	}

	FString PropertyText(UObject* Root, const FString& Path)
	{
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("."));
		UObject* Owner = Root;
		for (int32 Index = 0; Owner && Index < Parts.Num(); ++Index)
		{
			const FProperty* Property = FindFProperty<FProperty>(Owner->GetClass(), *Parts[Index]);
			if (!Property)
			{
				return TEXT("<missing>");
			}
			const void* Address = Property->ContainerPtrToValuePtr<void>(Owner);
			if (Index + 1 < Parts.Num())
			{
				const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
				Owner = ObjectProperty ? ObjectProperty->GetObjectPropertyValue(Address) : nullptr;
				continue;
			}
			FString Text;
			Property->ExportTextItem_Direct(Text, Address, nullptr, Owner, PPF_None);
			return Text;
		}
		return TEXT("<missing>");
	}

	/**
	 * Compiles a material for the running shader platform and returns its
	 * errors (none when it compiled). False when this editor compiles no
	 * shaders (-nullrhi): run with SCENARIO_TEST_RENDER=1 to check them.
	 */
	bool CompileErrors(UMaterialInterface* Interface, FString& OutPlatform, TArray<FString>& OutErrors)
	{
		UMaterial* Material = Interface ? Interface->GetMaterial() : nullptr;
		if (!Material || !FApp::CanEverRender())
		{
			return false;
		}
		Material->ForceRecompileForRendering();
		OutPlatform = FDataDrivenShaderPlatformInfo::GetName(GMaxRHIShaderPlatform).ToString();
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		// Unverified on 5.6 and 5.7: those take the feature level here.
		FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIFeatureLevel);
#else
		FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
#endif
		if (!Resource)
		{
			return false;
		}
		Resource->FinishCompilation();
		OutErrors = Resource->GetCompileErrors();
		return true;
	}

	struct FImportState
	{
		bool bDone = false;
		TArray<UObject*> Objects;
		FString Error;
	};

	TSharedRef<FImportState> StartImport(const FString& File, const FString& Folder)
	{
		const TSharedRef<FImportState> State = MakeShared<FImportState>();
		ScenarioAssets::ImportFile(File, Folder, [State](const TArray<UObject*>& Objects, const FString& Error)
		{
			State->Objects = Objects;
			State->Error = Error;
			State->bDone = true;
		});
		return State;
	}
}

/** A smoothness map becomes a new roughness map (white to black, grey 128 to 127), the original is kept, and the instance samples the new one. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioSmoothnessTest, "Scenario.Editor.Import.SmoothnessToRoughness", SCENARIO_TEST_FLAGS)
bool FScenarioSmoothnessTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("smoothness has no PBR input of its own"), ScenarioAssets::PbrParameterForRole(TEXT("smoothness")), FName(NAME_None));
	TestEqual(TEXT("roughness input"), ScenarioAssets::PbrParameterForRole(TEXT("roughness")), FName(TEXT("Roughness")));

	FImage Tiny(2, 1, ERawImageFormat::G8, EGammaSpace::Linear);
	Tiny.AsG8()[0] = 255;
	Tiny.AsG8()[1] = 128;
	TestTrue(TEXT("G8 inverts"), ScenarioAssets::InvertImage(Tiny));
	TestEqual(TEXT("white becomes black"), static_cast<int32>(Tiny.AsG8()[0]), 0);
	TestEqual(TEXT("128 becomes 127"), static_cast<int32>(Tiny.AsG8()[1]), 127);
	FImage Float(1, 1, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
	Float.AsRGBA32F()[0] = FLinearColor(0.25f, 1.0f, 0.0f, 0.5f);
	ScenarioAssets::InvertImage(Float);
	TestTrue(TEXT("float inverts as 1 - v, alpha kept"), Float.AsRGBA32F()[0].Equals(FLinearColor(0.75f, 0.0f, 1.0f, 0.5f), 1e-4f));

	const FString Png = TestDir() / TEXT("T_ScenarioSmoothTest_S.png");
	if (!TestTrue(TEXT("fixture written"), WriteSplitPng(Png, 16, 8)))
	{
		return false;
	}
	const TSharedRef<FImportState> State = StartImport(Png, FString(TestFolder) / TEXT("Smoothness"));
	ADD_LATENT_AUTOMATION_COMMAND(FUntilCommand([State]() { return State->bDone; }, []() { return true; }, 60.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([this, State, Png]()
	{
		UTexture2D* Smoothness = nullptr;
		for (UObject* Object : State->Objects)
		{
			Smoothness = Smoothness ? Smoothness : Cast<UTexture2D>(Object);
		}
		TArray<UObject*> Cleanup = State->Objects;
		if (TestNotNull(TEXT("the smoothness map imported"), Smoothness))
		{
			ScenarioAssets::ConfigureTexture(Smoothness, TEXT("smoothness"), 0);
			TestFalse(TEXT("smoothness is read as data"), Smoothness->SRGB);

			FScenarioChangeReport Report;
			TMap<UTexture*, UTexture*> Derived;
			const TMap<FName, UTexture*> Maps = ScenarioAssets::PbrMaps({ { TEXT("smoothness"), Smoothness } }, true, 0, Report, Derived);
			UTexture2D* Roughness = Cast<UTexture2D>(Maps.FindRef(TEXT("Roughness")));
			if (TestNotNull(TEXT("a roughness map was made"), Roughness))
			{
				Cleanup.Add(Roughness);
				TestNotEqual(TEXT("it is a new texture"), static_cast<UTexture*>(Roughness), static_cast<UTexture*>(Smoothness));
				TestEqual(TEXT("derived from the smoothness map"), Derived.FindRef(Roughness), static_cast<UTexture*>(Smoothness));
				TestEqual(TEXT("named with the roughness suffix"), Roughness->GetName(), FString(TEXT("T_ScenarioSmoothTest_R")));
				TestFalse(TEXT("roughness is linear"), Roughness->SRGB);
				TestEqual(TEXT("roughness uses mask compression"), static_cast<int32>(Roughness->CompressionSettings), static_cast<int32>(TC_Masks));
				FImage Inverted;
				FImage Original;
				if (TestTrue(TEXT("roughness pixels read"), SourcePixels(Roughness, Inverted)) && TestTrue(TEXT("smoothness pixels read"), SourcePixels(Smoothness, Original)))
				{
					const FColor White = Inverted.AsBGRA8()[0];
					const FColor Grey = Inverted.AsBGRA8()[Inverted.SizeX - 1];
					TestTrue(FString::Printf(TEXT("white became black (got %d)"), White.R), White.R <= 1 && White.G <= 1 && White.B <= 1);
					TestTrue(FString::Printf(TEXT("grey 128 stays mid grey within one level (got %d)"), Grey.R), FMath::Abs(static_cast<int32>(Grey.R) - 127) <= 1);
					TestEqual(TEXT("the original is unchanged"), static_cast<int32>(Original.AsBGRA8()[0].R), 255);
				}
				FScenarioChangeReport MasterReport;
				UMaterialInstanceConstant* Instance = ScenarioAssets::CreateMaterialInstance(ScenarioAssets::Master(EScenarioMaster::Pbr, MasterReport),
					FString(TestFolder) / TEXT("Smoothness"), TEXT("MI_ScenarioSmoothTest"), Maps, {}, Report);
				if (TestNotNull(TEXT("instance made"), Instance))
				{
					Cleanup.Add(Instance);
					UTexture* Sampled = nullptr;
					Instance->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Roughness")), Sampled);
					TestEqual(TEXT("the instance samples the new roughness map"), Sampled, static_cast<UTexture*>(Roughness));
				}
				TestTrue(TEXT("the result says it inverted"), Report.Checks.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("Smoothness map inverted")); }));
				for (const FString& Path : MasterReport.CreatedAssets)
				{
					if (UObject* Made = FindObject<UObject>(nullptr, *Path))
					{
						Cleanup.Add(Made);
					}
				}
			}

			// With the setting off, the map goes to Roughness as it is.
			FScenarioChangeReport Kept;
			TMap<UTexture*, UTexture*> None;
			const TMap<FName, UTexture*> AsIs = ScenarioAssets::PbrMaps({ { TEXT("smoothness"), Smoothness } }, false, 0, Kept, None);
			TestEqual(TEXT("setting off: used as it is"), AsIs.FindRef(TEXT("Roughness")), static_cast<UTexture*>(Smoothness));
			TestEqual(TEXT("setting off: nothing new"), None.Num(), 0);
		}
		DeleteAssets(Cleanup);
		IFileManager::Get().Delete(*Png, false, true, true);
	}, 0.0f));
	return true;
}

/** The PBR master matches the running renderer, and both graphs compile; unlit and sky compile too. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioSubstrateTest, "Scenario.Editor.Materials.Substrate", SCENARIO_TEST_FLAGS)
bool FScenarioSubstrateTest::RunTest(const FString& Parameters)
{
	const ScenarioAssets::FMaterialVariant Variant = ScenarioAssets::MaterialVariant();
	TestEqual(TEXT("variant follows r.Substrate"), Variant.bSubstrate, Substrate::IsSubstrateEnabled());
	TestEqual(TEXT("master name"), Variant.MasterName, FString(Variant.bSubstrate ? TEXT("M_Scenario_PBR_Substrate") : TEXT("M_Scenario_PBR")));
	TestTrue(TEXT("the result line names the master"), Variant.Describe().Contains(Variant.MasterName));
	TestTrue(TEXT("the result line names the mode"), Variant.Describe().Contains(Variant.bSubstrate ? TEXT("Substrate slab") : TEXT("legacy PBR")));
	AddInfo(FString::Printf(TEXT("r.Substrate %d, project setting %d: %s"), Variant.bSubstrate ? 1 : 0, Variant.bProjectSetting ? 1 : 0, *Variant.Describe()));

	FScenarioChangeReport Report;
	TArray<UObject*> Made;
	UMaterialInterface* Running = ScenarioAssets::Master(EScenarioMaster::Pbr, Report);
	if (!TestNotNull(TEXT("master for the running renderer"), Running))
	{
		return false;
	}
	TestEqual(TEXT("Master(Pbr) is the variant's master"), Running->GetName(), Variant.MasterName);
	for (const bool bSubstrate : { false, true })
	{
		UMaterialInterface* Master = ScenarioAssets::PbrMaster(bSubstrate, Report);
		if (!TestNotNull(TEXT("master built"), Master))
		{
			continue;
		}
		const UMaterial* Material = Master->GetMaterial();
		const bool bHasFrontMaterial = Material && Material->GetEditorOnlyData()->FrontMaterial.IsConnected();
		TestEqual(FString::Printf(TEXT("%s: Front Material wired only in the Substrate graph"), *Master->GetName()), bHasFrontMaterial, bSubstrate);
		TestEqual(FString::Printf(TEXT("%s: Base Color wired only in the legacy graph"), *Master->GetName()),
			Material && Material->GetEditorOnlyData()->BaseColor.IsConnected(), !bSubstrate);
		FString Platform;
		TArray<FString> Errors;
		if (!CompileErrors(Master, Platform, Errors))
		{
			AddInfo(FString::Printf(TEXT("%s: graph checked; shaders are not compiled in this editor (-nullrhi). SCENARIO_TEST_RENDER=1 compiles them."), *Master->GetName()));
			continue;
		}
		if (bSubstrate == Variant.bSubstrate)
		{
			TestEqual(FString::Printf(TEXT("%s compiles without errors on %s"), *Master->GetName(), *Platform), Errors.Num(), 0);
			for (const FString& Error : Errors)
			{
				AddError(Error);
			}
			AddInfo(FString::Printf(TEXT("%s (the running variant) on %s: %d compile errors"), *Master->GetName(), *Platform, Errors.Num()));
		}
		else
		{
			AddInfo(FString::Printf(TEXT("%s (the other variant) on %s: %d compile errors%s"), *Master->GetName(), *Platform, Errors.Num(),
				Errors.Num() > 0 ? *(TEXT(": ") + Errors[0]) : TEXT("")));
		}
	}
	for (const EScenarioMaster Which : { EScenarioMaster::Unlit, EScenarioMaster::SkySphere })
	{
		UMaterialInterface* Master = ScenarioAssets::Master(Which, Report);
		FString Platform;
		TArray<FString> Errors;
		if (TestNotNull(TEXT("unlit master built"), Master) && CompileErrors(Master, Platform, Errors))
		{
			TestEqual(FString::Printf(TEXT("%s compiles without errors on %s"), *Master->GetName(), *Platform), Errors.Num(), 0);
			for (const FString& Error : Errors)
			{
				AddError(Error);
			}
		}
	}
	// Remove what this test created (a project that already had the masters keeps them).
	for (const FString& Path : Report.CreatedAssets)
	{
		if (UObject* Object = FindObject<UObject>(nullptr, *Path))
		{
			Made.Add(Object);
		}
	}
	DeleteAssets(Made);
	return true;
}

/** The plugin's pipelines: Nanite and collision from Project Settings, lightmap UVs off, scale 1, materials and textures on. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioPipelinesTest, "Scenario.Editor.Import.Pipelines", SCENARIO_TEST_FLAGS)
bool FScenarioPipelinesTest::RunTest(const FString& Parameters)
{
	TArray<FString> Notes;
	const TArray<UObject*> Gltf = ScenarioAssets::ImportPipelines(TEXT("/tmp/a.glb"), Notes);
	TestEqual(TEXT("glTF: assets pipeline plus Epic's glTF pipeline"), Gltf.Num(), 2);
	TestEqual(TEXT("every policy property exists in this engine"), Notes.Num(), 0);
	for (const FString& Note : Notes)
	{
		AddWarning(Note);
	}
	if (Gltf.Num() > 0)
	{
		UObject* Assets = Gltf[0];
		TestTrue(TEXT("a copy, not the engine asset"), Assets->GetOutermost() == GetTransientPackage());
		const UScenarioProjectSettings& Settings = UScenarioProjectSettings::Get();
		TestEqual(TEXT("Nanite per Project Settings"), PropertyText(Assets, TEXT("MeshPipeline.bBuildNanite")), FString(Settings.bNanite ? TEXT("True") : TEXT("False")));
		TestEqual(TEXT("no lightmap UVs"), PropertyText(Assets, TEXT("MeshPipeline.bGenerateLightmapUVs")), FString(TEXT("False")));
		TestEqual(TEXT("simple box collision"), PropertyText(Assets, TEXT("MeshPipeline.Collision")), FString(TEXT("Box")));
		TestEqual(TEXT("scale 1"), PropertyText(Assets, TEXT("ImportOffsetUniformScale")), FString(TEXT("1.000000")));
		TestEqual(TEXT("materials imported"), PropertyText(Assets, TEXT("MaterialPipeline.bImportMaterials")), FString(TEXT("True")));
		TestEqual(TEXT("textures imported"), PropertyText(Assets, TEXT("MaterialPipeline.TexturePipeline.bImportTextures")), FString(TEXT("True")));
	}
	TestEqual(TEXT("images get one texture pipeline"), ScenarioAssets::ImportPipelines(TEXT("/tmp/a.png"), Notes).Num(), 1);
	TestEqual(TEXT("FBX gets one assets pipeline"), ScenarioAssets::ImportPipelines(TEXT("/tmp/a.fbx"), Notes).Num(), 1);
	TestEqual(TEXT("audio keeps the project's stack"), ScenarioAssets::ImportPipelines(TEXT("/tmp/a.wav"), Notes).Num(), 0);
	TestFalse(TEXT("an unknown property path is refused"), ScenarioAssets::SetPropertyByPath(Gltf.Num() > 0 ? Gltf[0] : nullptr, TEXT("MeshPipeline.bNoSuchThing"), TEXT("True")));
	return true;
}

/** A 1 m glTF cube through the policy: 100 cm, simple collision, Nanite only while opaque, texture color spaces per role. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioMeshPolicyTest, "Scenario.Editor.Import.MeshPolicy", SCENARIO_TEST_FLAGS)
bool FScenarioMeshPolicyTest::RunTest(const FString& Parameters)
{
	const FString Glb = TestDir() / TEXT("SM_ScenarioPolicyTest.glb");
	if (!TestTrue(TEXT("fixture written"), WriteGlb(Glb)))
	{
		return false;
	}
	UScenarioProjectSettings* Settings = GetMutableDefault<UScenarioProjectSettings>();
	const EScenarioMeshCollision CollisionBefore = Settings->MeshCollision;
	const bool bNaniteBefore = Settings->bNanite;
	Settings->MeshCollision = EScenarioMeshCollision::ComplexAsSimple;
	Settings->bNanite = true;
	const TSharedRef<FImportState> State = StartImport(Glb, FString(TestFolder) / TEXT("Policy"));
	ADD_LATENT_AUTOMATION_COMMAND(FUntilCommand([State]() { return State->bDone; }, []() { return true; }, 120.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([this, State, Glb, Settings, CollisionBefore, bNaniteBefore]()
	{
		TestTrue(TEXT("imported without error"), State->Error.IsEmpty());
		UStaticMesh* Mesh = nullptr;
		for (UObject* Object : State->Objects)
		{
			Mesh = Mesh ? Mesh : Cast<UStaticMesh>(Object);
		}
		if (TestNotNull(TEXT("a static mesh came out"), Mesh))
		{
			for (UObject* Object : State->Objects)
			{
				if (UTexture* Texture = Cast<UTexture>(Object))
				{
					ScenarioAssets::ConfigureTexture(Texture, FString(), 0);
				}
			}
			ScenarioAssets::FixMeshTextures(State->Objects);
			TArray<FString> Checks;
			ScenarioAssets::ConfigureStaticMesh(Mesh, &Checks);
			FStaticMeshCompilingManager::Get().FinishAllCompilation();
			ScenarioAssets::CheckMesh(Mesh, Checks, 100.0);
			for (const FString& Line : Checks)
			{
				AddInfo(Line);
			}

			const FVector Size = Mesh->GetBoundingBox().GetSize();
			TestTrue(FString::Printf(TEXT("1 m in glTF is 100 cm (got %.1f x %.1f x %.1f)"), Size.X, Size.Y, Size.Z), Size.Equals(FVector(100.0), 1.0));
			TestTrue(TEXT("size check passes against the requested 100 cm"), Checks.Contains(TEXT("Size matches the requested 100 cm.")));
			const UBodySetup* Body = Mesh->GetBodySetup();
			TestTrue(TEXT("simple collision present"), Body && Body->AggGeom.GetElementCount() > 0);
			TestTrue(TEXT("complex as simple for queries"), Body && Body->CollisionTraceFlag == CTF_UseComplexAsSimple);
			TestTrue(TEXT("Nanite on an opaque mesh"), Mesh->IsNaniteEnabled());
			TestTrue(TEXT("no generated lightmap UVs"), Mesh->GetNumSourceModels() > 0 && !Mesh->GetSourceModel(0).BuildSettings.bGenerateLightmapUVs);
			TestTrue(TEXT("Nanite line in the result"), Checks.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("Nanite: on")); }));
			TestTrue(TEXT("collision line in the result"), Checks.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("Collision: ")); }));

			bool bSawBase = false;
			bool bSawMask = false;
			bool bSawNormal = false;
			if (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Mesh->GetMaterial(0)))
			{
				for (const FTextureParameterValue& Value : Instance->TextureParameterValues)
				{
					const FString Name = Value.ParameterInfo.Name.ToString();
					const UTexture* Texture = Value.ParameterValue;
					if (!Texture)
					{
						continue;
					}
					if (Name.Contains(TEXT("BaseColor")))
					{
						bSawBase = true;
						TestTrue(TEXT("albedo is sRGB"), Texture->SRGB);
					}
					else if (Name.Contains(TEXT("MetallicRoughness")))
					{
						bSawMask = true;
						TestFalse(TEXT("metallic-roughness (ORM) is linear"), Texture->SRGB);
					}
					else if (Name.Contains(TEXT("Normal")))
					{
						bSawNormal = true;
						TestFalse(TEXT("normal is linear"), Texture->SRGB);
						TestEqual(TEXT("normal compression"), static_cast<int32>(Texture->CompressionSettings), static_cast<int32>(TC_Normalmap));
					}
				}
			}
			TestTrue(TEXT("base color map seen"), bSawBase);
			TestTrue(TEXT("metallic-roughness map seen"), bSawMask);
			TestTrue(TEXT("normal map seen"), bSawNormal);

			// A translucent slot: Nanite goes off and the result says why.
			UMaterial* Glass = NewObject<UMaterial>(GetTransientPackage(), TEXT("M_ScenarioTestGlass"), RF_Transient);
			Glass->BlendMode = BLEND_Translucent;
			TArray<FStaticMaterial> Slots = Mesh->GetStaticMaterials();
			if (TestTrue(TEXT("the mesh has a slot"), Slots.Num() > 0))
			{
				Slots[0].MaterialInterface = Glass;
				Mesh->SetStaticMaterials(Slots);
				TArray<FString> GlassChecks;
				ScenarioAssets::ConfigureStaticMesh(Mesh, &GlassChecks);
				FStaticMeshCompilingManager::Get().FinishAllCompilation();
				TestFalse(TEXT("no Nanite with a translucent slot"), Mesh->IsNaniteEnabled());
				TestTrue(TEXT("the result says why"), GlassChecks.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("Nanite: off, slot 0")); }));
			}
		}
		Settings->MeshCollision = CollisionBefore;
		Settings->bNanite = bNaniteBefore;
		DeleteAssets(State->Objects);
		IFileManager::Get().Delete(*Glb, false, true, true);
	}, 0.0f));
	return true;
}

/** scene_summary and placement text for open worlds, from plain states (no level needed). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioOpenWorldTextTest, "Scenario.Editor.OpenWorld.Notes", SCENARIO_TEST_FLAGS)
bool FScenarioOpenWorldTextTest::RunTest(const FString& Parameters)
{
	ScenarioOpenWorld::FState Plain;
	TestEqual(TEXT("a plain level has no notes"), ScenarioOpenWorld::SummaryNotes(Plain).Num(), 0);
	TestTrue(TEXT("a plain level has no placement note"), ScenarioOpenWorld::PlacementNote(Plain).IsEmpty());
	TArray<FString> Checks;
	TArray<FString> Warnings;
	ScenarioOpenWorld::DescribePlacedActor(Plain, false, {}, Checks, Warnings);
	TestEqual(TEXT("nothing said for a plain level"), Checks.Num() + Warnings.Num(), 0);

	ScenarioOpenWorld::FState Open;
	Open.bWorldPartition = true;
	Open.bExternalActors = true;
	Open.LoadedActors = 12;
	Open.TotalActors = 480;
	Open.DataLayers = { { TEXT("Props"), true, true, false, true }, { TEXT("Night"), false, false, false, false }, { TEXT("Locked"), true, true, true, false } };
	const TArray<FString> Notes = ScenarioOpenWorld::SummaryNotes(Open);
	TestTrue(TEXT("states the loaded-only limit with counts"), Notes.Num() > 0 && Notes[0].Contains(TEXT("only loaded actors are listed (12 of 480)")));
	TestTrue(TEXT("states one file per actor"), Notes.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("One file per actor")); }));
	TestTrue(TEXT("names the current data layers"), Notes.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("current data layers: Props")); }));
	TestTrue(TEXT("names unloaded data layers"), Notes.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("not loaded in the editor")) && Line.Contains(TEXT("Night")); }));
	TestEqual(TEXT("placement note"), ScenarioOpenWorld::PlacementNote(Open),
		FString(TEXT("World Partition level: new actors go into the current data layers (Props) and are saved in their own files (one file per actor).")));

	Open.TotalActors = -1;
	TestTrue(TEXT("unknown total still states the limit"), ScenarioOpenWorld::SummaryNotes(Open)[0].StartsWith(TEXT("World Partition level: only loaded actors are listed.")));

	ScenarioOpenWorld::DescribePlacedActor(Open, true, { TEXT("Props") }, Checks, Warnings);
	TestTrue(TEXT("own file reported"), Checks.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("Saved in its own actor file")); }));
	TestTrue(TEXT("data layer reported"), Checks.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("Data layers: Props")); }));
	TestEqual(TEXT("a loaded, visible, writable layer gives no warning"), Warnings.Num(), 0);
	ScenarioOpenWorld::DescribePlacedActor(Open, true, { TEXT("Night"), TEXT("Locked") }, Checks, Warnings);
	TestTrue(TEXT("hidden layer warned"), Warnings.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("Night is not loaded")); }));
	TestTrue(TEXT("read-only layer warned"), Warnings.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("Locked is read-only")); }));

	const TSharedRef<FJsonObject> Json = ScenarioOpenWorld::ToJson(Open);
	TestTrue(TEXT("JSON says World Partition"), Json->GetBoolField(TEXT("world_partition")));
	TestEqual(TEXT("JSON lists the data layers"), Json->GetArrayField(TEXT("data_layers")).Num(), 3);
	return true;
}

/** A World Partition world made headless: its state reads as open, and an actor placed in it lands in its own file without a crash. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioOpenWorldLiveTest, "Scenario.Editor.OpenWorld.WorldPartition", SCENARIO_TEST_FLAGS)
bool FScenarioOpenWorldLiveTest::RunTest(const FString& Parameters)
{
	UPackage* Package = CreatePackage(TEXT("/Game/ScenarioTests/OpenWorld/WP_ScenarioTest"));
	UWorld::InitializationValues Values;
	Values.RequiresHitProxies(false);
	Values.ShouldSimulatePhysics(false);
	Values.EnableTraceCollision(false);
	Values.CreateNavigation(false);
	Values.CreateAISystem(false);
	Values.AllowAudioPlayback(false);
	Values.CreatePhysicsScene(false);
	Values.CreateWorldPartition(true);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("WP_ScenarioTest"), Package, false, ERHIFeatureLevel::Num, &Values);
	if (!TestNotNull(TEXT("a World Partition world was created"), World))
	{
		return false;
	}
	const ScenarioOpenWorld::FState State = ScenarioOpenWorld::Read(World);
	TestTrue(TEXT("reads as World Partition"), State.bWorldPartition);
	TestTrue(TEXT("uses one file per actor"), State.bExternalActors);
	AddInfo(FString::Printf(TEXT("loaded %d, on disk %d, data layers %d"), State.LoadedActors, State.TotalActors, State.DataLayers.Num()));
	TestTrue(TEXT("summary states the loaded-only limit"), ScenarioOpenWorld::SummaryNotes(State).Num() > 0
		&& ScenarioOpenWorld::SummaryNotes(State)[0].Contains(TEXT("only loaded actors are listed")));

	FActorSpawnParameters Spawn;
	Spawn.OverrideLevel = World->PersistentLevel;
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	if (TestNotNull(TEXT("an actor spawns in the open world"), Actor))
	{
		TArray<FString> Checks;
		TArray<FString> Warnings;
		ScenarioOpenWorld::DescribePlacedActor(Actor, Checks, Warnings);
		TestTrue(TEXT("the actor has its own package"), Actor->IsPackageExternal());
		TestTrue(TEXT("the placement says so"), Checks.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("Saved in its own actor file")); }));
		for (const FString& Line : Checks)
		{
			AddInfo(Line);
		}
	}
	World->DestroyWorld(false);
	return true;
}

/** A capture of the editor world comes back with a frame check over all its pixels (needs a rendering RHI: SCENARIO_TEST_RENDER=1). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioCaptureCheckTest, "Scenario.Editor.Capture.FrameCheck", SCENARIO_TEST_FLAGS)
bool FScenarioCaptureCheckTest::RunTest(const FString& Parameters)
{
	UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return false;
	}
	if (!FApp::CanEverRender())
	{
		AddInfo(TEXT("This editor renders nothing (-nullrhi): the capture is checked with SCENARIO_TEST_RENDER=1; the frame check itself is Scenario.Core.Images.FrameCheck."));
		return true;
	}
	FScenarioCaptureCamera Camera;
	Camera.Location = FVector(-600.0, 0.0, 300.0);
	Camera.Rotation = FRotator(-20.0, 0.0, 0.0);
	Camera.Fov = 70.0f;
	FString Error;
	FString Description;
	FScenarioFrameCheck Check;
	const FString Path = Subsystem->CaptureViewport(320, 180, Error, &Description, &Camera, &Check);
	if (!TestFalse(FString::Printf(TEXT("captured (%s)"), *Error), Path.IsEmpty()))
	{
		return false;
	}
	TestEqual(TEXT("every pixel checked"), Check.Pixels, 320 * 180);
	const TSharedRef<FJsonObject> Json = Check.ToJson();
	AddInfo(FString::Printf(TEXT("%s: verdict %s, mean luma %.1f, clipped %.1f%%, crushed %.1f%%%s"), *Description, *Json->GetStringField(TEXT("verdict")), Check.MeanLuma,
		Check.ClippedShare * 100.0, Check.CrushedShare * 100.0, Check.Issues().Num() > 0 ? *(TEXT(", ") + Check.Issues()[0]) : TEXT("")));
	IFileManager::Get().Delete(*Path, false, true, true);
	return true;
}

/** The exposure note an agent gets with a screenshot when the viewport's exposure is fixed. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenarioExposureNoteTest, "Scenario.Editor.Capture.ExposureNote", SCENARIO_TEST_FLAGS)
bool FScenarioExposureNoteTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("no note without an override"), ScenarioPlacement::ExposureNote(false, 3.0f).IsEmpty());
	const FString Note = ScenarioPlacement::ExposureNote(true, 3.0f);
	TestTrue(TEXT("names the EV100"), Note.Contains(TEXT("EV100 3.0")));
	TestTrue(TEXT("says it bypasses the post process volume"), Note.Contains(TEXT("bypasses the post process volume")));
	// Headless editors have no level viewport: the live read must say nothing and not fail.
	const FString Live = ScenarioPlacement::ExposureOverrideNote();
	AddInfo(Live.IsEmpty() ? TEXT("No viewport exposure override in this editor.") : Live);
	return true;
}

#endif
