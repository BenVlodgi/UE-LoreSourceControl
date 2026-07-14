// Copyright 2026 Dream Seed LLC. MIT License.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/Base64.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Async/Async.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "SourceControlOperations.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlModule.h"
#include "LoreSourceControlProvider.h"
#include "LorePluginStore.h"
#include "LoreChangelistStore.h"
#include "LoreOperationMarker.h"
#include "LoreGuardedRewrite.h"
#include "LoreSourceControlCommand.h"
#include "LoreSourceControlOperations.h"
#include "ILoreSourceControlWorker.h"
#include "LoreSourceControlChangelist.h"
#include "LoreSourceControlChangelistState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if LORE_WITH_LIBLORE
#include "LoreCApi.h"
#include "LoreClient.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS

// Unit tests: state logic and parsers, no client or server.

#if LORE_WITH_LIBLORE
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreLibVersionTest, "LoreSourceControl.LibLoreVersion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreLibVersionTest::RunTest(const FString& Parameters)
{
	// Prove the native liblore C API links and runs in-process, and that the FLoreClient bridge returns the same version through its own translation unit.
	const char* RawVersion = lore_version();
	const FString Version = RawVersion ? FString(UTF8_TO_TCHAR(RawVersion)) : FString();
	AddInfo(FString::Printf(TEXT("liblore version: %s"), *Version));
	TestFalse(TEXT("liblore version is non-empty"), Version.IsEmpty());
	TestTrue(TEXT("liblore version is 0.8.x"), Version.Contains(TEXT("0.8")));
	TestEqual(TEXT("bridge version matches the raw client version"), LoreClient::GetVersion(), Version);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSourceControlAuthTest, "LoreSourceControl.Auth", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSourceControlAuthTest::RunTest(const FString& Parameters)
{
	// Exercise the native auth bridge without an identity provider: a token login stores a bearer client-side, a local query reads the identity back, and logout clears it. A throwaway auth URL keeps this out of any real credential, and a JWT carries the identity in its payload so the library can decode it offline.
	auto Base64Url = [](const FString& Json) -> FString
	{
		FTCHARToUTF8 Utf8(*Json);
		FString Encoded = FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		Encoded.ReplaceInline(TEXT("+"), TEXT("-"));
		Encoded.ReplaceInline(TEXT("/"), TEXT("_"));
		Encoded.ReplaceInline(TEXT("="), TEXT(""));
		return Encoded;
	};

	const FString AuthUrl = TEXT("ucs-auth://lore-plugin-test.invalid");
	const FString Header = TEXT("{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"lore-plugin-test\"}");
	// JWTUserInfo (lore-credential) requires iss, sub, name, exp, and aud; the library decodes the payload offline (no signature check) and verifies aud covers the remote domain, so aud lists the auth URL's host.
	const FString Payload = TEXT("{\"iss\":\"ucs-auth://lore-plugin-test.invalid\",\"sub\":\"lore-plugin-test-user\",\"name\":\"Lore Plugin Test\",\"exp\":4102444800,\"aud\":[\"lore-plugin-test.invalid\"]}");
	const FString Token = Base64Url(Header) + TEXT(".") + Base64Url(Payload) + TEXT(".") + Base64Url(TEXT("not-a-real-signature"));

	// Remove any leftover token for this throwaway URL first; a local operation that is a no-op when none is stored.
	FString CleanupError;
	LoreClient::Logout(FString(), AuthUrl, FString(), CleanupError);

	LoreClient::FAuthUserInfo User;
	FString LoginError;
	const bool bLoggedIn = LoreClient::LoginWithToken(FString(), FString(), AuthUrl, Token, TEXT("lore"), User, LoginError);
	AddInfo(FString::Printf(TEXT("login_with_token offline: %s; id='%s' name='%s'"), bLoggedIn ? TEXT("OK") : TEXT("FAILED"), *User.Id, *User.Name));
	TestTrue(FString::Printf(TEXT("token login succeeds offline (err: %s)"), *LoginError), bLoggedIn);
	TestEqual(TEXT("login resolves the token subject as the identity"), User.Id, FString(TEXT("lore-plugin-test-user")));

	// Read the identity back from the local credential store, no network.
	LoreClient::FAuthUserInfo Queried;
	FString QueryError;
	const bool bQueried = LoreClient::GetLocalUserInfo(FString(), AuthUrl, Queried, QueryError);
	TestTrue(FString::Printf(TEXT("local user info succeeds (err: %s)"), *QueryError), bQueried);
	TestEqual(TEXT("local user info round-trips the stored identity"), Queried.Id, User.Id);

	// Log out removes the stored token.
	FString LogoutError;
	const bool bLoggedOut = LoreClient::Logout(FString(), AuthUrl, FString(), LogoutError);
	TestTrue(FString::Printf(TEXT("logout clears the stored token (err: %s)"), *LogoutError), bLoggedOut);
	return true;
}
#endif

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
		TestEqual(TEXT("Clean held lock reads as Checked out"), State.GetDisplayName().ToString(), FString(TEXT("Checked out")));
		TestTrue(TEXT("Held-lock tooltip names you"), State.GetDisplayTooltip().ToString().Contains(TEXT("Checked out by you")));
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
		TestTrue(TEXT("LockedOther tooltip names the owner"), State.GetDisplayTooltip().ToString().Contains(TEXT("teammate@example.com")));
	}
	{
		// A local edit must not mask a foreign lock.
		FLoreSourceControlState State(TEXT("C:/Project/G.uasset"));
		State.bUsingLocking = true;
		State.WorkingCopyState = ELoreWorkingCopyState::Modified;
		State.LockState = ELoreLockState::LockedOther;
		State.LockUser = TEXT("teammate@example.com");
		FString Who;
		TestTrue(TEXT("Locally modified file still reads locked by other"), State.IsCheckedOutOther(&Who));
		TestTrue(TEXT("Locally modified file is still modified"), State.IsModified());
		TestFalse(TEXT("Cannot check in a file locked by another"), State.CanCheckIn());
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSourceControlCheckInGuardTest, "LoreSourceControl.CheckInGuard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSourceControlCheckInGuardTest::RunTest(const FString& Parameters)
{
	{
		FLoreSourceControlState State(TEXT("C:/Project/Clean.uasset"));
		State.bUsingLocking = true;
		State.WorkingCopyState = ELoreWorkingCopyState::Modified;
		State.LockState = ELoreLockState::Locked;
		TArray<FString> Messages;
		TestFalse(TEXT("A clean, current, self-locked file does not block submit"), LoreSourceControlUtils::CollectCheckInBlockers({ State }, false, Messages));
		TestEqual(TEXT("No messages for a clean file"), Messages.Num(), 0);
	}
	{
		FLoreSourceControlState State(TEXT("C:/Project/Other.uasset"));
		State.bUsingLocking = true;
		State.WorkingCopyState = ELoreWorkingCopyState::Modified;
		State.LockState = ELoreLockState::LockedOther;
		State.LockUser = TEXT("alice@example.com");
		TArray<FString> Messages;
		TestTrue(TEXT("A file locked by another blocks submit"), LoreSourceControlUtils::CollectCheckInBlockers({ State }, false, Messages));
		TestTrue(TEXT("The block message names the locker"), Messages.Num() == 1 && Messages[0].Contains(TEXT("alice@example.com")));

		// Forcing over the lock lets it through.
		TArray<FString> Forced;
		TestFalse(TEXT("Forcing over the lock does not block submit"), LoreSourceControlUtils::CollectCheckInBlockers({ State }, true, Forced));
	}
	{
		FLoreSourceControlState State(TEXT("C:/Project/Conflicted.uasset"));
		State.WorkingCopyState = ELoreWorkingCopyState::Conflicted;
		TArray<FString> Messages;
		TestTrue(TEXT("A conflicted file blocks submit"), LoreSourceControlUtils::CollectCheckInBlockers({ State }, false, Messages));

		// A conflict still blocks even when forcing over a lock.
		TArray<FString> Forced;
		TestTrue(TEXT("Forcing over a lock does not override a conflict"), LoreSourceControlUtils::CollectCheckInBlockers({ State }, true, Forced));
	}
	{
		// Behind the server no longer blocks; the commit is local and the row warns instead.
		FLoreSourceControlState Stale(TEXT("C:/Project/Stale.uasset"));
		Stale.WorkingCopyState = ELoreWorkingCopyState::Modified;
		Stale.bNewerVersionOnServer = true;
		TArray<FString> Messages;
		TestFalse(TEXT("A file behind the server does not block submit"), LoreSourceControlUtils::CollectCheckInBlockers({ Stale }, false, Messages));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSourceControlStatusParseTest, "LoreSourceControl.StatusParse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSourceControlStatusParseTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Lines = {
		TEXT("{\"tagName\":\"repositoryStatusRevision\",\"data\":{\"branchName\":\"main\",\"isRemoteAhead\":1,\"isLocalAhead\":0,\"remoteAuthorized\":1}}"),
		TEXT("{\"tagName\":\"repositoryStatusFile\",\"data\":{\"path\":\"Config\",\"type\":\"directory\",\"action\":\"add\",\"flagStaged\":false,\"flagDirty\":true}}"),
		TEXT("{\"tagName\":\"repositoryStatusFile\",\"data\":{\"path\":\"staged_add.uasset\",\"type\":\"file\",\"action\":\"add\",\"flagStaged\":true,\"flagDirty\":true}}"),
		TEXT("{\"tagName\":\"repositoryStatusFile\",\"data\":{\"path\":\"untracked.txt\",\"type\":\"file\",\"action\":\"add\",\"flagStaged\":false,\"flagDirty\":true}}"),
		TEXT("{\"tagName\":\"repositoryStatusFile\",\"data\":{\"path\":\"edited.uasset\",\"type\":\"file\",\"action\":\"keep\",\"flagStaged\":false,\"flagDirty\":true}}"),
		TEXT("{\"tagName\":\"repositoryStatusFile\",\"data\":{\"path\":\"clean.uasset\",\"type\":\"file\",\"action\":\"keep\",\"flagStaged\":false,\"flagDirty\":false}}"),
		TEXT("{\"tagName\":\"repositoryStatusFile\",\"data\":{\"path\":\"gone.bin\",\"type\":\"file\",\"action\":\"delete\",\"flagStaged\":true,\"flagDirty\":true}}"),
		TEXT("{\"tagName\":\"repositoryStatusFile\",\"data\":{\"path\":\"new.uasset\",\"type\":\"file\",\"action\":\"move\",\"fromPath\":\"old.uasset\",\"flagStaged\":true,\"flagDirty\":true}}"),
		TEXT("{\"tagName\":\"repositoryStatusFile\",\"data\":{\"path\":\"dup.uasset\",\"type\":\"file\",\"action\":\"copy\",\"flagStaged\":true,\"flagDirty\":true}}"),
		TEXT("{\"tagName\":\"repositoryStatusFile\",\"data\":{\"path\":\"merge.uasset\",\"type\":\"file\",\"action\":\"keep\",\"flagConflict\":true,\"flagConflictUnresolved\":true,\"flagDirty\":true}}"),
		TEXT("{\"tagName\":\"repositoryStatusFile\",\"data\":{\"path\":\"Saved/Autosaves/x.uasset\",\"type\":\"file\",\"action\":\"add\",\"flagStaged\":false,\"flagDirty\":true}}"),
		TEXT("{\"tagName\":\"repositoryStatusFile\",\"data\":{\"path\":\"Plugins/Foo/Intermediate/y.bin\",\"type\":\"file\",\"action\":\"add\",\"flagStaged\":false,\"flagDirty\":true}}"),
		TEXT("{\"tagName\":\"repositoryStatusFile\",\"data\":{\"path\":\".gitignore\",\"type\":\"file\",\"action\":\"keep\",\"flagStaged\":false,\"flagDirty\":true}}"),
		TEXT("{\"tagName\":\"complete\",\"data\":{\"status\":0}}"),
	};
	TMap<FString, ELoreWorkingCopyState::Type> Parsed;
	bool bRemoteAhead = false;
	LoreSourceControlUtils::ParseStatusForTest(Lines, Parsed, bRemoteAhead);

	TestTrue(TEXT("Remote-ahead read from the revision event"), bRemoteAhead);
	TestEqual(TEXT("Directory and transient entries skipped, nine files parsed"), Parsed.Num(), 9);
	TestFalse(TEXT("A Saved path is filtered"), Parsed.Contains(TEXT("Saved/Autosaves/x.uasset")));
	TestFalse(TEXT("A nested Intermediate path is filtered"), Parsed.Contains(TEXT("Plugins/Foo/Intermediate/y.bin")));
	TestTrue(TEXT("A tracked .gitignore is kept (not confused with the .git folder)"), Parsed.Contains(TEXT(".gitignore")) && Parsed[TEXT(".gitignore")] == ELoreWorkingCopyState::Modified);
	TestTrue(TEXT("staged add is Added"), Parsed.Contains(TEXT("staged_add.uasset")) && Parsed[TEXT("staged_add.uasset")] == ELoreWorkingCopyState::Added);
	TestTrue(TEXT("unstaged add is NotControlled"), Parsed.Contains(TEXT("untracked.txt")) && Parsed[TEXT("untracked.txt")] == ELoreWorkingCopyState::NotControlled);
	TestTrue(TEXT("keep with dirty is Modified"), Parsed.Contains(TEXT("edited.uasset")) && Parsed[TEXT("edited.uasset")] == ELoreWorkingCopyState::Modified);
	TestTrue(TEXT("keep without dirty is Unchanged"), Parsed.Contains(TEXT("clean.uasset")) && Parsed[TEXT("clean.uasset")] == ELoreWorkingCopyState::Unchanged);
	TestTrue(TEXT("delete is Deleted"), Parsed.Contains(TEXT("gone.bin")) && Parsed[TEXT("gone.bin")] == ELoreWorkingCopyState::Deleted);
	// A move event keys on the destination path
	TestTrue(TEXT("move is Moved"), Parsed.Contains(TEXT("new.uasset")) && Parsed[TEXT("new.uasset")] == ELoreWorkingCopyState::Moved);
	TestTrue(TEXT("copy is Copied"), Parsed.Contains(TEXT("dup.uasset")) && Parsed[TEXT("dup.uasset")] == ELoreWorkingCopyState::Copied);
	TestTrue(TEXT("conflict flags make Conflicted"), Parsed.Contains(TEXT("merge.uasset")) && Parsed[TEXT("merge.uasset")] == ELoreWorkingCopyState::Conflicted);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSourceControlAuthParseTest, "LoreSourceControl.AuthParse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSourceControlAuthParseTest::RunTest(const FString& Parameters)
{
	{
		// An unauthenticated server reports the remote available and the client authorized, so the row reads not required.
		const TArray<FString> Lines = {
			TEXT("{\"tagName\":\"repositoryStatusRevision\",\"data\":{\"branchName\":\"main\",\"remoteAvailable\":1,\"remoteAuthorized\":1}}"),
			TEXT("{\"tagName\":\"complete\",\"data\":{\"status\":0}}"),
		};
		bool bAvailable = false, bAuthorized = false;
		LoreSourceControlUtils::ParseRemoteAuthForTest(Lines, bAvailable, bAuthorized);
		TestTrue(TEXT("Remote reads available"), bAvailable);
		TestTrue(TEXT("Client reads authorized"), bAuthorized);
	}
	{
		// A server that requires a login the client lacks reads available but not authorized, so the sign-in button shows.
		const TArray<FString> Lines = {
			TEXT("{\"tagName\":\"repositoryStatusRevision\",\"data\":{\"branchName\":\"main\",\"remoteAvailable\":1,\"remoteAuthorized\":0}}"),
		};
		bool bAvailable = false, bAuthorized = true;
		LoreSourceControlUtils::ParseRemoteAuthForTest(Lines, bAvailable, bAuthorized);
		TestTrue(TEXT("Remote reads available"), bAvailable);
		TestFalse(TEXT("Client reads not authorized"), bAuthorized);
	}
	{
		// An unreachable remote sets neither flag, so the row reads not reachable.
		const TArray<FString> Lines = {
			TEXT("{\"tagName\":\"repositoryStatusRevision\",\"data\":{\"branchName\":\"main\",\"remoteAvailable\":0,\"remoteAuthorized\":0}}"),
		};
		bool bAvailable = true, bAuthorized = true;
		LoreSourceControlUtils::ParseRemoteAuthForTest(Lines, bAvailable, bAuthorized);
		TestFalse(TEXT("Remote reads not reachable"), bAvailable);
		TestFalse(TEXT("Not authorized when unreachable"), bAuthorized);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSourceControlLockParseTest, "LoreSourceControl.LockParse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSourceControlLockParseTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Lines = {
		TEXT("{\"tagName\":\"lockFileStatusBegin\",\"data\":{\"count\":2}}"),
		TEXT("{\"tagName\":\"lockFileStatus\",\"data\":{\"path\":\"asset.bin\",\"owner\":\"alice@example.com\",\"lockedAt\":1781854778871}}"),
		TEXT("{\"tagName\":\"lockFileStatus\",\"data\":{\"path\":\"Content/Stand by Me.uasset\",\"owner\":\"bob@example.com\",\"lockedAt\":1781854778872}}"),
		TEXT("{\"tagName\":\"complete\",\"data\":{\"status\":0}}"),
		TEXT("{\"tagName\":\"error\",\"data\":{\"errorType\":4294967295,\"errorInner\":\"No auth endpoint available\"}}"),
		TEXT("{\"tagName\":\"complete\",\"data\":{\"status\":1}}"),
	};
	TMap<FString, FString> Locks;
	LoreSourceControlUtils::ParseLocksForTest(Lines, Locks);

	TestEqual(TEXT("Two locks parsed, the trailing auth error ignored"), Locks.Num(), 2);
	TestTrue(TEXT("asset.bin owned by alice"), Locks.Contains(TEXT("asset.bin")) && Locks[TEXT("asset.bin")] == TEXT("alice@example.com"));
	// A path with " by " in it carries no ambiguity in JSON
	TestTrue(TEXT("path with spaces parsed whole"), Locks.Contains(TEXT("Content/Stand by Me.uasset")) && Locks[TEXT("Content/Stand by Me.uasset")] == TEXT("bob@example.com"));

	// Non-event lines are never records
	const TArray<FString> Noise = { TEXT("some banner on startup"), TEXT("") };
	TMap<FString, FString> NoLocks;
	LoreSourceControlUtils::ParseLocksForTest(Noise, NoLocks);
	TestEqual(TEXT("Noise yields no locks"), NoLocks.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSyncDeltaParseTest, "LoreSourceControl.SyncDeltaParse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSyncDeltaParseTest::RunTest(const FString& Parameters)
{
	// A dry-run sync's revisionSyncFile events name each file a sync would touch, with the action token and flagFile. A directory entry and a non-event line carry no file to protect.
	const TArray<FString> Lines = {
		TEXT("{\"tagName\":\"revisionSyncTarget\",\"data\":{\"branchName\":\"main\"}}"),
		TEXT("{\"tagName\":\"revisionSyncFile\",\"data\":{\"path\":\"Content/Maps/Arena.umap\",\"size\":4096,\"action\":\"keep\",\"flagFile\":1}}"),
		TEXT("{\"tagName\":\"revisionSyncFile\",\"data\":{\"path\":\"Content/Props/Crate.uasset\",\"size\":2048,\"action\":\"add\",\"flagFile\":1}}"),
		TEXT("{\"tagName\":\"revisionSyncFile\",\"data\":{\"path\":\"Content/Old.uasset\",\"size\":0,\"action\":\"delete\",\"flagFile\":1}}"),
		TEXT("{\"tagName\":\"revisionSyncFile\",\"data\":{\"path\":\"Content\\\\Props\",\"size\":0,\"action\":\"keep\",\"flagFile\":0}}"),
		TEXT("some banner"),
		TEXT("{\"tagName\":\"complete\",\"data\":{\"status\":0}}"),
	};
	TArray<FLoreDeltaEntry> Delta;
	LoreSourceControlUtils::ParseSyncDeltaForTest(Lines, Delta);

	TestEqual(TEXT("Three files parsed, the directory entry skipped"), Delta.Num(), 3);
	TestEqual(TEXT("First path is the map"), Delta[0].Path, FString(TEXT("Content/Maps/Arena.umap")));
	TestEqual(TEXT("A modified file reads keep"), Delta[0].Action, FString(TEXT("keep")));
	TestEqual(TEXT("An added file reads add"), Delta[1].Action, FString(TEXT("add")));
	TestEqual(TEXT("A removed file reads delete"), Delta[2].Action, FString(TEXT("delete")));

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

	// Search order for loreserver: the LORESERVER override, next to the client, then on PATH.
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

			// Pin every store to our own path so none falls back to the shared <tmp>/lore-server a developer's server also uses. TOML basic strings read backslashes as escapes, so emit the path with forward slashes; the server accepts them on Windows. lock_store must be pinned too, or the repository registry it backs leaks to the shared store.
			FString StorePath = StoreDir;
			StorePath.ReplaceInline(TEXT("\\"), TEXT("/"));
			const FString Toml = FString::Printf(
				TEXT("[server.quic]\nport = %d\n\n[server.grpc]\nport = %d\n\n[server.http]\nport = %d\n\n[immutable_store.local]\npath = \"%s\"\n\n[mutable_store.local]\npath = \"%s\"\n\n[lock_store.local]\npath = \"%s\"\n"),
				QuicPort, QuicPort, HttpPort, *StorePath, *StorePath, *StorePath);
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

	// One repository on a freshly started loreserver, with a working tree, for a live test. Construct, then check IsReady; the destructor stops the server and deletes the tree. The common preamble (find the client, start the server, make a tree, create the repository) lives here so each live test states only what it exercises.
	struct FLoreTestFixture
	{
		FString Binary;
		FString Version;
		FLoreManagedServer Server;
		FString Root;
		FString RepoUrl;
		bool bReady = false;

		FLoreTestFixture(FAutomationTestBase& Test, const TCHAR* RootLabel, const TCHAR* RepoPrefix)
		{
			Binary = LoreSourceControlUtils::FindLoreBinaryPath();
			if (!LoreSourceControlUtils::CheckLoreAvailability(ELoreBackendMode::CommandLine, Binary, Version))
			{
				Test.AddError(TEXT("Lore client not found; this live test requires it."));
				return;
			}
			if (!Server.Start(Binary, Test))
			{
				return;
			}
			Root = FPaths::Combine(FString(FPlatformProcess::UserTempDir()), RootLabel, FGuid::NewGuid().ToString(EGuidFormats::Digits));
			IFileManager::Get().MakeDirectory(*Root, true);
			RepoUrl = FString::Printf(TEXT("%s/%s-%s"), *Server.ServerBase, RepoPrefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
			TArray<FString> Results, Errors;
			const bool bCreated = LoreSourceControlUtils::RunCommand(FString::Printf(TEXT("--identity test@example.com repository create %s"), *RepoUrl), Binary, Root, TArray<FString>(), TArray<FString>(), Results, Errors);
			if (!bCreated)
			{
				Test.AddError(TEXT("Could not create the repository on the test server."));
				return;
			}
			bReady = true;
		}

		~FLoreTestFixture()
		{
			if (!Root.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Root, false, true);
			}
		}

		// Clone the same repository into a second sibling tree, for divergence (remote ahead or behind, push races). Returns the absolute clone path, empty on failure.
		FString CloneSecondTree(FAutomationTestBase& Test, const TCHAR* CloneLeaf)
		{
			const FString Parent = FPaths::Combine(FString(FPlatformProcess::UserTempDir()), TEXT("LoreSecondClone"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
			IFileManager::Get().MakeDirectory(*Parent, true);
			const FString CloneRoot = Parent / CloneLeaf;
			TArray<FString> Results, Errors;
			const bool bCloned = LoreSourceControlUtils::RunCommand(FString::Printf(TEXT("--identity test@example.com clone %s \"%s\""), *RepoUrl, *CloneRoot), Binary, Parent, TArray<FString>(), TArray<FString>(), Results, Errors);
			if (!bCloned)
			{
				Test.AddError(TEXT("Could not clone a second working tree."));
				return FString();
			}
			return CloneRoot;
		}
	};

	// Stage one relative file, commit with a message, then push, through the active backend. The common "land a revision on the server" step for a live test.
	bool StageCommitPush(const FLoreTestFixture& Fixture, const FString& RelativeFile, const FString& Message)
	{
		using namespace LoreSourceControlUtils;
		TArray<FString> Errors;
		if (!StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, { RelativeFile }, Errors))
		{
			return false;
		}
		if (!CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), Message, Errors))
		{
			return false;
		}
		return PushBranch(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, Errors);
	}

	// Point the registered provider at a fixture's working tree, returning the base interface. The caller restores the previous provider with ON_SCOPE_EXIT.
	ISourceControlProvider& BeginProviderOn(const FLoreTestFixture& Fixture, FName& OutPrevious)
	{
		ISourceControlModule& SourceControl = ISourceControlModule::Get();
		OutPrevious = SourceControl.GetProvider().GetName();
		SourceControl.SetProvider(FName("Lore"));
		FLoreSourceControlProvider& LoreProvider = FLoreSourceControlModule::Get().GetProvider();
		LoreProvider.Init();
		LoreProvider.SetRepositoryInfo(true, true, Fixture.Root, Fixture.RepoUrl, TEXT("test@example.com"), TEXT("main"), FString(), Fixture.Version);
		return SourceControl.GetProvider();
	}
}

// Test all utilities against a loreserver.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSourceControlIntegrationTest, "LoreSourceControl.Integration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSourceControlIntegrationTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	const FString Binary = FindLoreBinaryPath();
	FString Version;
	if (!CheckLoreAvailability(ELoreBackendMode::CommandLine, Binary, Version))
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
		RunUpdateStatus(ELoreBackendMode::Automatic, Binary, Root, TEXT("test@example.com"), bLocking, Files, StatusErrors, States);
		return States.Num() == 1 ? States[0] : FLoreSourceControlState(File);
	};

#if LORE_WITH_LIBLORE
	// The liblore status path must classify identically to the CLI JSON path for the same files; the migration depends on the two staying in lock step.
	auto CompareStatusPaths = [&](const TArray<FString>& InAbsoluteFiles, const TCHAR* Label)
	{
		const TArray<FString> RelativeFiles = RelativeFilenames(InAbsoluteFiles, Root);

		TArray<FString> CliResults, CliErrors;
		RunCommand(TEXT("status"), Binary, Root, { TEXT("--scan"), TEXT("--json") }, RelativeFiles, CliResults, CliErrors);
		TMap<FString, ELoreWorkingCopyState::Type> CliStates;
		bool bCliAhead = false;
		ParseStatusForTest(CliResults, CliStates, bCliAhead);

		LoreClient::FStatusResult ClientResult;
		FString ClientError;
		const bool bClientOk = LoreClient::GetStatus(Root, TEXT("test@example.com"), RelativeFiles, true, ClientResult, ClientError);
		TestTrue(FString::Printf(TEXT("liblore status succeeds (%s)"), Label), bClientOk);
		if (!bClientOk)
		{
			AddInfo(FString::Printf(TEXT("liblore GetStatus failed (%s) for root '%s': %s"), Label, *Root, *ClientError));
		}
		TMap<FString, ELoreWorkingCopyState::Type> ClientStates;
		bool bClientAhead = false;
		ParseStatusFromClient(ClientResult, ClientStates, bClientAhead);

		TestEqual(FString::Printf(TEXT("liblore and CLI agree on reported file count (%s)"), Label), ClientStates.Num(), CliStates.Num());
		TestEqual(FString::Printf(TEXT("liblore and CLI agree on remote-ahead (%s)"), Label), bClientAhead, bCliAhead);
		for (const TPair<FString, ELoreWorkingCopyState::Type>& Pair : CliStates)
		{
			const ELoreWorkingCopyState::Type* Found = ClientStates.Find(Pair.Key);
			TestNotNull(*FString::Printf(TEXT("liblore reports %s (%s)"), *Pair.Key, Label), Found);
			if (Found)
			{
				TestEqual(*FString::Printf(TEXT("liblore and CLI agree on state for %s (%s)"), *Pair.Key, Label), (int32)*Found, (int32)Pair.Value);
			}
		}
	};
#endif

	TestEqual(TEXT("Untracked file reads as not controlled"), (int32)StateOf(false).WorkingCopyState, (int32)ELoreWorkingCopyState::NotControlled);

	// Out-of-tree paths (engine content) are skipped, not failed, and read as Unknown so the submit dialog never offers them for add.
	{
		const FString OutsidePath = FPaths::Combine(FString(FPlatformProcess::UserTempDir()), TEXT("LoreOutsideProbe"), TEXT("outside.txt"));
		TArray<FLoreSourceControlState> MixedStates;
		TArray<FString> MixedErrors;
		const bool bMixedOk = RunUpdateStatus(ELoreBackendMode::Automatic, Binary, Root, TEXT("test@example.com"), false, { File, OutsidePath }, MixedErrors, MixedStates);
		TestTrue(TEXT("Status with an out-of-tree path still succeeds"), bMixedOk);
		TestEqual(TEXT("Both paths return a state"), MixedStates.Num(), 2);
		if (MixedStates.Num() == 2)
		{
			TestEqual(TEXT("Out-of-tree path reads as Unknown"), (int32)MixedStates[1].WorkingCopyState, (int32)ELoreWorkingCopyState::Unknown);
		}
	}

	RunCommand(TEXT("stage"), Binary, Root, TArray<FString>(), { TEXT("hello.txt") }, Results, Errors);
	TestEqual(TEXT("Staged file reads as added"), (int32)StateOf(false).WorkingCopyState, (int32)ELoreWorkingCopyState::Added);
#if LORE_WITH_LIBLORE
	CompareStatusPaths(Files, TEXT("staged add"));
#endif

	const bool bCommitted = RunCommand(TEXT("commit"), Binary, Root, { TEXT("\"integration commit\"") }, TArray<FString>(), Results, Errors);
	TestTrue(TEXT("Commit succeeds"), bCommitted);
	TestEqual(TEXT("Committed file reads as unchanged"), (int32)StateOf(false).WorkingCopyState, (int32)ELoreWorkingCopyState::Unchanged);

	// An edited committed asset reads back as modified.
	FFileHelper::SaveStringToFile(TEXT("Hello Lore, edited"), *File);
	TestEqual(TEXT("Edited committed file reads as modified"), (int32)StateOf(false).WorkingCopyState, (int32)ELoreWorkingCopyState::Modified);
	FFileHelper::SaveStringToFile(TEXT("Hello Lore"), *File);

	// Commit is local, so push then sync exercise the server. These go through the liblore wrappers on Win64.
	const bool bPushed = PushBranch(ELoreBackendMode::Automatic, Binary, Root, Errors);
	TestTrue(TEXT("Push succeeds"), bPushed);
	const bool bSynced = SyncWorkingTree(ELoreBackendMode::Automatic, Binary, Root, Errors);
	TestTrue(TEXT("Sync succeeds"), bSynced);
	TestTrue(TEXT("Synced file is current"), StateOf(false).IsCurrent());

	// Backend modes: native connect reads branch and remote flags with no command line; command-line mode forces the subprocess.
#if LORE_WITH_LIBLORE
	{
		FString NativeBranch;
		const bool bNativeBranchOk = GetBranchName(ELoreBackendMode::Native, Binary, Root, NativeBranch);
		TestTrue(TEXT("Native branch read succeeds"), bNativeBranchOk);
		TestFalse(TEXT("Native branch is non-empty"), NativeBranch.IsEmpty());
		bool bNativeAvailable = false, bNativeAuthorized = false;
		GetRemoteAuthState(ELoreBackendMode::Native, Binary, Root, bNativeAvailable, bNativeAuthorized);
		TestTrue(TEXT("Native remote-auth read reports the remote reachable"), bNativeAvailable);
	}
#endif
	{
		TArray<FLoreSourceControlState> CliModeStates;
		TArray<FString> CliModeErrors;
		const bool bCliModeOk = RunUpdateStatus(ELoreBackendMode::CommandLine, Binary, Root, TEXT("test@example.com"), false, Files, CliModeErrors, CliModeStates);
		TestTrue(TEXT("Command-line-mode status succeeds"), bCliModeOk);
	}

	TLoreSourceControlHistory History;
	RunGetHistory(ELoreBackendMode::Automatic, Binary, Root, File, Errors, History);
	TestTrue(TEXT("History has at least one revision"), History.Num() >= 1);
	if (History.Num() >= 1)
	{
		TestFalse(TEXT("Revision carries a commit id"), History[0]->GetRevision().IsEmpty());
		TestEqual(TEXT("Revision description is the commit message"), History[0]->GetDescription(), FString(TEXT("integration commit")));
		TestEqual(TEXT("Revision author is the commit identity"), History[0]->GetUserName(), FString(TEXT("test@example.com")));
		TestEqual(TEXT("Creation revision action reads as Add"), History[0]->GetAction(), FString(TEXT("Add")));
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

#if LORE_WITH_LIBLORE
		// Confirm the native file write works on its own, not only because the CLI fallback covers for it.
		const FString NativeOut = FPaths::Combine(Root, TEXT("native_write_probe.txt"));
		IFileManager::Get().Delete(*NativeOut, false, true);
		FString WriteErr;
		const bool bNativeWrite = LoreClient::WriteFile(Root, RelativeFilename(File, Root), History[0]->CommitId, NativeOut, WriteErr);
		TestTrue(TEXT("liblore file write succeeds"), bNativeWrite);
		if (!bNativeWrite)
		{
			AddInfo(FString::Printf(TEXT("liblore WriteFile failed: %s"), *WriteErr));
		}
		else
		{
			TestTrue(TEXT("native-written file is non-empty"), IFileManager::Get().FileSize(*NativeOut) > 0);
		}
#endif
	}

	// A second commit makes the latest revision an edit, which the JSON history reports as the "keep" action; confirm it reads as Keep (Edit).
	FFileHelper::SaveStringToFile(TEXT("Hello Lore, second revision"), *File);
	RunCommand(TEXT("stage"), Binary, Root, TArray<FString>(), { TEXT("hello.txt") }, Results, Errors);
	RunCommand(TEXT("commit"), Binary, Root, { TEXT("\"edit commit\"") }, TArray<FString>(), Results, Errors);
	TLoreSourceControlHistory History2;
	RunGetHistory(ELoreBackendMode::Automatic, Binary, Root, File, Errors, History2);
	TestTrue(TEXT("History has two revisions after a second commit"), History2.Num() >= 2);
	if (History2.Num() >= 2)
	{
		TestEqual(TEXT("Latest revision action reads as Keep (Edit)"), History2[0]->GetAction(), FString(TEXT("Keep (Edit)")));
	}

	RunCommand(TEXT("lock acquire"), Binary, Root, TArray<FString>(), { TEXT("hello.txt") }, Results, Errors);
	// With authentication off the lock owner is unattributable and reads as this user's own.
	const ELoreLockState::Type LockState = StateOf(true).LockState;
	TestEqual(TEXT("Locked file reads as checked out by this user"), (int32)LockState, (int32)ELoreLockState::Locked);
#if LORE_WITH_LIBLORE
	// Differential: the liblore lock read must report the same locked paths as the CLI lock-status command. Owner strings are unattributable here, so compare the locked set, not the owner literal.
	{
		const TArray<FString> LockRel = RelativeFilenames(Files, Root);
		TArray<FString> CliLockResults, CliLockErrors;
		RunCommand(TEXT("lock status"), Binary, Root, { TEXT("--json") }, LockRel, CliLockResults, CliLockErrors);
		TMap<FString, FString> CliLocks;
		ParseLocksForTest(CliLockResults, CliLocks);

		TArray<LoreClient::FLockStatus> ClientLockList;
		FString LockErr;
		const bool bLockOk = LoreClient::GetLockStatus(Root, LockRel, ClientLockList, LockErr);
		TestTrue(TEXT("liblore lock status succeeds"), bLockOk);
		if (!bLockOk)
		{
			AddInfo(FString::Printf(TEXT("liblore GetLockStatus failed: %s"), *LockErr));
		}
		TMap<FString, FString> ClientLocks;
		ParseLocksFromClient(ClientLockList, ClientLocks);

		TestEqual(TEXT("liblore and CLI agree on locked file count"), ClientLocks.Num(), CliLocks.Num());
		for (const TPair<FString, FString>& Pair : CliLocks)
		{
			TestTrue(*FString::Printf(TEXT("liblore reports a lock on %s"), *Pair.Key), ClientLocks.Contains(Pair.Key));
		}
	}
#endif

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
	// Exercise liblore unstage-all (the repository root path) on Win64; the assertions below confirm beta is not left staged.
	UnstageFiles(ELoreBackendMode::Automatic, Binary, Root, { TEXT(".") }, Errors);
	RunCommand(TEXT("stage"), Binary, Root, TArray<FString>(), { TEXT("alpha.txt") }, Results, Errors);
	RunCommand(TEXT("commit"), Binary, Root, { TEXT("\"partial submit\"") }, TArray<FString>(), Results, Errors);

	auto WorkingStateOf = [&](const FString& Absolute) -> ELoreWorkingCopyState::Type
	{
		TArray<FLoreSourceControlState> PartialStates;
		TArray<FString> PartialErrors;
		RunUpdateStatus(ELoreBackendMode::Automatic, Binary, Root, TEXT("test@example.com"), false, { Absolute }, PartialErrors, PartialStates);
		return PartialStates.Num() == 1 ? PartialStates[0].WorkingCopyState : ELoreWorkingCopyState::Unknown;
	};
	TestEqual(TEXT("Submitted file is committed"), (int32)WorkingStateOf(FileA), (int32)ELoreWorkingCopyState::Unchanged);
	TestEqual(TEXT("Unselected file is not committed"), (int32)WorkingStateOf(FileB), (int32)ELoreWorkingCopyState::NotControlled);
#if LORE_WITH_LIBLORE
	CompareStatusPaths({ FileA, FileB }, TEXT("post-partial-submit"));
#endif

	return true;
}

// Test operations through the registered provider, the path the engine tests use.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSourceControlProviderTest, "LoreSourceControl.Provider", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSourceControlProviderTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	const FString Binary = FindLoreBinaryPath();
	FString Version;
	if (!CheckLoreAvailability(ELoreBackendMode::CommandLine, Binary, Version))
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

	// Check out: with authentication off a lock reads as this user's own.
	const bool bWasLocking = FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();
	FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(true);
	ON_SCOPE_EXIT { FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(bWasLocking); };

	const ECommandResult::Type CheckOutResult = Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("CheckOut through the provider succeeds"), (int32)CheckOutResult, (int32)ECommandResult::Succeeded);
	const FSourceControlStatePtr CheckedOutState = Provider.GetState(TempFile, EStateCacheUsage::Use);
	TestTrue(TEXT("Checked-out file reports IsCheckedOut"), CheckedOutState.IsValid() && CheckedOutState->IsCheckedOut());

	// Empty submit with no changes should succeed.
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

	// A committed binary asset stays read-only until this user checks it out.
	const FString BinaryFile = FPaths::ConvertRelativePathToFull(Root / TEXT("readonly_probe.uasset"));
	FFileHelper::SaveStringToFile(TEXT("binary probe"), *BinaryFile);
	const TArray<FString> BinaryTargets = { BinaryFile };
	Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), BinaryTargets, EConcurrency::Synchronous);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> BinaryCheckIn = ISourceControlOperation::Create<FCheckIn>();
	BinaryCheckIn->SetDescription(FText::FromString(TEXT("add binary probe")));
	Provider.Execute(BinaryCheckIn, BinaryTargets, EConcurrency::Synchronous);

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), BinaryTargets, EConcurrency::Synchronous);
	TestTrue(TEXT("Committed binary asset is read-only until checked out"), PlatformFile.IsReadOnly(*BinaryFile));
	Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), BinaryTargets, EConcurrency::Synchronous);
	TestFalse(TEXT("Checked-out binary asset is writable"), PlatformFile.IsReadOnly(*BinaryFile));
	Provider.Execute(ISourceControlOperation::Create<FRevert>(), BinaryTargets, EConcurrency::Synchronous);
	TestTrue(TEXT("Reverted binary asset is read-only again"), PlatformFile.IsReadOnly(*BinaryFile));

	// Delete marks the file deleted.
	const FString DeleteFile = FPaths::ConvertRelativePathToFull(Root / TEXT("delete_probe.txt"));
	FFileHelper::SaveStringToFile(TEXT("delete probe"), *DeleteFile);
	const TArray<FString> DeleteTargets = { DeleteFile };
	Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), DeleteTargets, EConcurrency::Synchronous);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> DeleteCheckIn = ISourceControlOperation::Create<FCheckIn>();
	DeleteCheckIn->SetDescription(FText::FromString(TEXT("add delete probe")));
	Provider.Execute(DeleteCheckIn, DeleteTargets, EConcurrency::Synchronous);
	const ECommandResult::Type DeleteResult = Provider.Execute(ISourceControlOperation::Create<FDelete>(), DeleteTargets, EConcurrency::Synchronous);
	TestEqual(TEXT("Delete through the provider succeeds"), (int32)DeleteResult, (int32)ECommandResult::Succeeded);
	const FSourceControlStatePtr DeletedState = Provider.GetState(DeleteFile, EStateCacheUsage::Use);
	TestTrue(TEXT("Deleted file reports IsDeleted"), DeletedState.IsValid() && DeletedState->IsDeleted());

	// Reverting a newly added file returns it to not-controlled.
	const FString AddRevertFile = FPaths::ConvertRelativePathToFull(Root / TEXT("addrevert_probe.txt"));
	FFileHelper::SaveStringToFile(TEXT("add revert probe"), *AddRevertFile);
	const TArray<FString> AddRevertTargets = { AddRevertFile };
	Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), AddRevertTargets, EConcurrency::Synchronous);
	const FSourceControlStatePtr StagedProbe = Provider.GetState(AddRevertFile, EStateCacheUsage::Use);
	TestTrue(TEXT("Staged probe reports IsAdded"), StagedProbe.IsValid() && StagedProbe->IsAdded());
	Provider.Execute(ISourceControlOperation::Create<FRevert>(), AddRevertTargets, EConcurrency::Synchronous);
	const FSourceControlStatePtr UnaddedProbe = Provider.GetState(AddRevertFile, EStateCacheUsage::Use);
	TestFalse(TEXT("Reverted added file is no longer added"), UnaddedProbe.IsValid() && UnaddedProbe->IsAdded());

	// A single operation marks several files for add at once, exercising the batched file list.
	const FString BatchA = FPaths::ConvertRelativePathToFull(Root / TEXT("batch_a.txt"));
	const FString BatchB = FPaths::ConvertRelativePathToFull(Root / TEXT("batch_b.txt"));
	FFileHelper::SaveStringToFile(TEXT("batch a"), *BatchA);
	FFileHelper::SaveStringToFile(TEXT("batch b"), *BatchB);
	const TArray<FString> BatchTargets = { BatchA, BatchB };
	Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), BatchTargets, EConcurrency::Synchronous);
	const FSourceControlStatePtr BatchAState = Provider.GetState(BatchA, EStateCacheUsage::Use);
	const FSourceControlStatePtr BatchBState = Provider.GetState(BatchB, EStateCacheUsage::Use);
	TestTrue(TEXT("First batched file is added"), BatchAState.IsValid() && BatchAState->IsAdded());
	TestTrue(TEXT("Second batched file is added"), BatchBState.IsValid() && BatchBState->IsAdded());

	// Move or rename: FCopy stages the destination so the moved asset is tracked (the editor copies content to the new path and redirects the old).
	const FString MoveDest = FPaths::ConvertRelativePathToFull(Root / TEXT("moved.txt"));
	FFileHelper::SaveStringToFile(TEXT("moved content"), *MoveDest);
	TSharedRef<FCopy, ESPMode::ThreadSafe> CopyOp = ISourceControlOperation::Create<FCopy>();
	CopyOp->SetDestination(MoveDest);
	const ECommandResult::Type CopyResult = Provider.Execute(CopyOp, TArray<FString>{ MoveDest }, EConcurrency::Synchronous);
	TestEqual(TEXT("Copy through the provider succeeds"), (int32)CopyResult, (int32)ECommandResult::Succeeded);
	const FSourceControlStatePtr MovedState = Provider.GetState(MoveDest, EStateCacheUsage::Use);
	TestTrue(TEXT("Moved destination is staged for add"), MovedState.IsValid() && MovedState->IsAdded());

	return true;
}

