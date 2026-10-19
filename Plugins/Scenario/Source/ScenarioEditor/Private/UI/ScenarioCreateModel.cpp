// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioCreateModel.h"

#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "IContentBrowserSingleton.h"
#include "LevelEditorViewport.h"
#include "Misc/MessageDialog.h"
#include "ScenarioCatalog.h"
#include "ScenarioCU.h"
#include "ScenarioFiles.h"
#include "ScenarioJson.h"
#include "ScenarioSettings.h"
#include "ScenarioStyle.h"
#include "ScenarioSubsystem.h"
#include "ScenarioWidgets.h"
#include "SScenarioJobCard.h"

#define LOCTEXT_NAMESPACE "ScenarioCreate"

namespace
{
	TSharedPtr<FScenarioCreateModel> Instance;

	/** Seconds between the last edit and the automatic price check. */
	constexpr float PriceDelay = 0.6f;

	FScenarioSession* SessionOrNull()
	{
		UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
		return Subsystem ? &Subsystem->Session() : nullptr;
	}

	double SessionNow(const FScenarioSession& Session)
	{
		return Session.Clock ? Session.Clock() : ScenarioNow();
	}

	/** True when a carried-over value still fits the new model's field. */
	bool Accepts(const FScenarioField& Field, const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Field.IsFile())
		{
			return false;
		}
		TArray<FScenarioField> One = { Field };
		const TSharedRef<FJsonObject> Probe = MakeShared<FJsonObject>();
		Probe->SetField(Field.Name, Value);
		return ScenarioSchema::Validate(One, Probe).Num() == 0;
	}

	FString ModalityForLane(const FString& Lane)
	{
		if (Lane == TEXT("3d") || Lane == TEXT("video") || Lane == TEXT("audio"))
		{
			return Lane;
		}
		return TEXT("image");
	}

	FString LaneForModality(const FString& Modality)
	{
		if (Modality == TEXT("3d") || Modality == TEXT("video") || Modality == TEXT("audio"))
		{
			return Modality;
		}
		return TEXT("image");
	}

	/**
	 * A lane always opens on a model: the last one used if the project still
	 * allows it, else the lane's default, else its first allowed pick.
	 */
	FString ResolveModel(const FScenarioLane& Lane, const FString& Mode, const FString& Saved)
	{
		const UScenarioProjectSettings& Project = UScenarioProjectSettings::Get();
		auto Allowed = [&Project](const FString& Id) { return !Id.IsEmpty() && Project.IsModelAllowed(Id); };
		const bool bSavedFits = Lane.ModelsFor(Mode).ContainsByPredicate([&Saved](const FScenarioLaneModel& Model) { return Model.Id == Saved; })
			|| !Lane.Models.ContainsByPredicate([&Saved](const FScenarioLaneModel& Model) { return Model.Id == Saved; });
		if (Allowed(Saved) && bSavedFits)
		{
			return Saved;
		}
		const FString Default = FScenarioLanes::Get().DefaultModel(Lane.Id, Mode);
		if (Allowed(Default))
		{
			return Default;
		}
		for (const FScenarioLaneModel& Model : Lane.ModelsFor(Mode))
		{
			if (Allowed(Model.Id))
			{
				return Model.Id;
			}
		}
		for (const FScenarioLaneModel& Model : Lane.Models)
		{
			if (Allowed(Model.Id))
			{
				return Model.Id;
			}
		}
		return FString();
	}

	/** The mode a lane model belongs to, or the current one. */
	FString ModeOf(const FScenarioLane& Lane, const FString& ModelId, const FString& Current)
	{
		for (const FScenarioLaneModel& Model : Lane.Models)
		{
			if (Model.Id == ModelId && !Model.Mode.IsEmpty())
			{
				return Model.Mode;
			}
		}
		return Current;
	}
}

TSharedRef<FScenarioCreateModel> FScenarioCreateModel::Get()
{
	if (!Instance.IsValid())
	{
		Instance = MakeShared<FScenarioCreateModel>();
	}
	return Instance.ToSharedRef();
}

void FScenarioCreateModel::Subscribe()
{
	if (FScenarioSession* Session = SessionOrNull())
	{
		SessionHandle = Session->OnChanged.AddSP(this, &FScenarioCreateModel::OnSessionChanged);
		CatalogHandle = Session->Catalog().OnChanged.AddLambda([WeakThis = TWeakPtr<FScenarioCreateModel>(AsShared())]()
		{
			if (const TSharedPtr<FScenarioCreateModel> This = WeakThis.Pin())
			{
				This->OnChanged.Broadcast();
			}
		});
	}
}

void FScenarioCreateModel::Initialize()
{
	Subscribe();
	const FString Saved = UScenarioEditorSettings::Get().LastModelFor(LaneId);
	const FScenarioLane* Lane = GetLane();
	Mode = Lane && Lane->Modes.Num() > 0 ? Lane->Modes[0] : FString();
	if (Lane)
	{
		Mode = ModeOf(*Lane, Saved, Mode);
		ModelId = ResolveModel(*Lane, Mode, Saved);
		Mode = ModeOf(*Lane, ModelId, Mode);
	}
	LoadSchema();
}

void FScenarioCreateModel::Shutdown()
{
	FTSTicker::GetCoreTicker().RemoveTicker(DebounceHandle);
	DebounceHandle.Reset();
	if (FScenarioSession* Session = SessionOrNull())
	{
		Session->OnChanged.Remove(SessionHandle);
		Session->Catalog().OnChanged.Remove(CatalogHandle);
	}
	OnChanged.Clear();
	if (Instance.Get() == this)
	{
		Instance.Reset();
	}
}

// Lane, mode and model

const FScenarioLane* FScenarioCreateModel::GetLane() const
{
	return FScenarioLanes::Get().Find(LaneId);
}

void FScenarioCreateModel::SetLane(const FString& InLane)
{
	if ((InLane == LaneId && !bWorkflow && !ModelId.IsEmpty()) || !FScenarioLanes::Get().Find(InLane))
	{
		return;
	}
	bWorkflow = false;
	WorkflowDescription.Reset();
	LaneId = InLane;
	const FScenarioLane* Lane = GetLane();
	const FString Saved = UScenarioEditorSettings::Get().LastModelFor(LaneId);
	Mode = ModeOf(*Lane, Saved, Lane->Modes.Num() > 0 ? Lane->Modes[0] : FString());
	ModelId = ResolveModel(*Lane, Mode, Saved);
	Mode = ModeOf(*Lane, ModelId, Mode);
	ModelNameOverride.Reset();
	LoadSchema();
}

void FScenarioCreateModel::SetMode(const FString& InMode)
{
	if (InMode == Mode)
	{
		return;
	}
	Mode = InMode;
	const FScenarioLane* Lane = GetLane();
	const TArray<FScenarioLaneModel> Fits = Lane ? Lane->ModelsFor(Mode) : TArray<FScenarioLaneModel>();
	if (Fits.ContainsByPredicate([this](const FScenarioLaneModel& Model) { return Model.Id == ModelId; }))
	{
		Changed(true);
		return;
	}
	ModelId = Lane ? ResolveModel(*Lane, Mode, FString()) : FString();
	ModelNameOverride.Reset();
	LoadSchema();
}

FString FScenarioCreateModel::GetModelName() const
{
	if (!ModelNameOverride.IsEmpty())
	{
		return ModelNameOverride;
	}
	if (bWorkflow)
	{
		return ModelId.IsEmpty() ? FString() : FString(TEXT("Workflow"));
	}
	if (const FScenarioLane* Lane = FScenarioLanes::Get().LaneForModel(ModelId))
	{
		for (const FScenarioLaneModel& Model : Lane->Models)
		{
			if (Model.Id == ModelId && !Model.Name.IsEmpty())
			{
				return Model.Name;
			}
		}
	}
	if (FScenarioSession* Session = SessionOrNull())
	{
		if (const FScenarioCatalogModel* Model = Session->Catalog().Find(ModelId))
		{
			return Model->Summary.Name;
		}
	}
	return ModelId;
}

FString FScenarioCreateModel::GetModelNote() const
{
	if (bWorkflow)
	{
		return WorkflowDescription;
	}
	if (const FScenarioLane* Lane = FScenarioLanes::Get().LaneForModel(ModelId))
	{
		for (const FScenarioLaneModel& Model : Lane->Models)
		{
			if (Model.Id == ModelId && !Model.Note.IsEmpty())
			{
				return Model.Note;
			}
		}
	}
	if (FScenarioSession* Session = SessionOrNull())
	{
		if (const FScenarioCatalogModel* Model = Session->Catalog().Find(ModelId))
		{
			return Model->Summary.ShortDescription;
		}
	}
	return FString();
}

