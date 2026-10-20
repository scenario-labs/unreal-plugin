// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioAssets.h"

#include "Animation/MorphTarget.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/RendererSettings.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "FileHelpers.h"
#include "FileMediaSource.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "ImageCore.h"
#include "InterchangeManager.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionObjectPositionWS.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "ReferenceSkeleton.h"
#include "RenderUtils.h"
#include "ScenarioCoreModule.h"
#include "ScenarioFiles.h"
#include "ScenarioSettings.h"
#include "ScenarioSubsystem.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
	IAssetTools& Tools()
	{
		return FAssetToolsModule::GetModule().Get();
	}

	FString SharedPath()
	{
		return UScenarioProjectSettings::Get().RootPath() / TEXT("_Shared");
	}

	template <typename T>
	T* LoadAsset(const FString& PackagePath, const FString& Name)
	{
		return LoadObject<T>(nullptr, *(PackagePath / Name + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** A 4x4 linear (non-sRGB) mask texture: white for roughness and AO defaults, black for metallic. */
	UTexture2D* SolidMask(const FString& Name, uint8 Value, FScenarioChangeReport& Report)
	{
		if (UTexture2D* Existing = LoadAsset<UTexture2D>(SharedPath(), Name))
		{
			return Existing;
		}
		UPackage* Package = CreatePackage(*(SharedPath() / Name));
		UTexture2D* Texture = NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		TArray<uint8> Pixels;
		Pixels.Init(Value, 4 * 4 * 4);
		for (int32 Alpha = 3; Alpha < Pixels.Num(); Alpha += 4)
		{
			Pixels[Alpha] = 255;
		}
		Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, Pixels.GetData());
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Masks;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->PostEditChange();
		FAssetRegistryModule::AssetCreated(Texture);
		Package->MarkPackageDirty();
		Report.CreatedAssets.Add(ScenarioAssets::PathOf(Texture));
		return Texture;
	}

	UMaterialExpressionTextureSampleParameter2D* TextureParam(UMaterial* Material, const TCHAR* Name, EMaterialSamplerType Sampler, UTexture* Default, int32 X, int32 Y)
	{
		UMaterialExpressionTextureSampleParameter2D* Param = Cast<UMaterialExpressionTextureSampleParameter2D>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), X, Y));
		Param->ParameterName = Name;
		Param->SamplerType = Sampler;
		Param->Texture = Default;
		return Param;
	}

	UMaterial* NewMaterial(const FString& Name)
	{
		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
		return Cast<UMaterial>(Tools().CreateAsset(Name, SharedPath(), UMaterial::StaticClass(), Factory));
	}

	/**
	 * Lit PBR master. The parameters are the same in both graphs (BaseColor,
	 * Normal, Roughness, Metallic, AmbientOcclusion, Tiling), so instances and
	 * scripts do not care which one the project has. Legacy: the classic
	 * material inputs. Substrate: a Slab BSDF fed by the metalness helper, on
	 * the Front Material input.
	 */
	void BuildPbr(UMaterial* Material, bool bSubstrate, FScenarioChangeReport& Report)
	{
		UTexture* DefaultColor = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
		UTexture* DefaultNormal = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/DefaultNormal.DefaultNormal"));
		UTexture* White = SolidMask(TEXT("T_Scenario_WhiteMask"), 255, Report);
		UTexture* Black = SolidMask(TEXT("T_Scenario_BlackMask"), 0, Report);

		UMaterialExpression* Coord = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -1100, 0);
		UMaterialExpressionScalarParameter* Tiling = Cast<UMaterialExpressionScalarParameter>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -1100, 150));
		Tiling->ParameterName = TEXT("Tiling");
		Tiling->DefaultValue = 1.0f;
		UMaterialExpression* Uv = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass(), -850, 50);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Coord, FString(), Uv, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tiling, FString(), Uv, TEXT("B"));

		struct FMap { const TCHAR* Name; EMaterialSamplerType Sampler; UTexture* Default; const TCHAR* Output; EMaterialProperty Property; };
		const FMap Maps[] = {
			{ TEXT("BaseColor"), SAMPLERTYPE_Color, DefaultColor, TEXT("RGB"), MP_BaseColor },
			{ TEXT("Normal"), SAMPLERTYPE_Normal, DefaultNormal, TEXT("RGB"), MP_Normal },
			{ TEXT("Roughness"), SAMPLERTYPE_Masks, White, TEXT("R"), MP_Roughness },
			{ TEXT("Metallic"), SAMPLERTYPE_Masks, Black, TEXT("R"), MP_Metallic },
			{ TEXT("AmbientOcclusion"), SAMPLERTYPE_Masks, White, TEXT("R"), MP_AmbientOcclusion },
		};
		TMap<EMaterialProperty, UMaterialExpressionTextureSampleParameter2D*> Params;
		int32 Y = -300;
		for (const FMap& Map : Maps)
		{
			UMaterialExpressionTextureSampleParameter2D* Param = TextureParam(Material, Map.Name, Map.Sampler, Map.Default, -500, Y);
			UMaterialEditingLibrary::ConnectMaterialExpressions(Uv, FString(), Param, TEXT("UVs"));
			Params.Add(Map.Property, Param);
			if (!bSubstrate || Map.Property == MP_AmbientOcclusion)
			{
				// Ambient occlusion stays on the root node with Substrate too.
				UMaterialEditingLibrary::ConnectMaterialProperty(Param, Map.Output, Map.Property);
			}
			Y += 260;
		}
		if (!bSubstrate)
		{
			return;
		}

		// Texture sample outputs: 0 RGB, 1 R.
		constexpr int32 Rgb = 0;
		constexpr int32 Red = 1;
		UMaterialExpressionSubstrateMetalnessToDiffuseAlbedoF0* Metalness = Cast<UMaterialExpressionSubstrateMetalnessToDiffuseAlbedoF0>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSubstrateMetalnessToDiffuseAlbedoF0::StaticClass(), -200, -300));
		UMaterialExpressionConstant* Specular = Cast<UMaterialExpressionConstant>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant::StaticClass(), -400, -500));
		Specular->R = 0.5f;
		Metalness->BaseColor.Connect(Rgb, Params[MP_BaseColor]);
		Metalness->Metallic.Connect(Red, Params[MP_Metallic]);
		Metalness->Specular.Connect(0, Specular);

		UMaterialExpressionSubstrateSlabBSDF* Slab = Cast<UMaterialExpressionSubstrateSlabBSDF>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSubstrateSlabBSDF::StaticClass(), 100, -200));
		Slab->DiffuseAlbedo.Connect(0, Metalness);
		Slab->F0.Connect(1, Metalness);
		Slab->Roughness.Connect(Red, Params[MP_Roughness]);
		Slab->Normal.Connect(Rgb, Params[MP_Normal]);
		UMaterialEditingLibrary::ConnectMaterialProperty(Slab, FString(), MP_FrontMaterial);
	}

	void BuildUnlit(UMaterial* Material)
	{
		UTexture* DefaultColor = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
		Material->SetShadingModel(MSM_Unlit);
		Material->BlendMode = BLEND_Masked;
		Material->TwoSided = true;
		UMaterialExpressionTextureSampleParameter2D* Image = TextureParam(Material, TEXT("Image"), SAMPLERTYPE_Color, DefaultColor, -400, 0);
		UMaterialEditingLibrary::ConnectMaterialProperty(Image, TEXT("RGB"), MP_EmissiveColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Image, TEXT("A"), MP_OpacityMask);
	}

	void BuildSky(UMaterial* Material)
	{
		UTexture* DefaultColor = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
		Material->SetShadingModel(MSM_Unlit);
		Material->TwoSided = true;
		UMaterialExpression* World = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionWorldPosition::StaticClass(), -1100, 0);
		UMaterialExpression* Center = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionObjectPositionWS::StaticClass(), -1100, 150);
		UMaterialExpression* Direction = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSubtract::StaticClass(), -900, 50);
		UMaterialEditingLibrary::ConnectMaterialExpressions(World, FString(), Direction, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Center, FString(), Direction, TEXT("B"));

		// Direction from the sphere's center to longitude/latitude: +X is the panorama's center, top row is straight up.
		UMaterialExpressionCustom* Equirect = Cast<UMaterialExpressionCustom>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -700, 50));
		Equirect->Description = TEXT("Equirect UV");
		Equirect->OutputType = CMOT_Float2;
		Equirect->Code = TEXT("float3 d = normalize(Dir);\nfloat u = atan2(d.y, d.x) * 0.15915494 + 0.5;\nfloat v = acos(clamp(d.z, -1.0, 1.0)) * 0.31830989;\nreturn float2(u, v);");
		Equirect->Inputs.Reset();
		FCustomInput Input;
		Input.InputName = TEXT("Dir");
		Equirect->Inputs.Add(Input);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Direction, FString(), Equirect, TEXT("Dir"));

		UMaterialExpressionTextureSampleParameter2D* Panorama = TextureParam(Material, TEXT("Panorama"), SAMPLERTYPE_Color, DefaultColor, -400, 0);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Equirect, FString(), Panorama, TEXT("UVs"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Panorama, TEXT("RGB"), MP_EmissiveColor);
	}
}