// Regression tests --------------------------------------------------------------
// Each pins a bug that was fixed (or, for the historical diff, is still under investigation). The name states the behavior so a failure names the regression.

// Bug 1+2: after a normal submit with locking on and keep-checked-out off, the check-in worker releases the held lock and asserts NotLocked, so a committed lockable asset ends read-only and not checked out, and a text file ends writable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreRegressionSubmitReleasesLockTest, "LoreSourceControl.Regression.SubmitReleasesLockAndWritable", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreRegressionSubmitReleasesLockTest::RunTest(const FString& Parameters)
{
	FLoreTestFixture Fixture(*this, TEXT("LoreRegSubmitLock"), TEXT("rsl"));
	if (!Fixture.bReady)
	{
		return false;
	}

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	const bool bWasLocking = FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();
	FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(true);
	ON_SCOPE_EXIT { FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(bWasLocking); };

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	// A lockable asset is .uasset by extension; the content is plain text, which Lore stores as bytes regardless.
	const FString Asset = FPaths::ConvertRelativePathToFull(Fixture.Root / TEXT("locktest.uasset"));
	FFileHelper::SaveStringToFile(TEXT("asset version one"), *Asset);
	const TArray<FString> AssetTargets = { Asset };

	Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), AssetTargets, EConcurrency::Synchronous);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> AddCheckIn = ISourceControlOperation::Create<FCheckIn>();
	AddCheckIn->SetDescription(FText::FromString(TEXT("add lockable asset")));
	Provider.Execute(AddCheckIn, AssetTargets, EConcurrency::Synchronous);

	// Check out: lock acquired, read-only cleared.
	const ECommandResult::Type CheckOutResult = Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), AssetTargets, EConcurrency::Synchronous);
	TestEqual(TEXT("Check out the lockable asset succeeds"), (int32)CheckOutResult, (int32)ECommandResult::Succeeded);
	const FSourceControlStatePtr CheckedOut = Provider.GetState(Asset, EStateCacheUsage::Use);
	TestTrue(TEXT("Asset reads as checked out after check out"), CheckedOut.IsValid() && CheckedOut->IsCheckedOut());
	TestFalse(TEXT("Checked-out lockable asset is writable on disk"), PlatformFile.IsReadOnly(*Asset));

	// Edit and submit with keep-checked-out off (the default).
	FFileHelper::SaveStringToFile(TEXT("asset version two"), *Asset);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> EditCheckIn = ISourceControlOperation::Create<FCheckIn>();
	EditCheckIn->SetDescription(FText::FromString(TEXT("edit lockable asset")));
#if LORE_UE5_1_OR_LATER
	EditCheckIn->SetKeepCheckedOut(false);
#endif
	const ECommandResult::Type EditResult = Provider.Execute(EditCheckIn, AssetTargets, EConcurrency::Synchronous);
	TestEqual(TEXT("Submitting the edited lockable asset succeeds"), (int32)EditResult, (int32)ECommandResult::Succeeded);

	const FSourceControlStatePtr Submitted = Provider.GetState(Asset, EStateCacheUsage::Use);
	TestFalse(TEXT("Submitted lockable asset is no longer checked out"), Submitted.IsValid() && Submitted->IsCheckedOut());
	TestTrue(TEXT("Submitted lockable asset is read-only on disk again"), PlatformFile.IsReadOnly(*Asset));

	// A non-lockable text file never takes a lock and stays writable after submit.
	const FString TextFile = FPaths::ConvertRelativePathToFull(Fixture.Root / TEXT("locktest.txt"));
	FFileHelper::SaveStringToFile(TEXT("text version one"), *TextFile);
	const TArray<FString> TextTargets = { TextFile };
	Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), TextTargets, EConcurrency::Synchronous);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> TextCheckIn = ISourceControlOperation::Create<FCheckIn>();
	TextCheckIn->SetDescription(FText::FromString(TEXT("add text file")));
	const ECommandResult::Type TextResult = Provider.Execute(TextCheckIn, TextTargets, EConcurrency::Synchronous);
	TestEqual(TEXT("Submitting the text file succeeds"), (int32)TextResult, (int32)ECommandResult::Succeeded);
	TestFalse(TEXT("Submitted text file stays writable on disk"), PlatformFile.IsReadOnly(*TextFile));

	return true;
}