FString FScenarioCreateModel::GetModelBadge() const
{
	if (FScenarioSession* Session = SessionOrNull())
	{
		if (const FScenarioCatalogModel* Model = Session->Catalog().Find(ModelId))
		{
			return Model->Badge();
		}
	}
	return GetModelName().Left(2).ToUpper();
}

void FScenarioCreateModel::SetModel(const FString& InModelId, const FString& InName)
{
	if (InModelId.IsEmpty())
	{
		return;
	}
	ModelNameOverride = InName;
	bWorkflow = false;
	WorkflowDescription.Reset();
	const FScenarioLane* Current = GetLane();
	const bool bInLane = Current && Current->Models.ContainsByPredicate([&InModelId](const FScenarioLaneModel& Model) { return Model.Id == InModelId; });
	if (!bInLane)
	{
		if (const FScenarioLane* Owner = FScenarioLanes::Get().LaneForModel(InModelId))
		{
			LaneId = Owner->Id;
		}
		else if (FScenarioSession* Session = SessionOrNull())
		{
			// A catalog model outside the lanes: keep the lane when the modality fits, else switch.
			if (const FScenarioCatalogModel* Model = Session->Catalog().Find(InModelId))
			{
				if (ModalityForLane(LaneId) != Model->Modality)
				{
					LaneId = LaneForModality(Model->Modality);
				}
			}
		}
	}
	if (const FScenarioLane* Lane = GetLane())
	{
		Mode = Lane->Modes.Num() > 0 && !Lane->Modes.Contains(Mode) ? Lane->Modes[0] : Mode;
		for (const FScenarioLaneModel& Model : Lane->Models)
		{
			if (Model.Id == InModelId && !Model.Mode.IsEmpty())
			{
				Mode = Model.Mode;
				break;
			}
		}
	}
	ModelId = InModelId;
	UScenarioEditorSettings::GetMutable().SetLastModel(LaneId, ModelId);
	LoadSchema();
}

void FScenarioCreateModel::SetWorkflow(const FString& WorkflowId, const FString& Name, const FString& Description)
{
	bWorkflow = true;
	ModelId = WorkflowId;
	ModelNameOverride = Name;
	WorkflowDescription = Description;
	if (!WorkflowId.IsEmpty())
	{
		UScenarioEditorSettings::GetMutable().SetLastModel(TEXT("workflow"), WorkflowId);
	}
	LoadWorkflows();
	LoadSchema();
}

void FScenarioCreateModel::EnterWorkflowLane()
{
	if (bWorkflow)
	{
		return;
	}
	const FString Saved = UScenarioEditorSettings::Get().LastModelFor(TEXT("workflow"));
	const FScenarioWorkflowItem* Known = Workflows.FindByPredicate([&Saved](const FScenarioWorkflowItem& Item) { return Item.Id == Saved; });
	SetWorkflow(Saved, Known ? Known->Name : FString(), Known ? Known->Description : FString());
}

void FScenarioCreateModel::LoadWorkflows(bool bForce)
{
	FScenarioSession* Session = SessionOrNull();
	if (!Session || !Session->IsReady())
	{
		return;
	}
	const FString Key = Session->GetWorkspace().ProjectId;
	if (bLoadingWorkflows || (!bForce && WorkflowsFor == Key))
	{
		return;
	}
	WorkflowsFor = Key;
	bLoadingWorkflows = true;
	WorkflowsError.Reset();
	OnChanged.Broadcast();

	auto Parse = [](const FScenarioReply& Reply, bool bTeam, TArray<FScenarioWorkflowItem>& Out)
	{
		for (const TSharedPtr<FJsonValue>& Value : ScenarioJson::GetArray(Reply.Value, TEXT("workflows")))
		{
			const TSharedPtr<FJsonObject> Row = Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			const FString Id = ScenarioJson::GetString(Row, TEXT("id"));
			if (Id.IsEmpty() || Out.ContainsByPredicate([&Id](const FScenarioWorkflowItem& Item) { return Item.Id == Id; }))
			{
				continue;
			}
			FScenarioWorkflowItem Item;
			Item.Id = Id;
			Item.Name = ScenarioJson::GetString(Row, TEXT("name"), Id);
			Item.Description = ScenarioJson::GetString(Row, TEXT("description"), ScenarioJson::GetString(Row, TEXT("shortDescription")));
			Item.bTeam = bTeam;
			Out.Add(Item);
		}
	};
	TWeakPtr<FScenarioCreateModel> WeakThis = AsShared();
	Session->Api().ListWorkflows(TEXT("private"), TEXT("ready"), 50, FString(), {}, [WeakThis, Parse](const FScenarioReply& Team)
	{
		const TSharedPtr<FScenarioCreateModel> This = WeakThis.Pin();
		FScenarioSession* S = SessionOrNull();
		if (!This.IsValid() || !S)
		{
			return;
		}
		TArray<FScenarioWorkflowItem> Found;
		Parse(Team, true, Found);
		const FString TeamError = Team.bOk ? FString() : Team.Error.Message;
		S->Api().ListWorkflows(TEXT("public"), TEXT("ready"), 30, FString(), { TEXT("featured") }, [WeakThis, Parse, TeamError, Found](const FScenarioReply& Featured) mutable
		{
			const TSharedPtr<FScenarioCreateModel> Later = WeakThis.Pin();
			if (!Later.IsValid())
			{
				return;
			}
			Parse(Featured, false, Found);
			Later->Workflows = MoveTemp(Found);
			Later->bLoadingWorkflows = false;
			Later->WorkflowsError = !TeamError.IsEmpty() ? TeamError : (Featured.bOk ? FString() : Featured.Error.Message);
			// The lane opened on a saved workflow before its name was known.
			if (Later->bWorkflow && Later->ModelNameOverride.IsEmpty())
			{
				const FString Id = Later->ModelId;
				if (const FScenarioWorkflowItem* Known = Later->Workflows.FindByPredicate([&Id](const FScenarioWorkflowItem& Item) { return Item.Id == Id; }))
				{
					Later->ModelNameOverride = Known->Name;
					Later->WorkflowDescription = Known->Description;
				}
			}
			Later->OnChanged.Broadcast();
		});
	});
}

TArray<FScenarioLaneModel> FScenarioCreateModel::QuickPicks() const
{
	if (bWorkflow)
	{
		return {};
	}
	const FScenarioLane* Lane = GetLane();
	TArray<FScenarioLaneModel> Picks = Lane ? Lane->ModelsFor(Mode) : TArray<FScenarioLaneModel>();
	const UScenarioProjectSettings& Project = UScenarioProjectSettings::Get();
	Picks.RemoveAll([&Project](const FScenarioLaneModel& Model) { return !Project.IsModelAllowed(Model.Id); });
	return Picks;
}

FString FScenarioCreateModel::PickerModality() const
{
	// The picker has Material and Sky tabs of their own.
	if (LaneId == TEXT("material"))
	{
		return TEXT("material");
	}
	if (LaneId == TEXT("environment"))
	{
		return TEXT("sky");
	}
	return ModalityForLane(LaneId);
}

// Form

void FScenarioCreateModel::LoadSchema()
{
	const int32 Generation = ++SchemaGeneration;
	SchemaError.Reset();
	LastError.Reset();
	if (ModelId.IsEmpty())
	{
		Fields.Reset();
		bLoadingSchema = false;
		Changed(true);
		return;
	}
	if (!bWorkflow && !UScenarioProjectSettings::Get().IsModelAllowed(ModelId))
	{
		Fields.Reset();
		bLoadingSchema = false;
		SchemaError = TEXT("This project's Scenario settings do not allow this model. Pick another one.");
		Changed(true);
		return;
	}
	UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
	if (!Subsystem || !Subsystem->Session().IsSignedIn())
	{
		// Loaded again when the session connects.
		bLoadingSchema = false;
		Changed(false);
		return;
	}
	bLoadingSchema = true;
	Changed(false);
	TWeakPtr<FScenarioCreateModel> WeakThis = AsShared();
	auto Loaded = [WeakThis, Generation](const TArray<FScenarioField>& NewFields, const FScenarioError& Error)
	{
		const TSharedPtr<FScenarioCreateModel> This = WeakThis.Pin();
		if (!This.IsValid() || Generation != This->SchemaGeneration)
		{
			return;
		}
		This->bLoadingSchema = false;
		if (Error.Kind != EScenarioErrorKind::None)
		{
			This->Fields.Reset();
			This->SchemaError = Error.Kind == EScenarioErrorKind::NotFound
				? (This->bWorkflow ? TEXT("Scenario has no workflow with this id any more.") : TEXT("Scenario has no model with this id any more. Pick another one."))
				: Error.Message;
			This->Changed(true);
			return;
		}
		This->SchemaLoaded(NewFields);
	};
	if (bWorkflow)
	{
		Subsystem->GetWorkflowFields(ModelId, [Loaded](const TArray<FScenarioField>& NewFields, const TSharedPtr<FJsonObject>&, const FScenarioError& Error)
		{
			Loaded(NewFields, Error);
		});
	}
	else
	{
		Subsystem->GetModelFields(ModelId, Loaded);
	}
}

