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
	/** The big Generate button bound to a form model. */
	TSharedRef<SWidget> GenerateButton(const TSharedRef<FScenarioCreateModel>& Model, const FText& Hint);
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
	void OpenPicker();

private:
	TSharedRef<SWidget> BuildLanes();
	TSharedRef<SWidget> BuildModes();
	TSharedRef<SWidget> BuildModelCard();
	TSharedRef<SWidget> BuildDestination();
	TSharedRef<SWidget> BuildTray();
	void RefreshTray();
	void OnModelChanged();

	TSharedPtr<FScenarioCreateModel> Model;
	TSharedPtr<SScenarioForm> Form;
	TSharedPtr<SBox> ModesBox;
	TSharedPtr<SVerticalBox> Tray;
	TArray<FString> TrayIds;
	FString ModesFor;
	FDelegateHandle ModelHandle;
	FDelegateHandle SessionHandle;
};

class SScenarioWorkflowsPage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioWorkflowsPage) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioWorkflowsPage() override;

private:
	struct FWorkflow
	{
		FString Id;
		FString Name;
		FString Description;
		bool bTeam = false;
	};
	using FItem = TSharedPtr<FWorkflow>;

	void Load();
	TSharedRef<ITableRow> MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner);

	TSharedPtr<FScenarioCreateModel> Model;
	TArray<FItem> Items;
	TSharedPtr<SListView<FItem>> List;
	FString Status;
	bool bLoading = false;
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

private:
	using FItem = TSharedPtr<FString>;
	void Refresh();
	TSharedRef<ITableRow> MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner);

	/** all, active, attention */
	FString Filter = TEXT("all");
	TArray<FItem> Items;
	TArray<FString> Shown;
	TSharedPtr<SListView<FItem>> List;
	FDelegateHandle SessionHandle;
};

class SScenarioResultsPage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioResultsPage) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioResultsPage() override;

private:
	using FItem = TSharedPtr<FString>;
	using FCloud = TSharedPtr<FScenarioJobInfo>;
	void Refresh();
	void LoadCloud();
	TSharedRef<ITableRow> MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakeCloudRow(FCloud Item, const TSharedRef<STableViewBase>& Owner);

	FString Lane;
	TArray<FItem> Items;
	TArray<FString> Shown;
	TArray<FCloud> Cloud;
	TSharedPtr<SListView<FItem>> List;
	TSharedPtr<SListView<FCloud>> CloudList;
	FString CloudStatus;
	bool bCloudLoading = false;
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
