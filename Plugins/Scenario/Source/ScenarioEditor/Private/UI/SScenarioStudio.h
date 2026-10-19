// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SBox;
class SScenarioCreatePage;

/**
 * The Studio tab: a header (project, today's usage, agents, account) and a
 * rail (Create, Library, Jobs, Workflows, Agent, Settings) beside the current
 * page. Signed out, or before a project is chosen, the tab shows the sign-in
 * or the project picker alone. Below 560 px the rail keeps its icons only.
 */
class SScenarioStudio : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioStudio) {}
		SLATE_ARGUMENT(FName, Page)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

	/** Opens a page. Connection opens Settings, Results opens Jobs on Ready. */
	void SetPage(FName InPage);
	FName GetPage() const { return Page; }
	/** The Create page, created on first use. */
	TSharedPtr<SScenarioCreatePage> GetCreatePage();

	/** The open Studio, if any. */
	static TSharedPtr<SScenarioStudio> Current();

private:
	TSharedRef<SWidget> BuildHeader();
	TSharedRef<SWidget> BuildRail();
	TSharedRef<SWidget> BuildProjectMenu();
	TSharedRef<SWidget> BuildAccountMenu();
	TSharedRef<SWidget> PageWidget(FName InPage);
	int32 GateIndex() const;
	bool IsNarrow() const;

	FName Page;
	TSharedPtr<SBox> PageBox;
	TMap<FName, TSharedPtr<SWidget>> Pages;
};