void FScenarioCreateModel::SchemaLoaded(const TArray<FScenarioField>& NewFields)
{
	const TArray<FScenarioField> OldFields = Fields;
	const TSharedRef<FJsonObject> OldValues = Values;
	const FString OldPrompt = GetPrompt();

	Fields = NewFields;
	if (!bWorkflow)
	{
		FScenarioLanes::Get().ApplyFieldLabels(LaneId, ModelId, Fields);
	}
	Values = ScenarioSchema::InitialValues(Fields, bWorkflow ? nullptr : FScenarioLanes::Get().Presets(ModelId));

	// Keep what the person typed or chose when the new model has the same setting.
	if (!OldPrompt.IsEmpty())
	{
		SetPrompt(OldPrompt);
	}
	const FString PromptField = ScenarioSchema::PromptName(Fields);
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ScenarioJson::Fields(OldValues))
	{
		if (Pair.Key == PromptField)
		{
			continue;
		}
		const FScenarioField* Field = ScenarioSchema::Find(Fields, Pair.Key);
		const FScenarioField* OldField = ScenarioSchema::Find(OldFields, Pair.Key);
		const bool bChangedByPerson = OldField && !ScenarioJson::Canonical(Pair.Value).Equals(ScenarioJson::Canonical(OldField->Default));
		if (Field && bChangedByPerson && Accepts(*Field, Pair.Value))
		{
			Values->SetField(Pair.Key, Pair.Value);
		}
	}

	// Move references to a file field of the same kind when the names differ between models.
	for (int32 Index = References.Num() - 1; Index >= 0; --Index)
	{
		FScenarioReference& Reference = References[Index];
		if (ScenarioSchema::Find(Fields, Reference.Field))
		{
			continue;
		}
		const FScenarioField* OldField = ScenarioSchema::Find(OldFields, Reference.Field);
		const FString Kind = OldField ? OldField->Kind : FString();
		const FScenarioField* Target = Fields.FindByPredicate([&Kind](const FScenarioField& Field) { return Field.IsFile() && (Kind.IsEmpty() || Field.Kind.IsEmpty() || Field.Kind == Kind); });
		if (Target)
		{
			Reference.Field = Target->Name;
		}
		else
		{
			References.RemoveAt(Index);
		}
	}
	// A single-file field keeps only its newest reference.
	for (const FScenarioField& Field : Fields)
	{
		const int32 Limit = Field.Type == EScenarioFieldType::File ? 1 : Field.MaxItems;
		if (Limit <= 0)
		{
			continue;
		}
		int32 Seen = 0;
		for (int32 Index = References.Num() - 1; Index >= 0; --Index)
		{
			if (References[Index].Field == Field.Name && ++Seen > Limit)
			{
				References.RemoveAt(Index);
			}
		}
	}

	AttachPendingReferences();
	if (PendingValues.IsValid())
	{
		const TSharedPtr<FJsonObject> Pending = PendingValues;
		PendingValues.Reset();
		ApplyValues(Pending);
		return;
	}
	Changed(true);
}

const FScenarioField* FScenarioCreateModel::FileFieldFor(const FString& Kind) const
{
	const FScenarioField* Loose = nullptr;
	for (const FScenarioField& Field : Fields)
	{
		if (!Field.IsFile())
		{
			continue;
		}
		if (Field.Kind == Kind)
		{
			return &Field;
		}
		if (Field.Kind.IsEmpty() && !Loose)
		{
			Loose = &Field;
		}
	}
	return Loose;
}

void FScenarioCreateModel::UseReference(const FString& AssetId, const FString& Kind, const FString& Label)
{
	PendingReferences.Add({ AssetId, FString(), Kind, Label });
	if (!bLoadingSchema && Fields.Num() > 0)
	{
		AttachPendingReferences();
		Changed(true);
	}
}

FString FScenarioCreateModel::UseFile(const FString& Path, const FString& Label)
{
	const FString Kind = ScenarioFiles::UploadKindForExtension(FPaths::GetExtension(Path));
	if (bLoadingSchema || Fields.Num() == 0)
	{
		PendingReferences.Add({ FString(), Path, Kind, Label });
		return FString();
	}
	const FScenarioField* Field = FileFieldFor(Kind);
	if (!Field)
	{
		return FString::Printf(TEXT("%s takes no %s input. Pick a model that does."), *GetModelName(), Kind.IsEmpty() ? TEXT("file") : *Kind);
	}
	return AddReferenceFile(Field->Name, Path, Label);
}

void FScenarioCreateModel::AttachPendingReferences()
{
	const TArray<FPendingReference> Items = MoveTemp(PendingReferences);
	PendingReferences.Reset();
	for (const FPendingReference& Item : Items)
	{
		const FScenarioField* Field = FileFieldFor(Item.Kind);
		if (!Field)
		{
			LastError = FString::Printf(TEXT("%s takes no %s input, so the reference was not added."), *GetModelName(), Item.Kind.IsEmpty() ? TEXT("file") : *Item.Kind);
			continue;
		}
		const FString FieldName = Field->Name;
		const FString Error = Item.AssetId.IsEmpty() ? AddReferenceFile(FieldName, Item.Path, Item.Label) : AddReferenceAsset(FieldName, Item.AssetId, Item.Label);
		if (!Error.IsEmpty())
		{
			LastError = Error;
		}
	}
}

TSharedPtr<FJsonValue> FScenarioCreateModel::GetValue(const FString& Name) const
{
	return Values->TryGetField(Name);
}

void FScenarioCreateModel::SetValue(const FString& Name, const TSharedPtr<FJsonValue>& Value)
{
	if (Value.IsValid() && !Value->IsNull())
	{
		Values->SetField(Name, Value);
	}
	else
	{
		Values->RemoveField(Name);
	}
	Changed(true);
}

FString FScenarioCreateModel::GetPrompt() const
{
	const FString Name = ScenarioSchema::PromptName(Fields);
	return Name.IsEmpty() ? FString() : ScenarioJson::GetString(Values, Name);
}

void FScenarioCreateModel::SetPrompt(const FString& Prompt)
{
	const FString Name = ScenarioSchema::PromptName(Fields);
	if (Name.IsEmpty())
	{
		return;
	}
	if (GetPrompt() == Prompt)
	{
		return;
	}
	Values->SetStringField(Name, Prompt);
	Changed(true);
}

void FScenarioCreateModel::ApplyValues(const TSharedPtr<FJsonObject>& InValues)
{
	if (!InValues.IsValid())
	{
		return;
	}
	if (bLoadingSchema)
	{
		PendingValues = InValues;
		return;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ScenarioJson::Fields(InValues))
	{
		const FScenarioField* Field = ScenarioSchema::Find(Fields, Pair.Key);
		if (!Field)
		{
			continue;
		}
		if (Field->IsFile())
		{
			// Asset ids from an earlier run ("Reuse settings"): reused, nothing uploaded.
			References.RemoveAll([Field](const FScenarioReference& Reference) { return Reference.Field == Field->Name; });
			TArray<FString> Ids;
			if (Pair.Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Item : Pair.Value->AsArray())
				{
					Ids.Add(ScenarioJson::ValueText(Item));
				}
			}
			else
			{
				Ids.Add(ScenarioJson::ValueText(Pair.Value));
			}
			for (const FString& Id : Ids)
			{
				if (!Id.IsEmpty())
				{
					FScenarioReference Reference;
					Reference.Id = NextReferenceId++;
					Reference.Field = Field->Name;
					Reference.AssetId = Id;
					Reference.Label = Id.Right(8);
					References.Add(Reference);
				}
			}
			continue;
		}
		Values->SetField(Pair.Key, Pair.Value);
	}
	Changed(true);
}

