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
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "FileHelpers.h"
#include "FileMediaSource.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "InterchangeManager.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionObjectPositionWS.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "ReferenceSkeleton.h"
#include "ScenarioCoreModule.h"
#include "ScenarioFiles.h"
#include "ScenarioSettings.h"
#include "ScenarioSubsystem.h"
#include "UObject/Package.h"

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

	void BuildPbr(UMaterial* Material, FScenarioChangeReport& Report)
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
		int32 Y = -300;
		for (const FMap& Map : Maps)
		{
			UMaterialExpressionTextureSampleParameter2D* Param = TextureParam(Material, Map.Name, Map.Sampler, Map.Default, -500, Y);
			UMaterialEditingLibrary::ConnectMaterialExpressions(Uv, FString(), Param, TEXT("UVs"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Param, Map.Output, Map.Property);
			Y += 260;
		}
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

void ScenarioAssets::ImportFile(const FString& File, const FString& PackagePath, FImportDone OnDone)
{
	UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();
	UInterchangeSourceData* Source = UInterchangeManager::CreateSourceData(File);
	if (Source && Manager.CanTranslateSourceData(Source))
	{
		FImportAssetParameters Parameters;
		Parameters.bIsAutomated = true;
		UE::Interchange::FAssetImportResultRef Result = Manager.ImportAssetAsync(PackagePath, Source, Parameters);
		Result->OnDone([OnDone, File](UE::Interchange::FImportResult& Done)
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
	else if (Role == TEXT("roughness") || Role == TEXT("metallic") || Role == TEXT("height") || Role == TEXT("ao"))
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

UMaterialInterface* ScenarioAssets::Master(EScenarioMaster Which, FScenarioChangeReport& Report)
{
	const TCHAR* Name = Which == EScenarioMaster::Pbr ? TEXT("M_Scenario_PBR") : Which == EScenarioMaster::Unlit ? TEXT("M_Scenario_Unlit") : TEXT("M_Scenario_SkySphere");
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
	switch (Which)
	{
	case EScenarioMaster::Pbr: BuildPbr(Material, Report); break;
	case EScenarioMaster::Unlit: BuildUnlit(Material); break;
	default: BuildSky(Material); break;
	}
	UMaterialEditingLibrary::RecompileMaterial(Material);
	Material->MarkPackageDirty();
	Report.CreatedAssets.Add(PathOf(Material));
	return Material;
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

void ScenarioAssets::ConfigureStaticMesh(UObject* Mesh)
{
	UStaticMesh* StaticMesh = Cast<UStaticMesh>(Mesh);
	if (!StaticMesh)
	{
		return;
	}
	const UScenarioProjectSettings& Settings = UScenarioProjectSettings::Get();
	StaticMesh->Modify();
	if (Settings.MeshCollision != EScenarioMeshCollision::None)
	{
		StaticMesh->CreateBodySetup();
		if (UBodySetup* Body = StaticMesh->GetBodySetup())
		{
			Body->Modify();
			if (Settings.MeshCollision == EScenarioMeshCollision::ComplexAsSimple)
			{
				Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
			}
			else if (Body->AggGeom.GetElementCount() == 0)
			{
				const FBox Box = StaticMesh->GetBoundingBox();
				FKBoxElem Element(Box.GetSize().X, Box.GetSize().Y, Box.GetSize().Z);
				Element.Center = Box.GetCenter();
				Body->AggGeom.BoxElems.Add(Element);
			}
			Body->InvalidatePhysicsData();
			Body->CreatePhysicsMeshes();
		}
	}
	if (Settings.bNanite)
	{
		FMeshNaniteSettings Nanite = StaticMesh->GetNaniteSettings();
		Nanite.bEnabled = true;
		StaticMesh->SetNaniteSettings(Nanite);
	}
	StaticMesh->PostEditChange();
	StaticMesh->MarkPackageDirty();
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

void ScenarioAssets::CheckMesh(UObject* Mesh, TArray<FString>& OutChecks)
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
	if (Size.GetMax() < 5.0)
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