FString ScenarioAssets::PathOf(const UObject* Asset)
{
	return Asset ? Asset->GetPathName() : FString();
}

FString ScenarioAssets::UniqueName(const FString& PackagePath, const FString& Name)
{
	FString PackageName;
	FString AssetName;
	Tools().CreateUniqueAssetName(PackagePath / Name, FString(), PackageName, AssetName);
	return AssetName.IsEmpty() ? Name : AssetName;
}

bool ScenarioAssets::StageFile(const FString& Source, const FString& LocalId, const FString& AssetName, FString& OutStaged, FString& OutError)
{
	const FString Folder = FPaths::Combine(UScenarioSubsystem::StateDir(), TEXT("Staging"), ScenarioFiles::SafeObjectName(LocalId));
	OutStaged = FPaths::Combine(Folder, AssetName + TEXT(".") + FPaths::GetExtension(Source).ToLower());
	IFileManager::Get().MakeDirectory(*Folder, true);
	if (IFileManager::Get().Copy(*OutStaged, *Source, true, true) != COPY_OK)
	{
		OutError = FString::Printf(TEXT("Could not prepare %s for import."), *FPaths::GetCleanFilename(Source));
		return false;
	}
	return true;
}

bool ScenarioAssets::SetPropertyByPath(UObject* Root, const FString& Path, const FString& Value)
{
	TArray<FString> Parts;
	Path.ParseIntoArray(Parts, TEXT("."));
	UObject* Owner = Root;
	for (int32 Index = 0; Owner && Index < Parts.Num(); ++Index)
	{
		FProperty* Property = FindFProperty<FProperty>(Owner->GetClass(), *Parts[Index]);
		if (!Property)
		{
			return false;
		}
		void* Address = Property->ContainerPtrToValuePtr<void>(Owner);
		if (Index + 1 < Parts.Num())
		{
			const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
			Owner = ObjectProperty ? ObjectProperty->GetObjectPropertyValue(Address) : nullptr;
			continue;
		}
		return Property->ImportText_Direct(*Value, Address, Owner, PPF_None) != nullptr;
	}
	return false;
}

