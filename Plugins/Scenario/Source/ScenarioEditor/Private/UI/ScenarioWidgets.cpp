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
#include "Styling/StyleDefaults.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
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
		case EText::Heading: return FCoreStyle::GetDefaultFontStyle("Bold", 12);
		case EText::Title: return FCoreStyle::GetDefaultFontStyle("Bold", 10);
		case EText::Section:
		{
			FSlateFontInfo Small = FCoreStyle::GetDefaultFontStyle("Bold", 7);
			Small.LetterSpacing = 120;
			return Small;
		}
		case EText::Meta: return FCoreStyle::GetDefaultFontStyle("Regular", 8);
		case EText::Chip: return FCoreStyle::GetDefaultFontStyle("Bold", 7);
		case EText::Body:
		default: return FCoreStyle::GetDefaultFontStyle("Regular", 9);
		}
	}

	const ISlateStyle& Style()
	{
		return FScenarioStyle::Get();
	}

	const FSlateBrush* IconBrush(FName Name)
	{
		const FSlateBrush* Brush = FScenarioStyle::Get().GetOptionalBrush(FName(*(TEXT("Scenario.Icon.") + Name.ToString())), nullptr, nullptr);
		return Brush ? Brush : FStyleDefaults::GetNoBrush();
	}

	TSharedRef<SWidget> Icon(FName Name, float Size, TAttribute<FSlateColor> Color)
	{
		return SNew(SBox)
			.WidthOverride(Size)
			.HeightOverride(Size)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(IconBrush(Name))
				.DesiredSizeOverride(FVector2D(Size, Size))
				.ColorAndOpacity(Color)
			];
	}

	FName KindIconName(const FString& Kind)
	{
		if (Kind == TEXT("3d"))
		{
			return TEXT("cube");
		}
		if (Kind == TEXT("environment") || Kind == TEXT("sky") || Kind == TEXT("image-hdr"))
		{
			return TEXT("skybox");
		}
		if (Kind == TEXT("workflow"))
		{
			return TEXT("workflows");
		}
		if (Kind == TEXT("render"))
		{
			return TEXT("wand");
		}
		if (Kind == TEXT("material") || Kind == TEXT("audio") || Kind == TEXT("video") || Kind == TEXT("motion"))
		{
			return FName(*Kind);
		}
		return TEXT("image");
	}

	FName ButtonStyleName(EButton Kind, bool bSmall)
	{
		static const FName Names[4][2] = {
			{ TEXT("Scenario.Button.Primary"), TEXT("Scenario.Button.Primary.Small") },
			{ TEXT("Scenario.Button"), TEXT("Scenario.Button.Small") },
			{ TEXT("Scenario.Button.Outline"), TEXT("Scenario.Button.Outline.Small") },
			{ TEXT("Scenario.Button.Ghost"), TEXT("Scenario.Button.Ghost.Small") },
		};
		return Names[static_cast<int32>(Kind)][bSmall ? 1 : 0];
	}

	TSharedRef<SButton> Button(TAttribute<FText> Label, EButton Kind, TFunction<void()> OnClick, FName IconName, TAttribute<FText> Tooltip, bool bSmall)
	{
		const bool bPrimary = Kind == EButton::Primary;
		const bool bIconOnly = !Label.IsBound() && Label.Get().IsEmpty();
		TSharedRef<SHorizontalBox> Content = SNew(SHorizontalBox);
		if (!IconName.IsNone())
		{
			Content->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, bIconOnly ? 0.0f : 6.0f, 0.0f)
				[
					Icon(IconName, bSmall ? 14.0f : 16.0f)
				];
		}
		if (!bIconOnly)
		{
			Content->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Label)
					.Font(bPrimary ? FCoreStyle::GetDefaultFontStyle("Bold", bSmall ? 8 : 10) : FCoreStyle::GetDefaultFontStyle("Regular", bSmall ? 8 : 9))
				];
		}
		return SNew(SButton)
			.ButtonStyle(&FScenarioStyle::Get().GetWidgetStyle<FButtonStyle>(ButtonStyleName(Kind, bSmall)))
			.ToolTipText(Tooltip)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.OnClicked_Lambda([OnClick]()
			{
				if (OnClick)
				{
					OnClick();
				}
				return FReply::Handled();
			})
			[
				Content
			];
	}

	TSharedRef<SWidget> ChipToggle(TAttribute<FText> Label, TAttribute<bool> IsOn, TFunction<void()> OnClick, FName IconName, TAttribute<FText> Tooltip, bool bSmall)
	{
		TSharedRef<SHorizontalBox> Content = SNew(SHorizontalBox);
		if (!IconName.IsNone())
		{
			Content->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					Icon(IconName, bSmall ? 12.0f : 14.0f)
				];
		}
		Content->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font_Lambda([IsOn, bSmall]() { return FCoreStyle::GetDefaultFontStyle(IsOn.Get() ? "Bold" : "Regular", bSmall ? 8 : 9); })
			];
		return SNew(SCheckBox)
			.Style(&FScenarioStyle::Get().GetWidgetStyle<FCheckBoxStyle>(bSmall ? "Scenario.Chip.Toggle.Small" : "Scenario.Chip.Toggle"))
			.ToolTipText(Tooltip)
			.IsChecked_Lambda([IsOn]() { return IsOn.Get() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([OnClick](ECheckBoxState)
			{
				if (OnClick)
				{
					OnClick();
				}
			})
			[
				Content
			];
	}

	FLinearColor ToneColor(ETone Tone)
	{
		switch (Tone)
		{
		case ETone::Info: return Palette::Info();
		case ETone::Success: return Palette::Success();
		case ETone::Warning: return Palette::Warning();
		case ETone::Error: return Palette::Error();
		default: return Palette::Dim();
		}
	}

	TSharedRef<SWidget> Pill(TAttribute<FText> Label, TAttribute<ETone> Tone, TAttribute<FText> Tooltip)
	{
		return Chip(MoveTemp(Label), MakeAttributeLambda([Tone]() { return FSlateColor(ToneColor(Tone.Get())); }), MoveTemp(Tooltip));
	}

	TSharedRef<SWidget> Card(const TSharedRef<SWidget>& Content, FMargin Padding)
	{
		return SNew(SBorder)
			.BorderImage(FScenarioStyle::Get().GetBrush("Scenario.Card"))
			.Padding(Padding)
			[
				Content
			];
	}

	TSharedRef<SWidget> PageHeader(const FText& Title, TAttribute<FText> Subtitle, const TSharedPtr<SWidget>& Right)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(Title)
					.Font(Font(EText::Heading))
					.ColorAndOpacity(Palette::Text())
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 3.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(Subtitle)
					.Font(Font(EText::Body))
					.ColorAndOpacity(Palette::Dim())
					.AutoWrapText(true)
					.Visibility_Lambda([Subtitle]() { return Subtitle.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(Gutter, 0.0f, 0.0f, 0.0f)
			[
				Right.IsValid() ? Right.ToSharedRef() : SNullWidget::NullWidget
			];
	}

	TSharedRef<SWidget> UnderlineTabs(const TArray<TPair<FName, FText>>& Tabs, TAttribute<FName> Current, TFunction<void(FName)> OnChanged)
	{
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		for (const TPair<FName, FText>& Tab : Tabs)
		{
			const FName Id = Tab.Key;
			auto IsOn = [Current, Id]() { return Current.Get() == Id; };
			Row->AddSlot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 18.0f, 0.0f)
				[
					SNew(SCheckBox)
					.Style(&FScenarioStyle::Get().GetWidgetStyle<FCheckBoxStyle>("Scenario.Tab"))
					.IsChecked_Lambda([IsOn]() { return IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([OnChanged, Id](ECheckBoxState)
					{
						if (OnChanged)
						{
							OnChanged(Id);
						}
					})
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 4.0f, 0.0f, 7.0f)
						[
							SNew(STextBlock)
							.Text(Tab.Value)
							.Font_Lambda([IsOn]() { return FCoreStyle::GetDefaultFontStyle(IsOn() ? "Bold" : "Regular", 9); })
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SBox)
							.HeightOverride(2.0f)
							[
								SNew(SImage)
								.Image(FScenarioStyle::Get().GetBrush("Scenario.Underline"))
								.Visibility_Lambda([IsOn]() { return IsOn() ? EVisibility::HitTestInvisible : EVisibility::Hidden; })
							]
						]
					]
				];
		}
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				Row
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				Divider()
			];
	}

	TSharedRef<SWidget> Switch(TAttribute<bool> IsOn, TFunction<void(bool)> OnChanged, TAttribute<FText> Tooltip)
	{
		auto Knob = [IsOn](bool bRight)
		{
			return SNew(SImage)
				.Image(FScenarioStyle::Get().GetBrush("Scenario.Switch.Knob"))
				.ColorAndOpacity_Lambda([IsOn]() { return IsOn.Get() ? FSlateColor(FLinearColor::White) : FSlateColor(Palette::Dim()); })
				.Visibility_Lambda([IsOn, bRight]() { return IsOn.Get() == bRight ? EVisibility::HitTestInvisible : EVisibility::Hidden; });
		};
		return SNew(SCheckBox)
			.Style(&FScenarioStyle::Get().GetWidgetStyle<FCheckBoxStyle>("Scenario.Switch"))
			.ToolTipText(Tooltip)
			.IsChecked_Lambda([IsOn]() { return IsOn.Get() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([OnChanged](ECheckBoxState State)
			{
				if (OnChanged)
				{
					OnChanged(State == ECheckBoxState::Checked);
				}
			})
			[
				SNew(SBox)
				.WidthOverride(30.0f)
				.HeightOverride(16.0f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SImage)
						.Image(FScenarioStyle::Get().GetBrush("Scenario.Switch.Track"))
						.ColorAndOpacity_Lambda([IsOn]() { return IsOn.Get() ? FSlateColor(Palette::Success()) : FSlateColor(Palette::BorderStrong()); })
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Left)
					.VAlign(VAlign_Center)
					.Padding(2.0f, 0.0f)
					[
						Knob(false)
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Right)
					.VAlign(VAlign_Center)
					.Padding(2.0f, 0.0f)
					[
						Knob(true)
					]
				]
			];
	}

	TSharedRef<SWidget> Divider(bool bVertical)
	{
		return SNew(SBox)
			.WidthOverride(bVertical ? 1.0f : FOptionalSize())
			.HeightOverride(bVertical ? FOptionalSize() : 1.0f)
			[
				SNew(SImage).Image(FScenarioStyle::Get().GetBrush("Scenario.Line"))
			];
	}

	TSharedRef<SWidget> FieldLabel(const FText& Label, const FText& Hint, const FText& Tooltip)
	{
		return SNew(SVerticalBox)
			.ToolTipText(Tooltip)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(Font(EText::Body))
				.ColorAndOpacity(Palette::Dim())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(Hint)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
				.ColorAndOpacity(Palette::Faint())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.Visibility(Hint.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
			];
	}

	TSharedRef<SWidget> Kbd(const FText& Keys)
	{
		return SNew(SBorder)
			.BorderImage(FScenarioStyle::Get().GetBrush("Scenario.Kbd"))
			.Padding(FMargin(5.0f, 1.0f))
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Keys)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
				.ColorAndOpacity(Palette::Dim())
			];
	}

	TSharedRef<SWidget> Chip(TAttribute<FText> Label, TAttribute<FSlateColor> Color, TAttribute<FText> Tooltip)
	{
		return SNew(SBorder)
			.BorderImage(FScenarioStyle::Get().GetBrush("Scenario.Chip"))
			.BorderBackgroundColor_Lambda([Color]()
			{
				return FSlateColor(Color.Get().GetSpecifiedColor().CopyWithNewOpacity(0.16f));
			})
			.Padding(FMargin(7.0f, 2.0f))
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
		return IconBrush(KindIconName(Kind));
	}

	TSharedRef<SWidget> Thumbnail(float Size, TAttribute<const FSlateBrush*> Image, TAttribute<FString> Kind)
	{
		const float Inset = FMath::RoundToFloat(Size * 0.22f);
		const float Radius = FMath::Min(8.0f, FMath::RoundToFloat(Size * 0.18f));
		// The image drawn as a rounded box: a copy of its brush with rounded corners.
		struct FRounded
		{
			FSlateBrush Brush;
			const FSlateBrush* Source = nullptr;
			const UObject* Resource = nullptr;
		};
		const TSharedRef<FRounded> Rounded = MakeShared<FRounded>();
		auto RoundedImage = [Image, Rounded, Radius]() -> const FSlateBrush*
		{
			const FSlateBrush* Source = Image.Get();
			if (!Source)
			{
				return nullptr;
			}
			if (Source != Rounded->Source || Source->GetResourceObject() != Rounded->Resource || Source->GetImageSize() != Rounded->Brush.GetImageSize())
			{
				Rounded->Source = Source;
				Rounded->Resource = Source->GetResourceObject();
				Rounded->Brush = *Source;
				Rounded->Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
				Rounded->Brush.OutlineSettings = FSlateBrushOutlineSettings(FVector4(Radius, Radius, Radius, Radius));
			}
			return &Rounded->Brush;
		};
		return SNew(SBox)
			.WidthOverride(Size)
			.HeightOverride(Size)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage).Image(FScenarioStyle::Get().GetBrush("Scenario.Thumb"))
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
					.Image_Lambda(RoundedImage)
					.Visibility_Lambda([Image]() { return Image.Get() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				]
			];
	}

	TSharedRef<SWidget> SectionHeader(const FText& Title, const TSharedPtr<SWidget>& Right)
	{
		return SNew(SBox)
			.Padding(FMargin(Gutter, 14.0f, Gutter, 6.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Title.ToUpper())
					.Font(Font(EText::Section))
					.ColorAndOpacity(Palette::Faint())
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
		// "search": a Scenario icon; "Scenario.Mark": a Scenario brush; "Icons.Refresh": the editor's.
		const FString Name = Brush.ToString();
		const FSlateBrush* Image = !Name.Contains(TEXT(".")) ? IconBrush(Brush)
			: Name.StartsWith(TEXT("Scenario.")) ? FScenarioStyle::Get().GetBrush(Brush) : FAppStyle::GetBrush(Brush);
		return SNew(SButton)
			.ButtonStyle(&FScenarioStyle::Get().GetWidgetStyle<FButtonStyle>("Scenario.Button.Ghost.Small"))
			.ToolTipText(Tooltip)
			.ContentPadding(FMargin(4.0f))
			.VAlign(VAlign_Center)
			.OnClicked_Lambda([OnClick]()
			{
				OnClick.ExecuteIfBound();
				return FReply::Handled();
			})
			[
				SNew(SImage)
				.Image(Image)
				.DesiredSizeOverride(FVector2D(16.0f, 16.0f))
				.ColorAndOpacity(FSlateColor::UseForeground())
			];
	}

	TSharedRef<SWidget> Line(TAttribute<FText> Text, EText Kind, bool bSubdued)
	{
		return SNew(STextBlock)
			.Text(Text)
			.ToolTipText(Text)
			.Font(Font(Kind))
			.ColorAndOpacity(bSubdued ? FSlateColor(Palette::Dim()) : FSlateColor::UseForeground())
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
