// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

// The kit tools: world, game and film recipes written in Python (Content/Python/scenario_unreal)
// and declared in Resources/Kit.json, so each tool's schema sits next to its description.

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "IPythonScriptPlugin.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScenarioActions.h"
#include "ScenarioCoreModule.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "ScenarioKit"

namespace
{
	const TCHAR* ResultMarker = TEXT("SCENARIO_KIT_RESULT ");

	FString KitFile()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scenario"));
		return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Kit.json")) : FString();
	}

	FScenarioActionResult RunRecipe(const FString& Name, const TSharedPtr<FJsonObject>& Args, bool bUndo)
	{
		IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
		if (!Python || !Python->IsPythonAvailable())
		{
			return FScenarioActionResult::Fail(TEXT("Unavailable"), TEXT("The kit tools need the Python Editor Script Plugin: Edit > Plugins, enable it, restart the editor."));
		}
		FString ArgsJson = TEXT("{}");
		if (Args.IsValid())
		{
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsJson);
			FJsonSerializer::Serialize(Args.ToSharedRef(), Writer);
		}
		// Base64 keeps quotes and newlines in the arguments out of the Python source.
		const FTCHARToUTF8 Utf8(*ArgsJson);
		const FString Encoded = FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

		FPythonCommandEx Command;
		Command.Command = FString::Printf(TEXT("import scenario_unreal.recipes as _kit\n_kit.run(\"%s\", \"%s\")\n"), *Name, *Encoded);
		Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		Command.FileExecutionScope = EPythonFileExecutionScope::Private;
		{
			TOptional<FScopedTransaction> Transaction;
			if (bUndo)
			{
				Transaction.Emplace(FText::Format(LOCTEXT("KitStep", "Scenario: {0}"), FText::FromString(Name)));
			}
			Python->ExecPythonCommandEx(Command);
		}

		FString ResultJson;
		FString Output;
		for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
		{
			TArray<FString> Lines;
			Entry.Output.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				if (Line.StartsWith(ResultMarker))
				{
					ResultJson = Line.Mid(FCString::Strlen(ResultMarker));
				}
				else if (Output.Len() < 4000)
				{
					Output += Line + TEXT("\n");
				}
			}
		}
		TSharedPtr<FJsonObject> Result;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
		if (ResultJson.IsEmpty() || !FJsonSerializer::Deserialize(Reader, Result) || !Result.IsValid())
		{
			const FString Tail = Output.IsEmpty() ? Command.CommandResult : Output;
			return FScenarioActionResult::Fail(TEXT("Kit"), FString::Printf(TEXT("%s gave no result. %s"), *Name, *Tail.Right(1500)));
		}
		FString Error;
		if (Result->TryGetStringField(TEXT("error"), Error))
		{
			FScenarioActionResult Failed = FScenarioActionResult::Fail(TEXT("Kit"), Error);
			Failed.Data = Result;
			return Failed;
		}
		if (!Output.IsEmpty())
		{
			Result->SetStringField(TEXT("log"), Output);
		}
		return FScenarioActionResult::Ok(Result);
	}
}

void FScenarioActions::RegisterKitActions()
{
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *KitFile()) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogScenario, Warning, TEXT("The kit tools are off: Resources/Kit.json is missing or not valid JSON."));
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
	if (!Root->TryGetArrayField(TEXT("tools"), Tools))
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Tools)
	{
		const TSharedPtr<FJsonObject> Tool = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Name;
		if (!Tool.IsValid() || !Tool->TryGetStringField(TEXT("name"), Name))
		{
			continue;
		}
		const FString Kind = Tool->GetStringField(TEXT("kind"));
		bool bUndo = false;
		Tool->TryGetBoolField(TEXT("undo"), bUndo);
		double TimeLimit = 120.0;
		Tool->TryGetNumberField(TEXT("time_limit"), TimeLimit);

		FScenarioAction Action;
		Action.Name = Name;
		Action.Description = Tool->GetStringField(TEXT("description"));
		const TSharedPtr<FJsonObject>* Schema = nullptr;
		Action.InputSchema = Tool->TryGetObjectField(TEXT("input_schema"), Schema) ? *Schema : MakeShared<FJsonObject>();
		Action.Kind = Kind == TEXT("read") ? EScenarioActionKind::Read : Kind == TEXT("editor") ? EScenarioActionKind::Editor : EScenarioActionKind::Change;
		Action.bDestructive = Action.Kind == EScenarioActionKind::Change;
		Action.TimeLimit = TimeLimit;
		Action.Run = [Name, bUndo](const TSharedPtr<FJsonObject>& Args, const FScenarioCaller&, FScenarioActionDone Done)
		{
			Done(RunRecipe(Name, Args, bUndo));
		};
		Register(MoveTemp(Action));
	}
}

#undef LOCTEXT_NAMESPACE
