// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioJson.h"

#include "Policies/CondensedJsonPrintPolicy.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

TSharedPtr<FJsonObject> ScenarioJson::ParseObject(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return nullptr;
	}
	return Object;
}

TSharedPtr<FJsonValue> ScenarioJson::ParseValue(const FString& Text)
{
	TSharedPtr<FJsonValue> Value;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Value) || !Value.IsValid())
	{
		return nullptr;
	}
	return Value;
}

FString ScenarioJson::Write(const TSharedPtr<FJsonObject>& Object, bool bPretty)
{
	FString Out;
	if (!Object.IsValid())
	{
		return TEXT("{}");
	}
	if (bPretty)
	{
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	else
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Out;
}

FString ScenarioJson::WriteValue(const TSharedPtr<FJsonValue>& Value, bool bPretty)
{
	if (!Value.IsValid())
	{
		return TEXT("null");
	}
	if (Value->Type == EJson::Object)
	{
		return Write(Value->AsObject(), bPretty);
	}
	// The serializer needs an object or array at the root; canonical form is fine for scalars and arrays.
	return Canonical(Value);
}

FString ScenarioJson::EscapeString(const FString& Text)
{
	FString Out;
	Out.Reserve(Text.Len() + 2);
	Out += TEXT('"');
	for (const TCHAR Char : Text)
	{
		switch (Char)
		{
		case TEXT('"'): Out += TEXT("\\\""); break;
		case TEXT('\\'): Out += TEXT("\\\\"); break;
		case TEXT('\n'): Out += TEXT("\\n"); break;
		case TEXT('\r'): Out += TEXT("\\r"); break;
		case TEXT('\t'): Out += TEXT("\\t"); break;
		case TEXT('\b'): Out += TEXT("\\b"); break;
		case TEXT('\f'): Out += TEXT("\\f"); break;
		default:
			if (Char < 0x20)
			{
				Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Char));
			}
			else
			{
				Out += Char;
			}
			break;
		}
	}
	Out += TEXT('"');
	return Out;
}

FString ScenarioJson::NumberText(double Value)
{
	if (!FMath::IsFinite(Value))
	{
		return TEXT("0");
	}
	if (FMath::Abs(Value) < 9.0e15 && Value == FMath::FloorToDouble(Value))
	{
		return FString::Printf(TEXT("%lld"), static_cast<long long>(Value));
	}
	// Shortest decimal that reads back to the same double.
	for (int32 Precision = 1; Precision <= 17; ++Precision)
	{
		const FString Candidate = FString::Printf(TEXT("%.*g"), Precision, Value);
		if (FCString::Atod(*Candidate) == Value)
		{
			return Candidate;
		}
	}
	return FString::Printf(TEXT("%.17g"), Value);
}

FString ScenarioJson::Canonical(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return TEXT("null");
	}
	switch (Value->Type)
	{
	case EJson::Null:
	case EJson::None:
		return TEXT("null");
	case EJson::Boolean:
		return Value->AsBool() ? TEXT("true") : TEXT("false");
	case EJson::Number:
		return NumberText(Value->AsNumber());
	case EJson::String:
		return EscapeString(Value->AsString());
	case EJson::Array:
	{
		FString Out = TEXT("[");
		const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			if (Index > 0)
			{
				Out += TEXT(",");
			}
			Out += Canonical(Items[Index]);
		}
		return Out + TEXT("]");
	}
	case EJson::Object:
		return Canonical(Value->AsObject());
	}
	return TEXT("null");
}

FString ScenarioJson::Canonical(const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid())
	{
		return TEXT("null");
	}
	TArray<FString> Keys;
	Object->Values.GetKeys(Keys);
	Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	FString Out = TEXT("{");
	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		if (Index > 0)
		{
			Out += TEXT(",");
		}
		Out += EscapeString(Keys[Index]);
		Out += TEXT(":");
		Out += Canonical(Object->Values[Keys[Index]]);
	}
	return Out + TEXT("}");
}

TSharedPtr<FJsonValue> ScenarioJson::Clone(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return MakeShared<FJsonValueNull>();
	}
	switch (Value->Type)
	{
	case EJson::Boolean: return MakeShared<FJsonValueBoolean>(Value->AsBool());
	case EJson::Number: return MakeShared<FJsonValueNumber>(Value->AsNumber());
	case EJson::String: return MakeShared<FJsonValueString>(Value->AsString());
	case EJson::Array:
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			Items.Add(Clone(Item));
		}
		return MakeShared<FJsonValueArray>(Items);
	}
	case EJson::Object: return MakeShared<FJsonValueObject>(CloneObject(Value->AsObject()));
	default: return MakeShared<FJsonValueNull>();
	}
}

