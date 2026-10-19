// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioApi.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STileView.h"

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
	/** The team and project menu (the header's project name, Settings > Switch project). */
	TSharedRef<SWidget> ProjectMenu();
	/** What this editor's jobs cost today: charged amounts, or quotes until Scenario reports the charge. */
	double TodayCU();
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
	/** The lane's recommended models as chips under the model card. */
	TSharedRef<SWidget> BuildPicks();
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
	TSharedPtr<SBox> PicksBox;
	FString PicksFor;
	TSharedPtr<SWidget> ModelCard;
	TSharedPtr<class SHorizontalBox> Recent;
	TArray<FString> RecentShown;
	FString ModesFor;
	FDelegateHandle ModelHandle;
	FDelegateHandle SessionHandle;
};

/**
 * Library: assets on Scenario (searched live) and the ones imported into this
 * project (from provenance), as thumbnail tiles. The footer acts on the
 * selected tile: Import or Show, then Send to another model.
 */
class SScenarioLibraryPage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioLibraryPage) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioLibraryPage() override;

private:
	/** One tile: an asset on Scenario, or an asset imported into this project. */
	struct FTile
	{
		bool bLocal = false;
		FScenarioAssetInfo Asset;
		FString ObjectPath;
		TMap<FName, FString> Values;
		FString AssetId;
		FString Title;
		FString ModelName;
		/** image, 3d, material, sky, audio or video. */
		FString Kind;
		double CreatedAt = 0.0;
		FString ThumbnailUrl;
		TSharedPtr<class FAssetThumbnail> Thumbnail;
	};
	using FItem = TSharedPtr<FTile>;

	void SetSource(bool bInLocal);
	void ScheduleSearch();
	void Search(bool bAppend);
	/** The scope, query and type the cloud tiles were searched with. */
	FString SearchKey() const;
	void OnSessionChanged();
	void RefreshLocal();
	void RefreshImported();
	void ApplyFilter();
	void ImportAsset(const FScenarioAssetInfo& Asset, bool bPlace);
	void Activate(const FItem& Item);
	bool IsImporting(const FString& AssetId) const;
	TSharedRef<ITableRow> MakeTile(FItem Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<SWidget> BuildFooter();
	TSharedRef<SWidget> BuildEmpty();
	TSharedRef<SWidget> TileThumbnail(const FItem& Item, float Size);

	bool bLocal = false;
	FString Query;
	/** Empty for every type, else image, 3d, material, sky, audio, video or "new" (not imported yet). */
	FString Kind;
	TArray<FItem> Cloud;
	TArray<FItem> Locals;
	TArray<FItem> Items;
	FItem Selected;
	TSharedPtr<STileView<FItem>> Tiles;
	TSharedPtr<class SSearchBox> SearchBox;
	TSharedPtr<class SBox> FooterBox;
	FString Error;
	FString SearchedKey;
	int32 LocalCount = -1;
	int32 Generation = 0;
	bool bLoading = false;
	bool bMore = false;
	/** Scenario asset ids imported into this project. */
	TSet<FString> Imported;
	/** Library imports waiting for their files: job row to asset id. */
	TMap<FString, FString> PendingImports;
	TSet<FString> PlaceAfterImport;
	FDelegateHandle FilesHandle;
	FDelegateHandle SessionHandle;
	TSharedPtr<class FActiveTimerHandle> SearchTimer;
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

/** Settings: the account and project, this plugin's preferences, diagnostics and versions. */
class SScenarioSettingsPage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioSettingsPage) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioSettingsPage() override;

	/** Plain-text diagnostics without secrets, for support. */
	static FString Diagnostics();

private:
	void Rebuild();

	TSharedPtr<SBox> Content;
	bool bWasSignedIn = false;
	FDelegateHandle SessionHandle;
};

/** Workflows: the team's and Scenario's featured ones; a row opens it in Create. */
class SScenarioWorkflowsPage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioWorkflowsPage) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioWorkflowsPage() override;

private:
	void Refresh();
	TSharedRef<SWidget> BuildRow(const struct FScenarioWorkflowItem& Item);

	FName Source = TEXT("team");
	FString Query;
	FString Shown;
	TSharedPtr<SVerticalBox> Rows;
	FDelegateHandle ModelHandle;
};

/** Agent: Scenario MCP in this editor, from the sections other modules add. */
class SScenarioAgentPage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioAgentPage) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};
