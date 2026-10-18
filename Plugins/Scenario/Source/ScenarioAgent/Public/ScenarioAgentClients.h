// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * Connect buttons: each writes only its own "scenario-unreal" entry into one
 * client's configuration, after a timestamped backup, and leaves a file it
 * cannot parse untouched. The token goes into the client's config and nowhere
 * else (never into logs).
 */
namespace ScenarioAgentClients
{
	struct FClient
	{
		/** claude-code, codex, cursor, claude-desktop. */
		FString Id;
		FText Name;
		/** Fragment of the clientInfo name sent at initialize, to show "last seen". */
		FString ClientInfoName;
	};
	SCENARIOAGENT_API const TArray<FClient>& All();

	/** Where the client's configuration lives on this machine. */
	SCENARIOAGENT_API FString ConfigPath(const FString& ClientId);

	/** True when the client's configuration already has a scenario-unreal entry for this URL. */
	SCENARIOAGENT_API bool IsConfigured(const FString& ClientId, const FString& Url);

	/** Writes the entry. Runs the client's own CLI when there is one (Claude Code), else edits its file. Blocking: call off the game thread. */
	SCENARIOAGENT_API bool Connect(const FString& ClientId, const FString& Url, const FString& Token, FString& OutMessage);

	/** Setup for any other client: JSON, TOML and command lines, with the token. */
	SCENARIOAGENT_API FString SetupText(const FString& Url, const FString& Token);

	/** Copies the agent skill to ~/.claude/skills/scenario-unreal. */
	SCENARIOAGENT_API bool InstallSkill(FString& OutMessage);

	// Pure helpers, exposed for tests.

	/** Sets Root[Container][Name] = Entry in a JSON document, keeping everything else. False when Text is not a JSON object. */
	SCENARIOAGENT_API bool UpsertJsonEntry(const FString& Text, const FString& Container, const FString& Name, const TSharedRef<FJsonObject>& Entry, FString& OutText);

	/** Replaces the TOML table [Table] and its sub-tables [Table.*] with Body, or appends it. */
	SCENARIOAGENT_API FString UpsertTomlTable(const FString& Text, const FString& Table, const FString& Body);

	SCENARIOAGENT_API FString CodexTable(const FString& Url, const FString& Token);
	SCENARIOAGENT_API TSharedRef<FJsonObject> HttpEntry(const FString& Url, const FString& Token, bool bWithType);
	SCENARIOAGENT_API TSharedRef<FJsonObject> DesktopEntry(const FString& Url, const FString& Token);
}
