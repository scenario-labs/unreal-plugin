// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "ScenarioCatalog.h"
#include "ScenarioCU.h"
#include "ScenarioEditorModule.h"
#include "ScenarioSession.h"
#include "ScenarioSettings.h"
#include "ScenarioStyle.h"
#include "ScenarioSubsystem.h"
#include "SScenarioPages.h"
#include "ScenarioWidgets.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "UObject/UnrealType.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScenarioSettings"

namespace
{
	FString AuthText(const FScenarioSession& Session)
	{
		switch (Session.GetAuthMode())
		{
		case EScenarioAuthMode::OAuth:
			return TEXT("Signed in with your Scenario account.");
		case EScenarioAuthMode::ApiKey:
			return Session.GetApiKeySource() == TEXT("environment")
				? TEXT("Using the API key from the SCENARIO_API_KEY environment variable.")
				: TEXT("Using a saved API key.");
		default:
			return TEXT("Signed out.");
		}
	}

	/** One preference row: a setting of the project (shared) or of this editor (yours). */
	struct FPreference
	{
		bool bProject = false;
		FName Property;
		FText Label;
	};

	struct FPreferenceSection
	{
		FText Title;
		TArray<FPreference> Rows;
	};

	const TArray<FPreferenceSection>& Preferences()
	{
		static const TArray<FPreferenceSection> Sections = {
			{ LOCTEXT("Output", "Output"), {
				{ true, TEXT("OutputRoot"), LOCTEXT("OutputRoot", "Content folder") },
				{ true, TEXT("bDateFolders"), LOCTEXT("DateFolders", "A folder per day") },
			} },
			{ LOCTEXT("Import", "Import"), {
				{ false, TEXT("bAutoImport"), LOCTEXT("AutoImport", "Import results as they arrive") },
				{ true, TEXT("bSaveImportedAssets"), LOCTEXT("SaveImported", "Save imported assets") },
				{ true, TEXT("MeshCollision"), LOCTEXT("Collision", "Mesh collision") },
				{ true, TEXT("bNanite"), LOCTEXT("Nanite", "Nanite meshes") },
				{ true, TEXT("MaxTextureSize"), LOCTEXT("MaxTexture", "Largest texture size") },
			} },
			{ LOCTEXT("Spending", "Spending"), {
				{ false, TEXT("ConfirmAboveCU"), LOCTEXT("ConfirmAbove", "Ask before spending more than") },
			} },
			{ LOCTEXT("ViewportPrivacy", "Viewport and privacy"), {
				{ false, TEXT("bShowComposer"), LOCTEXT("Composer", "Quick composer in the viewport") },
				{ true, TEXT("bOmitPromptFromAssets"), LOCTEXT("OmitPrompt", "Keep prompts out of asset metadata") },
				{ false, TEXT("bTelemetry"), LOCTEXT("Telemetry", "Anonymous usage statistics") },
			} },
		};
		return Sections;
	}

	UObject* SettingsObject(bool bProject)
	{
		return bProject ? static_cast<UObject*>(GetMutableDefault<UScenarioProjectSettings>()) : static_cast<UObject*>(GetMutableDefault<UScenarioEditorSettings>());
	}

	/** Applies a change the way the Details panel does, then saves it where it belongs. */
	void Commit(UObject* Settings, FProperty* Property)
	{
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Settings->PostEditChangeProperty(Event);
		if (Settings->GetClass()->HasAnyClassFlags(CLASS_DefaultConfig))
		{
			Settings->TryUpdateDefaultConfigFile();
		}
		else
		{
			Settings->SaveConfig();
		}
	}

	/** The first sentence of a property's tooltip, when it is short enough to sit under the label. */
	FText Hint(const FProperty* Property)
	{
		FString Tip = Property->GetToolTipText().ToString();
		int32 Dot = INDEX_NONE;
		if (Tip.FindChar(TEXT('.'), Dot))
		{
			Tip.LeftInline(Dot);
		}
		return Tip.Len() <= 72 ? FText::FromString(Tip) : FText::GetEmpty();
	}

