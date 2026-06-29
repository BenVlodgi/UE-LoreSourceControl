// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreClient.h"

#if LORE_WITH_LIBLORE

#include "Misc/Paths.h"
#include "LoreCApi.h"

namespace
{
	// liblore strings are UTF-8 with an explicit length and no required NUL terminator, so copy to a terminated buffer before decoding.
	FString StringFromLore(const lore_string_t& In)
	{
		if (!In.string || In.length == 0)
		{
			return FString();
		}
		TArray<ANSICHAR> Terminated;
		Terminated.Append(In.string, static_cast<int32>(In.length));
		Terminated.Add('\0');
		return FString(UTF8_TO_TCHAR(Terminated.GetData()));
	}

	// Hold an FString as a UTF-8 byte buffer for the duration of a liblore call. lore_string_t needs only a pointer and a length, no NUL.
	struct FLoreUtf8
	{
		TArray<uint8> Bytes;

		explicit FLoreUtf8(const FString& In)
		{
			FTCHARToUTF8 Convert(*In);
			Bytes.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
		}

		lore_string_t View() const
		{
			lore_string_t Out;
			Out.string = Bytes.Num() ? reinterpret_cast<const char*>(Bytes.GetData()) : nullptr;
			Out.length = static_cast<uintptr_t>(Bytes.Num());
			return Out;
		}
	};

	const TCHAR* ActionToken(lore_file_action_t InAction)
	{
		switch (InAction)
		{
			case LORE_FILE_ACTION_ADD:    return TEXT("add");
			case LORE_FILE_ACTION_DELETE: return TEXT("delete");
			case LORE_FILE_ACTION_MOVE:   return TEXT("move");
			case LORE_FILE_ACTION_COPY:   return TEXT("copy");
			default:                      return TEXT("keep");
		}
	}

	FString HashToHex(const lore_hash_t& InHash)
	{
		FString Out;
		Out.Reserve(sizeof(InHash.data) * 2);
		for (int32 Index = 0; Index < static_cast<int32>(sizeof(InHash.data)); ++Index)
		{
			Out.Appendf(TEXT("%02x"), InHash.data[Index]);
		}
		return Out;
	}

	FString ContextToHex(const lore_context_t& InContext)
	{
		FString Out;
		Out.Reserve(sizeof(InContext.data) * 2);
		for (int32 Index = 0; Index < static_cast<int32>(sizeof(InContext.data)); ++Index)
		{
			Out.Appendf(TEXT("%02x"), InContext.data[Index]);
		}
		return Out;
	}

	// Canonical Lore content address: the hash hex, a hyphen, then the context hex.
	FString AddressToString(const lore_address_t& InAddress)
	{
		return HashToHex(InAddress.hash) + TEXT("-") + ContextToHex(InAddress.context);
	}