TSharedRef<FJsonObject> FScenarioCreateModel::ValuesWithReferences() const
{
	TSharedRef<FJsonObject> Out = ScenarioJson::CloneObject(Values).ToSharedRef();
	for (const FScenarioField& Field : Fields)
	{
		if (!Field.IsFile())
		{
			continue;
		}
		TArray<FString> Ids;
		for (const FScenarioReference& Reference : References)
		{
			if (Reference.Field == Field.Name && !Reference.AssetId.IsEmpty())
			{
				Ids.Add(Reference.AssetId);
			}
		}
		if (Ids.Num() == 0)
		{
			Out->RemoveField(Field.Name);
		}
		else if (Field.Type == EScenarioFieldType::File)
		{
			Out->SetStringField(Field.Name, Ids.Last());
		}
		else
		{
			Out->SetField(Field.Name, ScenarioJson::StringArrayValue(Ids));
		}
	}
	return Out;
}

TArray<FString> FScenarioCreateModel::Problems() const
{
	TArray<FString> Out;
	if (ModelId.IsEmpty())
	{
		Out.Add(TEXT("Choose a model."));
		return Out;
	}
	for (const FScenarioReference& Reference : References)
	{
		if (!Reference.Error.IsEmpty())
		{
			Out.Add(FString::Printf(TEXT("%s: %s"), *Reference.Label, *Reference.Error));
		}
	}
	Out.Append(ScenarioSchema::Validate(Fields, ValuesWithReferences()));
	return Out;
}

const FScenarioField* FScenarioCreateModel::FirstProblemField() const
{
	const TSharedRef<FJsonObject> Current = ValuesWithReferences();
	for (const FScenarioField& Field : Fields)
	{
		TArray<FScenarioField> One = { Field };
		if (ScenarioSchema::Validate(One, Current).Num() > 0)
		{
			return &Field;
		}
	}
	return nullptr;
}

bool FScenarioCreateModel::IsUnrealReadyModel() const
{
	if (bWorkflow)
	{
		return false;
	}
	const FScenarioLane* Lane = FScenarioLanes::Get().LaneForModel(ModelId);
	if (!Lane || FScenarioLanes::Get().UnrealReadyPrompt().IsEmpty())
	{
		return false;
	}
	return Lane->Models.ContainsByPredicate([this](const FScenarioLaneModel& Model) { return Model.Id == ModelId && Model.bUnrealReady; });
}

void FScenarioCreateModel::SetUnrealReady(bool bOn)
{
	if (bUnrealReady != bOn)
	{
		bUnrealReady = bOn;
		Changed(true);
	}
}

// References

TArray<FScenarioReference> FScenarioCreateModel::ReferencesFor(const FString& Field) const
{
	return References.FilterByPredicate([&Field](const FScenarioReference& Reference) { return Reference.Field == Field; });
}

FScenarioReference* FScenarioCreateModel::FindReference(int32 Id)
{
	return References.FindByPredicate([Id](const FScenarioReference& Reference) { return Reference.Id == Id; });
}

FString FScenarioCreateModel::AddReferenceFile(const FString& FieldName, const FString& Path, const FString& Label)
{
	const FScenarioField* Field = ScenarioSchema::Find(Fields, FieldName);
	if (!Field || !Field->IsFile())
	{
		return TEXT("This model takes no file here.");
	}
	if (!FPaths::FileExists(Path))
	{
		return FString::Printf(TEXT("%s does not exist."), *FPaths::GetCleanFilename(Path));
	}
	const FString Kind = ScenarioFiles::UploadKindForExtension(FPaths::GetExtension(Path));
	if (!Field->Kind.IsEmpty() && !Kind.IsEmpty() && Field->Kind != Kind)
	{
		return FString::Printf(TEXT("%s takes %s files, not %s."), *Field->Label, *Field->Kind, *FPaths::GetExtension(Path).ToLower());
	}
	if (Field->Type == EScenarioFieldType::File)
	{
		References.RemoveAll([&FieldName](const FScenarioReference& Reference) { return Reference.Field == FieldName; });
	}
	else if (Field->MaxItems > 0 && ReferencesFor(FieldName).Num() >= Field->MaxItems)
	{
		return FString::Printf(TEXT("%s takes at most %d files. Remove one first."), *Field->Label, Field->MaxItems);
	}

	UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
	if (!Subsystem)
	{
		return TEXT("Scenario is not ready yet.");
	}
	Subsystem->AllowUpload(Path);

	FScenarioReference Reference;
	Reference.Id = NextReferenceId++;
	Reference.Field = FieldName;
	Reference.LocalPath = Path;
	Reference.Label = Label.IsEmpty() ? FPaths::GetCleanFilename(Path) : Label;
	Reference.bUploading = true;
	References.Add(Reference);
	Changed(true);

	const int32 Id = Reference.Id;
	TWeakPtr<FScenarioCreateModel> WeakThis = AsShared();
	Subsystem->Session().Upload(Path,
		[WeakThis, Id](const TScenarioResult<FString>& Result)
		{
			const TSharedPtr<FScenarioCreateModel> This = WeakThis.Pin();
			FScenarioReference* Found = This.IsValid() ? This->FindReference(Id) : nullptr;
			if (!Found)
			{
				return;
			}
			Found->bUploading = false;
			Found->Progress = 1.0f;
			if (Result.bOk)
			{
				Found->AssetId = Result.Value;
				Found->Error.Reset();
			}
			else
			{
				Found->Error = Result.Error.Message;
			}
			This->Changed(true);
		},
		[WeakThis, Id](float Progress)
		{
			const TSharedPtr<FScenarioCreateModel> This = WeakThis.Pin();
			if (FScenarioReference* Found = This.IsValid() ? This->FindReference(Id) : nullptr)
			{
				Found->Progress = Progress;
				This->OnChanged.Broadcast();
			}
		});
	return FString();
}

FString FScenarioCreateModel::AddReferenceAsset(const FString& FieldName, const FString& AssetId, const FString& Label)
{
	const FScenarioField* Field = ScenarioSchema::Find(Fields, FieldName);
	if (!Field || !Field->IsFile())
	{
		return TEXT("This model takes no file here.");
	}
	if (Field->Type == EScenarioFieldType::File)
	{
		References.RemoveAll([&FieldName](const FScenarioReference& Reference) { return Reference.Field == FieldName; });
	}
	else if (Field->MaxItems > 0 && ReferencesFor(FieldName).Num() >= Field->MaxItems)
	{
		return FString::Printf(TEXT("%s takes at most %d files. Remove one first."), *Field->Label, Field->MaxItems);
	}
	FScenarioReference Reference;
	Reference.Id = NextReferenceId++;
	Reference.Field = FieldName;
	Reference.AssetId = AssetId;
	Reference.Label = Label.IsEmpty() ? AssetId.Right(8) : Label;
	References.Add(Reference);
	Changed(true);
	return FString();
}

void FScenarioCreateModel::RemoveReference(const FString& Field, int32 IndexInField)
{
	int32 Seen = 0;
	for (int32 Index = 0; Index < References.Num(); ++Index)
	{
		if (References[Index].Field == Field && Seen++ == IndexInField)
		{
			References.RemoveAt(Index);
			Changed(true);
			return;
		}
	}
}

FString FScenarioCreateModel::AddViewportCapture(const FString& Field)
{
	UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
	if (!Subsystem)
	{
		return TEXT("Scenario is not ready yet.");
	}
	// The viewport's aspect, 1536 px on the long side (enough for every reference input).
	int32 Width = 1280;
	int32 Height = 720;
	if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
	{
		const FIntPoint Size = GCurrentLevelEditingViewportClient->Viewport->GetSizeXY();
		if (Size.X > 0 && Size.Y > 0)
		{
			const float Scale = 1536.0f / FMath::Max(Size.X, Size.Y);
			Width = FMath::RoundToInt(Size.X * Scale);
			Height = FMath::RoundToInt(Size.Y * Scale);
		}
	}
	FString Error;
	const FString Path = Subsystem->CaptureViewport(Width, Height, Error);
	if (Path.IsEmpty())
	{
		return Error;
	}
	return AddReferenceFile(Field, Path, TEXT("Viewport"));
}

FString FScenarioCreateModel::AddContentSelection(const FString& Field)
{
	UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
	if (!Subsystem)
	{
		return TEXT("Scenario is not ready yet.");
	}
	TArray<FAssetData> Selected;
	FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().GetSelectedAssets(Selected);
	if (Selected.Num() == 0)
	{
		return TEXT("Select a texture, mesh or sound in the Content Browser first.");
	}
	return AddAssets(Field, Selected);
}

