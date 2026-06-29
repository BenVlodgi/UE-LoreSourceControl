// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlProvider.h"
#include "LoreSourceControlModule.h"
#include "LoreSourceControlCommand.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlOperations.h"
#include "LoreSourceControlDownload.h"
#include "ISourceControlModule.h"
#include "SourceControlOperations.h"
#include "SourceControlHelpers.h"
#include "ScopedSourceControlProgress.h"
#include "Misc/Paths.h"
#include "Misc/QueuedThreadPool.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Async/Async.h"
#include "UObject/Package.h"
#if LORE_UE5_0_OR_LATER
#include "UObject/ObjectSaveContext.h"
#endif

#if SOURCE_CONTROL_WITH_SLATE
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#endif

#define LOCTEXT_NAMESPACE "LoreSourceControl.Provider"

void FLoreSourceControlProvider::Init(bool bForceConnection)
{
	FLoreSourceControlModule& Module = FLoreSourceControlModule::Get();
	FString StoredPath = Module.GetSettings().GetBinaryPath();
	if (StoredPath.IsEmpty())
	{
		// Discover the binary and write it back, relative when bundled, so the field shows a resolved path.
		StoredPath = LoreSourceControlUtils::MakeBinaryPathRelativeToPlugin(LoreSourceControlUtils::FindLoreBinaryPath());
		if (!StoredPath.IsEmpty())
		{
			Module.AccessSettings().SetBinaryPath(StoredPath);
			Module.AccessSettings().SaveSettings();
		}
	}
	PathToLoreBinary = LoreSourceControlUtils::ResolveBinaryPath(StoredPath);

	const ELoreBackendMode Mode = Module.GetSettings().GetBackendMode();
	bLoreAvailable = LoreSourceControlUtils::CheckLoreAvailability(Mode, PathToLoreBinary, LoreVersion);

	bLoreRepositoryFound = LoreSourceControlUtils::GetRepositoryRoot(FPaths::ProjectDir(), PathToRepositoryRoot);
	if (bLoreRepositoryFound)
	{
		// Read identity and remote before any status query, so our own locks attribute right.
		LoreSourceControlUtils::GetRepositoryConfig(PathToRepositoryRoot, RemoteUrl, Identity);
		LoreSourceControlUtils::GetBranchName(Mode, PathToLoreBinary, PathToRepositoryRoot, BranchName);
	}

	if (!OnPackageSavedHandle.IsValid())
	{
#if LORE_UE5_0_OR_LATER
		OnPackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddRaw(this, &FLoreSourceControlProvider::HandlePackageSaved);
#else
		OnPackageSavedHandle = UPackage::PackageSavedEvent.AddRaw(this, &FLoreSourceControlProvider::HandlePackageSaved);
#endif
	}
}

void FLoreSourceControlProvider::Close()
{
	if (OnPackageSavedHandle.IsValid())
	{
#if LORE_UE5_0_OR_LATER
		UPackage::PackageSavedWithContextEvent.Remove(OnPackageSavedHandle);
#else
		UPackage::PackageSavedEvent.Remove(OnPackageSavedHandle);
#endif
		OnPackageSavedHandle.Reset();
	}

	StateCache.Empty();
	bLoreAvailable = false;
	bLoreRepositoryFound = false;
}

#if LORE_UE5_0_OR_LATER
void FLoreSourceControlProvider::HandlePackageSaved(const FString& InPackageFilename, UPackage* InPackage, FObjectPostSaveContext InObjectSaveContext)
#else
void FLoreSourceControlProvider::HandlePackageSaved(const FString& InPackageFilename, UObject* InOuter)
#endif
{
	// Only a file already cached as unchanged is flipped; others have no cached unchanged state and are ignored.
	const FString Absolute = FPaths::ConvertRelativePathToFull(InPackageFilename);
	const TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> State = GetStateInternal(Absolute);
	if (State->WorkingCopyState == ELoreWorkingCopyState::Unchanged)
	{
		State->WorkingCopyState = ELoreWorkingCopyState::Modified;
		OnSourceControlStateChanged.Broadcast();
	}
}

