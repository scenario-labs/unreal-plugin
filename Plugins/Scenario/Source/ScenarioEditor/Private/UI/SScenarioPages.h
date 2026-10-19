// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioApi.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FScenarioCreateModel;
class FScenarioSession;
class SBox;
class SScenarioForm;
class SVerticalBox;

namespace ScenarioUI
{
	FScenarioSession* Session();
	/** A titled block used by every page. */
	TSharedRef<SWidget> Section(const FText& Title, const TSharedRef<SWidget>& Body);
}

/** Sign in with Scenario (browser or code) or with an API key. */
class SScenarioSignIn : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioSignIn) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FString Status;
	FString Error;
	FString DeviceCode;
	FString DeviceUrl;
	FString SignInUrl;
	FString Key;
	FString Secret;
	bool bShowKey = false;
	bool bShowPrivacy = false;
};

/** Team and project, when the account has several. */
class SScenarioWorkspacePicker : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioWorkspacePicker) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FString TeamId;
	FString ProjectId;
};

class SScenarioCreatePage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioCreatePage) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioCreatePage() override;
	void FocusPrompt();
	/** The model picker, or the workflow picker on the Workflow lane. */
	void OpenPicker();

private:
	TSharedRef<SWidget> BuildLanes();
	TSharedRef<SWidget> BuildModes();
	TSharedRef<SWidget> BuildModelCard();
	TSharedRef<SWidget> BuildModelMenu();
	TSharedRef<SWidget> BuildRecent();
	TSharedRef<SWidget> BuildActionBar();
	TSharedRef<SWidget> BuildDestination();
	void RefreshRecent();
	void OnModelChanged();
	void ShowSuggestions();

	TSharedPtr<FScenarioCreateModel> Model;
	TSharedPtr<SScenarioForm> Form;
	TSharedPtr<SBox> ModesBox;
	TSharedPtr<SWidget> ModelCard;
	TSharedPtr<class SHorizontalBox> Recent;
	TArray<FString> RecentShown;
	FString ModesFor;
	FDelegateHandle ModelHandle;
	FDelegateHandle SessionHandle;
};

class SScenarioLibraryPage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioLibraryPage) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioLibraryPage() override;

private:
	using FAsset = TSharedPtr<FScenarioAssetInfo>;
	using FLocal = TSharedPtr<TPair<FString, TMap<FName, FString>>>;

	void Search(bool bMore);
	void RefreshLocal();
	void ImportAsset(const FScenarioAssetInfo& Asset);
	TSharedRef<ITableRow> MakeAssetRow(FAsset Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakeLocalRow(FLocal Item, const TSharedRef<STableViewBase>& Owner);

	bool bLocal = false;
	FString Query;
	FString Kind;
	TArray<FAsset> Assets;
	TArray<FLocal> Locals;
	TSharedPtr<SListView<FAsset>> AssetList;
	TSharedPtr<SListView<FLocal>> LocalList;
	FString Status;
	int32 Generation = 0;
	bool bLoading = false;
	TSet<FString> PendingImports;
	FDelegateHandle FilesHandle;
};

class SScenarioJobsPage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioJobsPage) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioJobsPage() override;
	/** all, active, ready or attention. */
	void SetFilter(const FString& InFilter);

private:
	using FItem = TSharedPtr<FString>;
	void Refresh();
	TSharedRef<ITableRow> MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner);
	bool Passes(const FScenarioJobRow& Row) const;

	FString Filter = TEXT("all");
	/** Empty for every type, else image, 3d, material, sky, audio or video. */
	FString Kind;
	TArray<FItem> Items;
	TArray<FString> Shown;
	TSharedPtr<SListView<FItem>> List;
	FDelegateHandle SessionHandle;
};

class SScenarioConnectionPage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioConnectionPage) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioConnectionPage() override;

	/** Plain-text diagnostics without secrets, for support. */
	static FString Diagnostics();

private:
	void Rebuild();

	TSharedPtr<SBox> Content;
	bool bWasSignedIn = false;
	FDelegateHandle SessionHandle;
};