TArray<UObject*> ScenarioAssets::ImportPipelines(const FString& File, TArray<FString>& OutNotes)
{
	const FString Extension = FPaths::GetExtension(File).ToLower();
	const bool bGltf = Extension == TEXT("glb") || Extension == TEXT("gltf");
	const bool bMesh = bGltf || Extension == TEXT("fbx") || Extension == TEXT("obj");
	const bool bImage = Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg") || Extension == TEXT("tga") || Extension == TEXT("exr")
		|| Extension == TEXT("hdr") || Extension == TEXT("webp") || Extension == TEXT("bmp") || Extension == TEXT("tif") || Extension == TEXT("tiff");
	TArray<UObject*> Pipelines;
	if (!bMesh && !bImage)
	{
		return Pipelines;
	}
	// The engine's own defaults for the format, copied: the project's import stack and the last dialog never reach them.
	const TCHAR* Base = bGltf ? TEXT("/Interchange/Pipelines/DefaultGLTFAssetsPipeline.DefaultGLTFAssetsPipeline")
		: bMesh ? TEXT("/Interchange/Pipelines/DefaultFBXOBJAssetsPipeline.DefaultFBXOBJAssetsPipeline")
		: TEXT("/Interchange/Pipelines/DefaultTexturePipeline.DefaultTexturePipeline");
	UObject* Default = LoadObject<UObject>(nullptr, Base, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Default)
	{
		OutNotes.Add(FString::Printf(TEXT("Interchange has no %s in this engine: the project's import settings applied."), *FPackageName::ObjectPathToObjectName(FString(Base))));
		return Pipelines;
	}
	UObject* Pipeline = DuplicateObject<UObject>(Default, GetTransientPackage());
	Pipeline->ClearFlags(RF_Public | RF_Standalone);
	Pipeline->SetFlags(RF_Transient);
	Pipelines.Add(Pipeline);

	TArray<TPair<FString, FString>> Policy;
	if (bMesh)
	{
		const UScenarioProjectSettings& Settings = UScenarioProjectSettings::Get();
		const bool bCollision = Settings.MeshCollision != EScenarioMeshCollision::None;
		Policy = {
			// glTF is in meters: the translator converts to centimeters, and no extra scale is applied.
			{ TEXT("ImportOffsetUniformScale"), TEXT("1.0") },
			{ TEXT("MeshPipeline.bBuildNanite"), Settings.bNanite ? TEXT("True") : TEXT("False") },
			// Lumen lights the scene: no lightmap UV channel is generated.
			{ TEXT("MeshPipeline.bGenerateLightmapUVs"), TEXT("False") },
			// A simple box always; complex-as-simple is set after the build when the settings ask for it.
			{ TEXT("MeshPipeline.bCollision"), bCollision ? TEXT("True") : TEXT("False") },
			{ TEXT("MeshPipeline.Collision"), TEXT("Box") },
			{ TEXT("MeshPipeline.bImportCollisionAccordingToMeshName"), TEXT("True") },
			{ TEXT("MaterialPipeline.bImportMaterials"), TEXT("True") },
			{ TEXT("MaterialPipeline.TexturePipeline.bImportTextures"), TEXT("True") },
			{ TEXT("MaterialPipeline.TexturePipeline.bDetectNormalMapTexture"), TEXT("True") },
		};
	}
	else
	{
		Policy = {
			{ TEXT("bImportTextures"), TEXT("True") },
			{ TEXT("bDetectNormalMapTexture"), TEXT("True") },
		};
	}
	for (const TPair<FString, FString>& Entry : Policy)
	{
		if (!SetPropertyByPath(Pipeline, Entry.Key, Entry.Value))
		{
			OutNotes.Add(FString::Printf(TEXT("This engine's Interchange has no %s: that part of the import follows the engine default."), *Entry.Key));
		}
	}
	if (bGltf)
	{
		// glTF materials: Epic's glTF pipeline maps them onto its glTF material library, as in a default import.
		if (UObject* Gltf = LoadObject<UObject>(nullptr, TEXT("/Interchange/Pipelines/DefaultGLTFPipeline.DefaultGLTFPipeline"), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			Pipelines.Add(Gltf);
		}
	}
	return Pipelines;
}

void ScenarioAssets::ImportFile(const FString& File, const FString& PackagePath, FImportDone OnDone)
{
	UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();
	UInterchangeSourceData* Source = UInterchangeManager::CreateSourceData(File);
	if (Source && Manager.CanTranslateSourceData(Source))
	{
		FImportAssetParameters Parameters;
		Parameters.bIsAutomated = true;
		TArray<FString> Notes;
		TArray<TStrongObjectPtr<UObject>> Keep;
		for (UObject* Pipeline : ImportPipelines(File, Notes))
		{
			Parameters.OverridePipelines.Add(FSoftObjectPath(Pipeline));
			Keep.Emplace(Pipeline);
		}
		for (const FString& Note : Notes)
		{
			UE_LOG(LogScenario, Warning, TEXT("%s"), *Note);
		}
		UE::Interchange::FAssetImportResultRef Result = Manager.ImportAssetAsync(PackagePath, Source, Parameters);
		Result->OnDone([OnDone, File, Keep = MoveTemp(Keep)](UE::Interchange::FImportResult& Done)
		{
			TArray<TWeakObjectPtr<UObject>> Weak;
			for (UObject* Object : Done.GetImportedObjects())
			{
				Weak.Add(Object);
			}
			// The core ticker, not a game-thread task: tasks can run inside a render flush (a window
			// resize, say), where saving the imported packages trips asset streaming checks.
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([OnDone, Weak, File](float)
			{
				TArray<UObject*> Objects;
				for (const TWeakObjectPtr<UObject>& Object : Weak)
				{
					if (Object.IsValid())
					{
						Objects.Add(Object.Get());
					}
				}
				OnDone(Objects, Objects.Num() > 0 ? FString() : FString::Printf(TEXT("Unreal imported nothing from %s."), *FPaths::GetCleanFilename(File)));
				return false;
			}));
		});
		return;
	}

	// Formats Interchange does not read here (audio on some versions): the asset tools, without dialogs.
	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = File;
	Task->DestinationPath = PackagePath;
	Task->bAutomated = true;
	Task->bSave = false;
	Task->bReplaceExisting = false;
	Tools().ImportAssetTasks({ Task });
	TArray<UObject*> Objects;
	for (const FString& ObjectPath : Task->ImportedObjectPaths)
	{
		if (UObject* Object = LoadObject<UObject>(nullptr, *ObjectPath))
		{
			Objects.Add(Object);
		}
	}
	OnDone(Objects, Objects.Num() > 0 ? FString() : FString::Printf(TEXT("Unreal could not import %s."), *FPaths::GetCleanFilename(File)));
}

