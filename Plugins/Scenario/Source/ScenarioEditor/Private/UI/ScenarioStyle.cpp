// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioStyle.h"

#include "Interfaces/IPluginManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyleMacros.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "ScenarioWidgets.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"

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
	// Status chips and dots: tinted per state where they are used.
	Style.Set("Scenario.Chip", new FSlateRoundedBoxBrush(FLinearColor::White, 9.0f));
	Style.Set("Scenario.Dot", new FSlateRoundedBoxBrush(FLinearColor::White, 4.0f, FVector2D(8.0f, 8.0f)));
	Style.Set("Scenario.Tile", new FSlateRoundedBoxBrush(FLinearColor::White, 8.0f));

	// The 24 px stroke icons shared with Scenario's other engine plugins, white, tinted where used.
	TArray<FString> Icons;
	IFileManager::Get().FindFiles(Icons, *(Style.GetContentRootDir() / TEXT("Icons") / TEXT("*.svg")), true, false);
	for (const FString& File : Icons)
	{
		const FString Name = FPaths::GetBaseFilename(File);
		Style.Set(*(TEXT("Scenario.Icon.") + Name), new FSlateVectorImageBrush(Style.RootToContentDir(TEXT("Icons") / Name, TEXT(".svg")), FVector2D(16.0f, 16.0f)));
	}

	using namespace ScenarioUI::Palette;
	const auto Round = [](const FLinearColor& Fill, float Radius, const FLinearColor& Outline = FLinearColor::Transparent)
	{
		return FSlateRoundedBoxBrush(Fill, Radius, Outline, Outline.A > 0.0f ? 1.0f : 0.0f);
	};

	// Surfaces.
	Style.Set("Scenario.Panel", new FSlateColorBrush(Background()));
	Style.Set("Scenario.Rail", new FSlateColorBrush(RailBackground()));
	Style.Set("Scenario.Line", new FSlateColorBrush(Border()));
	Style.Set("Scenario.Card", new FSlateRoundedBoxBrush(Round(Surface(), 10.0f, Border())));
	Style.Set("Scenario.Card.Hover", new FSlateRoundedBoxBrush(Round(Hover(), 10.0f, BorderStrong())));
	Style.Set("Scenario.Card.Selected", new FSlateRoundedBoxBrush(Round(Selected(), 10.0f, Primary().CopyWithNewOpacity(0.55f))));
	Style.Set("Scenario.Slot", new FSlateRoundedBoxBrush(Round(Surface(), 8.0f, BorderStrong())));
	Style.Set("Scenario.Thumb", new FSlateRoundedBoxBrush(Round(Surface2(), 8.0f)));
	Style.Set("Scenario.Kbd", new FSlateRoundedBoxBrush(Round(Surface2(), 4.0f, Border())));
	Style.Set("Scenario.Usage", new FSlateRoundedBoxBrush(Round(Surface2(), 12.0f, Border())));
	Style.Set("Scenario.Badge", new FSlateRoundedBoxBrush(Info(), 7.0f));
	Style.Set("Scenario.Underline", new FSlateRoundedBoxBrush(Text(), 1.0f));
	Style.Set("Scenario.Overlay", new FSlateRoundedBoxBrush(Round(FLinearColor(0.0f, 0.0f, 0.0f, 0.72f), 6.0f)));

	// Buttons: Primary is the white call to action, one per view.
	const auto Button = [&Round](const FLinearColor& Fill, const FLinearColor& HoverFill, const FLinearColor& PressFill, const FLinearColor& Outline,
		const FLinearColor& Fore, float Radius, const FMargin& Padding)
	{
		const FLinearColor Disabled = Fill.A > 0.5f ? Fill.CopyWithNewOpacity(0.28f) : Fill;
		return FButtonStyle()
			.SetNormal(Round(Fill, Radius, Outline))
			.SetHovered(Round(HoverFill, Radius, Outline))
			.SetPressed(Round(PressFill, Radius, Outline))
			.SetDisabled(Round(Disabled, Radius, Outline.CopyWithNewOpacity(Outline.A * 0.5f)))
			.SetNormalForeground(Fore)
			.SetHoveredForeground(Fore)
			.SetPressedForeground(Fore)
			.SetDisabledForeground(Fore.CopyWithNewOpacity(0.45f))
			.SetNormalPadding(Padding)
			.SetPressedPadding(Padding);
	};
	const FLinearColor None = FLinearColor::Transparent;
	Style.Set("Scenario.Button.Primary", Button(Primary(), FLinearColor::White, PrimaryPressed(), None, OnPrimary(), 8.0f, FMargin(16.0f, 8.0f)));
	Style.Set("Scenario.Button.Primary.Small", Button(Primary(), FLinearColor::White, PrimaryPressed(), None, OnPrimary(), 6.0f, FMargin(10.0f, 4.0f)));
	Style.Set("Scenario.Button", Button(Surface2(), HoverStrong(), Selected(), Border(), Text(), 6.0f, FMargin(10.0f, 5.0f)));
	Style.Set("Scenario.Button.Small", Button(Surface2(), HoverStrong(), Selected(), Border(), Text(), 6.0f, FMargin(8.0f, 3.0f)));
	Style.Set("Scenario.Button.Outline", Button(None, Hover(), HoverStrong(), BorderStrong(), Text(), 6.0f, FMargin(10.0f, 5.0f)));
	Style.Set("Scenario.Button.Outline.Small", Button(None, Hover(), HoverStrong(), BorderStrong(), Text(), 6.0f, FMargin(8.0f, 3.0f)));
	Style.Set("Scenario.Button.Ghost", Button(None, Hover(), HoverStrong(), None, Text(), 6.0f, FMargin(6.0f, 4.0f)));
	Style.Set("Scenario.Button.Ghost.Small", Button(None, Hover(), HoverStrong(), None, Text(), 6.0f, FMargin(4.0f, 2.0f)));
	Style.Set("Scenario.Button.Row", Button(None, Hover(), HoverStrong(), None, Text(), 8.0f, FMargin(0.0f)));

	// Toggles: chips (lanes, filters), rail items and underline tabs.
	const auto Toggle = [](const FSlateBrush& Off, const FSlateBrush& OffHover, const FSlateBrush& On, const FSlateBrush& OnHover,
		const FLinearColor& OffFore, const FLinearColor& OffHoverFore, const FLinearColor& OnFore, const FMargin& Padding)
	{
		return FCheckBoxStyle()
			.SetCheckBoxType(ESlateCheckBoxType::ToggleButton)
			.SetUncheckedImage(Off)
			.SetUncheckedHoveredImage(OffHover)
			.SetUncheckedPressedImage(OffHover)
			.SetCheckedImage(On)
			.SetCheckedHoveredImage(OnHover)
			.SetCheckedPressedImage(On)
			.SetUndeterminedImage(Off)
			.SetUndeterminedHoveredImage(OffHover)
			.SetUndeterminedPressedImage(OffHover)
			.SetForegroundColor(OffFore)
			.SetHoveredForegroundColor(OffHoverFore)
			.SetPressedForegroundColor(OffHoverFore)
			.SetCheckedForegroundColor(OnFore)
			.SetCheckedHoveredForegroundColor(OnFore)
			.SetCheckedPressedForegroundColor(OnFore)
			.SetPadding(Padding);
	};
	const FSlateNoResource NoBrush;
	Style.Set("Scenario.Chip.Toggle", Toggle(Round(None, 14.0f, BorderStrong()), Round(Hover(), 14.0f, BorderStrong()), Round(Primary(), 14.0f), Round(FLinearColor::White, 14.0f),
		Text(), Text(), OnPrimary(), FMargin(10.0f, 5.0f)));
	Style.Set("Scenario.Chip.Toggle.Small", Toggle(Round(None, 12.0f, BorderStrong()), Round(Hover(), 12.0f, BorderStrong()), Round(Primary(), 12.0f), Round(FLinearColor::White, 12.0f),
		Dim(), Text(), OnPrimary(), FMargin(8.0f, 3.0f)));
	Style.Set("Scenario.Rail.Item", Toggle(Round(None, 8.0f), Round(Hover(), 8.0f), Round(Selected(), 8.0f), Round(HoverStrong(), 8.0f),
		Dim(), Text(), Text(), FMargin(10.0f, 0.0f)));
	Style.Set("Scenario.Tab", Toggle(NoBrush, NoBrush, NoBrush, NoBrush, Dim(), Text(), Text(), FMargin(0.0f)));
	Style.Set("Scenario.Switch", Toggle(NoBrush, NoBrush, NoBrush, NoBrush, Text(), Text(), Text(), FMargin(0.0f)));
	Style.Set("Scenario.Switch.Track", new FSlateRoundedBoxBrush(FLinearColor::White, 8.0f));
	Style.Set("Scenario.Switch.Knob", new FSlateRoundedBoxBrush(FLinearColor::White, 6.0f, FVector2D(12.0f, 12.0f)));

	// Fields: text boxes, the prompt, search and drop-downs.
	const auto TextBox = [&Round](float Radius, const FMargin& Padding)
	{
		FEditableTextBoxStyle Box = FAppStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
		Box.SetBackgroundImageNormal(Round(Surface(), Radius, Border()))
			.SetBackgroundImageHovered(Round(Surface(), Radius, BorderStrong()))
			.SetBackgroundImageFocused(Round(Surface(), Radius, Focus()))
			.SetBackgroundImageReadOnly(Round(Surface(), Radius, Border()))
			.SetForegroundColor(Text())
			.SetFocusedForegroundColor(Text())
			.SetPadding(Padding);
		return Box;
	};
	Style.Set("Scenario.Input", TextBox(6.0f, FMargin(8.0f, 5.0f)));
	Style.Set("Scenario.Prompt", TextBox(8.0f, FMargin(10.0f, 8.0f)));
	FSearchBoxStyle Search = FAppStyle::Get().GetWidgetStyle<FSearchBoxStyle>("SearchBox");
	Search.SetTextBoxStyle(TextBox(8.0f, FMargin(10.0f, 7.0f)));
	Style.Set("Scenario.Search", Search);
	FComboButtonStyle Combo = FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>("ComboButton");
	Combo.SetButtonStyle(Button(Surface(), Surface(), Surface(), Border(), Text(), 6.0f, FMargin(8.0f, 4.0f))
		.SetHovered(Round(Surface(), 6.0f, BorderStrong())));
	Style.Set("Scenario.Combo", Combo);

	// List rows: rounded hover and selection, no blue.
	FTableRowStyle Row = FAppStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.Row");
	const FSlateRoundedBoxBrush RowHover = Round(Hover(), 8.0f);
	const FSlateRoundedBoxBrush RowSelected = Round(Selected(), 8.0f, BorderStrong());
	const FSlateNoResource NoRow;
	Row.SetEvenRowBackgroundBrush(NoRow).SetOddRowBackgroundBrush(NoRow)
		.SetEvenRowBackgroundHoveredBrush(RowHover).SetOddRowBackgroundHoveredBrush(RowHover)
		.SetActiveBrush(RowSelected).SetActiveHoveredBrush(RowSelected)
		.SetInactiveBrush(RowSelected).SetInactiveHoveredBrush(RowSelected)
		.SetActiveHighlightedBrush(RowSelected).SetInactiveHighlightedBrush(RowSelected)
		.SetSelectorFocusedBrush(NoRow)
		.SetTextColor(Text()).SetSelectedTextColor(Text());
	Style.Set("Scenario.TableRow", Row);
	FProgressBarStyle Progress = FAppStyle::Get().GetWidgetStyle<FProgressBarStyle>("ProgressBar");
	Progress.SetBackgroundImage(Round(Surface2(), 1.5f)).SetFillImage(Round(Text(), 1.5f)).SetMarqueeImage(Round(Dim(), 1.5f)).SetEnableFillAnimation(false);
	Style.Set("Scenario.Progress", Progress);
	FSplitterStyle Splitter = FAppStyle::Get().GetWidgetStyle<FSplitterStyle>("Splitter");
	Splitter.SetHandleNormalBrush(FSlateNoResource()).SetHandleHighlightBrush(FSlateColorBrush(Border()));
	Style.Set("Scenario.Splitter", Splitter);
	Style.Set("Scenario.TableRow.Header", FTableRowStyle(Row).SetEvenRowBackgroundHoveredBrush(NoRow).SetOddRowBackgroundHoveredBrush(NoRow));
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
	return ScenarioUI::Palette::Dim();
}

FSlateColor FScenarioStyle::Warning()
{
	return ScenarioUI::Palette::Warning();
}

FSlateColor FScenarioStyle::Error()
{
	return ScenarioUI::Palette::Error();
}

FSlateColor FScenarioStyle::Success()
{
	return ScenarioUI::Palette::Success();
}

FSlateColor FScenarioStyle::Accent()
{
	return ScenarioUI::Palette::Info();
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