FString FScenarioCreateModel::AddAssets(const FString& Field, const TArray<FAssetData>& Assets)
{
	UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
	if (!Subsystem)
	{
		return TEXT("Scenario is not ready yet.");
	}
	TArray<FString> Errors;
	for (const FAssetData& Asset : Assets)
	{
		FScenarioUploadSource Source;
		FString Error;
		if (!Subsystem->ResolveUploadSource(Asset.GetAsset(), Source, Error))
		{
			Errors.Add(Error);
			continue;
		}
		const int32 Id = NextReferenceId;
		Error = Source.AssetId.IsEmpty() ? AddReferenceFile(Field, Source.Path, Source.Label) : AddReferenceAsset(Field, Source.AssetId, Source.Label);
		if (!Error.IsEmpty())
		{
			Errors.Add(Error);
		}
		else if (FScenarioReference* Added = FindReference(Id))
		{
			Added->ObjectPath = Asset.GetSoftObjectPath().ToString();
		}
	}
	return FString::Join(Errors, TEXT(" "));
}

FString FScenarioCreateModel::AddFiles(const FString& Field, const TArray<FString>& Paths)
{
	TArray<FString> Errors;
	for (const FString& Path : Paths)
	{
		const FString Error = AddReferenceFile(Field, Path, FString());
		if (!Error.IsEmpty())
		{
			Errors.Add(Error);
		}
	}
	return FString::Join(Errors, TEXT(" "));
}

FString FScenarioCreateModel::AddSelectedActors(const FString& Field)
{
	UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
	if (!Subsystem || !GEditor)
	{
		return TEXT("Scenario is not ready yet.");
	}
	TArray<AActor*> Actors;
	GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Actors);
	FString Error;
	const FString Path = Subsystem->ExportActorsToGlb(Actors, Error);
	if (Path.IsEmpty())
	{
		return Error;
	}
	return AddReferenceFile(Field, Path, Actors.Num() == 1 ? Actors[0]->GetActorLabel() : FString::Printf(TEXT("%d actors"), Actors.Num()));
}

// Destination

FString FScenarioCreateModel::DestinationText() const
{
	UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
	if (!Subsystem)
	{
		return Destination;
	}
	FScenarioJobRow Row;
	Row.Lane = LaneId;
	Row.Destination = Destination;
	return Subsystem->DestinationFor(Row);
}

FString FScenarioCreateModel::PlacementText() const
{
	if (bWorkflow)
	{
		return TEXT("Import and place: each output goes where its type belongs (mesh, texture, sound...).");
	}
	const FScenarioLane* Lane = GetLane();
	const FString Importer = Lane ? Lane->Importer : FString();
	if (Importer == TEXT("mesh"))
	{
		return TEXT("Import and place: on the ground in front of the camera.");
	}
	if (Importer == TEXT("material"))
	{
		return TEXT("Import and place: on the selected actor, or on a preview sphere.");
	}
	if (Importer == TEXT("sky"))
	{
		return TEXT("Import and place: as the level's sky.");
	}
	if (Importer == TEXT("media"))
	{
		return TEXT("Import and place: on a Media Plate in front of the camera.");
	}
	if (Importer == TEXT("sound"))
	{
		return TEXT("Import and place: an ambient sound at the camera's focus point.");
	}
	return TEXT("Import and place: on the selected actor, or on a plane in front of the camera.");
}

// Price and Generate

FScenarioRunRequest FScenarioCreateModel::BuildRequest() const
{
	FScenarioRunRequest Request;
	Request.Kind = bWorkflow ? EScenarioQuoteKind::Workflow : EScenarioQuoteKind::Model;
	Request.TargetId = ModelId;
	Request.TargetName = GetModelName();
	Request.Lane = bWorkflow ? FString() : LaneId;
	Request.Source = TEXT("person");
	Request.Client = TEXT("Unreal Editor");
	Request.Destination = Destination;

	const TSharedRef<FJsonObject> Payload = ScenarioSchema::Payload(Fields, ValuesWithReferences());
	const FString PromptField = ScenarioSchema::PromptName(Fields);
	if (bUnrealReady && IsUnrealReadyModel() && !PromptField.IsEmpty())
	{
		const FString Prompt = ScenarioJson::GetString(Payload, PromptField);
		if (!Prompt.IsEmpty())
		{
			Payload->SetStringField(PromptField, Prompt.TrimEnd() + TEXT(" ") + FScenarioLanes::Get().UnrealReadyPrompt());
		}
	}
	Request.Parameters = Payload;
	return Request;
}

FString FScenarioCreateModel::Fingerprint() const
{
	if (bFingerprintDirty)
	{
		FScenarioSession* Session = SessionOrNull();
		CachedFingerprint = Session ? Session->Fingerprint(BuildRequest()) : FString();
		bFingerprintDirty = false;
	}
	return CachedFingerprint;
}

const FScenarioQuote* FScenarioCreateModel::CurrentQuote() const
{
	FScenarioSession* Session = SessionOrNull();
	if (!Session || ModelId.IsEmpty() || bLoadingSchema)
	{
		return nullptr;
	}
	return Session->Quotes().Usable(Fingerprint(), SessionNow(*Session));
}

EScenarioGenerateState FScenarioCreateModel::GetState() const
{
	FScenarioSession* Session = SessionOrNull();
	if (!Session || !Session->IsSignedIn())
	{
		return EScenarioGenerateState::SignedOut;
	}
	if (!Session->IsReady())
	{
		return EScenarioGenerateState::NeedsWorkspace;
	}
	if (ModelId.IsEmpty())
	{
		return EScenarioGenerateState::NoModel;
	}
	if (bLoadingSchema)
	{
		return EScenarioGenerateState::LoadingModel;
	}
	if (!SchemaError.IsEmpty())
	{
		return EScenarioGenerateState::ModelError;
	}
	if (bSubmitting || ScenarioNow() < WaitingUntil)
	{
		return EScenarioGenerateState::Submitting;
	}
	if (References.ContainsByPredicate([](const FScenarioReference& Reference) { return Reference.bUploading; }))
	{
		return EScenarioGenerateState::Uploading;
	}
	if (Problems().Num() > 0)
	{
		return EScenarioGenerateState::NeedsInput;
	}
	if (Session->IsSpendingDisabled())
	{
		return EScenarioGenerateState::SpendingOff;
	}
	const FString Fp = Fingerprint();
	if (Session->Ledger().Blocks(Fp))
	{
		return EScenarioGenerateState::Blocked;
	}
	// A price shown for this exact request stays clickable after its quote expires: the click checks it again.
	if (CurrentQuote() || (!PricedFingerprint.IsEmpty() && PricedFingerprint == Fp && !bPricing))
	{
		return EScenarioGenerateState::Priced;
	}
	if (!bPricing && PriceError.Kind != EScenarioErrorKind::None)
	{
		return EScenarioGenerateState::PriceFailed;
	}
	return EScenarioGenerateState::Pricing;
}

FText FScenarioCreateModel::GetButtonText() const
{
	const EScenarioGenerateState State = GetState();
	if (State == EScenarioGenerateState::Submitting)
	{
		return LOCTEXT("Sending", "Sending...");
	}
	if (State == EScenarioGenerateState::PriceFailed)
	{
		return LOCTEXT("RetryPrice", "Retry price");
	}
	double CU = -1.0;
	bool bApprox = false;
	if (State == EScenarioGenerateState::Priced)
	{
		const FScenarioQuote* Quote = CurrentQuote();
		CU = Quote ? Quote->CU : ExpectedCU;
	}
	else if ((State == EScenarioGenerateState::Pricing || State == EScenarioGenerateState::NeedsInput || State == EScenarioGenerateState::SpendingOff) && EstimateModel == ModelId)
	{
		// The last known price of this model: exact once checked for this request, "~" until then.
		CU = EstimateCU;
		bApprox = State != EScenarioGenerateState::Pricing;
	}
	// While the last request runs or was just made, a click sends a second, paid request.
	const bool bAgain = !LastLocalId.IsEmpty() && SentLane == GetLaneKey() && (IsSentJobRunning() || GetHint().Tone == EScenarioHintTone::Success);
	const FText Verb = bAgain ? LOCTEXT("GenerateAgain", "Generate again") : LOCTEXT("GenerateVerb", "Generate");
	if (CU < 0.0)
	{
		return Verb;
	}
	return FText::Format(LOCTEXT("GeneratePrice", "{0} · {1}{2}"), Verb, FText::FromString(bApprox ? TEXT("~") : TEXT("")), FText::FromString(ScenarioCU::Format(CU)));
}

