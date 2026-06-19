// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlVersion.h"
#include "ISourceControlRevision.h"

/** One historical revision of a file, from `lore file history`. */
class FLoreSourceControlRevision : public ISourceControlRevision
{
public:
	//~ Begin ISourceControlRevision interface
#if LORE_UE5_0_OR_LATER
	virtual bool Get(FString& InOutFilename, EConcurrency::Type InConcurrency = EConcurrency::Synchronous) const override;
#else
	virtual bool Get(FString& InOutFilename) const override;
#endif
	virtual bool GetAnnotated(TArray<FAnnotationLine>& OutLines) const override;
	virtual bool GetAnnotated(FString& InOutFilename) const override;
	virtual const FString& GetFilename() const override;
	virtual int32 GetRevisionNumber() const override;
	virtual const FString& GetRevision() const override;
	virtual const FString& GetDescription() const override;
	virtual const FString& GetUserName() const override;
	virtual const FString& GetClientSpec() const override;
	virtual const FString& GetAction() const override;
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetBranchSource() const override;
	virtual const FDateTime& GetDate() const override;
	virtual int32 GetCheckInIdentifier() const override;
	virtual int32 GetFileSize() const override;
	//~ End ISourceControlRevision interface

public:
	/** Absolute path of file this revision belongs to. */
	FString Filename;

	/** The Lore revision hash signature. */
	FString CommitId;

	/** Short form of hash for display. */
	FString ShortCommitId;

	/** The Lore revision number along the branch. */
	int32 RevisionNumber = 0;

	/** Commit message. */
	FString Description;

	/** Commit author identity. */
	FString UserName;

	/** Action recorded for this file in this revision (added, modified, deleted). */
	FString Action;

	/** Commit timestamp. */
	FDateTime Date = FDateTime::MinValue();

	/** File size at this revision, zero if deleted. */
	int32 FileSize = 0;

	/** Repository root, used to run extract command. */
	FString PathToRepositoryRoot;

	/** Lore binary, used to run extract command. */
	FString PathToLoreBinary;
};

typedef TSharedRef<FLoreSourceControlRevision, ESPMode::ThreadSafe> FLoreSourceControlRevisionRef;
typedef TArray<FLoreSourceControlRevisionRef> TLoreSourceControlHistory;