void ScenarioAssets::ConfigureTexture(UTexture* Texture, const FString& Role, int32 MaxSize)
{
	if (!Texture)
	{
		return;
	}
	Texture->Modify();
	const bool bHdr = Texture->CompressionSettings == TC_HDR || Texture->CompressionSettings == TC_HDR_Compressed || Texture->IsA<UTextureCube>();
	if (Role == TEXT("normal"))
	{
		Texture->CompressionSettings = TC_Normalmap;
		Texture->SRGB = false;
		Texture->LODGroup = TEXTUREGROUP_WorldNormalMap;
	}
	else if (Role == TEXT("roughness") || Role == TEXT("smoothness") || Role == TEXT("metallic") || Role == TEXT("height") || Role == TEXT("ao"))
	{
		Texture->CompressionSettings = TC_Masks;
		Texture->SRGB = false;
	}
	else if (Role == TEXT("panorama"))
	{
		// Full size, no mips: a sky is seen at its native resolution and mips blur the seam.
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->LODGroup = TEXTUREGROUP_Skybox;
		Texture->MaxTextureSize = 0;
		Texture->SRGB = !bHdr;
	}
	else if (Role.IsEmpty())
	{
		// Mesh textures: the importer set sRGB from the material slot that uses each map. FixMeshTextures corrects the masks.
	}
	else if (!bHdr)
	{
		Texture->SRGB = true;
	}
	if (MaxSize > 0 && Role != TEXT("panorama"))
	{
		Texture->MaxTextureSize = MaxSize;
	}
	Texture->PostEditChange();
}

namespace
{
	/** Material parameters that sample data, not color: metallic, roughness, occlusion and packed masks. */
	bool IsMaskParameter(const FString& Parameter)
	{
		const FString Name = Parameter.ToLower();
		if (Name.Contains(TEXT("normal")) || Name.Contains(TEXT("basecolor")) || Name.Contains(TEXT("albedo")) || Name.Contains(TEXT("emissive")) || Name.Contains(TEXT("diffuse")))
		{
			return false;
		}
		for (const TCHAR* Token : { TEXT("metallic"), TEXT("roughness"), TEXT("occlusion"), TEXT("orm"), TEXT("specular") })
		{
			if (Name.Contains(Token))
			{
				return true;
			}
		}
		return false;
	}

	void AddInstance(UMaterialInterface* Material, TSet<UMaterialInstance*>& Out)
	{
		if (UMaterialInstance* Instance = Cast<UMaterialInstance>(Material))
		{
			Out.Add(Instance);
		}
	}
}

int32 ScenarioAssets::FixMeshTextures(const TArray<UObject*>& Objects)
{
	TSet<UMaterialInstance*> Instances;
	for (UObject* Object : Objects)
	{
		if (UMaterialInterface* Material = Cast<UMaterialInterface>(Object))
		{
			AddInstance(Material, Instances);
		}
		else if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Object))
		{
			for (const FStaticMaterial& Slot : StaticMesh->GetStaticMaterials())
			{
				AddInstance(Slot.MaterialInterface, Instances);
			}
		}
		else if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Object))
		{
			for (const FSkeletalMaterial& Slot : SkeletalMesh->GetMaterials())
			{
				AddInstance(Slot.MaterialInterface, Instances);
			}
		}
	}
	TSet<UTexture*> Changed;
	for (const UMaterialInstance* Instance : Instances)
	{
		for (const FTextureParameterValue& Value : Instance->TextureParameterValues)
		{
			UTexture* Texture = Value.ParameterValue;
			if (!Texture || Changed.Contains(Texture))
			{
				continue;
			}
			const FString Parameter = Value.ParameterInfo.Name.ToString();
			if (Parameter.Contains(TEXT("normal"), ESearchCase::IgnoreCase) && (Texture->SRGB || Texture->CompressionSettings != TC_Normalmap))
			{
				// A normal map the importer did not detect: linear, normal compression (the material samples it as a normal).
				Texture->Modify();
				Texture->SRGB = false;
				Texture->CompressionSettings = TC_Normalmap;
				Texture->PostEditChange();
				Texture->MarkPackageDirty();
				Changed.Add(Texture);
				continue;
			}
			if (!Texture->SRGB || !IsMaskParameter(Parameter))
			{
				continue;
			}
			// Linear only: the compression stays what the importer chose, so it still matches the material's sampler.
			Texture->Modify();
			Texture->SRGB = false;
			Texture->PostEditChange();
			Texture->MarkPackageDirty();
			Changed.Add(Texture);
		}
	}
	return Changed.Num();
}

