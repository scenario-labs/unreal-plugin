// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetThumbnail.h"
#include "ContentBrowserModule.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IContentBrowserSingleton.h"
#include "Misc/PackageName.h"
#include "ScenarioCreateModel.h"
#include "ScenarioEditorModule.h"
#include "ScenarioJson.h"
#include "ScenarioLedger.h"
#include "ScenarioProvenance.h"
#include "ScenarioSession.h"
#include "ScenarioSubsystem.h"
#include "ScenarioThumbnails.h"
#include "ScenarioWidgets.h"
#include "SScenarioJobCard.h"
#include "SScenarioPages.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioLibrary"

namespace
{
	constexpr int32 PageSize = 50;
	constexpr float TileSize = 112.0f;
	constexpr float FooterThumbnail = 40.0f;

	FString ThumbnailUrl(const FScenarioAssetInfo& Asset)
	{
		const TSharedPtr<FJsonObject>& Raw = Asset.Raw;
		if (!Raw.IsValid())
		{
			return FString();
		}
		const TSharedPtr<FJsonObject> Thumb = ScenarioJson::GetObject(Raw, TEXT("thumbnail"));
		if (Thumb.IsValid())
		{
			return ScenarioJson::GetString(Thumb, TEXT("url"));
		}
		FString Text = ScenarioJson::GetString(Raw, TEXT("thumbnail"));
		if (Text.IsEmpty() && Asset.Kind.StartsWith(TEXT("image")))
		{
			Text = ScenarioJson::GetString(Raw, TEXT("url"));
		}
		return Text;
	}

	FString LaneForKind(const FString& Kind)
	{
		if (Kind == TEXT("3d") || Kind == TEXT("video") || Kind == TEXT("audio"))
		{
			return Kind;
		}
		return Kind == TEXT("image-hdr") ? TEXT("environment") : TEXT("image");
	}

	/** Scenario asset kinds the plugin can import. */
	bool IsImportable(const FString& AssetKind)
	{
		return AssetKind.StartsWith(TEXT("image")) || AssetKind == TEXT("3d") || AssetKind == TEXT("video") || AssetKind == TEXT("audio");
	}

	/** The tile kind of a Scenario asset: image-hdr is a sky. */
	FString TileKind(const FString& AssetKind)
	{
		if (AssetKind == TEXT("image-hdr"))
		{
			return TEXT("sky");
		}
		return AssetKind == TEXT("3d") || AssetKind == TEXT("video") || AssetKind == TEXT("audio") ? AssetKind : FString(TEXT("image"));
	}

	/** The tile kind of an imported asset, from the lane that made it. */
	FString KindForLane(const FString& Lane)
	{
		if (Lane == TEXT("environment"))
		{
			return TEXT("sky");
		}
		return Lane == TEXT("3d") || Lane == TEXT("material") || Lane == TEXT("video") || Lane == TEXT("audio") ? Lane : FString(TEXT("image"));
	}

	FText KindName(const FString& Kind)
	{
		if (Kind == TEXT("3d")) return LOCTEXT("Kind3D", "3D");
		if (Kind == TEXT("material")) return LOCTEXT("KindMaterial", "Material");
		if (Kind == TEXT("sky")) return LOCTEXT("KindSky", "Sky");
		if (Kind == TEXT("audio")) return LOCTEXT("KindAudio", "Audio");
		if (Kind == TEXT("video")) return LOCTEXT("KindVideo", "Video");
		return LOCTEXT("KindImage", "Image");
	}

	/** Next steps take an image, a mesh, a video or a sound. */
	FString StepKind(const FString& Kind)
	{
		return Kind == TEXT("3d") || Kind == TEXT("video") || Kind == TEXT("audio") ? Kind : FString(TEXT("image"));
	}

	double ParseTime(const FString& Iso)
	{
		FDateTime Time;
		return !Iso.IsEmpty() && FDateTime::ParseIso8601(*Iso, Time) ? static_cast<double>(Time.ToUnixTimestamp()) : 0.0;
	}

	/** The name, else the start of the prompt, else "3D from Meshy 7.1". Never an id. */
	FString TitleFor(const FString& Name, const FString& Prompt, const FString& Kind, const FString& Model)
	{
		if (!Name.IsEmpty())
		{
			return Name;
		}
		const FString Trimmed = Prompt.TrimStartAndEnd();
		if (!Trimmed.IsEmpty())
		{
			return Trimmed.Len() > 60 ? Trimmed.Left(60).TrimEnd() + TEXT("...") : Trimmed;
		}
		return Model.IsEmpty() ? KindName(Kind).ToString() : FString::Printf(TEXT("%s from %s"), *KindName(Kind).ToString(), *Model);
	}