	// OutStorage and OutViews must outlive the returned array, which holds only pointers into them.
	lore_string_array_t BuildAbsolutePaths(const FString& InRoot, const TArray<FString>& InPaths, TArray<FLoreUtf8>& OutStorage, TArray<lore_string_t>& OutViews)
	{
		OutStorage.Reserve(InPaths.Num());
		for (const FString& Path : InPaths)
		{
			OutStorage.Emplace(FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? FPaths::Combine(InRoot, Path) : Path));
		}
		OutViews.Reserve(OutStorage.Num());
		for (const FLoreUtf8& Buffer : OutStorage)
		{
			OutViews.Add(Buffer.View());
		}
		lore_string_array_t Out;
		Out.ptr = OutViews.Num() ? OutViews.GetData() : nullptr;
		Out.count = static_cast<uintptr_t>(OutViews.Num());
		return Out;
	}

	// Terminal and diagnostic events shared by every call; a mutation needs nothing more than these.
	struct FTerminalContext
	{
		bool bError = false;
		FString ErrorMessage;
		int32 CompleteStatus = 0;
		TArray<FString> Diagnostics;
	};

	void TerminalEventCallback(const lore_event_t* Event, uint64_t UserContext)
	{
		FTerminalContext* Context = reinterpret_cast<FTerminalContext*>(static_cast<UPTRINT>(UserContext));
		if (!Event || !Context)
		{
			return;
		}
		switch (Event->tag)
		{
			case LORE_EVENT_ERROR:
				Context->bError = true;
				Context->ErrorMessage = StringFromLore(Event->error.error_inner);
				Context->Diagnostics.Add(Context->ErrorMessage);
				break;
			case LORE_EVENT_LOG:
				if (Event->log.level >= LORE_LOG_LEVEL_WARN)
				{
					Context->Diagnostics.Add(StringFromLore(Event->log.message));
				}
				break;
			case LORE_EVENT_COMPLETE:
				Context->CompleteStatus = Event->complete.status;
				break;
			default:
				break;
		}
	}

	// False with OutError set when a call reported failure through its return code, an error event, or a non-zero completion status.
	bool TerminalSucceeded(const FTerminalContext& Context, int32 Code, const TCHAR* Label, FString& OutError)
	{
		if (Code != 0 || Context.bError || Context.CompleteStatus != 0)
		{
			const FString Detail = Context.Diagnostics.Num() ? FString::Join(Context.Diagnostics, TEXT("; ")) : Context.ErrorMessage;
			OutError = FString::Printf(TEXT("%s failed (return %d, status %d): %s"), Label, Code, Context.CompleteStatus, *Detail);
			return false;
		}
		return true;
	}

	// One synchronous status call's events accumulate here. The library delivers a single call's events one at a time, so no locking is needed.
	struct FStatusAccumulator
	{
		LoreClient::FStatusResult* Result = nullptr;
		bool bError = false;
		FString ErrorMessage;
		int32 CompleteStatus = 0;
		TArray<FString> Diagnostics; // liblore warning and error log text, surfaced when a call fails
	};

	void StatusEventCallback(const lore_event_t* Event, uint64_t UserContext)
	{
		FStatusAccumulator* Accumulator = reinterpret_cast<FStatusAccumulator*>(static_cast<UPTRINT>(UserContext));
		if (!Event || !Accumulator || !Accumulator->Result)
		{
			return;
		}

		switch (Event->tag)
		{
			case LORE_EVENT_REPOSITORY_STATUS_REVISION:
			{
				const lore_repository_status_revision_event_data_t& Revision = Event->repository_status_revision;
				Accumulator->Result->bRemoteAhead = Revision.is_remote_ahead != 0;
				Accumulator->Result->bRemoteAvailable = Revision.remote_available != 0;
				Accumulator->Result->bRemoteAuthorized = Revision.remote_authorized != 0;
				Accumulator->Result->BranchName = StringFromLore(Revision.branch_name);
				break;
			}
			case LORE_EVENT_REPOSITORY_STATUS_FILE:
			{
				const lore_repository_status_file_event_data_t& File = Event->repository_status_file;
				LoreClient::FStatusFile Out;
				Out.Path = StringFromLore(File.path);
				Out.FromPath = StringFromLore(File.from_path);
				Out.Action = ActionToken(File.action);
				Out.bIsDirectory = File.type == LORE_NODE_TYPE_DIRECTORY;
				Out.bStaged = File.flag_staged != 0;
				Out.bDirty = File.flag_dirty != 0;
				Out.bConflict = File.flag_conflict != 0 || File.flag_conflict_unresolved != 0;
				Accumulator->Result->Files.Add(MoveTemp(Out));
				break;
			}
			case LORE_EVENT_ERROR:
			{
				Accumulator->bError = true;
				Accumulator->ErrorMessage = StringFromLore(Event->error.error_inner);
				Accumulator->Diagnostics.Add(Accumulator->ErrorMessage);
				break;
			}
			case LORE_EVENT_LOG:
			{
				if (Event->log.level >= LORE_LOG_LEVEL_WARN)
				{
					Accumulator->Diagnostics.Add(StringFromLore(Event->log.message));
				}
				break;
			}
			case LORE_EVENT_COMPLETE:
			{
				Accumulator->CompleteStatus = Event->complete.status;
				break;
			}
			default:
				break;
		}
	}

	struct FLockAccumulator
	{
		TArray<LoreClient::FLockStatus>* Locks = nullptr;
		bool bError = false;
		FString ErrorMessage;
		int32 CompleteStatus = 0;
		TArray<FString> Diagnostics;
	};

	void LockStatusEventCallback(const lore_event_t* Event, uint64_t UserContext)
	{
		FLockAccumulator* Accumulator = reinterpret_cast<FLockAccumulator*>(static_cast<UPTRINT>(UserContext));
		if (!Event || !Accumulator || !Accumulator->Locks)
		{
			return;
		}

		switch (Event->tag)
		{
			case LORE_EVENT_LOCK_FILE_STATUS:
			{
				const lore_lock_file_status_event_data_t& Lock = Event->lock_file_status;
				LoreClient::FLockStatus Out;
				Out.Path = StringFromLore(Lock.path);
				Out.Owner = StringFromLore(Lock.owner);
				Accumulator->Locks->Add(MoveTemp(Out));
				break;
			}
			case LORE_EVENT_ERROR:
			{
				Accumulator->bError = true;
				Accumulator->ErrorMessage = StringFromLore(Event->error.error_inner);
				Accumulator->Diagnostics.Add(Accumulator->ErrorMessage);
				break;
			}
			case LORE_EVENT_LOG:
			{
				if (Event->log.level >= LORE_LOG_LEVEL_WARN)
				{
					Accumulator->Diagnostics.Add(StringFromLore(Event->log.message));
				}
				break;
			}
			case LORE_EVENT_COMPLETE:
			{
				Accumulator->CompleteStatus = Event->complete.status;
				break;
			}
			default:
				break;
		}
	}

	// One immutable-store query's result accumulates here. The library emits one query event per fragment; only the top-level fragment for the queried address matters, so the first event with the queried-back local result is kept.
	struct FStoreQueryAccumulator
	{
		bool* Available = nullptr;  // set true when any store, local or remote, holds the exact address with its payload
		bool bError = false;
		FString ErrorMessage;
		int32 CompleteStatus = 0;
		TArray<FString> Diagnostics;
	};

	void StoreQueryEventCallback(const lore_event_t* Event, uint64_t UserContext)
	{
		FStoreQueryAccumulator* Accumulator = reinterpret_cast<FStoreQueryAccumulator*>(static_cast<UPTRINT>(UserContext));
		if (!Event || !Accumulator)
		{
			return;
		}

		switch (Event->tag)
		{
			case LORE_EVENT_REPOSITORY_STORE_IMMUTABLE_QUERY:
			{
				const lore_repository_store_immutable_query_event_data_t& Query = Event->repository_store_immutable_query;
				// One event fires per store. A pushed fragment reads payload != 0 only on the remote, since the local store keeps the index but drops the bytes after a push. status 0 is the exact address. The content is recoverable if any store carries the payload.
				if (Accumulator->Available && Query.status == 0 && Query.payload != 0)
				{
					*Accumulator->Available = true;
				}
				break;
			}
			case LORE_EVENT_ERROR:
			{
				Accumulator->bError = true;
				Accumulator->ErrorMessage = StringFromLore(Event->error.error_inner);
				Accumulator->Diagnostics.Add(Accumulator->ErrorMessage);
				break;
			}
			case LORE_EVENT_LOG:
			{
				if (Event->log.level >= LORE_LOG_LEVEL_WARN)
				{
					Accumulator->Diagnostics.Add(StringFromLore(Event->log.message));
				}
				break;
			}
			case LORE_EVENT_COMPLETE:
			{
				Accumulator->CompleteStatus = Event->complete.status;
				break;
			}
			default:
				break;
		}
	}

	// One synchronous file-history call's events accumulate here. FILE_HISTORY starts a revision; any following METADATA attaches to it.
	struct FHistoryAccumulator
	{
		TArray<LoreClient::FHistoryRevision>* Revisions = nullptr;
		bool bError = false;
		FString ErrorMessage;
		int32 CompleteStatus = 0;
		TArray<FString> Diagnostics;
	};

	void HistoryEventCallback(const lore_event_t* Event, uint64_t UserContext)
	{
		FHistoryAccumulator* Accumulator = reinterpret_cast<FHistoryAccumulator*>(static_cast<UPTRINT>(UserContext));
		if (!Event || !Accumulator || !Accumulator->Revisions)
		{
			return;
		}

		switch (Event->tag)
		{
			case LORE_EVENT_FILE_HISTORY:
			{
				const lore_file_history_event_data_t& Entry = Event->file_history;
				LoreClient::FHistoryRevision Out;
				Out.CommitId = HashToHex(Entry.revision);
				Out.RevisionNumber = static_cast<int32>(Entry.revision_number);
				Out.Action = ActionToken(Entry.action);
				Out.FileSize = static_cast<int32>(Entry.size);
				Out.Address = AddressToString(Entry.address);
				Accumulator->Revisions->Add(MoveTemp(Out));
				break;
			}
			case LORE_EVENT_METADATA:
			{
				if (Accumulator->Revisions->Num() == 0)
				{
					break;
				}
				LoreClient::FHistoryRevision& Current = Accumulator->Revisions->Last();
				const lore_metadata_event_data_t& Meta = Event->metadata;
				const FString Key = StringFromLore(Meta.key);
				if (Key == TEXT("timestamp") && Meta.value.tag == LORE_METADATA_NUMERIC)
				{
					Current.Date = FDateTime::FromUnixTimestamp(static_cast<int64>(Meta.value.numeric / 1000));
				}
				else if (Key == TEXT("message") && Meta.value.tag == LORE_METADATA_STRING)
				{
					Current.Description = StringFromLore(Meta.value.string);
				}
				else if ((Key == TEXT("committed-by") || (Key == TEXT("created-by") && Current.UserName.IsEmpty())) && Meta.value.tag == LORE_METADATA_STRING)
				{
					Current.UserName = StringFromLore(Meta.value.string);
				}
				break;
			}
			case LORE_EVENT_ERROR:
			{
				Accumulator->bError = true;
				Accumulator->ErrorMessage = StringFromLore(Event->error.error_inner);
				Accumulator->Diagnostics.Add(Accumulator->ErrorMessage);
				break;
			}
			case LORE_EVENT_LOG:
			{
				if (Event->log.level >= LORE_LOG_LEVEL_WARN)
				{
					Accumulator->Diagnostics.Add(StringFromLore(Event->log.message));
				}
				break;
			}
			case LORE_EVENT_COMPLETE:
			{
				Accumulator->CompleteStatus = Event->complete.status;
				break;
			}
			default:
				break;
		}
	}
	// One synchronous auth call's events accumulate here. AUTH_USER_INFO carries the resolved identity; AUTH_URL carries the login URL in no-browser interactive mode.
	struct FAuthAccumulator
	{
		LoreClient::FAuthUserInfo* User = nullptr;  // optional sink for the resolved identity
		FString* AuthUrl = nullptr;                 // optional sink for the no-browser login URL
		bool bError = false;
		FString ErrorMessage;
		int32 CompleteStatus = 0;
		TArray<FString> Diagnostics;
	};

	void AuthEventCallback(const lore_event_t* Event, uint64_t UserContext)
	{
		FAuthAccumulator* Accumulator = reinterpret_cast<FAuthAccumulator*>(static_cast<UPTRINT>(UserContext));
		if (!Event || !Accumulator)
		{
			return;
		}

		switch (Event->tag)
		{
			case LORE_EVENT_AUTH_USER_INFO:
			{
				if (Accumulator->User)
				{
					Accumulator->User->Id = StringFromLore(Event->auth_user_info.id);
					Accumulator->User->Name = StringFromLore(Event->auth_user_info.name);
				}
				break;
			}
			case LORE_EVENT_AUTH_URL:
			{
				if (Accumulator->AuthUrl)
				{
					*Accumulator->AuthUrl = StringFromLore(Event->auth_url.url);
				}
				break;
			}
			case LORE_EVENT_ERROR:
			{
				Accumulator->bError = true;
				Accumulator->ErrorMessage = StringFromLore(Event->error.error_inner);
				Accumulator->Diagnostics.Add(Accumulator->ErrorMessage);
				break;
			}
			case LORE_EVENT_LOG:
			{
				if (Event->log.level >= LORE_LOG_LEVEL_WARN)
				{
					Accumulator->Diagnostics.Add(StringFromLore(Event->log.message));
				}
				break;
			}
			case LORE_EVENT_COMPLETE:
			{
				Accumulator->CompleteStatus = Event->complete.status;
				break;
			}
			default:
				break;
		}
	}

	bool AuthSucceeded(const FAuthAccumulator& Accumulator, int32 Code, const TCHAR* Label, FString& OutError)
	{
		if (Code != 0 || Accumulator.bError || Accumulator.CompleteStatus != 0)
		{
			const FString Detail = Accumulator.Diagnostics.Num() ? FString::Join(Accumulator.Diagnostics, TEXT("; ")) : Accumulator.ErrorMessage;
			OutError = FString::Printf(TEXT("%s failed (return %d, status %d): %s"), Label, Code, Accumulator.CompleteStatus, *Detail);
			return false;
		}
		return true;
	}
}