// Bug 2: a re-edit and re-submit of a checked-out lockable asset still succeeds and records a new revision, with no save error left on the operation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreRegressionReEditThenSubmitTest, "LoreSourceControl.Regression.ReEditThenSubmitSucceeds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreRegressionReEditThenSubmitTest::RunTest(const FString& Parameters)
{
	FLoreTestFixture Fixture(*this, TEXT("LoreRegReEdit"), TEXT("rre"));
	if (!Fixture.bReady)
	{
		return false;
	}

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	const bool bWasLocking = FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();
	FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(true);
	ON_SCOPE_EXIT { FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(bWasLocking); };

	const FString Asset = FPaths::ConvertRelativePathToFull(Fixture.Root / TEXT("reedit.uasset"));
	FFileHelper::SaveStringToFile(TEXT("reedit version one"), *Asset);
	const TArray<FString> AssetTargets = { Asset };

	Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), AssetTargets, EConcurrency::Synchronous);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> AddCheckIn = ISourceControlOperation::Create<FCheckIn>();
	AddCheckIn->SetDescription(FText::FromString(TEXT("add reedit asset")));
	Provider.Execute(AddCheckIn, AssetTargets, EConcurrency::Synchronous);

	// First normal submit of a checked-out edit.
	Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), AssetTargets, EConcurrency::Synchronous);
	FFileHelper::SaveStringToFile(TEXT("reedit version two"), *Asset);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> FirstCheckIn = ISourceControlOperation::Create<FCheckIn>();
	FirstCheckIn->SetDescription(FText::FromString(TEXT("first reedit submit")));
	const ECommandResult::Type FirstResult = Provider.Execute(FirstCheckIn, AssetTargets, EConcurrency::Synchronous);
	TestEqual(TEXT("First re-edit submit succeeds"), (int32)FirstResult, (int32)ECommandResult::Succeeded);

	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> FirstHistory = ISourceControlOperation::Create<FUpdateStatus>();
	FirstHistory->SetUpdateHistory(true);
	Provider.Execute(FirstHistory, AssetTargets, EConcurrency::Synchronous);
	const FSourceControlStatePtr AfterFirst = Provider.GetState(Asset, EStateCacheUsage::Use);
	const int32 HistoryAfterFirst = AfterFirst.IsValid() ? AfterFirst->GetHistorySize() : 0;
	TestTrue(TEXT("History has at least one revision after the first submit"), HistoryAfterFirst >= 1);

	// Second check out, edit, submit; this is the path the lock-release fix had to keep working.
	Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), AssetTargets, EConcurrency::Synchronous);
	FFileHelper::SaveStringToFile(TEXT("reedit version three"), *Asset);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> SecondCheckIn = ISourceControlOperation::Create<FCheckIn>();
	SecondCheckIn->SetDescription(FText::FromString(TEXT("second reedit submit")));
	const ECommandResult::Type SecondResult = Provider.Execute(SecondCheckIn, AssetTargets, EConcurrency::Synchronous);
	TestEqual(TEXT("Second re-edit submit succeeds"), (int32)SecondResult, (int32)ECommandResult::Succeeded);

	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> SecondHistory = ISourceControlOperation::Create<FUpdateStatus>();
	SecondHistory->SetUpdateHistory(true);
	Provider.Execute(SecondHistory, AssetTargets, EConcurrency::Synchronous);
	const FSourceControlStatePtr AfterSecond = Provider.GetState(Asset, EStateCacheUsage::Use);
	const int32 HistoryAfterSecond = AfterSecond.IsValid() ? AfterSecond->GetHistorySize() : 0;
	TestTrue(TEXT("History grew by one after the second submit"), HistoryAfterSecond > HistoryAfterFirst);

	return true;
}

// Bug 3: diffing against the depot extracts the committed server revision, not the working file. An empty revision id used to make liblore write the working tree, diffing the asset against itself; FLoreSourceControlRevision::Get now refuses an empty id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreRegressionDiffAgainstDepotTest, "LoreSourceControl.Regression.DiffAgainstDepotIsServerContent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreRegressionDiffAgainstDepotTest::RunTest(const FString& Parameters)
{
	FLoreTestFixture Fixture(*this, TEXT("LoreRegDiffDepot"), TEXT("rdd"));
	if (!Fixture.bReady)
	{
		return false;
	}

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	const FString File = FPaths::ConvertRelativePathToFull(Fixture.Root / TEXT("diffdepot.txt"));
	FFileHelper::SaveStringToFile(TEXT("VERSION-ONE"), *File);
	const TArray<FString> Targets = { File };

	Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), Targets, EConcurrency::Synchronous);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckIn = ISourceControlOperation::Create<FCheckIn>();
	CheckIn->SetDescription(FText::FromString(TEXT("commit version one")));
	const ECommandResult::Type CommitResult = Provider.Execute(CheckIn, Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("Committing version one succeeds"), (int32)CommitResult, (int32)ECommandResult::Succeeded);

	// Edit the working file without committing; the depot still holds version one.
	FFileHelper::SaveStringToFile(TEXT("VERSION-TWO"), *File);

	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> HistoryOp = ISourceControlOperation::Create<FUpdateStatus>();
	HistoryOp->SetUpdateHistory(true);
	Provider.Execute(HistoryOp, Targets, EConcurrency::Synchronous);

	const FSourceControlStatePtr State = Provider.GetState(File, EStateCacheUsage::Use);
	TestTrue(TEXT("State carries history"), State.IsValid() && State->GetHistorySize() >= 1);
	if (State.IsValid() && State->GetHistorySize() >= 1)
	{
		TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Depot = State->GetHistoryItem(0);
		TestTrue(TEXT("Depot revision item is valid"), Depot.IsValid());
		if (Depot.IsValid())
		{
			FString DepotTemp;
			const bool bExtracted = Depot->Get(DepotTemp);
			TestTrue(TEXT("Depot revision extracts to a file"), bExtracted);
			if (bExtracted)
			{
				FString DepotContent;
				FFileHelper::LoadFileToString(DepotContent, *DepotTemp);
				TestEqual(TEXT("Extracted depot content is the committed version one"), DepotContent, FString(TEXT("VERSION-ONE")));
				TestNotEqual(TEXT("Extracted depot content is not the edited working file"), DepotContent, FString(TEXT("VERSION-TWO")));
			}
		}
	}

	return true;
}

// Bug 4 (under investigation): extracting an older historical revision must load its content, not fail with "content unsupported". Two commits give two revisions; the older one must extract to "V1".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreRegressionHistoricalDiffTest, "LoreSourceControl.Regression.HistoricalRevisionDiffLoadable", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreRegressionHistoricalDiffTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreRegHistDiff"), TEXT("rhd"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString File = Fixture.Root / TEXT("histdiff.txt");
	FFileHelper::SaveStringToFile(TEXT("V1"), *File);
	TestTrue(TEXT("First commit (V1) lands"), StageCommitPush(Fixture, TEXT("histdiff.txt"), TEXT("commit V1")));

	FFileHelper::SaveStringToFile(TEXT("V2"), *File);
	TestTrue(TEXT("Second commit (V2) lands"), StageCommitPush(Fixture, TEXT("histdiff.txt"), TEXT("commit V2")));

	TLoreSourceControlHistory History;
	TArray<FString> Errors;
	RunGetHistory(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, File, Errors, History);
	TestTrue(TEXT("History has two revisions"), History.Num() >= 2);
	if (History.Num() >= 2)
	{
		// Index zero is newest (V2); index one is the older revision (V1).
		const FLoreSourceControlRevisionRef Older = History[1];
		FString OlderTemp;
		const bool bExtracted = Older->Get(OlderTemp);
		TestTrue(TEXT("Older historical revision extracts (bug 4: must not fail with content unsupported)"), bExtracted);
		if (bExtracted)
		{
			TestTrue(TEXT("Extracted older revision is non-empty"), IFileManager::Get().FileSize(*OlderTemp) > 0);
			FString OlderContent;
			FFileHelper::LoadFileToString(OlderContent, *OlderTemp);
			TestEqual(TEXT("Older historical revision content is V1"), OlderContent, FString(TEXT("V1")));
		}
	}

	return true;
}

// Breadth: Status ---------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreStatusMovedTest, "LoreSourceControl.Status.Moved", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreStatusMovedTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreStatusMoved"), TEXT("smv"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString Source = Fixture.Root / TEXT("source.txt");
	FFileHelper::SaveStringToFile(TEXT("moved content"), *Source);
	TestTrue(TEXT("Initial commit lands"), StageCommitPush(Fixture, TEXT("source.txt"), TEXT("add source")));

	// lore stage move records a move already performed on disk, so rename the file before staging the move.
	const FString Dest = Fixture.Root / TEXT("dest.txt");
	TestTrue(TEXT("On-disk rename"), IFileManager::Get().Move(*Dest, *Source));

	// Stage the move; Lore records the destination as Moved with the source in fromPath.
	TArray<FString> Results, Errors;
	const bool bMoved = RunCommand(TEXT("stage move"), Fixture.Binary, Fixture.Root, TArray<FString>(), { TEXT("source.txt"), TEXT("dest.txt") }, Results, Errors);
	TestTrue(TEXT("Stage move succeeds"), bMoved);

	TArray<FLoreSourceControlState> States;
	TArray<FString> StatusErrors;
	RunUpdateStatus(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), false, { Dest }, StatusErrors, States);
	TestEqual(TEXT("One state for the move destination"), States.Num(), 1);
	if (States.Num() == 1)
	{
		TestEqual(TEXT("Move destination reads as Moved"), (int32)States[0].WorkingCopyState, (int32)ELoreWorkingCopyState::Moved);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreStatusCopiedTest, "LoreSourceControl.Status.Copied", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreStatusCopiedTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreStatusCopied"), TEXT("scp"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString Source = Fixture.Root / TEXT("original.txt");
	FFileHelper::SaveStringToFile(TEXT("copied content"), *Source);
	TestTrue(TEXT("Initial commit lands"), StageCommitPush(Fixture, TEXT("original.txt"), TEXT("add original")));

	// A copy on disk with identical content stages by hash as a Copied entry.
	const FString Copy = Fixture.Root / TEXT("duplicate.txt");
	IFileManager::Get().Copy(*Copy, *Source);
	TArray<FString> Results, Errors;
	RunCommand(TEXT("stage"), Fixture.Binary, Fixture.Root, { TEXT("--scan") }, { TEXT("duplicate.txt") }, Results, Errors);

	TArray<FLoreSourceControlState> States;
	TArray<FString> StatusErrors;
	RunUpdateStatus(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), false, { Copy }, StatusErrors, States);
	TestEqual(TEXT("One state for the copy"), States.Num(), 1);
	if (States.Num() == 1)
	{
		// Lore may classify a content-identical new file as Copied or as Added; accept either, but it must be a staged-for-inclusion state, not Unknown or NotControlled.
		const ELoreWorkingCopyState::Type Got = States[0].WorkingCopyState;
		const bool bStagedNew = Got == ELoreWorkingCopyState::Copied || Got == ELoreWorkingCopyState::Added;
		TestTrue(TEXT("Content-identical staged file reads as Copied or Added"), bStagedNew);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreStatusIgnoredTest, "LoreSourceControl.Status.Ignored", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreStatusIgnoredTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreStatusIgnored"), TEXT("sig"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// The status parser filters transient editor paths (Saved, Intermediate) so they never offer for add. A file under Saved must not come back as a controllable state.
	const FString SavedDir = Fixture.Root / TEXT("Saved") / TEXT("Autosaves");
	IFileManager::Get().MakeDirectory(*SavedDir, true);
	const FString Transient = SavedDir / TEXT("transient.txt");
	FFileHelper::SaveStringToFile(TEXT("transient"), *Transient);

	TArray<FLoreSourceControlState> States;
	TArray<FString> StatusErrors;
	RunUpdateStatus(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), false, { Transient }, StatusErrors, States);
	if (States.Num() == 1)
	{
		// The status parser drops transient editor paths, so a file under Saved never reads as NotControlled and the submit dialog never offers it for add.
		TestNotEqual(TEXT("Transient Saved path is not offered for add"), (int32)States[0].WorkingCopyState, (int32)ELoreWorkingCopyState::NotControlled);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreStatusRemoteAheadTest, "LoreSourceControl.Status.RemoteAhead", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreStatusRemoteAheadTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreStatusRemoteAhead"), TEXT("sra"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString File = Fixture.Root / TEXT("shared.txt");
	FFileHelper::SaveStringToFile(TEXT("first revision"), *File);
	TestTrue(TEXT("First commit lands"), StageCommitPush(Fixture, TEXT("shared.txt"), TEXT("first")));

	// A second clone pushes a newer revision; this clone is then behind the remote.
	const FString Clone = Fixture.CloneSecondTree(*this, TEXT("aheadclone"));
	if (Clone.IsEmpty())
	{
		return false;
	}
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Clone), false, true); };

	const FString CloneFile = Clone / TEXT("shared.txt");
	FFileHelper::SaveStringToFile(TEXT("second revision from the clone"), *CloneFile);
	TArray<FString> CloneErrors;
	TestTrue(TEXT("Clone stages its edit"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Clone, { TEXT("shared.txt") }, CloneErrors));
	TestTrue(TEXT("Clone commits its edit"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Clone, TEXT("test@example.com"), TEXT("second from clone"), CloneErrors));
	TestTrue(TEXT("Clone pushes its edit"), PushBranch(ELoreBackendMode::Automatic, Fixture.Binary, Clone, CloneErrors));

	// The first clone now reports remote-ahead, so its file reads as not current.
	TArray<FLoreSourceControlState> States;
	TArray<FString> StatusErrors;
	RunUpdateStatus(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), false, { File }, StatusErrors, States);
	TestEqual(TEXT("One state for the behind file"), States.Num(), 1);
	if (States.Num() == 1)
	{
		TestFalse(TEXT("File behind the remote reads as not current"), States[0].IsCurrent());
	}

	return true;
}

// Breadth: Revert ---------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreRevertModifiedWithLockTest, "LoreSourceControl.Revert.ModifiedFileWithLock", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreRevertModifiedWithLockTest::RunTest(const FString& Parameters)
{
	FLoreTestFixture Fixture(*this, TEXT("LoreRevertLock"), TEXT("rvl"));
	if (!Fixture.bReady)
	{
		return false;
	}

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	const bool bWasLocking = FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();
	FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(true);
	ON_SCOPE_EXIT { FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(bWasLocking); };

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	const FString Asset = FPaths::ConvertRelativePathToFull(Fixture.Root / TEXT("revertlock.uasset"));
	FFileHelper::SaveStringToFile(TEXT("revert version one"), *Asset);
	const TArray<FString> Targets = { Asset };

	Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), Targets, EConcurrency::Synchronous);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> AddCheckIn = ISourceControlOperation::Create<FCheckIn>();
	AddCheckIn->SetDescription(FText::FromString(TEXT("add revert-lock asset")));
	Provider.Execute(AddCheckIn, Targets, EConcurrency::Synchronous);

	Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), Targets, EConcurrency::Synchronous);
	FFileHelper::SaveStringToFile(TEXT("revert version two"), *Asset);
	const FSourceControlStatePtr Edited = Provider.GetState(Asset, EStateCacheUsage::Use);
	TestTrue(TEXT("Edited locked asset reads as checked out"), Edited.IsValid() && Edited->IsCheckedOut());
	TestFalse(TEXT("Checked-out asset is writable"), PlatformFile.IsReadOnly(*Asset));

	const ECommandResult::Type RevertResult = Provider.Execute(ISourceControlOperation::Create<FRevert>(), Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("Revert succeeds"), (int32)RevertResult, (int32)ECommandResult::Succeeded);
	const FSourceControlStatePtr Reverted = Provider.GetState(Asset, EStateCacheUsage::Use);
	TestFalse(TEXT("Reverted asset is no longer checked out"), Reverted.IsValid() && Reverted->IsCheckedOut());
	TestTrue(TEXT("Reverted lockable asset is read-only on disk again"), PlatformFile.IsReadOnly(*Asset));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreRevertModifiedFileTest, "LoreSourceControl.Revert.ModifiedFile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreRevertModifiedFileTest::RunTest(const FString& Parameters)
{
	FLoreTestFixture Fixture(*this, TEXT("LoreRevertModified"), TEXT("rvm"));
	if (!Fixture.bReady)
	{
		return false;
	}

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	const FString File = FPaths::ConvertRelativePathToFull(Fixture.Root / TEXT("revertcontent.txt"));
	FFileHelper::SaveStringToFile(TEXT("committed content"), *File);
	const TArray<FString> Targets = { File };

	Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), Targets, EConcurrency::Synchronous);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> AddCheckIn = ISourceControlOperation::Create<FCheckIn>();
	AddCheckIn->SetDescription(FText::FromString(TEXT("commit content")));
	Provider.Execute(AddCheckIn, Targets, EConcurrency::Synchronous);

	// Edit on disk, then revert; the committed content must come back.
	FFileHelper::SaveStringToFile(TEXT("dirty content"), *File);
	const ECommandResult::Type RevertResult = Provider.Execute(ISourceControlOperation::Create<FRevert>(), Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("Revert succeeds"), (int32)RevertResult, (int32)ECommandResult::Succeeded);

	FString Content;
	FFileHelper::LoadFileToString(Content, *File);
	TestEqual(TEXT("Reverted file holds the committed content"), Content, FString(TEXT("committed content")));

	return true;
}

// Breadth: CheckIn --------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreCheckInPushOffTest, "LoreSourceControl.CheckIn.PushOff", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreCheckInPushOffTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreCheckInPushOff"), TEXT("cpo"));
	if (!Fixture.bReady)
	{
		return false;
	}

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	const bool bWasPushing = FLoreSourceControlModule::Get().GetSettings().IsPushAfterCommit();
	FLoreSourceControlModule::Get().AccessSettings().SetPushAfterCommit(false);
	ON_SCOPE_EXIT { FLoreSourceControlModule::Get().AccessSettings().SetPushAfterCommit(bWasPushing); };

	const FString File = FPaths::ConvertRelativePathToFull(Fixture.Root / TEXT("pushoff.txt"));
	FFileHelper::SaveStringToFile(TEXT("push off content"), *File);
	const TArray<FString> Targets = { File };

	Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), Targets, EConcurrency::Synchronous);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckIn = ISourceControlOperation::Create<FCheckIn>();
	CheckIn->SetDescription(FText::FromString(TEXT("local commit, no push")));
	const ECommandResult::Type CommitResult = Provider.Execute(CheckIn, Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("Commit with push off succeeds"), (int32)CommitResult, (int32)ECommandResult::Succeeded);

	// The commit is local: the working file reads as unchanged (committed), and a clean clone made now must not see it yet.
	const FSourceControlStatePtr Committed = Provider.GetState(File, EStateCacheUsage::Use);
	TestTrue(TEXT("Locally committed file is no longer added"), Committed.IsValid() && !Committed->IsAdded());

	const FString Clone = Fixture.CloneSecondTree(*this, TEXT("pushoffclone"));
	if (!Clone.IsEmpty())
	{
		ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Clone), false, true); };
		const FString ClonedFile = Clone / TEXT("pushoff.txt");
		TestFalse(TEXT("Unpushed commit is absent from a fresh clone"), IFileManager::Get().FileExists(*ClonedFile));

		// A later explicit push delivers the local commit to the server.
		TArray<FString> PushErrors;
		const bool bPushed = PushBranch(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, PushErrors);
		TestTrue(TEXT("A later push of the local commit succeeds"), bPushed);

		// A second clone made after the push contains the file.
		const FString Clone2 = Fixture.CloneSecondTree(*this, TEXT("pushoffclone2"));
		if (!Clone2.IsEmpty())
		{
			ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Clone2), false, true); };
			const FString ClonedFile2 = Clone2 / TEXT("pushoff.txt");
			TestTrue(TEXT("After the push, a fresh clone has the file"), IFileManager::Get().FileExists(*ClonedFile2));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreCheckInBehindRemoteTest, "LoreSourceControl.CheckIn.BehindRemoteRejectsPush", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreCheckInBehindRemoteTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreCheckInBehind"), TEXT("cbr"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString File = Fixture.Root / TEXT("contended.txt");
	FFileHelper::SaveStringToFile(TEXT("base revision"), *File);
	TestTrue(TEXT("Base commit lands"), StageCommitPush(Fixture, TEXT("contended.txt"), TEXT("base")));

	// A second clone pushes first, so this clone's branch tip is behind the remote.
	const FString Clone = Fixture.CloneSecondTree(*this, TEXT("behindclone"));
	if (Clone.IsEmpty())
	{
		return false;
	}
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Clone), false, true); };

	const FString CloneFile = Clone / TEXT("contended.txt");
	FFileHelper::SaveStringToFile(TEXT("clone wins the race"), *CloneFile);
	TArray<FString> CloneErrors;
	TestTrue(TEXT("Clone stages"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Clone, { TEXT("contended.txt") }, CloneErrors));
	TestTrue(TEXT("Clone commits"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Clone, TEXT("test@example.com"), TEXT("clone first"), CloneErrors));
	TestTrue(TEXT("Clone pushes first"), PushBranch(ELoreBackendMode::Automatic, Fixture.Binary, Clone, CloneErrors));

	// This clone commits locally, then its push is rejected because the remote moved.
	FFileHelper::SaveStringToFile(TEXT("our late edit"), *File);
	TArray<FString> Errors;
	TestTrue(TEXT("Local stage succeeds"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, { TEXT("contended.txt") }, Errors));
	TestTrue(TEXT("Local commit succeeds"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), TEXT("our late edit"), Errors));

	TArray<FString> PushErrors;
	const bool bPushed = PushBranch(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, PushErrors);
	TestFalse(TEXT("Pushing from behind the remote is rejected"), bPushed);

	// The commit stays local: it is not lost, so a sync-and-resync resolves it. Confirm the local revision is still present by reading branch status.
	FString Branch;
	TestTrue(TEXT("Branch name still reads after the rejected push"), GetBranchName(ELoreBackendMode::CommandLine, Fixture.Binary, Fixture.Root, Branch));

	return true;
}

#if LORE_UE5_1_OR_LATER
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreCheckInKeepCheckedOutTest, "LoreSourceControl.CheckIn.KeepCheckedOutOn", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreCheckInKeepCheckedOutTest::RunTest(const FString& Parameters)
{
	FLoreTestFixture Fixture(*this, TEXT("LoreKeepCheckedOut"), TEXT("kco"));
	if (!Fixture.bReady)
	{
		return false;
	}

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	const bool bWasLocking = FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();
	FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(true);
	ON_SCOPE_EXIT { FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(bWasLocking); };

	const FString Asset = FPaths::ConvertRelativePathToFull(Fixture.Root / TEXT("keepout.uasset"));
	FFileHelper::SaveStringToFile(TEXT("keep version one"), *Asset);
	const TArray<FString> Targets = { Asset };

	Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), Targets, EConcurrency::Synchronous);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> AddCheckIn = ISourceControlOperation::Create<FCheckIn>();
	AddCheckIn->SetDescription(FText::FromString(TEXT("add keepout asset")));
	Provider.Execute(AddCheckIn, Targets, EConcurrency::Synchronous);

	Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), Targets, EConcurrency::Synchronous);
	FFileHelper::SaveStringToFile(TEXT("keep version two"), *Asset);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> KeepCheckIn = ISourceControlOperation::Create<FCheckIn>();
	KeepCheckIn->SetDescription(FText::FromString(TEXT("keep checked out submit")));
	KeepCheckIn->SetKeepCheckedOut(true);
	const ECommandResult::Type KeepResult = Provider.Execute(KeepCheckIn, Targets, EConcurrency::Synchronous);
	TestEqual(TEXT("Keep-checked-out submit succeeds"), (int32)KeepResult, (int32)ECommandResult::Succeeded);

	const FSourceControlStatePtr Kept = Provider.GetState(Asset, EStateCacheUsage::Use);
	TestTrue(TEXT("Asset stays checked out after a keep-checked-out submit"), Kept.IsValid() && Kept->IsCheckedOut());

	return true;
}
#endif

