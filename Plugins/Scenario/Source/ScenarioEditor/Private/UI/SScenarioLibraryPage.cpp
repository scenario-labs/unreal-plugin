// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "ContentBrowserModule.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IContentBrowserSingleton.h"
#include "ScenarioCreateModel.h"
#include "ScenarioEditorModule.h"
#include "ScenarioJson.h"
#include "ScenarioProvenance.h"
#include "ScenarioSession.h"
#include "ScenarioStyle.h"
#include "ScenarioSubsystem.h"
#include "ScenarioThumbnails.h"
#include "SScenarioJobCard.h"
#include "SScenarioPages.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
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

	TSharedRef<SWidget> KindToggle(const FText& Label, TFunction<bool()> IsOn, TFunction<void()> OnClick)
	{
		return SNew(SBox)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.IsChecked_Lambda([IsOn]() { return IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([OnClick](ECheckBoxState) { OnClick(); })
				[
					SNew(SBox).Padding(FMargin(8.0f, 2.0f))[SNew(STextBlock).Text(Label)]
				]
			];
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
				ScenarioJobActions::Import(LocalId, false);
			}
		});
	}

	const TArray<TPair<FString, FText>> Kinds = {
		{ FString(), LOCTEXT("AllKinds", "All") },
		{ TEXT("image"), LOCTEXT("Images", "Images") },
		{ TEXT("3d"), LOCTEXT("Meshes", "3D") },
		{ TEXT("video"), LOCTEXT("Videos", "Video") },
		{ TEXT("audio"), LOCTEXT("Sounds", "Audio") },
	};
	TSharedRef<SHorizontalBox> KindRow = SNew(SHorizontalBox);
	for (const TPair<FString, FText>& Entry : Kinds)
	{
		const FString Id = Entry.Key;
		KindRow->AddSlot().AutoWidth()[KindToggle(Entry.Value, [this, Id]() { return Kind == Id; }, [this, Id]() { Kind = Id; Search(false); })];
	}

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				KindToggle(LOCTEXT("Cloud", "Scenario project"), [this]() { return !bLocal; }, [this]() { bLocal = false; })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				KindToggle(LOCTEXT("Local", "In this project"), [this]() { return bLocal; }, [this]() { bLocal = true; RefreshLocal(); })
			]
		]
		// Scenario project
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SVerticalBox)
			.Visibility_Lambda([this]() { return bLocal ? EVisibility::Collapsed : EVisibility::Visible; })
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f, 0.0f, 8.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SSearchBox)
					.HintText(LOCTEXT("Search", "Search your Scenario project..."))
					.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type) { Query = Text.ToString(); Search(false); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					KindRow
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return bLoading ? LOCTEXT("Searching", "Searching...") : FText::FromString(Status); })
				.ColorAndOpacity(FScenarioStyle::Muted())
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(AssetList, SListView<FAsset>)
				.ListItemsSource(&Assets)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SScenarioLibraryPage::MakeAssetRow)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(4.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("More", "Load more"))
				.Visibility_Lambda([this]() { return Assets.Num() > 0 && Assets.Num() % 50 == 0 ? EVisibility::Visible : EVisibility::Collapsed; })
				.OnClicked_Lambda([this]() { Search(true); return FReply::Handled(); })
			]
		]
		// In this project
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SVerticalBox)
			.Visibility_Lambda([this]() { return bLocal ? EVisibility::Visible : EVisibility::Collapsed; })
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f, 0.0f, 8.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					return Locals.Num() == 0
						? LOCTEXT("NoLocal", "No asset in this project was generated with Scenario yet.")
						: FText::Format(LOCTEXT("LocalCount", "{0} assets generated with Scenario. Each keeps its model, prompt and job id."), FText::AsNumber(Locals.Num()));
				})
				.ColorAndOpacity(FScenarioStyle::Muted())
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(LocalList, SListView<FLocal>)
				.ListItemsSource(&Locals)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SScenarioLibraryPage::MakeLocalRow)
			]
		]
	];
	Search(false);
}

SScenarioLibraryPage::~SScenarioLibraryPage()
{
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		Session->OnFilesReady.Remove(FilesHandle);
	}
}