namespace LoreClient
{
	FString GetVersion()
	{
		const char* Raw = lore_version();
		return Raw ? FString(UTF8_TO_TCHAR(Raw)) : FString();
	}

	bool GetStatus(const FString& InRepositoryRoot, const FString& InIdentity, const TArray<FString>& InRelativePaths, bool bScan, FStatusResult& OutResult, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 Identity(InIdentity);

		// liblore resolves a relative path against the process working directory, not repository_path, so hand it absolute paths under the repository root.
		// Reserve so the inner byte buffers do not move after View() captures their pointers.
		TArray<FLoreUtf8> PathStorage;
		PathStorage.Reserve(InRelativePaths.Num());
		for (const FString& Path : InRelativePaths)
		{
			const FString Absolute = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? FPaths::Combine(InRepositoryRoot, Path) : Path);
			PathStorage.Emplace(Absolute);
		}
		TArray<lore_string_t> PathViews;
		PathViews.Reserve(PathStorage.Num());
		for (const FLoreUtf8& Buffer : PathStorage)
		{
			PathViews.Add(Buffer.View());
		}

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();
		if (!InIdentity.IsEmpty())
		{
			Globals.identity = Identity.View();
		}

		lore_repository_status_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.staged = 1;
		Args.scan = bScan ? 1 : 0;
		Args.paths.ptr = PathViews.Num() ? PathViews.GetData() : nullptr;
		Args.paths.count = static_cast<uintptr_t>(PathViews.Num());

