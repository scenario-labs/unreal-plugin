// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioQuotes.h"

#include "ScenarioCU.h"
#include "ScenarioHash.h"
#include "ScenarioJson.h"

double FScenarioQuote::ExpiresAt() const
{
	return CreatedAt + FScenarioQuotes::MaxAgeSeconds;
}

FString FScenarioQuotes::Fingerprint(const FString& TargetId, const TSharedPtr<FJsonObject>& Parameters, const FString& Scope)
{
	const FString Canonical = ScenarioJson::Canonical(Parameters.IsValid() ? Parameters : MakeShared<FJsonObject>());
	return ScenarioHash::Sha256Hex(TargetId + TEXT("\n") + Scope + TEXT("\n") + Canonical);
}

FString FScenarioQuotes::WorkspaceScope(const FString& TeamId, const FString& ProjectId)
{
	if (TeamId.IsEmpty() && ProjectId.IsEmpty())
	{
		return TEXT("key");
	}
	return TeamId + TEXT("/") + ProjectId;
}

const FScenarioQuote& FScenarioQuotes::Store(EScenarioQuoteKind Kind, const FString& TargetId, const TSharedPtr<FJsonObject>& Parameters, const FString& Scope, double CU, double Discount, double Now)
{
	Prune(Now);
	FScenarioQuote Quote;
	Quote.Kind = Kind;
	Quote.TargetId = TargetId;
	Quote.Scope = Scope;
	Quote.Parameters = ScenarioJson::CloneObject(Parameters.IsValid() ? Parameters : MakeShared<FJsonObject>());
	Quote.Fingerprint = Fingerprint(TargetId, Quote.Parameters, Scope);
	Quote.Id = TEXT("q_") + ScenarioHash::RandomToken(9);
	Quote.CU = CU;
	Quote.Discount = Discount;
	Quote.Exact = ScenarioCU::Exact(CU);
	Quote.CreatedAt = Now;
	return Quotes.Add(Quote.Fingerprint, MoveTemp(Quote));
}

const FScenarioQuote* FScenarioQuotes::Usable(const FString& InFingerprint, double Now) const
{
	const FScenarioQuote* Quote = Quotes.Find(InFingerprint);
	if (!Quote || Now - Quote->CreatedAt > MaxAgeSeconds || Now < Quote->CreatedAt - 5.0)
	{
		return nullptr;
	}
	return Quote;
}

const FScenarioQuote* FScenarioQuotes::FindById(const FString& QuoteId, double Now) const
{
	for (const TPair<FString, FScenarioQuote>& Pair : Quotes)
	{
		if (Pair.Value.Id == QuoteId)
		{
			return Usable(Pair.Key, Now);
		}
	}
	return nullptr;
}

bool FScenarioQuotes::Consume(const FString& InFingerprint, double Now, FScenarioQuote& Out)
{
	const FScenarioQuote* Quote = Usable(InFingerprint, Now);
	if (Quote)
	{
		Out = *Quote;
	}
	// A stale quote is dropped too.
	Quotes.Remove(InFingerprint);
	return Quote != nullptr;
}

bool FScenarioQuotes::ConsumeById(const FString& QuoteId, double Now, FScenarioQuote& Out)
{
	FString Found;
	for (const TPair<FString, FScenarioQuote>& Pair : Quotes)
	{
		if (Pair.Value.Id == QuoteId)
		{
			Found = Pair.Key;
			break;
		}
	}
	return !Found.IsEmpty() && Consume(Found, Now, Out);
}

void FScenarioQuotes::Clear()
{
	Quotes.Reset();
}

void FScenarioQuotes::Prune(double Now)
{
	for (auto It = Quotes.CreateIterator(); It; ++It)
	{
		if (Now - It.Value().CreatedAt > MaxAgeSeconds)
		{
			It.RemoveCurrent();
		}
	}
}
