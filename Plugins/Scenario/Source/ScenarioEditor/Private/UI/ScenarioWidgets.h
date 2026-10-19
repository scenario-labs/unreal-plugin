// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Misc/Attribute.h"
#include "Widgets/SWidget.h"

struct FSlateBrush;

/**
 * The Studio's small visual system: one type scale, a 4 px spacing grid,
 * status chips, kind thumbnails and section headers. Every view builds on
 * these so the plugin reads like the editor's own panels.
 */
namespace ScenarioUI
{
	enum class EText : uint8
	{
		/** Sign-in tagline only. */
		Display,
		/** Model or workflow name, the selected asset, the Generate label. */
		Title,
		/** Section headers (References, Settings, Recent). */
		Section,
		/** Labels, values, list titles, prompt text. */
		Body,
		/** Notes, hints, costs, times. Use with a subdued color. */
		Meta,
		/** Status chips and badges. */
		Chip,
	};
	FSlateFontInfo Font(EText Kind);

	constexpr float Gutter = 8.0f;
	constexpr float RowGap = 4.0f;
	constexpr float SectionGap = 12.0f;
	constexpr float LabelWidth = 140.0f;

	/** A rounded label tinted with a state color (Generating, Ready, Failed...). */
	TSharedRef<SWidget> Chip(TAttribute<FText> Label, TAttribute<FSlateColor> Color, TAttribute<FText> Tooltip = TAttribute<FText>());

	/** An 8 px dot (agent status). */
	TSharedRef<SWidget> Dot(TAttribute<FSlateColor> Color);

	/** The editor's class thumbnail for an output kind: image, 3d, material, sky, audio, video. Never text. */
	const FSlateBrush* KindThumbnail(const FString& Kind);
	/** The 16 px class icon for an output kind. */
	const FSlateBrush* KindIcon(const FString& Kind);

	/** A square thumbnail: the image when there is one, else the kind's class thumbnail, dimmed. */
	TSharedRef<SWidget> Thumbnail(float Size, TAttribute<const FSlateBrush*> Image, TAttribute<FString> Kind);

	/** A Details-style category header with an optional widget on the right. */
	TSharedRef<SWidget> SectionHeader(const FText& Title, const TSharedPtr<SWidget>& Right = nullptr);

	/** A borderless icon button. The tooltip starts with a verb. */
	TSharedRef<SWidget> IconButton(FName Brush, const FText& Tooltip, FSimpleDelegate OnClick);

	/** A one-line label that ends with an ellipsis and shows the full text as its tooltip. */
	TSharedRef<SWidget> Line(TAttribute<FText> Text, EText Kind = EText::Body, bool bSubdued = false);

	/** "/Game/Scenario/Meshes/2026-10-19" -> "Meshes/2026-10-19". */
	FString ShortPath(const FString& Path);
	/** "just now", "5 min ago", "2 h ago", "Oct 19". */
	FText RelativeTime(double UnixSeconds);
	/** The catalog's display name for a model id, else Fallback, else a readable form of the id. */
	FString ModelName(const FString& ModelId, const FString& Fallback = FString());
}
