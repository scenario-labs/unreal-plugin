// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ScenarioActions.h"
#include "ScenarioJson.h"
#include "ScenarioSubsystem.h"

/** Shared by the action files: argument readers, schema builders and the state actions keep between calls. */
namespace ScenarioActionHelpers
{
	/** Builds an input schema: Object({Prop("name", "string", "what"), ...}, {"required"}). */
	struct FProp
	{
		FString Name;
		FString Type;
		FString Description;
		/** For arrays: the item type. For strings: allowed values. */
		FString Items;
		TArray<FString> Enum;
	};
	TSharedPtr<FJsonObject> Object(const TArray<FProp>& Props, const TArray<FString>& Required = {});

	FString Str(const TSharedPtr<FJsonObject>& Args, const FString& Name, const FString& Default = FString());
	int32 Int(const TSharedPtr<FJsonObject>& Args, const FString& Name, int32 Default, int32 Min, int32 Max);
	bool Bool(const TSharedPtr<FJsonObject>& Args, const FString& Name, bool Default = false);
	TArray<FString> Strings(const TSharedPtr<FJsonObject>& Args, const FString& Name);
	TSharedPtr<FJsonObject> Obj(const TSharedPtr<FJsonObject>& Args, const FString& Name);

	/** The subsystem and a ready session, or a failed result to return. */
	UScenarioSubsystem* Subsystem(FScenarioActionResult& OutFail, bool bNeedsReady);

	/** One ledger row, flat, for job_status and lists. */
	TSharedRef<FJsonObject> JobJson(const FScenarioJobRow& Row, bool bDetail);
	/** True when a finished job has every file on disk (or a download error). */
	bool IsDelivered(const FScenarioJobRow& Row);

	/** Project-relative form of a path for results (no paths outside the project). */
	FString ProjectRelative(const FString& Path);

	TSharedRef<FJsonObject> FieldJson(const FScenarioField& Field);

	/** Uploads started by upload_reference, by handle. */
	struct FUpload
	{
		FString Handle;
		FString Path;
		FString State = TEXT("uploading");
		FString AssetId;
		FString Error;
		float Progress = 0.0f;
	};
	TMap<FString, FUpload>& Uploads();

	/** Plans made by prepare_result_application, applied once by apply_result_application. */
	struct FApplication
	{
		FString Id;
		FString LocalId;
		FString Purpose;
		FScenarioPlaceOptions Options;
		FString Review;
		double CreatedAt = 0.0;
		bool bApplied = false;
	};
	TMap<FString, FApplication>& Applications();

	/** Files captured or exported for agents, by reference id. */
	TMap<FString, FString>& References();

	/** A paid call waiting for the person (FScenarioCaller::bSpendWaitsForApproval), by approval id. */
	struct FSpendApproval
	{
		FString Id;
		FString Action;
		FString QuoteId;
		FString CostExact;
		FString Target;
		/** pending, declined, expired, sending, sent or failed. */
		FString State = TEXT("pending");
		/** Local job id once sent. */
		FString JobId;
		FString Error;
		double CreatedAt = 0.0;
		double Deadline = 0.0;
		/** Closes the editor's question as declined. */
		TFunction<void()> Decline;
	};
	TMap<FString, FSpendApproval>& SpendApprovals();
	/** The approval's state for job_status: the job once sent, else the approval itself. */
	TSharedRef<FJsonObject> ApprovalJson(const FSpendApproval& Approval);

	/**
	 * Asks the person in the editor (toast with two buttons). Answers false after Timeout seconds.
	 * Returns a function that answers false now (for shutdown and tests); it does nothing once answered.
	 */
	TFunction<void()> AskPerson(const FText& Title, const FText& Detail, double Timeout, TFunction<void(bool bApproved)> OnAnswer);

	FString NewId(const TCHAR* Prefix);
}
