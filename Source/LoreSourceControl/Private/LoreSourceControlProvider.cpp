// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlProvider.h"
#include "LoreSourceControlModule.h"
#include "LoreSourceControlCommand.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlOperations.h"
#include "LoreSourceControlDownload.h"
#include "LoreSourceControlLog.h"
#include "LoreGuardedRewrite.h"
#include "LoreOperationMarker.h"
#include "LorePluginStore.h"
#include "LoreChangelistStore.h"
#include "LoreSourceControlChangelist.h"
#include "LoreSourceControlChangelistState.h"
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
		// A marker left over from a rewrite that took the editor down surfaces here on the next start.
		DetectInterruptedRewrite();
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
	// The changelist surface needs ISourceControlChangelist's 5.3 methods, so below 5.3 there is no changelist state to return.
#if LORE_UE5_3_OR_LATER
	if (!IsEnabled())
	{
		return ECommandResult::Failed;
	}

	// Membership reconciliation runs here on the game thread, where the state cache lives, and the reconciled records persist so a changelist-keyed submit expands to the same set the view shows. The store read is live, so a force update has nothing extra to refresh. IsModified covers exactly the changed working-copy states; an out-of-tree cache entry (engine content the editor queried) never joins a changelist.
	FString RootPrefix = FPaths::ConvertRelativePathToFull(PathToRepositoryRoot);
	FPaths::NormalizeDirectoryName(RootPrefix);
	RootPrefix += TEXT("/");
	TArray<FString> ChangedFiles;
	for (const auto& Pair : StateCache)
	{
		if (Pair.Key.StartsWith(RootPrefix) && Pair.Value->IsModified())
		{
			ChangedFiles.Add(LoreSourceControlUtils::RelativeFilename(Pair.Key, PathToRepositoryRoot));
		}
	}

	// The whole load-reconcile-save runs under the store lock, so a worker's pool-thread edit cannot land in between and be lost.
	TArray<FLoreChangelistRecord> Records;
	LoreChangelists::Mutate(PathToRepositoryRoot, [this, &ChangedFiles, &Records](TArray<FLoreChangelistRecord>& StoreRecords)
	{
		// A member whose live state is unknown (absent from the cache, or cached before any status query) counts as changed so Reconcile keeps it: the editor reads changelist state before the first status pass, and dropping on unknown would wipe every assignment at startup. Only a member known to be not changed drops out.
		for (const FLoreChangelistRecord& Record : StoreRecords)
		{
			for (const FString& RelativeFile : Record.Files)
			{
				const TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>* Found = StateCache.Find(FPaths::ConvertRelativePathToFull(PathToRepositoryRoot / RelativeFile));
				if (!Found || (*Found)->WorkingCopyState == ELoreWorkingCopyState::Unknown)
				{
					ChangedFiles.Add(RelativeFile);
				}
			}
		}

		// The cache carries no move source, so a move reconciles as its destination joining Default.
		LoreChangelists::Reconcile(StoreRecords, ChangedFiles, TMap<FString, FString>());
		Records = StoreRecords;
		return true;
	});

	for (const FSourceControlChangelistRef& InChangelist : InChangelists)
	{
		const FString Id = InChangelist->GetIdentifier();
		FLoreSourceControlChangelistStateRef State = MakeShared<FLoreSourceControlChangelistState, ESPMode::ThreadSafe>(MakeShared<FLoreSourceControlChangelist, ESPMode::ThreadSafe>(Id));
		// A stale identifier whose record is gone yields an empty state rather than failing the whole query.
		if (const FLoreChangelistRecord* Record = Records.FindByPredicate([&Id](const FLoreChangelistRecord& R) { return R.Id == Id; }))
		{
			State->Title = Record->Title;
			State->Description = Record->Description;
			if (Record->Updated != FDateTime::MinValue())
			{
				State->TimeStamp = Record->Updated;
			}
			for (const FString& RelativeFile : Record->Files)
			{
				State->Files.Add(GetStateInternal(FPaths::ConvertRelativePathToFull(PathToRepositoryRoot / RelativeFile)));
			}
		}
		OutState.Add(State);
	}
	return ECommandResult::Succeeded;
#else
	return ECommandResult::Failed;
#endif
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
#if LORE_UE5_3_OR_LATER
	if (InChangelist.IsValid())
	{
		Command->ChangelistId = InChangelist->GetIdentifier();
	}

	// The Changes view submits a changelist by identifier with no file list. Expand the membership here, at issue time on the game thread under the store lock, so the submitted set is exactly what the view rendered; the worker does not re-read the store on the pool thread, where a concurrent edit could change the set mid-flight.
	if (Command->Files.Num() == 0 && !Command->ChangelistId.IsEmpty() && (InOperation->GetName() == "CheckIn" || InOperation->GetName() == "CheckInOverLock"))
	{
		for (const FLoreChangelistRecord& Record : LoreChangelists::Load(PathToRepositoryRoot))
		{
			if (Record.Id == Command->ChangelistId)
			{
				for (const FString& RelativeFile : Record.Files)
				{
					Command->Files.Add(FPaths::ConvertRelativePathToFull(PathToRepositoryRoot / RelativeFile));
				}
				break;
			}
		}
	}
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
	// The changelist surface needs ISourceControlChangelist's 5.3 methods, so on 5.0 through 5.2 there are no changelists and the uncontrolled-changelist fallback stands.
