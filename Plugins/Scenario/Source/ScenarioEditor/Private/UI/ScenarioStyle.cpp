// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioStyle.h"

#include "Interfaces/IPluginManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyleMacros.h"
#include "Brushes/SlateImageBrush.h"
#include "Styling/SlateStyleRegistry.h"

#define LOCTEXT_NAMESPACE "Scenario"

TSharedPtr<FSlateStyleSet> FScenarioStyle::Instance;

void FScenarioStyle::Register()
{
	if (Instance.IsValid())
	{
		return;
	}
	Instance = MakeShared<FSlateStyleSet>(GetStyleSetName());
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scenario"));
	Instance->SetContentRoot(Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Resources") : FString());

	FSlateStyleSet& Style = *Instance;
	Style.Set("Scenario.Mark", new FSlateVectorImageBrush(Style.RootToContentDir(TEXT("ScenarioMark"), TEXT(".svg")), FVector2D(20.0f, 20.0f)));
	Style.Set("Scenario.Mark.Small", new FSlateVectorImageBrush(Style.RootToContentDir(TEXT("ScenarioMark"), TEXT(".svg")), FVector2D(16.0f, 16.0f)));
	Style.Set("Scenario.Mark.Large", new FSlateVectorImageBrush(Style.RootToContentDir(TEXT("ScenarioMark"), TEXT(".svg")), FVector2D(48.0f, 48.0f)));
	Style.Set("Scenario.Studio", new FSlateVectorImageBrush(Style.RootToContentDir(TEXT("ScenarioMark"), TEXT(".svg")), FVector2D(16.0f, 16.0f)));
	FSlateStyleRegistry::RegisterSlateStyle(Style);
}

void FScenarioStyle::Unregister()
{
	if (Instance.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*Instance);
		Instance.Reset();
	}
}

const ISlateStyle& FScenarioStyle::Get()
{
	return *Instance;
}

FName FScenarioStyle::GetStyleSetName()
{
	static const FName Name(TEXT("ScenarioStyle"));
	return Name;
}

FSlateFontInfo FScenarioStyle::Font(int32 Size, bool bBold)
{
	return FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size);
}

FSlateColor FScenarioStyle::Muted()
{
	return FSlateColor::UseSubduedForeground();
}

FSlateColor FScenarioStyle::Warning()
{
	return FAppStyle::Get().GetSlateColor("Colors.Warning");
}

FSlateColor FScenarioStyle::Error()
{
	return FAppStyle::Get().GetSlateColor("Colors.Error");
}

FSlateColor FScenarioStyle::Success()
{
	return FAppStyle::Get().GetSlateColor("Colors.Success");
}

FScenarioCommands::FScenarioCommands()
	: TCommands<FScenarioCommands>(TEXT("Scenario"), LOCTEXT("Commands", "Scenario"), NAME_None, FScenarioStyle::GetStyleSetName())
{
}

void FScenarioCommands::RegisterCommands()
{
	UI_COMMAND(OpenStudio, "Scenario Studio", "Opens the Scenario Studio: every Scenario model, priced before you spend.", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Alt | EModifierKey::Shift, EKeys::S));
	UI_COMMAND(ToggleComposer, "Quick Composer", "Shows or hides the Scenario bar at the bottom of the level viewport.", EUserInterfaceActionType::ToggleButton, FInputChord(EModifierKey::Alt | EModifierKey::Shift, EKeys::SpaceBar));
	UI_COMMAND(Generate, "Generate", "Sends the priced request to Scenario. It spends the Creative Units shown on the button.", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Control, EKeys::Enter));
	UI_COMMAND(OpenPicker, "Choose model", "Opens the model picker.", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Control, EKeys::M));
	UI_COMMAND(ImportAndPlace, "Import and place", "Imports the newest result and places it in the level.", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Control, EKeys::I));
}

#undef LOCTEXT_NAMESPACE
