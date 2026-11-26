// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioTrained.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScenarioApi.h"
#include "ScenarioCoreModule.h"
#include "ScenarioJson.h"

namespace
{
	/** Longer text is a description, not trigger words. */
	constexpr int32 MaxTriggerLength = 48;

	/** The base inputs that carry a trained model; the trained form fills them. */
	const TCHAR* const TrainedInputs[] = { TEXT("modelId"), TEXT("loras"), TEXT("lorasScale") };

	FString CleanWords(const FString& Value)
	{
		TArray<FString> Words;
		Value.ParseIntoArrayWS(Words);
		const FString Out = FString::Join(Words, TEXT(" "));
		return Out.Len() <= MaxTriggerLength ? Out : FString();
	}

	bool IsWordChar(TCHAR Char)
	{
		return (Char >= TEXT('A') && Char <= TEXT('Z')) || (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('_');
	}

	/** Runs of letters, digits and underscores, in order. */
	TArray<FString> WordsOf(const FString& Text)
	{
		TArray<FString> Words;
		FString Current;
		for (const TCHAR Char : Text)
		{
			if (IsWordChar(Char))
			{
				Current.AppendChar(Char);
			}
			else if (!Current.IsEmpty())
			{
				Words.Add(MoveTemp(Current));
				Current.Reset();
			}
		}
		if (!Current.IsEmpty())
		{
			Words.Add(MoveTemp(Current));
		}
		return Words;
	}

	/** A word in capitals: a capital letter, then capitals, digits or underscores, four characters at least. */
	bool IsCapitalWord(const FString& Word)
	{
		if (Word.Len() < 4 || Word.Len() > MaxTriggerLength || Word[0] < TEXT('A') || Word[0] > TEXT('Z'))
		{
			return false;
		}
		for (const TCHAR Char : Word)
		{
			if (!((Char >= TEXT('A') && Char <= TEXT('Z')) || (Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('_')))
			{
				return false;
			}
		}
		return true;
	}

	bool IsTrainedInput(const FString& Name)
	{
		for (const TCHAR* Input : TrainedInputs)
		{
			if (Name == Input)
			{
				return true;
			}
		}
		return false;
	}

	TArray<FString> ModelTypes(const TSharedPtr<FJsonObject>& Input)
	{
		TArray<FString> Types = ScenarioJson::GetStringArray(Input, TEXT("modelTypes"));
		return Types.Num() > 0 ? Types : ScenarioJson::GetStringArray(Input, TEXT("model_types"));
	}

	/** True when Text holds Words (any case) with no letter or digit glued to either end: FAKESTYLE is not in FAKESTYLES. */
	bool HoldsWords(const FString& Text, const FString& Words)
	{
		auto IsInner = [](TCHAR Char) { return FChar::IsAlnum(Char) || Char == TEXT('_'); };
		for (int32 Start = Text.Find(Words, ESearchCase::IgnoreCase); Start != INDEX_NONE; Start = Text.Find(Words, ESearchCase::IgnoreCase, ESearchDir::FromStart, Start + 1))
		{
			const int32 End = Start + Words.Len();
			const bool bOpen = Start == 0 || !IsInner(Words[0]) || !IsInner(Text[Start - 1]);
			const bool bClose = End >= Text.Len() || !IsInner(Words[Words.Len() - 1]) || !IsInner(Text[End]);
			if (bOpen && bClose)
			{
				return true;
			}
		}
		return false;
	}
}

// Data

const FScenarioTrainedData& FScenarioTrainedData::Get()
{
	static FScenarioTrainedData Instance;
	static bool bLoaded = false;
	if (!bLoaded)
	{
		bLoaded = true;
		FString Text;
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scenario"));
		const FString Path = Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Trained.json")) : FString();
		if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path))
		{
			UE_LOG(LogScenario, Warning, TEXT("Scenario's Trained.json is missing: trained models cannot run."));
		}
		else
		{
			Instance = FromJson(ScenarioJson::ParseObject(Text));
		}
	}
	return Instance;
}

