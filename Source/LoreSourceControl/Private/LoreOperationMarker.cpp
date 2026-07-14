// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreOperationMarker.h"
#include "LorePluginStore.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace LoreOperationMarker
{
	namespace
	{
		// Per-instance marker file, so two working trees against one shared store never overwrite each other's marker. Falls back to the unkeyed name when no instance identifier is readable.
		FString MarkerPath(const FString& InRepositoryRoot)
		{
			const FString Instance = LorePluginStore::ReadInstanceId(InRepositoryRoot);
			const FString Name = Instance.IsEmpty() ? FString(TEXT("operation.json")) : FString::Printf(TEXT("operation-%s.json"), *Instance);
			return LorePluginStore::GetStorePath(InRepositoryRoot, Name);
		}

		FString KindToString(ELoreOperationKind In)
		{
			switch (In)
			{
				case ELoreOperationKind::BranchSwitch:   return TEXT("branchSwitch");
				case ELoreOperationKind::RevisionSync:   return TEXT("revisionSync");
				case ELoreOperationKind::ReturnToLatest: return TEXT("returnToLatest");
				default:                                 return TEXT("none");
			}
		}

		ELoreOperationKind KindFromString(const FString& In)
		{
			if (In == TEXT("branchSwitch"))   return ELoreOperationKind::BranchSwitch;
			if (In == TEXT("revisionSync"))   return ELoreOperationKind::RevisionSync;
			if (In == TEXT("returnToLatest")) return ELoreOperationKind::ReturnToLatest;
			return ELoreOperationKind::None;
		}
	}

	bool Write(const FString& InRepositoryRoot, const FLoreOperationMarker& InMarker)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("kind"), KindToString(InMarker.Kind));
		Object->SetStringField(TEXT("instance"), InMarker.Instance);
		Object->SetStringField(TEXT("sourceBranch"), InMarker.SourceBranch);
		Object->SetStringField(TEXT("targetBranch"), InMarker.TargetBranch);
		Object->SetStringField(TEXT("sourceRevision"), InMarker.SourceRevision);
		Object->SetStringField(TEXT("targetRevision"), InMarker.TargetRevision);
		Object->SetStringField(TEXT("started"), InMarker.Started == FDateTime::MinValue() ? FString() : InMarker.Started.ToIso8601());
		TArray<TSharedPtr<FJsonValue>> Files;
		for (const FString& File : InMarker.ChangedFiles)
		{
			Files.Add(MakeShared<FJsonValueString>(File));
		}
		Object->SetArrayField(TEXT("changedFiles"), Files);
		return LorePluginStore::WriteJsonObjectAtomic(MarkerPath(InRepositoryRoot), Object);
	}

	FLoreOperationMarker Read(const FString& InRepositoryRoot)
	{
		FLoreOperationMarker Marker;

		TSharedPtr<FJsonObject> Object;
		if (!LorePluginStore::ReadJsonObject(MarkerPath(InRepositoryRoot), Object) || !Object.IsValid())
		{
			return Marker;
		}

		FString Kind;
		Object->TryGetStringField(TEXT("kind"), Kind);
		Marker.Kind = KindFromString(Kind);
		Object->TryGetStringField(TEXT("instance"), Marker.Instance);
		Object->TryGetStringField(TEXT("sourceBranch"), Marker.SourceBranch);
		Object->TryGetStringField(TEXT("targetBranch"), Marker.TargetBranch);
		Object->TryGetStringField(TEXT("sourceRevision"), Marker.SourceRevision);
		Object->TryGetStringField(TEXT("targetRevision"), Marker.TargetRevision);
		FString Started;
		if (Object->TryGetStringField(TEXT("started"), Started) && !Started.IsEmpty())
		{
			FDateTime::ParseIso8601(*Started, Marker.Started);
		}
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if (Object->TryGetArrayField(TEXT("changedFiles"), Files) && Files)
		{
			for (const TSharedPtr<FJsonValue>& File : *Files)
			{
				FString Path;
				if (File.IsValid() && File->TryGetString(Path))
				{
					Marker.ChangedFiles.Add(Path);
				}
			}
		}
		return Marker;
	}

	bool Clear(const FString& InRepositoryRoot)
	{
		const FString Path = MarkerPath(InRepositoryRoot);
		if (!IFileManager::Get().FileExists(*Path))
		{
			return true;
		}
		return IFileManager::Get().Delete(*Path, false, true);
	}
}
