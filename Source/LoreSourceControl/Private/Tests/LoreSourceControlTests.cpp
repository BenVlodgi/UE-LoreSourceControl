// Copyright 2026 Dream Seed LLC. MIT License.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "SourceControlOperations.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlModule.h"
#include "LoreSourceControlProvider.h"

#if WITH_DEV_AUTOMATION_TESTS

// Unit tests: state logic and parsers, no client or server.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSourceControlStateTest, "LoreSourceControl.State", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSourceControlStateTest::RunTest(const FString& Parameters)
{
	{
		FLoreSourceControlState State(TEXT("C:/Project/A.uasset"));
		State.bUsingLocking = true;
		State.WorkingCopyState = ELoreWorkingCopyState::Added;
		State.LockState = ELoreLockState::NotLocked;
		TestTrue(TEXT("Added is added"), State.IsAdded());
		TestTrue(TEXT("Added can check in"), State.CanCheckIn());
		TestFalse(TEXT("Added is not checked out"), State.IsCheckedOut());
		TestTrue(TEXT("Added is modified"), State.IsModified());
	}
	{
		FLoreSourceControlState State(TEXT("C:/Project/B.uasset"));
		State.WorkingCopyState = ELoreWorkingCopyState::Deleted;
		TestTrue(TEXT("Deleted is deleted"), State.IsDeleted());
	}
	{
		FLoreSourceControlState State(TEXT("C:/Project/C.uasset"));
		State.bUsingLocking = true;
		State.WorkingCopyState = ELoreWorkingCopyState::Unchanged;
		State.LockState = ELoreLockState::Locked;
		TestTrue(TEXT("Locked is checked out"), State.IsCheckedOut());
		TestFalse(TEXT("Locked cannot check out again"), State.CanCheckout());
	}
	{
		FLoreSourceControlState State(TEXT("C:/Project/D.uasset"));
		State.bUsingLocking = true;
		State.WorkingCopyState = ELoreWorkingCopyState::Unchanged;
		State.LockState = ELoreLockState::LockedOther;
		State.LockUser = TEXT("teammate@example.com");
		FString Who;
		TestTrue(TEXT("Other is checked out by other"), State.IsCheckedOutOther(&Who));
		TestEqual(TEXT("Other owner reported"), Who, FString(TEXT("teammate@example.com")));
		TestFalse(TEXT("Other is not mine"), State.IsCheckedOut());
	}
	{
		FLoreSourceControlState State(TEXT("C:/Project/E.uasset"));
		State.bUsingLocking = true;
		State.WorkingCopyState = ELoreWorkingCopyState::Unchanged;
		State.LockState = ELoreLockState::NotLocked;
		TestTrue(TEXT("Unchanged is source controlled"), State.IsSourceControlled());
		TestTrue(TEXT("Unchanged can check out"), State.CanCheckout());
		TestFalse(TEXT("Unchanged is not modified"), State.IsModified());
	}
	{
		FLoreSourceControlState State(TEXT("C:/Project/F.txt"));
		State.WorkingCopyState = ELoreWorkingCopyState::NotControlled;
		TestTrue(TEXT("NotControlled can add"), State.CanAdd());
		TestFalse(TEXT("NotControlled is not source controlled"), State.IsSourceControlled());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSourceControlStatusParseTest, "LoreSourceControl.StatusParse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSourceControlStatusParseTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Lines = {
		TEXT("Repository 019ed92f26b87992ad207981d8273aaa"),
		TEXT("On branch main revision 1 -> abc"),
		TEXT("Remote revision 1 -> abc"),
		TEXT("Local branch in sync with remote"),
		TEXT("Changes staged for commit:"),
		TEXT("A asset.bin"),
		TEXT("A hello.txt"),
		TEXT("V old.uasset -> new.uasset"),
		TEXT("Changes not staged for commit:"),
		TEXT("M edited.txt"),
		TEXT("D gone.bin"),
		TEXT("Tracked changes: 2 added, 1 moved, 1 modified, 1 deleted"),
	};
	TMap<FString, ELoreWorkingCopyState::Type> Parsed;
	LoreSourceControlUtils::ParseStatusForTest(Lines, Parsed);

	TestEqual(TEXT("Five files parsed"), Parsed.Num(), 5);
	TestTrue(TEXT("asset.bin added"), Parsed.Contains(TEXT("asset.bin")) && Parsed[TEXT("asset.bin")] == ELoreWorkingCopyState::Added);
	TestTrue(TEXT("hello.txt added"), Parsed.Contains(TEXT("hello.txt")) && Parsed[TEXT("hello.txt")] == ELoreWorkingCopyState::Added);
	TestTrue(TEXT("edited.txt modified"), Parsed.Contains(TEXT("edited.txt")) && Parsed[TEXT("edited.txt")] == ELoreWorkingCopyState::Modified);
	TestTrue(TEXT("gone.bin deleted"), Parsed.Contains(TEXT("gone.bin")) && Parsed[TEXT("gone.bin")] == ELoreWorkingCopyState::Deleted);
	// A move reads "V <from> -> <to>", so key it on the destination
	TestTrue(TEXT("new.uasset moved"), Parsed.Contains(TEXT("new.uasset")) && Parsed[TEXT("new.uasset")] == ELoreWorkingCopyState::Moved);

	const TArray<FString> Untracked = {
		TEXT("Untracked files:"),
		TEXT("A new.txt"),
		TEXT("Tracked changes: 1 added"),
	};
	TMap<FString, ELoreWorkingCopyState::Type> ParsedUntracked;
	LoreSourceControlUtils::ParseStatusForTest(Untracked, ParsedUntracked);
	TestTrue(TEXT("new.txt is not controlled"), ParsedUntracked.Contains(TEXT("new.txt")) && ParsedUntracked[TEXT("new.txt")] == ELoreWorkingCopyState::NotControlled);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSourceControlLockParseTest, "LoreSourceControl.LockParse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSourceControlLockParseTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Lines = {
		TEXT("Files locked for edit:"),
		TEXT("asset.bin by alice@example.com on Thu, 18 Jun 2026 07:32:07 +0000"),
		TEXT("Content/Stand by Me.uasset by bob@example.com on Thu, 18 Jun 2026 07:32:07 +0000"),
	};
	TMap<FString, FString> Locks;
	LoreSourceControlUtils::ParseLocksForTest(Lines, Locks);

	TestEqual(TEXT("Two locks parsed"), Locks.Num(), 2);
	TestTrue(TEXT("asset.bin owned by alice"), Locks.Contains(TEXT("asset.bin")) && Locks[TEXT("asset.bin")] == TEXT("alice@example.com"));
	// A path with " by " in it must key on the whole path, not a truncation
	TestTrue(TEXT("path containing ' by ' parsed whole"), Locks.Contains(TEXT("Content/Stand by Me.uasset")) && Locks[TEXT("Content/Stand by Me.uasset")] == TEXT("bob@example.com"));

	// Lines outside the locked-files section are never records
	const TArray<FString> NoHeader = { TEXT("some banner by the way on startup") };
	TMap<FString, FString> NoLocks;
	LoreSourceControlUtils::ParseLocksForTest(NoHeader, NoLocks);
	TestEqual(TEXT("No header means no locks"), NoLocks.Num(), 0);

	return true;
}

