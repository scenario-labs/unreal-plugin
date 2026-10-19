// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioSchema.h"

#include "ScenarioJson.h"

namespace
{
	TOptional<double> OptionalNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		if (ScenarioJson::HasNumber(Object, Field))
		{
			return ScenarioJson::GetNumber(Object, Field);
		}
		return TOptional<double>();
	}

	/** First present field among snake_case and camelCase spellings. */
	TSharedPtr<FJsonValue> FieldOf(const TSharedPtr<FJsonObject>& Object, std::initializer_list<const TCHAR*> Names)
	{
		for (const TCHAR* Name : Names)
		{
			const TSharedPtr<FJsonValue> Value = Object->TryGetField(Name);
			if (Value.IsValid() && Value->Type != EJson::Null)
			{
				return Value;
			}
		}
		return nullptr;
	}

	bool LooksInteger(const FString& Name, const TOptional<double>& Step)
	{
		if (Step.IsSet())
		{
			return Step.GetValue() == FMath::FloorToDouble(Step.GetValue()) && Step.GetValue() >= 1.0;
		}
		const FString Lower = Name.ToLower();
		for (const TCHAR* Hint : { TEXT("seed"), TEXT("num"), TEXT("count"), TEXT("steps"), TEXT("face") })
		{
			if (Lower.Contains(Hint))
			{
				return true;
			}
		}
		return false;
	}

	bool InOptions(const TSharedPtr<FJsonValue>& Value, const TArray<TSharedPtr<FJsonValue>>& Options)
	{
		for (const TSharedPtr<FJsonValue>& Option : Options)
		{
			if (!Option.IsValid())
			{
				continue;
			}
			if (Option->Type == EJson::Number && Value->Type == EJson::Number && Option->AsNumber() == Value->AsNumber())
			{
				return true;
			}
			if (Option->Type == EJson::String && Value->Type == EJson::String && Option->AsString() == Value->AsString())
			{
				return true;
			}
			if (Option->Type == EJson::Boolean && Value->Type == EJson::Boolean && Option->AsBool() == Value->AsBool())
			{
				return true;
			}
		}
		return false;
	}

	FScenarioField ParseField(const TSharedPtr<FJsonObject>& P)
	{
		FScenarioField Field;
		Field.Name = ScenarioJson::GetString(P, TEXT("name"));
		const FString RawType = ScenarioJson::GetString(P, TEXT("type"), TEXT("string"));
		Field.Label = ScenarioJson::GetString(P, TEXT("label"));
		if (Field.Label.IsEmpty())
		{
			Field.Label = ScenarioSchema::Humanize(Field.Name);
		}
		Field.Description = ScenarioJson::GetString(P, TEXT("description"));
		Field.Placeholder = ScenarioJson::GetString(P, TEXT("placeholder"));
		Field.Group = ScenarioJson::GetString(P, TEXT("group"));
		// Models say "required": true; workflows say "required": {"always": true}.
		Field.bRequired = ScenarioJson::Truthy(P->TryGetField(TEXT("required")));
		Field.Default = P->TryGetField(TEXT("default"));
		if (Field.Default.IsValid() && Field.Default->Type == EJson::Null)
		{
			Field.Default.Reset();
		}
		Field.Min = OptionalNumber(P, TEXT("min"));
		Field.Max = OptionalNumber(P, TEXT("max"));
		Field.Step = OptionalNumber(P, TEXT("step"));
		if (const TSharedPtr<FJsonValue> Allowed = FieldOf(P, { TEXT("allowed_values"), TEXT("allowedValues"), TEXT("options") }))
		{
			if (Allowed->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Option : Allowed->AsArray())
				{
					// Workflows sometimes list {value, label} objects.
					if (Option.IsValid() && Option->Type == EJson::Object)
					{
						const TSharedPtr<FJsonValue> Inner = Option->AsObject()->TryGetField(TEXT("value"));
						if (Inner.IsValid())
						{
							Field.Options.Add(Inner);
						}
					}
					else if (Option.IsValid())
					{
						Field.Options.Add(Option);
					}
				}
			}
		}
		Field.Kind = ScenarioJson::GetString(P, TEXT("kind"));
		const TSharedPtr<FJsonValue> MaxLength = FieldOf(P, { TEXT("max_length"), TEXT("maxLength"), TEXT("maxItems") });
		const int32 MaxValue = MaxLength.IsValid() && MaxLength->Type == EJson::Number ? static_cast<int32>(MaxLength->AsNumber()) : 0;
		Field.bCostImpact = ScenarioJson::Truthy(FieldOf(P, { TEXT("cost_impact"), TEXT("costImpact") }));
		Field.bPromptSpark = ScenarioJson::Truthy(FieldOf(P, { TEXT("prompt_spark"), TEXT("promptSpark") }));
		const bool bPromptFlag = ScenarioJson::Truthy(P->TryGetField(TEXT("prompt")));
		const bool bArray = RawType.EndsWith(TEXT("array")) || ScenarioJson::GetBool(P, TEXT("array"), false);

		if (RawType == TEXT("string"))
		{
			Field.MaxLength = MaxValue;
			if (bPromptFlag || Field.Name == TEXT("prompt"))
			{
				Field.Type = EScenarioFieldType::Prompt;
			}
			else if (Field.Options.Num() > 0)
			{
				Field.Type = EScenarioFieldType::Enum;
			}
			else
			{
				Field.Type = EScenarioFieldType::Text;
			}
		}
		else if (RawType == TEXT("number") || RawType == TEXT("integer"))
		{
			if (Field.Options.Num() > 0)
			{
				Field.Type = EScenarioFieldType::Enum;
			}
			else if (RawType == TEXT("integer") || LooksInteger(Field.Name, Field.Step))
			{
				Field.Type = EScenarioFieldType::Integer;
			}
			else
			{
				Field.Type = EScenarioFieldType::Number;
			}
		}
		else if (RawType == TEXT("boolean"))
		{
			Field.Type = EScenarioFieldType::Boolean;
		}
		else if (RawType == TEXT("string_array"))
		{
			Field.Type = EScenarioFieldType::Multi;
			Field.MaxItems = MaxValue;
		}
		else if (RawType == TEXT("file") || RawType == TEXT("file_array") || RawType == TEXT("image") || RawType == TEXT("asset"))
		{
			Field.Type = bArray ? EScenarioFieldType::Files : EScenarioFieldType::File;
			Field.MaxItems = bArray ? MaxValue : 1;
		}
		else
		{
			Field.Type = EScenarioFieldType::Text;
		}
		return Field;
	}

	TSharedPtr<FJsonValue> CoerceNumber(const TSharedPtr<FJsonValue>& Value, bool bInteger)
	{
		double Number = 0.0;
		if (Value->Type == EJson::Number)
		{
			Number = Value->AsNumber();
		}
		else if (Value->Type == EJson::String && Value->AsString().IsNumeric())
		{
			Number = FCString::Atod(*Value->AsString());
		}
		else
		{
			return nullptr;
		}
		return MakeShared<FJsonValueNumber>(bInteger ? FMath::RoundToDouble(Number) : Number);
	}
}

