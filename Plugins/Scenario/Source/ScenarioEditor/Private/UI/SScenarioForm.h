// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioSchema.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FScenarioCreateModel;
class SBox;
class SMultiLineEditableTextBox;

/**
 * The form of a model or workflow, built from its schema: prompt (with
 * Prompt Spark), references, settings and "More settings". Every edit goes to
 * the shared model, which re-checks the price.
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
	TSharedRef<SWidget> BuildReferences(const FScenarioField& Field);
	TSharedRef<SWidget> BuildField(const FScenarioField& Field);
	TSharedRef<SWidget> BuildControl(const FScenarioField& Field);
	TSharedRef<SWidget> Section(const FText& Title, const TSharedRef<SWidget>& Body, const TSharedPtr<SWidget>& Extra = nullptr);
	void ShowError(const FString& Error);

	TSharedPtr<FScenarioCreateModel> Model;
	TSharedPtr<SBox> Content;
	TSharedPtr<SMultiLineEditableTextBox> PromptBox;
	FString BuiltSignature;
	FDelegateHandle ModelHandle;
	bool bShowMore = false;

	FString SparkPrice;
	FString SparkPricedPrompt;
	TArray<FString> SparkVariants;
	bool bSparkBusy = false;
	FString InlineError;
};