	TSharedRef<SWidget> Control(bool bProject, FProperty* Property)
	{
		if (FBoolProperty* Bool = CastField<FBoolProperty>(Property))
		{
			return ScenarioUI::Switch(
				MakeAttributeLambda([Bool, bProject]() { return Bool->GetPropertyValue_InContainer(SettingsObject(bProject)); }),
				[Bool, bProject](bool bOn)
				{
					UObject* S = SettingsObject(bProject);
					Bool->SetPropertyValue_InContainer(S, bOn);
					Commit(S, Bool);
				});
		}
		if (FEnumProperty* Enum = CastField<FEnumProperty>(Property))
		{
			UEnum* Type = Enum->GetEnum();
			FNumericProperty* Underlying = Enum->GetUnderlyingProperty();
			auto Current = [Type, Underlying, Enum, bProject]()
			{
				const int64 Value = Underlying->GetSignedIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(SettingsObject(bProject)));
				return Type->GetDisplayNameTextByValue(Value);
			};
			return SNew(SComboButton)
				.ComboButtonStyle(&ScenarioUI::Style().GetWidgetStyle<FComboButtonStyle>("Scenario.Combo"))
				.ButtonContent()
				[
					SNew(STextBlock).Text_Lambda(Current).Font(ScenarioUI::Font(ScenarioUI::EText::Body))
				]
				.OnGetMenuContent_Lambda([Type, Underlying, Enum, bProject]()
				{
					FMenuBuilder Menu(true, nullptr);
					for (int32 Index = 0; Index < Type->NumEnums() - 1; ++Index)
					{
						if (Type->HasMetaData(TEXT("Hidden"), Index))
						{
							continue;
						}
						const int64 Value = Type->GetValueByIndex(Index);
						Menu.AddMenuEntry(Type->GetDisplayNameTextByIndex(Index), Type->GetToolTipTextByIndex(Index), FSlateIcon(),
							FUIAction(
								FExecuteAction::CreateLambda([Underlying, Enum, bProject, Value]()
								{
									UObject* S = SettingsObject(bProject);
									Underlying->SetIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(S), Value);
									Commit(S, Enum);
								}),
								FCanExecuteAction(),
								FIsActionChecked::CreateLambda([Underlying, Enum, bProject, Value]()
								{
									return Underlying->GetSignedIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(SettingsObject(bProject))) == Value;
								})),
							NAME_None, EUserInterfaceActionType::RadioButton);
					}
					return Menu.MakeWidget();
				});
		}
		if (FIntProperty* Int = CastField<FIntProperty>(Property))
		{
			const int32 Min = Int->HasMetaData(TEXT("ClampMin")) ? FCString::Atoi(*Int->GetMetaData(TEXT("ClampMin"))) : 0;
			const int32 Max = Int->HasMetaData(TEXT("ClampMax")) ? FCString::Atoi(*Int->GetMetaData(TEXT("ClampMax"))) : 1 << 20;
			return SNew(SBox)
				.WidthOverride(110.0f)
				[
					SNew(SSpinBox<int32>)
					.MinValue(Min)
					.MaxValue(Max)
					.Delta(1)
					.Value_Lambda([Int, bProject]() { return Int->GetPropertyValue_InContainer(SettingsObject(bProject)); })
					.OnValueCommitted_Lambda([Int, bProject](int32 Value, ETextCommit::Type)
					{
						UObject* S = SettingsObject(bProject);
						Int->SetPropertyValue_InContainer(S, Value);
						Commit(S, Int);
					})
				];
		}
		if (FFloatProperty* Float = CastField<FFloatProperty>(Property))
		{
			return SNew(SBox)
				.WidthOverride(110.0f)
				[
					SNew(SSpinBox<float>)
					.MinValue(0.0f)
					.MaxValue(100000.0f)
					.Delta(1.0f)
					.Value_Lambda([Float, bProject]() { return Float->GetPropertyValue_InContainer(SettingsObject(bProject)); })
					.OnValueCommitted_Lambda([Float, bProject](float Value, ETextCommit::Type)
					{
						UObject* S = SettingsObject(bProject);
						Float->SetPropertyValue_InContainer(S, Value);
						Commit(S, Float);
					})
				];
		}
		if (FStructProperty* Struct = CastField<FStructProperty>(Property); Struct && Struct->Struct == FDirectoryPath::StaticStruct())
		{
			return SNew(SBox)
				.WidthOverride(200.0f)
				[
					SNew(SEditableTextBox)
					.Style(&ScenarioUI::Style().GetWidgetStyle<FEditableTextBoxStyle>("Scenario.Input"))
					.Font(ScenarioUI::Font(ScenarioUI::EText::Body))
					.Text_Lambda([Struct, bProject]() { return FText::FromString(Struct->ContainerPtrToValuePtr<FDirectoryPath>(SettingsObject(bProject))->Path); })
					.OnTextCommitted_Lambda([Struct, bProject](const FText& Text, ETextCommit::Type)
					{
						UObject* S = SettingsObject(bProject);
						Struct->ContainerPtrToValuePtr<FDirectoryPath>(S)->Path = Text.ToString().TrimStartAndEnd();
						Commit(S, Struct);
					})
				];
		}
		return SNullWidget::NullWidget;
	}

	TSharedRef<SWidget> PreferenceRow(const FPreference& Row)
	{
		UObject* Settings = SettingsObject(Row.bProject);
		FProperty* Property = FindFProperty<FProperty>(Settings->GetClass(), Row.Property);
		if (!Property)
		{
			return SNullWidget::NullWidget;
		}
		const FText Scope = Row.bProject
			? LOCTEXT("ProjectScope", "Project setting: shared with your team through Config/DefaultEditor.ini.")
			: LOCTEXT("EditorScope", "Your setting: kept on this computer for this project.");
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, ScenarioUI::Gutter, 0.0f)
			[
				ScenarioUI::FieldLabel(Row.Label, Hint(Property), FText::Format(LOCTEXT("RowTip", "{0}\n\n{1}"), Property->GetToolTipText(), Scope))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				Control(Row.bProject, Property)
			];
	}
}