bool FScenarioField::IsPrimary() const
{
	return Type == EScenarioFieldType::Prompt || bRequired || IsFile() || bCostImpact;
}

TArray<FScenarioField> ScenarioSchema::FieldsFromParameters(const TArray<TSharedPtr<FJsonValue>>& Parameters, const TMap<FString, TArray<FString>>& Restrict)
{
	TArray<FScenarioField> Fields;
	for (const TSharedPtr<FJsonValue>& Value : Parameters)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object || ScenarioJson::GetString(Value->AsObject(), TEXT("name")).IsEmpty())
		{
			continue;
		}
		FScenarioField Field = ParseField(Value->AsObject());
		if (const TArray<FString>* Allowed = Restrict.Find(Field.Name))
		{
			if (Field.Type == EScenarioFieldType::Enum || Field.Type == EScenarioFieldType::Multi)
			{
				Field.Options = Field.Options.FilterByPredicate([Allowed](const TSharedPtr<FJsonValue>& Option)
				{
					return Option.IsValid() && Option->Type == EJson::String && Allowed->Contains(Option->AsString());
				});
				const bool bDefaultAllowed = Field.Default.IsValid() && Field.Default->Type == EJson::String && Allowed->Contains(Field.Default->AsString());
				if (!bDefaultAllowed && Field.Type == EScenarioFieldType::Enum && Field.Options.Num() > 0)
				{
					// The restrict list is in order of preference (wav before mp3).
					for (const FString& Preferred : *Allowed)
					{
						if (InOptions(MakeShared<FJsonValueString>(Preferred), Field.Options))
						{
							Field.Default = MakeShared<FJsonValueString>(Preferred);
							break;
						}
					}
				}
			}
		}
		Fields.Add(MoveTemp(Field));
	}
	// The prompt goes first, then required fields, then the rest in schema order.
	TArray<FScenarioField> Ordered;
	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		for (const FScenarioField& Field : Fields)
		{
			const int32 Rank = Field.Type == EScenarioFieldType::Prompt ? 0 : (Field.bRequired ? 1 : 2);
			if (Rank == Pass)
			{
				Ordered.Add(Field);
			}
		}
	}
	return Ordered;
}

