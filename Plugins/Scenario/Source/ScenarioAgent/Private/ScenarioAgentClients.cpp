// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioAgentClients.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScenarioJson.h"
#include "ScenarioMcpServer.h"

#define LOCTEXT_NAMESPACE "ScenarioAgentClients"

namespace
{
	FString Home()
	{
		FString Path = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
		if (Path.IsEmpty())
		{
			Path = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		}
		return Path;
	}

	/** Copies the file next to itself with a timestamp, so nothing is lost. */
	bool Backup(const FString& Path)
	{
		if (!IFileManager::Get().FileExists(*Path))
		{
			return true;
		}
		const FString Copy = FString::Printf(TEXT("%s.scenario-backup-%s"), *Path, *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
		return IFileManager::Get().Copy(*Copy, *Path) == COPY_OK;
	}

	bool WriteJsonEntry(const FString& Path, const FString& Container, const TSharedRef<FJsonObject>& Entry, FString& OutMessage)
	{
		FString Text;
		if (IFileManager::Get().FileExists(*Path) && !FFileHelper::LoadFileToString(Text, *Path))
		{
			OutMessage = FString::Printf(TEXT("Could not read %s."), *Path);
			return false;
		}
		FString Updated;
		if (!ScenarioAgentClients::UpsertJsonEntry(Text, Container, FScenarioMcpServer::ServerName, Entry, Updated))
		{
			OutMessage = FString::Printf(TEXT("%s is not valid JSON, so it was left untouched. Use Copy setup and add the entry by hand."), *Path);
			return false;
		}
		if (!Backup(Path))
		{
			OutMessage = FString::Printf(TEXT("Could not back up %s, so it was left untouched."), *Path);
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		if (!FFileHelper::SaveStringToFile(Updated, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutMessage = FString::Printf(TEXT("Could not write %s."), *Path);
			return false;
		}
		OutMessage = FString::Printf(TEXT("Added scenario-unreal to %s (backup kept next to it)."), *Path);
		return true;
	}

	/** The claude CLI: PATH is short for apps started from the Dock, so the usual places are tried too. */
	FString FindClaude()
	{
		const FString H = Home();
		for (const FString& Candidate : { H / TEXT(".local/bin/claude"), H / TEXT(".claude/local/claude"), FString(TEXT("/opt/homebrew/bin/claude")), FString(TEXT("/usr/local/bin/claude")) })
		{
			if (IFileManager::Get().FileExists(*Candidate))
			{
				return Candidate;
			}
		}
		return FString();
	}

	FString Quote(const FString& Value)
	{
		return TEXT("\"") + Value.Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
	}
}

namespace ScenarioAgentClients
{
	const TArray<FClient>& All()
	{
		static const TArray<FClient> Clients = {
			{ TEXT("claude-code"), LOCTEXT("ClaudeCode", "Claude Code"), TEXT("claude-code") },
			{ TEXT("codex"), LOCTEXT("Codex", "Codex"), TEXT("codex") },
			{ TEXT("cursor"), LOCTEXT("Cursor", "Cursor"), TEXT("cursor") },
			{ TEXT("claude-desktop"), LOCTEXT("ClaudeDesktop", "Claude Desktop"), TEXT("mcp-remote") },
		};
		return Clients;
	}

	FString ConfigPath(const FString& ClientId)
	{
		const FString H = Home();
		if (ClientId == TEXT("claude-code"))
		{
			return H / TEXT(".claude.json");
		}
		if (ClientId == TEXT("codex"))
		{
			return H / TEXT(".codex/config.toml");
		}
		if (ClientId == TEXT("cursor"))
		{
			return H / TEXT(".cursor/mcp.json");
		}
		if (ClientId == TEXT("claude-desktop"))
		{
#if PLATFORM_MAC
			return H / TEXT("Library/Application Support/Claude/claude_desktop_config.json");
#elif PLATFORM_WINDOWS
			return FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA")) / TEXT("Claude/claude_desktop_config.json");
#else
			return H / TEXT(".config/Claude/claude_desktop_config.json");
#endif
		}
		return FString();
	}

	bool IsConfigured(const FString& ClientId, const FString& Url)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *ConfigPath(ClientId)))
		{
			return false;
		}
		return Text.Contains(FScenarioMcpServer::ServerName) && Text.Contains(Url);
	}