// Breadth: Sync -----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSyncBringsStaleCurrentTest, "LoreSourceControl.Sync.BringsStaleCurrent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSyncBringsStaleCurrentTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreSyncStale"), TEXT("sys"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString File = Fixture.Root / TEXT("syncme.txt");
	FFileHelper::SaveStringToFile(TEXT("first revision"), *File);
	TestTrue(TEXT("First commit lands"), StageCommitPush(Fixture, TEXT("syncme.txt"), TEXT("first")));

	const FString Clone = Fixture.CloneSecondTree(*this, TEXT("syncclone"));
	if (Clone.IsEmpty())
	{
		return false;
	}
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Clone), false, true); };

	const FString CloneFile = Clone / TEXT("syncme.txt");
	FFileHelper::SaveStringToFile(TEXT("second revision"), *CloneFile);
	TArray<FString> CloneErrors;
	TestTrue(TEXT("Clone stages"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Clone, { TEXT("syncme.txt") }, CloneErrors));
	TestTrue(TEXT("Clone commits"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Clone, TEXT("test@example.com"), TEXT("second"), CloneErrors));
	TestTrue(TEXT("Clone pushes"), PushBranch(ELoreBackendMode::Automatic, Fixture.Binary, Clone, CloneErrors));

	// The first clone is now behind; after a sync it is level with the server and reads as current.
	auto CurrentState = [&]() -> FLoreSourceControlState
	{
		TArray<FLoreSourceControlState> States;
		TArray<FString> StatusErrors;
		RunUpdateStatus(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), false, { File }, StatusErrors, States);
		return States.Num() == 1 ? States[0] : FLoreSourceControlState(File);
	};
	TestFalse(TEXT("Before sync the file is not current"), CurrentState().IsCurrent());

	TArray<FString> SyncErrors;
	const bool bSynced = SyncWorkingTree(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, SyncErrors);
	TestTrue(TEXT("Sync succeeds"), bSynced);
	TestTrue(TEXT("After sync the file reads as current"), CurrentState().IsCurrent());

	FString Synced;
	FFileHelper::LoadFileToString(Synced, *File);
	TestEqual(TEXT("After sync the working file holds the clone's revision"), Synced, FString(TEXT("second revision")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreStatusAnchorsReadTest, "LoreSourceControl.Sync.StatusRevisionAnchors", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreStatusAnchorsReadTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreAnchors"), TEXT("anc"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString File = Fixture.Root / TEXT("anchor.txt");
	FFileHelper::SaveStringToFile(TEXT("first"), *File);
	TestTrue(TEXT("Commit lands"), StageCommitPush(Fixture, TEXT("anchor.txt"), TEXT("first")));

	// Both backends read the same branch, current revision, and remote latest off the status revision header.
	for (const ELoreBackendMode Mode : { ELoreBackendMode::CommandLine, ELoreBackendMode::Native })
	{
		FLoreStatusAnchors Anchors;
		TArray<FString> Errors;
		const bool bRead = GetStatusRevisionAnchors(Mode, Fixture.Binary, Fixture.Root, Anchors, Errors);
		TestTrue(FString::Printf(TEXT("Anchors read (mode %d)"), (int32)Mode), bRead);
		TestEqual(FString::Printf(TEXT("Branch is main (mode %d)"), (int32)Mode), Anchors.BranchName, FString(TEXT("main")));
		TestTrue(FString::Printf(TEXT("Current revision is a non-empty hash (mode %d)"), (int32)Mode), !Anchors.CurrentRevision.IsEmpty());
		TestTrue(FString::Printf(TEXT("Current revision number advanced past zero (mode %d)"), (int32)Mode), Anchors.CurrentRevisionNumber > 0);
		// The tree pushed its revision, so the remote latest matches the current revision.
		TestEqual(FString::Printf(TEXT("Remote latest equals the pushed revision (mode %d)"), (int32)Mode), Anchors.RemoteRevision, Anchors.CurrentRevision);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSyncDeltaEnumerateTest, "LoreSourceControl.Sync.DeltaEnumeration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSyncDeltaEnumerateTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreSyncDelta"), TEXT("sd"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString File = Fixture.Root / TEXT("delta.txt");
	FFileHelper::SaveStringToFile(TEXT("first revision content"), *File);
	TestTrue(TEXT("First commit lands"), StageCommitPush(Fixture, TEXT("delta.txt"), TEXT("first")));

	// A second clone commits a change and pushes it, so the first tree is now one revision behind.
	const FString Clone = Fixture.CloneSecondTree(*this, TEXT("deltaclone"));
	if (Clone.IsEmpty())
	{
		return false;
	}
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Clone), false, true); };

	const FString CloneFile = Clone / TEXT("delta.txt");
	FFileHelper::SaveStringToFile(TEXT("second revision content"), *CloneFile);
	TArray<FString> CloneErrors;
	TestTrue(TEXT("Clone stages"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Clone, { TEXT("delta.txt") }, CloneErrors));
	TestTrue(TEXT("Clone commits"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Clone, TEXT("test@example.com"), TEXT("second"), CloneErrors));
	TestTrue(TEXT("Clone pushes"), PushBranch(ELoreBackendMode::Automatic, Fixture.Binary, Clone, CloneErrors));

	// The proving test: the dry-run names exactly the changed file with the keep action, and changes nothing. A modified file rewrites in place, so its action is keep.
	for (const ELoreBackendMode Mode : { ELoreBackendMode::CommandLine, ELoreBackendMode::Native })
	{
		TArray<FLoreDeltaEntry> Delta;
		TArray<FString> Errors;
		const bool bEnumerated = EnumerateSyncDelta(Mode, Fixture.Binary, Fixture.Root, Delta, Errors);
		TestTrue(FString::Printf(TEXT("Delta enumerated (mode %d)"), (int32)Mode), bEnumerated);

		const FLoreDeltaEntry* Entry = Delta.FindByPredicate([](const FLoreDeltaEntry& E) { return E.Path.EndsWith(TEXT("delta.txt")); });
		TestNotNull(FString::Printf(TEXT("The changed file is in the delta (mode %d)"), (int32)Mode), Entry);
		if (Entry)
		{
			TestEqual(FString::Printf(TEXT("A modified file reads keep (mode %d)"), (int32)Mode), Entry->Action, FString(TEXT("keep")));
		}

		// Dry run touches nothing: the working file still holds the pre-sync content and the anchor has not moved.
		FString OnDisk;
		FFileHelper::LoadFileToString(OnDisk, *File);
		TestEqual(FString::Printf(TEXT("The working file is unchanged by the dry run (mode %d)"), (int32)Mode), OnDisk, FString(TEXT("first revision content")));

		FLoreStatusAnchors Anchors;
		TArray<FString> AnchorErrors;
		GetStatusRevisionAnchors(Mode, Fixture.Binary, Fixture.Root, Anchors, AnchorErrors);
		TestTrue(FString::Printf(TEXT("The branch is still behind the remote after the dry run (mode %d)"), (int32)Mode), Anchors.RemoteRevision != Anchors.CurrentRevision);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSwitchDeltaEnumerateTest, "LoreSourceControl.Switch.DeltaEnumeration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSwitchDeltaEnumerateTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreSwitchDelta"), TEXT("swd"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// Seed main with a file, then branch off. Create switches to the new branch in the same step, so a divergent commit here lands on feature.
	const FString File = Fixture.Root / TEXT("shared.txt");
	FFileHelper::SaveStringToFile(TEXT("main content"), *File);
	TestTrue(TEXT("Seed commit on main lands"), StageCommitPush(Fixture, TEXT("shared.txt"), TEXT("main-seed")));

	TArray<FString> Errors;
	TestTrue(TEXT("Branch create succeeds and switches"), RunBranchCreate(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("feature"), FString(), Errors));

	// A divergent commit on feature, so main and feature differ by this file's content.
	FFileHelper::SaveStringToFile(TEXT("feature content, longer than main"), *File);
	TestTrue(TEXT("Feature commit lands"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, { TEXT("shared.txt") }, Errors));
	TestTrue(TEXT("Feature commits"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), TEXT("feature-change"), Errors));

	// The proving test: a dry-run switch from feature back to main names exactly the changed file with the keep action and changes nothing on disk.
	for (const ELoreBackendMode Mode : { ELoreBackendMode::CommandLine, ELoreBackendMode::Native })
	{
		TArray<FLoreDeltaEntry> Delta;
		TArray<FString> EnumErrors;
		const bool bEnumerated = EnumerateSwitchDelta(Mode, Fixture.Binary, Fixture.Root, TEXT("main"), FString(), Delta, EnumErrors);
		TestTrue(FString::Printf(TEXT("Switch delta enumerated (mode %d, err: %s)"), (int32)Mode, *FString::Join(EnumErrors, TEXT("; "))), bEnumerated);

		const FLoreDeltaEntry* Entry = Delta.FindByPredicate([](const FLoreDeltaEntry& E) { return E.Path.EndsWith(TEXT("shared.txt")); });
		TestNotNull(FString::Printf(TEXT("The changed file is in the switch delta (mode %d)"), (int32)Mode), Entry);
		if (Entry)
		{
			// A file present in both revisions with different content rewrites in place, so its action is keep.
			TestEqual(FString::Printf(TEXT("A rewritten file reads keep (mode %d)"), (int32)Mode), Entry->Action, FString(TEXT("keep")));
		}

		// Dry run touches nothing: the working file still holds the feature content, and the tree is still on feature.
		FString OnDisk;
		FFileHelper::LoadFileToString(OnDisk, *File);
		TestEqual(FString::Printf(TEXT("The working file is unchanged by the dry-run switch (mode %d)"), (int32)Mode), OnDisk, FString(TEXT("feature content, longer than main")));

		FString CurrentBranch;
		GetBranchName(Mode, Fixture.Binary, Fixture.Root, CurrentBranch);
		TestEqual(FString::Printf(TEXT("The tree is still on feature after the dry run (mode %d)"), (int32)Mode), CurrentBranch, FString(TEXT("feature")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreGuardedSwitchRoundTripTest, "LoreSourceControl.Switch.GuardedRoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreGuardedSwitchRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreSwitchRT"), TEXT("swr"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// Seed main with a binary asset, then create feature (which switches to it in the same step) and diverge it.
	const FString Asset = Fixture.Root / TEXT("Content/Switch.uasset");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Asset), true);
	FFileHelper::SaveStringToFile(TEXT("main asset content"), *Asset);
	TestTrue(TEXT("Seed commit on main lands"), StageCommitPush(Fixture, TEXT("Content/Switch.uasset"), TEXT("main-seed")));

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };
	const bool bWasLocking = FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();
	FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(true);
	ON_SCOPE_EXIT { FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(bWasLocking); };

	// Create the branch through the provider. Create switches the tree to it and rewrites no files.
	TSharedRef<FLoreCreateBranch, ESPMode::ThreadSafe> Create = ISourceControlOperation::Create<FLoreCreateBranch>();
	Create->BranchName = TEXT("feature");
	TestEqual(TEXT("Create branch succeeds through the provider"), (int32)Provider.Execute(Create, TArray<FString>(), EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);

	FString BranchAfterCreate;
	GetBranchName(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, BranchAfterCreate);
	TestEqual(TEXT("The tree is on feature after create"), BranchAfterCreate, FString(TEXT("feature")));

	// Diverge feature: rewrite the asset and commit, so main and feature differ by this file. Push it so it exists on the remote, which archive needs (archive on a remote-absent branch fails "Not found").
	FFileHelper::SaveStringToFile(TEXT("feature asset content, changed"), *Asset);
	TArray<FString> Errors;
	TestTrue(TEXT("Feature stages"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, { TEXT("Content/Switch.uasset") }, Errors));
	TestTrue(TEXT("Feature commits"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), TEXT("feature-change"), Errors));
	TestTrue(TEXT("Feature pushes"), PushBranch(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, Errors));

	// The asset is read-only under the locking policy, the exact file the unguarded switch aborts on.
	SetFilesReadOnly({ Asset }, true);
	TestTrue(TEXT("The asset is read-only before the guarded switch"), IFileManager::Get().IsReadOnly(*Asset));

	// Switch back to main through the provider. The guard clears read-only on the delta, switches, verifies, and reapplies read-only.
	TSharedRef<FLoreSwitchBranch, ESPMode::ThreadSafe> Switch = ISourceControlOperation::Create<FLoreSwitchBranch>();
	Switch->BranchName = TEXT("main");
	TestEqual(TEXT("Switch to main succeeds through the provider"), (int32)Provider.Execute(Switch, TArray<FString>(), EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);

	FString BranchAfterSwitch;
	GetBranchName(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, BranchAfterSwitch);
	TestEqual(TEXT("The tree lands on main after the switch"), BranchAfterSwitch, FString(TEXT("main")));

	FString OnDisk;
	FFileHelper::LoadFileToString(OnDisk, *Asset);
	TestEqual(TEXT("The asset carries main's content after the switch"), OnDisk, FString(TEXT("main asset content")));
	TestTrue(TEXT("Read-only is reapplied per the locking policy after the switch"), IFileManager::Get().IsReadOnly(*Asset));
	TestFalse(TEXT("No marker remains after a verified switch"), LoreOperationMarker::Read(Fixture.Root).IsValid());
	TestTrue(TEXT("The switch reported the rewritten file"), Switch->UpdatedFiles.ContainsByPredicate([](const FString& P) { return P.EndsWith(TEXT("Switch.uasset")); }));

	// Protect main through the provider, then read the flag back through BranchMetadataGet.
	TSharedRef<FLoreProtectBranch, ESPMode::ThreadSafe> Protect = ISourceControlOperation::Create<FLoreProtectBranch>();
	Protect->BranchName = TEXT("main");
	TestEqual(TEXT("Protect main succeeds through the provider"), (int32)Provider.Execute(Protect, TArray<FString>(), EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);

	FString ProtectValue;
	bool bProtectFound = false;
	TArray<FString> ProtectReadErrors;
	TestTrue(TEXT("Protect flag reads back"), RunBranchMetadataGet(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("main"), TEXT("protect"), ProtectValue, bProtectFound, ProtectReadErrors));
	TestTrue(TEXT("Protect flag reads true after protect"), bProtectFound && ProtectValue == TEXT("true"));

	// Unprotect through the provider, then confirm the flag no longer reads true.
	TSharedRef<FLoreUnprotectBranch, ESPMode::ThreadSafe> Unprotect = ISourceControlOperation::Create<FLoreUnprotectBranch>();
	Unprotect->BranchName = TEXT("main");
	TestEqual(TEXT("Unprotect main succeeds through the provider"), (int32)Provider.Execute(Unprotect, TArray<FString>(), EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);
	ProtectValue.Empty(); bProtectFound = false;
	RunBranchMetadataGet(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("main"), TEXT("protect"), ProtectValue, bProtectFound, ProtectReadErrors);
	TestFalse(TEXT("Protect flag does not read true after unprotect"), ProtectValue == TEXT("true"));

	// Archive feature through the provider; the branch then drops out of the default listing.
	TSharedRef<FLoreArchiveBranch, ESPMode::ThreadSafe> Archive = ISourceControlOperation::Create<FLoreArchiveBranch>();
	Archive->BranchName = TEXT("feature");
	TestEqual(TEXT("Archive feature succeeds through the provider"), (int32)Provider.Execute(Archive, TArray<FString>(), EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);

	TArray<FLoreBranchItem> Branches;
	TArray<FString> ListErrors;
	TestTrue(TEXT("Branch listing succeeds after archive"), RunGetBranches(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, false, Branches, ListErrors));
	TestFalse(TEXT("The archived branch is absent from the default listing"), Branches.ContainsByPredicate([](const FLoreBranchItem& B) { return B.Name == TEXT("feature"); }));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreGuardedRewriteSuccessTest, "LoreSourceControl.Guard.SuccessRestoresReadOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreGuardedRewriteSuccessTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreGuardOk"), TEXT("gk"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// A committed binary asset the first tree does not lock is read-only under the locking policy, which is exactly the file Lore's rewrite aborts on. Land one, then diverge it from a second clone.
	const FString Asset = Fixture.Root / TEXT("Content/Guarded.uasset");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Asset), true);
	FFileHelper::SaveStringToFile(TEXT("first binary revision"), *Asset);
	TestTrue(TEXT("First commit lands"), StageCommitPush(Fixture, TEXT("Content/Guarded.uasset"), TEXT("first")));

	const FString Clone = Fixture.CloneSecondTree(*this, TEXT("guardclone"));
	if (Clone.IsEmpty())
	{
		return false;
	}
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Clone), false, true); };

	const FString CloneAsset = Clone / TEXT("Content/Guarded.uasset");
	FFileHelper::SaveStringToFile(TEXT("second binary revision, longer"), *CloneAsset);
	TArray<FString> CloneErrors;
	TestTrue(TEXT("Clone stages"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Clone, { TEXT("Content/Guarded.uasset") }, CloneErrors));
	TestTrue(TEXT("Clone commits"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Clone, TEXT("test@example.com"), TEXT("second"), CloneErrors));
	TestTrue(TEXT("Clone pushes"), PushBranch(ELoreBackendMode::Automatic, Fixture.Binary, Clone, CloneErrors));

	// The file is read-only, standing in for a not-checked-out binary asset. An unguarded sync would abort here.
	SetFilesReadOnly({ Asset }, true);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TestTrue(TEXT("The asset is read-only before the guarded sync"), PlatformFile.FileExists(*Asset) && IFileManager::Get().IsReadOnly(*Asset));

	// Point the provider at the fixture and turn locking on, so a command constructed here carries the right root and policy.
	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };
	const bool bWasLocking = FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();
	FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(true);
	ON_SCOPE_EXIT { FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(bWasLocking); };

	// Enumerate the delta, then run a real sync as the rewrite callback through the guard.
	TArray<FLoreDeltaEntry> Delta;
	TArray<FString> EnumErrors;
	TestTrue(TEXT("Delta enumerated"), EnumerateSyncDelta(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, Delta, EnumErrors));
	TestTrue(TEXT("The read-only asset is in the delta"), Delta.ContainsByPredicate([](const FLoreDeltaEntry& E) { return E.Path.EndsWith(TEXT("Guarded.uasset")); }));

	FLoreSourceControlCommand Command(ISourceControlOperation::Create<FSync>(), MakeShared<FLoreSyncWorker, ESPMode::ThreadSafe>());
	Command.PathToRepositoryRoot = Fixture.Root;
	Command.PathToLoreBinary = Fixture.Binary;
	Command.BackendMode = ELoreBackendMode::Automatic;
	Command.Identity = TEXT("test@example.com");
	Command.bUsingLocking = true;
	Command.bRemoteAvailable = true;

	LoreGuardedRewrite::FRewriteRequest Request;
	Request.Kind = ELoreOperationKind::RevisionSync;
	Request.SourceBranch = TEXT("main");
	Request.Delta = Delta;

	TArray<FLoreSourceControlState> States;
	const bool bGuarded = LoreGuardedRewrite::Run(Request, Command, [&](TArray<FString>& OutErrors)
	{
		return SyncWorkingTree(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, OutErrors);
	}, States);

	TestTrue(TEXT("The guarded sync succeeds through a read-only file"), bGuarded);

	FString OnDisk;
	FFileHelper::LoadFileToString(OnDisk, *Asset);
	TestEqual(TEXT("The asset carries the synced content"), OnDisk, FString(TEXT("second binary revision, longer")));
	TestTrue(TEXT("Read-only is reapplied per the locking policy after the sync"), IFileManager::Get().IsReadOnly(*Asset));
	TestFalse(TEXT("No marker remains after a verified success"), LoreOperationMarker::Read(Fixture.Root).IsValid());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreGuardedRewriteFailureTest, "LoreSourceControl.Guard.FailureKeepsMarker", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreGuardedRewriteFailureTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreGuardFail"), TEXT("gf"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString File = Fixture.Root / TEXT("kept.txt");
	FFileHelper::SaveStringToFile(TEXT("content"), *File);
	TestTrue(TEXT("Commit lands"), StageCommitPush(Fixture, TEXT("kept.txt"), TEXT("first")));

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	FLoreSourceControlCommand Command(ISourceControlOperation::Create<FSync>(), MakeShared<FLoreSyncWorker, ESPMode::ThreadSafe>());
	Command.PathToRepositoryRoot = Fixture.Root;
	Command.PathToLoreBinary = Fixture.Binary;
	Command.BackendMode = ELoreBackendMode::Automatic;
	Command.Identity = TEXT("test@example.com");
	Command.bUsingLocking = false;
	Command.bRemoteAvailable = true;

	LoreGuardedRewrite::FRewriteRequest Request;
	Request.Kind = ELoreOperationKind::RevisionSync;
	Request.SourceBranch = TEXT("main");
	Request.Delta = { FLoreDeltaEntry{ TEXT("kept.txt"), TEXT("keep") } };

	// A callback that returns false stands for a killed subprocess or an aborted realization.
	TArray<FLoreSourceControlState> States;
	const bool bGuarded = LoreGuardedRewrite::Run(Request, Command, [](TArray<FString>& OutErrors)
	{
		OutErrors.Add(TEXT("simulated rewrite failure"));
		return false;
	}, States);

	TestFalse(TEXT("A failed rewrite returns false"), bGuarded);
	TestTrue(TEXT("The states were refreshed so the cache shows the tear"), States.Num() > 0);
	const FLoreOperationMarker Marker = LoreOperationMarker::Read(Fixture.Root);
	TestTrue(TEXT("The marker stays for recovery after a failed rewrite"), Marker.IsValid());
	TestEqual(TEXT("The marker records the sync kind"), (int32)Marker.Kind, (int32)ELoreOperationKind::RevisionSync);
	LoreOperationMarker::Clear(Fixture.Root);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreSyncProviderGuardedTest, "LoreSourceControl.Sync.ProviderGuardedThroughReadOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreSyncProviderGuardedTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreSyncProv"), TEXT("sp"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString Asset = Fixture.Root / TEXT("Content/ProvSync.uasset");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Asset), true);
	FFileHelper::SaveStringToFile(TEXT("first"), *Asset);
	TestTrue(TEXT("First commit lands"), StageCommitPush(Fixture, TEXT("Content/ProvSync.uasset"), TEXT("first")));

	const FString Clone = Fixture.CloneSecondTree(*this, TEXT("provclone"));
	if (Clone.IsEmpty())
	{
		return false;
	}
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Clone), false, true); };

	const FString CloneAsset = Clone / TEXT("Content/ProvSync.uasset");
	FFileHelper::SaveStringToFile(TEXT("second, from the clone"), *CloneAsset);
	TArray<FString> CloneErrors;
	TestTrue(TEXT("Clone stages"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Clone, { TEXT("Content/ProvSync.uasset") }, CloneErrors));
	TestTrue(TEXT("Clone commits"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Clone, TEXT("test@example.com"), TEXT("second"), CloneErrors));
	TestTrue(TEXT("Clone pushes"), PushBranch(ELoreBackendMode::Automatic, Fixture.Binary, Clone, CloneErrors));

	// The not-checked-out binary asset is read-only, the exact file the unguarded sync aborted on.
	SetFilesReadOnly({ Asset }, true);

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };
	const bool bWasLocking = FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();
	FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(true);
	ON_SCOPE_EXIT { FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(bWasLocking); };

	const FString Absolute = FPaths::ConvertRelativePathToFull(Asset);
	const ECommandResult::Type SyncResult = Provider.Execute(ISourceControlOperation::Create<FSync>(), { Absolute }, EConcurrency::Synchronous);
	TestEqual(TEXT("FSync through the provider succeeds over a read-only delta file"), (int32)SyncResult, (int32)ECommandResult::Succeeded);

	FString OnDisk;
	FFileHelper::LoadFileToString(OnDisk, *Asset);
	TestEqual(TEXT("The asset carries the synced content"), OnDisk, FString(TEXT("second, from the clone")));
	TestFalse(TEXT("No marker remains after the guarded provider sync"), LoreOperationMarker::Read(Fixture.Root).IsValid());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreInterruptedRewriteRecoveryTest, "LoreSourceControl.Recovery.DetectAndRepair", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreInterruptedRewriteRecoveryTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreRecovery"), TEXT("rc"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString File = Fixture.Root / TEXT("torn.txt");
	FFileHelper::SaveStringToFile(TEXT("anchor content"), *File);
	TestTrue(TEXT("Commit lands"), StageCommitPush(Fixture, TEXT("torn.txt"), TEXT("first")));

	FName Previous;
	FLoreSourceControlProvider& Provider = FLoreSourceControlModule::Get().GetProvider();
	BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	// Simulate a crash mid-sync: the file differs from the anchor on disk, and a marker names it. The anchor never moved.
	FFileHelper::SaveStringToFile(TEXT("half-written torn content"), *File);
	FLoreOperationMarker Marker;
	Marker.Kind = ELoreOperationKind::RevisionSync;
	Marker.Instance = LorePluginStore::ReadInstanceId(Fixture.Root);
	Marker.SourceBranch = TEXT("main");
	Marker.ChangedFiles = { TEXT("torn.txt") };
	Marker.Started = FDateTime::UtcNow();
	TestTrue(TEXT("Marker write succeeds"), LoreOperationMarker::Write(Fixture.Root, Marker));

	// Detection reads the marker for this instance and records a pending recovery.
	Provider.DetectInterruptedRewrite();
	TestTrue(TEXT("Detection records a pending recovery"), Provider.HasPendingRecovery());
	TestEqual(TEXT("The pending recovery names the torn file"), Provider.GetPendingRecovery().ChangedFiles.Num(), 1);

	// Recovery resets the torn file to the anchor, re-runs the sync, and clears the marker.
	const bool bRecovered = Provider.PerformRecovery();
	TestTrue(TEXT("Recovery succeeds"), bRecovered);
	TestFalse(TEXT("The pending recovery is cleared"), Provider.HasPendingRecovery());
	TestFalse(TEXT("The marker is gone after recovery"), LoreOperationMarker::Read(Fixture.Root).IsValid());

	FString Restored;
	FFileHelper::LoadFileToString(Restored, *File);
	TestEqual(TEXT("The torn file is restored to the anchor content"), Restored, FString(TEXT("anchor content")));

	// A marker over a clean tree just clears: detection records it, recovery finds nothing dirty and removes it.
	FFileHelper::SaveStringToFile(TEXT("anchor content"), *File);
	FLoreOperationMarker CleanMarker = Marker;
	TestTrue(TEXT("Clean-tree marker write succeeds"), LoreOperationMarker::Write(Fixture.Root, CleanMarker));
	Provider.DetectInterruptedRewrite();
	TestTrue(TEXT("Detection records the clean-tree marker"), Provider.HasPendingRecovery());
	TestTrue(TEXT("Recovery over a clean tree succeeds"), Provider.PerformRecovery());
	TestFalse(TEXT("The clean-tree marker is cleared"), LoreOperationMarker::Read(Fixture.Root).IsValid());

	// A marker whose instance differs from this tree is left alone.
	FLoreOperationMarker OtherInstance = Marker;
	OtherInstance.Instance = TEXT("some-other-instance");
	// Write it directly to this instance's marker file so detection reads it, but with a foreign instance recorded.
	TestTrue(TEXT("Foreign-instance marker write succeeds"), LoreOperationMarker::Write(Fixture.Root, OtherInstance));
	Provider.DetectInterruptedRewrite();
	TestFalse(TEXT("A marker from another instance is not adopted"), Provider.HasPendingRecovery());
	LoreOperationMarker::Clear(Fixture.Root);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreInterruptedSwitchRecoveryTest, "LoreSourceControl.Recovery.SwitchReDispatchArm", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreInterruptedSwitchRecoveryTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreSwitchRecovery"), TEXT("swrc"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// Seed main, then create and diverge feature so main and feature differ by this file. The tree ends on feature.
	const FString File = Fixture.Root / TEXT("shared.txt");
	FFileHelper::SaveStringToFile(TEXT("main content"), *File);
	TestTrue(TEXT("Seed commit on main lands"), StageCommitPush(Fixture, TEXT("shared.txt"), TEXT("main-seed")));

	TArray<FString> Errors;
	TestTrue(TEXT("Feature create succeeds"), RunBranchCreate(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("feature"), FString(), Errors));
	FFileHelper::SaveStringToFile(TEXT("feature content on the branch"), *File);
	TestTrue(TEXT("Feature stages"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, { TEXT("shared.txt") }, Errors));
	TestTrue(TEXT("Feature commits"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), TEXT("feature-change"), Errors));

	FString BranchNow;
	GetBranchName(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, BranchNow);
	TestEqual(TEXT("The tree is on feature before the interrupted switch"), BranchNow, FString(TEXT("feature")));

	FName Previous;
	FLoreSourceControlProvider& Provider = FLoreSourceControlModule::Get().GetProvider();
	BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };
	// The provider's cached branch is set by BeginProviderOn to main; the tree is really on feature, which is what a recovery from a torn switch back to main faces.

	// Simulate a switch to main interrupted mid-rewrite: the shared file is half-written and a branchSwitch marker names main as the target.
	FFileHelper::SaveStringToFile(TEXT("half-written torn during switch"), *File);
	FLoreOperationMarker Marker;
	Marker.Kind = ELoreOperationKind::BranchSwitch;
	Marker.Instance = LorePluginStore::ReadInstanceId(Fixture.Root);
	Marker.SourceBranch = TEXT("feature");
	Marker.TargetBranch = TEXT("main");
	Marker.ChangedFiles = { TEXT("shared.txt") };
	Marker.Started = FDateTime::UtcNow();
	TestTrue(TEXT("Switch marker write succeeds"), LoreOperationMarker::Write(Fixture.Root, Marker));

	Provider.DetectInterruptedRewrite();
	TestTrue(TEXT("Detection records a pending switch recovery"), Provider.HasPendingRecovery());
	TestEqual(TEXT("The pending recovery names main as the target branch"), Provider.GetPendingRecovery().TargetBranch, FString(TEXT("main")));

	// Recovery must reset the torn file, then choose the switch re-dispatch arm (target branch non-empty), landing the tree on main.
	const bool bRecovered = Provider.PerformRecovery();
	TestTrue(TEXT("Switch recovery succeeds"), bRecovered);
	TestFalse(TEXT("The pending recovery is cleared"), Provider.HasPendingRecovery());
	TestFalse(TEXT("The marker is gone after switch recovery"), LoreOperationMarker::Read(Fixture.Root).IsValid());

	FString BranchAfter;
	GetBranchName(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, BranchAfter);
	TestEqual(TEXT("The switch re-dispatch landed the tree on main"), BranchAfter, FString(TEXT("main")));

	FString OnDisk;
	FFileHelper::LoadFileToString(OnDisk, *File);
	TestEqual(TEXT("The file carries main's content after switch recovery"), OnDisk, FString(TEXT("main content")));

	return true;
}

