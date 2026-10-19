// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioSchema.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FAssetThumbnail;
class FScenarioCreateModel;
class SBox;
class SGridPanel;
class SMultiLineEditableTextBox;
class SVerticalBox;
struct FScenarioReference;

/**
 * The form of a model or workflow, built from its schema: the prompt (with
 * Examples and Improve prompt), references as drop-in slots, and settings in
 * a Details-style grid. Every edit goes to the shared model, which re-checks
 * the price; prices only show on the buttons that spend.
 */
class SScenarioForm : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioForm) {}
		SLATE_ARGUMENT(TSharedPtr<FScenarioCreateModel>, Model)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioForm() override;

	/** Puts the keyboard focus in the prompt. */
	void FocusPrompt();

private:
	void OnModelChanged();
	void Rebuild();
	FString Signature() const;

	TSharedRef<SWidget> BuildPrompt(const FScenarioField& Field);
	TSharedRef<SWidget> BuildExamples();
	TSharedRef<SWidget> BuildImprove();
	TSharedRef<SWidget> BuildSlots(const FScenarioField& Field);
	TSharedRef<SWidget> BuildSlot(const FScenarioField& Field, const FScenarioReference& Reference, int32 IndexInField);
	TSharedRef<SWidget> BuildAddSlot(const FScenarioField& Field);
	TSharedRef<SWidget> BuildAddMenu(FScenarioField Field);
	TSharedRef<SWidget> BuildControl(const FScenarioField& Field);
	TSharedRef<SWidget> BuildSize(const FScenarioField& Width, const FScenarioField& Height);
	/** One Details-style row: the label column, then the value. */
	void AddRow(const TSharedRef<SGridPanel>& Grid, int32& Row, const FText& Label, const FText& Tooltip, const TSharedRef<SWidget>& Value, bool bOptional, bool bMore);
	void AddFieldRow(const TSharedRef<SGridPanel>& Grid, int32& Row, const FScenarioField& Field, bool bMore);
	/** Runs an add-reference action and shows its error under References. */
	void RunAdd(TFunction<FString()> Action);

	// Improve prompt: priced for free while typing, one click runs it.
	void SchedulePriceSpark();
	void PriceSpark(bool bRunAfter, const FString& ShownPrice);
	void RunSpark();
	bool IsSparkPriced() const;
	void SetSparkMessage(const FString& Text, bool bError);
	void RefreshVariants();

	TSharedPtr<FScenarioCreateModel> Model;
	TSharedPtr<SBox> Content;
	TSharedPtr<SMultiLineEditableTextBox> PromptBox;
	TSharedPtr<SVerticalBox> VariantsBox;
	TArray<TSharedPtr<FAssetThumbnail>> SlotThumbnails;
	FString BuiltSignature;
	FDelegateHandle ModelHandle;
	bool bShowMore = false;
	FString InlineError;

	FString LastPrompt;
	FString SparkPrice;
	FString SparkPricedPrompt;
	double SparkPricedAt = 0.0;
	bool bSparkPricing = false;
	bool bSparkRunning = false;
	TArray<FString> SparkVariants;
	FString SparkMessage;
	bool bSparkMessageError = false;
	TSharedPtr<class FActiveTimerHandle> SparkTimer;
};
