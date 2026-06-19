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

	bLoreAvailable = LoreSourceControlUtils::CheckLoreAvailability(PathToLoreBinary, LoreVersion);

	bLoreRepositoryFound = LoreSourceControlUtils::GetRepositoryRoot(FPaths::ProjectDir(), PathToRepositoryRoot);
	if (bLoreRepositoryFound)
	{
		// Read identity and remote before any status query, so our own locks attribute right.
		LoreSourceControlUtils::GetRepositoryConfig(PathToRepositoryRoot, RemoteUrl, Identity);
		LoreSourceControlUtils::GetBranchName(PathToLoreBinary, PathToRepositoryRoot, BranchName);
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
	for (int32 Index = 0; Index < CommandQueue.Num(); /* in-loop */)
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
	// Advanced: the client path and the downloader
	+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 8.0f, 2.0f, 2.0f)
	[
		SNew(SExpandableArea)
		.InitiallyCollapsed(true)
		.AreaTitle(LOCTEXT("Advanced", "Advanced"))
		.BodyContent()
		[
			SNew(SHorizontalBox)
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
	FScopedSourceControlProgress Progress(Task);
	InCommand.DoWork();
	const bool bStatesChanged = InCommand.Worker->UpdateStates();
	const ECommandResult::Type Result = InCommand.ReturnResults();
	if (bStatesChanged)
	{
		OnSourceControlStateChanged.Broadcast();
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
