// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioSession.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScenarioCatalog.h"
#include "ScenarioCoreModule.h"
#include "ScenarioCredentialStore.h"
#include "ScenarioErrors.h"
#include "ScenarioFiles.h"
#include "ScenarioHttp.h"
#include "ScenarioJson.h"
#include "ScenarioLanes.h"
#include "ScenarioMcpClient.h"
#include "ScenarioOAuth.h"
#include "ScenarioTransfer.h"

namespace
{
	const TCHAR* KindName(EScenarioQuoteKind Kind)
	{
		switch (Kind)
		{
		case EScenarioQuoteKind::Workflow: return TEXT("workflow");
		case EScenarioQuoteKind::PromptSpark: return TEXT("prompt_spark");
		default: return TEXT("model");
		}
	}

	FScenarioError SessionError(EScenarioErrorKind Kind, const FString& Message, bool bNotSent = true)
	{
		FScenarioError Error;
		Error.Kind = Kind;
		Error.Message = Message;
		Error.bNotSent = bNotSent;
		return Error;
	}

	/** Asset ids anywhere in a payload: string values and string arrays that look like Scenario asset ids. */
	void CollectAssetIds(const TSharedPtr<FJsonObject>& Object, TArray<FString>& Out)
	{
		if (!Object.IsValid())
		{
			return;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ScenarioJson::Fields(Object))
		{
			if (!Pair.Value.IsValid())
			{
				continue;
			}
			if (Pair.Value->Type == EJson::String && Pair.Value->AsString().StartsWith(TEXT("asset_")))
			{
				Out.AddUnique(Pair.Value->AsString());
			}
			else if (Pair.Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Item : Pair.Value->AsArray())
				{
					if (Item.IsValid() && Item->Type == EJson::String && Item->AsString().StartsWith(TEXT("asset_")))
					{
						Out.AddUnique(Item->AsString());
					}
				}
			}
		}
	}

	/** The prompt a run carries, for the job list: "prompt", or the first long text input of a workflow. */
	FString PromptOf(const TSharedPtr<FJsonObject>& Parameters)
	{
		const FString Prompt = ScenarioJson::GetString(Parameters, TEXT("prompt"));
		if (!Prompt.IsEmpty() || !Parameters.IsValid())
		{
			return Prompt;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ScenarioJson::Fields(Parameters))
		{
			if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String && !Pair.Value->AsString().StartsWith(TEXT("asset_")) && Pair.Value->AsString().Contains(TEXT(" ")))
			{
				return Pair.Value->AsString();
			}
		}
		return FString();
	}

	FString ReadHeadExtension(const FString& Path)
	{
		TArray<uint8> Head;
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
		if (!Reader)
		{
			return FString();
		}
		const int64 Count = FMath::Min<int64>(Reader->TotalSize(), 64);
		Head.SetNumUninitialized(static_cast<int32>(Count));
		Reader->Serialize(Head.GetData(), Count);
		return ScenarioFiles::SniffExtension(Head);
	}
}

FString FScenarioWorkspace::Label() const
{
	const FString Team = TeamName.IsEmpty() ? TeamId : TeamName;
	const FString Project = ProjectName.IsEmpty() ? ProjectId : ProjectName;
	if (Team.IsEmpty())
	{
		return Project;
	}
	return Project.IsEmpty() ? Team : Team + TEXT(" / ") + Project;
}

FString FScenarioRunRequest::QuoteTarget() const
{
	return Kind == EScenarioQuoteKind::PromptSpark ? TEXT("prompt_spark:") + TargetId : TargetId;
}

FScenarioSession::FScenarioSession(const FScenarioSessionConfig& InConfig)
	: Config(InConfig)
{
	Transport = Config.Transport;
	if (!Transport.IsValid())
	{
		Transport = MakeShared<FScenarioHttpTransport>();
	}
	Store = Config.Store;
	if (!Store.IsValid())
	{
		Store = ScenarioCredentials::CreatePlatformStore();
	}
	if (Config.Backend.IsValid())
	{
		Backend = Config.Backend;
	}
	else
	{
		Client = MakeShared<FScenarioMcpClient>(Transport.ToSharedRef(), Config.Endpoint.IsEmpty() ? FString(FScenarioMcpClient::DefaultEndpoint) : Config.Endpoint);
		Backend = Client;
		OAuth = MakeShared<FScenarioOAuth>(Transport.ToSharedRef(), Store.ToSharedRef());
	}
	ApiPtr = MakeShared<FScenarioApi>(Backend.ToSharedRef());
	LedgerPtr = MakeShared<FScenarioLedger>(Config.StateDir.IsEmpty() ? FString() : FPaths::Combine(Config.StateDir, TEXT("jobs.json")));
	CatalogPtr = MakeShared<FScenarioCatalog>(ApiPtr.ToSharedRef(), Config.StateDir.IsEmpty() ? FString() : FPaths::Combine(Config.StateDir, TEXT("catalog.json")));
	Downloader = MakeShared<FScenarioDownloader>(Transport.ToSharedRef());
	Uploader = MakeShared<FScenarioUploader>(Transport.ToSharedRef(), ApiPtr.ToSharedRef());
}

FScenarioSession::~FScenarioSession()
{
	Shutdown();
}

double FScenarioSession::Now() const
{
	return Clock ? Clock() : ScenarioNow();
}

void FScenarioSession::Start()
{
	LedgerPtr->Load();
	CatalogPtr->LoadCache();
	LoadWorkspace();
	if (Config.bUseTicker && !TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FScenarioSession::Tick), 0.5f);
	}

	if (!Client.IsValid())
	{
		// Injected backend (tests): signed in when it says so.
		if (Backend->HasCredentials())
		{
			AuthMode = EScenarioAuthMode::ApiKey;
			Connected();
		}
		return;
	}

	const FScenarioApiKeyCredentials Key = FScenarioApiKeyCredentials::Load(*Store);
	if (Key.IsValid())
	{
		UseApiKey(Key.Key, Key.Secret, Key.Source);
		Connected();
		return;
	}
	if (OAuth->UseEnvironmentToken())
	{
		UseOAuth();
		Connected();
		return;
	}
	if (OAuth->Restore())
	{
		UseOAuth();
		SetConnection(EScenarioConnection::Connecting);
		const uint32 Generation = AuthGeneration;
		TWeakPtr<FScenarioSession> WeakThis = AsShared();
		OAuth->Refresh([WeakThis, Generation](bool bRefreshed)
		{
			const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
			if (!This.IsValid() || This->AuthGeneration != Generation)
			{
				return;
			}
			if (bRefreshed)
			{
				This->Connected();
			}
			else if (!This->OAuth->HasRefreshToken())
			{
				This->AuthMode = EScenarioAuthMode::None;
				This->SetConnection(EScenarioConnection::SignedOut, SessionError(EScenarioErrorKind::Auth, TEXT("Your Scenario session ended. Sign in again.")));
			}
			else
			{
				// Offline: keep the sign-in, jobs resume when Scenario is reachable.
				This->SetConnection(This->Workspace.IsSet() ? EScenarioConnection::Ready : EScenarioConnection::NeedsWorkspace,
					SessionError(EScenarioErrorKind::Transport, FScenarioErrors::MessageFor(EScenarioErrorKind::Transport)));
			}
		});
	}
}