bool FScenarioCreateModel::IsPriceStale() const
{
	return GetState() == EScenarioGenerateState::Pricing;
}

FScenarioHint FScenarioCreateModel::GetHint() const
{
	FScenarioHint Hint;
	auto Set = [&Hint](EScenarioHintTone Tone, const FText& Text, EScenarioHintAction Primary = EScenarioHintAction::None, EScenarioHintAction Secondary = EScenarioHintAction::None)
	{
		Hint.Tone = Tone;
		Hint.Text = Text;
		Hint.Primary = Primary;
		Hint.Secondary = Secondary;
	};
	const EScenarioGenerateState State = GetState();
	const FText Name = FText::FromString(GetModelName());
	switch (State)
	{
	case EScenarioGenerateState::SignedOut:
		Set(EScenarioHintTone::Info, LOCTEXT("HintSignIn", "Sign in to Scenario to generate."));
		return Hint;
	case EScenarioGenerateState::NeedsWorkspace:
		Set(EScenarioHintTone::Info, LOCTEXT("HintProject", "Choose a Scenario project first."));
		return Hint;
	case EScenarioGenerateState::NoModel:
		Set(EScenarioHintTone::Info, bWorkflow ? LOCTEXT("HintNoWorkflow", "Pick a workflow first.") : LOCTEXT("HintNoModel", "Pick a model first."), EScenarioHintAction::ChangeModel);
		return Hint;
	case EScenarioGenerateState::LoadingModel:
		Set(EScenarioHintTone::Busy, FText::Format(LOCTEXT("HintLoading", "Loading {0} settings..."), Name));
		return Hint;
	case EScenarioGenerateState::ModelError:
		Set(EScenarioHintTone::Error, FText::FromString(SchemaError), EScenarioHintAction::ChangeModel);
		return Hint;
	case EScenarioGenerateState::SpendingOff:
		Set(EScenarioHintTone::Info, LOCTEXT("HintNoSpend", "SCENARIO_NO_SPEND is set: prices only, nothing is sent."));
		return Hint;
	case EScenarioGenerateState::Blocked:
		Set(EScenarioHintTone::Warning, LOCTEXT("HintBlocked", "An identical request may still be running."), EScenarioHintAction::OpenJobs);
		return Hint;
	case EScenarioGenerateState::PriceFailed:
		Set(EScenarioHintTone::Error, FText::Format(LOCTEXT("HintPriceFailed", "Price unavailable: {0}"), FText::FromString(PriceError.Message)));
		return Hint;
	case EScenarioGenerateState::Uploading:
	{
		float Progress = 0.0f;
		for (const FScenarioReference& Reference : References)
		{
			Progress = Reference.bUploading ? FMath::Max(Progress, Reference.Progress) : Progress;
		}
		Set(EScenarioHintTone::Busy, FText::Format(LOCTEXT("HintUploading", "Uploading reference... {0}%"), FText::AsNumber(FMath::RoundToInt(Progress * 100.0f))));
		return Hint;
	}
	default:
		break;
	}
	if (!LastError.IsEmpty())
	{
		Set(EScenarioHintTone::Error, FText::FromString(LastError));
		return Hint;
	}

	// The request this form sent last, on the lane it was sent from.
	FScenarioSession* Session = SessionOrNull();
	const FScenarioJobRow* Row = Session && !LastLocalId.IsEmpty() && SentLane == GetLaneKey() ? Session->Ledger().Find(LastLocalId) : nullptr;
	if (Row && Row->Kind != TEXT("prompt_spark") && Row->State != EScenarioJobState::Dismissed)
	{
		Hint.LocalId = Row->LocalId;
		const FText Model = FText::FromString(ScenarioJobActions::ModelLabel(*Row));
		const FText Cost = ScenarioJobActions::CostText(*Row);
		switch (ScenarioJobActions::View(*Row))
		{
		case ScenarioJobActions::EView::Sending:
			Set(EScenarioHintTone::Busy, LOCTEXT("HintSending", "Sending to Scenario..."));
			return Hint;
		case ScenarioJobActions::EView::Queued:
			Set(EScenarioHintTone::Busy, FText::Format(LOCTEXT("HintQueued", "Queued at Scenario for {0} · {1}. It lands in Recent."), Model, Cost));
			return Hint;
		case ScenarioJobActions::EView::Running:
			Set(EScenarioHintTone::Busy, Row->Progress > 0.0
				? FText::Format(LOCTEXT("HintRunningPct", "Generating with {0}: {1}% · {2}. It lands in Recent."), Model, FText::AsNumber(FMath::RoundToInt(Row->Progress * 100.0)), Cost)
				: FText::Format(LOCTEXT("HintRunning", "Generating with {0} · {1}. It lands in Recent."), Model, Cost));
			return Hint;
		case ScenarioJobActions::EView::Downloading:
		case ScenarioJobActions::EView::Importing:
			Set(EScenarioHintTone::Busy, FText::Format(LOCTEXT("HintFinishing", "{0}: {1}..."), Model, ScenarioJobActions::ChipText(*Row)));
			return Hint;
		case ScenarioJobActions::EView::Ready:
			Set(EScenarioHintTone::Success, FText::Format(LOCTEXT("HintReady", "Ready: {0}."), Model), EScenarioHintAction::ImportAndPlace, EScenarioHintAction::ImportOnly);
			return Hint;
		case ScenarioJobActions::EView::Imported:
		{
			UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
			const FString Folder = Subsystem ? ScenarioUI::ShortPath(Subsystem->DestinationFor(*Row)) : FString();
			Set(EScenarioHintTone::Success, FText::Format(LOCTEXT("HintImported", "Imported into {0}."), FText::FromString(Folder)), EScenarioHintAction::Show);
			return Hint;
		}
		case ScenarioJobActions::EView::DownloadStopped:
			Set(EScenarioHintTone::Warning, ScenarioJobActions::StateText(*Row), EScenarioHintAction::OpenJobs);
			return Hint;
		case ScenarioJobActions::EView::Failed:
		case ScenarioJobActions::EView::Blocked:
			Set(EScenarioHintTone::Error, FText::Format(LOCTEXT("HintFailed", "{0}: {1}"), Model, ScenarioJobActions::StateText(*Row)), EScenarioHintAction::Details);
			return Hint;
		case ScenarioJobActions::EView::Unknown:
			Set(EScenarioHintTone::Warning, LOCTEXT("HintUnknown", "We do not know if Scenario received this request."), EScenarioHintAction::CheckNow);
			return Hint;
		default:
			break;
		}
	}

	switch (State)
	{
	case EScenarioGenerateState::Submitting:
		Set(EScenarioHintTone::Busy, LOCTEXT("HintSubmitting", "Sending to Scenario..."));
		break;
	case EScenarioGenerateState::NeedsInput:
	{
		const FScenarioField* Field = FirstProblemField();
		if (!Field || Field->Type == EScenarioFieldType::Prompt)
		{
			Set(EScenarioHintTone::None, Field ? LOCTEXT("HintPrompt", "Write a prompt to generate.") : FText::FromString(FString::Join(Problems(), TEXT(" "))));
		}
		else
		{
			Set(EScenarioHintTone::None, FText::Format(LOCTEXT("HintField", "Add {0} first."), FText::FromString(Field->Label.ToLower())));
		}
		break;
	}
	case EScenarioGenerateState::Pricing:
		Set(EScenarioHintTone::Busy, LOCTEXT("HintPricing", "Checking price (free)..."));
		break;
	default:
		break;
	}
	return Hint;
}

