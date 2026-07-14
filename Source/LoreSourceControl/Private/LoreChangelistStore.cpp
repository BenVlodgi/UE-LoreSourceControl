// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreChangelistStore.h"
#include "LorePluginStore.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

bool FLoreChangelistRecord::IsDefault() const
{
	return Id == LoreChangelists::DefaultId;
}

namespace LoreChangelists
{
	const TCHAR* const DefaultId = TEXT("default");

	namespace
	{
		// Store file for this working tree's instance. Falls back to a fixed name when no instance file is present, so a tree without one still persists.
		FString RecordsPath(const FString& InRepositoryRoot)
		{
			FString InstanceId = LorePluginStore::ReadInstanceId(InRepositoryRoot);
			if (InstanceId.IsEmpty())
			{
				InstanceId = TEXT("local");
			}
			return LorePluginStore::GetStorePath(InRepositoryRoot, FString::Printf(TEXT("changelists/%s.json"), *InstanceId));
		}

		FLoreChangelistRecord MakeDefault()
		{
			FLoreChangelistRecord Default;
			Default.Id = DefaultId;
			Default.Title = TEXT("Default");
			return Default;
		}

		TSharedRef<FJsonObject> RecordToJson(const FLoreChangelistRecord& In)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("id"), In.Id);
			Object->SetStringField(TEXT("title"), In.Title);
			Object->SetStringField(TEXT("description"), In.Description);
			Object->SetStringField(TEXT("author"), In.Author);
			Object->SetStringField(TEXT("created"), In.Created == FDateTime::MinValue() ? FString() : In.Created.ToIso8601());
			Object->SetStringField(TEXT("updated"), In.Updated == FDateTime::MinValue() ? FString() : In.Updated.ToIso8601());
			TArray<TSharedPtr<FJsonValue>> Files;
			for (const FString& File : In.Files)
			{
				Files.Add(MakeShared<FJsonValueString>(File));
			}
			Object->SetArrayField(TEXT("files"), Files);
			return Object;
		}

		FLoreChangelistRecord RecordFromJson(const TSharedPtr<FJsonObject>& In)
		{
			FLoreChangelistRecord Out;
			In->TryGetStringField(TEXT("id"), Out.Id);
			In->TryGetStringField(TEXT("title"), Out.Title);
			In->TryGetStringField(TEXT("description"), Out.Description);
			In->TryGetStringField(TEXT("author"), Out.Author);
			FString Created, Updated;
			if (In->TryGetStringField(TEXT("created"), Created) && !Created.IsEmpty())
			{
				FDateTime::ParseIso8601(*Created, Out.Created);
			}
			if (In->TryGetStringField(TEXT("updated"), Updated) && !Updated.IsEmpty())
			{
				FDateTime::ParseIso8601(*Updated, Out.Updated);
			}
			const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
			if (In->TryGetArrayField(TEXT("files"), Files) && Files)
			{
				for (const TSharedPtr<FJsonValue>& File : *Files)
				{
					FString Path;
					if (File.IsValid() && File->TryGetString(Path))
					{
						Out.Files.Add(Path);
					}
				}
			}
			return Out;
		}

		// One process-wide lock across every read and load-edit-save of the store, because workers write from pool threads while the provider's changelist GetState reconciles on the game thread. Recursion is safe: FCriticalSection is re-entrant on the same thread.
		FCriticalSection StoreLock;

		TArray<FLoreChangelistRecord> LoadRecords(const FString& InRepositoryRoot)
		{
			TArray<FLoreChangelistRecord> Records;

			TSharedPtr<FJsonObject> Root;
			if (LorePluginStore::ReadJsonObject(RecordsPath(InRepositoryRoot), Root) && Root.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
				if (Root->TryGetArrayField(TEXT("changelists"), List) && List)
				{
					for (const TSharedPtr<FJsonValue>& Entry : *List)
					{
						const TSharedPtr<FJsonObject>* Object = nullptr;
						if (Entry.IsValid() && Entry->TryGetObject(Object) && Object)
						{
							Records.Add(RecordFromJson(*Object));
						}
					}
				}
			}

			// The Default changelist is mandatory and always first, since the Changes view needs it to render unassigned files.
			if (!Records.ContainsByPredicate([](const FLoreChangelistRecord& R) { return R.IsDefault(); }))
			{
				Records.Insert(MakeDefault(), 0);
			}
			return Records;
		}

		bool SaveRecords(const FString& InRepositoryRoot, const TArray<FLoreChangelistRecord>& InRecords)
		{
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetNumberField(TEXT("version"), 1);
			TArray<TSharedPtr<FJsonValue>> List;
			for (const FLoreChangelistRecord& Record : InRecords)
			{
				List.Add(MakeShared<FJsonValueObject>(RecordToJson(Record)));
			}
			Root->SetArrayField(TEXT("changelists"), List);
			return LorePluginStore::WriteJsonObjectAtomic(RecordsPath(InRepositoryRoot), Root);
		}
	}

	TArray<FLoreChangelistRecord> Load(const FString& InRepositoryRoot)
	{
		FScopeLock Lock(&StoreLock);
		return LoadRecords(InRepositoryRoot);
	}

	bool Save(const FString& InRepositoryRoot, const TArray<FLoreChangelistRecord>& InRecords)
	{
		FScopeLock Lock(&StoreLock);
		return SaveRecords(InRepositoryRoot, InRecords);
	}

	bool Mutate(const FString& InRepositoryRoot, TFunctionRef<bool(TArray<FLoreChangelistRecord>&)> InEdit)
	{
		FScopeLock Lock(&StoreLock);
		TArray<FLoreChangelistRecord> Records = LoadRecords(InRepositoryRoot);
		if (!InEdit(Records))
		{
			return false;
		}
		return SaveRecords(InRepositoryRoot, Records);
	}

	void Reconcile(TArray<FLoreChangelistRecord>& Records, const TArray<FString>& InChangedFiles, const TMap<FString, FString>& InMovesFromTo)
	{
		const TSet<FString> Changed(InChangedFiles);

		// Rewrite a moved file in place by its new path, so it keeps its changelist across the move.
		for (FLoreChangelistRecord& Record : Records)
		{
			for (FString& File : Record.Files)
			{
				if (const FString* To = InMovesFromTo.Find(File))
				{
					File = *To;
				}
			}
		}

		// Drop a file that is no longer changed, and collect which changed files are already assigned.
		TSet<FString> Assigned;
		for (FLoreChangelistRecord& Record : Records)
		{
			Record.Files.RemoveAll([&Changed](const FString& File) { return !Changed.Contains(File); });
			for (const FString& File : Record.Files)
			{
				Assigned.Add(File);
			}
		}

		// A changed file in no changelist belongs to Default. A file already assigned elsewhere is left where the user put it.
		FLoreChangelistRecord* Default = Records.FindByPredicate([](const FLoreChangelistRecord& R) { return R.IsDefault(); });
		if (Default)
		{
			for (const FString& File : InChangedFiles)
			{
				if (!Assigned.Contains(File))
				{
					Default->Files.Add(File);
					Assigned.Add(File);
				}
			}
		}
	}
}