FScenarioTrainedData FScenarioTrainedData::FromJson(const TSharedPtr<FJsonObject>& Json)
{
	FScenarioTrainedData Data;
	Data.BaseProbes = ScenarioJson::GetStringArray(Json, TEXT("base_probes"));
	if (const TSharedPtr<FJsonObject> Names = ScenarioJson::GetObject(Json, TEXT("base_names")))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ScenarioJson::Fields(Names))
		{
			const FString Id = ScenarioJson::ValueText(Pair.Value);
			if (!Id.IsEmpty())
			{
				Data.BaseNames.Add(Pair.Key, Id);
			}
		}
	}
	Data.DefaultScale = ScenarioJson::GetNumber(Json, TEXT("default_scale"), Data.DefaultScale);
	return Data;
}

// Bases

bool FScenarioTrainedBase::FromModel(const TSharedPtr<FJsonObject>& Model, FScenarioTrainedBase& Out)
{
	Out = FScenarioTrainedBase();
	Out.Id = ScenarioJson::GetString(Model, TEXT("id"));
	Out.Name = ScenarioJson::GetString(Model, TEXT("name"), Out.Id);
	bool bPrompt = false;
	for (const TSharedPtr<FJsonValue>& Value : ScenarioJson::GetArray(Model, TEXT("inputs")))
	{
		const TSharedPtr<FJsonObject> Input = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
		const FString Name = ScenarioJson::GetString(Input, TEXT("name"));
		if (Name == TEXT("loras"))
		{
			Out.LoraTypes = ModelTypes(Input);
			Out.MaxLoras = static_cast<int32>(ScenarioJson::GetNumber(Input, TEXT("maxLength"), ScenarioJson::GetNumber(Input, TEXT("max_length"), 0.0)));
		}
		else if (Name == TEXT("modelId"))
		{
			Out.ModelIdTypes = ModelTypes(Input);
		}
		else if (Name == TEXT("lorasScale"))
		{
			Out.ScaleMin = ScenarioJson::GetNumber(Input, TEXT("min"), Out.ScaleMin);
			Out.ScaleMax = ScenarioJson::GetNumber(Input, TEXT("max"), Out.ScaleMax);
			Out.ScaleStep = ScenarioJson::GetNumber(Input, TEXT("step"), Out.ScaleStep);
		}
		else if (!bPrompt && !Name.IsEmpty() && (Name == TEXT("prompt") || ScenarioJson::Truthy(Input->TryGetField(TEXT("prompt")))))
		{
			Out.PromptField = Name;
			Out.PromptMaxLength = static_cast<int32>(ScenarioJson::GetNumber(Input, TEXT("maxLength"), ScenarioJson::GetNumber(Input, TEXT("max_length"), 0.0)));
			bPrompt = true;
		}
	}
	return !Out.Id.IsEmpty() && (Out.LoraTypes.Num() > 0 || Out.ModelIdTypes.Num() > 0);
}

TSharedRef<FJsonObject> FScenarioTrainedBase::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("id"), Id);
	Json->SetStringField(TEXT("name"), Name);
	Json->SetField(TEXT("lora_types"), ScenarioJson::StringArrayValue(LoraTypes));
	Json->SetField(TEXT("model_id_types"), ScenarioJson::StringArrayValue(ModelIdTypes));
	Json->SetNumberField(TEXT("max_loras"), MaxLoras);
	Json->SetNumberField(TEXT("scale_min"), ScaleMin);
	Json->SetNumberField(TEXT("scale_max"), ScaleMax);
	Json->SetNumberField(TEXT("scale_step"), ScaleStep);
	Json->SetStringField(TEXT("prompt_field"), PromptField);
	Json->SetNumberField(TEXT("prompt_max_length"), PromptMaxLength);
	return Json;
}