FText FScenarioCreateModel::GetButtonTooltip() const
{
	const TSharedPtr<FUICommandInfo> Command = FScenarioCommands::IsRegistered() ? FScenarioCommands::Get().Generate : nullptr;
	const FText Chord = Command.IsValid() ? Command->GetInputText() : FText::GetEmpty();
	switch (GetState())
	{
	case EScenarioGenerateState::Priced:
	{
		const FScenarioQuote* Quote = CurrentQuote();
		const FText Price = FText::FromString(ScenarioCU::Format(Quote ? Quote->CU : ExpectedCU));
		const FText Text = Quote
			? FText::Format(LOCTEXT("PricedTip", "Sends this exact request to {0}. Scenario charges {1} for it."), FText::FromString(GetModelName()), Price)
			: FText::Format(LOCTEXT("PricedStaleTip", "The price was {0} a moment ago. Clicking checks it again and sends only if it is unchanged."), Price);
		return Chord.IsEmpty() ? Text : FText::Format(LOCTEXT("WithChord", "{0} ({1})"), Text, Chord);
	}
	case EScenarioGenerateState::PriceFailed:
		return LOCTEXT("RetryTip", "Checks the price again. Free.");
	case EScenarioGenerateState::NeedsInput:
		return FText::FromString(FString::Join(Problems(), TEXT("\n")));
	case EScenarioGenerateState::Blocked:
		return LOCTEXT("BlockedTip", "An identical request may already be running on Scenario. Open Jobs to check it or mark it as not sent.");
	default:
	{
		const FText Line = GetHint().Text;
		return Line.IsEmpty() ? LOCTEXT("GenerateTip", "Sends the request to Scenario once its price is checked.") : Line;
	}
	}
}

bool FScenarioCreateModel::CanClickGenerate() const
{
	const EScenarioGenerateState State = GetState();
	return State == EScenarioGenerateState::Priced || State == EScenarioGenerateState::PriceFailed;
}

void FScenarioCreateModel::Reprice()
{
	PriceError = FScenarioError();
	FTSTicker::GetCoreTicker().RemoveTicker(DebounceHandle);
	DebounceHandle.Reset();
	PriceNow(false);
}

bool FScenarioCreateModel::IsSentJobRunning() const
{
	FScenarioSession* Session = SessionOrNull();
	const FScenarioJobRow* Row = Session && !LastLocalId.IsEmpty() ? Session->Ledger().Find(LastLocalId) : nullptr;
	return Row && (Row->State == EScenarioJobState::Submitting || Row->State == EScenarioJobState::Queued || Row->State == EScenarioJobState::Running);
}

FText FScenarioCreateModel::GetSentJobText() const
{
	FScenarioSession* Session = SessionOrNull();
	const FScenarioJobRow* Row = Session && !LastLocalId.IsEmpty() ? Session->Ledger().Find(LastLocalId) : nullptr;
	if (!Row || Row->Kind == TEXT("prompt_spark"))
	{
		return FText::GetEmpty();
	}
	const FText Model = FText::FromString(Row->ModelName.IsEmpty() ? (Row->ModelId.IsEmpty() ? Row->WorkflowId : Row->ModelId) : Row->ModelName);
	const FText Cost = FText::FromString(ScenarioCU::Format(Row->QuotedCU));
	switch (Row->State)
	{
	case EScenarioJobState::Submitting:
		return FText::Format(LOCTEXT("SentSubmitting", "Sending to {0}..."), Model);
	case EScenarioJobState::Queued:
		return FText::Format(LOCTEXT("SentQueued", "Sent ({0}). Queued at Scenario for {1}."), Cost, Model);
	case EScenarioJobState::Running:
		return Row->Progress > 0.0
			? FText::Format(LOCTEXT("SentRunningPct", "Sent ({0}). Generating with {1}: {2}%."), Cost, Model, FText::AsNumber(FMath::RoundToInt(Row->Progress * 100.0)))
			: FText::Format(LOCTEXT("SentRunning", "Sent ({0}). Generating with {1}..."), Cost, Model);
	case EScenarioJobState::Success:
		return Row->bImported
			? FText::Format(LOCTEXT("SentImported", "Imported: {0}. It is in Results below."), Model)
			: FText::Format(LOCTEXT("SentReady", "Ready: {0}. Import it from its card below."), Model);
	case EScenarioJobState::Unknown:
		return LOCTEXT("SentUnknown", "We do not know if Scenario received this request. Check it in Jobs.");
	case EScenarioJobState::Failed:
	case EScenarioJobState::Blocked:
		return FText::Format(LOCTEXT("SentFailed", "{0} failed. See its card below."), Model);
	default:
		return FText::GetEmpty();
	}
}

void FScenarioCreateModel::Generate()
{
	const EScenarioGenerateState State = GetState();
	if (State == EScenarioGenerateState::PriceFailed)
	{
		Reprice();
		return;
	}
	if (State != EScenarioGenerateState::Priced || bSubmitting)
	{
		return;
	}
	const FScenarioQuote* Quote = CurrentQuote();
	const double CU = Quote ? Quote->CU : ExpectedCU;
	const double ConfirmAbove = UScenarioEditorSettings::Get().ConfirmAboveCU;
	if (ConfirmAbove > 0.0 && CU > ConfirmAbove)
	{
		const FText Message = FText::Format(LOCTEXT("Confirm", "Send this request to {0}? Scenario charges {1} for it."),
			FText::FromString(GetModelName()), FText::FromString(ScenarioCU::Format(CU)));
		if (FMessageDialog::Open(EAppMsgType::YesNo, Message, LOCTEXT("ConfirmTitle", "Scenario")) != EAppReturnType::Yes)
		{
			return;
		}
	}
	if (Quote)
	{
		Submit();
		return;
	}
	// The quote expired: check again, and send only if the price did not change.
	bGenerateAfterPrice = true;
	PriceNow(false);
}

void FScenarioCreateModel::Submit()
{
	FScenarioSession* Session = SessionOrNull();
	if (!Session)
	{
		return;
	}
	bSubmitting = true;
	LastError.Reset();
	OnChanged.Broadcast();
	TWeakPtr<FScenarioCreateModel> WeakThis = AsShared();
	Session->Submit(BuildRequest(), FString(), FString(), [WeakThis](const TScenarioResult<FString>& Result)
	{
		const TSharedPtr<FScenarioCreateModel> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		This->bSubmitting = false;
		This->LastLocalId = Result.Value;
		This->SentLane = This->GetLaneKey();
		This->LastError = Result.bOk ? FString() : Result.Error.Message;
		if (Result.bOk)
		{
			This->WaitingUntil = ScenarioNow() + 2.0;
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis](float)
			{
				if (const TSharedPtr<FScenarioCreateModel> Later = WeakThis.Pin())
				{
					Later->OnChanged.Broadcast();
				}
				return false;
			}), 2.1f);
		}
		// The quote was used; the same request can be priced again for another take.
		This->PricedFingerprint.Reset();
		This->Changed(true);
	});
}

void FScenarioCreateModel::Changed(bool bReprice)
{
	bFingerprintDirty = true;
	if (bReprice)
	{
		++PriceGeneration;
		bPricing = false;
		bGenerateAfterPrice = false;
		PriceError = FScenarioError();
		SchedulePrice(PriceDelay);
	}
	OnChanged.Broadcast();
}

void FScenarioCreateModel::SchedulePrice(float Delay)
{
	FTSTicker::GetCoreTicker().RemoveTicker(DebounceHandle);
	DebounceHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FScenarioCreateModel::OnDebounce), Delay);
}

bool FScenarioCreateModel::OnDebounce(float DeltaSeconds)
{
	DebounceHandle.Reset();
	PriceNow(true);
	return false;
}

void FScenarioCreateModel::Estimate()
{
	// Only an empty prompt stands between the form and a price: price the lane's example prompt instead.
	FScenarioSession* Session = SessionOrNull();
	const FScenarioLane* Lane = GetLane();
	const FScenarioField* Missing = FirstProblemField();
	if (!Session || bEstimating || EstimateModel == ModelId || Problems().Num() != 1 || !Missing || Missing->Type != EScenarioFieldType::Prompt)
	{
		return;
	}
	const FString Example = !bWorkflow && Lane && Lane->StarterPrompts.Num() > 0 ? Lane->StarterPrompts[0] : FString(TEXT("a small stone lantern"));
	FScenarioRunRequest Request = BuildRequest();
	if (!Request.Parameters.IsValid())
	{
		return;
	}
	Request.Parameters->SetStringField(Missing->Name, Example);
	bEstimating = true;
	const FString For = ModelId;
	TWeakPtr<FScenarioCreateModel> WeakThis = AsShared();
	Session->Price(Request, true, [WeakThis, For](const TScenarioResult<FScenarioQuote>& Result)
	{
		const TSharedPtr<FScenarioCreateModel> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		This->bEstimating = false;
		if (Result.bOk && For == This->ModelId && This->EstimateModel != For)
		{
			This->EstimateCU = Result.Value.CU;
			This->EstimateModel = For;
		}
		This->OnChanged.Broadcast();
		// The model changed while this one was priced.
		if (For != This->ModelId)
		{
			This->Estimate();
		}
	});
}

