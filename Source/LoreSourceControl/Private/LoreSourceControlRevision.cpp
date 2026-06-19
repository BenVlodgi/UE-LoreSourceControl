// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlRevision.h"
#include "LoreSourceControlUtils.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#if LORE_UE5_0_OR_LATER
bool FLoreSourceControlRevision::Get(FString& InOutFilename, EConcurrency::Type InConcurrency) const
#else
bool FLoreSourceControlRevision::Get(FString& InOutFilename) const
#endif
{
	// An empty name means choose our own output path.
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
	TArray<FString> Results;
	TArray<FString> Errors;
	TArray<FString> Parameters;
	Parameters.Add(FString::Printf(TEXT("--revision %s"), *CommitId));
	Parameters.Add(FString::Printf(TEXT("--path \"%s\""), *RelativeFile));
	Parameters.Add(FString::Printf(TEXT("--output \"%s\""), *InOutFilename));

	const bool bWritten = LoreSourceControlUtils::RunCommand(TEXT("file write"), PathToLoreBinary, PathToRepositoryRoot, Parameters, TArray<FString>(), Results, Errors)
		&& IFileManager::Get().FileExists(*InOutFilename);
	if (!bWritten)
	{
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