bool FScenarioTrainedBase::FromJson(const TSharedPtr<FJsonObject>& Json, FScenarioTrainedBase& Out)
{
	Out = FScenarioTrainedBase();
	Out.Id = ScenarioJson::GetString(Json, TEXT("id"));
	Out.Name = ScenarioJson::GetString(Json, TEXT("name"), Out.Id);
	Out.LoraTypes = ScenarioJson::GetStringArray(Json, TEXT("lora_types"));
	Out.ModelIdTypes = ScenarioJson::GetStringArray(Json, TEXT("model_id_types"));
	Out.MaxLoras = static_cast<int32>(ScenarioJson::GetNumber(Json, TEXT("max_loras")));
	Out.ScaleMin = ScenarioJson::GetNumber(Json, TEXT("scale_min"), Out.ScaleMin);
	Out.ScaleMax = ScenarioJson::GetNumber(Json, TEXT("scale_max"), Out.ScaleMax);
	Out.ScaleStep = ScenarioJson::GetNumber(Json, TEXT("scale_step"), Out.ScaleStep);
	Out.PromptField = ScenarioJson::GetString(Json, TEXT("prompt_field"), Out.PromptField);
	Out.PromptMaxLength = static_cast<int32>(ScenarioJson::GetNumber(Json, TEXT("prompt_max_length")));
	return !Out.Id.IsEmpty() && (Out.LoraTypes.Num() > 0 || Out.ModelIdTypes.Num() > 0);
}

// Info

FString FScenarioTrainedInfo::Label() const
{
	if (!IsRunnable())
	{
		return FString::Printf(TEXT("No base model for %s"), Type.IsEmpty() ? TEXT("this type") : *Type);
	}
	return FString::Printf(TEXT("%s via %s"), Kind == EScenarioTrainedKind::Lora ? TEXT("LoRA") : TEXT("Composition"), *BaseName());
}

FString FScenarioTrainedInfo::Problem() const
{
	if (IsRunnable())
	{
		return FString();
	}
	return FString::Printf(TEXT("This trained model's type (%s) has no base model in this version of the plugin, so it cannot run here."), Type.IsEmpty() ? TEXT("unknown") : *Type);
}

TSharedRef<FJsonObject> FScenarioTrainedInfo::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("type"), Type);
	Json->SetStringField(TEXT("kind"), ScenarioTrained::KindName(Kind));
	Json->SetStringField(TEXT("base_model_id"), BaseId());
	Json->SetStringField(TEXT("base_name"), BaseName());
	if (Kind == EScenarioTrainedKind::Lora)
	{
		Json->SetNumberField(TEXT("weight"), Scale);
		Json->SetNumberField(TEXT("weight_min"), ScaleMin);
		Json->SetNumberField(TEXT("weight_max"), ScaleMax);
	}
	if (!TriggerWords.IsEmpty())
	{
		Json->SetStringField(TEXT("trigger_words"), TriggerWords);
	}
	return Json;
}

// Derivation

bool ScenarioTrained::IsTrainedType(const FString& Type)
{
	return !Type.IsEmpty() && !Type.Equals(TEXT("custom"), ESearchCase::IgnoreCase);
}

FString ScenarioTrained::KindName(EScenarioTrainedKind Kind)
{
	switch (Kind)
	{
	case EScenarioTrainedKind::Lora: return TEXT("lora");
	case EScenarioTrainedKind::Composition: return TEXT("composition");
	default: return FString();
	}
}