const FName& FLoreSourceControlProvider::GetName() const
{
	return ProviderName;
}

FText FLoreSourceControlProvider::GetStatusText() const
{
	FTextBuilder Builder;
	Builder.AppendLine(LOCTEXT("ProviderName", "Provider: Lore"));
	Builder.AppendLine(FText::Format(LOCTEXT("Available", "Client: {0}"), FText::FromString(bLoreAvailable ? LoreVersion : TEXT("not found"))));
	Builder.AppendLine(FText::Format(LOCTEXT("Root", "Repository: {0}"), FText::FromString(PathToRepositoryRoot)));
	Builder.AppendLine(FText::Format(LOCTEXT("Branch", "Branch: {0}"), FText::FromString(BranchName)));
	return Builder.ToText();
}

#if LORE_UE5_3_OR_LATER
TMap<ISourceControlProvider::EStatus, FString> FLoreSourceControlProvider::GetStatus() const
{
	TMap<EStatus, FString> Result;
	Result.Add(EStatus::Enabled, IsEnabled() ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::Connected, IsAvailable() ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::Repository, PathToRepositoryRoot);
	Result.Add(EStatus::Remote, RemoteUrl);
	Result.Add(EStatus::Branch, BranchName);
	Result.Add(EStatus::ScmVersion, LoreVersion);
	return Result;
}
#endif

bool FLoreSourceControlProvider::IsEnabled() const
{
	return true;
}

bool FLoreSourceControlProvider::IsAvailable() const
{
	return bLoreAvailable && bLoreRepositoryFound;
}

bool FLoreSourceControlProvider::QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest)
{
	return false;
}

void FLoreSourceControlProvider::RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRoot)
{
}

int32 FLoreSourceControlProvider::GetStateBranchIndex(const FString& InBranchName) const
{
	return INDEX_NONE;
}

#if LORE_UE5_7_OR_LATER
bool FLoreSourceControlProvider::GetStateBranchAtIndex(int32 BranchIndex, FString& OutBranchName) const
{
	return false;
}
#endif

ECommandResult::Type FLoreSourceControlProvider::GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	if (!IsAvailable())
	{
		return ECommandResult::Failed;
	}

	const TArray<FString> AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);

	if (InStateCacheUsage == EStateCacheUsage::ForceUpdate)
	{
		TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FUpdateStatus>();
#if LORE_UE5_0_OR_LATER
		Execute(Operation, FSourceControlChangelistPtr(), AbsoluteFiles, EConcurrency::Synchronous);
#else
		Execute(Operation, AbsoluteFiles, EConcurrency::Synchronous);
#endif
	}

	for (const FString& File : AbsoluteFiles)
	{
		OutState.Add(GetStateInternal(File));
	}

	return ECommandResult::Succeeded;
}

#if LORE_UE5_0_OR_LATER
ECommandResult::Type FLoreSourceControlProvider::GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	return ECommandResult::Failed;
}
#endif

TArray<FSourceControlStateRef> FLoreSourceControlProvider::GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const
{
	TArray<FSourceControlStateRef> Result;
	for (const auto& Pair : StateCache)
	{
		FSourceControlStateRef State = Pair.Value;
		if (Predicate(State))
		{
			Result.Add(State);
		}
	}

	return Result;
}

FDelegateHandle FLoreSourceControlProvider::RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged)
{
	return OnSourceControlStateChanged.Add(SourceControlStateChanged);
}

void FLoreSourceControlProvider::UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle)
{
	OnSourceControlStateChanged.Remove(Handle);
}