TSharedPtr<FJsonObject> ScenarioJson::CloneObject(const TSharedPtr<FJsonObject>& Object)
{
	TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
	if (Object.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			Copy->SetField(Pair.Key, Clone(Pair.Value));
		}
	}
	return Copy;
}

FString ScenarioJson::GetString(const TSharedPtr<FJsonObject>& Object, const FString& Field, const FString& Default)
{
	if (!Object.IsValid())
	{
		return Default;
	}
	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	if (!Value.IsValid() || Value->IsNull())
	{
		return Default;
	}
	if (Value->Type == EJson::String)
	{
		return Value->AsString();
	}
	if (Value->Type == EJson::Number)
	{
		return NumberText(Value->AsNumber());
	}
	if (Value->Type == EJson::Boolean)
	{
		return Value->AsBool() ? TEXT("true") : TEXT("false");
	}
	return Default;
}

double ScenarioJson::GetNumber(const TSharedPtr<FJsonObject>& Object, const FString& Field, double Default)
{
	if (!Object.IsValid())
	{
		return Default;
	}
	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	if (!Value.IsValid())
	{
		return Default;
	}
	if (Value->Type == EJson::Number)
	{
		return Value->AsNumber();
	}
	if (Value->Type == EJson::String)
	{
		const FString Text = Value->AsString().TrimStartAndEnd();
		if (Text.IsNumeric())
		{
			return FCString::Atod(*Text);
		}
	}
	return Default;
}

bool ScenarioJson::HasNumber(const TSharedPtr<FJsonObject>& Object, const FString& Field)
{
	if (!Object.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	return Value.IsValid() && (Value->Type == EJson::Number || (Value->Type == EJson::String && Value->AsString().TrimStartAndEnd().IsNumeric()));
}

bool ScenarioJson::GetBool(const TSharedPtr<FJsonObject>& Object, const FString& Field, bool Default)
{
	if (!Object.IsValid())
	{
		return Default;
	}
	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	return Value.IsValid() ? Truthy(Value) : Default;
}

TSharedPtr<FJsonObject> ScenarioJson::GetObject(const TSharedPtr<FJsonObject>& Object, const FString& Field)
{
	if (!Object.IsValid())
	{
		return nullptr;
	}
	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	if (Value.IsValid() && Value->Type == EJson::Object)
	{
		return Value->AsObject();
	}
	return nullptr;
}

TArray<TSharedPtr<FJsonValue>> ScenarioJson::GetArray(const TSharedPtr<FJsonObject>& Object, const FString& Field)
{
	if (!Object.IsValid())
	{
		return {};
	}
	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	if (Value.IsValid() && Value->Type == EJson::Array)
	{
		return Value->AsArray();
	}
	return {};
}

TArray<FString> ScenarioJson::GetStringArray(const TSharedPtr<FJsonObject>& Object, const FString& Field)
{
	TArray<FString> Out;
	for (const TSharedPtr<FJsonValue>& Item : GetArray(Object, Field))
	{
		if (Item.IsValid() && (Item->Type == EJson::String || Item->Type == EJson::Number))
		{
			Out.Add(ValueText(Item));
		}
	}
	return Out;
}

FString ScenarioJson::ValueText(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid() || Value->IsNull())
	{
		return FString();
	}
	switch (Value->Type)
	{
	case EJson::String: return Value->AsString();
	case EJson::Number: return NumberText(Value->AsNumber());
	case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
	default: return Canonical(Value);
	}
}

bool ScenarioJson::Truthy(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return false;
	}
	switch (Value->Type)
	{
	case EJson::Boolean: return Value->AsBool();
	case EJson::Number: return Value->AsNumber() != 0.0;
	case EJson::String:
	{
		const FString Lower = Value->AsString().TrimStartAndEnd().ToLower();
		return Lower == TEXT("true") || Lower == TEXT("1") || Lower == TEXT("yes");
	}
	case EJson::Object:
	{
		// Workflow inputs carry "required": {"always": true}.
		return Truthy(Value->AsObject()->TryGetField(TEXT("always")));
	}
	default: return false;
	}
}

bool ScenarioJson::IsEmptyValue(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid() || Value->IsNull() || Value->Type == EJson::None)
	{
		return true;
	}
	switch (Value->Type)
	{
	case EJson::String: return Value->AsString().TrimStartAndEnd().IsEmpty();
	case EJson::Array: return Value->AsArray().Num() == 0;
	case EJson::Object: return Value->AsObject()->Values.Num() == 0;
	default: return false;
	}
}

TSharedRef<FJsonValue> ScenarioJson::StringArrayValue(const TArray<FString>& Items)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& Item : Items)
	{
		Values.Add(MakeShared<FJsonValueString>(Item));
	}
	return MakeShared<FJsonValueArray>(Values);
}