void FScenarioCreateModel::PriceNow(bool bAutomatic)
{
	FScenarioSession* Session = SessionOrNull();
	if (!Session || !Session->IsReady() || ModelId.IsEmpty() || bLoadingSchema || !SchemaError.IsEmpty() || Fields.Num() == 0)
	{
		return;
	}
	if (References.ContainsByPredicate([](const FScenarioReference& Reference) { return Reference.bUploading; }))
	{
		return;
	}
	if (Problems().Num() > 0)
	{
		Estimate();
		return;
	}
	const FScenarioRunRequest Request = BuildRequest();
	const FString Fp = Session->Fingerprint(Request);
	const int32 Generation = ++PriceGeneration;
	bPricing = true;
	OnChanged.Broadcast();

	TWeakPtr<FScenarioCreateModel> WeakThis = AsShared();
	Session->Price(Request, bAutomatic, [WeakThis, Generation, Fp](const TScenarioResult<FScenarioQuote>& Result)
	{
		const TSharedPtr<FScenarioCreateModel> This = WeakThis.Pin();
		if (!This.IsValid() || Generation != This->PriceGeneration)
		{
			// The form changed while the price was checked: this answer is for another request.
			return;
		}
		This->bPricing = false;
		const bool bSendNow = This->bGenerateAfterPrice;
		This->bGenerateAfterPrice = false;
		if (!Result.bOk)
		{
			This->PriceError = Result.Error;
			This->PricedFingerprint.Reset();
			if (bSendNow)
			{
				This->LastError = Result.Error.Message;
			}
			This->OnChanged.Broadcast();
			return;
		}
		const bool bSamePrice = !This->ExpectedExact.IsEmpty() && This->ExpectedExact == Result.Value.Exact;
		This->PriceError = FScenarioError();
		This->PricedFingerprint = Fp;
		This->ExpectedExact = Result.Value.Exact;
		This->ExpectedCU = Result.Value.CU;
		This->EstimateCU = Result.Value.CU;
		This->EstimateModel = This->ModelId;
		if (bSendNow)
		{
			if (bSamePrice)
			{
				This->Submit();
				return;
			}
			This->LastError = FString::Printf(TEXT("The price changed to %s. Check it and click Generate again."), *ScenarioCU::Format(Result.Value.CU));
		}
		This->OnChanged.Broadcast();
	});
}

void FScenarioCreateModel::OnSessionChanged()
{
	FScenarioSession* Session = SessionOrNull();
	if (!Session)
	{
		return;
	}
	const FString Key = FString::Printf(TEXT("%d|%s"), static_cast<int32>(Session->GetConnection()), *Session->Scope());
	if (Key == SessionKey)
	{
		// Ledger updates change Blocked and Submitting, not the price.
		OnChanged.Broadcast();
		return;
	}
	const bool bWasSignedOut = SessionKey.IsEmpty() || Fields.Num() == 0;
	SessionKey = Key;
	if (Session->IsSignedIn() && bWasSignedOut && !bLoadingSchema)
	{
		LoadSchema();
		return;
	}
	PricedFingerprint.Reset();
	Changed(true);
}

// Prompt tools

FScenarioRunRequest FScenarioCreateModel::SparkRequest() const
{
	FScenarioRunRequest Request;
	Request.Kind = EScenarioQuoteKind::PromptSpark;
	Request.TargetId = ModelId;
	Request.TargetName = TEXT("Prompt Spark");
	Request.Lane = LaneId;
	Request.Client = TEXT("Unreal Editor");

	TArray<FString> Images;
	for (const FScenarioReference& Reference : References)
	{
		const FScenarioField* Field = ScenarioSchema::Find(Fields, Reference.Field);
		if (Field && (Field->Kind.IsEmpty() || Field->Kind == TEXT("image")) && !Reference.AssetId.IsEmpty())
		{
			Images.Add(Reference.AssetId);
		}
	}
	const TSharedRef<FJsonObject> Parameters = MakeShared<FJsonObject>();
	Parameters->SetStringField(TEXT("prompt"), GetPrompt());
	if (Images.Num() > 0)
	{
		Parameters->SetField(TEXT("images"), ScenarioJson::StringArrayValue(Images));
	}
	Parameters->SetNumberField(TEXT("num_results"), 3);
	Request.Parameters = Parameters;
	return Request;
}

void FScenarioCreateModel::PriceSpark(TFunction<void(const FString& Price, const FString& Error)> OnDone)
{
	FScenarioSession* Session = SessionOrNull();
	if (!Session)
	{
		OnDone(FString(), TEXT("Sign in first."));
		return;
	}
	if (GetPrompt().TrimStartAndEnd().IsEmpty())
	{
		OnDone(FString(), TEXT("Write a few words first."));
		return;
	}
	Session->Price(SparkRequest(), false, [OnDone](const TScenarioResult<FScenarioQuote>& Result)
	{
		OnDone(Result.bOk ? ScenarioCU::Format(Result.Value.CU) : FString(), Result.bOk ? FString() : Result.Error.Message);
	});
}

void FScenarioCreateModel::RunSpark(TFunction<void(const TArray<FString>& Variants, const FString& Error, const FString& LocalId)> OnDone)
{
	FScenarioSession* Session = SessionOrNull();
	if (!Session)
	{
		OnDone({}, TEXT("Sign in first."), FString());
		return;
	}
	Session->Submit(SparkRequest(), FString(), FString(), [OnDone](const TScenarioResult<FString>& Result)
	{
		if (!Result.bOk)
		{
			OnDone({}, Result.Error.Message, FString());
			return;
		}
		FScenarioSession* Session = SessionOrNull();
		const TSharedPtr<FJsonObject> Reply = Session ? Session->GetSparkResult(Result.Value) : nullptr;
		OnDone(FScenarioApi::SparkPrompts(Reply), FString(), Result.Value);
	});
}

void FScenarioCreateModel::Suggest(TFunction<void(const TArray<TPair<FString, FString>>& Models, const FString& Error)> OnDone)
{
	FScenarioSession* Session = SessionOrNull();
	const FScenarioLane* Lane = GetLane();
	if (!Session || !Session->IsReady() || !Lane)
	{
		OnDone({}, TEXT("Sign in and choose a project first."));
		return;
	}
	FString Capability = Lane->Capability;
	if (LaneId == TEXT("3d") && Mode == TEXT("image"))
	{
		Capability = TEXT("img23d");
	}
	FString Prompt = GetPrompt().TrimStartAndEnd();
	if (Prompt.IsEmpty() && Lane->StarterPrompts.Num() > 0)
	{
		Prompt = Lane->StarterPrompts[0];
	}
	TWeakPtr<FScenarioSession> WeakSession = Session->AsShared();
	Session->Api().Recommend(Capability, Prompt, 5, [WeakSession, OnDone](const FScenarioReply& Reply)
	{
		if (!Reply.bOk)
		{
			OnDone({}, Reply.Error.Message);
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Rows = ScenarioJson::GetArray(Reply.Value, TEXT("ranked"));
		if (Rows.Num() == 0)
		{
			Rows = ScenarioJson::GetArray(Reply.Value, TEXT("models"));
		}
		if (Rows.Num() == 0)
		{
			Rows = ScenarioJson::GetArray(Reply.Value, TEXT("results"));
		}
		const TSharedPtr<FScenarioSession> Pinned = WeakSession.Pin();
		const UScenarioProjectSettings& Project = UScenarioProjectSettings::Get();
		TArray<TPair<FString, FString>> Models;
		for (const TSharedPtr<FJsonValue>& Row : Rows)
		{
			const TSharedPtr<FJsonObject> Object = Row->Type == EJson::Object ? Row->AsObject() : nullptr;
			FString Id = ScenarioJson::GetString(Object, TEXT("model_id"), ScenarioJson::GetString(Object, TEXT("id")));
			if (Id.IsEmpty() || !Project.IsModelAllowed(Id))
			{
				continue;
			}
			FString Name = ScenarioJson::GetString(Object, TEXT("name"), ScenarioJson::GetString(Object, TEXT("model_name")));
			if (Name.IsEmpty() && Pinned.IsValid())
			{
				const FScenarioCatalogModel* Known = Pinned->Catalog().Find(Id);
				Name = Known ? Known->Summary.Name : Id;
			}
			Models.Add({ Id, Name });
		}
		OnDone(Models, Models.Num() == 0 ? TEXT("Scenario had no recommendation for this prompt.") : FString());
	});
}

#undef LOCTEXT_NAMESPACE