void FScenarioSession::Shutdown()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (OAuth.IsValid() && OAuth->IsSigningIn())
	{
		OAuth->Cancel();
	}
	if (Downloader.IsValid())
	{
		Downloader->CancelAll();
	}
}

bool FScenarioSession::Tick(float DeltaSeconds)
{
	Poll(Now());
	return true;
}

void FScenarioSession::SetConnection(EScenarioConnection InConnection, const FScenarioError& Error)
{
	Connection = InConnection;
	LastError = Error;
	if (InConnection == EScenarioConnection::Ready || InConnection == EScenarioConnection::NeedsWorkspace)
	{
		bPollPaused = false;
	}
	OnChanged.Broadcast();
}

FString FScenarioSession::DescribeCredentialStore() const
{
	return Store.IsValid() ? Store->Describe() : FString();
}

bool FScenarioSession::IsCredentialStoreSecure() const
{
	return Store.IsValid() && Store->IsOsStore();
}

void FScenarioSession::UseApiKey(const FString& Key, const FString& Secret, const FString& Source)
{
	++AuthGeneration;
	AuthMode = EScenarioAuthMode::ApiKey;
	ApiKeySource = Source;
	QuoteBook.Clear();
	if (Client.IsValid())
	{
		Client->SetAuth(MakeShared<FScenarioApiKeyAuth>(Key, Secret));
		Client->SetWorkspace(FString(), FString());
	}
}

void FScenarioSession::UseOAuth()
{
	++AuthGeneration;
	AuthMode = EScenarioAuthMode::OAuth;
	ApiKeySource.Reset();
	QuoteBook.Clear();
	if (Client.IsValid())
	{
		Client->SetAuth(OAuth);
		ApplyWorkspaceToClient();
	}
}

void FScenarioSession::ApplyWorkspaceToClient()
{
	if (Client.IsValid())
	{
		if (AuthMode == EScenarioAuthMode::OAuth)
		{
			Client->SetWorkspace(Workspace.TeamId, Workspace.ProjectId);
		}
		else
		{
			Client->SetWorkspace(FString(), FString());
		}
	}
}

void FScenarioSession::Connected()
{
	SetConnection(EScenarioConnection::Connecting);
	const uint32 Generation = AuthGeneration;
	TWeakPtr<FScenarioSession> WeakThis = AsShared();
	RefreshTeams([WeakThis, Generation](const FScenarioReply& Reply)
	{
		const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
		if (!This.IsValid() || This->AuthGeneration != Generation)
		{
			return;
		}
		if (!Reply.bOk)
		{
			const EScenarioErrorKind Kind = Reply.Error.Kind;
			if (Kind == EScenarioErrorKind::Auth || Kind == EScenarioErrorKind::Forbidden)
			{
				This->AuthMode = EScenarioAuthMode::None;
				if (This->Client.IsValid())
				{
					This->Client->SetAuth(nullptr);
				}
				This->SetConnection(EScenarioConnection::SignedOut, Reply.Error);
				return;
			}
			// Offline or Scenario unavailable: stay signed in with what is known.
			const bool bKnown = This->AuthMode == EScenarioAuthMode::ApiKey || This->Workspace.IsSet();
			This->SetConnection(bKnown ? EScenarioConnection::Ready : EScenarioConnection::NeedsWorkspace, Reply.Error);
			return;
		}

		FScenarioWorkspace Chosen;
		if (This->AuthMode == EScenarioAuthMode::ApiKey)
		{
			// An API key is scoped to one project; the first one listed is the key's (assumed, see docs/ARCHITECTURE.md).
			for (const FScenarioTeam& Team : This->Teams)
			{
				if (Team.Projects.Num() > 0)
				{
					Chosen = { Team.Id, Team.Name, Team.Projects[0].Id, Team.Projects[0].Name };
					break;
				}
			}
		}
		else
		{
			// Keep the saved choice when it is still there, otherwise pick the only project.
			int32 ProjectCount = 0;
			FScenarioWorkspace Only;
			for (const FScenarioTeam& Team : This->Teams)
			{
				for (const FScenarioProject& Project : Team.Projects)
				{
					++ProjectCount;
					Only = { Team.Id, Team.Name, Project.Id, Project.Name };
					if (Team.Id == This->Workspace.TeamId && Project.Id == This->Workspace.ProjectId)
					{
						Chosen = Only;
					}
				}
			}
			if (!Chosen.IsSet() && ProjectCount == 1)
			{
				Chosen = Only;
			}
		}

		if (!Chosen.IsSet())
		{
			This->Workspace = FScenarioWorkspace();
			This->SetConnection(EScenarioConnection::NeedsWorkspace);
			return;
		}
		This->SetWorkspace(Chosen);
	});
}

void FScenarioSession::SignInWithBrowser(TFunction<void(const FString& Url)> OnUrl, FScenarioReplyCallback OnDone)
{
	if (!OAuth.IsValid())
	{
		OnDone(FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("Sign-in is not available in this session.")));
		return;
	}
	SetConnection(EScenarioConnection::Connecting);
	TWeakPtr<FScenarioSession> WeakThis = AsShared();
	OAuth->BeginBrowserSignIn(MoveTemp(OnUrl), [WeakThis, OnDone](const FScenarioReply& Reply)
	{
		const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		if (Reply.bOk)
		{
			This->UseOAuth();
			This->Connected();
		}
		else
		{
			This->SetConnection(EScenarioConnection::SignedOut, Reply.Error);
		}
		if (OnDone)
		{
			OnDone(Reply);
		}
	});
}

