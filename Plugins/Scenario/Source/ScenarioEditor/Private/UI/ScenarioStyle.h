// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Styling/SlateStyle.h"

/**
 * Scenario's style set: the black and white mark, the stroke icons, and the
 * cards, buttons, chips and fields shared with Scenario's other engine
 * plugins. Widgets reach it through ScenarioUI::Style().
 */
class FScenarioStyle
{
public:
	static void Register();
	static void Unregister();
	static const ISlateStyle& Get();
	static FName GetStyleSetName();

	/** Fonts and colors shared by the views. */
	static FSlateFontInfo Font(int32 Size, bool bBold = false);
	static FSlateColor Muted();
	static FSlateColor Warning();
	static FSlateColor Error();
	static FSlateColor Success();
	/** Blue, for live state such as running jobs. */
	static FSlateColor Accent();

private:
	static TSharedPtr<FSlateStyleSet> Instance;
};

class FScenarioCommands : public TCommands<FScenarioCommands>
{
public:
	FScenarioCommands();
	virtual void RegisterCommands() override;

	TSharedPtr<FUICommandInfo> OpenStudio;
	TSharedPtr<FUICommandInfo> ToggleComposer;
	TSharedPtr<FUICommandInfo> Generate;
	TSharedPtr<FUICommandInfo> OpenPicker;
	TSharedPtr<FUICommandInfo> ImportAndPlace;
};
