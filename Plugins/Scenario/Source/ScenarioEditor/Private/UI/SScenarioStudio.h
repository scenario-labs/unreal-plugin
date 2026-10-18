// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SBox;
class SScenarioCreatePage;

/** The Studio tab: header, left rail and the pages. Signed out, every page shows the sign-in. */
class SScenarioStudio : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioStudio) {}
		SLATE_ARGUMENT(FName, Page)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

	void SetPage(FName InPage);
	FName GetPage() const { return Page; }
	/** The Create page, created on first use. */
	TSharedPtr<SScenarioCreatePage> GetCreatePage();

	/** The open Studio, if any. */
	static TSharedPtr<SScenarioStudio> Current();

private:
	TSharedRef<SWidget> BuildHeader();
	TSharedRef<SWidget> BuildRail();
	TSharedRef<SWidget> PageWidget(FName InPage);
	int32 GateIndex() const;

	FName Page;
	TSharedPtr<SBox> PageBox;
	TMap<FName, TSharedPtr<SWidget>> Pages;
};
