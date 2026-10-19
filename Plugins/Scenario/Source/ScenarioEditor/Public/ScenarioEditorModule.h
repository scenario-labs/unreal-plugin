// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class SWidget;
class FUICommandList;

/** Studio pages, for OpenStudio. */
namespace ScenarioPages
{
	inline const FName Create = TEXT("Create");
	inline const FName Workflows = TEXT("Workflows");
	inline const FName Library = TEXT("Library");
	inline const FName Jobs = TEXT("Jobs");
	inline const FName Results = TEXT("Results");
	inline const FName Connection = TEXT("Connection");
}

class SCENARIOEDITOR_API FScenarioEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FScenarioEditorModule& Get();

	/** Opens (or focuses) the Studio tab on a page. */
	static void OpenStudio(FName Page = NAME_None);
	/** Opens the Studio's Create page on a lane with a model, for context menus and "Reuse settings". Sends nothing. */
	static void OpenCreate(const FString& Lane, const FString& ModelId = FString(), const TSharedPtr<class FJsonObject>& Values = nullptr);
	static void ToggleComposer();
	static bool IsComposerVisible();
	/** Editor Preferences (false) or Project Settings (true) > Plugins > Scenario. */
	static void OpenSettings(bool bProject);

	/** A toast in the editor's corner. ButtonLabels and Buttons pair up. */
	static void Toast(const FText& Text, const FText& SubText, bool bSuccess, const TArray<FText>& ButtonLabels = {}, const TArray<TFunction<void()>>& Buttons = {});

	/** Extension points for the agent module: a Connection section and a header status widget. */
	using FSectionFactory = TFunction<TSharedRef<SWidget>()>;
	static void RegisterConnectionSection(FName Name, FSectionFactory Factory);
	static void UnregisterConnectionSection(FName Name);
	static const TArray<TPair<FName, FSectionFactory>>& ConnectionSections();
	static void SetHeaderStatus(FSectionFactory Factory);
	static TSharedRef<SWidget> MakeHeaderStatus();

	/** The Studio's keyboard commands (Generate, picker, Import and place). */
	TSharedPtr<FUICommandList> GetCommands() const { return Commands; }
	/** True once the Create model, composer and welcome have started. */
	bool IsStarted() const { return bStarted; }

private:
	void OnPostEngineInit();
	void RegisterMenus();
	void RegisterTabs();
	void BindCommands();

	TSharedPtr<FUICommandList> Commands;
	FDelegateHandle PostEngineInitHandle;
	FDelegateHandle PreExitHandle;
	FTSTicker::FDelegateHandle StartTicker;
	bool bStarted = false;
};