void FScenarioSession::SignInWithDeviceCode(TFunction<void(const FScenarioDeviceCode& Code)> OnCode, FScenarioReplyCallback OnDone)
{
	if (!OAuth.IsValid())
	{
		OnDone(FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("Sign-in is not available in this session.")));
		return;
	}
	SetConnection(EScenarioConnection::Connecting);
	TWeakPtr<FScenarioSession> WeakThis = AsShared();
	OAuth->BeginDeviceSignIn(MoveTemp(OnCode), [WeakThis, OnDone](const FScenarioReply& Reply)
	{
		const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		if (Reply.bOk)
		{
			This->UseOAuth();
			This->Connected();
		}
		else
		{
			This->SetConnection(EScenarioConnection::SignedOut, Reply.Error);
		}
		if (OnDone)
		{
			OnDone(Reply);
		}
	});
}

void FScenarioSession::SignInWithApiKey(const FString& Key, const FString& Secret, FScenarioReplyCallback OnDone)
{
	FString CleanKey = Key.TrimStartAndEnd();
	FString CleanSecret = Secret.TrimStartAndEnd();
	if (CleanSecret.IsEmpty())
	{
		// One "key:secret" pair pasted in the key field.
		const FString Pair = CleanKey;
		Pair.Split(TEXT(":"), &CleanKey, &CleanSecret);
	}
	if (CleanKey.IsEmpty() || CleanSecret.IsEmpty())
	{
		OnDone(FScenarioReply::Fail(EScenarioErrorKind::Invalid, TEXT("Enter the API key and its secret (or one key:secret pair).")));
		return;
	}
	if (!Client.IsValid())
	{
		OnDone(FScenarioReply::Fail(EScenarioErrorKind::Local, TEXT("Sign-in is not available in this session.")));
		return;
	}
	UseApiKey(CleanKey, CleanSecret, TEXT("store"));
	SetConnection(EScenarioConnection::Connecting);
	const uint32 Generation = AuthGeneration;
	TWeakPtr<FScenarioSession> WeakThis = AsShared();
	ApiPtr->ListTeams([WeakThis, Generation, CleanKey, CleanSecret, OnDone](const TScenarioResult<TArray<FScenarioTeam>>& Result)
	{
		const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
		if (!This.IsValid() || This->AuthGeneration != Generation)
		{
			return;
		}
		if (!Result.bOk)
		{
			This->AuthMode = EScenarioAuthMode::None;
			This->Client->SetAuth(nullptr);
			FScenarioError Error = Result.Error;
			if (Error.Kind == EScenarioErrorKind::Auth || Error.Kind == EScenarioErrorKind::Forbidden)
			{
				Error.Message = TEXT("Scenario did not accept this API key and secret.");
			}
			This->SetConnection(EScenarioConnection::SignedOut, Error);
			OnDone(FScenarioReply::Fail(Error));
			return;
		}
		const FString SaveError = FScenarioApiKeyCredentials::Save(*This->Store, CleanKey, CleanSecret);
		if (!SaveError.IsEmpty())
		{
			UE_LOG(LogScenario, Warning, TEXT("%s The key works for this session only."), *SaveError);
		}
		This->Teams = Result.Value;
		This->Connected();
		OnDone(FScenarioReply::Ok());
	});
}

void FScenarioSession::CancelSignIn()
{
	if (OAuth.IsValid() && OAuth->IsSigningIn())
	{
		OAuth->Cancel();
	}
}

bool FScenarioSession::IsSigningIn() const
{
	return OAuth.IsValid() && OAuth->IsSigningIn();
}

void FScenarioSession::SignOut()
{
	++AuthGeneration;
	if (AuthMode == EScenarioAuthMode::OAuth && OAuth.IsValid())
	{
		OAuth->SignOut();
	}
	if (AuthMode == EScenarioAuthMode::ApiKey && Store.IsValid() && ApiKeySource != TEXT("environment"))
	{
		FScenarioApiKeyCredentials::Clear(*Store);
	}
	if (Client.IsValid())
	{
		Client->SetAuth(nullptr);
	}
	AuthMode = EScenarioAuthMode::None;
	ApiKeySource.Reset();
	QuoteBook.Clear();
	Teams.Reset();
	UsageThisMonth = -1.0;
	CatalogPtr->ClearPrivate();
	// The saved workspace stays: signing in again with the same account lands in the same project.
	SetConnection(EScenarioConnection::SignedOut);
}

void FScenarioSession::RefreshTeams(FScenarioReplyCallback OnDone)
{
	TWeakPtr<FScenarioSession> WeakThis = AsShared();
	ApiPtr->ListTeams([WeakThis, OnDone](const TScenarioResult<TArray<FScenarioTeam>>& Result)
	{
		const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		if (Result.bOk)
		{
			This->Teams = Result.Value;
			// Teams may come without their projects; fill them in for the picker.
			for (int32 Index = 0; Index < This->Teams.Num(); ++Index)
			{
				if (This->Teams[Index].Projects.Num() > 0 || This->AuthMode == EScenarioAuthMode::ApiKey)
				{
					continue;
				}
				const FString TeamId = This->Teams[Index].Id;
				This->ApiPtr->ListProjects(TeamId, [WeakThis, TeamId](const TScenarioResult<TArray<FScenarioProject>>& Projects)
				{
					const TSharedPtr<FScenarioSession> Inner = WeakThis.Pin();
					if (!Inner.IsValid() || !Projects.bOk)
					{
						return;
					}
					for (FScenarioTeam& Team : Inner->Teams)
					{
						if (Team.Id == TeamId)
						{
							Team.Projects = Projects.Value;
						}
					}
					Inner->OnChanged.Broadcast();
				});
			}
			This->OnChanged.Broadcast();
		}
		if (OnDone)
		{
			OnDone(Result.bOk ? FScenarioReply::Ok() : FScenarioReply::Fail(Result.Error));
		}
	});
}

void FScenarioSession::SetWorkspace(const FScenarioWorkspace& InWorkspace)
{
	const bool bChanged = InWorkspace.TeamId != Workspace.TeamId || InWorkspace.ProjectId != Workspace.ProjectId;
	Workspace = InWorkspace;
	ApplyWorkspaceToClient();
	SaveWorkspace();
	if (bChanged)
	{
		QuoteBook.Clear();
		CatalogPtr->ClearPrivate();
		UsageThisMonth = -1.0;
	}
	SetConnection(Workspace.IsSet() ? EScenarioConnection::Ready : EScenarioConnection::NeedsWorkspace);
	if (!Workspace.IsSet())
	{
		return;
	}
	CatalogPtr->RefreshIfNeeded(Scope());
	RefreshUsage();
	CheckUnresolved();
	// Finished jobs whose files never arrived (editor closed mid-download).
	if (Config.bAutoDownload)
	{
		for (const FScenarioJobRow& Row : LedgerPtr->Rows())
		{
			if (Row.State == EScenarioJobState::Success && Row.AssetIds.Num() > 0 && !Row.bImported)
			{
				bool bMissing = Row.Files.Num() < Row.AssetIds.Num();
				for (const FScenarioJobFile& File : Row.Files)
				{
					bMissing |= File.LocalPath.IsEmpty() || !FPaths::FileExists(File.LocalPath);
				}
				if (bMissing)
				{
					DownloadFiles(Row.LocalId);
				}
			}
		}
	}
}

