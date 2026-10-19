// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Misc/Attribute.h"
#include "Styling/SlateColor.h"
#include "Widgets/SWidget.h"

class ISlateStyle;
class SButton;
struct FSlateBrush;

/**
 * The Studio's small visual system, shared with Scenario's other engine
 * plugins: one palette, one type scale, a 4 px spacing grid, cards, buttons,
 * chips, pills and section labels. Every view builds on these.
 */
namespace ScenarioUI
{
	/** The palette of Scenario's engine plugins (dark editor theme). */
	namespace Palette
	{
		inline FLinearColor Hex(uint8 R, uint8 G, uint8 B, float Alpha = 1.0f) { return FLinearColor::FromSRGBColor(FColor(R, G, B)).CopyWithNewOpacity(Alpha); }
		inline FLinearColor White(float Alpha) { return FLinearColor(1.0f, 1.0f, 1.0f, Alpha); }

		inline FLinearColor Background() { return Hex(0x24, 0x24, 0x24); }
		inline FLinearColor RailBackground() { return Hex(0x1E, 0x1E, 0x1E); }
		inline FLinearColor Text() { return Hex(0xED, 0xED, 0xED); }
		inline FLinearColor Dim() { return Hex(0xED, 0xED, 0xED, 0.62f); }
		inline FLinearColor Faint() { return Hex(0xED, 0xED, 0xED, 0.40f); }
		inline FLinearColor Border() { return White(0.08f); }
		inline FLinearColor BorderStrong() { return White(0.16f); }
		inline FLinearColor Surface() { return FLinearColor(0.0f, 0.0f, 0.0f, 0.16f); }
		inline FLinearColor Surface2() { return White(0.05f); }
		inline FLinearColor Hover() { return White(0.06f); }
		inline FLinearColor HoverStrong() { return White(0.10f); }
		inline FLinearColor Selected() { return White(0.11f); }
		inline FLinearColor Focus() { return White(0.36f); }
		inline FLinearColor Primary() { return Hex(0xF2, 0xF2, 0xF2); }
		inline FLinearColor PrimaryPressed() { return Hex(0xD6, 0xD6, 0xD6); }
		inline FLinearColor OnPrimary() { return Hex(0x11, 0x11, 0x11); }
		inline FLinearColor Success() { return Hex(0x3F, 0xB9, 0x50); }
		inline FLinearColor Warning() { return Hex(0xD2, 0x99, 0x22); }
		inline FLinearColor Error() { return Hex(0xF8, 0x51, 0x49); }
		inline FLinearColor Info() { return Hex(0x58, 0xA6, 0xFF); }
	}

	/** The plugin's style set: the mark, icons, cards, buttons, chips and fields. */
	SCENARIOEDITOR_API const ISlateStyle& Style();

	enum class EText : uint8
	{
		/** Sign-in tagline only. */
		Display,
		/** Page titles (Jobs, Library, Settings). */
		Heading,
		/** Model or workflow name, the selected asset, the Generate label. */
		Title,
		/** Section labels (PROMPT, INPUTS, SETTINGS): small capitals, faint. */
		Section,
		/** Labels, values, list titles, prompt text. */
		Body,
		/** Notes, hints, costs, times. Use with a subdued color. */
		Meta,
		/** Status chips and badges. */
		Chip,
	};
	SCENARIOEDITOR_API FSlateFontInfo Font(EText Kind);

	constexpr float Gutter = 8.0f;
	constexpr float RowGap = 4.0f;
	constexpr float SectionGap = 12.0f;
	constexpr float LabelWidth = 140.0f;
	/** The padding around a page's content. */
	constexpr float PagePadding = 14.0f;

	/** A stroke icon from Resources/Icons by name ("create", "search", "chevron-down"). */
	SCENARIOEDITOR_API const FSlateBrush* IconBrush(FName Name);
	/** An icon at Size px, tinted by Color (the foreground by default). */
	SCENARIOEDITOR_API TSharedRef<SWidget> Icon(FName Name, float Size = 16.0f, TAttribute<FSlateColor> Color = FSlateColor::UseForeground());
	/** The icon of an output kind or lane: image, 3d, material, sky, audio, video, workflow. */
	SCENARIOEDITOR_API FName KindIconName(const FString& Kind);

	enum class EButton : uint8
	{
		/** White, dark text: the one call to action of a view. */
		Primary,
		/** Filled grey. */
		Secondary,
		/** An outline. */
		Outline,
		/** Text or an icon alone; a background on hover. */
		Ghost,
	};
	SCENARIOEDITOR_API FName ButtonStyleName(EButton Kind, bool bSmall = false);
	/** A button with an optional leading icon. */
	SCENARIOEDITOR_API TSharedRef<SButton> Button(TAttribute<FText> Label, EButton Kind, TFunction<void()> OnClick, FName IconName = NAME_None,
		TAttribute<FText> Tooltip = TAttribute<FText>(), bool bSmall = false);