TArray<FScenarioField> ScenarioSchema::FieldsFromSchema(const TSharedPtr<FJsonObject>& Schema, const TMap<FString, TArray<FString>>& Restrict)
{
	if (!Schema.IsValid())
	{
		return {};
	}
	for (const TCHAR* Key : { TEXT("parameters"), TEXT("inputs_definition"), TEXT("inputs") })
	{
		const TArray<TSharedPtr<FJsonValue>> Parameters = ScenarioJson::GetArray(Schema, Key);
		if (Parameters.Num() > 0)
		{
			return FieldsFromParameters(Parameters, Restrict);
		}
	}
	// workflow_get wraps the record.
	if (const TSharedPtr<FJsonObject> Workflow = ScenarioJson::GetObject(Schema, TEXT("workflow")))
	{
		return FieldsFromSchema(Workflow, Restrict);
	}
	return {};
}

FString ScenarioSchema::PromptName(const TArray<FScenarioField>& Fields)
{
	for (const FScenarioField& Field : Fields)
	{
		if (Field.Type == EScenarioFieldType::Prompt)
		{
			return Field.Name;
		}
	}
	return FString();
}

const FScenarioField* ScenarioSchema::Find(const TArray<FScenarioField>& Fields, const FString& Name)
{
	return Fields.FindByPredicate([&Name](const FScenarioField& Field) { return Field.Name == Name; });
}

FString ScenarioSchema::Humanize(const FString& Name)
{
	TArray<FString> Words;
	FString Current;
	for (int32 Index = 0; Index < Name.Len(); ++Index)
	{
		const TCHAR Char = Name[Index];
		if (Char == TEXT('_') || Char == TEXT('-'))
		{
			if (!Current.IsEmpty())
			{
				Words.Add(Current);
			}
			Current.Reset();
		}
		else if (FChar::IsUpper(Char) && !Current.IsEmpty() && FChar::IsLower(Current[Current.Len() - 1]))
		{
			Words.Add(Current);
			Current = FString(1, &Char);
		}
		else
		{
			Current.AppendChar(Char);
		}
	}
	if (!Current.IsEmpty())
	{
		Words.Add(Current);
	}
	FString Text = FString::Join(Words, TEXT(" ")).ToLower();
	if (Text.IsEmpty())
	{
		return Name;
	}
	return Text.Left(1).ToUpper() + Text.Mid(1);
}

FString ScenarioSchema::OptionText(const FScenarioField& Field, const TSharedPtr<FJsonValue>& Value)
{
	const FString Raw = ScenarioJson::ValueText(Value);
	if (const FString* Label = Field.OptionLabels.Find(Raw))
	{
		return *Label;
	}
	if (Raw.IsEmpty())
	{
		return TEXT("Default");
	}
	bool bLetters = true;
	bool bUpper = true;
	bool bDigit = false;
	for (const TCHAR Char : Raw)
	{
		bLetters &= FChar::IsAlpha(Char) || Char == TEXT('_') || Char == TEXT('-') || Char == TEXT(' ');
		bUpper &= !FChar::IsLower(Char);
		bDigit |= FChar::IsDigit(Char);
	}
	if (bDigit || !bLetters)
	{
		return Raw;
	}
	if (bUpper)
	{
		return Raw.Left(1) + Raw.Mid(1).ToLower();
	}
	if (FChar::IsLower(Raw[0]))
	{
		const FString Spaced = Raw.Replace(TEXT("_"), TEXT(" "));
		return Spaced.Left(1).ToUpper() + Spaced.Mid(1);
	}
	return Raw;
}

TSharedRef<FJsonObject> ScenarioSchema::InitialValues(const TArray<FScenarioField>& Fields, const TSharedPtr<FJsonObject>& Presets)
{
	TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
	for (const FScenarioField& Field : Fields)
	{
		if (Presets.IsValid() && Presets->HasField(Field.Name))
		{
			Values->SetField(Field.Name, ScenarioJson::Clone(Presets->TryGetField(Field.Name)));
		}
		else if (Field.Default.IsValid())
		{
			Values->SetField(Field.Name, ScenarioJson::Clone(Field.Default));
		}
	}
	return Values;
}