FString FScenarioSession::Scope() const
{
	if (AuthMode == EScenarioAuthMode::ApiKey)
	{
		return FScenarioQuotes::WorkspaceScope(FString(), FString());
	}
	return FScenarioQuotes::WorkspaceScope(Workspace.TeamId, Workspace.ProjectId);
}

void FScenarioSession::LoadWorkspace()
{
	if (Config.StateDir.IsEmpty())
	{
		return;
	}
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Config.StateDir, TEXT("workspace.json"))))
	{
		return;
	}
	const TSharedPtr<FJsonObject> Json = ScenarioJson::ParseObject(Text);
	Workspace.TeamId = ScenarioJson::GetString(Json, TEXT("team_id"));
	Workspace.TeamName = ScenarioJson::GetString(Json, TEXT("team_name"));
	Workspace.ProjectId = ScenarioJson::GetString(Json, TEXT("project_id"));
	Workspace.ProjectName = ScenarioJson::GetString(Json, TEXT("project_name"));
}

void FScenarioSession::SaveWorkspace() const
{
	if (Config.StateDir.IsEmpty())
	{
		return;
	}
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("team_id"), Workspace.TeamId);
	Json->SetStringField(TEXT("team_name"), Workspace.TeamName);
	Json->SetStringField(TEXT("project_id"), Workspace.ProjectId);
	Json->SetStringField(TEXT("project_name"), Workspace.ProjectName);
	FFileHelper::SaveStringToFile(ScenarioJson::Write(Json, true), *FPaths::Combine(Config.StateDir, TEXT("workspace.json")));
}

void FScenarioSession::RefreshUsage()
{
	if (!IsSignedIn() || Workspace.ProjectId.IsEmpty())
	{
		return;
	}
	LastUsageRefresh = Now();
	const FDateTime Today = FDateTime::UtcNow();
	const FString Start = FString::Printf(TEXT("%04d-%02d-01T00:00:00Z"), Today.GetYear(), Today.GetMonth());
	const FString End = ScenarioIsoTime(Now());
	TWeakPtr<FScenarioSession> WeakThis = AsShared();
	const FString ProjectId = Workspace.ProjectId;
	ApiPtr->UsageTotal(ProjectId, Start, End, [WeakThis, ProjectId](const TScenarioResult<double>& Result)
	{
		const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
		if (!This.IsValid() || This->Workspace.ProjectId != ProjectId)
		{
			return;
		}
		if (Result.bOk)
		{
			This->UsageThisMonth = Result.Value;
			This->OnChanged.Broadcast();
		}
		else
		{
			UE_LOG(LogScenario, Verbose, TEXT("Usage unavailable: %s"), *Result.Error.ToString());
		}
	});
}

FString FScenarioSession::Fingerprint(const FScenarioRunRequest& Request) const
{
	return FScenarioQuotes::Fingerprint(Request.QuoteTarget(), Request.Parameters, Scope());
}

const FScenarioQuote* FScenarioSession::QuoteFor(const FScenarioRunRequest& Request) const
{
	return QuoteBook.Usable(Fingerprint(Request), Now());
}

bool FScenarioSession::IsSpendingDisabled() const
{
	if (Client.IsValid())
	{
		return Client->bSpendingDisabled;
	}
	return Config.SpendingDisabled.IsSet() ? Config.SpendingDisabled.GetValue() : FPlatformMisc::GetEnvironmentVariable(TEXT("SCENARIO_NO_SPEND")) == TEXT("1");
}

bool FScenarioSession::IsEstimateAlarmRaised() const
{
	return Backend.IsValid() && Backend->IsEstimateAlarmRaised();
}

void FScenarioSession::Price(const FScenarioRunRequest& Request, bool bAutomatic, TFunction<void(const TScenarioResult<FScenarioQuote>&)> OnDone)
{
	using FResult = TScenarioResult<FScenarioQuote>;
	if (!IsReady())
	{
		OnDone(FResult::Fail(SessionError(EScenarioErrorKind::Auth, TEXT("Sign in to Scenario and choose a project first."))));
		return;
	}
	if (Request.TargetId.IsEmpty())
	{
		OnDone(FResult::Fail(SessionError(EScenarioErrorKind::Invalid, TEXT("Choose a model first."))));
		return;
	}
	if (bAutomatic && IsEstimateAlarmRaised())
	{
		OnDone(FResult::Fail(SessionError(EScenarioErrorKind::EstimateJob,
			TEXT("Automatic price checks are off for this session because a price check returned a job. Check the price by hand to continue."))));
		return;
	}
	if (const FScenarioQuote* Existing = QuoteFor(Request))
	{
		OnDone(FResult::Ok(*Existing));
		return;
	}

	const EScenarioQuoteKind Kind = Request.Kind;
	const FString Target = Request.QuoteTarget();
	const FString QuoteScope = Scope();
	const TSharedPtr<FJsonObject> Params = ScenarioJson::CloneObject(Request.Parameters.IsValid() ? Request.Parameters : MakeShared<FJsonObject>());
	TWeakPtr<FScenarioSession> WeakThis = AsShared();
	auto Finish = [WeakThis, Kind, Target, QuoteScope, Params, OnDone](const TScenarioResult<FScenarioEstimate>& Result)
	{
		const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		if (!Result.bOk)
		{
			OnDone(FResult::Fail(Result.Error));
			return;
		}
		if (This->Scope() != QuoteScope)
		{
			OnDone(FResult::Fail(SessionError(EScenarioErrorKind::Invalid, TEXT("The project changed while the price was checked. Check the price again."))));
			return;
		}
		const FScenarioQuote& Quote = This->QuoteBook.Store(Kind, Target, Params, QuoteScope, Result.Value.CU, Result.Value.Discount, This->Now());
		OnDone(FResult::Ok(Quote));
	};

	switch (Kind)
	{
	case EScenarioQuoteKind::Workflow:
		ApiPtr->EstimateWorkflow(Request.TargetId, Params, Finish);
		break;
	case EScenarioQuoteKind::PromptSpark:
		ApiPtr->EstimatePromptSpark(Request.TargetId, ScenarioJson::GetString(Params, TEXT("prompt")), ScenarioJson::GetStringArray(Params, TEXT("images")),
			static_cast<int32>(ScenarioJson::GetNumber(Params, TEXT("num_results"), 1.0)), Finish);
		break;
	default:
		ApiPtr->Estimate(Request.TargetId, Params, Finish);
		break;
	}
}