// Breadth: Lock -----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreLockAcquireReleaseQueryTest, "LoreSourceControl.Lock.AcquireThenReleaseThenQuery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreLockAcquireReleaseQueryTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreLockQuery"), TEXT("lkq"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString Asset = Fixture.Root / TEXT("lockme.uasset");
	FFileHelper::SaveStringToFile(TEXT("lock content"), *Asset);
	TestTrue(TEXT("Commit lands"), StageCommitPush(Fixture, TEXT("lockme.uasset"), TEXT("add lockme")));

	auto LockOf = [&]() -> ELoreLockState::Type
	{
		TArray<FLoreSourceControlState> States;
		TArray<FString> StatusErrors;
		RunUpdateStatus(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), true, { Asset }, StatusErrors, States);
		return States.Num() == 1 ? States[0].LockState : ELoreLockState::Unknown;
	};

	TArray<FString> Errors;
	TestTrue(TEXT("Acquire lock succeeds"), AcquireLocks(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, { TEXT("lockme.uasset") }, Errors));
	// Against an auth-off server the owner is unattributable and reads as this user's own held lock.
	TestEqual(TEXT("After acquire the lock reads as Locked"), (int32)LockOf(), (int32)ELoreLockState::Locked);

	TestTrue(TEXT("Release lock succeeds"), ReleaseLocks(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, { TEXT("lockme.uasset") }, Errors));
	TestEqual(TEXT("After release the lock reads as NotLocked"), (int32)LockOf(), (int32)ELoreLockState::NotLocked);

	return true;
}

// Breadth: Offline -------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreOfflineSkipsLockQueryTest, "LoreSourceControl.Offline.UpdateStatusSkipsLockQuery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreOfflineSkipsLockQueryTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreOfflineLock"), TEXT("ofl"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString Asset = Fixture.Root / TEXT("offline.uasset");
	FFileHelper::SaveStringToFile(TEXT("offline content"), *Asset);
	TestTrue(TEXT("Commit lands"), StageCommitPush(Fixture, TEXT("offline.uasset"), TEXT("add offline")));

	TArray<FString> Errors;
	TestTrue(TEXT("Acquire lock succeeds"), AcquireLocks(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, { TEXT("offline.uasset") }, Errors));

	// The command-line backend probes with a fresh process, so it detects a server drop reliably; the in-process native library reuses a cached connection, so its mid-session detection is unreliable and it covers the offline-at-connect case instead. The caller always passes the stale online hint, so a correct result proves the fresh probe, not the hint, drives the decision.
	auto StatusState = [&]() -> FLoreSourceControlState
	{
		TArray<FLoreSourceControlState> States;
		TArray<FString> StatusErrors;
		RunUpdateStatus(ELoreBackendMode::CommandLine, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), true, { Asset }, StatusErrors, States, true);
		return States.Num() == 1 ? States[0] : FLoreSourceControlState(Asset);
	};

	// Online, the status probe reports the remote reachable, the lock query runs, and the held lock shows.
	const FLoreSourceControlState Online = StatusState();
	TestTrue(TEXT("Online state records the remote as available"), Online.bRemoteAvailable);
	TestEqual(TEXT("Online status reads the held lock"), (int32)Online.LockState, (int32)ELoreLockState::Locked);

	// Take the server down mid-session. The status probe reports the remote unavailable in seconds, so the lock query (which would otherwise block for minutes) is skipped: the lock reads as not held and check out grays out.
	Fixture.Server.Stop();
	// Terminate does not wait, so let the process exit and free its socket before probing, or the probe catches a server still answering mid-shutdown.
	FPlatformProcess::Sleep(2.0f);
	const FLoreSourceControlState Offline = StatusState();
	TestFalse(TEXT("Offline state records the remote as unavailable"), Offline.bRemoteAvailable);
	TestEqual(TEXT("Offline status skips the lock query, lock reads NotLocked"), (int32)Offline.LockState, (int32)ELoreLockState::NotLocked);
	TestFalse(TEXT("Offline check out is unavailable"), Offline.CanCheckout());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreOfflineCheckOutMakesWritableTest, "LoreSourceControl.Offline.CheckOutMakesWritable", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreOfflineCheckOutMakesWritableTest::RunTest(const FString& Parameters)
{
	FLoreTestFixture Fixture(*this, TEXT("LoreOfflineCheckout"), TEXT("ofc"));
	if (!Fixture.bReady)
	{
		return false;
	}

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	const bool bWasLocking = FLoreSourceControlModule::Get().GetSettings().IsUsingLocking();
	FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(true);
	ON_SCOPE_EXIT { FLoreSourceControlModule::Get().AccessSettings().SetUsingLocking(bWasLocking); };

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	const FString Asset = FPaths::ConvertRelativePathToFull(Fixture.Root / TEXT("offcheck.uasset"));
	FFileHelper::SaveStringToFile(TEXT("offline checkout content"), *Asset);
	const TArray<FString> AssetTargets = { Asset };

	Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), AssetTargets, EConcurrency::Synchronous);
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> AddCheckIn = ISourceControlOperation::Create<FCheckIn>();
	AddCheckIn->SetDescription(FText::FromString(TEXT("add offline asset")));
	Provider.Execute(AddCheckIn, AssetTargets, EConcurrency::Synchronous);

	// Locking left the committed binary asset read-only; confirm that starting state.
	TestTrue(TEXT("Committed lockable asset is read-only before check out"), PlatformFile.IsReadOnly(*Asset));

	// Force the offline state the command snapshots, then check out. The provider is a process-wide singleton, so restore reachability afterward or later tests inherit the offline path.
	const bool bPrevAvailable = FLoreSourceControlModule::Get().GetProvider().IsRemoteAvailable();
	const bool bPrevAuthorized = FLoreSourceControlModule::Get().GetProvider().IsRemoteAuthorized();
	ON_SCOPE_EXIT { FLoreSourceControlModule::Get().GetProvider().SetRemoteAuthState(bPrevAvailable, bPrevAuthorized); };
	FLoreSourceControlModule::Get().GetProvider().SetRemoteAuthState(false, false);
	const ECommandResult::Type Result = Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), AssetTargets, EConcurrency::Synchronous);
	TestEqual(TEXT("Offline check out succeeds without acquiring a lock"), (int32)Result, (int32)ECommandResult::Succeeded);
	TestFalse(TEXT("Offline check out clears the read-only attribute"), PlatformFile.IsReadOnly(*Asset));

	const FSourceControlStatePtr State = Provider.GetState(Asset, EStateCacheUsage::Use);
	TestTrue(TEXT("State is valid after offline check out"), State.IsValid());
	if (State.IsValid())
	{
		TestFalse(TEXT("Offline check out does not report a held lock"), State->IsCheckedOut());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreOfflineProviderSelfHealTest, "LoreSourceControl.Offline.StatusUpdatesProviderReachability", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreOfflineProviderSelfHealTest::RunTest(const FString& Parameters)
{
	FLoreTestFixture Fixture(*this, TEXT("LoreOfflineHeal"), TEXT("ofh"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString Asset = FPaths::ConvertRelativePathToFull(Fixture.Root / TEXT("heal.uasset"));
	FFileHelper::SaveStringToFile(TEXT("heal content"), *Asset);
	TestTrue(TEXT("Commit lands"), StageCommitPush(Fixture, TEXT("heal.uasset"), TEXT("add heal")));

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	FLoreSourceControlProvider& LoreProvider = FLoreSourceControlModule::Get().GetProvider();
	const bool bPrevAvailable = LoreProvider.IsRemoteAvailable();
	const bool bPrevAuthorized = LoreProvider.IsRemoteAuthorized();
	ON_SCOPE_EXIT { LoreProvider.SetRemoteAuthState(bPrevAvailable, bPrevAuthorized); };

	// The native library masks a mid-session drop behind its cached connection, so drive this through the command-line backend, whose fresh process detects the drop.
	const ELoreBackendMode PrevBackend = FLoreSourceControlModule::Get().GetSettings().GetBackendMode();
	FLoreSourceControlModule::Get().AccessSettings().SetBackendMode(ELoreBackendMode::CommandLine);
	ON_SCOPE_EXIT { FLoreSourceControlModule::Get().AccessSettings().SetBackendMode(PrevBackend); };

	const TArray<FString> Targets = { Asset };

	// Recovery: start the provider marked offline, then a status pass against the reachable server restores it.
	LoreProvider.SetRemoteAuthState(false, false);
	Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), Targets, EConcurrency::Synchronous);
	TestTrue(TEXT("Status pass against a reachable server marks the provider available"), LoreProvider.IsRemoteAvailable());

	// Drop: the server goes down, and the next status pass detects it so later commands gate on offline.
	Fixture.Server.Stop();
	// Terminate does not wait, so let the process exit and free its socket before probing.
	FPlatformProcess::Sleep(2.0f);
	Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), Targets, EConcurrency::Synchronous);
	TestFalse(TEXT("Status pass against a downed server marks the provider unavailable"), LoreProvider.IsRemoteAvailable());

	return true;
}

// Breadth: Diff -----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreDiffBinaryExtractionTest, "LoreSourceControl.Diff.BinaryContentExtraction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreDiffBinaryExtractionTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreDiffBinary"), TEXT("dfb"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// Bytes with embedded nulls and high values, so a text-only path would corrupt them.
	TArray<uint8> Bytes;
	for (int32 i = 0; i < 256; ++i)
	{
		Bytes.Add((uint8)i);
	}
	const FString Asset = Fixture.Root / TEXT("binblob.uasset");
	FFileHelper::SaveArrayToFile(Bytes, *Asset);
	TestTrue(TEXT("Binary commit lands"), StageCommitPush(Fixture, TEXT("binblob.uasset"), TEXT("add binary blob")));

	TLoreSourceControlHistory History;
	TArray<FString> Errors;
	RunGetHistory(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, Asset, Errors, History);
	TestTrue(TEXT("Binary file has a revision"), History.Num() >= 1);
	if (History.Num() >= 1)
	{
		FString ExtractTemp;
		const bool bExtracted = History[0]->Get(ExtractTemp);
		TestTrue(TEXT("Binary revision extracts"), bExtracted);
		if (bExtracted)
		{
			TArray<uint8> Extracted;
			FFileHelper::LoadFileToArray(Extracted, *ExtractTemp);
			TestEqual(TEXT("Extracted byte count matches"), Extracted.Num(), Bytes.Num());
			TestTrue(TEXT("Extracted bytes equal the committed bytes"), Extracted == Bytes);

			// Re-extraction returns the same path (the immutable name carries the revision id).
			FString ExtractRepeat;
			TestTrue(TEXT("Repeat extraction succeeds"), History[0]->Get(ExtractRepeat));
			TestEqual(TEXT("Repeat extraction reuses the same path"), ExtractRepeat, ExtractTemp);
		}
	}

	return true;
}

// Breadth: Move -----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreMoveCarriesHistoryTest, "LoreSourceControl.Move.StagedMoveTracked", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreMoveCarriesHistoryTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreMoveHistory"), TEXT("mvh"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString Source = Fixture.Root / TEXT("before.txt");
	FFileHelper::SaveStringToFile(TEXT("history carries across a move"), *Source);
	TestTrue(TEXT("Source commit lands"), StageCommitPush(Fixture, TEXT("before.txt"), TEXT("add before")));

	// lore stage move records a move already performed on disk, so rename first, then stage and commit the move.
	const FString Dest = Fixture.Root / TEXT("after.txt");
	TestTrue(TEXT("On-disk rename"), IFileManager::Get().Move(*Dest, *Source));
	TArray<FString> Results, Errors;
	TestTrue(TEXT("Stage move succeeds"), RunCommand(TEXT("stage move"), Fixture.Binary, Fixture.Root, TArray<FString>(), { TEXT("before.txt"), TEXT("after.txt") }, Results, Errors));
	TestTrue(TEXT("Move commit lands"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), TEXT("move before to after"), Errors));

	// Lore's file history reports from the new path and does not traverse the rename, so the destination shows its own tracked history rather than the source's depth.
	TLoreSourceControlHistory History;
	TArray<FString> HistErrors;
	RunGetHistory(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, Dest, HistErrors, History);
	TestTrue(TEXT("Moved destination is tracked at its new path"), History.Num() >= 1);

	return true;
}

// Breadth: Backend --------------------------------------------------------------

