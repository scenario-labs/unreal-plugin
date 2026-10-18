// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioAssets.h"

#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextureCube.h"
#include "Engine/World.h"
#include "ScenarioSettings.h"
#include "EngineUtils.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "LevelEditorViewport.h"
#include "MediaPlaylist.h"
#include "MediaSource.h"
#include "Sound/AmbientSound.h"
#include "Sound/SoundBase.h"

namespace
{
	AActor* Spawn(UClass* Class, const FVector& Location, const FRotator& Rotation, const FString& Label, FScenarioChangeReport& Report)
	{
		UWorld* World = ScenarioPlacement::EditorWorld();
		if (!World || !Class)
		{
			Report.Warnings.Add(TEXT("No editor level is open."));
			return nullptr;
		}
		ULevel* Level = World->GetCurrentLevel();
		Level->Modify();
		FActorSpawnParameters Parameters;
		Parameters.OverrideLevel = Level;
		Parameters.ObjectFlags = RF_Transactional;
		AActor* Actor = World->SpawnActor(Class, &Location, &Rotation, Parameters);
		if (!Actor)
		{
			Report.Warnings.Add(FString::Printf(TEXT("Could not spawn %s."), *Class->GetName()));
			return nullptr;
		}
		Actor->SetActorLabel(Label);
		Report.SpawnedActors.Add(ScenarioPlacement::Describe(Actor));
		return Actor;
	}

	UStaticMesh* BasicShape(const TCHAR* Name)
	{
		return LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), Name, Name));
	}
}

UWorld* ScenarioPlacement::EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

ScenarioPlacement::FView ScenarioPlacement::ActiveView()
{
	FView View;
	FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
	if ((!Client || !Client->IsPerspective()) && GEditor)
	{
		for (FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
		{
			if (Candidate && Candidate->IsPerspective())
			{
				Client = Candidate;
				break;
			}
		}
	}
	if (Client)
	{
		View.bValid = true;
		View.Location = Client->GetViewLocation();
		View.Rotation = Client->GetViewRotation();
		View.Fov = Client->ViewFOV;
	}
	return View;
}

TArray<AActor*> ScenarioPlacement::ResolveTargets(const TArray<FString>& Names)
{
	TArray<AActor*> Actors;
	if (Names.Num() == 0)
	{
		if (GEditor)
		{
			GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Actors);
		}
		return Actors;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		return Actors;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (Names.Contains(It->GetName()) || Names.Contains(It->GetActorLabel()))
		{
			Actors.Add(*It);
		}
	}
	return Actors;
}

FString ScenarioPlacement::Describe(const AActor* Actor)
{
	return Actor ? FString::Printf(TEXT("%s (%s)"), *Actor->GetActorLabel(), *Actor->GetName()) : FString();
}

FVector ScenarioPlacement::GroundBelow(UWorld* World, const FVector& Location)
{
	if (!World)
	{
		return Location;
	}
	FHitResult Hit;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ScenarioGround), true);
	const FVector Start = Location + FVector(0.0, 0.0, 200.0);
	const FVector End = Location - FVector(0.0, 0.0, 100000.0);
	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Query))
	{
		return Hit.ImpactPoint;
	}
	return Location;
}

FVector ScenarioPlacement::Location(const FScenarioPlaceOptions& Options, float Distance, bool bSnapToGround)
{
	FVector Point = FVector::ZeroVector;
	switch (Options.Placement)
	{
	case EScenarioPlacement::AtLocation:
		Point = Options.Location;
		break;
	case EScenarioPlacement::AtPivot:
		Point = GEditor ? GEditor->GetPivotLocation() : FVector::ZeroVector;
		break;
	default:
	{
		const FView View = ActiveView();
		Point = View.bValid ? View.Location + View.Rotation.Vector() * Distance : FVector::ZeroVector;
		break;
	}
	}
	return bSnapToGround ? GroundBelow(EditorWorld(), Point) : Point;
}

FRotator ScenarioPlacement::FacingCamera(const FVector& Location)
{
	const FView View = ActiveView();
	if (!View.bValid)
	{
		return FRotator::ZeroRotator;
	}
	const FVector ToCamera = (View.Location - Location).GetSafeNormal2D();
	return FRotator(0.0, ToCamera.Rotation().Yaw, 0.0);
}