FString ScenarioAssets::FMaterialVariant::Describe() const
{
	if (bSubstrate)
	{
		return FString::Printf(TEXT("Material: %s, a Substrate slab graph (Substrate is on in this project)."), *MasterName);
	}
	return bProjectSetting
		? FString::Printf(TEXT("Material: %s, a legacy PBR graph. Substrate is set in Project Settings but not running yet: restart the editor, then import again for a Substrate master."), *MasterName)
		: FString::Printf(TEXT("Material: %s, a legacy PBR graph (Substrate is off in this project)."), *MasterName);
}

ScenarioAssets::FMaterialVariant ScenarioAssets::MaterialVariant()
{
	FMaterialVariant Variant;
	// The running renderer decides: r.Substrate is read only at startup, so a changed setting waits for a restart.
	Variant.bSubstrate = Substrate::IsSubstrateEnabled();
	const URendererSettings* Renderer = GetDefault<URendererSettings>();
	Variant.bProjectSetting = Renderer && Renderer->bEnableSubstrate;
	Variant.MasterName = Variant.bSubstrate ? TEXT("M_Scenario_PBR_Substrate") : TEXT("M_Scenario_PBR");
	return Variant;
}

UMaterialInterface* ScenarioAssets::PbrMaster(bool bSubstrate, FScenarioChangeReport& Report)
{
	const TCHAR* Name = bSubstrate ? TEXT("M_Scenario_PBR_Substrate") : TEXT("M_Scenario_PBR");
	if (UMaterial* Existing = LoadAsset<UMaterial>(SharedPath(), Name))
	{
		return Existing;
	}
	UMaterial* Material = NewMaterial(Name);
	if (!Material)
	{
		Report.Warnings.Add(FString::Printf(TEXT("Could not create %s in %s."), Name, *SharedPath()));
		return nullptr;
	}
	BuildPbr(Material, bSubstrate, Report);
	UMaterialEditingLibrary::RecompileMaterial(Material);
	Material->MarkPackageDirty();
	Report.CreatedAssets.Add(PathOf(Material));
	return Material;
}

UMaterialInterface* ScenarioAssets::Master(EScenarioMaster Which, FScenarioChangeReport& Report)
{
	if (Which == EScenarioMaster::Pbr)
	{
		return PbrMaster(MaterialVariant().bSubstrate, Report);
	}
	// Unlit graphs: the engine converts them to Substrate's unlit BSDF when Substrate is on, so one graph serves both.
	const TCHAR* Name = Which == EScenarioMaster::Unlit ? TEXT("M_Scenario_Unlit") : TEXT("M_Scenario_SkySphere");
	if (UMaterial* Existing = LoadAsset<UMaterial>(SharedPath(), Name))
	{
		return Existing;
	}
	UMaterial* Material = NewMaterial(Name);
	if (!Material)
	{
		Report.Warnings.Add(FString::Printf(TEXT("Could not create %s in %s."), Name, *SharedPath()));
		return nullptr;
	}
	if (Which == EScenarioMaster::Unlit)
	{
		BuildUnlit(Material);
	}
	else
	{
		BuildSky(Material);
	}
	UMaterialEditingLibrary::RecompileMaterial(Material);
	Material->MarkPackageDirty();
	Report.CreatedAssets.Add(PathOf(Material));
	return Material;
}

FName ScenarioAssets::PbrParameterForRole(const FString& Role)
{
	if (Role == TEXT("normal")) return TEXT("Normal");
	if (Role == TEXT("roughness")) return TEXT("Roughness");
	if (Role == TEXT("metallic")) return TEXT("Metallic");
	if (Role == TEXT("ao")) return TEXT("AmbientOcclusion");
	if (Role == TEXT("height") || Role == TEXT("smoothness")) return NAME_None;
	return TEXT("BaseColor");
}

bool ScenarioAssets::InvertImage(FImage& Image)
{
	const int64 Count = Image.GetNumPixels();
	switch (Image.Format)
	{
	case ERawImageFormat::G8:
		for (uint8& Value : Image.AsG8())
		{
			Value = 255 - Value;
		}
		return true;
	case ERawImageFormat::BGRA8:
		for (FColor& Pixel : Image.AsBGRA8())
		{
			Pixel.R = 255 - Pixel.R;
			Pixel.G = 255 - Pixel.G;
			Pixel.B = 255 - Pixel.B;
		}
		return true;
	case ERawImageFormat::G16:
		for (uint16& Value : Image.AsG16())
		{
			Value = 65535 - Value;
		}
		return true;
	case ERawImageFormat::RGBA16:
	{
		uint16* Values = reinterpret_cast<uint16*>(Image.RawData.GetData());
		for (int64 Index = 0; Index < Count; ++Index)
		{
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				Values[Index * 4 + Channel] = 65535 - Values[Index * 4 + Channel];
			}
		}
		return true;
	}
	default:
	{
		// Float and other layouts: through linear RGBA32F, inverted, then back to the source format.
		const ERawImageFormat::Type Format = Image.Format;
		const EGammaSpace Gamma = Image.GammaSpace;
		FImage Linear;
		Image.CopyTo(Linear, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		for (FLinearColor& Pixel : Linear.AsRGBA32F())
		{
			Pixel.R = 1.0f - Pixel.R;
			Pixel.G = 1.0f - Pixel.G;
			Pixel.B = 1.0f - Pixel.B;
		}
		Linear.CopyTo(Image, Format, Gamma);
		return true;
	}
	}
}