#if LORE_WITH_LIBLORE
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreBackendStatusParityTest, "LoreSourceControl.Backend.StatusParity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreBackendStatusParityTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreBackendStatus"), TEXT("bks"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// One committed file, one edited file, one untracked file, so several working-copy states appear at once.
	const FString Committed = Fixture.Root / TEXT("committed.txt");
	FFileHelper::SaveStringToFile(TEXT("committed"), *Committed);
	TestTrue(TEXT("Commit lands"), StageCommitPush(Fixture, TEXT("committed.txt"), TEXT("add committed")));

	const FString Edited = Fixture.Root / TEXT("edited.txt");
	FFileHelper::SaveStringToFile(TEXT("edited one"), *Edited);
	TestTrue(TEXT("Edited file first commit lands"), StageCommitPush(Fixture, TEXT("edited.txt"), TEXT("add edited")));
	FFileHelper::SaveStringToFile(TEXT("edited two"), *Edited);

	const FString Untracked = Fixture.Root / TEXT("untracked.txt");
	FFileHelper::SaveStringToFile(TEXT("untracked"), *Untracked);

	const TArray<FString> Files = { Committed, Edited, Untracked };

	auto StatesFor = [&](ELoreBackendMode Mode, TMap<FString, ELoreWorkingCopyState::Type>& OutMap, bool& bOutAhead)
	{
		TArray<FLoreSourceControlState> States;
		TArray<FString> StatusErrors;
		RunUpdateStatus(Mode, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), false, Files, StatusErrors, States);
		bOutAhead = false;
		for (const FLoreSourceControlState& State : States)
		{
			OutMap.Add(State.LocalFilename, State.WorkingCopyState);
			if (!State.IsCurrent())
			{
				bOutAhead = true;
			}
		}
	};

	TMap<FString, ELoreWorkingCopyState::Type> NativeMap, CliMap, AutoMap;
	bool bNativeAhead = false, bCliAhead = false, bAutoAhead = false;
	StatesFor(ELoreBackendMode::Native, NativeMap, bNativeAhead);
	StatesFor(ELoreBackendMode::CommandLine, CliMap, bCliAhead);
	StatesFor(ELoreBackendMode::Automatic, AutoMap, bAutoAhead);

	TestEqual(TEXT("Native and command line agree on file count"), NativeMap.Num(), CliMap.Num());
	TestEqual(TEXT("Native and automatic agree on file count"), NativeMap.Num(), AutoMap.Num());
	TestEqual(TEXT("Native and command line agree on remote-ahead"), bNativeAhead, bCliAhead);
	TestEqual(TEXT("Native and automatic agree on remote-ahead"), bNativeAhead, bAutoAhead);
	for (const TPair<FString, ELoreWorkingCopyState::Type>& Pair : NativeMap)
	{
		const ELoreWorkingCopyState::Type* CliState = CliMap.Find(Pair.Key);
		const ELoreWorkingCopyState::Type* AutoState = AutoMap.Find(Pair.Key);
		TestNotNull(*FString::Printf(TEXT("Command line reports %s"), *Pair.Key), CliState);
		TestNotNull(*FString::Printf(TEXT("Automatic reports %s"), *Pair.Key), AutoState);
		if (CliState)
		{
			TestEqual(*FString::Printf(TEXT("Native and command line agree on state for %s"), *Pair.Key), (int32)*CliState, (int32)Pair.Value);
		}
		if (AutoState)
		{
			TestEqual(*FString::Printf(TEXT("Native and automatic agree on state for %s"), *Pair.Key), (int32)*AutoState, (int32)Pair.Value);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreBackendWriteFileParityTest, "LoreSourceControl.Backend.WriteFileParity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreBackendWriteFileParityTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreBackendWrite"), TEXT("bkw"));
	if (!Fixture.bReady)
	{
		return false;
	}

	TArray<uint8> Bytes;
	for (int32 i = 0; i < 200; ++i)
	{
		Bytes.Add((uint8)((i * 7 + 3) & 0xFF));
	}
	const FString Asset = Fixture.Root / TEXT("parity.uasset");
	FFileHelper::SaveArrayToFile(Bytes, *Asset);
	TestTrue(TEXT("Commit lands"), StageCommitPush(Fixture, TEXT("parity.uasset"), TEXT("add parity blob")));

	TLoreSourceControlHistory History;
	TArray<FString> Errors;
	RunGetHistory(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, Asset, Errors, History);
	TestTrue(TEXT("Parity file has a revision"), History.Num() >= 1);
	if (History.Num() < 1)
	{
		return false;
	}
	const FString CommitId = History[0]->CommitId;
	const FString RelativeFile = RelativeFilename(Asset, Fixture.Root);

	auto ExtractThrough = [&](ELoreBackendMode Mode, const TCHAR* Leaf, TArray<uint8>& OutBytes) -> bool
	{
		const FString Out = Fixture.Root / Leaf;
		IFileManager::Get().Delete(*Out, false, true);
		TArray<FString> WriteErrors;
		const bool bWritten = WriteRevisionFile(Mode, Fixture.Binary, Fixture.Root, RelativeFile, CommitId, Out, WriteErrors);
		if (!bWritten)
		{
			return false;
		}
		return FFileHelper::LoadFileToArray(OutBytes, *Out);
	};

	TArray<uint8> NativeBytes, CliBytes, AutoBytes;
	const bool bNative = ExtractThrough(ELoreBackendMode::Native, TEXT("out_native.bin"), NativeBytes);
	const bool bCli = ExtractThrough(ELoreBackendMode::CommandLine, TEXT("out_cli.bin"), CliBytes);
	const bool bAuto = ExtractThrough(ELoreBackendMode::Automatic, TEXT("out_auto.bin"), AutoBytes);
	TestTrue(TEXT("Native extraction succeeds"), bNative);
	TestTrue(TEXT("Command-line extraction succeeds"), bCli);
	TestTrue(TEXT("Automatic extraction succeeds"), bAuto);

	TestTrue(TEXT("Native bytes equal the committed bytes"), NativeBytes == Bytes);
	TestTrue(TEXT("Command line and native extract identical bytes"), CliBytes == NativeBytes);
	TestTrue(TEXT("Automatic and native extract identical bytes"), AutoBytes == NativeBytes);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreContentQueryLocalPresentTest, "LoreSourceControl.Content.QueryLocalPresent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreContentQueryLocalPresentTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreContentPresent"), TEXT("cqp"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString Asset = Fixture.Root / TEXT("present.txt");
	FFileHelper::SaveStringToFile(TEXT("freshly committed content is local"), *Asset);
	TestTrue(TEXT("Commit lands"), StageCommitPush(Fixture, TEXT("present.txt"), TEXT("add present blob")));

	TLoreSourceControlHistory History;
	TArray<FString> Errors;
	RunGetHistory(ELoreBackendMode::Native, Fixture.Binary, Fixture.Root, Asset, Errors, History);
	TestTrue(TEXT("Present file has a revision"), History.Num() >= 1);
	if (History.Num() < 1)
	{
		return false;
	}

	const FString Address = History[0]->ContentAddress;
	TestFalse(TEXT("Latest revision carries a content address"), Address.IsEmpty());

	bool bAvailable = false;
	FString ProbeError;
	const bool bQueried = LoreClient::QueryContentAvailable(Fixture.Root, Address, bAvailable, ProbeError);
	TestTrue(FString::Printf(TEXT("Content-availability query succeeds (err: %s)"), *ProbeError), bQueried);
	TestTrue(TEXT("Pushed content reads as available, since its payload is on the remote"), bAvailable);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreContentQueryBogusAddressAbsentTest, "LoreSourceControl.Content.QueryBogusAddressAbsent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreContentQueryBogusAddressAbsentTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreContentBogus"), TEXT("cqb"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// Syntactically valid address (64 hex hash, hyphen, 32 hex context) that no content hashes to.
	const FString HashHex = FString::ChrN(64, TEXT('0'));
	const FString ContextHex = FString::ChrN(32, TEXT('0'));
	const FString BogusAddress = HashHex + TEXT("-") + ContextHex;

	bool bAvailable = true;
	FString ProbeError;
	const bool bQueried = LoreClient::QueryContentAvailable(Fixture.Root, BogusAddress, bAvailable, ProbeError);
	TestTrue(FString::Printf(TEXT("Query of a non-existent address does not error fatally (err: %s)"), *ProbeError), bQueried);
	TestFalse(TEXT("A non-existent address reads as not available"), bAvailable);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreHistoryCapturesContentAddressTest, "LoreSourceControl.History.CapturesContentAddress", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreHistoryCapturesContentAddressTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreHistoryAddress"), TEXT("hca"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString Asset = Fixture.Root / TEXT("address.txt");
	FFileHelper::SaveStringToFile(TEXT("revision carries a content address"), *Asset);
	TestTrue(TEXT("Commit lands"), StageCommitPush(Fixture, TEXT("address.txt"), TEXT("add address blob")));

	TLoreSourceControlHistory History;
	TArray<FString> Errors;
	RunGetHistory(ELoreBackendMode::Native, Fixture.Binary, Fixture.Root, Asset, Errors, History);
	TestTrue(TEXT("Address file has a revision"), History.Num() >= 1);
	if (History.Num() < 1)
	{
		return false;
	}

	const FString Address = History[0]->ContentAddress;
	TestFalse(TEXT("Content address is non-empty"), Address.IsEmpty());
	TestTrue(TEXT("Content address is in hash-context form"), Address.Contains(TEXT("-")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreContentObliteratedMarkedTest, "LoreSourceControl.Content.ObliteratedContentMarked", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreContentObliteratedMarkedTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreObliterate"), TEXT("obl"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// Two local revisions, no push, so the older blob lives only in the local store and obliterate truly removes it.
	const FString Asset = Fixture.Root / TEXT("oblit.txt");
	const FString RelFile = TEXT("oblit.txt");
	TArray<FString> Results, Errors;

	FFileHelper::SaveStringToFile(TEXT("first revision content for obliterate"), *Asset);
	TestTrue(TEXT("First stage"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, { RelFile }, Errors));
	TestTrue(TEXT("First commit"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), TEXT("first oblit revision"), Errors));

	FFileHelper::SaveStringToFile(TEXT("second revision content, different bytes"), *Asset);
	TestTrue(TEXT("Second stage"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, { RelFile }, Errors));
	TestTrue(TEXT("Second commit"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), TEXT("second oblit revision"), Errors));

	TLoreSourceControlHistory History;
	RunGetHistory(ELoreBackendMode::Native, Fixture.Binary, Fixture.Root, Asset, Errors, History);
	TestTrue(TEXT("History has two revisions"), History.Num() >= 2);
	if (History.Num() < 2)
	{
		return false;
	}

	// History is newest first, so the last entry is the original revision whose content the second revision replaced.
	const FString OlderAddress = History.Last()->ContentAddress;
	TestFalse(TEXT("Older revision carries a content address"), OlderAddress.IsEmpty());

	// lore file obliterate --address <addr> removes the blob from the local immutable store.
	const bool bObliterated = RunCommand(TEXT("file obliterate"), Fixture.Binary, Fixture.Root, { FString::Printf(TEXT("--address %s"), *OlderAddress) }, TArray<FString>(), Results, Errors);
	if (!bObliterated)
	{
		// Obliterate is not available in this client build; skip rather than fail the fixture.
		AddInfo(TEXT("lore file obliterate did not run; skipping the marker assertions for this client."));
		return true;
	}

	// The older content was never pushed, so after a local obliterate it is in neither store and the probe reports it unavailable.
	bool bAvailableAfter = true;
	FString ProbeError;
	LoreClient::QueryContentAvailable(Fixture.Root, OlderAddress, bAvailableAfter, ProbeError);
	TestFalse(TEXT("Obliterated content reads as unavailable"), bAvailableAfter);

	// Rebuild history; the older revision now carries the not-local marker, and its extraction fails with no server to fetch from.
	TLoreSourceControlHistory After;
	RunGetHistory(ELoreBackendMode::Native, Fixture.Binary, Fixture.Root, Asset, Errors, After);
	TestTrue(TEXT("Rebuilt history still has two revisions"), After.Num() >= 2);
	if (After.Num() >= 2)
	{
		TestTrue(TEXT("Older revision description carries the unavailable marker"), After.Last()->GetDescription().Contains(TEXT("[Unavailable]")));
		FString DiffTemp;
		TestFalse(TEXT("Obliterated revision cannot extract for diff"), After.Last()->Get(DiffTemp));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreContentAvailableNotMarkedTest, "LoreSourceControl.Content.AvailableNotMarked", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreContentAvailableNotMarkedTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreContentAvail"), TEXT("cav"));
	if (!Fixture.bReady)
	{
		return false;
	}

	const FString Asset = Fixture.Root / TEXT("avail.txt");
	FFileHelper::SaveStringToFile(TEXT("available content"), *Asset);
	TestTrue(TEXT("Commit lands"), StageCommitPush(Fixture, TEXT("avail.txt"), TEXT("add avail")));

	// The server is up, so the pushed content is fetchable from the remote even though the local store dropped the payload after the push. History must not mark it unavailable; this guards against the regression where every pushed revision read as unavailable.
	TLoreSourceControlHistory History;
	TArray<FString> Errors;
	RunGetHistory(ELoreBackendMode::Native, Fixture.Binary, Fixture.Root, Asset, Errors, History);
	TestTrue(TEXT("History has a revision"), History.Num() >= 1);
	if (History.Num() >= 1)
	{
		TestFalse(TEXT("Content available on the server is not marked unavailable"), History[0]->GetDescription().Contains(TEXT("[Unavailable]")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreBranchListTest, "LoreSourceControl.Branch.ListEnumeratesBranches", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreBranchListTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreBranchList"), TEXT("brl"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// A branch is created from a revision, so seed one first.
	const FString Asset = Fixture.Root / TEXT("seed.txt");
	FFileHelper::SaveStringToFile(TEXT("seed"), *Asset);
	TestTrue(TEXT("Seed commit lands"), StageCommitPush(Fixture, TEXT("seed.txt"), TEXT("seed")));

	// The default branch enumerates as the current local branch.
	TArray<LoreClient::FBranchInfo> Branches;
	FString Error;
	TestTrue(FString::Printf(TEXT("Branch list succeeds (err: %s)"), *Error), LoreClient::BranchList(Fixture.Root, false, Branches, Error));
	const LoreClient::FBranchInfo* Main = Branches.FindByPredicate([](const LoreClient::FBranchInfo& B) { return B.Name == TEXT("main") && !B.bIsRemote; });
	TestNotNull(TEXT("Local main branch is listed"), Main);
	if (Main)
	{
		TestTrue(TEXT("Local main branch is current"), Main->bIsCurrent);
		TestFalse(TEXT("Local main branch is not archived"), Main->bArchived);
		TestFalse(TEXT("Local main branch latest hash is set"), Main->LatestHash.IsEmpty());
	}

	// A newly created branch appears in the listing.
	TArray<FString> Results, Errors;
	const bool bCreated = RunCommand(TEXT("branch create feature-x"), Fixture.Binary, Fixture.Root, TArray<FString>(), TArray<FString>(), Results, Errors);
	TestTrue(TEXT("Branch create succeeds"), bCreated);

	TArray<LoreClient::FBranchInfo> After;
	TestTrue(TEXT("Branch list succeeds after create"), LoreClient::BranchList(Fixture.Root, false, After, Error));
	const bool bHasFeature = After.ContainsByPredicate([](const LoreClient::FBranchInfo& B) { return B.Name == TEXT("feature-x") && !B.bIsRemote; });
	TestTrue(TEXT("Newly created branch is listed"), bHasFeature);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreBranchProtectMetadataTest, "LoreSourceControl.Branch.ProtectMetadataRoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreBranchProtectMetadataTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreBranchProtect"), TEXT("bpr"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// A branch needs a revision before it can be protected and pushed, so seed one.
	const FString Asset = Fixture.Root / TEXT("seed.txt");
	FFileHelper::SaveStringToFile(TEXT("seed"), *Asset);
	TestTrue(TEXT("Seed commit lands"), StageCommitPush(Fixture, TEXT("seed.txt"), TEXT("seed")));

	// The protect key is absent on a fresh branch.
	FString Value;
	bool bFound = false;
	FString Error;
	TestTrue(FString::Printf(TEXT("Metadata get succeeds on a fresh branch (err: %s)"), *Error), LoreClient::BranchMetadataGet(Fixture.Root, TEXT("main"), TEXT("protect"), Value, bFound, Error));
	TestFalse(TEXT("A fresh branch has no protect key set to true"), bFound && Value == TEXT("true"));

	// Protecting the branch sets the key to true.
	TArray<FString> Results, Errors;
	TestTrue(TEXT("Branch protect succeeds"), RunCommand(TEXT("branch protect main"), Fixture.Binary, Fixture.Root, TArray<FString>(), TArray<FString>(), Results, Errors));
	Value.Empty(); bFound = false;
	TestTrue(TEXT("Metadata get succeeds after protect"), LoreClient::BranchMetadataGet(Fixture.Root, TEXT("main"), TEXT("protect"), Value, bFound, Error));
	TestTrue(TEXT("Protect key reads found after protect"), bFound);
	TestEqual(TEXT("Protect key reads true after protect"), Value, FString(TEXT("true")));

	// The native path and the command-line mirror agree on the protected key.
	FString NativeValue, CommandLineValue;
	bool bNativeFound = false, bCommandLineFound = false;
	TArray<FString> NativeErrors, CommandLineErrors;
	TestTrue(TEXT("Native metadata get succeeds"), RunBranchMetadataGet(ELoreBackendMode::Native, Fixture.Binary, Fixture.Root, TEXT("main"), TEXT("protect"), NativeValue, bNativeFound, NativeErrors));
	TestTrue(TEXT("Command-line metadata get succeeds"), RunBranchMetadataGet(ELoreBackendMode::CommandLine, Fixture.Binary, Fixture.Root, TEXT("main"), TEXT("protect"), CommandLineValue, bCommandLineFound, CommandLineErrors));
	TestEqual(TEXT("Backends agree the key is found"), bNativeFound, bCommandLineFound);
	TestEqual(TEXT("Backends agree on the protect value"), NativeValue, CommandLineValue);
	TestEqual(TEXT("Command-line mirror reads true"), CommandLineValue, FString(TEXT("true")));

	// Unprotecting sets the key back to false.
	TestTrue(TEXT("Branch unprotect succeeds"), RunCommand(TEXT("branch unprotect main"), Fixture.Binary, Fixture.Root, TArray<FString>(), TArray<FString>(), Results, Errors));
	Value.Empty(); bFound = false;
	TestTrue(TEXT("Metadata get succeeds after unprotect"), LoreClient::BranchMetadataGet(Fixture.Root, TEXT("main"), TEXT("protect"), Value, bFound, Error));
	TestFalse(TEXT("Protect key does not read true after unprotect"), Value == TEXT("true"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreRevisionHistoryOrderAndPagingTest, "LoreSourceControl.Revision.HistoryOrderAndPaging", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreRevisionHistoryOrderAndPagingTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreRevisionHistory"), TEXT("rvh"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// Three known commits give the listing a known count, order, and messages.
	const FString Asset = Fixture.Root / TEXT("page.txt");
	FFileHelper::SaveStringToFile(TEXT("one"), *Asset);
	TestTrue(TEXT("First commit lands"), StageCommitPush(Fixture, TEXT("page.txt"), TEXT("first page commit")));
	FFileHelper::SaveStringToFile(TEXT("two"), *Asset);
	TestTrue(TEXT("Second commit lands"), StageCommitPush(Fixture, TEXT("page.txt"), TEXT("second page commit")));
	FFileHelper::SaveStringToFile(TEXT("three"), *Asset);
	TestTrue(TEXT("Third commit lands"), StageCommitPush(Fixture, TEXT("page.txt"), TEXT("third page commit")));

	TArray<LoreClient::FRevisionEntry> All;
	FString Error;
	TestTrue(FString::Printf(TEXT("Revision list succeeds (err: %s)"), *Error), LoreClient::RevisionList(Fixture.Root, FString(), FString(), 0, All, Error));
	TestEqual(TEXT("Three fixture commits list three revisions"), All.Num(), 3);
	if (All.Num() != 3)
	{
		return false;
	}
	TestEqual(TEXT("Listing is newest first"), All[0].RevisionNumber, 3);
	TestEqual(TEXT("Middle entry is revision two"), All[1].RevisionNumber, 2);
	TestEqual(TEXT("Oldest entry is revision one"), All[2].RevisionNumber, 1);
	TestEqual(TEXT("Newest entry carries the newest message"), All[0].Message, FString(TEXT("third page commit")));
	TestEqual(TEXT("Oldest entry carries the oldest message"), All[2].Message, FString(TEXT("first page commit")));
	TestEqual(TEXT("Newest entry names the fixture author"), All[0].Author, FString(TEXT("test@example.com")));
	TestEqual(TEXT("Newest entry chains to the middle entry"), All[0].ParentHash, All[1].Hash);
	TestEqual(TEXT("Middle entry chains to the oldest entry"), All[1].ParentHash, All[2].Hash);

	// The start revision is included in its page, so the next page starts at the oldest loaded entry's parent and the pages meet with no overlap.
	TArray<LoreClient::FRevisionEntry> PageOne;
	TestTrue(TEXT("First page succeeds"), LoreClient::RevisionList(Fixture.Root, FString(), FString(), 2, PageOne, Error));
	TestEqual(TEXT("First page holds exactly the page length"), PageOne.Num(), 2);
	if (PageOne.Num() == 2)
	{
		TestEqual(TEXT("First page starts at the tip"), PageOne[0].Hash, All[0].Hash);
		TArray<LoreClient::FRevisionEntry> PageTwo;
		TestTrue(TEXT("Second page succeeds"), LoreClient::RevisionList(Fixture.Root, FString(), PageOne.Last().ParentHash, 2, PageTwo, Error));
		TestEqual(TEXT("Second page holds the one remaining revision"), PageTwo.Num(), 1);
		if (PageTwo.Num() == 1)
		{
			TestEqual(TEXT("Pages meet exactly"), PageTwo[0].Hash, All[2].Hash);
			TestNotEqual(TEXT("Pages do not repeat the boundary revision"), PageTwo[0].Hash, PageOne.Last().Hash);
		}
	}

	// The native path and the command-line mirror agree entry for entry.
	TArray<FLoreRevisionListEntry> NativeList, CommandLineList;
	TArray<FString> NativeErrors, CommandLineErrors;
	TestTrue(TEXT("Native revision list succeeds"), RunRevisionList(ELoreBackendMode::Native, Fixture.Binary, Fixture.Root, FString(), FString(), 0, NativeList, NativeErrors));
	TestTrue(TEXT("Command-line revision list succeeds"), RunRevisionList(ELoreBackendMode::CommandLine, Fixture.Binary, Fixture.Root, FString(), FString(), 0, CommandLineList, CommandLineErrors));
	TestEqual(TEXT("Backends agree on revision count"), NativeList.Num(), CommandLineList.Num());
	for (int32 Index = 0; Index < FMath::Min(NativeList.Num(), CommandLineList.Num()); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("Backends agree on hash %d"), Index), NativeList[Index].Hash, CommandLineList[Index].Hash);
		TestEqual(*FString::Printf(TEXT("Backends agree on number %d"), Index), NativeList[Index].RevisionNumber, CommandLineList[Index].RevisionNumber);
		TestEqual(*FString::Printf(TEXT("Backends agree on parent %d"), Index), NativeList[Index].ParentHash, CommandLineList[Index].ParentHash);
		TestEqual(*FString::Printf(TEXT("Backends agree on merge parent %d"), Index), NativeList[Index].ParentOtherHash, CommandLineList[Index].ParentOtherHash);
		TestTrue(*FString::Printf(TEXT("Backends agree on date %d"), Index), NativeList[Index].Date == CommandLineList[Index].Date);
		TestEqual(*FString::Printf(TEXT("Backends agree on message %d"), Index), NativeList[Index].Message, CommandLineList[Index].Message);
		TestEqual(*FString::Printf(TEXT("Backends agree on author %d"), Index), NativeList[Index].Author, CommandLineList[Index].Author);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreRevisionInfoMetadataAndDeltaTest, "LoreSourceControl.Revision.InfoMetadataAndDelta", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreRevisionInfoMetadataAndDeltaTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreRevisionInfo"), TEXT("rvi"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// First commit adds two files; the second edits one, so each revision has a known delta.
	const FString Alpha = Fixture.Root / TEXT("alpha.txt");
	const FString Beta = Fixture.Root / TEXT("beta.txt");
	FFileHelper::SaveStringToFile(TEXT("alpha one"), *Alpha);
	FFileHelper::SaveStringToFile(TEXT("beta one"), *Beta);
	TArray<FString> Errors;
	TestTrue(TEXT("Both files stage"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, { TEXT("alpha.txt"), TEXT("beta.txt") }, Errors));
	TestTrue(TEXT("First commit lands"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), TEXT("add alpha and beta"), Errors));
	TestTrue(TEXT("First push lands"), PushBranch(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, Errors));

	FFileHelper::SaveStringToFile(TEXT("alpha two"), *Alpha);
	TestTrue(TEXT("Second commit lands"), StageCommitPush(Fixture, TEXT("alpha.txt"), TEXT("edit alpha")));

	TArray<LoreClient::FRevisionEntry> Revisions;
	FString Error;
	TestTrue(TEXT("Revision list succeeds"), LoreClient::RevisionList(Fixture.Root, FString(), FString(), 0, Revisions, Error));
	TestEqual(TEXT("Two fixture commits list two revisions"), Revisions.Num(), 2);
	if (Revisions.Num() != 2)
	{
		return false;
	}

	// Metadata of the newest revision matches what the fixture committed.
	LoreClient::FRevisionDetails Newest;
	TestTrue(FString::Printf(TEXT("Revision info succeeds (err: %s)"), *Error), LoreClient::RevisionInfo(Fixture.Root, Revisions[0].Hash, true, Newest, Error));
	TestEqual(TEXT("Info reports the queried hash"), Newest.Hash, Revisions[0].Hash);
	TestEqual(TEXT("Info reports the revision number"), Newest.RevisionNumber, 2);
	TestEqual(TEXT("Message matches the fixture commit"), Newest.Message, FString(TEXT("edit alpha")));
	TestEqual(TEXT("Author matches the fixture identity"), Newest.Author, FString(TEXT("test@example.com")));
	TestTrue(TEXT("Timestamp is set"), Newest.Date > FDateTime(2020, 1, 1));

	// The edit delta names exactly the one edited file, a keep flagged modified.
	TArray<LoreClient::FRevisionDelta> NewestFiles;
	for (const LoreClient::FRevisionDelta& Delta : Newest.Deltas)
	{
		if (Delta.bIsFile)
		{
			NewestFiles.Add(Delta);
		}
	}
	TestEqual(TEXT("Edit delta holds exactly one file"), NewestFiles.Num(), 1);
	if (NewestFiles.Num() == 1)
	{
		TestEqual(TEXT("Edit delta names the edited file"), NewestFiles[0].Path, FString(TEXT("alpha.txt")));
		TestEqual(TEXT("Edited file keeps its identity"), NewestFiles[0].Action, FString(TEXT("keep")));
		TestTrue(TEXT("Edited file is flagged modified"), NewestFiles[0].bModified);
		TestTrue(TEXT("Edited file carries its size"), NewestFiles[0].Size > 0);
	}

	// The first revision's delta names exactly the two added files.
	LoreClient::FRevisionDetails Oldest;
	TestTrue(TEXT("Info of the first revision succeeds"), LoreClient::RevisionInfo(Fixture.Root, Revisions[1].Hash, true, Oldest, Error));
	TArray<FString> OldestFiles;
	for (const LoreClient::FRevisionDelta& Delta : Oldest.Deltas)
	{
		if (Delta.bIsFile)
		{
			OldestFiles.Add(Delta.Path);
			TestEqual(TEXT("Added file action is add"), Delta.Action, FString(TEXT("add")));
		}
	}
	OldestFiles.Sort();
	TestEqual(TEXT("Add delta holds exactly two files"), OldestFiles.Num(), 2);
	if (OldestFiles.Num() == 2)
	{
		TestEqual(TEXT("Add delta names alpha"), OldestFiles[0], FString(TEXT("alpha.txt")));
		TestEqual(TEXT("Add delta names beta"), OldestFiles[1], FString(TEXT("beta.txt")));
	}

	// The native path and the command-line mirror agree on metadata and delta.
	FLoreRevisionDetails NativeDetails, CommandLineDetails;
	TArray<FString> NativeErrors, CommandLineErrors;
	TestTrue(TEXT("Native revision info succeeds"), RunRevisionInfo(ELoreBackendMode::Native, Fixture.Binary, Fixture.Root, Revisions[0].Hash, true, NativeDetails, NativeErrors));
	TestTrue(TEXT("Command-line revision info succeeds"), RunRevisionInfo(ELoreBackendMode::CommandLine, Fixture.Binary, Fixture.Root, Revisions[0].Hash, true, CommandLineDetails, CommandLineErrors));
	TestEqual(TEXT("Backends agree on the hash"), NativeDetails.Hash, CommandLineDetails.Hash);
	TestEqual(TEXT("Backends agree on the number"), NativeDetails.RevisionNumber, CommandLineDetails.RevisionNumber);
	TestEqual(TEXT("Backends agree on the message"), NativeDetails.Message, CommandLineDetails.Message);
	TestEqual(TEXT("Backends agree on the author"), NativeDetails.Author, CommandLineDetails.Author);
	TestTrue(TEXT("Backends agree on the date"), NativeDetails.Date == CommandLineDetails.Date);
	TestEqual(TEXT("Backends agree on delta count"), NativeDetails.Deltas.Num(), CommandLineDetails.Deltas.Num());
	for (int32 Index = 0; Index < FMath::Min(NativeDetails.Deltas.Num(), CommandLineDetails.Deltas.Num()); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("Backends agree on delta path %d"), Index), NativeDetails.Deltas[Index].Path, CommandLineDetails.Deltas[Index].Path);
		TestEqual(*FString::Printf(TEXT("Backends agree on delta action %d"), Index), NativeDetails.Deltas[Index].Action, CommandLineDetails.Deltas[Index].Action);
		TestTrue(*FString::Printf(TEXT("Backends agree on delta size %d"), Index), NativeDetails.Deltas[Index].Size == CommandLineDetails.Deltas[Index].Size);
		TestTrue(*FString::Printf(TEXT("Backends agree on delta flags %d"), Index), NativeDetails.Deltas[Index].bModified == CommandLineDetails.Deltas[Index].bModified && NativeDetails.Deltas[Index].bMerged == CommandLineDetails.Deltas[Index].bMerged && NativeDetails.Deltas[Index].bIsFile == CommandLineDetails.Deltas[Index].bIsFile);
	}

	return true;
}
#endif // LORE_WITH_LIBLORE

// Branch item merge is a pure function, so it runs on every engine with no server.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreBranchMergeTest, "LoreSourceControl.Branch.MergeFoldsLocalAndRemote", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreBranchMergeTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	auto MakeEntry = [](const TCHAR* Name, bool bRemote, const TCHAR* Latest, bool bCurrent, bool bArchived)
	{
		FLoreBranchItem E;
		E.Name = Name;
		E.bHasLocal = !bRemote;
		E.bHasRemote = bRemote;
		if (bRemote) { E.LatestRemote = Latest; } else { E.LatestLocal = Latest; }
		E.bIsCurrent = bCurrent;
		E.bArchived = bArchived;
		return E;
	};

	// A local-only, a remote-only, a branch in both that agrees, a branch in both that diverges, and an archived local-only branch with a blank latest.
	TArray<FLoreBranchItem> Entries;
	Entries.Add(MakeEntry(TEXT("local-only"), false, TEXT("aaaa"), false, false));
	Entries.Add(MakeEntry(TEXT("remote-only"), true, TEXT("bbbb"), false, false));
	Entries.Add(MakeEntry(TEXT("both-agree"), false, TEXT("cccc"), true, false));
	Entries.Add(MakeEntry(TEXT("both-agree"), true, TEXT("cccc"), false, false));
	Entries.Add(MakeEntry(TEXT("both-diverge"), false, TEXT("dddd"), false, false));
	Entries.Add(MakeEntry(TEXT("both-diverge"), true, TEXT("eeee"), false, false));
	Entries.Add(MakeEntry(TEXT("archived-branch"), false, TEXT(""), false, true));

	TArray<FLoreBranchItem> Merged;
	MergeBranchListEntries(Entries, Merged);

	TestEqual(TEXT("Five distinct names fold to five rows"), Merged.Num(), 5);

	auto Find = [&Merged](const TCHAR* Name) -> const FLoreBranchItem*
	{
		return Merged.FindByPredicate([Name](const FLoreBranchItem& R) { return R.Name == Name; });
	};

	const FLoreBranchItem* LocalOnly = Find(TEXT("local-only"));
	TestNotNull(TEXT("Local-only row exists"), LocalOnly);
	if (LocalOnly)
	{
		TestTrue(TEXT("Local-only has local"), LocalOnly->bHasLocal);
		TestFalse(TEXT("Local-only has no remote"), LocalOnly->bHasRemote);
	}

	const FLoreBranchItem* RemoteOnly = Find(TEXT("remote-only"));
	TestNotNull(TEXT("Remote-only row exists"), RemoteOnly);
	if (RemoteOnly)
	{
		TestFalse(TEXT("Remote-only has no local"), RemoteOnly->bHasLocal);
		TestTrue(TEXT("Remote-only has remote"), RemoteOnly->bHasRemote);
	}

	const FLoreBranchItem* Both = Find(TEXT("both-agree"));
	TestNotNull(TEXT("Both-agree row exists"), Both);
	if (Both)
	{
		TestTrue(TEXT("Both-agree has local and remote"), Both->bHasLocal && Both->bHasRemote);
		TestTrue(TEXT("Both-agree keeps the local current flag"), Both->bIsCurrent);
		TestEqual(TEXT("Both-agree local and remote latest match"), Both->LatestLocal, Both->LatestRemote);
	}

	const FLoreBranchItem* Diverge = Find(TEXT("both-diverge"));
	TestNotNull(TEXT("Both-diverge row exists"), Diverge);
	if (Diverge)
	{
		TestTrue(TEXT("Both-diverge has local and remote"), Diverge->bHasLocal && Diverge->bHasRemote);
		TestNotEqual(TEXT("Both-diverge latest hashes differ"), Diverge->LatestLocal, Diverge->LatestRemote);
	}

	const FLoreBranchItem* Archived = Find(TEXT("archived-branch"));
	TestNotNull(TEXT("Archived row exists"), Archived);
	if (Archived)
	{
		TestTrue(TEXT("Archived is local-only"), Archived->bHasLocal && !Archived->bHasRemote);
		TestTrue(TEXT("Archived is flagged archived"), Archived->bArchived);
		TestTrue(TEXT("Archived renders a blank latest"), Archived->LatestLocal.IsEmpty());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreGetBranchesReadTest, "LoreSourceControl.Branch.GetBranchesReadsRowsAndProtect", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreGetBranchesReadTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreGetBranches"), TEXT("gbr"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// Seed a revision so branches have a latest, then create a second branch and protect main.
	const FString Asset = Fixture.Root / TEXT("seed.txt");
	FFileHelper::SaveStringToFile(TEXT("seed"), *Asset);
	TestTrue(TEXT("Seed commit lands"), StageCommitPush(Fixture, TEXT("seed.txt"), TEXT("seed")));

	TArray<FString> Results, Errors;
	TestTrue(TEXT("Branch create succeeds"), RunCommand(TEXT("branch create feature-y"), Fixture.Binary, Fixture.Root, TArray<FString>(), TArray<FString>(), Results, Errors));
	// Creating feature-y switched to it; switch back so main reads current, then protect main.
	TestTrue(TEXT("Switch back to main succeeds"), RunCommand(TEXT("branch switch main"), Fixture.Binary, Fixture.Root, TArray<FString>(), TArray<FString>(), Results, Errors));
	TestTrue(TEXT("Branch protect main succeeds"), RunCommand(TEXT("branch protect main"), Fixture.Binary, Fixture.Root, TArray<FString>(), TArray<FString>(), Results, Errors));

	TArray<FLoreBranchItem> Branches;
	TArray<FString> ReadErrors;
	TestTrue(TEXT("GetBranches succeeds"), RunGetBranches(ELoreBackendMode::Native, Fixture.Binary, Fixture.Root, false, Branches, ReadErrors));

	const FLoreBranchItem* Main = Branches.FindByPredicate([](const FLoreBranchItem& B) { return B.Name == TEXT("main"); });
	TestNotNull(TEXT("main is enumerated"), Main);
	if (Main)
	{
		TestTrue(TEXT("main is the current branch"), Main->bIsCurrent);
		TestTrue(TEXT("main reads a local latest"), !Main->LatestLocal.IsEmpty());
		TestTrue(TEXT("main reads protected after protect"), Main->bProtected);
	}

	const FLoreBranchItem* Feature = Branches.FindByPredicate([](const FLoreBranchItem& B) { return B.Name == TEXT("feature-y"); });
	TestNotNull(TEXT("feature-y is enumerated"), Feature);
	if (Feature)
	{
		TestFalse(TEXT("feature-y is not the current branch"), Feature->bIsCurrent);
		TestFalse(TEXT("feature-y is not protected"), Feature->bProtected);
	}

	return true;
}

// The read operations dispatch through the provider and carry their rows on the operation object.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreReadOperationsProviderTest, "LoreSourceControl.Branch.ReadOperationsThroughProvider", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreReadOperationsProviderTest::RunTest(const FString& Parameters)
{
	using namespace LoreSourceControlUtils;

	FLoreTestFixture Fixture(*this, TEXT("LoreReadOps"), TEXT("rdo"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// Two commits: the first adds two files, the second edits one, so the delta of each revision is known.
	const FString Alpha = Fixture.Root / TEXT("alpha.txt");
	const FString Beta = Fixture.Root / TEXT("beta.txt");
	FFileHelper::SaveStringToFile(TEXT("alpha one"), *Alpha);
	FFileHelper::SaveStringToFile(TEXT("beta one"), *Beta);
	TArray<FString> Errors;
	TestTrue(TEXT("Both files stage"), StageFiles(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, { TEXT("alpha.txt"), TEXT("beta.txt") }, Errors));
	TestTrue(TEXT("First commit lands"), CommitChanges(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, TEXT("test@example.com"), TEXT("add alpha and beta"), Errors));
	TestTrue(TEXT("First push lands"), PushBranch(ELoreBackendMode::Automatic, Fixture.Binary, Fixture.Root, Errors));
	FFileHelper::SaveStringToFile(TEXT("alpha two"), *Alpha);
	TestTrue(TEXT("Second commit lands"), StageCommitPush(Fixture, TEXT("alpha.txt"), TEXT("edit alpha")));

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	// GetBranches enumerates main as the current branch.
	const TArray<FString> NoFiles;
	TSharedRef<FLoreGetBranches, ESPMode::ThreadSafe> BranchesOp = ISourceControlOperation::Create<FLoreGetBranches>();
	const ECommandResult::Type BranchesResult = Provider.Execute(BranchesOp, NoFiles, EConcurrency::Synchronous);
	TestEqual(TEXT("GetBranches through the provider succeeds"), (int32)BranchesResult, (int32)ECommandResult::Succeeded);
	const FLoreBranchItem* Main = BranchesOp->Branches.FindByPredicate([](const FLoreBranchItem& B) { return B.Name == TEXT("main"); });
	TestNotNull(TEXT("GetBranches lists main"), Main);
	if (Main)
	{
		TestTrue(TEXT("GetBranches flags main current"), Main->bIsCurrent);
	}

	// GetRevisions returns the two fixture commits newest first.
	TSharedRef<FLoreGetRevisions, ESPMode::ThreadSafe> RevisionsOp = ISourceControlOperation::Create<FLoreGetRevisions>();
	const ECommandResult::Type RevisionsResult = Provider.Execute(RevisionsOp, NoFiles, EConcurrency::Synchronous);
	TestEqual(TEXT("GetRevisions through the provider succeeds"), (int32)RevisionsResult, (int32)ECommandResult::Succeeded);
	TestEqual(TEXT("GetRevisions returns two revisions"), RevisionsOp->Revisions.Num(), 2);
	if (RevisionsOp->Revisions.Num() == 2)
	{
		TestEqual(TEXT("GetRevisions is newest first"), RevisionsOp->Revisions[0].RevisionNumber, 2);
		TestEqual(TEXT("GetRevisions oldest carries the first message"), RevisionsOp->Revisions[1].Message, FString(TEXT("add alpha and beta")));

		// GetRevisionFiles on the newest revision names exactly the edited file, filtered to files.
		TSharedRef<FLoreGetRevisionFiles, ESPMode::ThreadSafe> FilesOp = ISourceControlOperation::Create<FLoreGetRevisionFiles>();
		FilesOp->Revision = RevisionsOp->Revisions[0].Hash;
		const ECommandResult::Type FilesResult = Provider.Execute(FilesOp, NoFiles, EConcurrency::Synchronous);
		TestEqual(TEXT("GetRevisionFiles through the provider succeeds"), (int32)FilesResult, (int32)ECommandResult::Succeeded);
		TArray<FString> Changed;
		FString ChangedAction;
		for (const FLoreRevisionDeltaEntry& Delta : FilesOp->Details.Deltas)
		{
			if (Delta.bIsFile)
			{
				Changed.Add(Delta.Path);
				ChangedAction = Delta.Action;
			}
		}
		TestEqual(TEXT("GetRevisionFiles names exactly one changed file"), Changed.Num(), 1);
		if (Changed.Num() == 1)
		{
			TestEqual(TEXT("GetRevisionFiles names the edited file"), Changed[0], FString(TEXT("alpha.txt")));
			TestEqual(TEXT("An edit reads action keep with modify"), ChangedAction, FString(TEXT("keep")));
		}
	}

	return true;
}

// Breadth: Plugin store --------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLorePluginStoreRoundTripTest, "LoreSourceControl.Store.JsonRoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLorePluginStoreRoundTripTest::RunTest(const FString& Parameters)
{
	// A throwaway working-tree root; the store is local, so no server is needed.
	const FString Root = FPaths::Combine(FString(FPlatformProcess::UserTempDir()), TEXT("LoreStoreTest"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Root, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };

	const FString Path = LorePluginStore::GetStorePath(Root, TEXT("changelists/test.json"));

	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("title"), TEXT("Work in progress"));
	Object->SetNumberField(TEXT("version"), 1);
	TArray<TSharedPtr<FJsonValue>> Files;
	Files.Add(MakeShared<FJsonValueString>(TEXT("Content/A.uasset")));
	Object->SetArrayField(TEXT("files"), Files);

	TestTrue(TEXT("Atomic write succeeds"), LorePluginStore::WriteJsonObjectAtomic(Path, Object));
	TestTrue(TEXT("Store file lands under the plugin store"), IFileManager::Get().FileExists(*Path) && Path.Contains(TEXT("ueplugin")));

	TSharedPtr<FJsonObject> ReadBack;
	TestTrue(TEXT("Read back succeeds"), LorePluginStore::ReadJsonObject(Path, ReadBack));
	if (ReadBack.IsValid())
	{
		TestEqual(TEXT("Title round-trips"), ReadBack->GetStringField(TEXT("title")), FString(TEXT("Work in progress")));
		TestEqual(TEXT("File list round-trips"), ReadBack->GetArrayField(TEXT("files")).Num(), 1);
	}

	// A second write replaces the first in one step.
	Object->SetStringField(TEXT("title"), TEXT("Updated"));
	TestTrue(TEXT("Second atomic write succeeds"), LorePluginStore::WriteJsonObjectAtomic(Path, Object));
	TSharedPtr<FJsonObject> ReadBack2;
	LorePluginStore::ReadJsonObject(Path, ReadBack2);
	TestEqual(TEXT("Updated title round-trips"), ReadBack2.IsValid() ? ReadBack2->GetStringField(TEXT("title")) : FString(), FString(TEXT("Updated")));

	// No temporary files survive an atomic write.
	TArray<FString> Leftover;
	IFileManager::Get().FindFiles(Leftover, *(LorePluginStore::GetStorePath(Root, TEXT("changelists")) / TEXT("*.tmp")), true, false);
	TestEqual(TEXT("No temporary files remain after atomic writes"), Leftover.Num(), 0);

	// Reading an absent file fails rather than asserting.
	TSharedPtr<FJsonObject> Missing;
	TestFalse(TEXT("Reading an absent store file returns false"), LorePluginStore::ReadJsonObject(LorePluginStore::GetStorePath(Root, TEXT("changelists/absent.json")), Missing));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreChangelistStoreRoundTripTest, "LoreSourceControl.Store.ChangelistRoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreChangelistStoreRoundTripTest::RunTest(const FString& Parameters)
{
	const FString Root = FPaths::Combine(FString(FPlatformProcess::UserTempDir()), TEXT("LoreChangelistTest"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Root, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };

	// With no store file, Load yields only the mandatory Default changelist.
	TArray<FLoreChangelistRecord> Initial = LoreChangelists::Load(Root);
	TestEqual(TEXT("A fresh tree has one changelist"), Initial.Num(), 1);
	TestTrue(TEXT("The sole changelist is Default"), Initial.Num() == 1 && Initial[0].IsDefault());

	// Add a named changelist and persist.
	FLoreChangelistRecord Feature;
	Feature.Id = TEXT("cl-1");
	Feature.Title = TEXT("Lighting pass");
	Feature.Description = TEXT("Rebake the hero level lighting");
	Feature.Author = TEXT("test@example.com");
	Feature.Files = { TEXT("Content/Maps/Hero.umap"), TEXT("Content/Light.uasset") };
	Initial.Add(Feature);
	TestTrue(TEXT("Save succeeds"), LoreChangelists::Save(Root, Initial));

	// Reload and assert the round-trip.
	TArray<FLoreChangelistRecord> Reloaded = LoreChangelists::Load(Root);
	TestEqual(TEXT("Reload returns Default plus the named changelist"), Reloaded.Num(), 2);
	const FLoreChangelistRecord* Found = Reloaded.FindByPredicate([](const FLoreChangelistRecord& R) { return R.Id == TEXT("cl-1"); });
	TestNotNull(TEXT("The named changelist reloads"), Found);
	if (Found)
	{
		TestEqual(TEXT("Title round-trips"), Found->Title, FString(TEXT("Lighting pass")));
		TestEqual(TEXT("Description round-trips"), Found->Description, FString(TEXT("Rebake the hero level lighting")));
		TestEqual(TEXT("File list round-trips"), Found->Files.Num(), 2);
		TestFalse(TEXT("The named changelist is not Default"), Found->IsDefault());
	}
	TestTrue(TEXT("Default remains and is first"), Reloaded.Num() >= 1 && Reloaded[0].IsDefault());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreChangelistReconcileTest, "LoreSourceControl.Store.ChangelistReconcile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreChangelistReconcileTest::RunTest(const FString& Parameters)
{
	auto MakeSet = []()
	{
		TArray<FLoreChangelistRecord> Records;
		FLoreChangelistRecord Default; Default.Id = LoreChangelists::DefaultId; Default.Title = TEXT("Default");
		FLoreChangelistRecord Feature; Feature.Id = TEXT("cl-1"); Feature.Title = TEXT("Feature"); Feature.Files = { TEXT("A.uasset") };
		Records.Add(Default);
		Records.Add(Feature);
		return Records;
	};

	auto FilesOf = [](const TArray<FLoreChangelistRecord>& Records, const FString& Id) -> TArray<FString>
	{
		const FLoreChangelistRecord* R = Records.FindByPredicate([&Id](const FLoreChangelistRecord& X) { return X.Id == Id; });
		return R ? R->Files : TArray<FString>();
	};

	// A new changed file joins Default; an already-assigned file stays in its changelist and is not duplicated.
	{
		TArray<FLoreChangelistRecord> Records = MakeSet();
		LoreChangelists::Reconcile(Records, { TEXT("A.uasset"), TEXT("B.uasset") }, TMap<FString, FString>());
		TestTrue(TEXT("Assigned file stays in its changelist"), FilesOf(Records, TEXT("cl-1")).Contains(TEXT("A.uasset")));
		TestTrue(TEXT("Unassigned changed file joins Default"), FilesOf(Records, LoreChangelists::DefaultId).Contains(TEXT("B.uasset")));
		TestFalse(TEXT("Assigned file is not duplicated into Default"), FilesOf(Records, LoreChangelists::DefaultId).Contains(TEXT("A.uasset")));
	}

	// A file no longer changed is dropped, and its now-empty non-Default changelist survives.
	{
		TArray<FLoreChangelistRecord> Records = MakeSet();
		LoreChangelists::Reconcile(Records, { TEXT("B.uasset") }, TMap<FString, FString>());
		TestFalse(TEXT("Unchanged file is dropped from its changelist"), FilesOf(Records, TEXT("cl-1")).Contains(TEXT("A.uasset")));
		TestTrue(TEXT("The now-empty changelist survives"), Records.ContainsByPredicate([](const FLoreChangelistRecord& R) { return R.Id == TEXT("cl-1"); }));
	}

	// A moved file keeps its changelist under the new path.
	{
		TArray<FLoreChangelistRecord> Records = MakeSet();
		TMap<FString, FString> Moves;
		Moves.Add(TEXT("A.uasset"), TEXT("C.uasset"));
		LoreChangelists::Reconcile(Records, { TEXT("C.uasset") }, Moves);
		TestTrue(TEXT("Moved file follows its new path"), FilesOf(Records, TEXT("cl-1")).Contains(TEXT("C.uasset")));
		TestFalse(TEXT("Old path is gone from the changelist"), FilesOf(Records, TEXT("cl-1")).Contains(TEXT("A.uasset")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreChangelistConcurrentMutateTest, "LoreSourceControl.Store.ChangelistConcurrentMutate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreChangelistConcurrentMutateTest::RunTest(const FString& Parameters)
{
	const FString Root = FPaths::Combine(FString(FPlatformProcess::UserTempDir()), TEXT("LoreChangelistConcurrent"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Root, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };

	// Seed a named changelist for the pool task to edit while this thread edits Default.
	TestTrue(TEXT("Seed mutate succeeds"), LoreChangelists::Mutate(Root, [](TArray<FLoreChangelistRecord>& Records)
	{
		FLoreChangelistRecord Feature;
		Feature.Id = TEXT("cl-1");
		Feature.Title = TEXT("Feature");
		Records.Add(Feature);
		return true;
	}));

	// Interleave a few hundred load-edit-save cycles from a pool thread and this thread, the changelist-worker-versus-GetState race. Every edit must survive; an unguarded read-modify-write loses whichever save lands in between.
	constexpr int32 EditsPerSide = 200;
	TFuture<void> PoolEdits = Async(EAsyncExecution::ThreadPool, [&Root, EditsPerSide]()
	{
		for (int32 Index = 0; Index < EditsPerSide; ++Index)
		{
			LoreChangelists::Mutate(Root, [Index](TArray<FLoreChangelistRecord>& Records)
			{
				Records.FindByPredicate([](const FLoreChangelistRecord& R) { return R.Id == TEXT("cl-1"); })->Files.Add(FString::Printf(TEXT("Pool/%d.uasset"), Index));
				return true;
			});
		}
	});
	for (int32 Index = 0; Index < EditsPerSide; ++Index)
	{
		LoreChangelists::Mutate(Root, [Index](TArray<FLoreChangelistRecord>& Records)
		{
			Records.FindByPredicate([](const FLoreChangelistRecord& R) { return R.IsDefault(); })->Files.Add(FString::Printf(TEXT("Game/%d.uasset"), Index));
			return true;
		});
	}
	PoolEdits.Wait();

	const TArray<FLoreChangelistRecord> Records = LoreChangelists::Load(Root);
	const FLoreChangelistRecord* Default = Records.FindByPredicate([](const FLoreChangelistRecord& R) { return R.IsDefault(); });
	const FLoreChangelistRecord* Feature = Records.FindByPredicate([](const FLoreChangelistRecord& R) { return R.Id == TEXT("cl-1"); });
	TestEqual(TEXT("No game-thread edit is lost"), Default ? Default->Files.Num() : 0, EditsPerSide);
	TestEqual(TEXT("No pool-thread edit is lost"), Feature ? Feature->Files.Num() : 0, EditsPerSide);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreOperationMarkerTest, "LoreSourceControl.Store.OperationMarker", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreOperationMarkerTest::RunTest(const FString& Parameters)
{
	const FString Root = FPaths::Combine(FString(FPlatformProcess::UserTempDir()), TEXT("LoreMarkerTest"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Root, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };

	// With no marker, a read reports no operation in progress.
	TestFalse(TEXT("A fresh tree has no operation marker"), LoreOperationMarker::Read(Root).IsValid());

	// Write a marker for an interrupted branch switch and read it back.
	FLoreOperationMarker Marker;
	Marker.Kind = ELoreOperationKind::BranchSwitch;
	Marker.Instance = TEXT("deadbeef");
	Marker.SourceBranch = TEXT("main");
	Marker.TargetBranch = TEXT("feature-one");
	Marker.SourceRevision = TEXT("aaaa");
	Marker.TargetRevision = TEXT("bbbb");
	Marker.ChangedFiles = { TEXT("Content/A.uasset"), TEXT("Content/B.uasset") };
	TestTrue(TEXT("Marker write succeeds"), LoreOperationMarker::Write(Root, Marker));

	const FLoreOperationMarker ReadBack = LoreOperationMarker::Read(Root);
	TestTrue(TEXT("A written marker reads as a pending operation"), ReadBack.IsValid());
	TestEqual(TEXT("Marker kind round-trips"), (int32)ReadBack.Kind, (int32)ELoreOperationKind::BranchSwitch);
	TestEqual(TEXT("Instance round-trips"), ReadBack.Instance, FString(TEXT("deadbeef")));
	TestEqual(TEXT("Source branch round-trips"), ReadBack.SourceBranch, FString(TEXT("main")));
	TestEqual(TEXT("Target branch round-trips"), ReadBack.TargetBranch, FString(TEXT("feature-one")));
	TestEqual(TEXT("Source revision round-trips"), ReadBack.SourceRevision, FString(TEXT("aaaa")));
	TestEqual(TEXT("Target revision round-trips"), ReadBack.TargetRevision, FString(TEXT("bbbb")));
	TestEqual(TEXT("Changed file set round-trips"), ReadBack.ChangedFiles.Num(), 2);

	// Clearing removes the marker, so a later connect sees no interrupted operation.
	TestTrue(TEXT("Marker clear succeeds"), LoreOperationMarker::Clear(Root));
	TestFalse(TEXT("A cleared marker reads as no operation"), LoreOperationMarker::Read(Root).IsValid());

	// A record written without the branch fields (an older marker) still reads: the added fields default empty.
	FLoreOperationMarker Minimal;
	Minimal.Kind = ELoreOperationKind::RevisionSync;
	Minimal.SourceRevision = TEXT("cccc");
	TestTrue(TEXT("Minimal marker write succeeds"), LoreOperationMarker::Write(Root, Minimal));
	const FLoreOperationMarker MinimalReadBack = LoreOperationMarker::Read(Root);
	TestTrue(TEXT("A marker without branch fields reads"), MinimalReadBack.IsValid());
	TestEqual(TEXT("Minimal kind round-trips"), (int32)MinimalReadBack.Kind, (int32)ELoreOperationKind::RevisionSync);
	TestTrue(TEXT("Absent target branch reads empty"), MinimalReadBack.TargetBranch.IsEmpty());
	LoreOperationMarker::Clear(Root);

	// Clearing an absent marker is a no-op success.
	TestTrue(TEXT("Clearing an absent marker succeeds"), LoreOperationMarker::Clear(Root));

	return true;
}

#if LORE_UE5_3_OR_LATER
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreChangelistClassTest, "LoreSourceControl.Changelist.IdentifierAndState", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreChangelistClassTest::RunTest(const FString& Parameters)
{
	FLoreSourceControlChangelistRef Default = MakeShared<FLoreSourceControlChangelist, ESPMode::ThreadSafe>(FString(LoreChangelists::DefaultId));
	TestTrue(TEXT("Default changelist reports default"), Default->IsDefault());
	TestFalse(TEXT("Default changelist cannot be deleted"), Default->CanDelete());
	TestEqual(TEXT("Default identifier round-trips"), Default->GetIdentifier(), FString(LoreChangelists::DefaultId));

	FLoreSourceControlChangelistRef Named = MakeShared<FLoreSourceControlChangelist, ESPMode::ThreadSafe>(FString(TEXT("cl-1")));
	TestFalse(TEXT("Named changelist is not default"), Named->IsDefault());
	TestTrue(TEXT("Named changelist can be deleted"), Named->CanDelete());

	FLoreSourceControlChangelistStateRef State = MakeShared<FLoreSourceControlChangelistState, ESPMode::ThreadSafe>(Named);
	State->Title = TEXT("Feature work");
	State->Description = TEXT("Work in progress");
	TestEqual(TEXT("Display text is the title"), State->GetDisplayText().ToString(), FString(TEXT("Feature work")));
	TestEqual(TEXT("Description text round-trips"), State->GetDescriptionText().ToString(), FString(TEXT("Work in progress")));
	TestEqual(TEXT("A fresh changelist has no files"), State->GetFilesStates().Num(), 0);
	TestEqual(TEXT("The state's changelist round-trips"), State->GetChangelist()->GetIdentifier(), FString(TEXT("cl-1")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreProviderChangelistsTest, "LoreSourceControl.Changelist.ProviderGetChangelists", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreProviderChangelistsTest::RunTest(const FString& Parameters)
{
	FLoreTestFixture Fixture(*this, TEXT("LoreProviderCL"), TEXT("pcl"));
	if (!Fixture.bReady)
	{
		return false;
	}

	// Seed a named changelist in the store alongside the mandatory Default.
	TArray<FLoreChangelistRecord> Records = LoreChangelists::Load(Fixture.Root);
	FLoreChangelistRecord Feature;
	Feature.Id = TEXT("cl-1");
	Feature.Title = TEXT("Feature");
	Records.Add(Feature);
	TestTrue(TEXT("Seed save succeeds"), LoreChangelists::Save(Fixture.Root, Records));

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	const TArray<FSourceControlChangelistRef> Changelists = Provider.GetChangelists(EStateCacheUsage::Use);
	bool bHasDefault = false;
	bool bHasFeature = false;
	for (const FSourceControlChangelistRef& Changelist : Changelists)
	{
		const FString Id = Changelist->GetIdentifier();
		bHasDefault |= (Id == LoreChangelists::DefaultId);
		bHasFeature |= (Id == TEXT("cl-1"));
	}
	TestTrue(TEXT("The provider lists the Default changelist"), bHasDefault);
	TestTrue(TEXT("The provider lists the seeded changelist"), bHasFeature);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreChangelistWorkersTest, "LoreSourceControl.Changelist.WorkerRoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreChangelistWorkersTest::RunTest(const FString& Parameters)
{
	// The changelist workers edit only the plugin store, so a bare temporary root stands in for a working tree and no server runs.
	const FString Root = FPaths::Combine(FString(FPlatformProcess::UserTempDir()), TEXT("LoreChangelistWorkers"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Root, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };

	ISourceControlModule& SourceControl = ISourceControlModule::Get();
	const FName Previous = SourceControl.GetProvider().GetName();
	SourceControl.SetProvider(FName("Lore"));
	ON_SCOPE_EXIT { SourceControl.SetProvider(Previous); };
	FLoreSourceControlProvider& LoreProvider = FLoreSourceControlModule::Get().GetProvider();
	LoreProvider.Init();
	LoreProvider.SetRepositoryInfo(true, true, Root, FString(), TEXT("test@example.com"), TEXT("main"), FString(), TEXT("test"));
	ISourceControlProvider& Provider = SourceControl.GetProvider();

	auto RecordOf = [&Root](const FString& Id) -> FLoreChangelistRecord
	{
		const TArray<FLoreChangelistRecord> Records = LoreChangelists::Load(Root);
		const FLoreChangelistRecord* Found = Records.FindByPredicate([&Id](const FLoreChangelistRecord& R) { return R.Id == Id; });
		return Found ? *Found : FLoreChangelistRecord();
	};
	auto ProviderLists = [&Provider](const FString& Id) -> bool
	{
		for (const FSourceControlChangelistRef& Changelist : Provider.GetChangelists(EStateCacheUsage::Use))
		{
			if (Changelist->GetIdentifier() == Id)
			{
				return true;
			}
		}
		return false;
	};

	// New changelist: the worker mints a record carrying the description and hands the identifier back on the operation.
	TSharedRef<FNewChangelist, ESPMode::ThreadSafe> NewOperation = ISourceControlOperation::Create<FNewChangelist>();
	NewOperation->SetDescription(FText::FromString(TEXT("Lighting pass\nRebake the hero level")));
	TestEqual(TEXT("NewChangelist through the provider succeeds"), (int32)Provider.Execute(NewOperation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);
	const FSourceControlChangelistPtr Created = NewOperation->GetNewChangelist();
	TestTrue(TEXT("NewChangelist hands back the created changelist"), Created.IsValid());
	if (!Created.IsValid())
	{
		return false;
	}
	const FString CreatedId = Created->GetIdentifier();
	{
		const FLoreChangelistRecord Record = RecordOf(CreatedId);
		TestEqual(TEXT("The created record carries the description"), Record.Description, FString(TEXT("Lighting pass\nRebake the hero level")));
		TestEqual(TEXT("The title is the description's first line"), Record.Title, FString(TEXT("Lighting pass")));
		// The command snapshots the settings identity override when one is saved on this machine, else the provider identity.
		FString ExpectedIdentity = FLoreSourceControlModule::Get().GetSettings().GetIdentity();
		if (ExpectedIdentity.IsEmpty())
		{
			ExpectedIdentity = TEXT("test@example.com");
		}
		TestEqual(TEXT("The author is the command identity"), Record.Author, ExpectedIdentity);
	}
	TestTrue(TEXT("GetChangelists lists the created changelist"), ProviderLists(CreatedId));

	// Edit rewrites the description, and the title follows its first line.
	TSharedRef<FEditChangelist, ESPMode::ThreadSafe> EditOperation = ISourceControlOperation::Create<FEditChangelist>();
	EditOperation->SetDescription(FText::FromString(TEXT("Lighting and fog pass")));
	TestEqual(TEXT("EditChangelist through the provider succeeds"), (int32)Provider.Execute(EditOperation, Created, TArray<FString>(), EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);
	TestEqual(TEXT("Edit rewrites the description"), RecordOf(CreatedId).Description, FString(TEXT("Lighting and fog pass")));
	TestEqual(TEXT("Edit rewrites the title"), RecordOf(CreatedId).Title, FString(TEXT("Lighting and fog pass")));

	// Move-to: seed Default with two files, then move one into the created changelist and back.
	{
		TArray<FLoreChangelistRecord> Records = LoreChangelists::Load(Root);
		FLoreChangelistRecord* Default = Records.FindByPredicate([](const FLoreChangelistRecord& R) { return R.IsDefault(); });
		Default->Files = { TEXT("Content/A.uasset"), TEXT("Content/B.uasset") };
		TestTrue(TEXT("Seeding Default's files succeeds"), LoreChangelists::Save(Root, Records));
	}
	const FString FileA = Root / TEXT("Content/A.uasset");
	TestEqual(TEXT("MoveToChangelist out of Default succeeds"), (int32)Provider.Execute(ISourceControlOperation::Create<FMoveToChangelist>(), Created, TArray<FString>{ FileA }, EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);
	TestTrue(TEXT("The moved file joins the destination"), RecordOf(CreatedId).Files.Contains(TEXT("Content/A.uasset")));
	TestFalse(TEXT("The moved file leaves Default"), RecordOf(LoreChangelists::DefaultId).Files.Contains(TEXT("Content/A.uasset")));
	TestTrue(TEXT("The unmoved file stays in Default"), RecordOf(LoreChangelists::DefaultId).Files.Contains(TEXT("Content/B.uasset")));

	FSourceControlChangelistPtr DefaultChangelist;
	for (const FSourceControlChangelistRef& Changelist : Provider.GetChangelists(EStateCacheUsage::Use))
	{
		if (Changelist->IsDefault())
		{
			DefaultChangelist = Changelist;
		}
	}
	TestTrue(TEXT("GetChangelists yields the Default changelist"), DefaultChangelist.IsValid());
	TestEqual(TEXT("MoveToChangelist back into Default succeeds"), (int32)Provider.Execute(ISourceControlOperation::Create<FMoveToChangelist>(), DefaultChangelist, TArray<FString>{ FileA }, EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);
	TestTrue(TEXT("The file returns to Default"), RecordOf(LoreChangelists::DefaultId).Files.Contains(TEXT("Content/A.uasset")));
	TestFalse(TEXT("The file leaves the named changelist"), RecordOf(CreatedId).Files.Contains(TEXT("Content/A.uasset")));

	// New changelist with files carries them out of their previous changelist, the editor's move-to-new-changelist action.
	TSharedRef<FNewChangelist, ESPMode::ThreadSafe> CarryOperation = ISourceControlOperation::Create<FNewChangelist>();
	CarryOperation->SetDescription(FText::FromString(TEXT("Carved out")));
	TestEqual(TEXT("NewChangelist with files succeeds"), (int32)Provider.Execute(CarryOperation, FSourceControlChangelistPtr(), TArray<FString>{ FileA }, EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);
	const FString CarryId = CarryOperation->GetNewChangelist().IsValid() ? CarryOperation->GetNewChangelist()->GetIdentifier() : FString();
	TestTrue(TEXT("The carried file joins the new changelist"), RecordOf(CarryId).Files.Contains(TEXT("Content/A.uasset")));
	TestFalse(TEXT("The carried file leaves Default"), RecordOf(LoreChangelists::DefaultId).Files.Contains(TEXT("Content/A.uasset")));

	// Update pending changelists status succeeds; the live reconciliation lands with the GetState changelist surface.
	TSharedRef<FUpdatePendingChangelistsStatus, ESPMode::ThreadSafe> UpdateOperation = ISourceControlOperation::Create<FUpdatePendingChangelistsStatus>();
	UpdateOperation->SetUpdateAllChangelists(true);
	TestEqual(TEXT("UpdateChangelistsStatus through the provider succeeds"), (int32)Provider.Execute(UpdateOperation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);

	// Delete removes the named changelist and refuses the Default changelist.
	TestEqual(TEXT("DeleteChangelist through the provider succeeds"), (int32)Provider.Execute(ISourceControlOperation::Create<FDeleteChangelist>(), Created, TArray<FString>(), EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);
	TestFalse(TEXT("The deleted changelist is gone from the store"), LoreChangelists::Load(Root).ContainsByPredicate([&CreatedId](const FLoreChangelistRecord& R) { return R.Id == CreatedId; }));
	TestFalse(TEXT("GetChangelists no longer lists the deleted changelist"), ProviderLists(CreatedId));

	TestEqual(TEXT("Deleting the Default changelist is refused"), (int32)Provider.Execute(ISourceControlOperation::Create<FDeleteChangelist>(), DefaultChangelist, TArray<FString>(), EConcurrency::Synchronous), (int32)ECommandResult::Failed);
	TestTrue(TEXT("The Default changelist survives the refused delete"), ProviderLists(LoreChangelists::DefaultId));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreChangelistGetStateTest, "LoreSourceControl.Changelist.GetStateReconciles", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreChangelistGetStateTest::RunTest(const FString& Parameters)
{
	// GetState reconciles membership against the provider's cached file states, so a bare temporary root and a seeded cache stand in for a working tree and no server runs.
	const FString Root = FPaths::Combine(FString(FPlatformProcess::UserTempDir()), TEXT("LoreChangelistGetState"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Root, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };

	ISourceControlModule& SourceControl = ISourceControlModule::Get();
	const FName Previous = SourceControl.GetProvider().GetName();
	SourceControl.SetProvider(FName("Lore"));
	ON_SCOPE_EXIT { SourceControl.SetProvider(Previous); };
	FLoreSourceControlProvider& LoreProvider = FLoreSourceControlModule::Get().GetProvider();
	LoreProvider.Init();
	LoreProvider.SetRepositoryInfo(true, true, Root, FString(), TEXT("test@example.com"), TEXT("main"), FString(), TEXT("test"));
	ISourceControlProvider& Provider = SourceControl.GetProvider();

	// Seed a named changelist holding A, so the reconciliation must keep the explicit assignment.
	{
		TArray<FLoreChangelistRecord> Records = LoreChangelists::Load(Root);
		FLoreChangelistRecord Feature;
		Feature.Id = TEXT("cl-1");
		Feature.Title = TEXT("Feature");
		Feature.Files = { TEXT("Content/A.uasset") };
		Records.Add(Feature);
		TestTrue(TEXT("Seed save succeeds"), LoreChangelists::Save(Root, Records));
	}

	// Seed the state cache: A and B changed, C unchanged.
	const FString FileA = FPaths::ConvertRelativePathToFull(Root / TEXT("Content/A.uasset"));
	const FString FileB = FPaths::ConvertRelativePathToFull(Root / TEXT("Content/B.uasset"));
	const FString FileC = FPaths::ConvertRelativePathToFull(Root / TEXT("Content/C.uasset"));
	{
		TArray<FLoreSourceControlState> Seeded;
		FLoreSourceControlState StateA(FileA);
		StateA.WorkingCopyState = ELoreWorkingCopyState::Modified;
		FLoreSourceControlState StateB(FileB);
		StateB.WorkingCopyState = ELoreWorkingCopyState::Modified;
		FLoreSourceControlState StateC(FileC);
		StateC.WorkingCopyState = ELoreWorkingCopyState::Unchanged;
		Seeded.Add(StateA);
		Seeded.Add(StateB);
		Seeded.Add(StateC);
		LoreSourceControlUtils::UpdateCachedStates(Seeded);
	}

	TArray<FSourceControlChangelistStateRef> States;
	TestEqual(TEXT("GetState over the changelists succeeds"), (int32)Provider.GetState(Provider.GetChangelists(EStateCacheUsage::Use), States, EStateCacheUsage::Use), (int32)ECommandResult::Succeeded);

	auto FilesOf = [&States](const FString& Id) -> TArray<FString>
	{
		TArray<FString> Files;
		for (const FSourceControlChangelistStateRef& State : States)
		{
			if (State->GetChangelist()->GetIdentifier() == Id)
			{
				for (const FSourceControlStateRef& File : State->GetFilesStates())
				{
					Files.Add(File->GetFilename());
				}
			}
		}
		return Files;
	};

	TestTrue(TEXT("The explicit assignment stays in its changelist"), FilesOf(TEXT("cl-1")).Contains(FileA));
	TestTrue(TEXT("The unassigned changed file joins Default"), FilesOf(LoreChangelists::DefaultId).Contains(FileB));
	TestFalse(TEXT("The assigned file is not duplicated into Default"), FilesOf(LoreChangelists::DefaultId).Contains(FileA));
	TestFalse(TEXT("An unchanged file joins no changelist"), FilesOf(LoreChangelists::DefaultId).Contains(FileC) || FilesOf(TEXT("cl-1")).Contains(FileC));

	// The member file states resolve to the cached live states, so the Changes view shows real status.
	for (const FSourceControlChangelistStateRef& State : States)
	{
		if (State->GetChangelist()->GetIdentifier() == FString(LoreChangelists::DefaultId))
		{
			for (const FSourceControlStateRef& File : State->GetFilesStates())
			{
				TestTrue(TEXT("A member file state resolves to the cached modified state"), File->IsModified());
			}
		}
	}

	// The reconciled membership persists, so a changelist-keyed submit expands to the set the view showed.
	const TArray<FLoreChangelistRecord> Persisted = LoreChangelists::Load(Root);
	const FLoreChangelistRecord* PersistedDefault = Persisted.FindByPredicate([](const FLoreChangelistRecord& R) { return R.IsDefault(); });
	TestTrue(TEXT("Default's reconciled membership persists to the store"), PersistedDefault && PersistedDefault->Files.Contains(TEXT("Content/B.uasset")));

	// A changelist submit with no member files is refused rather than committing the whole staged set.
	{
		TArray<FLoreChangelistRecord> Records = LoreChangelists::Load(Root);
		FLoreChangelistRecord Empty;
		Empty.Id = TEXT("cl-empty");
		Empty.Title = TEXT("Empty");
		Records.Add(Empty);
		TestTrue(TEXT("Empty changelist seed saves"), LoreChangelists::Save(Root, Records));
	}
	FSourceControlChangelistPtr EmptyChangelist;
	for (const FSourceControlChangelistRef& Changelist : Provider.GetChangelists(EStateCacheUsage::Use))
	{
		if (Changelist->GetIdentifier() == TEXT("cl-empty"))
		{
			EmptyChangelist = Changelist;
		}
	}
	TestTrue(TEXT("The empty changelist is listed"), EmptyChangelist.IsValid());
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> EmptySubmit = ISourceControlOperation::Create<FCheckIn>();
	EmptySubmit->SetDescription(FText::FromString(TEXT("empty changelist submit")));
	TestEqual(TEXT("Submitting an empty changelist is refused"), (int32)Provider.Execute(EmptySubmit, EmptyChangelist, TArray<FString>(), EConcurrency::Synchronous), (int32)ECommandResult::Failed);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreChangelistSubmitTest, "LoreSourceControl.Changelist.SubmitMemberFiles", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreChangelistSubmitTest::RunTest(const FString& Parameters)
{
	FLoreTestFixture Fixture(*this, TEXT("LoreChangelistSubmit"), TEXT("clsub"));
	if (!Fixture.bReady)
	{
		return false;
	}

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	const FString FileMember = FPaths::ConvertRelativePathToFull(Fixture.Root / TEXT("member.txt"));
	const FString FileOutsider = FPaths::ConvertRelativePathToFull(Fixture.Root / TEXT("outsider.txt"));
	FFileHelper::SaveStringToFile(TEXT("member"), *FileMember);
	FFileHelper::SaveStringToFile(TEXT("outsider"), *FileOutsider);
	TestEqual(TEXT("MarkForAdd stages both files"), (int32)Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), TArray<FString>{ FileMember, FileOutsider }, EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);

	// Carve the member file into its own changelist; the outsider stays for Default.
	TSharedRef<FNewChangelist, ESPMode::ThreadSafe> NewOperation = ISourceControlOperation::Create<FNewChangelist>();
	NewOperation->SetDescription(FText::FromString(TEXT("Member submit")));
	TestEqual(TEXT("NewChangelist succeeds"), (int32)Provider.Execute(NewOperation, FSourceControlChangelistPtr(), TArray<FString>{ FileMember }, EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);
	const FSourceControlChangelistPtr Created = NewOperation->GetNewChangelist();
	if (!Created.IsValid())
	{
		AddError(TEXT("NewChangelist returned no changelist."));
		return false;
	}
	const FString CreatedId = Created->GetIdentifier();

	// A render pass, so the reconciliation collects the outsider into Default and persists the membership the submit will expand.
	TArray<FSourceControlChangelistStateRef> States;
	TestEqual(TEXT("GetState over the changelists succeeds"), (int32)Provider.GetState(Provider.GetChangelists(EStateCacheUsage::Use), States, EStateCacheUsage::Use), (int32)ECommandResult::Succeeded);
	for (const FSourceControlChangelistStateRef& State : States)
	{
		if (State->GetChangelist()->GetIdentifier() == CreatedId)
		{
			TestEqual(TEXT("The named changelist holds exactly the member file"), State->GetFilesStates().Num(), 1);
		}
	}
	{
		const TArray<FLoreChangelistRecord> Reconciled = LoreChangelists::Load(Fixture.Root);
		const FLoreChangelistRecord* Default = Reconciled.FindByPredicate([](const FLoreChangelistRecord& R) { return R.IsDefault(); });
		TestTrue(TEXT("The outsider is collected into Default"), Default && Default->Files.Contains(TEXT("outsider.txt")));
	}

	// Submit the changelist by identifier with no file list, the Changes view path.
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> Submit = ISourceControlOperation::Create<FCheckIn>();
	Submit->SetDescription(FText::FromString(TEXT("changelist member submit")));
	TestEqual(TEXT("Changelist-keyed submit succeeds"), (int32)Provider.Execute(Submit, Created, TArray<FString>(), EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);
	TestFalse(TEXT("Changelist submit sets a non-empty success message"), Submit->GetSuccessMessage().IsEmpty());

	// Exactly the member file committed: it reads source controlled while the outsider stays uncommitted.
	TArray<FSourceControlStateRef> FileStates;
	TestEqual(TEXT("Status refresh after the submit succeeds"), (int32)Provider.GetState(TArray<FString>{ FileMember, FileOutsider }, FileStates, EStateCacheUsage::ForceUpdate), (int32)ECommandResult::Succeeded);
	for (const FSourceControlStateRef& State : FileStates)
	{
		if (State->GetFilename() == FileMember)
		{
			TestTrue(TEXT("The member file is committed"), State->IsSourceControlled() && !State->IsAdded());
		}
		if (State->GetFilename() == FileOutsider)
		{
			TestFalse(TEXT("The outsider file is not committed"), State->IsSourceControlled());
		}
	}

	// The submitted record empties and goes away, the reference provider's implicit delete of a submitted named changelist. Default's membership is not asserted here: the partial submit unstages the outsider, so it reads not controlled and a later reconciliation drops it from Default.
	const TArray<FLoreChangelistRecord> Records = LoreChangelists::Load(Fixture.Root);
	TestFalse(TEXT("The submitted changelist record is gone"), Records.ContainsByPredicate([&CreatedId](const FLoreChangelistRecord& R) { return R.Id == CreatedId; }));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreChangelistSubmitExpandsAtIssueTest, "LoreSourceControl.Changelist.SubmitExpandsAtIssue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoreChangelistSubmitExpandsAtIssueTest::RunTest(const FString& Parameters)
{
	FLoreTestFixture Fixture(*this, TEXT("LoreChangelistExpand"), TEXT("clexp"));
	if (!Fixture.bReady)
	{
		return false;
	}

	FName Previous;
	ISourceControlProvider& Provider = BeginProviderOn(Fixture, Previous);
	ON_SCOPE_EXIT { ISourceControlModule::Get().SetProvider(Previous); };

	const FString FileMember = FPaths::ConvertRelativePathToFull(Fixture.Root / TEXT("member.txt"));
	FFileHelper::SaveStringToFile(TEXT("member"), *FileMember);
	TestEqual(TEXT("MarkForAdd stages the file"), (int32)Provider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), TArray<FString>{ FileMember }, EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);

	TSharedRef<FNewChangelist, ESPMode::ThreadSafe> NewOperation = ISourceControlOperation::Create<FNewChangelist>();
	NewOperation->SetDescription(FText::FromString(TEXT("Expansion site")));
	TestEqual(TEXT("NewChangelist succeeds"), (int32)Provider.Execute(NewOperation, FSourceControlChangelistPtr(), TArray<FString>{ FileMember }, EConcurrency::Synchronous), (int32)ECommandResult::Succeeded);
	const FSourceControlChangelistPtr Created = NewOperation->GetNewChangelist();
	if (!Created.IsValid())
	{
		AddError(TEXT("NewChangelist returned no changelist."));
		return false;
	}

	// Pin the expansion site: issue the changelist-keyed submit while this thread holds the store lock, then empty the membership before releasing it. The command's file list fills at issue time on this thread, so the mid-flight edit cannot change what commits; a worker that re-read the store on the pool thread would serialize behind this mutate and find the record empty.
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> Submit = ISourceControlOperation::Create<FCheckIn>();
	Submit->SetDescription(FText::FromString(TEXT("expansion site submit")));
	bool bDone = false;
	ECommandResult::Type Result = ECommandResult::Failed;
	LoreChangelists::Mutate(Fixture.Root, [&Provider, &Submit, &Created, &bDone, &Result](TArray<FLoreChangelistRecord>& Records)
	{
		Provider.Execute(Submit, Created, TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateLambda([&bDone, &Result](const FSourceControlOperationRef&, ECommandResult::Type InResult)
		{
			bDone = true;
			Result = InResult;
		}));
		FLoreChangelistRecord* Record = Records.FindByPredicate([&Created](const FLoreChangelistRecord& R) { return R.Id == Created->GetIdentifier(); });
		if (Record)
		{
			Record->Files.Empty();
		}
		return true;
	});

	const double StartSeconds = FPlatformTime::Seconds();
	while (!bDone && FPlatformTime::Seconds() - StartSeconds < 120.0)
	{
		Provider.Tick();
		FPlatformProcess::Sleep(0.01f);
	}
	TestTrue(TEXT("The asynchronous submit completes"), bDone);
	TestEqual(TEXT("The submit succeeds on the issue-time expansion"), (int32)Result, (int32)ECommandResult::Succeeded);

	// The member file committed even though its record emptied mid-flight, so the expansion ran at issue time.
	TArray<FSourceControlStateRef> FileStates;
	TestEqual(TEXT("Status refresh succeeds"), (int32)Provider.GetState(TArray<FString>{ FileMember }, FileStates, EStateCacheUsage::ForceUpdate), (int32)ECommandResult::Succeeded);
	for (const FSourceControlStateRef& State : FileStates)
	{
		TestTrue(TEXT("The member file is committed"), State->IsSourceControlled() && !State->IsAdded());
	}

	return true;
}
#endif // LORE_UE5_3_OR_LATER

#endif // WITH_DEV_AUTOMATION_TESTS
