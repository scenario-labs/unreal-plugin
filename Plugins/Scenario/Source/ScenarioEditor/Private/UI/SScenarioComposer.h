// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FScenarioCreateModel;
class SBox;
class SEditableTextBox;

/**
 * The quick composer: a bar at the bottom of the level viewport sharing the
 * Studio's Create state (lane, model, prompt, price). Enter generates when
 * the price is current; Esc hands the keyboard back to the viewport. It never
 * places anything by itself.
 */
class SScenarioComposer : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioComposer) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioComposer() override;
	void FocusPrompt();

	/** Adds the composer to the active level viewport, or removes it. */
	static void Show(bool bVisible);
	static bool IsShown();

private:
	void RefreshLatest();

	TSharedPtr<FScenarioCreateModel> Model;
	TSharedPtr<SEditableTextBox> Prompt;
	TSharedPtr<SBox> Latest;
	FString LatestId;
	FDelegateHandle SessionHandle;
};