UTexture2D* ScenarioAssets::CreateRoughnessFromSmoothness(UTexture2D* Smoothness, const FString& Name, int32 MaxSize, FScenarioChangeReport& Report)
{
	FImage Image;
	if (!Smoothness || !Smoothness->Source.IsValid() || !Smoothness->Source.GetMipImage(Image, 0, 0, 0))
	{
		Report.Warnings.Add(FString::Printf(TEXT("%s has no source pixels: the smoothness map was not inverted."), Smoothness ? *Smoothness->GetName() : TEXT("The smoothness map")));
		return nullptr;
	}
	// The pixels are data: invert the stored values, never through a gamma curve.
	Image.GammaSpace = EGammaSpace::Linear;
	if (!InvertImage(Image))
	{
		Report.Warnings.Add(FString::Printf(TEXT("%s has a pixel format that cannot be inverted."), *Smoothness->GetName()));
		return nullptr;
	}
	const FString PackagePath = FPackageName::GetLongPackagePath(Smoothness->GetOutermost()->GetName());
	const FString AssetName = UniqueName(PackagePath, Name);
	UPackage* Package = CreatePackage(*(PackagePath / AssetName));
	UTexture2D* Roughness = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	Roughness->Source.Init(Image);
	Roughness->SRGB = false;
	ConfigureTexture(Roughness, TEXT("roughness"), MaxSize);
	FAssetRegistryModule::AssetCreated(Roughness);
	Package->MarkPackageDirty();
	Report.CreatedAssets.Add(PathOf(Roughness));
	return Roughness;
}

TMap<FName, UTexture*> ScenarioAssets::PbrMaps(const TArray<FRoleTexture>& Textures, bool bInvertSmoothness, int32 MaxSize, FScenarioChangeReport& Report, TMap<UTexture*, UTexture*>& OutDerived)
{
	TMap<FName, UTexture*> Maps;
	UTexture* Smoothness = nullptr;
	for (const FRoleTexture& Entry : Textures)
	{
		if (!Entry.Texture)
		{
			continue;
		}
		if (Entry.Role == TEXT("smoothness"))
		{
			Smoothness = Smoothness ? Smoothness : Entry.Texture;
			continue;
		}
		const FName Parameter = PbrParameterForRole(Entry.Role);
		if (!Parameter.IsNone() && !Maps.Contains(Parameter))
		{
			Maps.Add(Parameter, Entry.Texture);
		}
	}
	if (Smoothness && !Maps.Contains(TEXT("Roughness")))
	{
		UTexture2D* Source = Cast<UTexture2D>(Smoothness);
		if (!bInvertSmoothness)
		{
			Maps.Add(TEXT("Roughness"), Smoothness);
			Report.Checks.Add(FString::Printf(TEXT("%s used as roughness without inversion (Project Settings: Invert Smoothness is off)."), *Smoothness->GetName()));
		}
		else if (Source)
		{
			FString Name = Source->GetName();
			if (Name.EndsWith(TEXT("_S")))
			{
				Name.LeftChopInline(2);
			}
			if (UTexture2D* Roughness = CreateRoughnessFromSmoothness(Source, Name + TEXT("_R"), MaxSize, Report))
			{
				Maps.Add(TEXT("Roughness"), Roughness);
				OutDerived.Add(Roughness, Smoothness);
				Report.Checks.Add(FString::Printf(TEXT("Smoothness map inverted into %s (roughness = 1 - smoothness); %s kept."), *Roughness->GetName(), *Smoothness->GetName()));
			}
		}
	}
	return Maps;
}

UMaterialInstanceConstant* ScenarioAssets::CreateMaterialInstance(UMaterialInterface* Parent, const FString& PackagePath, const FString& Name,
	const TMap<FName, UTexture*>& Textures, const TMap<FName, float>& Scalars, FScenarioChangeReport& Report)
{
	if (!Parent)
	{
		return nullptr;
	}
	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = Parent;
	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(
		Tools().CreateAsset(UniqueName(PackagePath, Name), PackagePath, UMaterialInstanceConstant::StaticClass(), Factory));
	if (!Instance)
	{
		Report.Warnings.Add(FString::Printf(TEXT("Could not create the material instance %s."), *Name));
		return nullptr;
	}
	for (const TPair<FName, UTexture*>& Pair : Textures)
	{
		if (Pair.Value)
		{
			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Pair.Key), Pair.Value);
		}
	}
	for (const TPair<FName, float>& Pair : Scalars)
	{
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Pair.Key), Pair.Value);
	}
	Instance->PostEditChange();
	Instance->MarkPackageDirty();
	Report.CreatedAssets.Add(PathOf(Instance));
	return Instance;
}