	struct FTypeFilter
	{
		FString Id;
		FText Label;
		bool bCloud = true;
		bool bLocal = true;
	};

	const TArray<FTypeFilter>& TypeFilters()
	{
		static const TArray<FTypeFilter> List = {
			{ FString(), LOCTEXT("AllTypes", "All"), true, true },
			{ TEXT("image"), LOCTEXT("Images", "Images"), true, true },
			{ TEXT("3d"), LOCTEXT("Meshes", "3D"), true, true },
			{ TEXT("material"), LOCTEXT("Materials", "Materials"), false, true },
			{ TEXT("sky"), LOCTEXT("Skies", "Skies"), false, true },
			{ TEXT("audio"), LOCTEXT("Sounds", "Audio"), true, true },
			{ TEXT("video"), LOCTEXT("Videos", "Video"), true, true },
			{ TEXT("new"), LOCTEXT("NotImported", "Not imported yet"), true, false },
		};
		return List;
	}

	/** The search's kind filter for a type filter; empty searches every kind. */
	FString CloudKind(const FString& Filter)
	{
		return Filter == TEXT("new") ? FString() : Filter;
	}

	/** The imported asset made from a Scenario asset, or empty. */
	FString LocalPathFor(const FString& AssetId)
	{
		const UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
		if (!Subsystem || AssetId.IsEmpty())
		{
			return FString();
		}
		for (const TPair<FString, TMap<FName, FString>>& Entry : Subsystem->Provenance().All())
		{
			const FString* Id = Entry.Value.Find(TEXT("Scenario.AssetId"));
			if (Id && *Id == AssetId)
			{
				return Entry.Key;
			}
		}
		return FString();
	}

	void ShowInContentBrowser(const FString& ObjectPath)
	{
		// Registry lookup: shows the asset without loading it.
		const FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		if (Data.IsValid())
		{
			FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(TArray<FAssetData>{ Data });
		}
	}

	/** Shows the widget only while When() holds. */
	TSharedRef<SWidget> ShownWhen(const TSharedRef<SWidget>& Widget, TFunction<bool()> When)
	{
		Widget->SetVisibility(MakeAttributeLambda([When]() { return When() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; }));
		return Widget;
	}
}