FScenarioTrainedInfo ScenarioTrained::Derive(const FScenarioModelSummary& Model, const TArray<FScenarioTrainedBase>& Bases, const FScenarioTrainedData& Data)
{
	FScenarioTrainedInfo Info;
	Info.Id = Model.Id;
	Info.Name = Model.Name.IsEmpty() ? Model.Id : Model.Name;
	Info.Type = Model.Type;
	Info.TriggerWords = Model.TriggerWords;

	// A LoRA when a loras input lists the type; a composition when only a modelId input does.
	TArray<FScenarioTrainedBase> Loras;
	TArray<FScenarioTrainedBase> Compositions;
	for (const FScenarioTrainedBase& Base : Bases)
	{
		if (Base.LoraTypes.Contains(Model.Type))
		{
			Loras.Add(Base);
		}
		else if (Base.ModelIdTypes.Contains(Model.Type))
		{
			Compositions.Add(Base);
		}
	}
	Info.Kind = Loras.Num() > 0 ? EScenarioTrainedKind::Lora : Compositions.Num() > 0 ? EScenarioTrainedKind::Composition : EScenarioTrainedKind::None;
	TArray<FScenarioTrainedBase>& Candidates = Info.Kind == EScenarioTrainedKind::Lora ? Loras : Compositions;

	// compliantModelIds holds base names, not ids: through the name map it puts its bases first. Then the probe order.
	TArray<FString> Preferred;
	for (const FString& Name : Model.CompliantModelIds)
	{
		if (const FString* Id = Data.BaseNames.Find(Name))
		{
			Preferred.AddUnique(*Id);
		}
	}
	auto Rank = [](const TArray<FString>& List, const FString& Id)
	{
		const int32 Index = List.IndexOfByKey(Id);
		return Index == INDEX_NONE ? List.Num() : Index;
	};
	Candidates.StableSort([&Preferred, &Data, &Rank](const FScenarioTrainedBase& A, const FScenarioTrainedBase& B)
	{
		const int32 PreferA = Rank(Preferred, A.Id);
		const int32 PreferB = Rank(Preferred, B.Id);
		if (PreferA != PreferB)
		{
			return PreferA < PreferB;
		}
		return Rank(Data.BaseProbes, A.Id) < Rank(Data.BaseProbes, B.Id);
	});
	for (const FScenarioTrainedBase& Base : Candidates)
	{
		Info.BaseIds.Add(Base.Id);
		Info.BaseNames.Add(Base.Name.IsEmpty() ? Base.Id : Base.Name);
	}
	if (Candidates.Num() > 0)
	{
		Info.ScaleMin = Candidates[0].ScaleMin;
		Info.ScaleMax = FMath::Max(Candidates[0].ScaleMin, Candidates[0].ScaleMax);
		Info.ScaleStep = Candidates[0].ScaleStep;
		Info.PromptField = Candidates[0].PromptField;
		Info.PromptMaxLength = Candidates[0].PromptMaxLength;
	}
	Info.Scale = FMath::Clamp(Model.OwnScale >= 0.0 ? Model.OwnScale : Data.DefaultScale, Info.ScaleMin, Info.ScaleMax);
	return Info;
}

FString ScenarioTrained::TriggerWords(const TSharedPtr<FJsonObject>& Record)
{
	const TSharedPtr<FJsonObject> Parameters = ScenarioJson::GetObject(Record, TEXT("parameters"));
	for (const FString& Direct : { CleanWords(ScenarioJson::GetString(Parameters, TEXT("conceptPrompt"))), CleanWords(ScenarioJson::GetString(Record, TEXT("promptEmbedding"))) })
	{
		if (!Direct.IsEmpty())
		{
			return Direct;
		}
	}
	// Trainings that leave conceptPrompt empty write the trigger into each sample prompt: a word in capitals they all hold.
	TArray<TArray<FString>> Samples;
	for (const TSharedPtr<FJsonValue>& Value : ScenarioJson::GetArray(Parameters, TEXT("samplePrompts")))
	{
		if (Value.IsValid() && Value->Type == EJson::String && !Value->AsString().IsEmpty())
		{
			Samples.Add(WordsOf(Value->AsString()));
		}
	}
	if (Samples.Num() < 2)
	{
		return FString();
	}
	for (const FString& Word : Samples[0])
	{
		if (!IsCapitalWord(Word))
		{
			continue;
		}
		bool bEverywhere = true;
		for (int32 Index = 1; Index < Samples.Num() && bEverywhere; ++Index)
		{
			bEverywhere = Samples[Index].Contains(Word);
		}
		if (bEverywhere)
		{
			return Word;
		}
	}
	return FString();
}