AActor* ScenarioPlacement::SpawnMesh(UObject* Mesh, const FScenarioPlaceOptions& Options, const FString& Label, FScenarioChangeReport& Report)
{
	FBox Bounds(ForceInit);
	if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Mesh))
	{
		Bounds = StaticMesh->GetBoundingBox();
	}
	else if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh))
	{
		Bounds = SkeletalMesh->GetBounds().GetBox();
	}
	else
	{
		Report.Warnings.Add(TEXT("Nothing to place: the import made no mesh."));
		return nullptr;
	}
	const double Radius = Bounds.IsValid ? Bounds.GetExtent().Size() : 50.0;
	FVector Point = Location(Options, static_cast<float>(FMath::Max(300.0, Radius * 3.0)), true);
	// Rest the mesh on the ground whatever its pivot.
	if (Bounds.IsValid)
	{
		Point.Z -= Bounds.Min.Z;
	}
	const FRotator Rotation = FacingCamera(Point);

	AActor* Actor = nullptr;
	if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Mesh))
	{
		AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Spawn(AStaticMeshActor::StaticClass(), Point, Rotation, Label, Report));
		if (MeshActor)
		{
			MeshActor->GetStaticMeshComponent()->SetStaticMesh(StaticMesh);
			if (UScenarioProjectSettings::Get().MeshCollision == EScenarioMeshCollision::None)
			{
				MeshActor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			}
		}
		Actor = MeshActor;
	}
	else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh))
	{
		ASkeletalMeshActor* MeshActor = Cast<ASkeletalMeshActor>(Spawn(ASkeletalMeshActor::StaticClass(), Point, Rotation, Label, Report));
		if (MeshActor)
		{
			MeshActor->GetSkeletalMeshComponent()->SetSkeletalMesh(SkeletalMesh);
		}
		Actor = MeshActor;
	}
	return Actor;
}

AActor* ScenarioPlacement::SpawnImagePlane(UMaterialInterface* Material, float Aspect, const FScenarioPlaceOptions& Options, const FString& Label, FScenarioChangeReport& Report)
{
	UStaticMesh* Plane = BasicShape(TEXT("Plane"));
	if (!Plane || !Material)
	{
		Report.Warnings.Add(TEXT("The image plane could not be built."));
		return nullptr;
	}
	const FVector Point = Location(Options, 300.0f, false);
	const FView View = ActiveView();
	// The plane's +Z faces the camera, its +X points up.
	const FVector Normal = View.bValid ? (View.Location - Point).GetSafeNormal2D() : FVector(-1.0, 0.0, 0.0);
	const FRotator Rotation = FRotationMatrix::MakeFromZX(Normal, FVector::UpVector).Rotator();
	AStaticMeshActor* Actor = Cast<AStaticMeshActor>(Spawn(AStaticMeshActor::StaticClass(), Point, Rotation, Label, Report));
	if (!Actor)
	{
		return nullptr;
	}
	const double Height = 200.0;
	const double Width = Height * FMath::Max(0.1f, Aspect);
	Actor->SetActorScale3D(FVector(Height / 100.0, Width / 100.0, 1.0));
	UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
	Component->SetStaticMesh(Plane);
	Component->SetMaterial(0, Material);
	Component->SetCastShadow(false);
	return Actor;
}

AActor* ScenarioPlacement::SpawnPreviewSphere(UMaterialInterface* Material, const FScenarioPlaceOptions& Options, const FString& Label, FScenarioChangeReport& Report)
{
	UStaticMesh* Sphere = BasicShape(TEXT("Sphere"));
	if (!Sphere)
	{
		return nullptr;
	}
	FVector Point = Location(Options, 300.0f, true);
	Point.Z += 50.0;
	AStaticMeshActor* Actor = Cast<AStaticMeshActor>(Spawn(AStaticMeshActor::StaticClass(), Point, FRotator::ZeroRotator, Label, Report));
	if (Actor)
	{
		Actor->GetStaticMeshComponent()->SetStaticMesh(Sphere);
		Actor->GetStaticMeshComponent()->SetMaterial(0, Material);
	}
	return Actor;
}

bool ScenarioPlacement::ApplyMaterial(AActor* Actor, int32 Slot, UMaterialInterface* Material, FScenarioChangeReport& Report)
{
	if (!Actor || !Material)
	{
		return false;
	}
	TArray<UMeshComponent*> Components;
	Actor->GetComponents<UMeshComponent>(Components);
	if (Components.Num() == 0)
	{
		Report.Warnings.Add(FString::Printf(TEXT("%s has no mesh to take a material."), *Actor->GetActorLabel()));
		return false;
	}
	UMeshComponent* Component = Components[0];
	const int32 Slots = Component->GetNumMaterials();
	if (Slot < 0 || (Slots > 0 && Slot >= Slots))
	{
		Report.Warnings.Add(FString::Printf(TEXT("%s has %d material slot(s); slot %d does not exist."), *Actor->GetActorLabel(), Slots, Slot));
		return false;
	}
	Actor->Modify();
	Component->Modify();
	Component->SetMaterial(Slot, Material);
	Report.ModifiedActors.Add(FString::Printf(TEXT("%s slot %d"), *Describe(Actor), Slot));
	return true;
}