FString FScenarioSession::CanSubmit(const FScenarioRunRequest& Request, const FString& QuoteId, const FString& ApprovedExact) const
{
	if (!IsReady())
	{
		return TEXT("Sign in to Scenario and choose a project first.");
	}
	if (IsSpendingDisabled())
	{
		return TEXT("Spending is turned off (SCENARIO_NO_SPEND=1), so nothing was sent.");
	}
	if (Request.Source == TEXT("agent") && (QuoteId.IsEmpty() || ApprovedExact.IsEmpty()))
	{
		return TEXT("Agents must pass quote_id and approved_cost from estimate_cost.");
	}
	const FString Fp = Fingerprint(Request);
	const double Time = Now();
	const FScenarioQuote* Quote = QuoteId.IsEmpty() ? QuoteBook.Usable(Fp, Time) : QuoteBook.FindById(QuoteId, Time);
	if (!Quote)
	{
		return QuoteId.IsEmpty()
			? TEXT("Check the price first: there is no current price for this exact request.")
			: TEXT("This quote is unknown, used or expired (quotes last 120 seconds). Price the request again.");
	}
	if (Quote->Fingerprint != Fp)
	{
		return TEXT("The request changed after it was priced. Price it again.");
	}
	if (!ApprovedExact.IsEmpty() && ApprovedExact.TrimStartAndEnd() != Quote->Exact)
	{
		return FString::Printf(TEXT("approved_cost must be the quote's exact cost \"%s\"."), *Quote->Exact);
	}
	if (LedgerPtr->Blocks(Fp))
	{
		return TEXT("An identical request may already be running on Scenario. Resolve it in Jobs first: Check, or Mark as not sent in the card's menu.");
	}
	return FString();
}

void FScenarioSession::Submit(const FScenarioRunRequest& Request, const FString& QuoteId, const FString& ApprovedExact, TFunction<void(const TScenarioResult<FString>&)> OnDone)
{
	using FResult = TScenarioResult<FString>;
	const FString Problem = CanSubmit(Request, QuoteId, ApprovedExact);
	if (!Problem.IsEmpty())
	{
		const EScenarioErrorKind Kind = !IsReady() ? EScenarioErrorKind::Auth : IsSpendingDisabled() ? EScenarioErrorKind::Local : EScenarioErrorKind::Invalid;
		OnDone(FResult::Fail(SessionError(Kind, Problem)));
		return;
	}

	const FString Fp = Fingerprint(Request);
	const double Time = Now();
	FScenarioQuote Quote;
	const bool bTaken = QuoteId.IsEmpty() ? QuoteBook.Consume(Fp, Time, Quote) : QuoteBook.ConsumeById(QuoteId, Time, Quote);
	if (!bTaken)
	{
		OnDone(FResult::Fail(SessionError(EScenarioErrorKind::Invalid, TEXT("The price expired a moment ago. Check it again."))));
		return;
	}

	const TSharedPtr<FJsonObject> Params = ScenarioJson::CloneObject(Request.Parameters.IsValid() ? Request.Parameters : MakeShared<FJsonObject>());
	FScenarioJobRow Row;
	Row.Kind = KindName(Request.Kind);
	Row.Lane = Request.Lane;
	if (Request.Kind == EScenarioQuoteKind::Workflow)
	{
		Row.WorkflowId = Request.TargetId;
	}
	else
	{
		Row.ModelId = Request.TargetId;
	}
	Row.ModelName = Request.TargetName;
	Row.Prompt = PromptOf(Params);
	Row.Parameters = Params;
	CollectAssetIds(Params, Row.InputAssetIds);
	Row.Fingerprint = Fp;
	Row.QuotedCU = Quote.CU;
	Row.QuotedExact = Quote.Exact;
	Row.Source = Request.Source;
	Row.Client = Request.Client;
	Row.TeamId = Workspace.TeamId;
	Row.ProjectId = Workspace.ProjectId;
	Row.Destination = Request.Destination;

	// The intent reaches the disk before the paid request leaves.
	const FString LocalId = LedgerPtr->AddIntent(Row, Time);
	if (!LedgerPtr->Save())
	{
		LedgerPtr->MarkRejected(LocalId, TEXT("The job list could not be written, so nothing was sent."), Now());
		OnDone(FResult::Fail(SessionError(EScenarioErrorKind::Local, TEXT("The job list in Saved/Scenario could not be written, so nothing was sent."))));
		return;
	}

	TWeakPtr<FScenarioSession> WeakThis = AsShared();
	auto Fail = [WeakThis, LocalId, OnDone](const FScenarioError& Error)
	{
		const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
		if (This.IsValid())
		{
			if (!Error.bNotSent && (Error.bAmbiguous || FScenarioErrors::IsAmbiguousKind(Error.Kind)))
			{
				This->LedgerPtr->MarkUnknown(LocalId, TEXT("We do not know if Scenario received this request. ") + Error.Message, This->Now());
			}
			else
			{
				This->LedgerPtr->MarkRejected(LocalId, Error.Message, This->Now());
			}
		}
		FResult Result = FResult::Fail(Error);
		Result.Value = LocalId;
		OnDone(Result);
	};
	auto Accepted = [WeakThis, LocalId, OnDone, Fail](const TScenarioResult<FScenarioSubmitted>& Submitted)
	{
		if (!Submitted.bOk)
		{
			Fail(Submitted.Error);
			return;
		}
		const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
		if (This.IsValid())
		{
			This->LedgerPtr->MarkSubmitted(LocalId, Submitted.Value.JobId, Submitted.Value.Status, This->Now());
			This->LastPolled.Add(LocalId, This->Now());
			This->AfterJobUpdate(LocalId);
		}
		OnDone(FResult::Ok(LocalId));
	};

	UE_LOG(LogScenario, Log, TEXT("Sending %s %s (%s CU quoted, %s)."), *Row.Kind, *Request.TargetId, *Quote.Exact, *Request.Source);
	switch (Request.Kind)
	{
	case EScenarioQuoteKind::Workflow:
		ApiPtr->SubmitWorkflow(Request.TargetId, Params, Accepted);
		break;
	case EScenarioQuoteKind::PromptSpark:
		ApiPtr->RunPromptSpark(Request.TargetId, ScenarioJson::GetString(Params, TEXT("prompt")), ScenarioJson::GetStringArray(Params, TEXT("images")),
			static_cast<int32>(ScenarioJson::GetNumber(Params, TEXT("num_results"), 1.0)),
			[WeakThis, LocalId, OnDone, Fail](const FScenarioReply& Reply)
			{
				if (!Reply.bOk)
				{
					Fail(Reply.Error);
					return;
				}
				const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
				if (This.IsValid())
				{
					This->SparkResults.Add(LocalId, Reply.Value);
					This->LedgerPtr->MarkSubmitted(LocalId, FScenarioMcpClient::CreatedJobId(Reply.Value), TEXT("success"), This->Now());
				}
				OnDone(FResult::Ok(LocalId));
			});
		break;
	default:
		ApiPtr->Submit(Request.TargetId, Params, Accepted);
		break;
	}
}

