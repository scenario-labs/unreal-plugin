// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioSubsystem.h"

class UMaterial;
class UMaterialInstanceConstant;
class UMaterialInterface;
class UTexture;
class UTexture2D;
struct FImage;

enum class EScenarioMaster : uint8
{
	/** Lit PBR: BaseColor, Normal, Roughness, Metallic, AO, Tiling. A Substrate slab graph when Substrate is on. */
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

	/**
	 * Imports one file into a content folder without any dialog: Interchange's
	 * async path when it reads the format, else the asset tools. Meshes and
	 * textures go through the plugin's own pipelines (ImportPipelines), so the
	 * result does not depend on the project's import stack or the last dialog.
	 */
	void ImportFile(const FString& File, const FString& PackagePath, FImportDone OnDone);

	/**
	 * The Interchange pipelines for a file: copies of the engine's default
	 * pipelines for glTF, FBX and OBJ, or images, with the plugin's policy set
	 * (Nanite and collision from Project Settings, lightmap UVs off, scale 1,
	 * materials and textures imported). Empty for other files, which keep the
	 * project's stack. OutNotes names any policy property this engine lacks.
	 */
	TArray<UObject*> ImportPipelines(const FString& File, TArray<FString>& OutNotes);

	/** Sets a reflected property by dotted path (MeshPipeline.bBuildNanite) from its text form. False when the path does not exist. */
	bool SetPropertyByPath(UObject* Root, const FString& Path, const FString& Value);

	/** sRGB, compression, mips and size for the map's role (albedo, normal, roughness, smoothness, metallic, height, ao, panorama, main). An empty role keeps the importer's color settings. */
	void ConfigureTexture(UTexture* Texture, const FString& Role, int32 MaxSize);

	/**
	 * Mesh imports: textures that a material samples as metallic, roughness,
	 * occlusion or packed masks become linear (glTF metallic-roughness maps
	 * arrive as data, not color), and normal maps get normal compression.
	 * Reads the material instances among the objects and the meshes' slots.
	 * Returns how many textures changed.
	 */
	int32 FixMeshTextures(const TArray<UObject*>& Objects);

	/** Which lit master the project needs. */
	struct FMaterialVariant
	{
		/** Substrate as the renderer runs now (r.Substrate). */
		bool bSubstrate = false;
		/** Project Settings > Rendering > Substrate materials, as saved in config (applies after a restart). */
		bool bProjectSetting = false;
		/** M_Scenario_PBR, or M_Scenario_PBR_Substrate. */
		FString MasterName;
		/** One line for the import result, naming the master and why. */
		FString Describe() const;
	};
	FMaterialVariant MaterialVariant();

	/**
	 * The project's copy of a master material under <root>/_Shared, created on
	 * first use. Pbr follows MaterialVariant(): a Substrate slab graph renders
	 * black when Substrate is off, and a legacy graph is converted by the
	 * engine when it is on, so the variant matches the running renderer.
	 */
	UMaterialInterface* Master(EScenarioMaster Which, FScenarioChangeReport& Report);
	/** The lit master for one variant, whatever the project runs (tests build both). */
	UMaterialInterface* PbrMaster(bool bSubstrate, FScenarioChangeReport& Report);

	/** The PBR master's parameter for a texture role; None for roles it has no input for (height, smoothness). */
	FName PbrParameterForRole(const FString& Role);

	/** Inverts the color channels of an image as data (alpha kept): 255 - v for 8 bits, 65535 - v for 16, 1 - v for float. */
	bool InvertImage(FImage& Image);

	/**
	 * A roughness texture made from a smoothness texture: its source pixels
	 * inverted as linear data, next to it under Name, with roughness settings
	 * (linear, mask compression). The smoothness texture is kept. Null with a
	 * warning when the source has no pixels.
	 */
	UTexture2D* CreateRoughnessFromSmoothness(UTexture2D* Smoothness, const FString& Name, int32 MaxSize, FScenarioChangeReport& Report);

	struct FRoleTexture
	{
		FString Role;
		UTexture* Texture = nullptr;
	};

	/**
	 * The material lane's maps by PBR parameter. A smoothness map becomes a new
	 * roughness map (the "_S" name becomes "_R") when bInvertSmoothness is set,
	 * or goes to Roughness as it is otherwise; an explicit roughness map wins.
	 * OutDerived maps each new texture to the texture it came from.
	 */
	TMap<FName, UTexture*> PbrMaps(const TArray<FRoleTexture>& Textures, bool bInvertSmoothness, int32 MaxSize, FScenarioChangeReport& Report, TMap<UTexture*, UTexture*>& OutDerived);

	UMaterialInstanceConstant* CreateMaterialInstance(UMaterialInterface* Parent, const FString& PackagePath, const FString& Name,
		const TMap<FName, UTexture*>& Textures, const TMap<FName, float>& Scalars, FScenarioChangeReport& Report);

	/** Copies a video under Content/Movies/Scenario/<date>/ and creates a File Media Source pointing at it. */
	UObject* CreateMediaSource(const FString& VideoFile, const FString& PackagePath, const FString& Name, FScenarioChangeReport& Report);

	/**
	 * The import policy on a static mesh, after its build: collision from
	 * Project Settings with at least one simple shape, Nanite only when the
	 * setting is on and every material slot is opaque or masked, no generated
	 * lightmap UVs. Adds one check line per rule to OutChecks when given.
	 */
	void ConfigureStaticMesh(UObject* Mesh, TArray<FString>* OutChecks = nullptr);

	/** Renames an imported asset in place (references fixed). Returns the asset under its new name. */
	UObject* RenameAsset(UObject* Asset, const FString& NewName);

	/**
	 * Scale, pivot, triangles, materials, bones and morph targets, one line
	 * each. Warnings start with "Check:". With ExpectedSizeCm, the largest side
	 * is compared with it (within half to double); otherwise with a plausible
	 * range (5 cm to 100 m).
	 */
	void CheckMesh(UObject* Mesh, TArray<FString>& OutChecks, double ExpectedSizeCm = 0.0);

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
	/**
	 * When the active level viewport has a fixed exposure (its Exposure menu),
	 * a sentence saying so: the override bypasses the post process volume's
	 * exposure in that viewport only, so captures and the game can look
	 * different from it. Empty otherwise.
	 */
	FString ExposureOverrideNote();
	/** The sentence for a viewport exposure state: empty when bFixed is false. */
	FString ExposureNote(bool bFixed, float FixedEV100);
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