FString SScenarioSettingsPage::Diagnostics()
{
	TArray<FString> Lines;
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scenario"));
	Lines.Add(FString::Printf(TEXT("Scenario for Unreal Engine %s"), Plugin.IsValid() ? *Plugin->GetDescriptor().VersionName : TEXT("?")));
	Lines.Add(FString::Printf(TEXT("Unreal Engine %s"), *FEngineVersion::Current().ToString(EVersionComponent::Patch)));
	Lines.Add(FString::Printf(TEXT("Platform %s %s"), ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()), *FPlatformMisc::GetOSVersion()));
	FScenarioSession* Session = ScenarioUI::Session();
	if (!Session)
	{
		Lines.Add(TEXT("Session: not started"));
		return FString::Join(Lines, TEXT("\n"));
	}
	static const TCHAR* Connections[] = { TEXT("signed out"), TEXT("connecting"), TEXT("needs a project"), TEXT("ready") };
	Lines.Add(FString::Printf(TEXT("Connection: %s"), Connections[FMath::Clamp(static_cast<int32>(Session->GetConnection()), 0, 3)]));
	Lines.Add(FString::Printf(TEXT("Sign-in: %s"), *AuthText(*Session)));
	Lines.Add(FString::Printf(TEXT("Credentials: %s%s"), *Session->DescribeCredentialStore(), Session->IsCredentialStoreSecure() ? TEXT("") : TEXT(" (not encrypted)")));
	Lines.Add(FString::Printf(TEXT("Endpoint: %s"), Session->GetConfig().Endpoint.IsEmpty() ? TEXT("default") : *Session->GetConfig().Endpoint));
	Lines.Add(FString::Printf(TEXT("Team/project ids: %s / %s"), *Session->GetWorkspace().TeamId, *Session->GetWorkspace().ProjectId));
	Lines.Add(FString::Printf(TEXT("Spending: %s"), Session->IsSpendingDisabled() ? TEXT("off (SCENARIO_NO_SPEND=1)") : TEXT("on")));
	Lines.Add(FString::Printf(TEXT("Automatic price checks: %s"), Session->IsEstimateAlarmRaised() ? TEXT("stopped (a price check returned a job)") : TEXT("on")));
	const FScenarioCatalog& Catalog = Session->Catalog();
	Lines.Add(FString::Printf(TEXT("Catalog: %d models, fetched %s"), Catalog.All().Num(),
		Catalog.FetchedAt() > 0.0 ? *FDateTime::FromUnixTimestamp(static_cast<int64>(Catalog.FetchedAt())).ToIso8601() : TEXT("never")));
	int32 Active = 0;
	int32 Unresolved = 0;
	for (const FScenarioJobRow& Row : Session->Ledger().Rows())
	{
		Active += Row.IsActive() ? 1 : 0;
		Unresolved += Row.IsUnresolved() ? 1 : 0;
	}
	Lines.Add(FString::Printf(TEXT("Jobs: %d in the list, %d running, %d unresolved"), Session->Ledger().Rows().Num(), Active, Unresolved));
	Lines.Add(FString::Printf(TEXT("State folder: %s"), *FPaths::ConvertRelativePathToFull(UScenarioSubsystem::StateDir())));
	if (Session->GetLastError().Kind != EScenarioErrorKind::None)
	{
		Lines.Add(FString::Printf(TEXT("Last error: %s"), *Session->GetLastError().ToString()));
	}
	return FString::Join(Lines, TEXT("\n"));
}

