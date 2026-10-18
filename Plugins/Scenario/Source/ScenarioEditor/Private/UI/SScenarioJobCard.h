// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioLedger.h"
#include "Styling/SlateColor.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FMenuBuilder;

/** Actions on jobs and outputs shared by the Create tray, Jobs, Results, the composer and menus. */
namespace ScenarioJobActions
{
	struct FNextStep
	{
		FText Label;
		FText Tooltip;
		FString Lane;
		FString Mode;
		/** When set, opens the model picker on this chip instead of a lane. */
		FString Chip;
		FString Modality;
	};

	/** What can follow an output of this kind (image, 3d, video, audio). */
	TArray<FNextStep> NextSteps(const FString& Kind);
	/** Opens Create (or the picker) with the asset as the reference. Sends nothing. */
	void RunNextStep(const FNextStep& Step, const FString& AssetId, const FString& Kind, const FString& Label);
	void AddNextStepsMenu(FMenuBuilder& Menu, const FString& AssetId, const FString& Kind, const FString& Label);

	/** "Generating 42%", "Ready", "Unknown"... */
	FText StateText(const FScenarioJobRow& Row);
	FSlateColor StateColor(const FScenarioJobRow& Row);
	FText CostText(const FScenarioJobRow& Row);
	/** The output kind of a job's first file (image when unknown). */
	FString OutputKind(const FScenarioJobRow& Row);
	/** A downloaded image of the job, for previews. */
	FString PreviewFile(const FScenarioJobRow& Row);

	void Import(const FString& LocalId, bool bPlace);
	/** Fills the Create form with the job's model and settings. Sends nothing. */
	void UseSettings(const FString& LocalId);
	void ShowDetails(const FString& LocalId);
	/** The "..." menu of a job. */
	TSharedRef<class SWidget> MoreMenu(const FString& LocalId);
}

/** One job: preview, state, cost and the actions that fit its state. */
class SScenarioJobCard : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioJobCard)
		: _bCompact(false)
	{}
		SLATE_ARGUMENT(FString, LocalId)
		/** One line for the composer. */
		SLATE_ARGUMENT(bool, bCompact)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	const FScenarioJobRow* Row() const;
	TSharedRef<SWidget> BuildActions();
	const FSlateBrush* PreviewBrush() const;
	FText PreviewLabel() const;
	EVisibility ShowWhen(TFunction<bool(const FScenarioJobRow&)> Test) const;

	FString LocalId;
	bool bCompact = false;
};