void SScenarioLibraryPage::Construct(const FArguments& InArgs)
{
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		// Library imports wait for their files, then import into the project.
		const TWeakPtr<SScenarioLibraryPage> Weak = SharedThis(this);
		FilesHandle = Session->OnFilesReady.AddLambda([Weak](const FString& LocalId)
		{
			const TSharedPtr<SScenarioLibraryPage> This = Weak.Pin();
			if (This.IsValid() && This->PendingImports.Remove(LocalId) > 0)
			{
				ScenarioJobActions::Import(LocalId, This->PlaceAfterImport.Remove(LocalId) > 0);
				This->RefreshImported();
			}
		});
		SessionHandle = Session->OnChanged.AddSP(this, &SScenarioLibraryPage::OnSessionChanged);
	}

	using namespace ScenarioUI;
	const FName CloudTab = TEXT("cloud");
	const FName LocalTab = TEXT("local");
	const TSharedRef<SWrapBox> Types = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6.0f, 6.0f));
	for (const FTypeFilter& Filter : TypeFilters())
	{
		const FString Id = Filter.Id;
		const bool bCloud = Filter.bCloud;
		const bool bLocalFilter = Filter.bLocal;
		Types->AddSlot()
		[
			SNew(SBox)
			.Visibility_Lambda([this, bCloud, bLocalFilter]() { return (bLocal ? bLocalFilter : bCloud) ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				ChipToggle(Filter.Label, MakeAttributeLambda([this, Id]() { return Kind == Id; }), [this, Id]()
				{
					Kind = Id;
					if (!bLocal && SearchKey() != SearchedKey)
					{
						Search(false);
					}
					else
					{
						ApplyFilter();
					}
				}, Id.IsEmpty() || Id == TEXT("new") ? NAME_None : KindIconName(Id), FText::GetEmpty(), true)
			]
		];
	}

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(PagePadding, PagePadding, PagePadding, 0.0f)
		[
			PageHeader(LOCTEXT("Title", "Library"), MakeAttributeLambda([this]()
			{
				const FScenarioSession* Session = ScenarioUI::Session();
				if (bLocal)
				{
					return LOCTEXT("SubtitleLocal", "Assets in this Unreal project that came from Scenario, with their model, prompt and settings.");
				}
				return Session && Session->GetWorkspace().IsSet()
					? FText::Format(LOCTEXT("SubtitleCloud", "Everything in {0} on Scenario, made in Unreal or anywhere else. Importing is free."), FText::FromString(Session->GetWorkspace().ProjectName))
					: LOCTEXT("SubtitleCloudNone", "Everything in your Scenario project, made in Unreal or anywhere else. Importing is free.");
			}),
			IconButton(TEXT("refresh"), LOCTEXT("RefreshTip", "Search again. Free."), FSimpleDelegate::CreateLambda([this]()
			{
				RefreshImported();
				if (bLocal)
				{
					ApplyFilter();
				}
				else
				{
					Search(false);
				}
			})))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(PagePadding, 10.0f, PagePadding, 0.0f)
		[
			UnderlineTabs({ { CloudTab, LOCTEXT("OnScenario", "On Scenario") }, { LocalTab, LOCTEXT("ImportedHere", "In this project") } },
				MakeAttributeLambda([this, CloudTab, LocalTab]() { return bLocal ? LocalTab : CloudTab; }),
				[this, LocalTab](FName Value) { SetSource(Value == LocalTab); })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(PagePadding, 12.0f, PagePadding, 0.0f)
		[
			SAssignNew(SearchBox, SSearchBox)
			.Style(&Style().GetWidgetStyle<FSearchBoxStyle>("Scenario.Search"))
			.HintText_Lambda([this]()
			{
				if (bLocal)
				{
					return LOCTEXT("SearchLocal", "Search assets imported here...");
				}
				const FScenarioSession* Session = ScenarioUI::Session();
				return Session && Session->GetWorkspace().IsSet()
					? FText::Format(LOCTEXT("SearchProject", "Search {0}..."), FText::FromString(Session->GetWorkspace().ProjectName))
					: LOCTEXT("SearchCloud", "Search Scenario...");
			})
			.OnTextChanged_Lambda([this](const FText& Text)
			{
				Query = Text.ToString();
				if (bLocal)
				{
					ApplyFilter();
				}
				else
				{
					ScheduleSearch();
				}
			})
			.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type Type)
			{
				if (Type == ETextCommit::OnEnter && !bLocal)
				{
					Query = Text.ToString();
					Search(false);
				}
			})
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(PagePadding, 10.0f, PagePadding, 0.0f)
		[
			Types
		]
		// A failed search, with Retry.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(PagePadding, 10.0f, PagePadding, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(Style().GetBrush("Scenario.Card"))
			.Padding(FMargin(12.0f, 8.0f, 8.0f, 8.0f))
			.Visibility_Lambda([this]() { return !bLocal && !Error.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					Icon(TEXT("error"), 16.0f, FSlateColor(Palette::Error()))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock).Font(Font(EText::Body)).AutoWrapText(true).Text_Lambda([this]() { return FText::FromString(Error); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(10.0f, 0.0f, 0.0f, 0.0f)
				[
					Button(LOCTEXT("Retry", "Retry"), EButton::Outline, [this]() { Search(false); }, TEXT("refresh"), LOCTEXT("RetryTip", "Search again. Free."), true)
				]
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(PagePadding - 6.0f, 10.0f, PagePadding - 6.0f, 0.0f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SAssignNew(Tiles, STileView<FItem>)
				.ListItemsSource(&Items)
				.SelectionMode(ESelectionMode::Single)
				.ItemWidth(TileSize + 16.0f)
				.ItemHeight(TileSize + 50.0f)
				.ItemAlignment(EListItemAlignment::LeftAligned)
				.OnGenerateTile(this, &SScenarioLibraryPage::MakeTile)
				.OnSelectionChanged_Lambda([this](FItem Item, ESelectInfo::Type)
				{
					Selected = Item;
					if (FooterBox.IsValid())
					{
						FooterBox->SetContent(BuildFooter());
					}
				})
				.OnMouseButtonDoubleClick_Lambda([this](FItem Item) { Activate(Item); })
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.Padding(SectionGap)
			[
				BuildEmpty()
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		.Padding(0.0f, 6.0f)
		[
			SNew(SBox)
			.Visibility_Lambda([this]() { return !bLocal && bMore && !bLoading ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				Button(LOCTEXT("More", "Load more"), EButton::Secondary, [this]() { Search(true); }, NAME_None, LOCTEXT("MoreTip", "Loads the next 50 assets. Free."), true)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Divider()
		]
		// The selected asset and what to do with it.
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(FooterBox, SBox)
			.MinDesiredHeight(FooterThumbnail + 2.0f * 10.0f)
			.Padding(FMargin(PagePadding, 10.0f))
			.VAlign(VAlign_Center)
			[
				BuildFooter()
			]
		]
	];
	RefreshImported();
	Search(false);
}

SScenarioLibraryPage::~SScenarioLibraryPage()
{
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		Session->OnFilesReady.Remove(FilesHandle);
		Session->OnChanged.Remove(SessionHandle);
	}
}

void SScenarioLibraryPage::SetSource(bool bInLocal)
{
	bLocal = bInLocal;
	const FTypeFilter* Filter = TypeFilters().FindByPredicate([this](const FTypeFilter& Entry) { return Entry.Id == Kind; });
	if (!Filter || (bLocal ? !Filter->bLocal : !Filter->bCloud))
	{
		Kind.Reset();
	}
	Selected.Reset();
	if (bLocal)
	{
		RefreshLocal();
	}
	else if (SearchKey() != SearchedKey && !bLoading)
	{
		Search(false);
	}
	else
	{
		ApplyFilter();
	}
}

FString SScenarioLibraryPage::SearchKey() const
{
	const FScenarioSession* Session = ScenarioUI::Session();
	return FString::Printf(TEXT("%s|%s|%s"), Session ? *Session->Scope() : TEXT(""), *Query, *CloudKind(Kind));
}

void SScenarioLibraryPage::ScheduleSearch()
{
	if (SearchTimer.IsValid())
	{
		UnRegisterActiveTimer(SearchTimer.ToSharedRef());
	}
	SearchTimer = RegisterActiveTimer(0.3f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
	{
		SearchTimer.Reset();
		Search(false);
		return EActiveTimerReturnType::Stop;
	}));
}

void SScenarioLibraryPage::Search(bool bAppend)
{
	if (SearchTimer.IsValid())
	{
		UnRegisterActiveTimer(SearchTimer.ToSharedRef());
		SearchTimer.Reset();
	}
	FScenarioSession* Session = ScenarioUI::Session();
	Error.Reset();
	if (!Session || !Session->IsReady())
	{
		Cloud.Reset();
		SearchedKey.Reset();
		bMore = false;
		ApplyFilter();
		return;
	}
	if (!bAppend)
	{
		Cloud.Reset();
		ApplyFilter();
	}
	SearchedKey = SearchKey();
	const int32 Ticket = ++Generation;
	bLoading = true;
	const TWeakPtr<SScenarioLibraryPage> Weak = SharedThis(this);
	if (Query.IsEmpty())
	{
		// Browsing reads the project itself; typed queries go to the search index.
		Session->Api().ListAssets(CloudKind(Kind), PageSize, bAppend ? NextPageToken : FString(), [Weak, Ticket](const TScenarioResult<FScenarioAssetPage>& Result)
		{
			if (const TSharedPtr<SScenarioLibraryPage> This = Weak.Pin())
			{
				This->AddCloudPage(Ticket, Result.bOk, Result.Error.Message, Result.Value.Assets, !Result.Value.NextPageToken.IsEmpty(), Result.Value.NextPageToken);
			}
		});
		return;
	}
	Session->Api().SearchAssets(Query, CloudKind(Kind), PageSize, bAppend ? Cloud.Num() : 0, [Weak, Ticket](const TScenarioResult<TArray<FScenarioAssetInfo>>& Result)
	{
		if (const TSharedPtr<SScenarioLibraryPage> This = Weak.Pin())
		{
			This->AddCloudPage(Ticket, Result.bOk, Result.Error.Message, Result.Value, Result.Value.Num() == PageSize, FString());
		}
	});
}

void SScenarioLibraryPage::AddCloudPage(int32 Ticket, bool bOk, const FString& ErrorMessage, const TArray<FScenarioAssetInfo>& Assets, bool bMoreAfter, const FString& NextToken)
{
	if (Ticket != Generation)
	{
		return;
	}
	NextPageToken = NextToken;
	bLoading = false;
	if (!bOk)
	{
		Error = ErrorMessage;
		bMore = false;
		ApplyFilter();
		return;
	}
	for (const FScenarioAssetInfo& Asset : Assets)
	{
		if (!IsImportable(Asset.Kind))
		{
			continue;
		}
		const FItem Item = MakeShared<FTile>();
		Item->Asset = Asset;
		Item->AssetId = Asset.AssetId;
		Item->Kind = TileKind(Asset.Kind);
		Item->ModelName = Asset.ModelId.IsEmpty() ? FString() : ScenarioUI::ModelName(Asset.ModelId);
		Item->Title = TitleFor(Asset.Name, Asset.Prompt, Item->Kind, Item->ModelName);
		Item->CreatedAt = Asset.Raw.IsValid() ? ParseTime(ScenarioJson::GetString(Asset.Raw, TEXT("createdAt"))) : 0.0;
		Item->ThumbnailUrl = ThumbnailUrl(Asset);
		Cloud.Add(Item);
	}
	bMore = bMoreAfter;
	ApplyFilter();
}

void SScenarioLibraryPage::OnSessionChanged()
{
	RefreshImported();
	// Signed in, or switched project: search the project now shown.
	const FScenarioSession* Session = ScenarioUI::Session();
	if (!bLocal && !bLoading && !SearchTimer.IsValid() && Session && Session->IsReady() && SearchKey() != SearchedKey)
	{
		Search(false);
	}
}

void SScenarioLibraryPage::RefreshImported()
{
	const UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get();
	if (!Subsystem)
	{
		return;
	}
	const TMap<FString, TMap<FName, FString>>& All = Subsystem->Provenance().All();
	if (All.Num() == LocalCount)
	{
		return;
	}
	LocalCount = All.Num();
	Imported.Reset();
	for (const TPair<FString, TMap<FName, FString>>& Entry : All)
	{
		const FString* Id = Entry.Value.Find(TEXT("Scenario.AssetId"));
		if (Id && !Id->IsEmpty())
		{
			Imported.Add(*Id);
		}
	}
	if (bLocal)
	{
		RefreshLocal();
	}
	else
	{
		ApplyFilter();
	}
}

void SScenarioLibraryPage::RefreshLocal()
{
	Locals.Reset();
	if (const UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get())
	{
		for (const TPair<FString, TMap<FName, FString>>& Entry : Subsystem->Provenance().All())
		{
			auto Get = [&Entry](const TCHAR* Key) { const FString* Found = Entry.Value.Find(Key); return Found ? *Found : FString(); };
			const FItem Item = MakeShared<FTile>();
			Item->bLocal = true;
			Item->ObjectPath = Entry.Key;
			Item->Values = Entry.Value;
			Item->AssetId = Get(TEXT("Scenario.AssetId"));
			Item->Kind = KindForLane(Get(TEXT("Scenario.Lane")));
			const FString ModelId = Get(TEXT("Scenario.ModelId"));
			Item->ModelName = ModelId.IsEmpty() ? Get(TEXT("Scenario.ModelName")) : ScenarioUI::ModelName(ModelId, Get(TEXT("Scenario.ModelName")));
			Item->Title = FPackageName::ObjectPathToObjectName(Entry.Key);
			Item->CreatedAt = ParseTime(Get(TEXT("Scenario.CreatedAt")));
			Locals.Add(Item);
		}
		Locals.Sort([](const FItem& A, const FItem& B) { return A->CreatedAt > B->CreatedAt; });
	}
	ApplyFilter();
}

void SScenarioLibraryPage::ApplyFilter()
{
	Items.Reset();
	for (const FItem& Item : bLocal ? Locals : Cloud)
	{
		if (bLocal)
		{
			if (!Kind.IsEmpty() && Item->Kind != Kind)
			{
				continue;
			}
			const FString* Prompt = Item->Values.Find(TEXT("Scenario.Prompt"));
			if (!Query.IsEmpty() && !Item->Title.Contains(Query) && !Item->ModelName.Contains(Query) && !(Prompt && Prompt->Contains(Query)))
			{
				continue;
			}
		}
		else if (Kind == TEXT("new") && Imported.Contains(Item->AssetId))
		{
			continue;
		}
		Items.Add(Item);
	}
	if (Selected.IsValid() && !Items.Contains(Selected))
	{
		Selected.Reset();
	}
	if (Tiles.IsValid())
	{
		Tiles->RequestListRefresh();
		if (Selected.IsValid())
		{
			Tiles->SetSelection(Selected, ESelectInfo::Direct);
		}
		else
		{
			Tiles->ClearSelection();
		}
	}
	if (FooterBox.IsValid())
	{
		FooterBox->SetContent(BuildFooter());
	}
}

bool SScenarioLibraryPage::IsImporting(const FString& AssetId) const
{
	const FScenarioSession* Session = ScenarioUI::Session();
	for (const TPair<FString, FString>& Pending : PendingImports)
	{
		if (Pending.Value == AssetId && Session && Session->IsDownloading(Pending.Key))
		{
			return true;
		}
	}
	return false;
}

void SScenarioLibraryPage::ImportAsset(const FScenarioAssetInfo& Asset, bool bPlace)
{
	FScenarioSession* Session = ScenarioUI::Session();
	if (!Session || IsImporting(Asset.AssetId))
	{
		return;
	}
	// The asset becomes a recovered job in the list, so it downloads, imports and keeps provenance like any result.
	FScenarioJobInfo Job;
	Job.JobId = Asset.ParentJobId.IsEmpty() ? TEXT("asset:") + Asset.AssetId : Asset.ParentJobId;
	Job.RawStatus = TEXT("success");
	Job.Phase = EScenarioJobPhase::Success;
	Job.ModelId = Asset.ModelId;
	Job.AssetIds = { Asset.AssetId };
	if (!Session->Ledger().FindByJob(Job.JobId))
	{
		Session->Recover(Job, LaneForKind(Asset.Kind));
	}
	FScenarioJobRow* Row = Session->Ledger().FindByJob(Job.JobId);
	if (!Row)
	{
		return;
	}
	if (Row->Prompt.IsEmpty())
	{
		Row->Prompt = Asset.Prompt;
	}
	const FString LocalId = Row->LocalId;
	const bool bOnDisk = Row->Files.Num() > 0 && !Row->Files.ContainsByPredicate([](const FScenarioJobFile& File)
	{
		return File.LocalPath.IsEmpty() || !FPaths::FileExists(File.LocalPath);
	});
	if (bOnDisk)
	{
		ScenarioJobActions::Import(LocalId, bPlace);
		RefreshImported();
		return;
	}
	PendingImports.Add(LocalId, Asset.AssetId);
	if (bPlace)
	{
		PlaceAfterImport.Add(LocalId);
	}
	Session->DownloadFiles(LocalId);
}

void SScenarioLibraryPage::Activate(const FItem& Item)
{
	if (!Item.IsValid())
	{
		return;
	}
	if (Item->bLocal)
	{
		ShowInContentBrowser(Item->ObjectPath);
		return;
	}
	const FString Local = LocalPathFor(Item->AssetId);
	if (!Local.IsEmpty())
	{
		ShowInContentBrowser(Local);
		return;
	}
	ImportAsset(Item->Asset, false);
}

TSharedRef<SWidget> SScenarioLibraryPage::TileThumbnail(const FItem& Item, float Size)
{
	if (Item->bLocal)
	{
		// Imported assets show their real thumbnail, rendered by the editor.
		if (!Item->Thumbnail.IsValid())
		{
			const FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Item->ObjectPath));
			if (Data.IsValid())
			{
				Item->Thumbnail = MakeShared<FAssetThumbnail>(Data, 128, 128, FScenarioThumbnails::Get().AssetPool());
			}
		}
		if (Item->Thumbnail.IsValid())
		{
			FAssetThumbnailConfig Config;
			Config.bAllowFadeIn = false;
			Config.bAllowHintText = false;
			Config.ShowAssetColor = false;
			return SNew(SBox)
				.WidthOverride(Size)
				.HeightOverride(Size)
				[
					Item->Thumbnail->MakeThumbnailWidget(Config)
				];
		}
		return ScenarioUI::Thumbnail(Size, TAttribute<const FSlateBrush*>(nullptr), Item->Kind);
	}
	const FString Key = Item->AssetId;
	const FString Url = Item->ThumbnailUrl;
	return ScenarioUI::Thumbnail(Size, TAttribute<const FSlateBrush*>::CreateLambda([Key, Url]() -> const FSlateBrush*
	{
		return Url.IsEmpty() ? nullptr : FScenarioThumbnails::Get().ForUrl(Key, Url);
	}), Item->Kind);
}

TSharedRef<ITableRow> SScenarioLibraryPage::MakeTile(FItem Item, const TSharedRef<STableViewBase>& Owner)
{
	const FString AssetId = Item->AssetId;
	const bool bCloudTile = !Item->bLocal;
	return SNew(STableRow<FItem>, Owner)
		.Style(&ScenarioUI::Style().GetWidgetStyle<FTableRowStyle>("Scenario.TableRow"))
		.Padding(FMargin(6.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					TileThumbnail(Item, TileSize)
				]
				// The kind, bottom right.
				+ SOverlay::Slot()
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Bottom)
				.Padding(4.0f)
				[
					SNew(SBorder)
					.BorderImage(ScenarioUI::Style().GetBrush("Scenario.Overlay"))
					.Padding(3.0f)
					.Visibility(EVisibility::HitTestInvisible)
					[
						SNew(SImage)
						.Image(ScenarioUI::KindIcon(Item->Kind))
						.DesiredSizeOverride(FVector2D(12.0f, 12.0f))
					]
				]
				// Already in this project, top right.
				+ SOverlay::Slot()
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Top)
				.Padding(4.0f)
				[
					ShownWhen(SNew(SBorder)
						.BorderImage(ScenarioUI::Style().GetBrush("Scenario.Overlay"))
						.Padding(3.0f)
						.ToolTipText(LOCTEXT("InProjectTip", "Imported into this project already."))
						[
							SNew(SImage)
							.Image(ScenarioUI::IconBrush(TEXT("check")))
							.DesiredSizeOverride(FVector2D(12.0f, 12.0f))
							.ColorAndOpacity(ScenarioUI::Palette::Success())
						],
						[this, AssetId, bCloudTile]() { return bCloudTile && Imported.Contains(AssetId); })
				]
				// Importing, bottom left.
				+ SOverlay::Slot()
				.HAlign(HAlign_Left)
				.VAlign(VAlign_Bottom)
				.Padding(4.0f)
				[
					ShownWhen(ScenarioUI::Pill(LOCTEXT("Importing", "Importing..."), ScenarioUI::ETone::Info),
						[this, AssetId, bCloudTile]() { return bCloudTile && IsImporting(AssetId); })
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f, 6.0f, 2.0f, 0.0f)
			[
				ScenarioUI::Line(FText::FromString(Item->Title))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f, 1.0f, 2.0f, 0.0f)
			[
				ScenarioUI::Line(FText::FromString(Item->ModelName), ScenarioUI::EText::Meta, true)
			]
		];
}