	TSharedRef<FJsonObject> HttpEntry(const FString& Url, const FString& Token, bool bWithType)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		if (bWithType)
		{
			Entry->SetStringField(TEXT("type"), TEXT("http"));
		}
		Entry->SetStringField(TEXT("url"), Url);
		if (!Token.IsEmpty())
		{
			TSharedRef<FJsonObject> Headers = MakeShared<FJsonObject>();
			Headers->SetStringField(TEXT("Authorization"), TEXT("Bearer ") + Token);
			Entry->SetObjectField(TEXT("headers"), Headers);
		}
		return Entry;
	}

	TSharedRef<FJsonObject> DesktopEntry(const FString& Url, const FString& Token)
	{
		// Claude Desktop speaks stdio: mcp-remote bridges to the HTTP server. The header goes through an
		// environment variable so the space in "Bearer <token>" survives argument parsing.
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("command"), TEXT("npx"));
		TArray<FString> Args = { TEXT("-y"), TEXT("mcp-remote"), Url, TEXT("--allow-http") };
		if (!Token.IsEmpty())
		{
			Args.Append({ TEXT("--header"), TEXT("Authorization:${SCENARIO_UNREAL_AUTH}") });
			TSharedRef<FJsonObject> Env = MakeShared<FJsonObject>();
			Env->SetStringField(TEXT("SCENARIO_UNREAL_AUTH"), TEXT("Bearer ") + Token);
			Entry->SetObjectField(TEXT("env"), Env);
		}
		Entry->SetField(TEXT("args"), ScenarioJson::StringArrayValue(Args));
		return Entry;
	}

	FString CodexTable(const FString& Url, const FString& Token)
	{
		FString Body = FString::Printf(TEXT("[mcp_servers.%s]\nurl = %s\n"), FScenarioMcpServer::ServerName, *Quote(Url));
		if (!Token.IsEmpty())
		{
			Body += FString::Printf(TEXT("\n[mcp_servers.%s.http_headers]\nAuthorization = %s\n"), FScenarioMcpServer::ServerName, *Quote(TEXT("Bearer ") + Token));
		}
		return Body;
	}

	bool UpsertJsonEntry(const FString& Text, const FString& Container, const FString& Name, const TSharedRef<FJsonObject>& Entry, FString& OutText)
	{
		TSharedPtr<FJsonObject> Root;
		if (Text.TrimStartAndEnd().IsEmpty())
		{
			Root = MakeShared<FJsonObject>();
		}
		else
		{
			Root = ScenarioJson::ParseObject(Text);
			if (!Root.IsValid())
			{
				return false;
			}
		}
		TSharedPtr<FJsonObject> Servers = ScenarioJson::GetObject(Root, Container);
		if (!Servers.IsValid())
		{
			if (Root->HasField(Container))
			{
				// Something else lives under that key: do not guess.
				return false;
			}
			Servers = MakeShared<FJsonObject>();
			Root->SetObjectField(Container, Servers);
		}
		Servers->SetObjectField(Name, Entry);
		OutText = ScenarioJson::Write(Root, true) + TEXT("\n");
		return true;
	}

	FString UpsertTomlTable(const FString& Text, const FString& Table, const FString& Body)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		const FString Own = TEXT("[") + Table + TEXT("]");
		const FString OwnPrefix = TEXT("[") + Table + TEXT(".");
		TArray<FString> Out;
		bool bSkipping = false;
		bool bReplaced = false;
		for (const FString& Line : Lines)
		{
			const FString Trimmed = Line.TrimStartAndEnd();
			const bool bHeader = Trimmed.StartsWith(TEXT("[")) && !Trimmed.StartsWith(TEXT("[["));
			if (bHeader)
			{
				const bool bMine = Trimmed == Own || Trimmed.StartsWith(OwnPrefix);
				if (bMine && !bReplaced)
				{
					// Our table goes where the old one was.
					TArray<FString> BodyLines;
					Body.ParseIntoArrayLines(BodyLines, false);
					Out.Append(BodyLines);
					bReplaced = true;
				}
				bSkipping = bMine;
				if (bMine)
				{
					continue;
				}
			}
			if (!bSkipping)
			{
				Out.Add(Line);
			}
		}
		if (!bReplaced)
		{
			while (Out.Num() > 0 && Out.Last().TrimStartAndEnd().IsEmpty())
			{
				Out.Pop();
			}
			if (Out.Num() > 0)
			{
				Out.Add(FString());
			}
			TArray<FString> BodyLines;
			Body.ParseIntoArrayLines(BodyLines, false);
			Out.Append(BodyLines);
		}
		return FString::Join(Out, TEXT("\n")) + TEXT("\n");
	}

	bool Connect(const FString& ClientId, const FString& Url, const FString& Token, FString& OutMessage)
	{
		const FString Path = ConfigPath(ClientId);
		if (ClientId == TEXT("claude-code"))
		{
			const FString Claude = FindClaude();
			if (!Claude.IsEmpty())
			{
				int32 Code = 0;
				FString Out;
				FString Err;
				// Replace an older entry (another port or token); a missing one is fine.
				FPlatformProcess::ExecProcess(*Claude, *FString::Printf(TEXT("mcp remove --scope user %s"), FScenarioMcpServer::ServerName), &Code, &Out, &Err);
				FString AddArgs = FString::Printf(TEXT("mcp add --transport http --scope user %s %s"), FScenarioMcpServer::ServerName, *Quote(Url));
				if (!Token.IsEmpty())
				{
					AddArgs += TEXT(" --header ") + Quote(TEXT("Authorization: Bearer ") + Token);
				}
				if (FPlatformProcess::ExecProcess(*Claude, *AddArgs, &Code, &Out, &Err) && Code == 0)
				{
					OutMessage = TEXT("Added scenario-unreal to Claude Code (user scope). Restart open Claude Code sessions to load it.");
					return true;
				}
				// Never echo the CLI output: it may repeat the header.
			}
			return WriteJsonEntry(Path, TEXT("mcpServers"), HttpEntry(Url, Token, true), OutMessage);
		}
		if (ClientId == TEXT("cursor"))
		{
			return WriteJsonEntry(Path, TEXT("mcpServers"), HttpEntry(Url, Token, false), OutMessage);
		}
		if (ClientId == TEXT("claude-desktop"))
		{
			const bool bOk = WriteJsonEntry(Path, TEXT("mcpServers"), DesktopEntry(Url, Token), OutMessage);
			if (bOk)
			{
				OutMessage += TEXT(" Claude Desktop needs Node.js (npx) and a restart.");
			}
			return bOk;
		}
		if (ClientId == TEXT("codex"))
		{
			FString Text;
			if (IFileManager::Get().FileExists(*Path) && !FFileHelper::LoadFileToString(Text, *Path))
			{
				OutMessage = FString::Printf(TEXT("Could not read %s."), *Path);
				return false;
			}
			if (!Backup(Path))
			{
				OutMessage = FString::Printf(TEXT("Could not back up %s, so it was left untouched."), *Path);
				return false;
			}
			const FString Updated = UpsertTomlTable(Text, FString::Printf(TEXT("mcp_servers.%s"), FScenarioMcpServer::ServerName), CodexTable(Url, Token));
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			if (!FFileHelper::SaveStringToFile(Updated, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutMessage = FString::Printf(TEXT("Could not write %s."), *Path);
				return false;
			}
			OutMessage = FString::Printf(TEXT("Added scenario-unreal to %s (backup kept next to it). Restart Codex to load it."), *Path);
			return true;
		}
		OutMessage = TEXT("Unknown client.");
		return false;
	}

	FString SetupText(const FString& Url, const FString& Token)
	{
		FString Text;
		Text += TEXT("Scenario for Unreal Engine, local MCP server\n\n");
		Text += FString::Printf(TEXT("URL: %s\n"), *Url);
		if (!Token.IsEmpty())
		{
			Text += FString::Printf(TEXT("Header: Authorization: Bearer %s\n"), *Token);
		}
		Text += TEXT("\nClaude Code:\n");
		Text += FString::Printf(TEXT("claude mcp add --transport http --scope user %s %s%s\n"), FScenarioMcpServer::ServerName, *Url,
			Token.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" --header \"Authorization: Bearer %s\""), *Token));
		Text += TEXT("\nJSON (Cursor, VS Code and most clients):\n");
		TSharedRef<FJsonObject> Servers = MakeShared<FJsonObject>();
		Servers->SetObjectField(FScenarioMcpServer::ServerName, HttpEntry(Url, Token, true));
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("mcpServers"), Servers);
		Text += ScenarioJson::Write(Root, true) + TEXT("\n");
		Text += TEXT("\nCodex (~/.codex/config.toml):\n");
		Text += CodexTable(Url, Token);
		return Text;
	}

	bool InstallSkill(FString& OutMessage)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scenario"));
		const FString Source = Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Resources/AgentSkill/SKILL.md") : FString();
		if (Source.IsEmpty() || !IFileManager::Get().FileExists(*Source))
		{
			OutMessage = TEXT("The skill file is missing from the plugin.");
			return false;
		}
		const FString Target = Home() / TEXT(".claude/skills/scenario-unreal/SKILL.md");
		if (!Backup(Target))
		{
			OutMessage = TEXT("Could not back up the installed skill, so it was left untouched.");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target), true);
		if (IFileManager::Get().Copy(*Target, *Source) != COPY_OK)
		{
			OutMessage = FString::Printf(TEXT("Could not write %s."), *Target);
			return false;
		}
		OutMessage = FString::Printf(TEXT("Installed the scenario-unreal skill in %s."), *FPaths::GetPath(Target));
		return true;
	}
}

#undef LOCTEXT_NAMESPACE
