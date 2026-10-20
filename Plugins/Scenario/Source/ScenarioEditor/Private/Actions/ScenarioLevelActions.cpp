// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

// Level and project actions: reads, captures, landing results, saving, Python.

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "IPythonScriptPlugin.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "ScenarioActionHelpers.h"
#include "ScenarioActions.h"
#include "ScenarioAssets.h"
#include "ScenarioEditorModule.h"
#include "ScenarioLanes.h"
#include "ScenarioOpenWorld.h"
#include "ScenarioOutputLog.h"
#include "ScenarioProvenance.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"
#include "WorldPartition/WorldPartition.h"

#define LOCTEXT_NAMESPACE "ScenarioLevelActions"

using namespace ScenarioActionHelpers;

namespace
{
	using FRunFn = TFunction<void(const TSharedPtr<FJsonObject>&, const FScenarioCaller&, FScenarioActionDone)>;

	FScenarioAction Make(const FString& Name, EScenarioActionKind Kind, const FString& Description, const TSharedPtr<FJsonObject>& Schema, FRunFn Run)
	{
		FScenarioAction Action;
		Action.Name = Name;
		Action.Kind = Kind;
		Action.Description = Description;
		Action.InputSchema = Schema;
		Action.Run = MoveTemp(Run);
		return Action;
	}

	TSharedRef<FJsonValue> Vec(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Items = {
			MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.X * 10.0) / 10.0),
			MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Y * 10.0) / 10.0),
			MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Z * 10.0) / 10.0),
		};
		return MakeShared<FJsonValueArray>(Items);
	}

	TSharedRef<FJsonValue> Rot(const FRotator& R)
	{
		return Vec(FVector(R.Pitch, R.Yaw, R.Roll));
	}

	/** Reads [x, y, z] in meters as centimeters. */
	bool ReadMeters(const TSharedPtr<FJsonObject>& Args, const FString& Name, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Args->TryGetArrayField(Name, Items) || Items->Num() != 3)
		{
			return false;
		}
		for (int32 Index = 0; Index < 3; ++Index)
		{
			double Value = 0.0;
			if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetNumber(Value))
			{
				return false;
			}
			Out[Index] = Value * 100.0;
		}
		return true;
	}

	/** screenshot_viewport's camera: location_m, then look_at_m or pitch and yaw, and fov. */
	bool ReadCamera(const TSharedPtr<FJsonObject>& Args, FScenarioCaptureCamera& Out, FString& OutError)
	{
		if (!ReadMeters(Args, TEXT("location_m"), Out.Location))
		{
			OutError = TEXT("camera needs location_m: [x, y, z] in meters.");
			return false;
		}
		FVector Target;
		if (ReadMeters(Args, TEXT("look_at_m"), Target))
		{
			if (Target.Equals(Out.Location, 1.0))
			{
				OutError = TEXT("camera look_at_m is the camera location.");
				return false;
			}
			Out.Rotation = (Target - Out.Location).Rotation();
		}
		else
		{
			double Pitch = 0.0;
			double Yaw = 0.0;
			Args->TryGetNumberField(TEXT("pitch"), Pitch);
			Args->TryGetNumberField(TEXT("yaw"), Yaw);
			Out.Rotation = FRotator(FMath::Clamp(Pitch, -89.0, 89.0), Yaw, 0.0);
		}
		double Fov = 70.0;
		Args->TryGetNumberField(TEXT("fov"), Fov);
		Out.Fov = FMath::Clamp(Fov, 10.0, 150.0);
		return true;
	}

	bool FromScenario(const AActor* Actor)
	{
		return Actor && Actor->Tags.Contains(ScenarioPlacement::ActorTag);
	}

	/** Scenario provenance of an asset: the index entry, as JSON. */
	TSharedPtr<FJsonObject> ProvenanceOf(const FString& ObjectPath)
	{
		UScenarioSubsystem* S = UScenarioSubsystem::Get();
		const TMap<FName, FString>* Values = S ? S->Provenance().Find(ObjectPath) : nullptr;
		if (!Values)
		{
			return nullptr;
		}
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		for (const TPair<FName, FString>& Pair : *Values)
		{
			if (!Pair.Value.IsEmpty())
			{
				// "Scenario.ModelId" -> "ModelId"
				FString Key = Pair.Key.ToString();
				Key.RemoveFromStart(TEXT("Scenario."));
				Json->SetStringField(Key, Pair.Value.Left(400));
			}
		}
		return Json;
	}

	FString PythonName(const FString& Name)
	{
		FString Out;
		for (int32 Index = 0; Index < Name.Len(); ++Index)
		{
			const TCHAR C = Name[Index];
			if (FChar::IsUpper(C) && Index > 0 && (FChar::IsLower(Name[Index - 1]) || (Index + 1 < Name.Len() && FChar::IsLower(Name[Index + 1]))))
			{
				Out.AppendChar(TEXT('_'));
			}
			Out.AppendChar(FChar::ToLower(C));
		}
		return Out;
	}

	FString PropertyType(const FProperty* Property)
	{
		return Property ? Property->GetCPPType() : FString();
	}

	EScenarioPlacement PlacementFromText(const FString& Text, bool bHasTargets)
	{
		if (Text == TEXT("in_front_of_camera")) return EScenarioPlacement::InFrontOfCamera;
		if (Text == TEXT("at_pivot")) return EScenarioPlacement::AtPivot;
		if (Text == TEXT("at_location")) return EScenarioPlacement::AtLocation;
		if (Text == TEXT("on_selection")) return EScenarioPlacement::OnSelection;
		return bHasTargets ? EScenarioPlacement::OnSelection : EScenarioPlacement::Auto;
	}

	FString DescribePlacement(const FApplication& App)
	{
		const FScenarioPlaceOptions& O = App.Options;
		const FString Targets = O.TargetActors.Num() > 0 ? FString::Join(O.TargetActors, TEXT(", ")) : TEXT("the selection");
		switch (O.Placement)
		{
		case EScenarioPlacement::None: return TEXT("Content Browser only, nothing placed in the level.");
		case EScenarioPlacement::InFrontOfCamera: return TEXT("Places it in front of the viewport camera.");
		case EScenarioPlacement::AtPivot: return FString::Printf(TEXT("Places it at the pivot of %s."), *Targets);
		case EScenarioPlacement::AtLocation: return FString::Printf(TEXT("Places it at (%.0f, %.0f, %.0f)."), O.Location.X, O.Location.Y, O.Location.Z);
		case EScenarioPlacement::OnSelection: return App.Purpose == TEXT("material")
			? FString::Printf(TEXT("Sets material slot %d of %s."), O.MaterialSlot, *Targets)
			: FString::Printf(TEXT("Applies it to %s."), *Targets);
		default: return TEXT("Places it where this lane lands by default (image plane, mesh in front of the camera, sky, ambient sound).");
		}
	}

	/** Runs an application: import (and place) once, or place assets already imported. */
	void RunApplication(const FString& ApplicationId, FScenarioActionDone Done)
	{
		FScenarioActionResult Fail;
		UScenarioSubsystem* S = Subsystem(Fail, false);
		FApplication* App = Applications().Find(ApplicationId);
		if (!S || !App)
		{
			Done(S ? FScenarioActionResult::Fail(TEXT("NotFound"), TEXT("Unknown application_id.")) : Fail);
			return;
		}
		if (App->bApplied)
		{
			Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("This application was already applied. Prepare a new one.")));
			return;
		}
		const FScenarioJobRow* Row = S->Session().Ledger().Find(App->LocalId);
		if (!Row)
		{
			Done(FScenarioActionResult::Fail(TEXT("NotFound"), TEXT("The job left the list.")));
			return;
		}
		App->bApplied = true;
		const FString LocalId = App->LocalId;
		auto Answer = [LocalId, Done](const FScenarioChangeReport& Report)
		{
			if (!Report.bOk && !Report.Error.IsEmpty())
			{
				Done(FScenarioActionResult::Fail(TEXT("Apply"), Report.Error));
				return;
			}
			if (!Report.ChangedAnything())
			{
				Done(FScenarioActionResult::Fail(TEXT("NoChange"), Report.Warnings.Num() > 0 ? FString::Join(Report.Warnings, TEXT(" ")) : TEXT("Nothing was created or changed.")));
				return;
			}
			TSharedRef<FJsonObject> Json = Report.ToJson();
			Json->SetStringField(TEXT("job_id"), LocalId);
			Json->SetStringField(TEXT("undo"), TEXT("Level changes are one Ctrl+Z (\"Scenario: ...\"). Imported assets stay until remove_scenario_items. The level is not saved: save_changes saves it."));
			Done(FScenarioActionResult::Ok(Json));
		};
		if (!Row->bImported)
		{
			S->ImportJob(LocalId, App->Options, Answer);
			return;
		}
		if (App->Options.Placement == EScenarioPlacement::None)
		{
			Done(FScenarioActionResult::Fail(TEXT("NoChange"), TEXT("This job is already imported; choose a placement to place it.")));
			return;
		}
		Answer(S->PlaceJob(LocalId, App->Options));
	}
}