TSharedRef<SWidget> SScenarioLibraryPage::BuildEmpty()
{
	return SNew(SVerticalBox)
		.Visibility_Lambda([this]() { return Items.Num() == 0 && (bLocal || Error.IsEmpty()) ? EVisibility::Visible : EVisibility::Collapsed; })
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		.Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			ScenarioUI::Icon(TEXT("library"), 28.0f, FSlateColor(ScenarioUI::Palette::Faint()))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				const FScenarioSession* Session = ScenarioUI::Session();
				if (!bLocal && (!Session || !Session->IsReady()))
				{
					return LOCTEXT("SignIn", "Sign in and choose a project to browse its assets.");
				}
				if (!bLocal && bLoading)
				{
					return LOCTEXT("Searching", "Searching Scenario...");
				}
				if (!Query.IsEmpty())
				{
					return FText::Format(LOCTEXT("NoMatch", "Nothing matches \"{0}\"."), FText::FromString(Query));
				}
				if (Kind == TEXT("new"))
				{
					return LOCTEXT("AllImported", "Everything shown here is imported already. Load more, or search.");
				}
				if (!Kind.IsEmpty())
				{
					return LOCTEXT("NoType", "No asset of this type here.");
				}
				return bLocal ? LOCTEXT("NoLocal", "Nothing in this Unreal project came from Scenario yet.")
					: LOCTEXT("NoCloud", "No assets in this Scenario project yet. Generate something in Create.");
			})
			.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
			.ColorAndOpacity(FSlateColor(ScenarioUI::Palette::Dim()))
			.Justification(ETextJustify::Center)
			.AutoWrapText(true)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		.Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>(ScenarioUI::ButtonStyleName(ScenarioUI::EButton::Secondary)))
			.Text(LOCTEXT("ClearSearch", "Clear search"))
			.Visibility_Lambda([this]() { return Query.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.OnClicked_Lambda([this]()
			{
				SearchBox->SetText(FText::GetEmpty());
				Query.Reset();
				if (bLocal)
				{
					ApplyFilter();
				}
				else
				{
					Search(false);
				}
				return FReply::Handled();
			})
		];
}

