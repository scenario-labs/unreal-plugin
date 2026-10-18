// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

enum class EScenarioQuoteKind : uint8
{
	Model,
	Workflow,
	PromptSpark,
};

struct SCENARIOCORE_API FScenarioQuote
{
	/** Short id handed to agents ("q_..."). */
	FString Id;
	FString Fingerprint;
	EScenarioQuoteKind Kind = EScenarioQuoteKind::Model;
	/** Model or workflow id. */
	FString TargetId;
	FString Scope;
	TSharedPtr<FJsonObject> Parameters;
	double CU = 0.0;
	double Discount = 0.0;
	FString Exact;
	double CreatedAt = 0.0;

	double ExpiresAt() const;
};

/**
 * Single-use price quotes bound to the exact request.
 *
 * A paid call is allowed only with a quote whose fingerprint (target, scope
 * and canonical parameters) matches the request now. Sending consumes it; any
 * edit gives a different fingerprint, so a stale price is never charged.
 */
class SCENARIOCORE_API FScenarioQuotes
{
public:
	static constexpr double MaxAgeSeconds = 120.0;

	/** SHA-256 of "<target>\n<scope>\n<canonical JSON>". */
	static FString Fingerprint(const FString& TargetId, const TSharedPtr<FJsonObject>& Parameters, const FString& Scope);

	/** Scope string for a workspace: "team/project", or "key" for API keys. */
	static FString WorkspaceScope(const FString& TeamId, const FString& ProjectId);

	const FScenarioQuote& Store(EScenarioQuoteKind Kind, const FString& TargetId, const TSharedPtr<FJsonObject>& Parameters, const FString& Scope, double CU, double Discount, double Now);

	/** The fresh quote for this fingerprint, or null. */
	const FScenarioQuote* Usable(const FString& Fingerprint, double Now) const;
	const FScenarioQuote* FindById(const FString& QuoteId, double Now) const;

	/** Takes the quote out (single use). False when there is none fresh to take. */
	bool Consume(const FString& Fingerprint, double Now, FScenarioQuote& Out);
	bool ConsumeById(const FString& QuoteId, double Now, FScenarioQuote& Out);

	void Clear();
	int32 Num() const { return Quotes.Num(); }

private:
	void Prune(double Now);
	TMap<FString, FScenarioQuote> Quotes;
};