TArray<FString> ScenarioSchema::Validate(const TArray<FScenarioField>& Fields, const TSharedPtr<FJsonObject>& Values)
{
	TArray<FString> Problems;
	for (const FScenarioField& Field : Fields)
	{
		const TSharedPtr<FJsonValue> Value = Values.IsValid() ? Values->TryGetField(Field.Name) : nullptr;
		if (ScenarioJson::IsEmptyValue(Value))
		{
			if (Field.bRequired)
			{
				Problems.Add(FString::Printf(TEXT("%s: required."), *Field.Label));
			}
			continue;
		}
		switch (Field.Type)
		{
		case EScenarioFieldType::Number:
		case EScenarioFieldType::Integer:
		{
			const TSharedPtr<FJsonValue> Number = CoerceNumber(Value, false);
			if (!Number.IsValid())
			{
				Problems.Add(FString::Printf(TEXT("%s must be a number."), *Field.Label));
			}
			else if (Field.Min.IsSet() && Number->AsNumber() < Field.Min.GetValue())
			{
				Problems.Add(FString::Printf(TEXT("%s must be at least %s."), *Field.Label, *ScenarioJson::NumberText(Field.Min.GetValue())));
			}
			else if (Field.Max.IsSet() && Number->AsNumber() > Field.Max.GetValue())
			{
				Problems.Add(FString::Printf(TEXT("%s must be at most %s."), *Field.Label, *ScenarioJson::NumberText(Field.Max.GetValue())));
			}
			break;
		}
		case EScenarioFieldType::Enum:
			if (Field.Options.Num() > 0 && !InOptions(Value, Field.Options))
			{
				Problems.Add(FString::Printf(TEXT("%s has an unsupported value."), *Field.Label));
			}
			break;
		case EScenarioFieldType::Prompt:
		case EScenarioFieldType::Text:
			if (Field.MaxLength > 0 && ScenarioJson::ValueText(Value).Len() > Field.MaxLength)
			{
				Problems.Add(FString::Printf(TEXT("%s is longer than %d characters."), *Field.Label, Field.MaxLength));
			}
			break;
		case EScenarioFieldType::Files:
		case EScenarioFieldType::Multi:
			if (Field.MaxItems > 0 && Value->Type == EJson::Array && Value->AsArray().Num() > Field.MaxItems)
			{
				Problems.Add(FString::Printf(TEXT("%s takes at most %d items."), *Field.Label, Field.MaxItems));
			}
			break;
		default:
			break;
		}
	}
	return Problems;
}

TSharedRef<FJsonObject> ScenarioSchema::Payload(const TArray<FScenarioField>& Fields, const TSharedPtr<FJsonObject>& Values)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	if (!Values.IsValid())
	{
		return Out;
	}
	for (const FScenarioField& Field : Fields)
	{
		const TSharedPtr<FJsonValue> Value = Values->TryGetField(Field.Name);
		if (ScenarioJson::IsEmptyValue(Value))
		{
			continue;
		}
		switch (Field.Type)
		{
		case EScenarioFieldType::Integer:
		case EScenarioFieldType::Number:
			if (const TSharedPtr<FJsonValue> Number = CoerceNumber(Value, Field.Type == EScenarioFieldType::Integer))
			{
				Out->SetField(Field.Name, Number);
			}
			break;
		case EScenarioFieldType::Boolean:
			Out->SetBoolField(Field.Name, ScenarioJson::Truthy(Value));
			break;
		case EScenarioFieldType::Files:
		case EScenarioFieldType::Multi:
			if (Value->Type == EJson::Array)
			{
				Out->SetField(Field.Name, ScenarioJson::Clone(Value));
			}
			else
			{
				// A bare value is ignored by the API: always send an array.
				TArray<TSharedPtr<FJsonValue>> One;
				One.Add(ScenarioJson::Clone(Value));
				Out->SetArrayField(Field.Name, One);
			}
			break;
		case EScenarioFieldType::Prompt:
		case EScenarioFieldType::Text:
			Out->SetStringField(Field.Name, ScenarioJson::ValueText(Value).TrimStartAndEnd());
			break;
		default:
			Out->SetField(Field.Name, ScenarioJson::Clone(Value));
			break;
		}
	}
	return Out;
}

TArray<FString> ScenarioSchema::InputAssetIds(const TArray<FScenarioField>& Fields, const TSharedPtr<FJsonObject>& InPayload)
{
	TArray<FString> Ids;
	if (!InPayload.IsValid())
	{
		return Ids;
	}
	for (const FScenarioField& Field : Fields)
	{
		if (!Field.IsFile())
		{
			continue;
		}
		const TSharedPtr<FJsonValue> Value = InPayload->TryGetField(Field.Name);
		if (!Value.IsValid())
		{
			continue;
		}
		if (Value->Type == EJson::String)
		{
			Ids.Add(Value->AsString());
		}
		else if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				if (Item.IsValid() && Item->Type == EJson::String)
				{
					Ids.Add(Item->AsString());
				}
			}
		}
	}
	return Ids;
}

FString ScenarioSchema::TypeName(EScenarioFieldType Type)
{
	switch (Type)
	{
	case EScenarioFieldType::Prompt: return TEXT("prompt");
	case EScenarioFieldType::Text: return TEXT("text");
	case EScenarioFieldType::Enum: return TEXT("enum");
	case EScenarioFieldType::Number: return TEXT("number");
	case EScenarioFieldType::Integer: return TEXT("integer");
	case EScenarioFieldType::Boolean: return TEXT("boolean");
	case EScenarioFieldType::Multi: return TEXT("multi");
	case EScenarioFieldType::File: return TEXT("file");
	case EScenarioFieldType::Files: return TEXT("files");
	}
	return TEXT("text");
}
