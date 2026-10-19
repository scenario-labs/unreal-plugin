// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioSubsystem.h"

class UMaterialInstanceConstant;
class UMaterialInterface;
class UTexture;

enum class EScenarioMaster : uint8
{
	/** Lit PBR: BaseColor, Normal, Roughness, Metallic, AO, Tiling. */
	Pbr,
	/** Unlit image with alpha mask, two-sided: image planes. */
	Unlit,
	/** Unlit equirectangular panorama on an inward sphere. */
	SkySphere,
};

/** Asset creation used by the importers. Game thread only. */
namespace ScenarioAssets
{
	using FImportDone = TFunction<void(const TArray<UObject*>& Objects, const FString& Error)>;

	/** An asset name free in the folder: Name, or Name_2, Name_3... */
	FString UniqueName(const FString& PackagePath, const FString& Name);

	/** Copies a download to Saved/Scenario/Staging/<LocalId>/<AssetName>.<ext>, so the importer names the asset. */
	bool StageFile(const FString& Source, const FString& LocalId, const FString& AssetName, FString& OutStaged, FString& OutError);

	/** Imports one file into a content folder without any dialog: Interchange's async path when it reads the format, else the asset tools. */
	void ImportFile(const FString& File, const FString& PackagePath, FImportDone OnDone);

	/** sRGB, compression, mips and size for the map's role (albedo, normal, roughness, metallic, height, ao, panorama, main). An empty role keeps the importer's color settings. */
	void ConfigureTexture(UTexture* Texture, const FString& Role, int32 MaxSize);

	/**
	 * Mesh imports: textures that a material samples as metallic, roughness,
	 * occlusion or packed masks become linear (glTF metallic-roughness maps
	 * arrive as data, not color). Reads the material instances among the
	 * objects and the meshes' slots. Returns how many textures changed.
	 */
	int32 FixMeshTextures(const TArray<UObject*>& Objects);

	/** The project's copy of a master material under <root>/_Shared, created on first use. */
	UMaterialInterface* Master(EScenarioMaster Which, FScenarioChangeReport& Report);

	UMaterialInstanceConstant* CreateMaterialInstance(UMaterialInterface* Parent, const FString& PackagePath, const FString& Name,
		const TMap<FName, UTexture*>& Textures, const TMap<FName, float>& Scalars, FScenarioChangeReport& Report);

	/** Copies a video under Content/Movies/Scenario/<date>/ and creates a File Media Source pointing at it. */
	UObject* CreateMediaSource(const FString& VideoFile, const FString& PackagePath, const FString& Name, FScenarioChangeReport& Report);

	/** Collision and Nanite from Project Settings. */
	void ConfigureStaticMesh(UObject* Mesh);

	/** Renames an imported asset in place (references fixed). Returns the asset under its new name. */
	UObject* RenameAsset(UObject* Asset, const FString& NewName);

	/** Scale, pivot, triangles, materials, bones and morph targets, one line each. Warnings start with "Check:". */
	void CheckMesh(UObject* Mesh, TArray<FString>& OutChecks);

	/** Saves the packages of these assets. Returns the files written. */
	TArray<FString> SaveAssets(const TArray<UObject*>& Assets);

	/** Object path for reports: /Game/Scenario/Images/2026-10-18/T_Chest_a1b2c3. */
	FString PathOf(const UObject* Asset);
}

/** Actors in the editor world. Callers open the transaction. */
namespace ScenarioPlacement
{
	/** Tag on every actor the plugin spawns. */
	inline const FName ActorTag = TEXT("Scenario");

	struct FView
	{
		bool bValid = false;
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		float Fov = 90.0f;
	};

	UWorld* EditorWorld();
	FView ActiveView();
	/** Actors by name or label, or the selection when Names is empty. */
	TArray<AActor*> ResolveTargets(const TArray<FString>& Names);
	FString Describe(const AActor* Actor);

	/** Point on the ground below Location (or Location when nothing is below). */
	FVector GroundBelow(UWorld* World, const FVector& Location);
	/** Where a new actor goes for these options, Distance in front of the camera. */
	FVector Location(const FScenarioPlaceOptions& Options, float Distance, bool bSnapToGround);
	/** Yaw that faces the camera from Location. */
	FRotator FacingCamera(const FVector& Location);

	AActor* SpawnMesh(UObject* Mesh, const FScenarioPlaceOptions& Options, const FString& Label, FScenarioChangeReport& Report);
	AActor* SpawnImagePlane(UMaterialInterface* Material, float Aspect, const FScenarioPlaceOptions& Options, const FString& Label, FScenarioChangeReport& Report);
	AActor* SpawnPreviewSphere(UMaterialInterface* Material, const FScenarioPlaceOptions& Options, const FString& Label, FScenarioChangeReport& Report);
	bool ApplyMaterial(AActor* Actor, int32 Slot, UMaterialInterface* Material, FScenarioChangeReport& Report);
	AActor* SpawnAmbientSound(class USoundBase* Sound, const FScenarioPlaceOptions& Options, const FString& Label, FScenarioChangeReport& Report);
	AActor* SpawnSkySphere(UMaterialInterface* Material, const FString& Label, FScenarioChangeReport& Report);
	/** Needs the HDRI Backdrop plugin; returns null with a warning when it is off. */
	AActor* SpawnHdriBackdrop(class UTextureCube* Cubemap, const FString& Label, FScenarioChangeReport& Report);
	/** Needs the Media Plate plugin; returns null with a warning when it is off. */
	AActor* SpawnMediaPlate(UObject* MediaSource, const FScenarioPlaceOptions& Options, const FString& Label, FScenarioChangeReport& Report);
	void Select(const TArray<AActor*>& Actors);
}
