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

	// Every accumulator ignores error and complete events after a complete with status 0: a server that answers and then challenges authentication emits a trailing error plus a second complete with status 1, and the command-line parsers already skip that tail. bCompletedOk records the successful complete.

	// Terminal and diagnostic events shared by every call; a mutation needs nothing more than these.
	struct FTerminalContext
	{
		bool bError = false;
		FString ErrorMessage;
		int32 CompleteStatus = 0;
		bool bCompletedOk = false;
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
				if (Context->bCompletedOk)
				{
					break;
				}
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
				if (!Context->bCompletedOk)
				{
					Context->CompleteStatus = Event->complete.status;
					Context->bCompletedOk = Event->complete.status == 0;
				}
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
		bool bCompletedOk = false;
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
				Accumulator->Result->CurrentRevision = HashToHex(Revision.revision);
				Accumulator->Result->CurrentRevisionNumber = static_cast<int32>(Revision.revision_number);
				Accumulator->Result->RemoteRevision = HashToHex(Revision.revision_remote);
				Accumulator->Result->RemoteRevisionNumber = static_cast<int32>(Revision.revision_remote_number);
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
				if (Accumulator->bCompletedOk)
				{
					break;
				}
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
				if (!Accumulator->bCompletedOk)
				{
					Accumulator->CompleteStatus = Event->complete.status;
					Accumulator->bCompletedOk = Event->complete.status == 0;
				}
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
		bool bCompletedOk = false;
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
				if (Accumulator->bCompletedOk)
				{
					break;
				}
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
				if (!Accumulator->bCompletedOk)
				{
					Accumulator->CompleteStatus = Event->complete.status;
					Accumulator->bCompletedOk = Event->complete.status == 0;
				}
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
		bool bCompletedOk = false;
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
				if (Accumulator->bCompletedOk)
				{
					break;
				}
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
				if (!Accumulator->bCompletedOk)
				{
					Accumulator->CompleteStatus = Event->complete.status;
					Accumulator->bCompletedOk = Event->complete.status == 0;
				}
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
		bool bCompletedOk = false;
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
				if (Accumulator->bCompletedOk)
				{
					break;
				}
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
				if (!Accumulator->bCompletedOk)
				{
					Accumulator->CompleteStatus = Event->complete.status;
					Accumulator->bCompletedOk = Event->complete.status == 0;
				}
				break;
			}
			default:
				break;
		}
	}

	// timestamp, message, and committed-by fill the given fields; created-by seeds the author until the committer arrives.
	void ApplyMetadata(const lore_metadata_event_data_t& InMeta, FDateTime& OutDate, FString& OutMessage, FString& OutAuthor)
	{
		const FString Key = StringFromLore(InMeta.key);
		if (Key == TEXT("timestamp") && InMeta.value.tag == LORE_METADATA_NUMERIC)
		{
			OutDate = FDateTime::FromUnixTimestamp(static_cast<int64>(InMeta.value.numeric / 1000));
		}
		else if (Key == TEXT("message") && InMeta.value.tag == LORE_METADATA_STRING)
		{
			OutMessage = StringFromLore(InMeta.value.string);
		}
		else if ((Key == TEXT("committed-by") || (Key == TEXT("created-by") && OutAuthor.IsEmpty())) && InMeta.value.tag == LORE_METADATA_STRING)
		{
			OutAuthor = StringFromLore(InMeta.value.string);
		}
	}

	// One synchronous revision-history call's events accumulate here. REVISION_HISTORY_ENTRY starts an entry; the library streams the revision's metadata right after it, though the header's event table omits that.
	struct FRevisionListAccumulator
	{
		TArray<LoreClient::FRevisionEntry>* Revisions = nullptr;
		bool bError = false;
		FString ErrorMessage;
		int32 CompleteStatus = 0;
		bool bCompletedOk = false;
		TArray<FString> Diagnostics;
	};

	void RevisionListEventCallback(const lore_event_t* Event, uint64_t UserContext)
	{
		FRevisionListAccumulator* Accumulator = reinterpret_cast<FRevisionListAccumulator*>(static_cast<UPTRINT>(UserContext));
		if (!Event || !Accumulator || !Accumulator->Revisions)
		{
			return;
		}

		switch (Event->tag)
		{
			case LORE_EVENT_REVISION_HISTORY_ENTRY:
			{
				const lore_revision_history_entry_event_data_t& Entry = Event->revision_history_entry;
				LoreClient::FRevisionEntry Out;
				Out.Hash = HashToHex(Entry.revision);
				Out.RevisionNumber = static_cast<int32>(Entry.revision_number);
				Out.ParentHash = HashToHex(Entry.parent[0]);
				Out.ParentOtherHash = HashToHex(Entry.parent[1]);
				Accumulator->Revisions->Add(MoveTemp(Out));
				break;
			}
			case LORE_EVENT_METADATA:
			{
				if (Accumulator->Revisions->Num() == 0)
				{
					break;
				}
				LoreClient::FRevisionEntry& Current = Accumulator->Revisions->Last();
				ApplyMetadata(Event->metadata, Current.Date, Current.Message, Current.Author);
				break;
			}
			case LORE_EVENT_ERROR:
			{
				if (Accumulator->bCompletedOk)
				{
					break;
				}
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
				if (!Accumulator->bCompletedOk)
				{
					Accumulator->CompleteStatus = Event->complete.status;
					Accumulator->bCompletedOk = Event->complete.status == 0;
				}
				break;
			}
			default:
				break;
		}
	}

	// One synchronous revision-info call's events accumulate here. REVISION_INFO carries the header, METADATA the commit fields, and each REVISION_INFO_DELTA one changed path.
	struct FRevisionInfoAccumulator
	{
		LoreClient::FRevisionDetails* Details = nullptr;
		bool bError = false;
		FString ErrorMessage;
		int32 CompleteStatus = 0;
		bool bCompletedOk = false;
		TArray<FString> Diagnostics;
	};

	void RevisionInfoEventCallback(const lore_event_t* Event, uint64_t UserContext)
	{
		FRevisionInfoAccumulator* Accumulator = reinterpret_cast<FRevisionInfoAccumulator*>(static_cast<UPTRINT>(UserContext));
		if (!Event || !Accumulator || !Accumulator->Details)
		{
			return;
		}

		switch (Event->tag)
		{
			case LORE_EVENT_REVISION_INFO:
			{
				const lore_revision_info_event_data_t& Info = Event->revision_info;
				Accumulator->Details->Hash = HashToHex(Info.revision);
				Accumulator->Details->RevisionNumber = static_cast<int32>(Info.revision_number);
				break;
			}
			case LORE_EVENT_METADATA:
			{
				ApplyMetadata(Event->metadata, Accumulator->Details->Date, Accumulator->Details->Message, Accumulator->Details->Author);
				break;
			}
			case LORE_EVENT_REVISION_INFO_DELTA:
			{
				const lore_revision_info_delta_event_data_t& Delta = Event->revision_info_delta;
				LoreClient::FRevisionDelta Out;
				Out.Path = StringFromLore(Delta.path);
				Out.Action = ActionToken(Delta.action);
				Out.Size = static_cast<int64>(Delta.size);
				Out.bModified = Delta.flag_modify != 0;
				Out.bMerged = Delta.flag_merged != 0;
				Out.bIsFile = Delta.flag_file != 0;
				Accumulator->Details->Deltas.Add(MoveTemp(Out));
				break;
			}
			case LORE_EVENT_ERROR:
			{
				if (Accumulator->bCompletedOk)
				{
					break;
				}
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
				if (!Accumulator->bCompletedOk)
				{
					Accumulator->CompleteStatus = Event->complete.status;
					Accumulator->bCompletedOk = Event->complete.status == 0;
				}
				break;
			}
			default:
				break;
		}
	}

	// One synchronous branch-list call's events accumulate here. Each BRANCH_LIST_ENTRY adds a branch; begin and end frame the stream.
	struct FBranchListAccumulator
	{
		TArray<LoreClient::FBranchInfo>* Branches = nullptr;
		bool bError = false;
		FString ErrorMessage;
		int32 CompleteStatus = 0;
		bool bCompletedOk = false;
		TArray<FString> Diagnostics;
	};

	void BranchListEventCallback(const lore_event_t* Event, uint64_t UserContext)
	{
		FBranchListAccumulator* Accumulator = reinterpret_cast<FBranchListAccumulator*>(static_cast<UPTRINT>(UserContext));
		if (!Event || !Accumulator || !Accumulator->Branches)
		{
			return;
		}

		switch (Event->tag)
		{
			case LORE_EVENT_BRANCH_LIST_ENTRY:
			{
				const lore_branch_list_entry_event_data_t& Entry = Event->branch_list_entry;
				LoreClient::FBranchInfo Out;
				Out.Name = StringFromLore(Entry.name);
				Out.Category = StringFromLore(Entry.category);
				Out.Creator = StringFromLore(Entry.creator);
				Out.LatestHash = HashToHex(Entry.latest);
				Out.Created = FDateTime::FromUnixTimestamp(static_cast<int64>(Entry.created / 1000));
				Out.bIsCurrent = Entry.is_current != 0;
				Out.bArchived = Entry.archived != 0;
				Out.bIsRemote = Entry.location != LORE_BRANCH_LOCATION_LOCAL;
				Accumulator->Branches->Add(MoveTemp(Out));
				break;
			}
			case LORE_EVENT_ERROR:
			{
				if (Accumulator->bCompletedOk)
				{
					break;
				}
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
				if (!Accumulator->bCompletedOk)
				{
					Accumulator->CompleteStatus = Event->complete.status;
					Accumulator->bCompletedOk = Event->complete.status == 0;
				}
				break;
			}
			default:
				break;
		}
	}

	// One branch-metadata-get call's events accumulate here. A METADATA event carries the requested key and its tagged value; a missing key emits no METADATA event, so bFound stays false.
	struct FBranchMetadataAccumulator
	{
		FString Key;         // key being read, so a metadata event for another key is ignored
		FString* Value = nullptr;
		bool* Found = nullptr;
		bool bError = false;
		FString ErrorMessage;
		int32 CompleteStatus = 0;
		bool bCompletedOk = false;
		TArray<FString> Diagnostics;
	};

	// Render a metadata value as text: boolean as "true"/"false", numeric decimal, string verbatim, and a hash or address as hex. Only boolean is load-bearing for the protect key; the rest keep the reader total.
	FString MetadataValueToString(const lore_metadata_t& InValue)
	{
		switch (InValue.tag)
		{
			case LORE_METADATA_BOOLEAN: return InValue.boolean != 0 ? TEXT("true") : TEXT("false");
			case LORE_METADATA_NUMERIC: return FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(InValue.numeric));
			case LORE_METADATA_STRING:  return StringFromLore(InValue.string);
			case LORE_METADATA_HASH:    return HashToHex(InValue.hash);
			case LORE_METADATA_ADDRESS: return AddressToString(InValue.address);
			default:                    return FString();
		}
	}

	void BranchMetadataEventCallback(const lore_event_t* Event, uint64_t UserContext)
	{
		FBranchMetadataAccumulator* Accumulator = reinterpret_cast<FBranchMetadataAccumulator*>(static_cast<UPTRINT>(UserContext));
		if (!Event || !Accumulator)
		{
			return;
		}

		switch (Event->tag)
		{
			case LORE_EVENT_METADATA:
			{
				const lore_metadata_event_data_t& Meta = Event->metadata;
				if (StringFromLore(Meta.key) != Accumulator->Key)
				{
					break;
				}
				if (Accumulator->Value)
				{
					*Accumulator->Value = MetadataValueToString(Meta.value);
				}
				if (Accumulator->Found)
				{
					*Accumulator->Found = true;
				}
				break;
			}
			case LORE_EVENT_ERROR:
			{
				if (Accumulator->bCompletedOk)
				{
					break;
				}
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
				if (!Accumulator->bCompletedOk)
				{
					Accumulator->CompleteStatus = Event->complete.status;
					Accumulator->bCompletedOk = Event->complete.status == 0;
				}
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
		bool bCompletedOk = false;
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
				if (Accumulator->bCompletedOk)
				{
					break;
				}
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
				if (!Accumulator->bCompletedOk)
				{
					Accumulator->CompleteStatus = Event->complete.status;
					Accumulator->bCompletedOk = Event->complete.status == 0;
				}
				break;
			}
			default:
				break;
		}
	}

	// One dry-run sync's per-file delta accumulates here. Each REVISION_SYNC_FILE names one file the sync would touch; under the dry_run global flag nothing on disk changes.
	struct FSyncDeltaAccumulator
	{
		TArray<LoreClient::FSyncDeltaFile>* Delta = nullptr;
		bool bError = false;
		FString ErrorMessage;
		int32 CompleteStatus = 0;
		bool bCompletedOk = false;
		TArray<FString> Diagnostics;
	};

	void SyncDeltaEventCallback(const lore_event_t* Event, uint64_t UserContext)
	{
		FSyncDeltaAccumulator* Accumulator = reinterpret_cast<FSyncDeltaAccumulator*>(static_cast<UPTRINT>(UserContext));
		if (!Event || !Accumulator || !Accumulator->Delta)
		{
			return;
		}

		switch (Event->tag)
		{
			case LORE_EVENT_REVISION_SYNC_FILE:
			{
				const lore_revision_sync_file_event_data_t& File = Event->revision_sync_file;
				// Directory entries carry no content to rewrite, so keep only files in the delta.
				if (File.flag_file == 0)
				{
					break;
				}
				LoreClient::FSyncDeltaFile Out;
				Out.Path = StringFromLore(File.path);
				Out.Action = ActionToken(File.action);
				Accumulator->Delta->Add(MoveTemp(Out));
				break;
			}
			case LORE_EVENT_ERROR:
			{
				if (Accumulator->bCompletedOk)
				{
					break;
				}
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
				if (!Accumulator->bCompletedOk)
				{
					Accumulator->CompleteStatus = Event->complete.status;
					Accumulator->bCompletedOk = Event->complete.status == 0;
				}
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

	bool BranchList(const FString& InRepositoryRoot, bool bIncludeArchived, TArray<FBranchInfo>& OutBranches, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_branch_list_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.archived = bIncludeArchived ? 1 : 0;

		FBranchListAccumulator Accumulator;
		Accumulator.Branches = &OutBranches;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &BranchListEventCallback;

		const int32 Code = lore_branch_list(&Globals, &Args, Callback);
		if (Code != 0 || Accumulator.bError || Accumulator.CompleteStatus != 0)
		{
			const FString Detail = Accumulator.Diagnostics.Num() ? FString::Join(Accumulator.Diagnostics, TEXT("; ")) : Accumulator.ErrorMessage;
			OutError = FString::Printf(TEXT("lore_branch_list failed (return %d, status %d): %s"), Code, Accumulator.CompleteStatus, *Detail);
			return false;
		}
		return true;
	}

	bool BranchMetadataGet(const FString& InRepositoryRoot, const FString& InBranch, const FString& InKey, FString& OutValue, bool& bOutFound, FString& OutError)
	{
		OutValue.Empty();
		bOutFound = false;

		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 Branch(InBranch);
		FLoreUtf8 Key(InKey);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_branch_metadata_get_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		if (!InBranch.IsEmpty()) { Args.branch = Branch.View(); }
		Args.key = Key.View();

		FBranchMetadataAccumulator Accumulator;
		Accumulator.Key = InKey;
		Accumulator.Value = &OutValue;
		Accumulator.Found = &bOutFound;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &BranchMetadataEventCallback;

		const int32 Code = lore_branch_metadata_get(&Globals, &Args, Callback);
		if (Code != 0 || Accumulator.bError || Accumulator.CompleteStatus != 0)
		{
			const FString Detail = Accumulator.Diagnostics.Num() ? FString::Join(Accumulator.Diagnostics, TEXT("; ")) : Accumulator.ErrorMessage;
			OutError = FString::Printf(TEXT("lore_branch_metadata_get failed (return %d, status %d): %s"), Code, Accumulator.CompleteStatus, *Detail);
			return false;
		}
		return true;
	}

	bool RevisionList(const FString& InRepositoryRoot, const FString& InBranch, const FString& InStartRevision, int32 InLength, TArray<FRevisionEntry>& OutRevisions, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 Branch(InBranch);
		FLoreUtf8 StartRevision(InStartRevision);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_revision_history_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		if (!InStartRevision.IsEmpty()) { Args.revision = StartRevision.View(); }
		if (!InBranch.IsEmpty()) { Args.branch = Branch.View(); }
		Args.length = InLength > 0 ? static_cast<uint32_t>(InLength) : 0;

		FRevisionListAccumulator Accumulator;
		Accumulator.Revisions = &OutRevisions;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &RevisionListEventCallback;

		const int32 Code = lore_revision_history(&Globals, &Args, Callback);
		if (Code != 0 || Accumulator.bError || Accumulator.CompleteStatus != 0)
		{
			const FString Detail = Accumulator.Diagnostics.Num() ? FString::Join(Accumulator.Diagnostics, TEXT("; ")) : Accumulator.ErrorMessage;
			OutError = FString::Printf(TEXT("lore_revision_history failed (return %d, status %d): %s"), Code, Accumulator.CompleteStatus, *Detail);
			return false;
		}
		return true;
	}

	bool RevisionInfo(const FString& InRepositoryRoot, const FString& InRevision, bool bIncludeDelta, FRevisionDetails& OutDetails, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 Revision(InRevision);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_revision_info_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		if (!InRevision.IsEmpty()) { Args.revision = Revision.View(); }
		Args.delta = bIncludeDelta ? 1 : 0;
		Args.metadata = 1;

		FRevisionInfoAccumulator Accumulator;
		Accumulator.Details = &OutDetails;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &RevisionInfoEventCallback;

		const int32 Code = lore_revision_info(&Globals, &Args, Callback);
		if (Code != 0 || Accumulator.bError || Accumulator.CompleteStatus != 0)
		{
			const FString Detail = Accumulator.Diagnostics.Num() ? FString::Join(Accumulator.Diagnostics, TEXT("; ")) : Accumulator.ErrorMessage;
			OutError = FString::Printf(TEXT("lore_revision_info failed (return %d, status %d): %s"), Code, Accumulator.CompleteStatus, *Detail);
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

	bool EnumerateSyncDelta(const FString& InRepositoryRoot, TArray<FSyncDeltaFile>& OutDelta, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();
		// Dry run: report the delta and touch nothing. The same realization the switch dry-run drives, verified against sync.rs:595,917.
		Globals.dry_run = 1;

		lore_revision_sync_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));

		FSyncDeltaAccumulator Accumulator;
		Accumulator.Delta = &OutDelta;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &SyncDeltaEventCallback;

		const int32 Code = lore_revision_sync(&Globals, &Args, Callback);
		if (Code != 0 || Accumulator.bError || Accumulator.CompleteStatus != 0)
		{
			const FString Detail = Accumulator.Diagnostics.Num() ? FString::Join(Accumulator.Diagnostics, TEXT("; ")) : Accumulator.ErrorMessage;
			OutError = FString::Printf(TEXT("lore_revision_sync (dry run) failed (return %d, status %d): %s"), Code, Accumulator.CompleteStatus, *Detail);
			return false;
		}
		return true;
	}

	bool EnumerateSwitchDelta(const FString& InRepositoryRoot, const FString& InBranch, const FString& InRevision, TArray<FSyncDeltaFile>& OutDelta, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 Branch(InBranch);
		FLoreUtf8 Revision(InRevision);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();
		// Dry run: the switch reports the delta and touches nothing, the same realization the sync dry-run drives.
		Globals.dry_run = 1;

		lore_branch_switch_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.branch = Branch.View();
		if (!InRevision.IsEmpty()) { Args.revision = Revision.View(); }

		FSyncDeltaAccumulator Accumulator;
		Accumulator.Delta = &OutDelta;

		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Accumulator));
		Callback.func = &SyncDeltaEventCallback;

		const int32 Code = lore_branch_switch(&Globals, &Args, Callback);
		if (Code != 0 || Accumulator.bError || Accumulator.CompleteStatus != 0)
		{
			const FString Detail = Accumulator.Diagnostics.Num() ? FString::Join(Accumulator.Diagnostics, TEXT("; ")) : Accumulator.ErrorMessage;
			OutError = FString::Printf(TEXT("lore_branch_switch (dry run) failed (return %d, status %d): %s"), Code, Accumulator.CompleteStatus, *Detail);
			return false;
		}
		return true;
	}

	bool BranchCreate(const FString& InRepositoryRoot, const FString& InName, const FString& InCategory, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 Name(InName);
		FLoreUtf8 Category(InCategory);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_branch_create_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.branch = Name.View();
		if (!InCategory.IsEmpty()) { Args.category = Category.View(); }

		FTerminalContext Context;
		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Context));
		Callback.func = &TerminalEventCallback;

		const int32 Code = lore_branch_create(&Globals, &Args, Callback);
		return TerminalSucceeded(Context, Code, TEXT("lore_branch_create"), OutError);
	}

	bool BranchSwitch(const FString& InRepositoryRoot, const FString& InBranch, const FString& InRevision, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 Branch(InBranch);
		FLoreUtf8 Revision(InRevision);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_branch_switch_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.branch = Branch.View();
		if (!InRevision.IsEmpty()) { Args.revision = Revision.View(); }
		// Never reset or bare: a conflicting dirty file must refuse the switch, not be overwritten.

		FTerminalContext Context;
		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Context));
		Callback.func = &TerminalEventCallback;

		const int32 Code = lore_branch_switch(&Globals, &Args, Callback);
		return TerminalSucceeded(Context, Code, TEXT("lore_branch_switch"), OutError);
	}

	bool BranchArchive(const FString& InRepositoryRoot, const FString& InBranch, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 Branch(InBranch);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		lore_branch_archive_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.branch = Branch.View();

		FTerminalContext Context;
		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Context));
		Callback.func = &TerminalEventCallback;

		const int32 Code = lore_branch_archive(&Globals, &Args, Callback);
		return TerminalSucceeded(Context, Code, TEXT("lore_branch_archive"), OutError);
	}

	bool BranchSetProtect(const FString& InRepositoryRoot, const FString& InBranch, bool bProtect, FString& OutError)
	{
		FLoreUtf8 RepositoryPath(InRepositoryRoot);
		FLoreUtf8 Branch(InBranch);

		lore_global_args_t Globals;
		FMemory::Memzero(&Globals, sizeof(Globals));
		Globals.repository_path = RepositoryPath.View();

		FTerminalContext Context;
		lore_event_callback_config_t Callback;
		Callback.user_context = static_cast<uint64_t>(reinterpret_cast<UPTRINT>(&Context));
		Callback.func = &TerminalEventCallback;

		int32 Code = 0;
		if (bProtect)
		{
			lore_branch_protect_args_t Args;
			FMemory::Memzero(&Args, sizeof(Args));
			Args.branch = Branch.View();
			Code = lore_branch_protect(&Globals, &Args, Callback);
			return TerminalSucceeded(Context, Code, TEXT("lore_branch_protect"), OutError);
		}
		lore_branch_unprotect_args_t Args;
		FMemory::Memzero(&Args, sizeof(Args));
		Args.branch = Branch.View();
		Code = lore_branch_unprotect(&Globals, &Args, Callback);
		return TerminalSucceeded(Context, Code, TEXT("lore_branch_unprotect"), OutError);
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
