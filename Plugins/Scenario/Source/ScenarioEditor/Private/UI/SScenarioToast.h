// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Notifications/INotificationWidget.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SCompoundWidget.h"

/** One button on a toast. It shows only in the listed states (all states when empty). */
struct FScenarioToastAction
{
	FText Label;
	FText Tooltip;
	TFunction<void()> Run;
	TArray<SNotificationItem::ECompletionState> States;
	/** Closes the toast after running. */
	bool bCloses = true;
};

/** What a toast says. Attributes update live (progress, state). */
struct FScenarioToastContent
{
	TAttribute<FText> Title;
	TAttribute<FText> Detail;
	/** 0 to 1 while a job runs; unset hides the bar. */
	TAttribute<TOptional<float>> Progress;
	TArray<FScenarioToastAction> Actions;
	FText CloseTooltip;
	/** Runs when the close cross is clicked (declining an approval, for one). */
	TFunction<void()> OnClosed;
};

/**
 * The plugin's notification: a close cross at the top left, a state icon
 * (throbber, success, failure or the Scenario mark), a title, one detail
 * line, an optional progress bar and the actions that fit the state. Every
 * Scenario toast uses it so they read alike.
 */
class SScenarioToast : public SCompoundWidget, public INotificationWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioToast) {}
		SLATE_ARGUMENT(FScenarioToastContent, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Shows a toast. ExpireSeconds 0 keeps it until it is closed or its state changes. */
	static TSharedPtr<SNotificationItem> Show(const FScenarioToastContent& Content, SNotificationItem::ECompletionState State, float ExpireSeconds);

	// INotificationWidget
	virtual void OnSetCompletionState(SNotificationItem::ECompletionState InState) override { State = InState; }
	virtual TSharedRef<SWidget> AsWidget() override { return AsShared(); }

private:
	void Close();

	FScenarioToastContent Content;
	SNotificationItem::ECompletionState State = SNotificationItem::CS_None;
	TWeakPtr<SNotificationItem> Item;
};