UObject* ScenarioAssets::CreateMediaSource(const FString& VideoFile, const FString& PackagePath, const FString& Name, FScenarioChangeReport& Report)
{
	// Content/Movies is the folder Unreal stages with packaged projects.
	const FString Folder = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Movies"), TEXT("Scenario"), ScenarioFiles::Today());
	IFileManager::Get().MakeDirectory(*Folder, true);
	const FString AssetName = UniqueName(PackagePath, Name);
	const FString Target = FPaths::Combine(Folder, AssetName.RightChop(AssetName.StartsWith(TEXT("FMS_")) ? 4 : 0) + TEXT(".") + FPaths::GetExtension(VideoFile).ToLower());
	if (IFileManager::Get().Copy(*Target, *VideoFile, false, true) != COPY_OK)
	{
		Report.Warnings.Add(TEXT("Could not copy the video into Content/Movies."));
		return nullptr;
	}
	Report.SavedFiles.Add(FPaths::ConvertRelativePathToFull(Target));
	UPackage* Package = CreatePackage(*(PackagePath / AssetName));
	UFileMediaSource* Source = NewObject<UFileMediaSource>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	Source->SetFilePath(FPaths::ConvertRelativePathToFull(Target));
	FAssetRegistryModule::AssetCreated(Source);
	Package->MarkPackageDirty();
	Report.CreatedAssets.Add(PathOf(Source));
	return Source;
}

void ScenarioAssets::ConfigureStaticMesh(UObject* Mesh, TArray<FString>* OutChecks)
{
	UStaticMesh* StaticMesh = Cast<UStaticMesh>(Mesh);
	if (!StaticMesh)
	{
		return;
	}
	const UScenarioProjectSettings& Settings = UScenarioProjectSettings::Get();
	// The importer ends with an async build: wait for it so the bounds and the body setup are final before editing them.
	FStaticMeshCompilingManager::Get().FinishCompilation({ StaticMesh });
	StaticMesh->Modify();
	if (Settings.MeshCollision != EScenarioMeshCollision::None)
	{
		StaticMesh->CreateBodySetup();
		if (UBodySetup* Body = StaticMesh->GetBodySetup())
		{
			Body->Modify();
			// A simple shape in every case: physics, overlaps and navigation read simple collision.
			if (Body->AggGeom.GetElementCount() == 0)
			{
				const FBox Box = StaticMesh->GetBoundingBox();
				FKBoxElem Element(Box.GetSize().X, Box.GetSize().Y, Box.GetSize().Z);
				Element.Center = Box.GetCenter();
				Body->AggGeom.BoxElems.Add(Element);
			}
			if (Settings.MeshCollision == EScenarioMeshCollision::ComplexAsSimple)
			{
				// Queries and the player use the triangles, so openings (gates, arches, doors) stay open.
				Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
				Body->bNeverNeedsCookedCollisionData = false;
			}
			else
			{
				Body->CollisionTraceFlag = CTF_UseDefault;
			}
			Body->InvalidatePhysicsData();
			Body->CreatePhysicsMeshes();
		}
		// A reimport keeps this collision.
#if UE_VERSION_OLDER_THAN(5, 7, 0)
		StaticMesh->bCustomizedCollision = true;
#else
		StaticMesh->SetCustomizedCollision(true);
#endif
	}

	// Nanite does not draw translucent materials: a mesh with one stays a regular mesh.
	FString Translucent;
	const TArray<FStaticMaterial>& Slots = StaticMesh->GetStaticMaterials();
	for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
	{
		const UMaterialInterface* Material = Slots[Slot].MaterialInterface;
		const EBlendMode Blend = Material ? Material->GetBlendMode() : BLEND_Opaque;
		if (Blend != BLEND_Opaque && Blend != BLEND_Masked)
		{
			Translucent = FString::Printf(TEXT("slot %d (%s) is not opaque or masked"), Slot, Material ? *Material->GetName() : TEXT("none"));
			break;
		}
	}
	const bool bWantNanite = Settings.bNanite && Translucent.IsEmpty();
	FMeshNaniteSettings Nanite = StaticMesh->GetNaniteSettings();
	if (Nanite.bEnabled != bWantNanite)
	{
		Nanite.bEnabled = bWantNanite;
		StaticMesh->SetNaniteSettings(Nanite);
	}
	bool bLightmapUvs = false;
	if (StaticMesh->GetNumSourceModels() > 0)
	{
		FStaticMeshSourceModel& Source = StaticMesh->GetSourceModel(0);
		bLightmapUvs = Source.BuildSettings.bGenerateLightmapUVs;
		if (bLightmapUvs)
		{
			Source.BuildSettings.bGenerateLightmapUVs = false;
			bLightmapUvs = false;
		}
	}
	StaticMesh->PostEditChange();
	StaticMesh->MarkPackageDirty();

	if (OutChecks)
	{
		FStaticMeshCompilingManager::Get().FinishCompilation({ StaticMesh });
		const UBodySetup* Body = StaticMesh->GetBodySetup();
		const int32 Shapes = Body ? Body->AggGeom.GetElementCount() : 0;
		if (Settings.MeshCollision == EScenarioMeshCollision::None)
		{
			OutChecks->Add(TEXT("Collision: none (Project Settings)."));
		}
		else if (Shapes == 0)
		{
			OutChecks->Add(TEXT("Check: no simple collision shape; physics and overlaps will not see this mesh."));
		}
		else
		{
			OutChecks->Add(FString::Printf(TEXT("Collision: %d simple shape%s%s."), Shapes, Shapes == 1 ? TEXT("") : TEXT("s"),
				Body && Body->CollisionTraceFlag == CTF_UseComplexAsSimple ? TEXT(", the mesh shape for queries and the player") : TEXT("")));
		}
		OutChecks->Add(StaticMesh->IsNaniteEnabled() ? FString(TEXT("Nanite: on (every material slot is opaque or masked)."))
			: !Settings.bNanite ? FString(TEXT("Nanite: off (Project Settings)."))
			: FString::Printf(TEXT("Nanite: off, %s."), *Translucent));
		OutChecks->Add(bLightmapUvs ? TEXT("Check: lightmap UVs are generated.") : TEXT("Lightmap UVs: not generated (Lumen lighting)."));
	}
}

