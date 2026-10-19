// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioGameHud.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "ScenarioGameplayActors.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	const FSlateBrush* Panel()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor(0.0f, 0.0f, 0.0f, 0.5f), 10.0f);
		return &Brush;
	}

	FSlateFontInfo Font(const char* Weight, int32 Size)
	{
		return FCoreStyle::GetDefaultFontStyle(Weight, Size);
	}
}

void SScenarioGameHud::Construct(const FArguments& InArgs)
{
	Rules = InArgs._Rules;
	auto Read = [this](FText (AScenarioGameRules::*Getter)() const)
	{
		return TAttribute<FText>::CreateLambda([this, Getter]()
		{
			const AScenarioGameRules* R = Rules.Get();
			return R ? (R->*Getter)() : FText::GetEmpty();
		});
	};
	auto Accent = [this]()
	{
		const AScenarioGameRules* R = Rules.Get();
		return FSlateColor(R ? R->AccentColor : FLinearColor::White);
	};

	ChildSlot
	[
		SNew(SOverlay)
		.Visibility(EVisibility::HitTestInvisible)

		+ SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(32.0f)
		[
			SNew(SBorder).BorderImage(Panel()).Padding(FMargin(20.0f, 14.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Font(Font("Bold", 22)).ColorAndOpacity(FLinearColor::White)
					.Text_Lambda([this]() { const AScenarioGameRules* R = Rules.Get(); return R ? R->Title : FText::GetEmpty(); })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Font(Font("Regular", 13)).ColorAndOpacity(FLinearColor(0.85f, 0.85f, 0.85f))
					.Text(Read(&AScenarioGameRules::GetObjectiveText))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Font(Font("Bold", 18)).ColorAndOpacity_Lambda(Accent)
					.Text(Read(&AScenarioGameRules::GetProgressText))
				]
			]
		]

		+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(32.0f)
		[
			SNew(SBorder).BorderImage(Panel()).Padding(FMargin(20.0f, 10.0f))
			[
				SNew(STextBlock).Font(Font("Bold", 30))
				.ColorAndOpacity_Lambda([this]()
				{
					const AScenarioGameRules* R = Rules.Get();
					return FSlateColor(R && R->IsTimeShort() ? FLinearColor(1.0f, 0.35f, 0.3f) : FLinearColor::White);
				})
				.Text(Read(&AScenarioGameRules::GetTimerText))
			]
		]

		+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
		[
			SNew(SBorder).BorderImage(Panel()).Padding(FMargin(36.0f, 20.0f))
			.Visibility_Lambda([this]()
			{
				const AScenarioGameRules* R = Rules.Get();
				return R && !R->GetMessage().IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
			})
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(STextBlock).Font(Font("Bold", 34)).ColorAndOpacity(FLinearColor::White).Justification(ETextJustify::Center)
					.Text(Read(&AScenarioGameRules::GetMessage))
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Font(Font("Regular", 16)).ColorAndOpacity_Lambda(Accent).Justification(ETextJustify::Center)
					.Text(Read(&AScenarioGameRules::GetSubMessage))
					.Visibility_Lambda([this]()
					{
						const AScenarioGameRules* R = Rules.Get();
						return R && !R->GetSubMessage().IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
					})
				]
			]
		]
	];
}
