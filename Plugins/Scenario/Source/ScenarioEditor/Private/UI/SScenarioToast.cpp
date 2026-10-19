// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "SScenarioToast.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "ScenarioStyle.h"
#include "ScenarioWidgets.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioToast"

namespace
{
	constexpr float ToastWidth = 360.0f;
}

void SScenarioToast::Construct(const FArguments& InArgs)
{
	Content = InArgs._Content;

	TSharedRef<SWrapBox> Buttons = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 4.0f));
	for (const FScenarioToastAction& Action : Content.Actions)
	{
		const TArray<SNotificationItem::ECompletionState> States = Action.States;
		const TFunction<void()> Run = Action.Run;
		const bool bCloses = Action.bCloses;
		Buttons->AddSlot()
		[
			SNew(SButton)
			.ContentPadding(FMargin(8.0f, 2.0f))
			.ToolTipText(Action.Tooltip)
			.Visibility_Lambda([this, States]() { return States.Num() == 0 || States.Contains(State) ? EVisibility::Visible : EVisibility::Collapsed; })
			.OnClicked_Lambda([this, Run, bCloses]()
			{
				if (Run)
				{
					Run();
				}
				if (bCloses)
				{
					Close();
				}
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(Action.Label)
				.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
			]
		];
	}

	const TAttribute<TOptional<float>> Progress = Content.Progress;
	ChildSlot
	[
		SNew(SBox)
		.WidthOverride(ToastWidth)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			.Padding(FMargin(28.0f, 10.0f, 12.0f, 10.0f))
			[
				SNew(SHorizontalBox)
				// State: running, done, failed, or the Scenario mark.
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Top)
				.Padding(0.0f, 1.0f, 10.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(20.0f)
					.HeightOverride(20.0f)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						[
							SNew(SCircularThrobber)
							.Radius(8.0f)
							.Visibility_Lambda([this]() { return State == SNotificationItem::CS_Pending ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
						]
						+ SOverlay::Slot()
						[
							SNew(SImage)
							.DesiredSizeOverride(FVector2D(16.0f, 16.0f))
							.Visibility_Lambda([this]() { return State == SNotificationItem::CS_Pending ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
							.Image_Lambda([this]() -> const FSlateBrush*
							{
								switch (State)
								{
								case SNotificationItem::CS_Success: return FAppStyle::GetBrush("Icons.SuccessWithColor");
								case SNotificationItem::CS_Fail: return FAppStyle::GetBrush("Icons.ErrorWithColor");
								default: return FScenarioStyle::Get().GetBrush("Scenario.Mark");
								}
							})
						]
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(Content.Title)
						.Font(ScenarioUI::Font(ScenarioUI::EText::Title))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(Content.Detail)
						.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
						.Visibility_Lambda([Detail = Content.Detail]() { return Detail.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(SBox)
						.HeightOverride(3.0f)
						.Visibility_Lambda([this, Progress]() { return State == SNotificationItem::CS_Pending && Progress.Get().IsSet() ? EVisibility::Visible : EVisibility::Collapsed; })
						[
							SNew(SProgressBar)
							.Percent(Progress)
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						Buttons
					]
				]
			]
			// Close, top left.
			+ SOverlay::Slot()
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Top)
			.Padding(4.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(3.0f))
				.ToolTipText(Content.CloseTooltip.IsEmpty() ? LOCTEXT("CloseTip", "Close this notification.") : Content.CloseTooltip)
				.OnClicked_Lambda([this]()
				{
					if (Content.OnClosed)
					{
						Content.OnClosed();
					}
					Close();
					return FReply::Handled();
				})
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.X"))
					.DesiredSizeOverride(FVector2D(10.0f, 10.0f))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		]
	];
}

void SScenarioToast::Close()
{
	if (const TSharedPtr<SNotificationItem> Pinned = Item.Pin())
	{
		Pinned->SetExpireDuration(0.0f);
		Pinned->SetFadeOutDuration(0.2f);
		Pinned->ExpireAndFadeout();
	}
}

TSharedPtr<SNotificationItem> SScenarioToast::Show(const FScenarioToastContent& Content, SNotificationItem::ECompletionState State, float ExpireSeconds)
{
	if (!FSlateApplication::IsInitialized())
	{
		return nullptr;
	}
	const TSharedRef<SScenarioToast> Widget = SNew(SScenarioToast).Content(Content);
	FNotificationInfo Info(StaticCastSharedRef<INotificationWidget>(Widget));
	Info.bFireAndForget = ExpireSeconds > 0.0f;
	Info.ExpireDuration = ExpireSeconds;
	Info.FadeOutDuration = 0.5f;
	Info.WidthOverride = FOptionalSize();
	const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
	if (Item.IsValid())
	{
		Widget->Item = Item;
		Item->SetCompletionState(State);
		Widget->State = State;
	}
	return Item;
}

#undef LOCTEXT_NAMESPACE