TSharedPtr<FJsonObject> FScenarioSession::GetSparkResult(const FString& LocalId) const
{
	const TSharedPtr<FJsonObject>* Found = SparkResults.Find(LocalId);
	return Found ? *Found : nullptr;
}

void FScenarioSession::Poll(double Time)
{
	if (!IsSignedIn() || bPollPaused)
	{
		return;
	}
	TArray<TPair<FString, FString>> Due;
	for (const FScenarioJobRow& Row : LedgerPtr->Rows())
	{
		if (!Row.IsActive() || PollsInFlight.Contains(Row.LocalId))
		{
			continue;
		}
		const double* Last = LastPolled.Find(Row.LocalId);
		if (Last && Time - *Last < Config.PollIntervalSeconds)
		{
			continue;
		}
		Due.Add({ Row.LocalId, Row.JobId });
	}
	TWeakPtr<FScenarioSession> WeakThis = AsShared();
	for (const TPair<FString, FString>& Item : Due)
	{
		const FString LocalId = Item.Key;
		const FString JobId = Item.Value;
		PollsInFlight.Add(LocalId);
		LastPolled.Add(LocalId, Time);
		ApiPtr->GetJob(JobId, [WeakThis, LocalId, JobId](const TScenarioResult<FScenarioJobInfo>& Result)
		{
			const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
			if (!This.IsValid())
			{
				return;
			}
			This->PollsInFlight.Remove(LocalId);
			if (Result.bOk)
			{
				if (This->LedgerPtr->ApplyJob(Result.Value, This->Now()))
				{
					This->AfterJobUpdate(LocalId);
				}
				return;
			}
			switch (Result.Error.Kind)
			{
			case EScenarioErrorKind::Auth:
				This->bPollPaused = true;
				This->SetConnection(EScenarioConnection::SignedOut, SessionError(EScenarioErrorKind::Auth, TEXT("Your Scenario session ended. Sign in again."), false));
				break;
			case EScenarioErrorKind::NotFound:
			{
				FScenarioJobInfo Gone;
				Gone.JobId = JobId;
				Gone.RawStatus = TEXT("failure");
				Gone.Error = TEXT("Scenario no longer has this job.");
				This->LedgerPtr->ApplyJob(Gone, This->Now());
				This->AfterJobUpdate(LocalId);
				break;
			}
			default:
				// Offline or busy: try again a little later.
				This->LastPolled.Add(LocalId, This->Now() + 7.0);
				break;
			}
		});
	}
}

void FScenarioSession::AfterJobUpdate(const FString& LocalId)
{
	const FScenarioJobRow* Row = LedgerPtr->Find(LocalId);
	if (!Row)
	{
		return;
	}
	if (Row->IsFinal())
	{
		LastPolled.Remove(LocalId);
	}
	if (Row->State == EScenarioJobState::Success)
	{
		if (Now() - LastUsageRefresh > 20.0)
		{
			RefreshUsage();
		}
		if (Config.bAutoDownload && Row->AssetIds.Num() > 0 && !Row->bImported && !Downloads.Contains(LocalId))
		{
			bool bMissing = Row->Files.Num() < Row->AssetIds.Num();
			for (const FScenarioJobFile& File : Row->Files)
			{
				bMissing |= File.LocalPath.IsEmpty() && File.Error.IsEmpty();
			}
			if (bMissing)
			{
				DownloadFiles(LocalId);
			}
		}
	}
}

void FScenarioSession::CancelJob(const FString& LocalId, FScenarioReplyCallback OnDone)
{
	const FScenarioJobRow* Row = LedgerPtr->Find(LocalId);
	if (!Row || !Row->IsActive())
	{
		if (OnDone)
		{
			OnDone(FScenarioReply::Fail(EScenarioErrorKind::Invalid, TEXT("This job is not running.")));
		}
		return;
	}
	TWeakPtr<FScenarioSession> WeakThis = AsShared();
	ApiPtr->CancelJob(Row->JobId, [WeakThis, LocalId, OnDone](const FScenarioReply& Reply)
	{
		const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
		if (This.IsValid())
		{
			if (Reply.bOk)
			{
				This->LedgerPtr->MarkCancelRequested(LocalId, This->Now());
				This->LastPolled.Remove(LocalId);
			}
			else
			{
				This->LedgerPtr->SetError(LocalId, TEXT("Cancel failed: ") + Reply.Error.Message, This->Now());
			}
		}
		if (OnDone)
		{
			OnDone(Reply);
		}
	});
}

void FScenarioSession::CheckUnresolved(TFunction<void(int32 Linked, const FScenarioError& Error)> OnDone)
{
	if (LedgerPtr->Unresolved().Num() == 0)
	{
		if (OnDone)
		{
			OnDone(0, FScenarioError());
		}
		return;
	}
	TWeakPtr<FScenarioSession> WeakThis = AsShared();
	ApiPtr->ListJobs(50, [WeakThis, OnDone](const TScenarioResult<TArray<FScenarioJobInfo>>& Result)
	{
		const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		if (!Result.bOk)
		{
			if (OnDone)
			{
				OnDone(0, Result.Error);
			}
			return;
		}
		const int32 Linked = This->LedgerPtr->Reconcile(Result.Value, This->Now());
		if (Linked > 0)
		{
			TArray<FString> Ids;
			for (const FScenarioJobRow& Row : This->LedgerPtr->Rows())
			{
				Ids.Add(Row.LocalId);
			}
			for (const FString& Id : Ids)
			{
				This->AfterJobUpdate(Id);
			}
		}
		if (OnDone)
		{
			OnDone(Linked, FScenarioError());
		}
	});
}