	/** A rounded toggle chip (lanes, filters, quick picks). The selected one is filled white. */
	SCENARIOEDITOR_API TSharedRef<SWidget> ChipToggle(TAttribute<FText> Label, TAttribute<bool> IsOn, TFunction<void()> OnClick, FName IconName = NAME_None,
		TAttribute<FText> Tooltip = TAttribute<FText>(), bool bSmall = false);

	enum class ETone : uint8 { Neutral, Info, Success, Warning, Error };
	SCENARIOEDITOR_API FLinearColor ToneColor(ETone Tone);
	/** A small status pill: Running, Done, Failed, Featured, New. */
	SCENARIOEDITOR_API TSharedRef<SWidget> Pill(TAttribute<FText> Label, TAttribute<ETone> Tone, TAttribute<FText> Tooltip = TAttribute<FText>());

	/** A rounded panel on the darker surface. */
	SCENARIOEDITOR_API TSharedRef<SWidget> Card(const TSharedRef<SWidget>& Content, FMargin Padding = FMargin(10.0f));
	/** A page title, a subtitle line and an optional widget on the right. */
	SCENARIOEDITOR_API TSharedRef<SWidget> PageHeader(const FText& Title, TAttribute<FText> Subtitle, const TSharedPtr<SWidget>& Right = nullptr);
	/** Text tabs with a line under the current one. */
	SCENARIOEDITOR_API TSharedRef<SWidget> UnderlineTabs(const TArray<TPair<FName, FText>>& Tabs, TAttribute<FName> Current, TFunction<void(FName)> OnChanged);
	/** An on and off switch. */
	SCENARIOEDITOR_API TSharedRef<SWidget> Switch(TAttribute<bool> IsOn, TFunction<void(bool)> OnChanged, TAttribute<FText> Tooltip = TAttribute<FText>());
	/** A 1 px line in the border color. */
	SCENARIOEDITOR_API TSharedRef<SWidget> Divider(bool bVertical = false);
	/** A setting's label over its hint ("Changes the price"). */
	SCENARIOEDITOR_API TSharedRef<SWidget> FieldLabel(const FText& Label, const FText& Hint = FText::GetEmpty(), const FText& Tooltip = FText::GetEmpty());
	/** A keyboard shortcut in a small box ("Ctrl+Enter"). */
	SCENARIOEDITOR_API TSharedRef<SWidget> Kbd(const FText& Keys);

	/** A pill tinted with any state color (Generating, Ready, Failed...). */
	SCENARIOEDITOR_API TSharedRef<SWidget> Chip(TAttribute<FText> Label, TAttribute<FSlateColor> Color, TAttribute<FText> Tooltip = TAttribute<FText>());

	/** An 8 px dot (agent status). */
	SCENARIOEDITOR_API TSharedRef<SWidget> Dot(TAttribute<FSlateColor> Color);

	/** The editor's class thumbnail for an output kind: image, 3d, material, sky, audio, video. Never text. */
	SCENARIOEDITOR_API const FSlateBrush* KindThumbnail(const FString& Kind);
	/** The 16 px class icon for an output kind. */
	SCENARIOEDITOR_API const FSlateBrush* KindIcon(const FString& Kind);

	/** A square thumbnail: the image when there is one, else the kind's class thumbnail, dimmed. */
	SCENARIOEDITOR_API TSharedRef<SWidget> Thumbnail(float Size, TAttribute<const FSlateBrush*> Image, TAttribute<FString> Kind);

	/** A section label in small faint capitals, with an optional widget on the right. */
	SCENARIOEDITOR_API TSharedRef<SWidget> SectionHeader(const FText& Title, const TSharedPtr<SWidget>& Right = nullptr);

	/** A borderless icon button. The tooltip starts with a verb. */
	SCENARIOEDITOR_API TSharedRef<SWidget> IconButton(FName Brush, const FText& Tooltip, FSimpleDelegate OnClick);

	/** A one-line label that ends with an ellipsis and shows the full text as its tooltip. */
	SCENARIOEDITOR_API TSharedRef<SWidget> Line(TAttribute<FText> Text, EText Kind = EText::Body, bool bSubdued = false);

	/** "/Game/Scenario/Meshes/2026-10-19" -> "Meshes/2026-10-19". */
	SCENARIOEDITOR_API FString ShortPath(const FString& Path);
	/** "just now", "5 min ago", "2 h ago", "Oct 19". */
	SCENARIOEDITOR_API FText RelativeTime(double UnixSeconds);
	/** The catalog's display name for a model id, else Fallback, else a readable form of the id. */
	SCENARIOEDITOR_API FString ModelName(const FString& ModelId, const FString& Fallback = FString());
}