		FStatusAccumulator Accumulator;
		Accumulator.Result = &OutResult;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &StatusEventCallback;

		const int32 Code = lore_repository_status(&Globals, &Args, Callback);
		if (Code != 0 || Accumulator.bError || Accumulator.CompleteStatus != 0)
		{
			const FString Detail = Accumulator.Diagnostics.Num() ? FString::Join(Accumulator.Diagnostics, TEXT("; ")) : Accumulator.ErrorMessage;
			OutError = FString::Printf(TEXT("lore_repository_status failed (return %d, status %d): %s"), Code, Accumulator.CompleteStatus, *Detail);
			return false;
		}
		return true;
	}

	bool GetLockStatus(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, TArray<FLockStatus>& OutLocks, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);

		// liblore resolves a relative path against the process working directory, so hand it absolute paths under the repository root.
		TArray<FLoreUtf8> PathStorage;
		PathStorage.Reserve(InRelativePaths.Num());
		for (const FString& Path : InRelativePaths)
		{
			const FString Absolute = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? FPaths::Combine(InRepositoryRoot, Path) : Path);
			PathStorage.Emplace(Absolute);
		}
		TArray<lore_string_t> PathViews;
		PathViews.Reserve(PathStorage.Num());
		for (const FLoreUtf8& Buffer : PathStorage)
		{
			PathViews.Add(Buffer.View());
		}

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_lock_file_status_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.paths.ptr = PathViews.Num() ? PathViews.GetData() : nullptr;
		Args.paths.count = static_cast<uintptr_t>(PathViews.Num());

		FLockAccumulator Accumulator;
		Accumulator.Locks = &OutLocks;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &LockStatusEventCallback;

		const int32 Code = lore_lock_file_status(&Globals, &Args, Callback);
		if (Code != 0 || Accumulator.bError || Accumulator.CompleteStatus != 0)
		{
			const FString Detail = Accumulator.Diagnostics.Num() ? FString::Join(Accumulator.Diagnostics, TEXT("; ")) : Accumulator.ErrorMessage;
			OutError = FString::Printf(TEXT("lore_lock_file_status failed (return %d, status %d): %s"), Code, Accumulator.CompleteStatus, *Detail);
			return false;
		}
		return true;
	}

	bool QueryContentAvailable(const FString& InRepositoryRoot, const FString& InAddress, bool& bOutAvailable, FString& OutError, bool bQueryRemote)
	{
		bOutAvailable = false;

		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 Address(InAddress);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();
		// A query against an unreachable server blocks on the connection timeout, so answer from the local store alone when the caller already knows the remote is down.
		Globals.local = bQueryRemote ? 0 : 1;

		lore_repository_store_immutable_query_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.address = Address.View();
		Args.recurse = 0;

		FStoreQueryAccumulator Accumulator;
		Accumulator.Available = &bOutAvailable;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &StoreQueryEventCallback;

		const int32 Code = lore_repository_store_immutable_query(&Globals, &Args, Callback);
		if (Code != 0 || Accumulator.bError || Accumulator.CompleteStatus != 0)
		{
			const FString Detail = Accumulator.Diagnostics.Num() ? FString::Join(Accumulator.Diagnostics, TEXT("; ")) : Accumulator.ErrorMessage;
			OutError = FString::Printf(TEXT("lore_repository_store_immutable_query failed (return %d, status %d): %s"), Code, Accumulator.CompleteStatus, *Detail);
			bOutAvailable = false;
			return false;
		}
		return true;
	}

	bool GetHistory(const FString& InRepositoryRoot, const FString& InRelativeFile, TArray<FHistoryRevision>& OutRevisions, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		const FString AbsoluteFile = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(InRelativeFile) ? FPaths::Combine(InRepositoryRoot, InRelativeFile) : InRelativeFile);
		FLoreUtf8 FilePath(AbsoluteFile);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_file_history_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.path = FilePath.View();

		FHistoryAccumulator Accumulator;
		Accumulator.Revisions = &OutRevisions;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &HistoryEventCallback;

		const int32 Code = lore_file_history(&Globals, &Args, Callback);
		if (Code != 0 || Accumulator.bError || Accumulator.CompleteStatus != 0)
		{
			const FString Detail = Accumulator.Diagnostics.Num() ? FString::Join(Accumulator.Diagnostics, TEXT("; ")) : Accumulator.ErrorMessage;
			OutError = FString::Printf(TEXT("lore_file_history failed (return %d, status %d): %s"), Code, Accumulator.CompleteStatus, *Detail);
			return false;
		}
		return true;
	}

	bool Stage(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		TArray<FLoreUtf8> PathStorage;
		TArray<lore_string_t> PathViews;
		const lore_string_array_t Paths = BuildAbsolutePaths(InRepositoryRoot, InRelativePaths, PathStorage, PathViews);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_file_stage_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.paths = Paths;

		FTerminalContext Context;
		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Context));
		Callback.func = &TerminalEventCallback;

		const int32 Code = lore_file_stage(&Globals, &Args, Callback);
		return TerminalSucceeded(Context, Code, TEXT("lore_file_stage"), OutError);
	}

	bool Unstage(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		TArray<FLoreUtf8> PathStorage;
		TArray<lore_string_t> PathViews;
		const lore_string_array_t Paths = BuildAbsolutePaths(InRepositoryRoot, InRelativePaths, PathStorage, PathViews);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_file_unstage_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.paths = Paths;

		FTerminalContext Context;
		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Context));
		Callback.func = &TerminalEventCallback;

		const int32 Code = lore_file_unstage(&Globals, &Args, Callback);
		return TerminalSucceeded(Context, Code, TEXT("lore_file_unstage"), OutError);
	}

	bool Reset(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		TArray<FLoreUtf8> PathStorage;
		TArray<lore_string_t> PathViews;
		const lore_string_array_t Paths = BuildAbsolutePaths(InRepositoryRoot, InRelativePaths, PathStorage, PathViews);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_file_reset_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.paths = Paths;

		FTerminalContext Context;
		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Context));
		Callback.func = &TerminalEventCallback;

		const int32 Code = lore_file_reset(&Globals, &Args, Callback);
		return TerminalSucceeded(Context, Code, TEXT("lore_file_reset"), OutError);
	}

	bool LockAcquire(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		TArray<FLoreUtf8> PathStorage;
		TArray<lore_string_t> PathViews;
		const lore_string_array_t Paths = BuildAbsolutePaths(InRepositoryRoot, InRelativePaths, PathStorage, PathViews);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_lock_file_acquire_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.paths = Paths;

		FTerminalContext Context;
		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Context));
		Callback.func = &TerminalEventCallback;

		const int32 Code = lore_lock_file_acquire(&Globals, &Args, Callback);
		return TerminalSucceeded(Context, Code, TEXT("lore_lock_file_acquire"), OutError);
	}

	bool LockRelease(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		TArray<FLoreUtf8> PathStorage;
		TArray<lore_string_t> PathViews;
		const lore_string_array_t Paths = BuildAbsolutePaths(InRepositoryRoot, InRelativePaths, PathStorage, PathViews);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_lock_file_release_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.paths = Paths;

		FTerminalContext Context;
		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Context));
		Callback.func = &TerminalEventCallback;

		const int32 Code = lore_lock_file_release(&Globals, &Args, Callback);
		return TerminalSucceeded(Context, Code, TEXT("lore_lock_file_release"), OutError);
	}

	bool Commit(const FString& InRepositoryRoot, const FString& InIdentity, const FString& InMessage, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 Identity(InIdentity);
		FLoreUtf8 MessageBuffer(InMessage);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();
		if (!InIdentity.IsEmpty())
		{
			Globals.identity = Identity.View();
		}

		lore_revision_commit_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.message = MessageBuffer.View();

		FTerminalContext Context;
		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Context));
		Callback.func = &TerminalEventCallback;

		const int32 Code = lore_revision_commit(&Globals, &Args, Callback);
		return TerminalSucceeded(Context, Code, TEXT("lore_revision_commit"), OutError);
	}

	bool Push(const FString& InRepositoryRoot, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_branch_push_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));

		FTerminalContext Context;
		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Context));
		Callback.func = &TerminalEventCallback;

		const int32 Code = lore_branch_push(&Globals, &Args, Callback);
		return TerminalSucceeded(Context, Code, TEXT("lore_branch_push"), OutError);
	}

	bool Sync(const FString& InRepositoryRoot, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_revision_sync_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));

		FTerminalContext Context;
		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Context));
		Callback.func = &TerminalEventCallback;

		const int32 Code = lore_revision_sync(&Globals, &Args, Callback);
		return TerminalSucceeded(Context, Code, TEXT("lore_revision_sync"), OutError);
	}

	bool WriteFile(const FString& InRepositoryRoot, const FString& InRelativeFile, const FString& InRevision, const FString& InOutputPath, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		const FString AbsoluteFile = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(InRelativeFile) ? FPaths::Combine(InRepositoryRoot, InRelativeFile) : InRelativeFile);
		FLoreUtf8 FilePath(AbsoluteFile);
		FLoreUtf8 Revision(InRevision);
		FLoreUtf8 Output(FPaths::ConvertRelativePathToFull(InOutputPath));

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_file_write_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.path = FilePath.View();
		Args.revision = Revision.View();
		Args.output = Output.View();

		FTerminalContext Context;
		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Context));
		Callback.func = &TerminalEventCallback;

		const int32 Code = lore_file_write(&Globals, &Args, Callback);
		return TerminalSucceeded(Context, Code, TEXT("lore_file_write"), OutError);
	}

	bool LoginWithToken(const FString& InRepositoryRoot, const FString& InRemoteUrl, const FString& InAuthUrl, const FString& InToken, const FString& InTokenType, FAuthUserInfo& OutUser, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 RemoteUrl(InRemoteUrl);
		FLoreUtf8 AuthUrl(InAuthUrl);
		FLoreUtf8 Token(InToken);
		FLoreUtf8 TokenType(InTokenType);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		if (!InRepositoryRoot.IsEmpty())
		{
			Globals.repository_path = RepositoryPath.View();
		}

		lore_auth_login_with_token_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		if (!InRemoteUrl.IsEmpty()) { Args.remote_url = RemoteUrl.View(); }
		if (!InAuthUrl.IsEmpty()) { Args.auth_url = AuthUrl.View(); }
		Args.token = Token.View();
		Args.token_type = TokenType.View();

		FAuthAccumulator Accumulator;
		Accumulator.User = &OutUser;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &AuthEventCallback;

		const int32 Code = lore_auth_login_with_token(&Globals, &Args, Callback);
		return AuthSucceeded(Accumulator, Code, TEXT("lore_auth_login_with_token"), OutError);
	}

	bool LoginInteractive(const FString& InRepositoryRoot, const FString& InRemoteUrl, bool bNoBrowser, FString& OutAuthUrl, FAuthUserInfo& OutUser, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 RemoteUrl(InRemoteUrl);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		if (!InRepositoryRoot.IsEmpty())
		{
			Globals.repository_path = RepositoryPath.View();
		}

		lore_auth_login_interactive_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		if (!InRemoteUrl.IsEmpty()) { Args.remote_url = RemoteUrl.View(); }
		Args.no_browser = bNoBrowser ? 1 : 0;

		FAuthAccumulator Accumulator;
		Accumulator.User = &OutUser;
		Accumulator.AuthUrl = &OutAuthUrl;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &AuthEventCallback;

		const int32 Code = lore_auth_login_interactive(&Globals, &Args, Callback);
		return AuthSucceeded(Accumulator, Code, TEXT("lore_auth_login_interactive"), OutError);
	}

	bool GetLocalUserInfo(const FString& InRepositoryRoot, const FString& InAuthEndpoint, FAuthUserInfo& OutUser, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 AuthEndpoint(InAuthEndpoint);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		if (!InRepositoryRoot.IsEmpty())
		{
			Globals.repository_path = RepositoryPath.View();
		}

		lore_auth_local_user_info_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		if (!InAuthEndpoint.IsEmpty()) { Args.auth_endpoint = AuthEndpoint.View(); }
		// Empty user_ids resolves the current user.

		FAuthAccumulator Accumulator;
		Accumulator.User = &OutUser;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &AuthEventCallback;

		const int32 Code = lore_auth_local_user_info(&Globals, &Args, Callback);
		return AuthSucceeded(Accumulator, Code, TEXT("lore_auth_local_user_info"), OutError);
	}

	bool Logout(const FString& InRepositoryRoot, const FString& InAuthUrl, const FString& InUserId, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 AuthUrl(InAuthUrl);
		FLoreUtf8 UserId(InUserId);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		if (!InRepositoryRoot.IsEmpty())
		{
			Globals.repository_path = RepositoryPath.View();
		}

		lore_auth_logout_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		if (!InAuthUrl.IsEmpty()) { Args.auth_url = AuthUrl.View(); }
		if (!InUserId.IsEmpty()) { Args.user_id = UserId.View(); }

		FAuthAccumulator Accumulator;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &AuthEventCallback;

		const int32 Code = lore_auth_logout(&Globals, &Args, Callback);
		return AuthSucceeded(Accumulator, Code, TEXT("lore_auth_logout"), OutError);
	}

	void Shutdown()
	{
		lore_shutdown();
	}
}

#endif