#if LORE_UE5_0_OR_LATER
ECommandResult::Type FLoreSourceControlProvider::Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate)
#else
ECommandResult::Type FLoreSourceControlProvider::Execute(const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe>& InOperation, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate)
#endif
{
	if (!WorkersMap.Contains(InOperation->GetName()))
	{
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	TSharedPtr<ILoreSourceControlWorker, ESPMode::ThreadSafe> Worker = CreateWorker(InOperation->GetName());
	if (!Worker.IsValid())
	{
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	FLoreSourceControlCommand* Command = new FLoreSourceControlCommand(InOperation, Worker.ToSharedRef(), InOperationCompleteDelegate);
	Command->Files = SourceControlHelpers::AbsoluteFilenames(InFiles);
#if LORE_UE5_0_OR_LATER
	Command->Changelist = InChangelist;
#endif

	if (InConcurrency == EConcurrency::Synchronous)
	{
		Command->bAutoDelete = false;
		return ExecuteSynchronousCommand(*Command, InOperation->GetInProgressString());
	}

	Command->bAutoDelete = true;
	return IssueCommand(*Command);
}

#if LORE_UE5_3_OR_LATER
bool FLoreSourceControlProvider::CanExecuteOperation(const FSourceControlOperationRef& InOperation) const
{
	return WorkersMap.Contains(InOperation->GetName());
}
#endif

bool FLoreSourceControlProvider::CanCancelOperation(const FSourceControlOperationRef& InOperation) const
{
	return false;
}

void FLoreSourceControlProvider::CancelOperation(const FSourceControlOperationRef& InOperation)
{
}

TArray<TSharedRef<ISourceControlLabel>> FLoreSourceControlProvider::GetLabels(const FString& InMatchingSpec) const
{
	return TArray<TSharedRef<ISourceControlLabel>>();
}

#if LORE_UE5_0_OR_LATER
TArray<FSourceControlChangelistRef> FLoreSourceControlProvider::GetChangelists(EStateCacheUsage::Type InStateCacheUsage)
{
	return TArray<FSourceControlChangelistRef>();
}
#endif

bool FLoreSourceControlProvider::UsesLocalReadOnlyState() const
{
	return FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();
}

bool FLoreSourceControlProvider::UsesChangelists() const
{
	return false;
}

#if LORE_UE5_2_OR_LATER
bool FLoreSourceControlProvider::UsesUncontrolledChangelists() const
{
	return true;
}
#endif

bool FLoreSourceControlProvider::UsesCheckout() const
{
	return FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();
}

#if LORE_UE5_1_OR_LATER
bool FLoreSourceControlProvider::UsesFileRevisions() const
{
	return true;
}
#endif

#if LORE_UE5_2_OR_LATER
bool FLoreSourceControlProvider::UsesSnapshots() const
{
	return false;
}

bool FLoreSourceControlProvider::AllowsDiffAgainstDepot() const
{
	return true;
}
#endif

#if LORE_UE5_8_OR_LATER
bool FLoreSourceControlProvider::UsesSoftRevertOnDelete() const
{
	return false;
}

TOptional<bool> FLoreSourceControlProvider::HasChangesToSync() const
{
	return TOptional<bool>();
}

TOptional<bool> FLoreSourceControlProvider::HasChangesToCheckIn() const
{
	return TOptional<bool>();
}
#endif

#if LORE_UE_HAS_LATEST_REVISION_QUERY
TOptional<bool> FLoreSourceControlProvider::IsAtLatestRevision() const
{
	return TOptional<bool>();
}

TOptional<int> FLoreSourceControlProvider::GetNumLocalChanges() const
{
	return TOptional<int>();
}
#endif

void FLoreSourceControlProvider::Tick()
{
	bool bStatesChanged = false;
	for (int32 Index = 0; Index < CommandQueue.Num(); )
	{
		FLoreSourceControlCommand* Command = CommandQueue[Index];
		if (Command->bExecuteProcessed.GetValue() > 0)
		{
			CommandQueue.RemoveAt(Index);
			bStatesChanged |= Command->Worker->UpdateStates();
			Command->ReturnResults();
			if (Command->bAutoDelete)
			{
				delete Command;
			}
		}
		else
		{
			++Index;
		}
	}

	if (bStatesChanged)
	{
		OnSourceControlStateChanged.Broadcast();
	}
}

#if SOURCE_CONTROL_WITH_SLATE
TSharedRef<SWidget> FLoreSourceControlProvider::MakeSettingsWidget() const
{
	auto InfoRow = [](const FText& Label, TFunction<FString()> Value) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(STextBlock).MinDesiredWidth(70.0f).Text(Label)
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text_Lambda([Value]() { const FString V = Value(); return V.IsEmpty() ? LOCTEXT("None", "(not connected)") : FText::FromString(V); })
		];
	};

	// One mutually-exclusive option in the backend selector.
	auto ModeOption = [](const FText& Label, ELoreBackendMode ThisMode) -> TSharedRef<SWidget>
	{
		return SNew(SCheckBox)
			.IsChecked_Lambda([ThisMode]() { return FLoreSourceControlModule::Get().GetSettings().GetBackendMode() == ThisMode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([ThisMode](ECheckBoxState NewState)
			{
				// Ignore the deselect click; selecting another option moves the choice.
				if (NewState != ECheckBoxState::Checked) { return; }
				FLoreSourceControlModule& Module = FLoreSourceControlModule::Get();
				Module.AccessSettings().SetBackendMode(ThisMode);
				Module.AccessSettings().SaveSettings();
				Module.GetProvider().Init();
			})
			[ SNew(STextBlock).Text(Label) ];
	};

	// Hold the token field's text for the sign-in handler; the shared buffer lives as long as the captured lambdas.
	TSharedRef<FString> TokenBuffer = MakeShared<FString>();
	TSharedRef<FString> TokenTypeBuffer = MakeShared<FString>(FString(TEXT("lore")));

	return SNew(SVerticalBox)
	// Connection information, discovered at connect.
	+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)
	[ InfoRow(LOCTEXT("ServerLabel", "Server"), []() { return FLoreSourceControlModule::Get().GetProvider().GetRemoteUrl(); }) ]
	+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)
	[ InfoRow(LOCTEXT("RepositoryLabel", "Repository"), []() { return FLoreSourceControlModule::Get().GetProvider().GetPathToRepositoryRoot(); }) ]
	+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)
	[ InfoRow(LOCTEXT("BranchLabel", "Branch"), []() { return FLoreSourceControlModule::Get().GetProvider().GetBranchName(); }) ]
	// Identity is editable: Lore resolves it once at create and has no config command, so the editor owns the override.
	+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(STextBlock).MinDesiredWidth(70.0f).Text(LOCTEXT("IdentityLabel", "Identity"))
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(SEditableTextBox)
			.HintText(LOCTEXT("IdentityHint", "Email recorded on commits"))
			.Text_Lambda([]()
			{
				const FString Override = FLoreSourceControlModule::Get().GetSettings().GetIdentity();
				return FText::FromString(Override.IsEmpty() ? FLoreSourceControlModule::Get().GetProvider().GetIdentity() : Override);
			})
			.OnTextCommitted_Lambda([](const FText& InText, ETextCommit::Type)
			{
				FLoreSourceControlModule& Module = FLoreSourceControlModule::Get();
				Module.AccessSettings().SetIdentity(InText.ToString());
				Module.AccessSettings().SaveSettings();
			})
		]
	]
	// Authentication: status, browser sign-in, and sign-out. Sign-in and sign-out run off the game thread, since the native browser flow blocks until it completes.
	+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 4.0f, 2.0f, 2.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(STextBlock).MinDesiredWidth(70.0f).Text(LOCTEXT("AuthLabel", "Authentication"))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(STextBlock).Text_Lambda([]()
			{
				const FLoreSourceControlProvider& Provider = FLoreSourceControlModule::Get().GetProvider();
				if (!Provider.IsRemoteAvailable())
				{
					return LOCTEXT("AuthUnreachable", "Server not reachable");
				}
				return Provider.IsRemoteAuthorized()
					? LOCTEXT("AuthSignedIn", "Signed in")
					: LOCTEXT("AuthRequired", "Required, not signed in");
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(SButton)
			.Visibility_Lambda([]()
			{
				const FLoreSourceControlProvider& Provider = FLoreSourceControlModule::Get().GetProvider();
				return (Provider.IsRemoteAvailable() && !Provider.IsRemoteAuthorized()) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.ToolTipText(LOCTEXT("LoginTip", "Sign in to the Lore server through a browser."))
			.OnClicked_Lambda([]()
			{
				FLoreSourceControlModule& Module = FLoreSourceControlModule::Get();
				const FLoreSourceControlProvider& Provider = Module.GetProvider();
				const ELoreBackendMode Mode = Module.GetSettings().GetBackendMode();
				const FString Binary = Provider.GetLoreBinaryPath();
				const FString Root = Provider.GetPathToRepositoryRoot();
				const FString Remote = Provider.GetRemoteUrl();
				FNotificationInfo Info(LOCTEXT("LoginStarting", "Opening a browser to sign in to Lore..."));
				Info.ExpireDuration = 4.0f;
				FSlateNotificationManager::Get().AddNotification(Info);
				AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Mode, Binary, Root, Remote]()
				{
					FString Error;
					const bool bOk = LoreSourceControlUtils::LoginInteractive(Mode, Binary, Root, Remote, Error);
					bool bAvailable = false, bAuthorized = false;
					if (bOk)
					{
						LoreSourceControlUtils::GetRemoteAuthState(Mode, Binary, Root, bAvailable, bAuthorized);
					}
					AsyncTask(ENamedThreads::GameThread, [bOk, Error, bAvailable, bAuthorized]()
					{
						if (!bOk)
						{
							FNotificationInfo Fail(FText::Format(LOCTEXT("LoginFail", "Lore sign-in failed: {0}"), FText::FromString(Error)));
							Fail.ExpireDuration = 6.0f;
							FSlateNotificationManager::Get().AddNotification(Fail);
						}
						else
						{
							FLoreSourceControlModule::Get().GetProvider().SetRemoteAuthState(bAvailable, bAuthorized);
						}
					});
				});
				return FReply::Handled();
			})
			[
				SNew(STextBlock).Text(LOCTEXT("Login", "Log in"))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton)
			.Visibility_Lambda([]()
			{
				return FLoreSourceControlModule::Get().GetProvider().IsRemoteAuthorized() ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.ToolTipText(LOCTEXT("LogoutTip", "Remove the stored Lore credentials."))
			.OnClicked_Lambda([]()
			{
				FLoreSourceControlModule& Module = FLoreSourceControlModule::Get();
				const FLoreSourceControlProvider& Provider = Module.GetProvider();
				const ELoreBackendMode Mode = Module.GetSettings().GetBackendMode();
				const FString Binary = Provider.GetLoreBinaryPath();
				const FString Root = Provider.GetPathToRepositoryRoot();
				AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Mode, Binary, Root]()
				{
					FString Error;
					LoreSourceControlUtils::Logout(Mode, Binary, Root, Error);
					bool bAvailable = false, bAuthorized = false;
					LoreSourceControlUtils::GetRemoteAuthState(Mode, Binary, Root, bAvailable, bAuthorized);
					AsyncTask(ENamedThreads::GameThread, [bAvailable, bAuthorized]()
					{
						FLoreSourceControlModule::Get().GetProvider().SetRemoteAuthState(bAvailable, bAuthorized);
					});
				});
				return FReply::Handled();
			})
			[
				SNew(STextBlock).Text(LOCTEXT("Logout", "Sign out"))
			]
		]
	]
	// Token sign-in: the non-interactive native path, for a server that issues tokens.
	+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)
	[
		SNew(SHorizontalBox)
		// Token sign-in applies only when the server is reachable and wants an auth the client lacks; hidden for an unreachable server or one with auth off.
		.Visibility_Lambda([]()
		{
			const FLoreSourceControlProvider& Provider = FLoreSourceControlModule::Get().GetProvider();
			return (Provider.IsRemoteAvailable() && !Provider.IsRemoteAuthorized()) ? EVisibility::Visible : EVisibility::Collapsed;
		})
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(STextBlock).MinDesiredWidth(70.0f).Text(LOCTEXT("TokenLabel", "Token"))
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(SEditableTextBox)
			.IsPassword(true)
			.HintText(LOCTEXT("TokenHint", "Paste a Lore auth token"))
			.OnTextChanged_Lambda([TokenBuffer](const FText& InText) { *TokenBuffer = InText.ToString(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(SBox).WidthOverride(70.0f)
			[
				SNew(SEditableTextBox)
				.ToolTipText(LOCTEXT("TokenTypeTip", "Token type: lore, api-key, or eg1."))
				.Text_Lambda([TokenTypeBuffer]() { return FText::FromString(*TokenTypeBuffer); })
				.OnTextChanged_Lambda([TokenTypeBuffer](const FText& InText) { *TokenTypeBuffer = InText.ToString(); })
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ToolTipText(LOCTEXT("TokenLoginTip", "Sign in with the pasted token. No browser."))
			.OnClicked_Lambda([TokenBuffer, TokenTypeBuffer]()
			{
				FLoreSourceControlModule& Module = FLoreSourceControlModule::Get();
				const FLoreSourceControlProvider& Provider = Module.GetProvider();
				const ELoreBackendMode Mode = Module.GetSettings().GetBackendMode();
				const FString Binary = Provider.GetLoreBinaryPath();
				const FString Root = Provider.GetPathToRepositoryRoot();
				const FString Remote = Provider.GetRemoteUrl();
				const FString Token = *TokenBuffer;
				const FString TokenType = TokenTypeBuffer->IsEmpty() ? FString(TEXT("lore")) : *TokenTypeBuffer;
				if (Token.IsEmpty())
				{
					FNotificationInfo Empty(LOCTEXT("TokenEmpty", "Paste a token first."));
					Empty.ExpireDuration = 4.0f;
					FSlateNotificationManager::Get().AddNotification(Empty);
					return FReply::Handled();
				}
				FNotificationInfo Info(LOCTEXT("TokenSigningIn", "Signing in to Lore with a token..."));
				Info.ExpireDuration = 4.0f;
				FSlateNotificationManager::Get().AddNotification(Info);
				AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Mode, Binary, Root, Remote, Token, TokenType]()
				{
					FString Error;
					const bool bOk = LoreSourceControlUtils::LoginWithToken(Mode, Binary, Root, Remote, Token, TokenType, Error);
					bool bAvailable = false, bAuthorized = false;
					if (bOk)
					{
						LoreSourceControlUtils::GetRemoteAuthState(Mode, Binary, Root, bAvailable, bAuthorized);
					}
					AsyncTask(ENamedThreads::GameThread, [bOk, Error, bAvailable, bAuthorized]()
					{
						FNotificationInfo Result(bOk ? LOCTEXT("TokenLoginOk", "Signed in to Lore.") : FText::Format(LOCTEXT("TokenLoginFail", "Token sign-in failed: {0}"), FText::FromString(Error)));
						Result.ExpireDuration = 6.0f;
						FSlateNotificationManager::Get().AddNotification(Result);
						if (bOk)
						{
							FLoreSourceControlModule::Get().GetProvider().SetRemoteAuthState(bAvailable, bAuthorized);
						}
					});
				});
				return FReply::Handled();
			})
			[
				SNew(STextBlock).Text(LOCTEXT("TokenLogin", "Sign in with token"))
			]
		]
	]
	// Settings
	+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 8.0f, 2.0f, 2.0f)
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([]() { return FLoreSourceControlModule::Get().GetSettings().IsUsingLocking() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([](ECheckBoxState NewState)
		{
			FLoreSourceControlModule& Module = FLoreSourceControlModule::Get();
			Module.AccessSettings().SetUsingLocking(NewState == ECheckBoxState::Checked);
			Module.AccessSettings().SaveSettings();
		})
		[
			SNew(STextBlock).Text(LOCTEXT("UseLocking", "Use file locking for check out"))
		]
	]
	+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([]() { return FLoreSourceControlModule::Get().GetSettings().IsPushAfterCommit() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([](ECheckBoxState NewState)
		{
			FLoreSourceControlModule& Module = FLoreSourceControlModule::Get();
			Module.AccessSettings().SetPushAfterCommit(NewState == ECheckBoxState::Checked);
			Module.AccessSettings().SaveSettings();
		})
		[
			SNew(STextBlock).Text(LOCTEXT("PushAfterCommit", "Push to server when submitting"))
		]
	]
	// Advanced: the backend selector, plus the client path and downloader. Folded away because the native default is what almost every project wants.
	+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 8.0f, 2.0f, 2.0f)
	[
		SNew(SExpandableArea)
		.InitiallyCollapsed(true)
		.AreaTitle(LOCTEXT("Advanced", "Advanced"))
		.BodyContent()
		[
			SNew(SVerticalBox)
			// Backend: which path operations run through. Native uses no command-line client.
			+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(STextBlock).MinDesiredWidth(70.0f).Text(LOCTEXT("BackendLabel", "Backend"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
				[ ModeOption(LOCTEXT("ModeNative", "Native library"), ELoreBackendMode::Native) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
				[ ModeOption(LOCTEXT("ModeCommandLine", "Command line"), ELoreBackendMode::CommandLine) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ ModeOption(LOCTEXT("ModeAutomatic", "Automatic"), ELoreBackendMode::Automatic) ]
			]
			// Client path and downloader are meaningless under the native default, where no command-line client runs.
			+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 8.0f, 2.0f, 2.0f)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([]() { return FLoreSourceControlModule::Get().GetSettings().GetBackendMode() != ELoreBackendMode::Native ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(STextBlock).MinDesiredWidth(70.0f).Text(LOCTEXT("LorePathLabel", "Lore Path"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SEditableTextBox)
					.Text_Lambda([]() { return FText::FromString(FLoreSourceControlModule::Get().GetSettings().GetBinaryPath()); })
					.OnTextCommitted_Lambda([](const FText& InText, ETextCommit::Type)
					{
						FLoreSourceControlModule& Module = FLoreSourceControlModule::Get();
						Module.AccessSettings().SetBinaryPath(InText.ToString());
						Module.AccessSettings().SaveSettings();
						Module.GetProvider().Init();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[
					SNew(SButton)
					.ToolTipText(LOCTEXT("DownloadClientTip", "Fetch the latest Lore CLI release from GitHub into the plugin."))
					.IsEnabled_Lambda([]() { return LoreSourceControlDownload::IsRunning() || !LoreSourceControlUtils::IsBundledClientPresent(); })
					.OnClicked_Lambda([]()
					{
						if (LoreSourceControlDownload::IsRunning())
						{
							LoreSourceControlDownload::Cancel();
						}
						else
						{
							LoreSourceControlDownload::Start();
						}
						return FReply::Handled();
					})
					[
						SNew(STextBlock).Text_Lambda([]() -> FText
						{
							if (LoreSourceControlDownload::IsRunning())
							{
								return LOCTEXT("CancelDownload", "Cancel download");
							}
							if (LoreSourceControlUtils::IsBundledClientPresent())
							{
								return LOCTEXT("ClientInstalled", "Lore CLI installed");
							}
							return LOCTEXT("DownloadLatest", "Download latest Lore CLI");
						})
					]
				]
			]
		]
	];
}
#endif

void FLoreSourceControlProvider::RegisterWorker(const FName& InName, const FGetLoreSourceControlWorker& InDelegate)
{
	WorkersMap.Add(InName, InDelegate);
}

void FLoreSourceControlProvider::SetRepositoryInfo(bool bInAvailable, bool bInRepositoryFound, const FString& InRepositoryRoot, const FString& InRemoteUrl, const FString& InIdentity, const FString& InBranchName, const FString& InRepositoryId, const FString& InLoreVersion)
{
	bLoreAvailable = bInAvailable;
	bLoreRepositoryFound = bInRepositoryFound;
	PathToRepositoryRoot = InRepositoryRoot;
	RemoteUrl = InRemoteUrl;
	Identity = InIdentity;
	BranchName = InBranchName;
	RepositoryId = InRepositoryId;
	LoreVersion = InLoreVersion;
}

void FLoreSourceControlProvider::SetRemoteAuthState(bool bInRemoteAvailable, bool bInRemoteAuthorized)
{
	bRemoteAvailable = bInRemoteAvailable;
	bRemoteAuthorized = bInRemoteAuthorized;
}

TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> FLoreSourceControlProvider::GetStateInternal(const FString& InFilename)
{
	if (TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>* Found = StateCache.Find(InFilename))
	{
		return *Found;
	}

	TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> NewState = MakeShared<FLoreSourceControlState, ESPMode::ThreadSafe>(InFilename);
	StateCache.Add(InFilename, NewState);
	return NewState;
}

TSharedPtr<ILoreSourceControlWorker, ESPMode::ThreadSafe> FLoreSourceControlProvider::CreateWorker(const FName& InOperationName) const
{
	if (const FGetLoreSourceControlWorker* Delegate = WorkersMap.Find(InOperationName))
	{
		return Delegate->Execute();
	}

	return nullptr;
}

ECommandResult::Type FLoreSourceControlProvider::ExecuteSynchronousCommand(FLoreSourceControlCommand& InCommand, const FText& Task)
{
	ECommandResult::Type Result = ECommandResult::Failed;

	// Run the worker on the pool and wait on the game thread, pumping Slate so the editor stays painted instead of freezing on an inline call, the way the engine Git and Perforce providers drive synchronous commands. The progress window only appears once an operation runs past a short threshold, so fast native operations stay silent while a slow one shows a cancellable dialog.
	{
		const double StartSeconds = FPlatformTime::Seconds();
		TUniquePtr<FScopedSourceControlProgress> Progress;

		IssueCommand(InCommand);

		while (InCommand.bExecuteProcessed.GetValue() == 0)
		{
			if (!Progress && FPlatformTime::Seconds() - StartSeconds > 0.5)
			{
				Progress = MakeUnique<FScopedSourceControlProgress>(Task);
			}

			Tick();
			if (Progress)
			{
				Progress->Tick();
			}
			FPlatformProcess::Sleep(0.01f);
		}

		// One more Tick to update states and clear the finished command from the queue.
		Tick();

		if (InCommand.bCommandSuccessful)
		{
			Result = ECommandResult::Succeeded;
		}
	}

	// A synchronous command is never auto-deleted; remove it from the queue if it is still there, then delete.
	check(!InCommand.bAutoDelete);
	if (CommandQueue.Contains(&InCommand))
	{
		CommandQueue.Remove(&InCommand);
	}
	delete &InCommand;

	return Result;
}

ECommandResult::Type FLoreSourceControlProvider::IssueCommand(FLoreSourceControlCommand& InCommand)
{
	if (GThreadPool != nullptr)
	{
		CommandQueue.Add(&InCommand);
		GThreadPool->AddQueuedWork(&InCommand);
		return ECommandResult::Succeeded;
	}

	// No thread pool, so run inline and report back.
	InCommand.DoWork();
	const bool bStatesChanged = InCommand.Worker->UpdateStates();
	const ECommandResult::Type Result = InCommand.ReturnResults();
	if (bStatesChanged)
	{
		OnSourceControlStateChanged.Broadcast();
	}

	if (InCommand.bAutoDelete)
	{
		delete &InCommand;
	}

	return Result;
}

#undef LOCTEXT_NAMESPACE