UObject* ScenarioAssets::RenameAsset(UObject* Asset, const FString& NewName)
{
	if (!Asset || Asset->GetName() == NewName)
	{
		return Asset;
	}
	const FString PackagePath = FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName());
	const FString Unique = UniqueName(PackagePath, NewName);
	TArray<FAssetRenameData> Renames;
	Renames.Emplace(Asset, PackagePath, Unique);
	if (!Tools().RenameAssets(Renames))
	{
		return Asset;
	}
	UObject* Renamed = LoadObject<UObject>(nullptr, *(PackagePath / Unique + TEXT(".") + Unique), nullptr, LOAD_NoWarn | LOAD_Quiet);
	return Renamed ? Renamed : Asset;
}

void ScenarioAssets::CheckMesh(UObject* Mesh, TArray<FString>& OutChecks, double ExpectedSizeCm)
{
	FBox Box(ForceInit);
	if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Mesh))
	{
		Box = StaticMesh->GetBoundingBox();
		OutChecks.Add(FString::Printf(TEXT("Triangles: %d"), StaticMesh->GetNumTriangles(0)));
		OutChecks.Add(FString::Printf(TEXT("Material slots: %d"), StaticMesh->GetStaticMaterials().Num()));
	}
	else if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh))
	{
		Box = SkeletalMesh->GetBounds().GetBox();
		const FReferenceSkeleton& Skeleton = SkeletalMesh->GetRefSkeleton();
		OutChecks.Add(FString::Printf(TEXT("Bones: %d"), Skeleton.GetNum()));
		static const TCHAR* Mannequin[] = { TEXT("pelvis"), TEXT("spine_01"), TEXT("neck_01"), TEXT("head"), TEXT("clavicle_l"), TEXT("upperarm_l"),
			TEXT("lowerarm_l"), TEXT("hand_l"), TEXT("thigh_l"), TEXT("calf_l"), TEXT("foot_l"), TEXT("thigh_r") };
		int32 Matches = 0;
		for (const TCHAR* Bone : Mannequin)
		{
			Matches += Skeleton.FindBoneIndex(FName(Bone)) != INDEX_NONE ? 1 : 0;
		}
		const int32 Wanted = UE_ARRAY_COUNT(Mannequin);
		OutChecks.Add(Matches == Wanted
			? TEXT("Bone names match the UE5 Mannequin.")
			: FString::Printf(TEXT("Check: %d of %d UE5 Mannequin bone names found; retargeting needs an IK Rig."), Matches, Wanted));
		OutChecks.Add(FString::Printf(TEXT("Morph targets: %d"), SkeletalMesh->GetMorphTargets().Num()));
	}
	else
	{
		return;
	}
	if (!Box.IsValid)
	{
		return;
	}
	const FVector Size = Box.GetSize();
	OutChecks.Add(FString::Printf(TEXT("Size: %.0f x %.0f x %.0f cm"), Size.X, Size.Y, Size.Z));
	if (ExpectedSizeCm > 0.0)
	{
		const double Ratio = Size.GetMax() / ExpectedSizeCm;
		OutChecks.Add(Ratio >= 0.5 && Ratio <= 2.0
			? FString::Printf(TEXT("Size matches the requested %.0f cm."), ExpectedSizeCm)
			: FString::Printf(TEXT("Check: the largest side is %.0f cm for a requested %.0f cm; the unit may be wrong."), Size.GetMax(), ExpectedSizeCm));
	}
	else if (Size.GetMax() < 5.0)
	{
		OutChecks.Add(TEXT("Check: under 5 cm. The model is probably unit-normalized; scale it to real size."));
	}
	else if (Size.GetMax() > 10000.0)
	{
		OutChecks.Add(TEXT("Check: over 100 m. The model may be in the wrong unit."));
	}
	const double Offset = Box.Min.Z;
	if (FMath::Abs(Offset) <= FMath::Max(1.0, Size.Z * 0.02))
	{
		OutChecks.Add(TEXT("Pivot at the base."));
	}
	else
	{
		OutChecks.Add(FString::Printf(TEXT("Check: the base is %.0f cm %s the pivot; placement compensates."), FMath::Abs(Offset), Offset > 0.0 ? TEXT("above") : TEXT("below")));
	}
}

TArray<FString> ScenarioAssets::SaveAssets(const TArray<UObject*>& Assets)
{
	TArray<UPackage*> Packages;
	for (const UObject* Asset : Assets)
	{
		if (Asset)
		{
			Packages.AddUnique(Asset->GetOutermost());
		}
	}
	TArray<FString> Files;
	if (Packages.Num() == 0 || !UEditorLoadingAndSavingUtils::SavePackages(Packages, true))
	{
		return Files;
	}
	for (const UPackage* Package : Packages)
	{
		FString File;
		if (FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), File, FPackageName::GetAssetPackageExtension()))
		{
			Files.Add(FPaths::ConvertRelativePathToFull(File));
		}
	}
	return Files;
}