void FScenarioActions::RegisterLevelActions()
{
	// scene_summary
	Register(Make(TEXT("scene_summary"), EScenarioActionKind::Read,
		TEXT("The open level: World Partition state (only loaded actors are listed there), one file per actor, data layers, selection, viewport camera, actor counts by class, and a page of actors with labels, ids, classes, locations and tags."),
		Object({
			{ TEXT("class"), TEXT("string"), TEXT("Only actors whose class name contains this.") },
			{ TEXT("page"), TEXT("integer") },
			{ TEXT("page_size"), TEXT("integer"), TEXT("Default 50, max 200.") },
		}),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			UWorld* World = ScenarioPlacement::EditorWorld();
			if (!World)
			{
				Done(FScenarioActionResult::Fail(TEXT("Unavailable"), TEXT("No editor level is open.")));
				return;
			}
			const FString ClassFilter = Str(Args, TEXT("class"));
			const int32 PageSize = Int(Args, TEXT("page_size"), 50, 1, 200);
			const int32 Page = Int(Args, TEXT("page"), 0, 0, 100000);
			TMap<FString, int32> Counts;
			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Listed = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor || !Actor->IsListedInSceneOutliner())
				{
					continue;
				}
				const FString Class = Actor->GetClass()->GetName();
				Counts.FindOrAdd(Class)++;
				if (!ClassFilter.IsEmpty() && !Class.Contains(ClassFilter))
				{
					continue;
				}
				if (Listed >= Page * PageSize && Rows.Num() < PageSize)
				{
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("label"), Actor->GetActorLabel());
					Row->SetStringField(TEXT("id"), Actor->GetName());
					Row->SetStringField(TEXT("class"), Class);
					Row->SetField(TEXT("location"), Vec(Actor->GetActorLocation()));
					if (Actor->Tags.Num() > 0)
					{
						TArray<FString> Tags;
						for (const FName& Tag : Actor->Tags)
						{
							Tags.Add(Tag.ToString());
						}
						Row->SetField(TEXT("tags"), ScenarioJson::StringArrayValue(Tags));
					}
					if (FromScenario(Actor))
					{
						Row->SetBoolField(TEXT("placed_by_scenario"), true);
					}
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				}
				++Listed;
			}
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("level"), World->GetOutermost()->GetName());
			Json->SetBoolField(TEXT("unsaved_changes"), World->GetOutermost()->IsDirty());
			const ScenarioOpenWorld::FState OpenWorld = ScenarioOpenWorld::Read(World);
			Json->SetBoolField(TEXT("world_partition"), OpenWorld.bWorldPartition);
			if (OpenWorld.bWorldPartition || OpenWorld.bExternalActors)
			{
				Json->SetObjectField(TEXT("open_world"), ScenarioOpenWorld::ToJson(OpenWorld));
				const TArray<FString> Notes = ScenarioOpenWorld::SummaryNotes(OpenWorld);
				Json->SetStringField(TEXT("note"), Notes.Num() > 0 ? Notes[0] : FString());
				Json->SetField(TEXT("notes"), ScenarioJson::StringArrayValue(Notes));
			}
			TArray<FString> Selected;
			for (AActor* Actor : ScenarioPlacement::ResolveTargets({}))
			{
				Selected.Add(ScenarioPlacement::Describe(Actor));
			}
			Json->SetField(TEXT("selection"), ScenarioJson::StringArrayValue(Selected));
			const ScenarioPlacement::FView View = ScenarioPlacement::ActiveView();
			if (View.bValid)
			{
				TSharedRef<FJsonObject> Camera = MakeShared<FJsonObject>();
				Camera->SetField(TEXT("location"), Vec(View.Location));
				Camera->SetField(TEXT("rotation"), Rot(View.Rotation));
				Camera->SetNumberField(TEXT("fov"), FMath::RoundToDouble(View.Fov));
				Json->SetObjectField(TEXT("viewport_camera"), Camera);
			}
			TSharedRef<FJsonObject> CountJson = MakeShared<FJsonObject>();
			Counts.ValueSort([](int32 A, int32 B) { return A > B; });
			int32 Shown = 0;
			for (const TPair<FString, int32>& Pair : Counts)
			{
				if (Shown++ >= 30)
				{
					break;
				}
				CountJson->SetNumberField(Pair.Key, Pair.Value);
			}
			Json->SetObjectField(TEXT("actor_counts"), CountJson);
			Json->SetArrayField(TEXT("actors"), Rows);
			Json->SetNumberField(TEXT("actors_matching"), Listed);
			if (Listed > (Page + 1) * PageSize)
			{
				Json->SetNumberField(TEXT("next_page"), Page + 1);
			}
			Done(FScenarioActionResult::Ok(Json));
		}));

	// object_detail
	Register(Make(TEXT("object_detail"), EScenarioActionKind::Read,
		TEXT("One actor by id or label: class, transform, bounds, components, meshes and materials per slot, and Scenario provenance."),
		Object({ { TEXT("actor_id"), TEXT("string") } }, { TEXT("actor_id") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			const TArray<AActor*> Found = ScenarioPlacement::ResolveTargets({ Str(Args, TEXT("actor_id"), TEXT("?")) });
			if (Found.Num() == 0)
			{
				Done(FScenarioActionResult::Fail(TEXT("NotFound"), TEXT("No actor with that id or label (scene_summary lists them).")));
				return;
			}
			AActor* Actor = Found[0];
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("label"), Actor->GetActorLabel());
			Json->SetStringField(TEXT("id"), Actor->GetName());
			Json->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
			Json->SetStringField(TEXT("folder"), Actor->GetFolderPath().ToString());
			Json->SetField(TEXT("location"), Vec(Actor->GetActorLocation()));
			Json->SetField(TEXT("rotation"), Rot(Actor->GetActorRotation()));
			Json->SetField(TEXT("scale"), Vec(Actor->GetActorScale3D()));
			FVector Origin;
			FVector Extent;
			Actor->GetActorBounds(false, Origin, Extent);
			Json->SetField(TEXT("bounds_center"), Vec(Origin));
			Json->SetField(TEXT("bounds_size"), Vec(Extent * 2.0));
			if (FromScenario(Actor))
			{
				Json->SetBoolField(TEXT("placed_by_scenario"), true);
			}
			TArray<TSharedPtr<FJsonValue>> Components;
			TSharedPtr<FJsonObject> Provenance;
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (!Component || Components.Num() >= 30)
				{
					continue;
				}
				TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
				C->SetStringField(TEXT("name"), Component->GetName());
				C->SetStringField(TEXT("class"), Component->GetClass()->GetName());
				if (const UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Component))
				{
					if (const UStaticMesh* Mesh = MeshComponent->GetStaticMesh())
					{
						C->SetStringField(TEXT("mesh"), Mesh->GetPathName());
						if (!Provenance.IsValid())
						{
							Provenance = ProvenanceOf(Mesh->GetPathName());
						}
					}
				}
				if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
				{
					TArray<TSharedPtr<FJsonValue>> Slots;
					for (int32 Slot = 0; Slot < FMath::Min(Primitive->GetNumMaterials(), 16); ++Slot)
					{
						const UMaterialInterface* Material = Primitive->GetMaterial(Slot);
						const FString Path = Material ? Material->GetPathName() : FString(TEXT("none"));
						Slots.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%d: %s"), Slot, *Path)));
						if (Material && !Provenance.IsValid())
						{
							Provenance = ProvenanceOf(Material->GetPathName());
						}
					}
					if (Slots.Num() > 0)
					{
						C->SetArrayField(TEXT("materials"), Slots);
					}
				}
				Components.Add(MakeShared<FJsonValueObject>(C));
			}
			Json->SetArrayField(TEXT("components"), Components);
			if (Provenance.IsValid())
			{
				Json->SetObjectField(TEXT("scenario"), Provenance);
			}
			Done(FScenarioActionResult::Ok(Json));
		}));

	// screenshot_viewport
	Register(Make(TEXT("screenshot_viewport"), EScenarioActionKind::Read,
		TEXT("Image of the active level viewport's camera (no gizmos), returned to you, with the world, camera, FOV and size it shows. ")
		TEXT("Pass camera to render another view without moving the person's viewport (also works with no viewport open). ")
		TEXT("frame_check flags all-black, all-white, uniform, clipped or crushed frames; exposure_override says when the viewport's fixed exposure differs from the game's."),
		Object({
			{ TEXT("width"), TEXT("integer"), TEXT("Default 1024, max 1920.") },
			{ TEXT("height"), TEXT("integer"), TEXT("Default 576, max 1080.") },
			{ TEXT("camera"), TEXT("object"), TEXT("Optional view: location_m [x, y, z] in meters, then look_at_m [x, y, z] or pitch and yaw in degrees, and fov (default 70).") },
		}),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, false);
			if (!S)
			{
				Done(Fail);
				return;
			}
			FString Error;
			FString Description;
			FScenarioCaptureCamera Camera;
			const TSharedPtr<FJsonObject> CameraArgs = Obj(Args, TEXT("camera"));
			if (CameraArgs.IsValid() && !ReadCamera(CameraArgs, Camera, Error))
			{
				Done(FScenarioActionResult::Fail(TEXT("Invalid"), Error));
				return;
			}
			FScenarioFrameCheck Check;
			const FString Path = S->CaptureViewport(Int(Args, TEXT("width"), 1024, 128, 1920), Int(Args, TEXT("height"), 576, 128, 1080), Error, &Description,
				CameraArgs.IsValid() ? &Camera : nullptr, &Check);
			TArray<uint8> Png;
			if (Path.IsEmpty() || !FFileHelper::LoadFileToArray(Png, *Path))
			{
				Done(FScenarioActionResult::Fail(TEXT("Capture"), Error.IsEmpty() ? TEXT("The capture could not be read.") : Error));
				return;
			}
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("captured"), Description);
			Json->SetStringField(TEXT("file"), ProjectRelative(Path));
			Json->SetObjectField(TEXT("frame_check"), Check.ToJson());
			const FString Exposure = ScenarioPlacement::ExposureOverrideNote();
			if (!Exposure.IsEmpty())
			{
				Json->SetStringField(TEXT("exposure_override"), Exposure);
			}
			FScenarioActionResult Result = FScenarioActionResult::Ok(Json);
			Result.Png = MoveTemp(Png);
			Done(Result);
		}));

	// find_assets
	Register(Make(TEXT("find_assets"), EScenarioActionKind::Read,
		TEXT("Content Browser search by folder, class, name and Scenario origin. Reads the asset registry; loads nothing."),
		Object({
			{ TEXT("path"), TEXT("string"), TEXT("Content folder, default /Game.") },
			{ TEXT("class"), TEXT("string"), TEXT("StaticMesh, Texture2D, MaterialInstanceConstant, SoundWave...") },
			{ TEXT("name"), TEXT("string"), TEXT("Part of the asset name.") },
			{ TEXT("scenario_only"), TEXT("boolean"), TEXT("Only assets imported from Scenario.") },
			{ TEXT("limit"), TEXT("integer"), TEXT("Default 50, max 200.") },
		}),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			IAssetRegistry& Registry = IAssetRegistry::GetChecked();
			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*Str(Args, TEXT("path"), TEXT("/Game"))));
			Filter.bRecursivePaths = true;
			const FString ClassName = Str(Args, TEXT("class"));
			if (!ClassName.IsEmpty())
			{
				const UClass* Class = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
				if (!Class)
				{
					Done(FScenarioActionResult::Fail(TEXT("Invalid"), FString::Printf(TEXT("Unknown class %s."), *ClassName)));
					return;
				}
				Filter.ClassPaths.Add(Class->GetClassPathName());
				Filter.bRecursiveClasses = true;
			}
			TArray<FAssetData> Assets;
			Registry.GetAssets(Filter, Assets);
			const FString Name = Str(Args, TEXT("name"));
			const bool bScenarioOnly = Bool(Args, TEXT("scenario_only"));
			const int32 Limit = Int(Args, TEXT("limit"), 50, 1, 200);
			UScenarioSubsystem* S = UScenarioSubsystem::Get();
			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Matched = 0;
			for (const FAssetData& Asset : Assets)
			{
				if (!Name.IsEmpty() && !Asset.AssetName.ToString().Contains(Name))
				{
					continue;
				}
				const FString ObjectPath = Asset.GetObjectPathString();
				const bool bScenario = S && S->Provenance().Contains(ObjectPath);
				if (bScenarioOnly && !bScenario)
				{
					continue;
				}
				++Matched;
				if (Rows.Num() >= Limit)
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("path"), ObjectPath);
				Row->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
				if (bScenario)
				{
					Row->SetBoolField(TEXT("from_scenario"), true);
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetArrayField(TEXT("assets"), Rows);
			Json->SetNumberField(TEXT("matched"), Matched);
			Done(FScenarioActionResult::Ok(Json));
		}));

	// asset_detail
	Register(Make(TEXT("asset_detail"), EScenarioActionKind::Read,
		TEXT("One asset: class, size on disk, referencers and dependencies, source file, Scenario provenance, and mesh checks (scale, triangles, bones)."),
		Object({ { TEXT("asset_path"), TEXT("string"), TEXT("Object path, for example /Game/Scenario/Meshes/SM_Well.SM_Well.") }, { TEXT("checks"), TEXT("boolean"), TEXT("Load meshes to run checks (default true).") } }, { TEXT("asset_path") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			IAssetRegistry& Registry = IAssetRegistry::GetChecked();
			FString Path = Str(Args, TEXT("asset_path"));
			if (!Path.Contains(TEXT(".")))
			{
				Path = Path + TEXT(".") + FPackageName::GetShortName(Path);
			}
			const FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
			if (!Asset.IsValid())
			{
				Done(FScenarioActionResult::Fail(TEXT("NotFound"), TEXT("No asset at that path (find_assets lists them).")));
				return;
			}
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			const FString Class = Asset.AssetClassPath.GetAssetName().ToString();
			Json->SetStringField(TEXT("path"), Asset.GetObjectPathString());
			Json->SetStringField(TEXT("class"), Class);
			FString Filename;
			if (FPackageName::DoesPackageExist(Asset.PackageName.ToString(), &Filename))
			{
				Json->SetNumberField(TEXT("size_on_disk_kb"), FMath::RoundToDouble(IFileManager::Get().FileSize(*Filename) / 1024.0));
			}
			TArray<FName> Referencers;
			Registry.GetReferencers(Asset.PackageName, Referencers);
			TArray<FName> Dependencies;
			Registry.GetDependencies(Asset.PackageName, Dependencies);
			auto Names = [](const TArray<FName>& In)
			{
				TArray<FString> Out;
				for (const FName& Item : In)
				{
					if (Out.Num() < 40)
					{
						Out.Add(Item.ToString());
					}
				}
				return ScenarioJson::StringArrayValue(Out);
			};
			Json->SetField(TEXT("referenced_by"), Names(Referencers));
			Json->SetField(TEXT("depends_on"), Names(Dependencies));
			FString ImportData;
			if (Asset.GetTagValue(TEXT("AssetImportData"), ImportData))
			{
				const TSharedPtr<FJsonValue> Parsed = ScenarioJson::ParseValue(ImportData);
				if (Parsed.IsValid() && Parsed->Type == EJson::Array && Parsed->AsArray().Num() > 0)
				{
					const FString Source = ScenarioJson::GetString(Parsed->AsArray()[0]->AsObject(), TEXT("RelativeFilename"));
					Json->SetStringField(TEXT("source_file"), FPaths::GetCleanFilename(Source));
				}
			}
			if (const TSharedPtr<FJsonObject> Provenance = ProvenanceOf(Asset.GetObjectPathString()))
			{
				Json->SetObjectField(TEXT("scenario"), Provenance);
			}
			if (Bool(Args, TEXT("checks"), true) && (Class == TEXT("StaticMesh") || Class == TEXT("SkeletalMesh")))
			{
				TArray<FString> Checks;
				ScenarioAssets::CheckMesh(Asset.GetAsset(), Checks);
				Json->SetField(TEXT("checks"), ScenarioJson::StringArrayValue(Checks));
			}
			Done(FScenarioActionResult::Ok(Json));
		}));

	// output_log
	Register(Make(TEXT("output_log"), EScenarioActionKind::Read,
		TEXT("Recent warnings and errors from the Output Log (and Scenario and Interchange messages), newest last."),
		Object({
			{ TEXT("severity"), TEXT("string"), FString(), FString(), { TEXT("errors"), TEXT("warnings"), TEXT("all") } },
			{ TEXT("category"), TEXT("string"), TEXT("Prefix, for example LogInterchange.") },
			{ TEXT("since_seconds"), TEXT("integer") },
			{ TEXT("limit"), TEXT("integer"), TEXT("Default 50, max 200.") },
		}),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			const FString Severity = Str(Args, TEXT("severity"), TEXT("warnings"));
			const TArray<FScenarioOutputLog::FEntry> Entries = FScenarioOutputLog::Get().Recent(Severity != TEXT("errors"), Severity == TEXT("all"),
				Str(Args, TEXT("category")), Int(Args, TEXT("limit"), 50, 1, 200), Int(Args, TEXT("since_seconds"), 0, 0, 86400));
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FScenarioOutputLog::FEntry& Entry : Entries)
			{
				Rows.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s %s %s: %s"), *ScenarioIsoTime(Entry.Time), *Entry.Category.ToString(),
					ToString(Entry.Verbosity), *Entry.Text)));
			}
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetArrayField(TEXT("lines"), Rows);
			Done(FScenarioActionResult::Ok(Json));
		}));

	// prepare_result_application
	Register(Make(TEXT("prepare_result_application"), EScenarioActionKind::Read,
		TEXT("Describes how a finished job will land: purpose import, place, material, environment, media_plate or mesh_edit, with targets. Changes nothing. Returns application_id and the review text."),
		Object({
			{ TEXT("job_id"), TEXT("string") },
			{ TEXT("purpose"), TEXT("string"), FString(), FString(), { TEXT("import"), TEXT("place"), TEXT("material"), TEXT("environment"), TEXT("media_plate"), TEXT("mesh_edit"), TEXT("sequencer_audio") } },
			{ TEXT("actor_ids"), TEXT("array"), TEXT("Targets (material, media_plate, mesh_edit, at_pivot); default the selection.") },
			{ TEXT("placement"), TEXT("string"), TEXT("For place."), FString(), { TEXT("auto"), TEXT("in_front_of_camera"), TEXT("at_pivot"), TEXT("at_location"), TEXT("on_selection") } },
			{ TEXT("location"), TEXT("array"), TEXT("[x, y, z] in cm for at_location."), TEXT("number") },
			{ TEXT("slot"), TEXT("integer"), TEXT("Material slot, default 0.") },
			{ TEXT("files"), TEXT("array"), TEXT("File indices to import (job_status); default all."), TEXT("integer") },
		}, { TEXT("job_id") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, false);
			if (!S)
			{
				Done(Fail);
				return;
			}
			FScenarioSession& Session = S->Session();
			const FString Id = Str(Args, TEXT("job_id"));
			const FScenarioJobRow* Row = Session.Ledger().Find(Id);
			Row = Row ? Row : Session.Ledger().FindByJob(Id);
			if (!Row)
			{
				Done(FScenarioActionResult::Fail(TEXT("NotFound"), TEXT("No such job in this project.")));
				return;
			}
			if (Row->Kind == TEXT("prompt_spark"))
			{
				Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("Prompt Spark jobs return text: read_prompt_result.")));
				return;
			}
			if (!IsDelivered(*Row))
			{
				Done(FScenarioActionResult::Fail(TEXT("NotReady"), Row->State == EScenarioJobState::Success ? TEXT("Files are still downloading: wait_for_job.") : FString::Printf(TEXT("The job is %s: only finished jobs can be applied."), LexToString(Row->State))));
				return;
			}
			const FString Purpose = Str(Args, TEXT("purpose"), TEXT("place"));
			if (Purpose == TEXT("sequencer_audio"))
			{
				Done(FScenarioActionResult::Fail(TEXT("Unsupported"), TEXT("Sequencer audio sections are not in v0.1: use place (ambient sound) or import.")));
				return;
			}
			FApplication App;
			App.Id = NewId(TEXT("app"));
			App.LocalId = Row->LocalId;
			App.Purpose = Purpose;
			App.CreatedAt = ScenarioNow();
			const TArray<AActor*> Targets = ScenarioPlacement::ResolveTargets(Strings(Args, TEXT("actor_ids")));
			for (const AActor* Actor : Targets)
			{
				App.Options.TargetActors.Add(Actor->GetName());
			}
			App.Options.MaterialSlot = Int(Args, TEXT("slot"), 0, 0, 64);
			for (const FString& Index : Strings(Args, TEXT("files")))
			{
				App.Options.Files.Add(FCString::Atoi(*Index));
			}
			const TArray<FString> Location = Strings(Args, TEXT("location"));
			if (Location.Num() == 3)
			{
				App.Options.Location = FVector(FCString::Atod(*Location[0]), FCString::Atod(*Location[1]), FCString::Atod(*Location[2]));
			}
			const bool bHasTargets = App.Options.TargetActors.Num() > 0;
			if (Purpose == TEXT("import"))
			{
				App.Options.Placement = EScenarioPlacement::None;
			}
			else if (Purpose == TEXT("material") || Purpose == TEXT("media_plate"))
			{
				if (Purpose == TEXT("material") && !bHasTargets)
				{
					Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("material needs actor_ids or a selection.")));
					return;
				}
				App.Options.Placement = bHasTargets ? EScenarioPlacement::OnSelection : EScenarioPlacement::Auto;
			}
			else if (Purpose == TEXT("mesh_edit"))
			{
				if (!bHasTargets)
				{
					Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("mesh_edit needs the original actor in actor_ids (or selected).")));
					return;
				}
				App.Options.Placement = EScenarioPlacement::AtPivot;
			}
			else if (Purpose == TEXT("environment"))
			{
				App.Options.Placement = EScenarioPlacement::Auto;
			}
			else
			{
				App.Options.Placement = PlacementFromText(Str(Args, TEXT("placement"), TEXT("auto")), false);
				if (App.Options.Placement == EScenarioPlacement::AtLocation && Location.Num() != 3)
				{
					Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("at_location needs location [x, y, z].")));
					return;
				}
			}
			const int32 FileCount = App.Options.Files.Num() > 0 ? App.Options.Files.Num() : Row->Files.Num();
			const FString What = Row->bImported
				? FString::Printf(TEXT("Uses the %d asset(s) already imported from job %s."), FileCount, *Row->LocalId)
				: FString::Printf(TEXT("Imports %d file(s) of job %s (%s) into %s."), FileCount, *Row->LocalId, *(Row->ModelName.IsEmpty() ? Row->ModelId : Row->ModelName), *S->DestinationFor(*Row));
			App.Review = What + TEXT(" ") + DescribePlacement(App);
			if (App.Options.Placement != EScenarioPlacement::None)
			{
				const FString OpenWorld = ScenarioOpenWorld::PlacementNote(ScenarioOpenWorld::Read(ScenarioPlacement::EditorWorld(), false));
				App.Review += OpenWorld.IsEmpty() ? FString() : TEXT(" ") + OpenWorld;
			}
			Applications().Add(App.Id, App);

			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("application_id"), App.Id);
			Json->SetStringField(TEXT("review"), App.Review);
			Json->SetStringField(TEXT("next"), TEXT("apply_result_application with this application_id."));
			Done(FScenarioActionResult::Ok(Json));
		}));

	// apply_result_application
	{
		FScenarioAction Action = Make(TEXT("apply_result_application"), EScenarioActionKind::Change,
			TEXT("Applies a prepared application once, in one undo step, and returns exactly what changed and the import checks. When agent changes are off, the person gets a proposal and the result is pending_approval."),
			Object({ { TEXT("application_id"), TEXT("string") } }, { TEXT("application_id") }),
			[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller& Caller, FScenarioActionDone Done)
			{
				const FString Id = Str(Args, TEXT("application_id"));
				const FApplication* App = Applications().Find(Id);
				if (!App || ScenarioNow() - App->CreatedAt > 600.0)
				{
					Done(FScenarioActionResult::Fail(TEXT("NotFound"), TEXT("Unknown or expired application_id (10 minutes). Prepare it again.")));
					return;
				}
				if (Caller.bCanChangeProject)
				{
					RunApplication(Id, Done);
					return;
				}
				// The person decides; the agent reads the outcome with job_status.
				const FText Title = FText::Format(LOCTEXT("Proposal", "{0} proposes a change"), FText::FromString(Caller.Client.IsEmpty() ? TEXT("An agent") : Caller.Client));
				AskPerson(Title, FText::FromString(App->Review), 600.0, [Id](bool bApproved)
				{
					if (bApproved)
					{
						RunApplication(Id, [](const FScenarioActionResult& Result)
						{
							if (!Result.bOk)
							{
								FScenarioEditorModule::Toast(LOCTEXT("ProposalFailed", "The proposal could not be applied"), FText::FromString(Result.Error), false);
							}
						});
					}
				});
				TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetStringField(TEXT("state"), TEXT("pending_approval"));
				Json->SetStringField(TEXT("review"), App->Review);
				Json->SetStringField(TEXT("next"), TEXT("The person sees Approve and Decline in the editor. job_status shows imported once applied."));
				Done(FScenarioActionResult::Ok(Json));
			});
		Action.bProposeWhenDenied = true;
		Register(MoveTemp(Action));
	}

	// select_objects
	Register(Make(TEXT("select_objects"), EScenarioActionKind::Editor,
		TEXT("Selects actors by id or label (replaces the selection unless add is true)."),
		Object({ { TEXT("actor_ids"), TEXT("array") }, { TEXT("add"), TEXT("boolean") } }, { TEXT("actor_ids") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			const TArray<FString> Ids = Strings(Args, TEXT("actor_ids"));
			TArray<AActor*> Actors = Ids.Num() > 0 ? ScenarioPlacement::ResolveTargets(Ids) : TArray<AActor*>();
			if (Actors.Num() == 0)
			{
				Done(FScenarioActionResult::Fail(TEXT("NotFound"), TEXT("No actor matches those ids or labels.")));
				return;
			}
			if (Bool(Args, TEXT("add")))
			{
				for (AActor* Existing : ScenarioPlacement::ResolveTargets({}))
				{
					Actors.AddUnique(Existing);
				}
			}
			ScenarioPlacement::Select(Actors);
			TArray<FString> Names;
			for (const AActor* Actor : Actors)
			{
				Names.Add(ScenarioPlacement::Describe(Actor));
			}
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("selected"), ScenarioJson::StringArrayValue(Names));
			Done(FScenarioActionResult::Ok(Json));
		}));

	// save_changes
	Register(Make(TEXT("save_changes"), EScenarioActionKind::Change,
		TEXT("Saves the packages the plugin created or changed this session (and the level, with World Partition actor files, when include_levels is true). Returns each file written."),
		Object({ { TEXT("include_levels"), TEXT("boolean") } }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FScenarioActionResult Fail;
			UScenarioSubsystem* S = Subsystem(Fail, false);
			if (!S)
			{
				Done(Fail);
				return;
			}
			const FScenarioChangeReport Report = S->SaveChanges(Bool(Args, TEXT("include_levels")));
			if (!Report.Error.IsEmpty())
			{
				Done(FScenarioActionResult::Fail(TEXT("Save"), Report.Error));
				return;
			}
			if (Report.SavedFiles.Num() == 0)
			{
				Done(FScenarioActionResult::Fail(TEXT("NoChange"), TEXT("Nothing to save: no unsaved package the plugin created or changed.")));
				return;
			}
			Done(FScenarioActionResult::Ok(Report.ToJson()));
		}));

	// remove_scenario_items
	{
		FScenarioAction Action = Make(TEXT("remove_scenario_items"), EScenarioActionKind::Change,
			TEXT("Removes actors the plugin placed (one undo step), and a job's imported assets when nothing references them. Returns what was removed."),
			Object({ { TEXT("actor_ids"), TEXT("array") }, { TEXT("job_id"), TEXT("string"), TEXT("Removes this job's imported assets.") } }),
			[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
			{
				FScenarioActionResult Fail;
				UScenarioSubsystem* S = Subsystem(Fail, false);
				if (!S)
				{
					Done(Fail);
					return;
				}
				FScenarioChangeReport Report;
				Report.bOk = true;
				const TArray<FString> Ids = Strings(Args, TEXT("actor_ids"));
				if (Ids.Num() > 0)
				{
					TArray<AActor*> Removable;
					for (AActor* Actor : ScenarioPlacement::ResolveTargets(Ids))
					{
						if (FromScenario(Actor))
						{
							Removable.Add(Actor);
						}
						else
						{
							Report.Warnings.Add(FString::Printf(TEXT("Kept %s: it was not placed by Scenario."), *ScenarioPlacement::Describe(Actor)));
						}
					}
					if (Removable.Num() > 0)
					{
						const FScopedTransaction Transaction(LOCTEXT("Remove", "Scenario: Remove placed actors"));
						for (AActor* Actor : Removable)
						{
							const FString Name = ScenarioPlacement::Describe(Actor);
							UWorld* World = Actor->GetWorld();
							Actor->Modify();
							if (World && World->EditorDestroyActor(Actor, true))
							{
								Report.ModifiedActors.Add(FString::Printf(TEXT("removed %s"), *Name));
							}
						}
					}
				}
				const FString JobId = Str(Args, TEXT("job_id"));
				if (!JobId.IsEmpty())
				{
					Report.Append(S->RemoveImported(JobId));
				}
				if (!Report.ChangedAnything())
				{
					Done(FScenarioActionResult::Fail(TEXT("NoChange"), Report.Warnings.Num() > 0 ? FString::Join(Report.Warnings, TEXT(" ")) : TEXT("Nothing was removed.")));
					return;
				}
				Done(FScenarioActionResult::Ok(Report.ToJson()));
			});
		Action.bDestructive = true;
		Register(MoveTemp(Action));
	}

	// execute_python
	{
		FScenarioAction Action = Make(TEXT("execute_python"), EScenarioActionKind::Python,
			TEXT("Runs Unreal Python in the editor (game thread) inside one undo step. Off unless the person allows it. Output capped at 20,000 characters."),
			Object({ { TEXT("code"), TEXT("string") } }, { TEXT("code") }),
			[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
			{
				IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
				if (!Python || !Python->IsPythonAvailable())
				{
					Done(FScenarioActionResult::Fail(TEXT("Unavailable"), TEXT("The Python Editor Script Plugin is off in this project.")));
					return;
				}
				const FString Code = Str(Args, TEXT("code"));
				if (Code.IsEmpty() || Code.Len() > 100000)
				{
					Done(FScenarioActionResult::Fail(TEXT("Invalid"), TEXT("code is required (100,000 characters at most).")));
					return;
				}
				FPythonCommandEx Command;
				Command.Command = Code;
				Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
				Command.FileExecutionScope = EPythonFileExecutionScope::Private;
				bool bOk = false;
				{
					const FScopedTransaction Transaction(LOCTEXT("Python", "Scenario: Agent Python"));
					bOk = Python->ExecPythonCommandEx(Command);
				}
				FString Output;
				for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
				{
					Output += Entry.Output + TEXT("\n");
				}
				if (Output.Len() > 20000)
				{
					Output = Output.Left(20000) + TEXT("\n[output cut at 20,000 characters]");
				}
				TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetBoolField(TEXT("ok"), bOk);
				Json->SetStringField(TEXT("output"), Output);
				if (!Command.CommandResult.IsEmpty())
				{
					Json->SetStringField(TEXT("result"), Command.CommandResult.Left(4000));
				}
				if (!bOk)
				{
					FScenarioActionResult Failed = FScenarioActionResult::Fail(TEXT("Python"), Output.Right(2000));
					Failed.Data = Json;
					Done(Failed);
					return;
				}
				Done(FScenarioActionResult::Ok(Json));
			});
		Action.bDestructive = true;
		Register(MoveTemp(Action));
	}

	// unreal_api_help
	Register(Make(TEXT("unreal_api_help"), EScenarioActionKind::Read,
		TEXT("Reflection help before writing Python: a class (functions and properties) or one member (\"StaticMeshActor\", \"EditorActorSubsystem.SpawnActorFromClass\")."),
		Object({ { TEXT("name"), TEXT("string") } }, { TEXT("name") }),
		[](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			FString ClassName = Str(Args, TEXT("name"));
			FString Member;
			ClassName.Split(TEXT("."), &ClassName, &Member);
			if (ClassName.IsEmpty())
			{
				ClassName = Str(Args, TEXT("name"));
			}
			UStruct* Type = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
			if (!Type && ClassName.Len() > 1 && (ClassName[0] == TEXT('U') || ClassName[0] == TEXT('A')))
			{
				Type = FindFirstObject<UClass>(*ClassName.RightChop(1), EFindFirstObjectOptions::NativeFirst);
			}
			if (!Type)
			{
				Type = FindFirstObject<UScriptStruct>(*ClassName.RightChop(ClassName.StartsWith(TEXT("F")) ? 1 : 0), EFindFirstObjectOptions::NativeFirst);
			}
			if (!Type)
			{
				Done(FScenarioActionResult::Fail(TEXT("NotFound"), FString::Printf(TEXT("No class or struct named %s."), *ClassName)));
				return;
			}
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("type"), Type->GetName());
			Json->SetStringField(TEXT("python"), FString::Printf(TEXT("unreal.%s"), *Type->GetName()));
			if (const UStruct* Super = Type->GetSuperStruct())
			{
				Json->SetStringField(TEXT("parent"), Super->GetName());
			}
			auto FunctionJson = [](const UFunction* Function)
			{
				TArray<FString> Params;
				FString Return;
				for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
				{
					if (It->HasAnyPropertyFlags(CPF_ReturnParm))
					{
						Return = PropertyType(*It);
					}
					else
					{
						Params.Add(FString::Printf(TEXT("%s %s"), *PropertyType(*It), *PythonName(It->GetName())));
					}
				}
				return FString::Printf(TEXT("%s(%s)%s"), *PythonName(Function->GetName()), *FString::Join(Params, TEXT(", ")), Return.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" -> %s"), *Return));
			};
			if (!Member.IsEmpty())
			{
				if (const UFunction* Function = Cast<UClass>(Type) ? Cast<UClass>(Type)->FindFunctionByName(FName(*Member)) : nullptr)
				{
					Json->SetStringField(TEXT("function"), FunctionJson(Function));
					Json->SetStringField(TEXT("about"), Function->GetMetaData(TEXT("ToolTip")).Left(800));
				}
				else if (const FProperty* Property = Type->FindPropertyByName(FName(*Member)))
				{
					Json->SetStringField(TEXT("property"), FString::Printf(TEXT("%s %s"), *PropertyType(Property), *PythonName(Property->GetName())));
					Json->SetStringField(TEXT("about"), Property->GetMetaData(TEXT("ToolTip")).Left(800));
				}
				else
				{
					Done(FScenarioActionResult::Fail(TEXT("NotFound"), FString::Printf(TEXT("%s has no member %s."), *Type->GetName(), *Member)));
					return;
				}
				Json->SetStringField(TEXT("note"), TEXT("Python names are snake_case forms of these; check with dir() when unsure."));
				Done(FScenarioActionResult::Ok(Json));
				return;
			}
			TArray<FString> Functions;
			if (const UClass* Class = Cast<UClass>(Type))
			{
				for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It && Functions.Num() < 60; ++It)
				{
					if (It->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
					{
						Functions.Add(FunctionJson(*It));
					}
				}
			}
			TArray<FString> Properties;
			for (TFieldIterator<FProperty> It(Type, EFieldIteratorFlags::ExcludeSuper); It && Properties.Num() < 60; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
				{
					Properties.Add(FString::Printf(TEXT("%s %s"), *PropertyType(*It), *PythonName(It->GetName())));
				}
			}
			Json->SetField(TEXT("functions"), ScenarioJson::StringArrayValue(Functions));
			Json->SetField(TEXT("properties"), ScenarioJson::StringArrayValue(Properties));
			Json->SetStringField(TEXT("note"), TEXT("Own members only; ask for the parent for inherited ones. Python names are approximate snake_case."));
			Done(FScenarioActionResult::Ok(Json));
		}));
}

#undef LOCTEXT_NAMESPACE