void SScenarioLibraryPage::Search(bool bMore)
{
	FScenarioSession* Session = ScenarioUI::Session();
	if (!Session || !Session->IsReady())
	{
		Status = LOCTEXT("SignIn", "Sign in and choose a project to browse its assets.").ToString();
		return;
	}
	if (!bMore)
	{
		Assets.Reset();
		AssetList->RequestListRefresh();
	}
	const int32 Ticket = ++Generation;
	bLoading = true;
	const TWeakPtr<SScenarioLibraryPage> Weak = SharedThis(this);
	Session->Api().SearchAssets(Query, Kind, 50, bMore ? Assets.Num() : 0, [Weak, Ticket](const TScenarioResult<TArray<FScenarioAssetInfo>>& Result)
	{
		const TSharedPtr<SScenarioLibraryPage> This = Weak.Pin();
		if (!This.IsValid() || Ticket != This->Generation)
		{
			return;
		}
		This->bLoading = false;
		if (!Result.bOk)
		{
			This->Status = Result.Error.Message;
			return;
		}
		for (const FScenarioAssetInfo& Asset : Result.Value)
		{
			This->Assets.Add(MakeShared<FScenarioAssetInfo>(Asset));
		}
		This->Status = This->Assets.Num() == 0 ? LOCTEXT("Nothing", "Nothing found.").ToString() : FString();
		This->AssetList->RequestListRefresh();
	});
}

void SScenarioLibraryPage::RefreshLocal()
{
	Locals.Reset();
	if (UScenarioSubsystem* Subsystem = UScenarioSubsystem::Get())
	{
		for (const TPair<FString, TMap<FName, FString>>& Entry : Subsystem->Provenance().All())
		{
			Locals.Add(MakeShared<TPair<FString, TMap<FName, FString>>>(Entry));
		}
		Locals.Sort([](const FLocal& A, const FLocal& B)
		{
			const FString* CA = A->Value.Find(TEXT("Scenario.CreatedAt"));
			const FString* CB = B->Value.Find(TEXT("Scenario.CreatedAt"));
			return (CA ? *CA : FString()) > (CB ? *CB : FString());
		});
	}
	if (LocalList.IsValid())
	{
		LocalList->RequestListRefresh();
	}
}

void SScenarioLibraryPage::ImportAsset(const FScenarioAssetInfo& Asset)
{
	FScenarioSession* Session = ScenarioUI::Session();
	if (!Session)
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
	if (const FScenarioJobRow* Existing = Session->Ledger().FindByJob(Job.JobId))
	{
		if (Existing->bImported)
		{
			FScenarioEditorModule::Toast(LOCTEXT("Already", "Already in this project"), FText::FromString(Asset.Name), true);
			return;
		}
	}
	else
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
		ScenarioJobActions::Import(LocalId, false);
		return;
	}
	PendingImports.Add(LocalId);
	Session->DownloadFiles(LocalId);
}

TSharedRef<ITableRow> SScenarioLibraryPage::MakeAssetRow(FAsset Item, const TSharedRef<STableViewBase>& Owner)
{
	const FScenarioAssetInfo Asset = *Item;
	const FString Url = ThumbnailUrl(Asset);
	const FString Label = Asset.Name.IsEmpty() ? Asset.Prompt.Left(40) : Asset.Name;
	const FString KindName = Asset.Kind.StartsWith(TEXT("image")) ? TEXT("image") : Asset.Kind;
	return SNew(STableRow<FAsset>, Owner)
		.Padding(FMargin(8.0f, 4.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(56.0f)
				.HeightOverride(56.0f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock).Text(FText::FromString(KindName.ToUpper())).Font(FScenarioStyle::Font(7, true)).ColorAndOpacity(FScenarioStyle::Muted())
						]
					]
					+ SOverlay::Slot()
					[
						SNew(SImage)
						.Image_Lambda([Asset, Url]() { return FScenarioThumbnails::Get().ForUrl(Asset.AssetId, Url); })
						.Visibility_Lambda([Asset, Url]() { return FScenarioThumbnails::Get().ForUrl(Asset.AssetId, Url) ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
					]
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock).Text(FText::FromString(Label.IsEmpty() ? Asset.AssetId : Label))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("%s%s%s"), *Asset.Kind, Asset.ModelId.IsEmpty() ? TEXT("") : TEXT(" · "), *Asset.ModelId)))
					.ColorAndOpacity(FScenarioStyle::Muted())
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Import", "Import"))
				.ToolTipText(LOCTEXT("ImportTip", "Downloads this asset and imports it into the project, with its provenance. Free."))
				.OnClicked_Lambda([this, Asset]() { ImportAsset(Asset); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Reference", "Use as reference"))
				.ToolTipText(LOCTEXT("ReferenceTip", "Adds this asset to the Create form as a reference. Nothing is uploaded again."))
				.OnClicked_Lambda([Asset, KindName, Label]()
				{
					FScenarioCreateModel::Get()->UseReference(Asset.AssetId, KindName, Label);
					FScenarioEditorModule::OpenStudio(ScenarioPages::Create);
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SComboButton)
				.ButtonContent()[SNew(STextBlock).Text(LOCTEXT("Next", "Next"))]
				.OnGetMenuContent_Lambda([Asset, KindName, Label]()
				{
					FMenuBuilder Menu(true, nullptr);
					ScenarioJobActions::AddNextStepsMenu(Menu, Asset.AssetId, KindName, Label);
					return Menu.MakeWidget();
				})
			]
		];
}