double ScenarioTrained::OwnScale(const TSharedPtr<FJsonObject>& Record)
{
	const FString Id = ScenarioJson::GetString(Record, TEXT("id"));
	for (const TSharedPtr<FJsonValue>& Value : ScenarioJson::GetArray(Record, TEXT("concepts")))
	{
		const TSharedPtr<FJsonObject> Concept = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
		if (!Id.IsEmpty() && ScenarioJson::GetString(Concept, TEXT("modelId")) == Id && ScenarioJson::HasNumber(Concept, TEXT("scale")))
		{
			return ScenarioJson::GetNumber(Concept, TEXT("scale"));
		}
	}
	return -1.0;
}

FString ScenarioTrained::WithTriggerWords(const FString& Prompt, const FString& Words)
{
	const FString Clean = Words.TrimStartAndEnd();
	if (Clean.IsEmpty() || Prompt.TrimStartAndEnd().IsEmpty() || HoldsWords(Prompt, Clean))
	{
		return Prompt;
	}
	return Clean + TEXT(", ") + Prompt.TrimStart();
}

FString ScenarioTrained::PromptProblem(const FScenarioTrainedInfo& Info, const TSharedPtr<FJsonObject>& Parameters)
{
	const int32 Length = ScenarioJson::GetString(Parameters, Info.PromptField).Len();
	if (Info.PromptMaxLength <= 0 || Length <= Info.PromptMaxLength)
	{
		return FString();
	}
	return Info.TriggerWords.IsEmpty()
		? FString::Printf(TEXT("The prompt is longer than %d characters."), Info.PromptMaxLength)
		: FString::Printf(TEXT("With its trigger words the prompt is longer than %d characters. Shorten it, or turn Trigger words off."), Info.PromptMaxLength);
}

// Forms and requests

TSharedRef<FJsonObject> ScenarioTrained::FormSchema(const FScenarioTrainedInfo& Info, const TSharedPtr<FJsonObject>& BaseSchema)
{
	TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("model_id"), Info.Id);
	Schema->SetStringField(TEXT("name"), Info.Name);
	Schema->SetField(TEXT("capabilities"), ScenarioJson::StringArrayValue(ScenarioJson::GetStringArray(BaseSchema, TEXT("capabilities"))));
	TArray<TSharedPtr<FJsonValue>> Parameters;
	for (const TSharedPtr<FJsonValue>& Value : ScenarioJson::GetArray(BaseSchema, TEXT("parameters")))
	{
		const TSharedPtr<FJsonObject> Input = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
		if (Input.IsValid() && !IsTrainedInput(ScenarioJson::GetString(Input, TEXT("name"))))
		{
			Parameters.Add(Value);
		}
	}
	if (Info.Kind == EScenarioTrainedKind::Lora)
	{
		TSharedRef<FJsonObject> Weight = MakeShared<FJsonObject>();
		Weight->SetStringField(TEXT("name"), WeightField);
		Weight->SetStringField(TEXT("type"), TEXT("number"));
		Weight->SetStringField(TEXT("label"), TEXT("LoRA weight"));
		Weight->SetStringField(TEXT("description"), FString::Printf(TEXT("How strongly %s applies, %s to %s. Its own weight is %s."),
			*Info.Name, *ScenarioJson::NumberText(Info.ScaleMin), *ScenarioJson::NumberText(Info.ScaleMax), *ScenarioJson::NumberText(Info.Scale)));
		Weight->SetNumberField(TEXT("min"), Info.ScaleMin);
		Weight->SetNumberField(TEXT("max"), Info.ScaleMax);
		Weight->SetNumberField(TEXT("step"), Info.ScaleStep);
		Weight->SetNumberField(TEXT("default"), Info.Scale);
		Parameters.Add(MakeShared<FJsonValueObject>(Weight));
	}
	if (!Info.TriggerWords.IsEmpty())
	{
		TSharedRef<FJsonObject> Trigger = MakeShared<FJsonObject>();
		Trigger->SetStringField(TEXT("name"), TriggerField);
		Trigger->SetStringField(TEXT("type"), TEXT("boolean"));
		Trigger->SetStringField(TEXT("label"), TEXT("Trigger words"));
		Trigger->SetStringField(TEXT("description"), FString::Printf(TEXT("Puts \"%s\" before the prompt unless the prompt already has it."), *Info.TriggerWords));
		Trigger->SetBoolField(TEXT("default"), true);
		Parameters.Add(MakeShared<FJsonValueObject>(Trigger));
	}
	Schema->SetArrayField(TEXT("parameters"), Parameters);
	Schema->SetObjectField(TEXT("trained"), Info.ToJson());
	return Schema;
}