TSharedRef<SWidget> SScenarioLibraryPage::BuildFooter()
{
	if (!Selected.IsValid())
	{
		return SNew(STextBlock)
			.Text(LOCTEXT("SelectHint", "Select an asset to import it, show it or send it to another model. Double-click imports or shows it."))
			.Font(ScenarioUI::Font(ScenarioUI::EText::Meta))
			.ColorAndOpacity(FSlateColor(ScenarioUI::Palette::Dim()))
			.AutoWrapText(true);
	}
	const FItem Item = Selected;
	TArray<FString> Facts = { KindName(Item->Kind).ToString() };
	if (!Item->ModelName.IsEmpty())
	{
		Facts.Add(Item->ModelName);
	}
	if (Item->CreatedAt > 0.0)
	{
		Facts.Add(ScenarioUI::RelativeTime(Item->CreatedAt).ToString());
	}
	if (Item->bLocal)
	{
		Facts.Add(ScenarioUI::ShortPath(FPackageName::GetLongPackagePath(FPackageName::ObjectPathToPackageName(Item->ObjectPath))));
	}

	const TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 10.0f, 0.0f)
		[
			TileThumbnail(Item, FooterThumbnail)
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				ScenarioUI::Line(FText::FromString(Item->Title), ScenarioUI::EText::Title)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				ScenarioUI::Line(FText::FromString(FString::Join(Facts, TEXT(" · "))), ScenarioUI::EText::Meta, true)
			]
		];

	// Send to: the Create form first, then the next steps for this kind.
	if (!Item->AssetId.IsEmpty())
	{
		Row->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
		[
			SNew(SComboButton)
			.ComboButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FComboButtonStyle>("Scenario.Combo"))
			.ButtonContent()
			[
				SNew(STextBlock).Text(LOCTEXT("SendTo", "Send to")).Font(ScenarioUI::Font(ScenarioUI::EText::Body))
			]
			.ToolTipText(LOCTEXT("SendToTip", "Use this asset as the input of another model. Nothing is sent until you generate."))
			.OnGetMenuContent_Lambda([Item]()
			{
				const FString Step = StepKind(Item->Kind);
				FMenuBuilder Menu(true, nullptr);
				Menu.AddMenuEntry(LOCTEXT("UseReference", "Use as reference in Create"),
					LOCTEXT("UseReferenceTip", "Adds this asset to the Create form. Nothing is uploaded again."),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"),
					FUIAction(FExecuteAction::CreateLambda([Item, Step]()
					{
						FScenarioCreateModel::Get()->UseReference(Item->AssetId, Step, Item->Title);
						FScenarioEditorModule::OpenStudio(ScenarioPages::Create);
					})));
				Menu.AddSeparator();
				ScenarioJobActions::AddNextStepsMenu(Menu, Item->AssetId, Step, Item->Title);
				return Menu.MakeWidget();
			})
		];
	}

	if (Item->bLocal)
	{
		const FString Lane = Item->Values.FindRef(TEXT("Scenario.Lane"));
		const FString ModelId = Item->Values.FindRef(TEXT("Scenario.ModelId"));
		const FString Parameters = Item->Values.FindRef(TEXT("Scenario.Parameters"));
		Row->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>(ScenarioUI::ButtonStyleName(ScenarioUI::EButton::Secondary)))
			.Text(LOCTEXT("Reuse", "Reuse settings"))
			.ToolTipText(LOCTEXT("ReuseTip", "Fills the Create form with the model and settings that made this asset. Sends nothing."))
			.IsEnabled(!ModelId.IsEmpty())
			.OnClicked_Lambda([Lane, ModelId, Parameters]()
			{
				FScenarioEditorModule::OpenCreate(Lane, ModelId, ScenarioJson::ParseObject(Parameters));
				return FReply::Handled();
			})
		];
		Row->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>(ScenarioUI::ButtonStyleName(ScenarioUI::EButton::Primary, true)))
			.Text(LOCTEXT("Show", "Show"))
			.ToolTipText(LOCTEXT("ShowTip", "Shows the asset in the Content Browser."))
			.OnClicked_Lambda([Item]() { ShowInContentBrowser(Item->ObjectPath); return FReply::Handled(); })
		];
		return Row;
	}

	// On Scenario: more import options, then Import (or Show once it is here).
	Row->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(ScenarioUI::RowGap, 0.0f, 0.0f, 0.0f)
	[
		SNew(SComboButton)
		.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>(ScenarioUI::ButtonStyleName(ScenarioUI::EButton::Ghost)))
		.HasDownArrow(false)
		.ButtonContent()
		[
			ScenarioUI::Icon(TEXT("more"), 16.0f, FSlateColor(ScenarioUI::Palette::Dim()))
		]
		.ToolTipText(LOCTEXT("MoreActionsTip", "More actions for this asset."))
		.OnGetMenuContent_Lambda([this, Item]()
		{
			FMenuBuilder Menu(true, nullptr);
			Menu.AddMenuEntry(LOCTEXT("ImportPlace", "Import and place"),
				LOCTEXT("ImportPlaceTip", "Downloads the asset, imports it and places it in front of the camera. Free."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.PlaceActors"),
				FUIAction(FExecuteAction::CreateLambda([this, Item]() { ImportAsset(Item->Asset, true); }),
					FCanExecuteAction::CreateLambda([this, Item]() { return !IsImporting(Item->AssetId); })));
			Menu.AddMenuEntry(LOCTEXT("CopyId", "Copy asset id"),
				LOCTEXT("CopyIdTip", "Copies the Scenario asset id, for scripts and support."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Clipboard"),
				FUIAction(FExecuteAction::CreateLambda([Item]() { FPlatformApplicationMisc::ClipboardCopy(*Item->AssetId); })));
			return Menu.MakeWidget();
		})
	];
	Row->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(ScenarioUI::Gutter, 0.0f, 0.0f, 0.0f)
	[
		SNew(SButton)
		.ButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FButtonStyle>(ScenarioUI::ButtonStyleName(ScenarioUI::EButton::Primary, true)))
		.Text_Lambda([this, Item]()
		{
			if (IsImporting(Item->AssetId))
			{
				return LOCTEXT("ImportingButton", "Importing...");
			}
			return Imported.Contains(Item->AssetId) ? LOCTEXT("ShowImported", "Show") : LOCTEXT("Import", "Import");
		})
		.ToolTipText_Lambda([this, Item]()
		{
			return Imported.Contains(Item->AssetId)
				? LOCTEXT("ShowImportedTip", "Already in this project: shows it in the Content Browser.")
				: LOCTEXT("ImportTip", "Downloads this asset and imports it into the project, with its provenance. Free.");
		})
		.IsEnabled_Lambda([this, Item]() { return !IsImporting(Item->AssetId); })
		.OnClicked_Lambda([this, Item]() { Activate(Item); return FReply::Handled(); })
	];
	return Row;
}

#undef LOCTEXT_NAMESPACE