TSharedRef<ITableRow> SScenarioLibraryPage::MakeLocalRow(FLocal Item, const TSharedRef<STableViewBase>& Owner)
{
	const FString ObjectPath = Item->Key;
	const TMap<FName, FString> Values = Item->Value;
	auto Get = [&Values](const TCHAR* Key) { const FString* Found = Values.Find(Key); return Found ? *Found : FString(); };
	const FString Model = Get(TEXT("Scenario.ModelName")).IsEmpty() ? Get(TEXT("Scenario.ModelId")) : Get(TEXT("Scenario.ModelName"));
	const FString Prompt = Get(TEXT("Scenario.Prompt"));
	const FString Created = Get(TEXT("Scenario.CreatedAt")).Left(10);
	const FString AssetId = Get(TEXT("Scenario.AssetId"));
	const FString Lane = Get(TEXT("Scenario.Lane"));
	const FString ModelId = Get(TEXT("Scenario.ModelId"));
	const FString Parameters = Get(TEXT("Scenario.Parameters"));
	const FString KindName = Lane == TEXT("3d") || Lane == TEXT("video") || Lane == TEXT("audio") ? Lane : TEXT("image");
	return SNew(STableRow<FLocal>, Owner)
		.Padding(FMargin(8.0f, 4.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock).Text(FText::FromString(FPackageName::ObjectPathToObjectName(ObjectPath)))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("%s · %s%s%s"), *Model, *Created, Prompt.IsEmpty() ? TEXT("") : TEXT(" · "), *Prompt.Left(80))))
					.ColorAndOpacity(FScenarioStyle::Muted())
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Browse", "Browse"))
				.ToolTipText(LOCTEXT("BrowseTip", "Shows the asset in the Content Browser."))
				.OnClicked_Lambda([ObjectPath]()
				{
					// Registry lookup: shows the asset without loading it.
					const FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
					if (Data.IsValid())
					{
						FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(TArray<FAssetData>{ Data });
					}
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("UseSettings", "Use settings"))
				.ToolTipText(LOCTEXT("UseSettingsTip", "Fills the Create form with the model and settings that made this asset. Sends nothing."))
				.IsEnabled(!ModelId.IsEmpty())
				.OnClicked_Lambda([Lane, ModelId, Parameters]()
				{
					FScenarioEditorModule::OpenCreate(Lane, ModelId, ScenarioJson::ParseObject(Parameters));
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SComboButton)
				.IsEnabled(!AssetId.IsEmpty())
				.ButtonContent()[SNew(STextBlock).Text(LOCTEXT("NextLocal", "Next"))]
				.OnGetMenuContent_Lambda([AssetId, KindName, ObjectPath]()
				{
					FMenuBuilder Menu(true, nullptr);
					ScenarioJobActions::AddNextStepsMenu(Menu, AssetId, KindName, FPackageName::ObjectPathToObjectName(ObjectPath));
					return Menu.MakeWidget();
				})
			]
		];
}

#undef LOCTEXT_NAMESPACE
