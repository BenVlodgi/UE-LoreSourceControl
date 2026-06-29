// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlRevision.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlLog.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#if SOURCE_CONTROL_WITH_SLATE
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#endif

#define LOCTEXT_NAMESPACE "LoreSourceControl"

#if LORE_UE5_0_OR_LATER
bool FLoreSourceControlRevision::Get(FString& InOutFilename, EConcurrency::Type InConcurrency) const
#else
bool FLoreSourceControlRevision::Get(FString& InOutFilename) const
#endif
{
	// A revision cannot be extracted without its id. liblore's file write treats an empty revision as "write the working tree", which would silently diff an asset against itself, so refuse rather than extract the wrong content.
	if (CommitId.IsEmpty())
	{
		return false;
	}

	// Empty name means choose our own output path.
	if (InOutFilename.IsEmpty())
	{
		// The temporary file must sit under Saved/Diff, which is mounted as the /Temp/ package root, so the editor can load it for diff. A path outside a mounted root yields an empty package name and the diff fails.
		const FString TempDir = FPaths::ConvertRelativePathToFull(FPaths::DiffDir());
		IFileManager::Get().MakeDirectory(*TempDir, true);
		const FString Extension = FPaths::GetExtension(Filename, true);
		const FString BaseName = FPaths::GetBaseFilename(Filename);
		// Full signature, not the short form, so same-named assets in different folders do not collide.
		InOutFilename = FPaths::Combine(TempDir, FString::Printf(TEXT("%s-%s%s"), *BaseName, *CommitId, *Extension));
	}

	// The client refuses to overwrite an existing output file. A revision is immutable and the name carries its signature, so reuse an existing file.
	if (IFileManager::Get().FileExists(*InOutFilename))
	{
		return true;
	}

	const FString RelativeFile = LoreSourceControlUtils::RelativeFilename(Filename, PathToRepositoryRoot);
	TArray<FString> Errors;
	const bool bWritten = LoreSourceControlUtils::WriteRevisionFile(BackendMode, PathToLoreBinary, PathToRepositoryRoot, RelativeFile, CommitId, InOutFilename, Errors)
		&& IFileManager::Get().FileExists(*InOutFilename);
	if (!bWritten)
	{
		// A failed extraction surfaces in the editor only as a generic "content unsupported" dialog, so name the revision and the likely cause: after a push the bytes live only on the remote, so extraction needs the content present locally or a reachable server.
		UE_LOG(LogLoreSourceControl, Warning, TEXT("Could not extract revision %s of %s for diff. Its content is not in the local store and could not be fetched from the server. %s"), *CommitId, *Filename, *FString::Join(Errors, TEXT(" | ")));
#if SOURCE_CONTROL_WITH_SLATE
		// Toast names the asset and cause, since the engine's own diff dialog is generic. The notification manager touches Slate, so only on the game thread; the EConcurrency overload can run elsewhere and there the log above is the only record.
		if (IsInGameThread())
		{
			FNotificationInfo Info(FText::Format(
				LOCTEXT("RevisionExtractFailed", "Could not load {0} for diff. This revision's content is not available in the local store or on a reachable Lore server."),
				FText::FromString(FPaths::GetCleanFilename(Filename))));
			Info.ExpireDuration = 6.0f;
			FSlateNotificationManager::Get().AddNotification(Info);
		}
#endif
		// Drop a partial output so a later diff cannot reuse a bad file.
		IFileManager::Get().Delete(*InOutFilename, false, true);
	}
	return bWritten;
}

bool FLoreSourceControlRevision::GetAnnotated(TArray<FAnnotationLine>& OutLines) const
{
	// Line annotation is not exposed yet.
	return false;
}

bool FLoreSourceControlRevision::GetAnnotated(FString& InOutFilename) const
{
	return false;
}

const FString& FLoreSourceControlRevision::GetFilename() const { return Filename; }
int32 FLoreSourceControlRevision::GetRevisionNumber() const { return RevisionNumber; }
const FString& FLoreSourceControlRevision::GetRevision() const { return ShortCommitId; }
const FString& FLoreSourceControlRevision::GetDescription() const { return Description; }
const FString& FLoreSourceControlRevision::GetUserName() const { return UserName; }

const FString& FLoreSourceControlRevision::GetClientSpec() const
{
	static const FString EmptyString;
	return EmptyString;
}

const FString& FLoreSourceControlRevision::GetAction() const { return Action; }

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FLoreSourceControlRevision::GetBranchSource() const
{
	return nullptr;
}

const FDateTime& FLoreSourceControlRevision::GetDate() const { return Date; }
int32 FLoreSourceControlRevision::GetCheckInIdentifier() const { return RevisionNumber; }
int32 FLoreSourceControlRevision::GetFileSize() const { return FileSize; }

#undef LOCTEXT_NAMESPACE
