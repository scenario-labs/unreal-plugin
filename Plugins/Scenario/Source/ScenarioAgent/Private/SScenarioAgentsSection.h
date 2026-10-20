// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;

/** Connection > Agents: server state, Connect per client, permissions and the activity log. */
class SScenarioAgentsSection : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScenarioAgentsSection) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScenarioAgentsSection() override;

private:
	TSharedRef<SWidget> BuildClients();
	TSharedRef<SWidget> BuildPermissions();
	void RefreshActivity();
	void Connect(const FString& ClientId);

	TSharedPtr<SVerticalBox> ActivityBox;
	FString Message;
	bool bMessageIsError = false;
	TSet<FString> Busy;
	/** Read from the clients' config files on open and after Connect, never per frame. */
	TMap<FString, bool> Configured;
	void RefreshConfigured();
	FDelegateHandle ActivityHandle;

	/** Epic's Unreal MCP (ModelContextProtocol plugin), read from its settings and a port probe off the game thread. */
	TSharedRef<SWidget> BuildEpicMcp();
	void RefreshEpicMcp();
	FText EpicMcpTitle() const;
	FText EpicMcpDetail() const;
	enum class EEpicMcp : uint8 { NotInEngine, Off, Checking, Stopped, Running };
	EEpicMcp EpicMcp = EEpicMcp::Checking;
	int32 EpicMcpPort = 0;
	FString EpicMcpPath;
};
