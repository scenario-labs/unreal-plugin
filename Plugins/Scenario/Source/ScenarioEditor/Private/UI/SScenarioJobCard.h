// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioLedger.h"
#include "Styling/SlateColor.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FMenuBuilder;
class SBox;

/** Actions on jobs and outputs shared by Create's Recent strip, Jobs, the composer and menus. */
namespace ScenarioJobActions
{
	struct FNextStep
	{
		FText Label;
		FText Tooltip;
		FString Lane;
		FString Mode;
		/** When set, opens the model picker on this chip instead of a lane. */
		FString Chip;
		FString Modality;
	};

	/** What can follow an output of this kind (image, 3d, video, audio). */
	TArray<FNextStep> NextSteps(const FString& Kind);
	/** Opens Create (or the picker) with the asset as the reference. Sends nothing. */
	void RunNextStep(const FNextStep& Step, const FString& AssetId, const FString& Kind, const FString& Label);
	void AddNextStepsMenu(FMenuBuilder& Menu, const FString& AssetId, const FString& Kind, const FString& Label);

	/** Where a job stands, as the person sees it. */
	enum class EView : uint8
	{
		Sending,
		Queued,
		Running,
		Downloading,
		Importing,
		Ready,
		Imported,
		DownloadStopped,
		Failed,
		Blocked,
		Unknown,
		Canceling,
		Canceled,
		NotSent,
		Hidden,
	};
	EView View(const FScenarioJobRow& Row);

	/** The full sentence for a job's state ("Failed: <reason>", "We do not know if Scenario received this request."). */
	FText StateText(const FScenarioJobRow& Row);
	/** The short chip label ("Generating 42%", "Ready", "Not confirmed"). */
	FText ChipText(const FScenarioJobRow& Row);
	FSlateColor ChipColor(const FScenarioJobRow& Row);
	/** A live status chip for a job. */
	TSharedRef<SWidget> StateChip(const FString& LocalId);

	/** "30 CU" once charged, "~30" while only quoted. */
	FText CostText(const FScenarioJobRow& Row);
	FText CostTooltip(const FScenarioJobRow& Row);
	/** The model or workflow's display name. */
	FString ModelLabel(const FScenarioJobRow& Row);
	/** "Model · prompt". */
	FString Title(const FScenarioJobRow& Row);
	/** The output kind of a job's first file (image when unknown), for next steps. */
	FString OutputKind(const FScenarioJobRow& Row);
	/** The kind to draw: image, 3d, material, sky, audio, video. */
	FString DisplayKind(const FScenarioJobRow& Row);
	/** A downloaded image that shows the result: never a normal, roughness or height map. */
	FString PreviewFile(const FScenarioJobRow& Row);
	/** The imported asset that best shows the result (the mesh before its textures), or empty. */
	FString PrimaryImportedObject(const FScenarioJobRow& Row);

	/** The one action that fits the state (Import and place, Show, Cancel, Check...). */
	struct FRowAction
	{
		FText Label;
		FText Tooltip;
		FName Icon;
		TFunction<void()> Run;
		bool IsSet() const { return static_cast<bool>(Run); }
	};
	FRowAction PrimaryAction(const FScenarioJobRow& Row);

	void Import(const FString& LocalId, bool bPlace);
	/** Selects the job's imported assets in the Content Browser. */
	void Show(const FString& LocalId);
	/** Fills the Create form with the job's model and settings. Sends nothing. */
	void UseSettings(const FString& LocalId);
	void ShowDetails(const FString& LocalId);
	/** Looks for Unknown requests on Scenario. Free. */
	void CheckUnresolved();
	/** The overflow menu of a job: what fits its state beyond the row action. */
	TSharedRef<class SWidget> MoreMenu(const FString& LocalId);
}

/** A job's picture: the imported asset's thumbnail, else a preview image, else the class thumbnail of its kind. */
class SScenarioJobThumb : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioJobThumb)
		: _Size(40.0f)
		, _bProgress(false)
	{}
		SLATE_ARGUMENT(FString, LocalId)
		SLATE_ARGUMENT(float, Size)
		/** A 3 px progress bar on the bottom edge while the job runs. */
		SLATE_ARGUMENT(bool, bProgress)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime) override;

private:
	void Rebuild();

	FString LocalId;
	float Size = 40.0f;
	TSharedPtr<SBox> AssetBox;
	/** The asset shown, "none" when the row says imported but its assets are not found yet. */
	FString ShownObject;
	double RetryAt = 0.0;
	TSharedPtr<class FAssetThumbnail> AssetThumbnail;
};

/** One job in the Jobs list: 52 px, thumbnail, title, status chip, cost, one action and an overflow menu. */
class SScenarioJobRow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioJobRow) {}
		SLATE_ARGUMENT(FString, LocalId)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FString LocalId;
};

/** One job in Create's Recent strip: a 56 px tile with its state under it. */
class SScenarioJobTile : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioJobTile) {}
		SLATE_ARGUMENT(FString, LocalId)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& Geometry, const FPointerEvent& MouseEvent) override;

private:
	FString LocalId;
};

/** One line for the composer: thumbnail, title, chip and the row action. */
class SScenarioJobCard : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioJobCard) {}
		SLATE_ARGUMENT(FString, LocalId)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FString LocalId;
};