namespace
{
	FString LoreServerExtension()
	{
#if PLATFORM_WINDOWS
		return TEXT(".exe");
#else
		return FString();
#endif
	}

	// Find loreserver: override, next to the client, or on PATH.
	FString FindLoreServerBinary(const FString& LoreClient)
	{
		TArray<FString> Candidates;
		const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("LORESERVER"));
		if (!Override.IsEmpty())
		{
			Candidates.Add(Override);
		}
		const FString Dir = FPaths::GetPath(LoreClient);
		if (!Dir.IsEmpty())
		{
			Candidates.Add(Dir / (TEXT("loreserver") + LoreServerExtension()));
		}
		Candidates.Add(TEXT("loreserver"));

		for (const FString& Candidate : Candidates)
		{
			int32 ReturnCode = -1;
			FString StdOut, StdErr;
			if (FPlatformProcess::ExecProcess(*Candidate, TEXT("--version"), &ReturnCode, &StdOut, &StdErr) && ReturnCode == 0)
			{
				return Candidate;
			}
		}
		return FString();
	}

	// Temporary loreserver for integration tests on alternative port.
	struct FLoreManagedServer
	{
		FProcHandle Proc;
		FString DataDir;
		FString ServerBase;

		~FLoreManagedServer()
		{
			Stop();
		}

		bool Start(const FString& LoreClient, FAutomationTestBase& Test)
		{
			const FString ServerBinary = FindLoreServerBinary(LoreClient);
			if (ServerBinary.IsEmpty())
			{
				Test.AddError(TEXT("loreserver not found; an integration test needs it next to the Lore client, on PATH, or in the LORESERVER variable."));
				return false;
			}

			const int32 QuicPort = 42337;
			const int32 HttpPort = 42339;
			ServerBase = FString::Printf(TEXT("lore://127.0.0.1:%d"), QuicPort);

			DataDir = FPaths::Combine(FString(FPlatformProcess::UserTempDir()), TEXT("LoreTestServer"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
			const FString ConfigDir = DataDir / TEXT("config");
			const FString StoreDir = DataDir / TEXT("store");
			IFileManager::Get().MakeDirectory(*ConfigDir, true);
			IFileManager::Get().MakeDirectory(*StoreDir, true);

			// Pin our own store path so it does not clash with a dev server.
			const FString Toml = FString::Printf(
				TEXT("[server.quic]\nport = %d\n\n[server.grpc]\nport = %d\n\n[server.http]\nport = %d\n\n[immutable_store.local]\npath = \"%s\"\n\n[mutable_store.local]\npath = \"%s\"\n"),
				QuicPort, QuicPort, HttpPort, *StoreDir, *StoreDir);
			if (!FFileHelper::SaveStringToFile(Toml, *(ConfigDir / TEXT("local.toml"))))
			{
				Test.AddError(TEXT("Could not write the test server configuration."));
				return false;
			}

			const FString Params = FString::Printf(TEXT("--config \"%s\""), *ConfigDir);
			Proc = FPlatformProcess::CreateProc(*ServerBinary, *Params, false, true, true, nullptr, 0, *DataDir, nullptr);
			if (!Proc.IsValid())
			{
				Test.AddError(FString::Printf(TEXT("Could not launch loreserver at %s"), *ServerBinary));
				return false;
			}

			// Ready once it accepts a repository create.
			const double Deadline = FPlatformTime::Seconds() + 30.0;
			while (FPlatformTime::Seconds() < Deadline)
			{
				if (!FPlatformProcess::IsProcRunning(Proc))
				{
					Test.AddError(TEXT("loreserver exited during startup."));
					return false;
				}
				if (Probe(LoreClient))
				{
					return true;
				}
				FPlatformProcess::Sleep(0.5f);
			}
			Test.AddError(TEXT("loreserver did not become ready within thirty seconds."));
			return false;
		}

		void Stop()
		{
			if (Proc.IsValid())
			{
				FPlatformProcess::TerminateProc(Proc, true);
				FPlatformProcess::CloseProc(Proc);
				Proc.Reset();
			}
			if (!DataDir.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*DataDir, false, true);
				DataDir.Empty();
			}
		}

	private:
		bool Probe(const FString& LoreClient)
		{
			const FString ProbeDir = DataDir / (TEXT("probe-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
			IFileManager::Get().MakeDirectory(*ProbeDir, true);
			ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ProbeDir, false, true); };
			const FString Url = FString::Printf(TEXT("%s/probe-%s"), *ServerBase, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
			TArray<FString> Results, Errors;
			return LoreSourceControlUtils::RunCommand(FString::Printf(TEXT("--identity test@example.com repository create %s"), *Url), LoreClient, ProbeDir, TArray<FString>(), TArray<FString>(), Results, Errors);
		}
	};
}

// Test all utilities against a loreserver.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSourceControlIntegrationTest, "LoreSourceControl.Integration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSourceControlIntegrationTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	const FString Binary = FindLoreBinaryPath();
	FString Version;
	if (!CheckLoreAvailability(Binary, Version))
	{
		AddError(TEXT("Lore client not found; the integration test requires it."));
		return false;
	}

	FLoreManagedServer Server;
	if (!Server.Start(Binary, *this))
	{
		return false;
	}

	// Working tree outside the project, so the project's own tree cannot shadow it.
	const FString Root = FPaths::Combine(FString(FPlatformProcess::UserTempDir()), TEXT("LoreIntegrationTest"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Root, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };

	const FString RepoUrl = FString::Printf(TEXT("%s/itest-%s"), *Server.ServerBase, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	TArray<FString> Results, Errors;
	const bool bCreated = RunCommand(FString::Printf(TEXT("--identity test@example.com repository create %s"), *RepoUrl), Binary, Root, TArray<FString>(), TArray<FString>(), Results, Errors);
	TestTrue(TEXT("Repository created on the test server"), bCreated);
	if (!bCreated)
	{
		return false;
	}

	const FString File = Root / TEXT("hello.txt");
	FFileHelper::SaveStringToFile(TEXT("Hello Lore"), *File);
	const TArray<FString> Files = { File };

	auto StateOf = [&](bool bLocking) -> FLoreSourceControlState
	{
		TArray<FLoreSourceControlState> States;
		TArray<FString> StatusErrors;
		RunUpdateStatus(Binary, Root, TEXT("test@example.com"), bLocking, Files, StatusErrors, States);
		return States.Num() == 1 ? States[0] : FLoreSourceControlState(File);
	};

	TestEqual(TEXT("Untracked file reads as not controlled"), (int32)StateOf(false).WorkingCopyState, (int32)ELoreWorkingCopyState::NotControlled);

	// Out-of-tree paths (engine content) are skipped, not failed, and read as not controlled.
	{
		const FString OutsidePath = FPaths::Combine(FString(FPlatformProcess::UserTempDir()), TEXT("LoreOutsideProbe"), TEXT("outside.txt"));
		TArray<FLoreSourceControlState> MixedStates;
		TArray<FString> MixedErrors;
		const bool bMixedOk = RunUpdateStatus(Binary, Root, TEXT("test@example.com"), false, { File, OutsidePath }, MixedErrors, MixedStates);
		TestTrue(TEXT("Status with an out-of-tree path still succeeds"), bMixedOk);
		TestEqual(TEXT("Both paths return a state"), MixedStates.Num(), 2);
		if (MixedStates.Num() == 2)
		{
			TestEqual(TEXT("Out-of-tree path reads as not controlled"), (int32)MixedStates[1].WorkingCopyState, (int32)ELoreWorkingCopyState::NotControlled);
		}
	}

	RunCommand(TEXT("stage"), Binary, Root, TArray<FString>(), { TEXT("hello.txt") }, Results, Errors);
	TestEqual(TEXT("Staged file reads as added"), (int32)StateOf(false).WorkingCopyState, (int32)ELoreWorkingCopyState::Added);

	const bool bCommitted = RunCommand(TEXT("commit"), Binary, Root, { TEXT("\"integration commit\"") }, TArray<FString>(), Results, Errors);
	TestTrue(TEXT("Commit succeeds"), bCommitted);
	TestEqual(TEXT("Committed file reads as unchanged"), (int32)StateOf(false).WorkingCopyState, (int32)ELoreWorkingCopyState::Unchanged);

	// An edited committed asset reads back as modified.
	FFileHelper::SaveStringToFile(TEXT("Hello Lore, edited"), *File);
	TestEqual(TEXT("Edited committed file reads as modified"), (int32)StateOf(false).WorkingCopyState, (int32)ELoreWorkingCopyState::Modified);
	FFileHelper::SaveStringToFile(TEXT("Hello Lore"), *File);

	// Commit is local, so push then sync exercise the server.
	const bool bPushed = RunCommand(TEXT("push"), Binary, Root, TArray<FString>(), TArray<FString>(), Results, Errors);
	TestTrue(TEXT("Push succeeds"), bPushed);
	const bool bSynced = RunCommand(TEXT("sync"), Binary, Root, TArray<FString>(), TArray<FString>(), Results, Errors);
	TestTrue(TEXT("Sync succeeds"), bSynced);
	TestTrue(TEXT("Synced file is current"), StateOf(false).IsCurrent());

	TLoreSourceControlHistory History;
	RunGetHistory(Binary, Root, File, Errors, History);
	TestTrue(TEXT("History has at least one revision"), History.Num() >= 1);
	if (History.Num() >= 1)
	{
		TestFalse(TEXT("Revision carries a commit id"), History[0]->GetRevision().IsEmpty());
		TestEqual(TEXT("Revision description is the commit message"), History[0]->GetDescription(), FString(TEXT("integration commit")));
		TestTrue(TEXT("Revision date parsed (year is current era)"), History[0]->GetDate().GetYear() > 2000);

		// Diff extracts a revision through this path; confirm it writes content.
		FString DiffTemp;
		const bool bExtracted = History[0]->Get(DiffTemp);
		TestTrue(TEXT("Revision content extracts to a file"), bExtracted);
		TestTrue(TEXT("Extracted revision is non-empty"), bExtracted && IFileManager::Get().FileSize(*DiffTemp) > 0);

		// The client cannot overwrite, so a repeat extraction reuses the existing file.
		FString DiffTempRepeat;
		TestTrue(TEXT("Repeat extraction reuses the existing temp file"), History[0]->Get(DiffTempRepeat));
		TestEqual(TEXT("Repeat extraction yields the same path"), DiffTempRepeat, DiffTemp);
	}

	RunCommand(TEXT("lock acquire"), Binary, Root, TArray<FString>(), { TEXT("hello.txt") }, Results, Errors);
	// With authentication off the lock owner is unattributable and reads as this user's own.
	const ELoreLockState::Type LockState = StateOf(true).LockState;
	TestEqual(TEXT("Locked file reads as checked out by this user"), (int32)LockState, (int32)ELoreLockState::Locked);

	RunCommand(TEXT("lock release"), Binary, Root, TArray<FString>(), { TEXT("hello.txt") }, Results, Errors);

	// Delete: remove on disk, stage, confirm deleted.
	IFileManager::Get().Delete(*File, false, true);
	RunCommand(TEXT("stage"), Binary, Root, TArray<FString>(), { TEXT("hello.txt") }, Results, Errors);
	TestEqual(TEXT("Deleted file reads as deleted"), (int32)StateOf(false).WorkingCopyState, (int32)ELoreWorkingCopyState::Deleted);

	// Partial submit: unstage all, stage only the subset, commit; the other file stays out.
	const FString FileA = Root / TEXT("alpha.txt");
	const FString FileB = Root / TEXT("beta.txt");
	FFileHelper::SaveStringToFile(TEXT("alpha"), *FileA);
	FFileHelper::SaveStringToFile(TEXT("beta"), *FileB);
	RunCommand(TEXT("stage"), Binary, Root, TArray<FString>(), { TEXT("alpha.txt"), TEXT("beta.txt") }, Results, Errors);
	RunCommand(TEXT("unstage"), Binary, Root, { TEXT(".") }, TArray<FString>(), Results, Errors);
	RunCommand(TEXT("stage"), Binary, Root, TArray<FString>(), { TEXT("alpha.txt") }, Results, Errors);
	RunCommand(TEXT("commit"), Binary, Root, { TEXT("\"partial submit\"") }, TArray<FString>(), Results, Errors);

	auto WorkingStateOf = [&](const FString& Absolute) -> ELoreWorkingCopyState::Type
	{
		TArray<FLoreSourceControlState> PartialStates;
		TArray<FString> PartialErrors;
		RunUpdateStatus(Binary, Root, TEXT("test@example.com"), false, { Absolute }, PartialErrors, PartialStates);
		return PartialStates.Num() == 1 ? PartialStates[0].WorkingCopyState : ELoreWorkingCopyState::Unknown;
	};
	TestEqual(TEXT("Submitted file is committed"), (int32)WorkingStateOf(FileA), (int32)ELoreWorkingCopyState::Unchanged);
	TestEqual(TEXT("Unselected file is not committed"), (int32)WorkingStateOf(FileB), (int32)ELoreWorkingCopyState::NotControlled);

	return true;
}

// Test operations through the registered provider, the path the engine tests use.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSourceControlProviderTest, "LoreSourceControl.Provider", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSourceControlProviderTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	const FString Binary = FindLoreBinaryPath();
	FString Version;
	if (!CheckLoreAvailability(Binary, Version))
	{
		AddError(TEXT("Lore client not found; the provider test requires it."));
		return false;
	}

	FLoreManagedServer Server;
	if (!Server.Start(Binary, *this))
	{
		return false;
	}

	const FString Root = FPaths::Combine(FString(FPlatformProcess::UserTempDir()), TEXT("LoreProviderTest"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Root, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };

	const FString RepoUrl = FString::Printf(TEXT("%s/ptest-%s"), *Server.ServerBase, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	TArray<FString> Results, Errors;
	const bool bCreated = RunCommand(FString::Printf(TEXT("--identity test@example.com repository create %s"), *RepoUrl), Binary, Root, TArray<FString>(), TArray<FString>(), Results, Errors);
	TestTrue(TEXT("Provider test repository created"), bCreated);
	if (!bCreated)
	{
		return false;
	}

	ISourceControlModule& SourceControl = ISourceControlModule::Get();
	const FName Previous = SourceControl.GetProvider().GetName();
	SourceControl.SetProvider(FName("Lore"));
	ON_SCOPE_EXIT { SourceControl.SetProvider(Previous); };

	// Point the provider at the test tree, not the active project.
	FLoreSourceControlProvider& LoreProvider = FLoreSourceControlModule::Get().GetProvider();
	LoreProvider.Init();
	LoreProvider.SetRepositoryInfo(true, true, Root, RepoUrl, TEXT("test@example.com"), TEXT("main"), FString(), Version);
	TestTrue(TEXT("Provider reports available"), LoreProvider.IsAvailable());

	// Use the base interface, whose convenience overloads the derived type hides.
	ISourceControlProvider& Provider = SourceControl.GetProvider();

	const FString TempFile = FPaths::ConvertRelativePathToFull(Root / TEXT("provider_roundtrip.txt"));
	FFileHelper::SaveStringToFile(TEXT("provider round-trip"), *TempFile);
	const TArray<FString> Targets = { TempFile };
	ON_SCOPE_EXIT
	{
		Provider.Execute(ISourceControlOperation::Create<FRevert>(), Targets, EConcurrency::Synchronous);
	};

	const ECommandResult::Type AddResult = Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("MarkForAdd through the provider succeeds"), (int32)AddResult, (int32)ECommandResult::Succeeded);

	const FSourceControlStatePtr AddedState = Provider.GetState(TempFile, EStateCacheUsage::Use);
	TestTrue(TEXT("Added file has a valid state"), AddedState.IsValid());
	if (AddedState.IsValid())
	{
		TestTrue(TEXT("Added file reports IsAdded through the provider"), AddedState->IsAdded());
	}

	// Submit sets a success message, so the editor notification is not blank.
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckInOp = ISourceControlOperation::Create<FCheckIn>();
	CheckInOp->SetDescription(FText::FromString(TEXT("provider submit")));
	const ECommandResult::Type CheckInResult = Provider.Execute(CheckInOp, Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("CheckIn through the provider succeeds"), (int32)CheckInResult, (int32)ECommandResult::Succeeded);
	TestFalse(TEXT("CheckIn sets a non-empty success message"), CheckInOp->GetSuccessMessage().IsEmpty());

	// Check out: an auth-off lock reads as this user's own.
	const bool bWasLocking = FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();
	FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(true);
	ON_SCOPE_EXIT { FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(bWasLocking); };

	const ECommandResult::Type CheckOutResult = Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("CheckOut through the provider succeeds"), (int32)CheckOutResult, (int32)ECommandResult::Succeeded);
	const FSourceControlStatePtr CheckedOutState = Provider.GetState(TempFile, EStateCacheUsage::Use);
	TestTrue(TEXT("Checked-out file reports IsCheckedOut"), CheckedOutState.IsValid() && CheckedOutState->IsCheckedOut());

	// Submit with no change succeeds gracefully.
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> EmptyCheckIn = ISourceControlOperation::Create<FCheckIn>();
	EmptyCheckIn->SetDescription(FText::FromString(TEXT("no change submit")));
	const ECommandResult::Type EmptyResult = Provider.Execute(EmptyCheckIn, Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("Submit with no changes is a graceful no-op"), (int32)EmptyResult, (int32)ECommandResult::Succeeded);

	const ECommandResult::Type RevertResult = Provider.Execute(ISourceControlOperation::Create<FRevert>(), Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("Revert through the provider succeeds"), (int32)RevertResult, (int32)ECommandResult::Succeeded);
	const FSourceControlStatePtr RevertedState = Provider.GetState(TempFile, EStateCacheUsage::Use);
	TestTrue(TEXT("Reverted file can be checked out again"), RevertedState.IsValid() && RevertedState->CanCheckout());

	// Editor diff path: status with history, then extract twice (the second hits the reuse path).
	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> HistoryOp = ISourceControlOperation::Create<FUpdateStatus>();
	HistoryOp->SetUpdateHistory(true);
	const ECommandResult::Type HistoryResult = Provider.Execute(HistoryOp, Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("UpdateStatus with history succeeds"), (int32)HistoryResult, (int32)ECommandResult::Succeeded);

	const FSourceControlStatePtr HistoryState = Provider.GetState(TempFile, EStateCacheUsage::Use);
	TestTrue(TEXT("State carries history after the refresh"), HistoryState.IsValid() && HistoryState->GetHistorySize() >= 1);
	if (HistoryState.IsValid() && HistoryState->GetHistorySize() >= 1)
	{
		TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Revision = HistoryState->GetHistoryItem(0);
		TestTrue(TEXT("History item is valid"), Revision.IsValid());
		if (Revision.IsValid())
		{
			FString DiffTemp;
			TestTrue(TEXT("Revision extracts for diff"), Revision->Get(DiffTemp));
			TestTrue(TEXT("Extracted revision is non-empty"), IFileManager::Get().FileSize(*DiffTemp) > 0);
			FString DiffTempRepeat;
			TestTrue(TEXT("Repeat diff reuses the extracted file"), Revision->Get(DiffTempRepeat));
		}
	}

	// After a push the tree is level with the server, so a sync resolves and the file reads as current.
	const ECommandResult::Type SyncResult = Provider.Execute(ISourceControlOperation::Create<FSync>(), Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("Sync through the provider succeeds"), (int32)SyncResult, (int32)ECommandResult::Succeeded);
	const FSourceControlStatePtr SyncedState = Provider.GetState(TempFile, EStateCacheUsage::Use);
	TestTrue(TEXT("Synced file reads as current"), SyncedState.IsValid() && SyncedState->IsCurrent());

	// Keep-checked-out: the lock stays, so the file still reads as checked out. FCheckIn gained the flag in 5.1.
#if LORE_UE5_1_OR_LATER
	const ECommandResult::Type ReCheckOut = Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("Re-checkout before the keep-checked-out submit succeeds"), (int32)ReCheckOut, (int32)ECommandResult::Succeeded);
	FFileHelper::SaveStringToFile(TEXT("provider round-trip, kept checked out"), *TempFile);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> KeepCheckIn = ISourceControlOperation::Create<FCheckIn>();
	KeepCheckIn->SetDescription(FText::FromString(TEXT("keep checked out submit")));
	KeepCheckIn->SetKeepCheckedOut(true);
	const ECommandResult::Type KeepResult = Provider.Execute(KeepCheckIn, Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("Keep-checked-out submit succeeds"), (int32)KeepResult, (int32)ECommandResult::Succeeded);
	const FSourceControlStatePtr KeptState = Provider.GetState(TempFile, EStateCacheUsage::Use);
	TestTrue(TEXT("File stays checked out after a keep-checked-out submit"), KeptState.IsValid() && KeptState->IsCheckedOut());
#endif

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
