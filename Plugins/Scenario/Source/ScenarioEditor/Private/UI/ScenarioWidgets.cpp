// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioWidgets.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "FileMediaSource.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ScenarioCatalog.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "ScenarioSubsystem.h"
#include "ScenarioTypes.h"
#include "Sound/SoundWave.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateIconFinder.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioWidgets"

namespace ScenarioUI
{
	FSlateFontInfo Font(EText Kind)
	{
		switch (Kind)
		{
		case EText::Display: return FCoreStyle::GetDefaultFontStyle("Bold", 14);
		case EText::Title: return FCoreStyle::GetDefaultFontStyle("Bold", 10);
		case EText::Section: return FCoreStyle::GetDefaultFontStyle("Bold", 8);
		case EText::Meta: return FCoreStyle::GetDefaultFontStyle("Regular", 8);
		case EText::Chip: return FCoreStyle::GetDefaultFontStyle("Bold", 8);
		case EText::Body:
		default: return FCoreStyle::GetDefaultFontStyle("Regular", 9);
		}
	}

	TSharedRef<SWidget> Chip(TAttribute<FText> Label, TAttribute<FSlateColor> Color, TAttribute<FText> Tooltip)
	{
		return SNew(SBorder)
			.BorderImage(FScenarioStyle::Get().GetBrush("Scenario.Chip"))
			.BorderBackgroundColor_Lambda([Color]()
			{
				return FSlateColor(Color.Get().GetSpecifiedColor().CopyWithNewOpacity(0.18f));
			})
			.Padding(FMargin(6.0f, 1.0f))
			.VAlign(VAlign_Center)
			.ToolTipText(Tooltip)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(Font(EText::Chip))
				.ColorAndOpacity(Color)
			];
	}

	TSharedRef<SWidget> Dot(TAttribute<FSlateColor> Color)
	{
		return SNew(SBox)
			.WidthOverride(8.0f)
			.HeightOverride(8.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(FScenarioStyle::Get().GetBrush("Scenario.Dot"))
				.ColorAndOpacity(Color)
			];
	}

	static UClass* ClassForKind(const FString& Kind)
	{
		if (Kind == TEXT("3d"))
		{
			return UStaticMesh::StaticClass();
		}
		if (Kind == TEXT("material"))
		{
			return UMaterialInstanceConstant::StaticClass();
		}
		if (Kind == TEXT("environment") || Kind == TEXT("sky") || Kind == TEXT("image-hdr"))
		{
			return UTextureCube::StaticClass();
		}
		if (Kind == TEXT("audio"))
		{
			return USoundWave::StaticClass();
		}
		if (Kind == TEXT("video"))
		{
			return UFileMediaSource::StaticClass();
		}
		return UTexture2D::StaticClass();
	}

	const FSlateBrush* KindThumbnail(const FString& Kind)
	{
		const FSlateBrush* Brush = FSlateIconFinder::FindCustomIconBrushForClass(ClassForKind(Kind), TEXT("ClassThumbnail"));
		return Brush ? Brush : FAppStyle::GetBrush("ClassThumbnail.Texture2D");
	}

	const FSlateBrush* KindIcon(const FString& Kind)
	{
		return FSlateIconFinder::FindIconBrushForClass(ClassForKind(Kind));
	}

	TSharedRef<SWidget> Thumbnail(float Size, TAttribute<const FSlateBrush*> Image, TAttribute<FString> Kind)
	{
		const float Inset = FMath::RoundToFloat(Size * 0.22f);
		return SNew(SBox)
			.WidthOverride(Size)
			.HeightOverride(Size)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage).Image(FAppStyle::GetBrush("ContentBrowser.AssetTileItem.ThumbnailAreaBackground"))
				]
				+ SOverlay::Slot()
				.Padding(Inset)
				[
					SNew(SImage)
					.Image_Lambda([Kind]() { return KindThumbnail(Kind.Get()); })
					.ColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, 0.45f))
					.Visibility_Lambda([Image]() { return Image.Get() ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
				]
				+ SOverlay::Slot()
				[
					SNew(SImage)
					.Image(Image)
					.Visibility_Lambda([Image]() { return Image.Get() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				]
			];
	}

	TSharedRef<SWidget> SectionHeader(const FText& Title, const TSharedPtr<SWidget>& Right)
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
			.Padding(FMargin(Gutter, 4.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Title)
					.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					Right.IsValid() ? Right.ToSharedRef() : SNullWidget::NullWidget
				]
			];
	}

	TSharedRef<SWidget> IconButton(FName Brush, const FText& Tooltip, FSimpleDelegate OnClick)
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(Tooltip)
			.ContentPadding(FMargin(4.0f, 2.0f))
			.VAlign(VAlign_Center)
			.OnClicked_Lambda([OnClick]()
			{
				OnClick.ExecuteIfBound();
				return FReply::Handled();
			})
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(Brush))
				.ColorAndOpacity(FSlateColor::UseForeground())
			];
	}

	TSharedRef<SWidget> Line(TAttribute<FText> Text, EText Kind, bool bSubdued)
	{
		return SNew(STextBlock)
			.Text(Text)
			.ToolTipText(Text)
			.Font(Font(Kind))
			.ColorAndOpacity(bSubdued ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground())
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis);
	}

	FString ShortPath(const FString& Path)
	{
		FString Out = Path;
		Out.RemoveFromStart(TEXT("/Game/Scenario/"));
		Out.RemoveFromStart(TEXT("/Game/"));
		return Out;
	}

	FText RelativeTime(double UnixSeconds)
	{
		if (UnixSeconds <= 0.0)
		{
			return FText::GetEmpty();
		}
		const double Age = FMath::Max(0.0, ScenarioNow() - UnixSeconds);
		if (Age < 60.0)
		{
			return LOCTEXT("JustNow", "just now");
		}
		if (Age < 3600.0)
		{
			return FText::Format(LOCTEXT("Minutes", "{0} min ago"), FText::AsNumber(FMath::FloorToInt(Age / 60.0)));
		}
		if (Age < 86400.0)
		{
			return FText::Format(LOCTEXT("Hours", "{0} h ago"), FText::AsNumber(FMath::FloorToInt(Age / 3600.0)));
		}
		const FDateTime When = FDateTime::FromUnixTimestamp(static_cast<int64>(UnixSeconds));
		return FText::AsDate(When, EDateTimeStyle::Medium, FText::GetInvariantTimeZone());
	}

	FString ModelName(const FString& ModelId, const FString& Fallback)
	{
		if (UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get())
		{
			if (const FScenarioCatalogModel* Model = Subsystem->Session().Catalog().Find(ModelId))
			{
				if (!Model->Summary.Name.IsEmpty())
				{
					return Model->Summary.Name;
				}
			}
		}
		if (!Fallback.IsEmpty())
		{
			return Fallback;
		}
		// "model_meshy-7-1-txt23d" -> "Meshy 7 1 txt23d": readable, never shown when the catalog knows it.
		FString Readable = ModelId;
		Readable.RemoveFromStart(TEXT("model_"));
		Readable.ReplaceInline(TEXT("-"), TEXT(" "));
		Readable.ReplaceInline(TEXT("_"), TEXT(" "));
		if (Readable.Len() > 0)
		{
			Readable[0] = FChar::ToUpper(Readable[0]);
		}
		return Readable;
	}
}

#undef LOCTEXT_NAMESPACE