TSharedRef<FJsonObject> ScenarioTrained::BaseParameters(const FScenarioTrainedInfo& Info, const TSharedPtr<FJsonObject>& Payload)
{
	TSharedRef<FJsonObject> Out = Payload.IsValid() ? ScenarioJson::CloneObject(Payload).ToSharedRef() : MakeShared<FJsonObject>();
	const double Weight = ScenarioJson::HasNumber(Out, WeightField) ? ScenarioJson::GetNumber(Out, WeightField) : Info.Scale;
	const bool bTrigger = !Out->HasField(TriggerField) || ScenarioJson::Truthy(Out->TryGetField(TriggerField));
	Out->RemoveField(TriggerField);
	for (const TCHAR* Input : TrainedInputs)
	{
		Out->RemoveField(Input);
	}
	if (Info.Kind == EScenarioTrainedKind::Lora)
	{
		Out->SetField(TEXT("loras"), ScenarioJson::StringArrayValue({ Info.Id }));
		Out->SetArrayField(TEXT("lorasScale"), { MakeShared<FJsonValueNumber>(Weight) });
	}
	else if (Info.Kind == EScenarioTrainedKind::Composition)
	{
		// A composition brings its own LoRAs and weights.
		Out->SetStringField(TEXT("modelId"), Info.Id);
	}
	const FString Prompt = ScenarioJson::GetString(Out, Info.PromptField);
	if (bTrigger && !Prompt.IsEmpty())
	{
		Out->SetStringField(Info.PromptField, WithTriggerWords(Prompt, Info.TriggerWords));
	}
	return Out;
}

TSharedRef<FJsonObject> ScenarioTrained::FormValues(const TSharedPtr<FJsonObject>& Parameters)
{
	TSharedRef<FJsonObject> Out = Parameters.IsValid() ? ScenarioJson::CloneObject(Parameters).ToSharedRef() : MakeShared<FJsonObject>();
	const TArray<TSharedPtr<FJsonValue>> Scales = ScenarioJson::GetArray(Out, TEXT("lorasScale"));
	for (const TCHAR* Input : TrainedInputs)
	{
		Out->RemoveField(Input);
	}
	if (Scales.Num() > 0 && Scales[0].IsValid() && Scales[0]->Type == EJson::Number)
	{
		Out->SetNumberField(WeightField, Scales[0]->AsNumber());
	}
	return Out;
}

FString ScenarioTrained::AttachedId(const TSharedPtr<FJsonObject>& Parameters)
{
	const TArray<FString> Loras = ScenarioJson::GetStringArray(Parameters, TEXT("loras"));
	return Loras.Num() > 0 ? Loras[0] : ScenarioJson::GetString(Parameters, TEXT("modelId"));
}

bool ScenarioTrained::HasFormFields(const TSharedPtr<FJsonObject>& Payload)
{
	return Payload.IsValid() && (Payload->HasField(TriggerField) || ScenarioJson::HasNumber(Payload, WeightField));
}