AActor* ScenarioPlacement::SpawnAmbientSound(USoundBase* Sound, const FScenarioPlaceOptions& Options, const FString& Label, FScenarioChangeReport& Report)
{
	if (!Sound)
	{
		return nullptr;
	}
	const FVector Point = Location(Options, 300.0f, true);
	AAmbientSound* Actor = Cast<AAmbientSound>(Spawn(AAmbientSound::StaticClass(), Point, FRotator::ZeroRotator, Label, Report));
	if (Actor)
	{
		Actor->GetAudioComponent()->SetSound(Sound);
	}
	return Actor;
}

AActor* ScenarioPlacement::SpawnSkySphere(UMaterialInterface* Material, const FString& Label, FScenarioChangeReport& Report)
{
	UStaticMesh* Sphere = BasicShape(TEXT("Sphere"));
	if (!Sphere || !Material)
	{
		return nullptr;
	}
	AStaticMeshActor* Actor = Cast<AStaticMeshActor>(Spawn(AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Label, Report));
	if (!Actor)
	{
		return nullptr;
	}
	// The basic sphere has a 50 cm radius: 2000x puts the sky 1 km away.
	Actor->SetActorScale3D(FVector(2000.0));
	UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
	Component->SetStaticMesh(Sphere);
	Component->SetMaterial(0, Material);
	Component->SetCastShadow(false);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	return Actor;
}

AActor* ScenarioPlacement::SpawnHdriBackdrop(UTextureCube* Cubemap, const FString& Label, FScenarioChangeReport& Report)
{
	// Loaded by path so the plugin does not depend on the HDRI Backdrop plugin.
	UClass* Class = LoadClass<AActor>(nullptr, TEXT("/HDRIBackdrop/Blueprints/HDRIBackdrop.HDRIBackdrop_C"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Class)
	{
		Report.Warnings.Add(TEXT("Enable the HDRI Backdrop plugin (Edit > Plugins) to place HDR skies. The cubemap was imported."));
		return nullptr;
	}
	AActor* Actor = Spawn(Class, FVector::ZeroVector, FRotator::ZeroRotator, Label, Report);
	if (!Actor || !Cubemap)
	{
		return Actor;
	}
	if (FObjectProperty* Property = FindFProperty<FObjectProperty>(Class, TEXT("Cubemap")))
	{
		Actor->Modify();
		Property->SetObjectPropertyValue_InContainer(Actor, Cubemap);
		FPropertyChangedEvent Changed(Property);
		Actor->PostEditChangeProperty(Changed);
	}
	else
	{
		Report.Warnings.Add(TEXT("The HDRI Backdrop has no Cubemap property in this engine version: set it by hand."));
	}
	return Actor;
}

AActor* ScenarioPlacement::SpawnMediaPlate(UObject* MediaSource, const FScenarioPlaceOptions& Options, const FString& Label, FScenarioChangeReport& Report)
{
	UClass* Class = LoadClass<AActor>(nullptr, TEXT("/Script/MediaPlate.MediaPlate"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Class)
	{
		Report.Warnings.Add(TEXT("Enable the Media Plate plugin (Edit > Plugins) to place videos. The media source was created."));
		return nullptr;
	}
	const FVector Point = Location(Options, 400.0f, false);
	const FRotator Rotation = FacingCamera(Point);
	AActor* Actor = Spawn(Class, Point, Rotation, Label, Report);
	if (!Actor)
	{
		return nullptr;
	}
	// The Media Plate is reached by reflection: its component holds a playlist the source goes into.
	UObject* Component = nullptr;
	if (FObjectProperty* ComponentProperty = FindFProperty<FObjectProperty>(Class, TEXT("MediaPlateComponent")))
	{
		Component = ComponentProperty->GetObjectPropertyValue_InContainer(Actor);
	}
	UMediaPlaylist* Playlist = nullptr;
	if (Component)
	{
		if (FObjectProperty* PlaylistProperty = FindFProperty<FObjectProperty>(Component->GetClass(), TEXT("MediaPlaylist")))
		{
			Playlist = Cast<UMediaPlaylist>(PlaylistProperty->GetObjectPropertyValue_InContainer(Component));
		}
	}
	UMediaSource* Source = Cast<UMediaSource>(MediaSource);
	if (Playlist && Source)
	{
		Playlist->Modify();
		Playlist->Add(Source);
		if (UFunction* Open = Component->FindFunction(TEXT("Open")))
		{
			Component->ProcessEvent(Open, nullptr);
		}
	}
	else
	{
		Report.Warnings.Add(TEXT("The Media Plate was placed; drag the media source onto it (its source could not be set automatically in this engine version)."));
	}
	return Actor;
}

void ScenarioPlacement::Select(const TArray<AActor*>& Actors)
{
	if (!GEditor || Actors.Num() == 0)
	{
		return;
	}
	GEditor->SelectNone(false, true);
	for (AActor* Actor : Actors)
	{
		if (Actor)
		{
			GEditor->SelectActor(Actor, true, false);
		}
	}
	GEditor->NoteSelectionChange();
}