#if LORE_UE5_3_OR_LATER
	TArray<FSourceControlChangelistRef> Result;
	for (const FLoreChangelistRecord& Record : LoreChangelists::Load(PathToRepositoryRoot))
	{
		Result.Add(MakeShared<FLoreSourceControlChangelist, ESPMode::ThreadSafe>(Record.Id));
	}
	return Result;
#else
	return TArray<FSourceControlChangelistRef>();
#endif
}
#endif

bool FLoreSourceControlProvider::UsesLocalReadOnlyState() const
{
	return FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();
}

bool FLoreSourceControlProvider::UsesChangelists() const
{
	// Real changelists need the 5.3 changelist interface; below that only the uncontrolled-changelist fallback stands.
#if LORE_UE5_3_OR_LATER
	return true;
#else
	return false;
#endif
}

#if LORE_UE5_2_OR_LATER
bool FLoreSourceControlProvider::UsesUncontrolledChangelists() const
{
	// Kept on alongside real changelists, matching the Perforce and Plastic providers.
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

#if SOURCE_CONTROL_WITH_SLATE
	// Raise the recovery notification from Tick, not from Init: Init can run during editor boot before the notification system is up. Raise it once per pending recovery.
	if (PendingRecovery.IsValid() && !bRecoveryNotificationShown)
	{
		bRecoveryNotificationShown = true;
		RaiseRecoveryNotification();
	}
#endif
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

bool FLoreSourceControlProvider::IsOperationRegistered(const FName& InOperationName) const
{
	return WorkersMap.Contains(InOperationName);
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

void FLoreSourceControlProvider::DetectInterruptedRewrite()
{
	check(IsInGameThread());
	if (PendingRecovery.IsValid() || PathToRepositoryRoot.IsEmpty())
	{
		return;
	}

	const FLoreOperationMarker Marker = LoreOperationMarker::Read(PathToRepositoryRoot);
	if (!Marker.IsValid())
	{
		return;
	}

	// A marker from a different working tree's instance belongs to that tree; leave it alone. Read() keys by this instance already, so a mismatch means the marker predates instance keying or came from a shared unkeyed file; only adopt it when it names this instance or carries none.
	const FString Instance = LorePluginStore::ReadInstanceId(PathToRepositoryRoot);
	if (!Marker.Instance.IsEmpty() && !Instance.IsEmpty() && Marker.Instance != Instance)
	{
		return;
	}

	PendingRecovery = Marker;
	bRecoveryNotificationShown = false;
}

bool FLoreSourceControlProvider::PerformRecovery()
{
	check(IsInGameThread());
	if (!PendingRecovery.IsValid())
	{
		return true;
	}

	const FLoreOperationMarker Marker = PendingRecovery;
	const ELoreBackendMode Mode = FLoreSourceControlModule::Get().GetSettings().GetBackendMode();
	const bool bUsingLocking = FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();

	// Clear the pending state first so the re-dispatched sync's own guard, which writes and clears a fresh marker, is not read back as a new interruption.
	PendingRecovery = FLoreOperationMarker();
	bRecoveryNotificationShown = false;

	// Reset the flagged-dirty delta files back to the anchor. A plain re-run does not repair a torn tree, and a dirty file inside a delta refuses the re-run, so the reset comes first.
	bool bAnyReset = false;
	TArray<FString> RepairErrors;
	const bool bRepaired = LoreGuardedRewrite::RepairTornFiles(Marker, Mode, PathToLoreBinary, PathToRepositoryRoot, Identity, bUsingLocking, bAnyReset, RepairErrors);
	if (!bRepaired)
	{
		LoreOperationMarker::Clear(PathToRepositoryRoot);
		return false;
	}

	// Re-dispatch the recorded operation through the normal worker path, whose guard runs again with a fresh marker and clears it on a verified success. When nothing was dirty the rewrite had completed, so there is nothing to re-run.
	//
	// Three-way discriminator, keyed by the marker fields, not the kind alone:
	//   - a non-empty target branch re-dispatches the branch switch to that branch;
	//   - a non-empty target revision with an empty target branch re-dispatches a move to that revision (the move worker is a later chunk, so absent it, log and leave the reset files for the manual path);
	//   - an empty target branch and an empty target revision re-dispatch FSync to the tip, the shipped sync case.
	// Keying on the fields keeps the sync arm safe: a shipped sync leaves both empty and must sync to the tip, while a move records a specific revision and must return to it, not carry the tree forward.
	bool bReDispatched = true;
	if (bAnyReset && !Marker.TargetBranch.IsEmpty())
	{
		TSharedRef<FLoreSwitchBranch, ESPMode::ThreadSafe> Switch = ISourceControlOperation::Create<FLoreSwitchBranch>();
		Switch->BranchName = Marker.TargetBranch;
#if LORE_UE5_0_OR_LATER
		bReDispatched = Execute(Switch, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Synchronous) == ECommandResult::Succeeded;
#else
		bReDispatched = Execute(Switch, TArray<FString>(), EConcurrency::Synchronous) == ECommandResult::Succeeded;
#endif
	}
	else if (bAnyReset && !Marker.TargetRevision.IsEmpty())
	{
		// A move to a specific revision. The move worker ships in a later chunk; until it registers, the reset has already returned the files to the anchor, so leave them for the user to move by hand rather than syncing the tree forward past the recorded revision.
		if (IsOperationRegistered("MoveToRevision"))
		{
			// The move re-dispatch is wired with the move worker; guarding on registration keeps this branch inert until then.
			UE_LOG(LogLoreSourceControl, Warning, TEXT("Interrupted move recovery: MoveToRevision registered but its re-dispatch is not wired yet; the reset restored the files, move to %s by hand."), *Marker.TargetRevision);
		}
		else
		{
			UE_LOG(LogLoreSourceControl, Warning, TEXT("Interrupted move recovery: the files were reset to the anchor. Move to revision %s by hand; the guarded move ships in a later 0.4/0.5 chunk."), *Marker.TargetRevision);
		}
	}
	else if (bAnyReset && Marker.Kind == ELoreOperationKind::RevisionSync)
	{
		TArray<FString> Files;
		for (const FString& Relative : Marker.ChangedFiles)
		{
			Files.Add(FPaths::ConvertRelativePathToFull(PathToRepositoryRoot / Relative));
		}
#if LORE_UE5_0_OR_LATER
		bReDispatched = Execute(ISourceControlOperation::Create<FSync>(), FSourceControlChangelistPtr(), Files, EConcurrency::Synchronous) == ECommandResult::Succeeded;
#else
		bReDispatched = Execute(ISourceControlOperation::Create<FSync>(), Files, EConcurrency::Synchronous) == ECommandResult::Succeeded;
#endif
	}

	// The re-dispatched operation clears its own marker on success. Clear here too, so a nothing-to-do recovery or a re-dispatch that never ran leaves no marker behind.
	LoreOperationMarker::Clear(PathToRepositoryRoot);
	return bReDispatched;
}

void FLoreSourceControlProvider::DismissRecovery()
{
	check(IsInGameThread());
	PendingRecovery = FLoreOperationMarker();
	bRecoveryNotificationShown = false;
	LoreOperationMarker::Clear(PathToRepositoryRoot);
}

#if SOURCE_CONTROL_WITH_SLATE
void FLoreSourceControlProvider::RaiseRecoveryNotification()
{
	// The operation names itself in the message, the target and file count give the user what they need to judge whether the interrupted rewrite carried merged work they would lose to a reset.
	const FString OperationName = PendingRecovery.Kind == ELoreOperationKind::BranchSwitch ? TEXT("branch switch")
		: PendingRecovery.Kind == ELoreOperationKind::ReturnToLatest ? TEXT("return to latest")
		: TEXT("sync");
	const FText Message = FText::Format(
		LOCTEXT("RecoveryOffer", "A Lore {0} was interrupted, leaving {1} {1}|plural(one=file,other=files) half-updated. Recover returns them to the pre-{0} revision first, discarding any local edit the interrupted operation merged in."),
		FText::FromString(OperationName), FText::AsNumber(PendingRecovery.ChangedFiles.Num()));

	FNotificationInfo Info(Message);
	Info.bFireAndForget = false;
	Info.FadeOutDuration = 0.5f;
	Info.ExpireDuration = 0.0f;

	// The button lambdas close the persistent notification, so they need its item; a shared holder captures it after AddNotification returns.
	TSharedRef<TSharedPtr<SNotificationItem>> ItemHolder = MakeShared<TSharedPtr<SNotificationItem>>();

	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("RecoverButton", "Recover"),
		LOCTEXT("RecoverButtonTip", "Reset the half-updated files to the pre-operation revision, then re-run the operation."),
		FSimpleDelegate::CreateLambda([this, ItemHolder]()
		{
			if (ItemHolder->IsValid()) { (*ItemHolder)->SetFadeOutDuration(0.5f); (*ItemHolder)->ExpireAndFadeout(); }
			PerformRecovery();
		})));

	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("DismissButton", "Dismiss"),
		LOCTEXT("DismissButtonTip", "Leave the half-updated files as ordinary local modifications to inspect by hand."),
		FSimpleDelegate::CreateLambda([this, ItemHolder]()
		{
			if (ItemHolder->IsValid()) { (*ItemHolder)->ExpireAndFadeout(); }
			DismissRecovery();
		})));

	*ItemHolder = FSlateNotificationManager::Get().AddNotification(Info);
}
#endif

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