void SScenarioSettingsPage::Construct(const FArguments& InArgs)
{
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		bWasSignedIn = Session->IsSignedIn();
		const TWeakPtr<SScenarioSettingsPage> Weak = SharedThis(this);
		SessionHandle = Session->OnChanged.AddLambda([Weak]()
		{
			const TSharedPtr<SScenarioSettingsPage> This = Weak.Pin();
			FScenarioSession* S = ScenarioUI::Session();
			if (This.IsValid() && S && S->IsSignedIn() != This->bWasSignedIn)
			{
				This->bWasSignedIn = S->IsSignedIn();
				This->Rebuild();
			}
		});
	}
	ChildSlot
	[
		SAssignNew(Content, SBox)
	];
	Rebuild();
}

SScenarioSettingsPage::~SScenarioSettingsPage()
{
	if (FScenarioSession* Session = ScenarioUI::Session())
	{
		Session->OnChanged.Remove(SessionHandle);
	}
}

void SScenarioSettingsPage::Rebuild()
{
	using namespace ScenarioUI;
	FScenarioSession* Session = ScenarioUI::Session();
	if (!Session || !Session->IsSignedIn())
	{
		Content->SetContent(SNew(SScenarioSignIn));
		return;
	}

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	Box->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, SectionGap)
	[
		PageHeader(LOCTEXT("Title", "Settings"), LOCTEXT("Subtitle", "Your account, where results go, and what to send for support."))
	];

	// Account: who is signed in, the project, and this month's team usage.
	const bool bOAuth = Session->GetAuthMode() == EScenarioAuthMode::OAuth;
	Box->AddSlot().AutoHeight()
	[
		Card(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					SNew(SBorder)
					.BorderImage(Style().GetBrush("Scenario.Thumb"))
					.Padding(8.0f)
					[
						Icon(TEXT("user"), 20.0f, Palette::Dim())
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
						Line(MakeAttributeLambda([]()
						{
							FScenarioSession* S = ScenarioUI::Session();
							return S && S->GetWorkspace().IsSet() ? FText::FromString(S->GetWorkspace().ProjectName) : LOCTEXT("NoProject", "No project chosen yet");
						}), EText::Title)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						Line(MakeAttributeLambda([]()
						{
							FScenarioSession* S = ScenarioUI::Session();
							if (!S)
							{
								return FText::GetEmpty();
							}
							const FString Team = S->GetWorkspace().TeamName;
							return Team.IsEmpty() ? FText::FromString(AuthText(*S))
								: FText::Format(LOCTEXT("TeamLine", "Team {0}. {1}"), FText::FromString(Team), FText::FromString(AuthText(*S)));
						}), EText::Meta, true)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(Gutter, 0.0f, 0.0f, 0.0f)
				[
					SNew(SComboButton)
					.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
					.ButtonStyle(&Style().GetWidgetStyle<FButtonStyle>("Scenario.Button.Outline.Small"))
					.HasDownArrow(false)
					.IsEnabled(bOAuth)
					.ToolTipText(bOAuth ? LOCTEXT("SwitchTip", "Generate in another team or project. Running jobs continue.") : LOCTEXT("SwitchKeyTip", "An API key belongs to one project: sign in with Scenario to switch."))
					.OnGetMenuContent_Lambda([]() { return ScenarioUI::ProjectMenu(); })
					.ButtonContent()
					[
						SNew(STextBlock).Text(LOCTEXT("Switch", "Switch project")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					Button(LOCTEXT("SignOut", "Sign out"), EButton::Outline, []()
					{
						if (FScenarioSession* S = ScenarioUI::Session())
						{
							S->SignOut();
						}
					}, NAME_None, LOCTEXT("SignOutTip", "Removes the saved sign-in from this machine. Jobs and imported assets stay."), true)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 10.0f)
			[
				Divider()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					FieldLabel(LOCTEXT("Usage", "Project usage this month"), LOCTEXT("UsageHint", "Every member and tool, from Scenario"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([]()
					{
						FScenarioSession* S = ScenarioUI::Session();
						const double Used = S ? S->GetUsageThisMonth() : -1.0;
						return Used < 0.0 ? LOCTEXT("UsageUnknown", "Unavailable") : FText::FromString(ScenarioCU::Format(Used));
					})
					.Font(Font(EText::Title))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					IconButton(TEXT("refresh"), LOCTEXT("RefreshUsage", "Refresh this month's usage. Free."),
						FSimpleDelegate::CreateLambda([]()
						{
							if (FScenarioSession* S = ScenarioUI::Session())
							{
								S->RefreshUsage();
							}
						}))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([]()
				{
					FScenarioSession* S = ScenarioUI::Session();
					if (!S)
					{
						return FText::GetEmpty();
					}
					return S->IsCredentialStoreSecure()
						? FText::Format(LOCTEXT("Stored", "Sign-in kept in {0}, never in project files."), FText::FromString(S->DescribeCredentialStore()))
						: FText::Format(LOCTEXT("Insecure", "Sign-in kept in {0}. This store is not encrypted on this system."), FText::FromString(S->DescribeCredentialStore()));
				})
				.Font(Font(EText::Meta))
				.ColorAndOpacity_Lambda([]() { FScenarioSession* S = ScenarioUI::Session(); return S && !S->IsCredentialStoreSecure() ? FSlateColor(Palette::Warning()) : FSlateColor(Palette::Faint()); })
				.AutoWrapText(true)
			], FMargin(12.0f))
	];

	// Preferences, grouped as in the Unity plugin; every one also lives in the editor's settings.
	for (const FPreferenceSection& Section : Preferences())
	{
		TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
		for (const FPreference& Row : Section.Rows)
		{
			Rows->AddSlot().AutoHeight().Padding(0.0f, Rows->NumSlots() > 0 ? 10.0f : 0.0f, 0.0f, 0.0f)[PreferenceRow(Row)];
		}
		Box->AddSlot().AutoHeight()[SectionHeader(Section.Title)];
		Box->AddSlot().AutoHeight()[Card(Rows, FMargin(12.0f))];
	}

	// Diagnostics
	Box->AddSlot().AutoHeight()[SectionHeader(LOCTEXT("Diagnostics", "Diagnostics"))];
	Box->AddSlot().AutoHeight()
	[
		Card(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text_Lambda([]() { return FText::FromString(SScenarioSettingsPage::Diagnostics()); })
				.Font(Font(EText::Meta))
				.ColorAndOpacity(Palette::Dim())
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					Button(LOCTEXT("Copy", "Copy diagnostics"), EButton::Secondary, []() { FPlatformApplicationMisc::ClipboardCopy(*SScenarioSettingsPage::Diagnostics()); },
						TEXT("copy"), LOCTEXT("CopyTip", "Copies these lines for support. They hold no key, token or prompt."), true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					Button(LOCTEXT("Folder", "Open state folder"), EButton::Secondary, []() { FPlatformProcess::ExploreFolder(*FPaths::ConvertRelativePathToFull(UScenarioSubsystem::StateDir())); },
						TEXT("folder"), LOCTEXT("FolderTip", "Opens Saved/Scenario: job list, downloads, captures."), true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					Button(LOCTEXT("AllSettings", "All settings"), EButton::Ghost, []() { FScenarioEditorModule::OpenSettings(true); },
						TEXT("external"), LOCTEXT("AllSettingsTip", "Opens Project Settings > Plugins > Scenario, with the allowed and hidden models."), true)
				]
			], FMargin(12.0f))
	];

	Content->SetContent(
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(PagePadding)
		[
			Box
		]);
}

#undef LOCTEXT_NAMESPACE