void FScenarioSession::MarkNotSent(const FString& LocalId)
{
	LedgerPtr->MarkNotSent(LocalId, Now());
}

void FScenarioSession::Dismiss(const FString& LocalId)
{
	LedgerPtr->Dismiss(LocalId, Now());
	LastPolled.Remove(LocalId);
}

void FScenarioSession::Recover(const FScenarioJobInfo& Job, const FString& Lane)
{
	const FString LocalId = LedgerPtr->AddRecovered(Job, Lane, Now());
	AfterJobUpdate(LocalId);
}

FString FScenarioSession::DownloadDir(const FString& LocalId) const
{
	const FScenarioJobRow* Row = LedgerPtr->Find(LocalId);
	const FString Folder = Row && !Row->JobId.IsEmpty() ? Row->JobId : LocalId;
	return FPaths::Combine(Config.StateDir.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("Scenario") : Config.StateDir, TEXT("Downloads"), ScenarioFiles::SafeObjectName(Folder));
}

bool FScenarioSession::IsDownloading(const FString& LocalId) const
{
	return Downloads.Contains(LocalId);
}

float FScenarioSession::DownloadProgress(const FString& LocalId) const
{
	const FDownloadJob* Job = Downloads.Find(LocalId);
	if (!Job)
	{
		return -1.0f;
	}
	int64 Done = 0;
	int64 Total = 0;
	for (int32 Index = 0; Index < Job->Done.Num(); ++Index)
	{
		Done += Job->Done[Index];
		Total += FMath::Max<int64>(Job->Total[Index], 0);
	}
	return Total > 0 ? static_cast<float>(static_cast<double>(Done) / static_cast<double>(Total)) : 0.0f;
}

void FScenarioSession::DownloadFiles(const FString& LocalId)
{
	FScenarioJobRow* Row = LedgerPtr->Find(LocalId);
	if (!Row || Row->AssetIds.Num() == 0 || Downloads.Contains(LocalId))
	{
		return;
	}
	TArray<FScenarioJobFile> Files = Row->Files;
	for (const FString& AssetId : Row->AssetIds)
	{
		if (!Files.ContainsByPredicate([&AssetId](const FScenarioJobFile& File) { return File.AssetId == AssetId; }))
		{
			FScenarioJobFile File;
			File.AssetId = AssetId;
			Files.Add(File);
		}
	}
	FDownloadJob Job;
	Job.Done.Init(0, Files.Num());
	Job.Total.Init(0, Files.Num());
	TArray<int32> Needed;
	for (int32 Index = 0; Index < Files.Num(); ++Index)
	{
		if (Files[Index].LocalPath.IsEmpty() || !FPaths::FileExists(Files[Index].LocalPath))
		{
			Files[Index].LocalPath.Reset();
			Files[Index].Error.Reset();
			Needed.Add(Index);
		}
	}
	LedgerPtr->SetFiles(LocalId, Files, Now());
	if (Needed.Num() == 0)
	{
		OnFilesReady.Broadcast(LocalId);
		return;
	}
	Job.Remaining = Needed.Num();
	Downloads.Add(LocalId, Job);
	for (const int32 Index : Needed)
	{
		DownloadQueue.Add({ LocalId, Index });
	}
	PumpDownloads();
}

void FScenarioSession::PumpDownloads()
{
	while (ActiveDownloads < FMath::Max(1, Config.MaxConcurrentDownloads) && DownloadQueue.Num() > 0)
	{
		const TPair<FString, int32> Next = DownloadQueue[0];
		DownloadQueue.RemoveAt(0);
		++ActiveDownloads;
		StartFileDownload(Next.Key, Next.Value, 0);
	}
}

