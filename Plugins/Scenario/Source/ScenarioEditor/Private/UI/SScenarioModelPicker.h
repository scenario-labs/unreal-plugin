// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioCatalog.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SSearchBox;
class SWindow;
struct FScenarioWorkflowItem;

/**
 * Choose a model: every Scenario model plus the team's trained ones, from the
 * cached catalog. Favorites first, then the lane's quick picks, then the
 * catalog. Type to search, arrows to move, Enter to choose, Esc to close.
 */
class SScenarioModelPicker : public SCompoundWidget
{
public:
	using FOnPicked = TFunction<void(const FString& ModelId, const FString& Name)>;

	SLATE_BEGIN_ARGS(SScenarioModelPicker) {}
		/** image, video, 3d or audio; empty for every type. */
		SLATE_ARGUMENT(FString, Modality)
		SLATE_ARGUMENT(FString, Chip)
		SLATE_ARGUMENT(TArray<FString>, QuickPicks)
		SLATE_ARGUMENT(FString, CurrentId)
	SLATE_END_ARGS()

	/** Opens the picker in its own window (non-modal). */
	static void Open(const FString& Modality, const FString& Chip, FOnPicked OnPicked, const TArray<FString>& QuickPicks = {}, const FString& CurrentId = FString());
	/** Opens the workflow picker: the team's workflows and Scenario's featured ones. */
	static void OpenWorkflows(TFunction<void(const FScenarioWorkflowItem& Workflow)> OnPicked, const FString& CurrentId = FString());

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioModelPicker() override;
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

	FOnPicked OnPicked;
	TWeakPtr<SWindow> Window;
	TSharedPtr<SSearchBox> SearchBox;

private:
	using FItem = TSharedPtr<FScenarioCatalogModel>;

	void Refresh();
	void Choose(const FItem& Item);
	TSharedRef<ITableRow> MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<SWidget> BuildTabs();
	TSharedRef<SWidget> BuildChips();
	TSharedRef<SWidget> BuildDetails();
	void SetModality(const FString& InModality);
	FScenarioCatalog* Catalog() const;

	FString Modality;
	FString Chip;
	FString Text;
	TArray<FString> QuickPicks;
	FString CurrentId;
	bool bShowLegacy = false;
	bool bTeamModels = true;
	TArray<FItem> Items;
	FItem Selected;
	TSharedPtr<SListView<FItem>> List;
	TSharedPtr<class SBox> ChipsBox;
	FDelegateHandle CatalogHandle;
};

/** Choose a workflow: search, Team or Featured, and the description before running it. */
class SScenarioWorkflowPicker : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioWorkflowPicker) {}
		SLATE_ARGUMENT(FString, CurrentId)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioWorkflowPicker() override;
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

	TFunction<void(const FScenarioWorkflowItem& Workflow)> OnPicked;
	TWeakPtr<SWindow> Window;
	TSharedPtr<SSearchBox> SearchBox;

private:
	using FItem = TSharedPtr<FScenarioWorkflowItem>;

	void Refresh();
	void Choose(const FItem& Item);
	TSharedRef<ITableRow> MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner);

	FString Text;
	/** all, team or featured. */
	FString Source = TEXT("all");
	FString CurrentId;
	TArray<FItem> Items;
	FItem Selected;
	TSharedPtr<SListView<FItem>> List;
	FDelegateHandle ModelHandle;
};