void FScenarioSession::StartFileDownload(const FString& LocalId, int32 FileIndex, int32 UrlAttempt)
{
	const FScenarioJobRow* Row = LedgerPtr->Find(LocalId);
	if (!Row || !Row->Files.IsValidIndex(FileIndex))
	{
		FScenarioJobFile Missing;
		Missing.Error = TEXT("The job left the list.");
		FinishFile(LocalId, FileIndex, Missing);
		return;
	}
	const FScenarioJobFile File = Row->Files[FileIndex];
	const FString Lane = Row->Lane;
	TWeakPtr<FScenarioSession> WeakThis = AsShared();

	// Step 1: what the file is (kind, mime, role), once.
	if (File.Kind.IsEmpty() && File.MimeType.IsEmpty())
	{
		ApiPtr->GetAsset(File.AssetId, [WeakThis, LocalId, FileIndex, UrlAttempt, File](const TScenarioResult<FScenarioAssetInfo>& Info)
		{
			const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
			if (!This.IsValid())
			{
				return;
			}
			FScenarioJobFile Updated = File;
			if (!Info.bOk)
			{
				Updated.Error = Info.Error.Kind == EScenarioErrorKind::NotFound ? TEXT("Scenario no longer has this file.") : Info.Error.Message;
				This->FinishFile(LocalId, FileIndex, Updated);
				return;
			}
			Updated.Kind = Info.Value.Kind.IsEmpty() ? TEXT("file") : Info.Value.Kind;
			Updated.MimeType = Info.Value.MimeType;
			Updated.Role = ScenarioFiles::RoleFor(Info.Value.Type);
			if (FScenarioJobRow* Current = This->LedgerPtr->Find(LocalId))
			{
				if (Current->Files.IsValidIndex(FileIndex))
				{
					TArray<FScenarioJobFile> Files = Current->Files;
					Files[FileIndex] = Updated;
					This->LedgerPtr->SetFiles(LocalId, Files, This->Now());
				}
			}
			This->StartFileDownload(LocalId, FileIndex, UrlAttempt);
		});
		return;
	}

	// Step 2: a fresh signed URL, then the bytes.
	FString Format;
	if (File.Kind == TEXT("3d"))
	{
		const FScenarioLane* LaneInfo = FScenarioLanes::Get().Find(Lane);
		Format = LaneInfo && !LaneInfo->DownloadFormat.IsEmpty() ? LaneInfo->DownloadFormat : TEXT("glb");
	}
	FString Extension = Format.IsEmpty() ? ScenarioFiles::ExtensionForMime(File.MimeType) : Format;
	if (Extension.IsEmpty())
	{
		Extension = TEXT("bin");
	}
	const FString Role = File.Role.IsEmpty() || File.Role == TEXT("main") ? FString() : TEXT("_") + File.Role;
	const FString Destination = FPaths::Combine(DownloadDir(LocalId), ScenarioFiles::SafeObjectName(File.AssetId) + Role + TEXT(".") + Extension);

	ApiPtr->DownloadUrl(File.AssetId, Format, [WeakThis, LocalId, FileIndex, UrlAttempt, File, Destination](const TScenarioResult<FString>& Url)
	{
		const TSharedPtr<FScenarioSession> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		if (!Url.bOk)
		{
			FScenarioJobFile Failed = File;
			Failed.Error = Url.Error.Kind == EScenarioErrorKind::NotFound ? TEXT("Scenario no longer has this file.") : Url.Error.Message;
			This->FinishFile(LocalId, FileIndex, Failed);
			return;
		}
		auto OnProgress = [WeakThis, LocalId, FileIndex](int64 Done, int64 Total)
		{
			const TSharedPtr<FScenarioSession> Inner = WeakThis.Pin();
			FDownloadJob* Job = Inner.IsValid() ? Inner->Downloads.Find(LocalId) : nullptr;
			if (Job && Job->Done.IsValidIndex(FileIndex))
			{
				Job->Done[FileIndex] = Done;
				Job->Total[FileIndex] = Total;
			}
		};
		This->Downloader->Download(Url.Value, Destination, OnProgress, [WeakThis, LocalId, FileIndex, UrlAttempt, File](const FScenarioDownloadResult& Result)
		{
			const TSharedPtr<FScenarioSession> Inner = WeakThis.Pin();
			if (!Inner.IsValid())
			{
				return;
			}
			FScenarioJobFile Updated = File;
			if (Result.bOk)
			{
				Updated.LocalPath = Result.Path;
				Updated.Size = Result.Size;
				Updated.Sha256 = Result.Sha256;
				Updated.Error.Reset();
				// Unknown type: name the file after its first bytes so the importer can read it.
				if (FPaths::GetExtension(Result.Path) == TEXT("bin"))
				{
					const FString Sniffed = ReadHeadExtension(Result.Path);
					if (!Sniffed.IsEmpty())
					{
						const FString Renamed = FPaths::ChangeExtension(Result.Path, Sniffed);
						if (IFileManager::Get().Move(*Renamed, *Result.Path, true))
						{
							Updated.LocalPath = Renamed;
						}
					}
				}
				Inner->FinishFile(LocalId, FileIndex, Updated);
				return;
			}
			if (Result.bUrlExpired && UrlAttempt < 2)
			{
				// Signed URLs are short-lived: ask for a fresh one and resume from the .part file.
				Inner->StartFileDownload(LocalId, FileIndex, UrlAttempt + 1);
				return;
			}
			if (Result.PartialBytes > 0 && Result.TotalBytes > 0)
			{
				Updated.Error = FString::Printf(TEXT("Download stopped at %d%%."), FMath::RoundToInt(100.0 * Result.PartialBytes / Result.TotalBytes));
			}
			else
			{
				Updated.Error = Result.Error.Message.IsEmpty() ? TEXT("Download failed.") : Result.Error.Message;
			}
			Inner->FinishFile(LocalId, FileIndex, Updated);
		});
	});
}

void FScenarioSession::FinishFile(const FString& LocalId, int32 FileIndex, const FScenarioJobFile& File)
{
	ActiveDownloads = FMath::Max(0, ActiveDownloads - 1);
	if (FScenarioJobRow* Row = LedgerPtr->Find(LocalId))
	{
		if (Row->Files.IsValidIndex(FileIndex))
		{
			TArray<FScenarioJobFile> Files = Row->Files;
			Files[FileIndex] = File;
			LedgerPtr->SetFiles(LocalId, Files, Now());
		}
	}
	FDownloadJob* Job = Downloads.Find(LocalId);
	if (Job)
	{
		Job->bFailed |= !File.Error.IsEmpty();
		if (--Job->Remaining <= 0)
		{
			const bool bFailed = Job->bFailed;
			Downloads.Remove(LocalId);
			if (bFailed)
			{
				FString First;
				if (const FScenarioJobRow* Row = LedgerPtr->Find(LocalId))
				{
					for (const FScenarioJobFile& Each : Row->Files)
					{
						if (!Each.Error.IsEmpty())
						{
							First = Each.Error;
							break;
						}
					}
				}
				LedgerPtr->SetError(LocalId, First + TEXT(" Resume from Jobs."), Now());
			}
			else
			{
				LedgerPtr->SetError(LocalId, FString(), Now());
				OnFilesReady.Broadcast(LocalId);
			}
		}
	}
	PumpDownloads();
}

void FScenarioSession::Upload(const FString& Path, TFunction<void(const TScenarioResult<FString>&)> OnDone, TFunction<void(float)> OnProgress)
{
	using FResult = TScenarioResult<FString>;
	const FString Extension = FPaths::GetExtension(Path).ToLower();
	const FString Kind = ScenarioFiles::UploadKindForExtension(Extension);
	if (Kind.IsEmpty())
	{
		OnDone(FResult::Fail(SessionError(EScenarioErrorKind::Invalid, FString::Printf(TEXT("A .%s file cannot be sent to Scenario."), *Extension))));
		return;
	}
	const FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
	if (!Stat.bIsValid || Stat.FileSize <= 0)
	{
		OnDone(FResult::Fail(SessionError(EScenarioErrorKind::Local, TEXT("The file to upload is missing or empty."))));
		return;
	}
	const FString CacheKey = FString::Printf(TEXT("%s|%s|%lld|%s"), *Scope(), *FPaths::ConvertRelativePathToFull(Path), static_cast<long long>(Stat.FileSize), *Stat.ModificationTime.ToIso8601());
	if (const FString* Known = UploadCache.Find(CacheKey))
	{
		OnDone(FResult::Ok(*Known));
		return;
	}
	TWeakPtr<FScenarioSession> WeakThis = AsShared();
	Uploader->Upload(Path, Kind, ScenarioFiles::MimeForExtension(Extension),
		[OnProgress](int64 Done, int64 Total)
		{
			if (OnProgress && Total > 0)
			{
				OnProgress(static_cast<float>(static_cast<double>(Done) / static_cast<double>(Total)));
			}
		},
		[WeakThis, CacheKey, OnDone](const FScenarioUploadResult& Result)
		{
			if (!Result.bOk)
			{
				OnDone(FResult::Fail(Result.Error));
				return;
			}
			if (const TSharedPtr<FScenarioSession> This = WeakThis.Pin())
			{
				This->UploadCache.Add(CacheKey, Result.AssetId);
			}
			OnDone(FResult::Ok(Result.AssetId));
		});
}
