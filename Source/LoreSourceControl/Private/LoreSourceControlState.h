// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlVersion.h"
#include "ISourceControlState.h"
#include "LoreSourceControlRevision.h"

/** Working-copy state of a file, parsed from `lore status`. */
namespace ELoreWorkingCopyState
{
	enum Type
	{
		Unknown,
		Unchanged,
		Added,
		Modified,
		Deleted,
		Moved,
		Copied,
		Conflicted,
		NotControlled,
		Ignored,
	};
}

/** Advisory lock state, orthogonal to the working-copy state. */
namespace ELoreLockState
{
	enum Type
	{
		Unknown,
		NotLocked,
		Locked,
		LockedOther,
	};
}

class FLoreSourceControlState : public ISourceControlState
{
public:
	explicit FLoreSourceControlState(const FString& InLocalFilename)
		: LocalFilename(InLocalFilename)
	{
	}

	//~ Begin ISourceControlState interface
	virtual int32 GetHistorySize() const override;
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetHistoryItem(int32 HistoryIndex) const override;
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(int32 RevisionNumber) const override;
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(const FString& InRevision) const override;
#if LORE_UE5_2_OR_LATER
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetCurrentRevision() const override;
#endif

#if LORE_UE5_3_OR_LATER
	virtual FResolveInfo GetResolveInfo() const override;
#else
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> GetBaseRevForMerge() const override;
#endif

#if LORE_UE5_0_OR_LATER
#if SOURCE_CONTROL_WITH_SLATE
	virtual FSlateIcon GetIcon() const override;
#endif
#else
	virtual FName GetIconName() const override;
	virtual FName GetSmallIconName() const override;
#endif
	virtual FText GetDisplayName() const override;
	virtual FText GetDisplayTooltip() const override;
	virtual const FString& GetFilename() const override;
	virtual const FDateTime& GetTimeStamp() const override;
	virtual bool CanCheckIn() const override;
	virtual bool CanCheckout() const override;
	virtual bool IsCheckedOut() const override;
	virtual bool IsCheckedOutOther(FString* Who = nullptr) const override;
	virtual bool IsCheckedOutInOtherBranch(const FString& CurrentBranch = FString()) const override;
	virtual bool IsModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override;
	virtual bool IsCheckedOutOrModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override;
	virtual TArray<FString> GetCheckedOutBranches() const override;
	virtual FString GetOtherUserBranchCheckedOuts() const override;
	virtual bool GetOtherBranchHeadModification(FString& HeadBranchOut, FString& ActionOut, int32& HeadChangeListOut) const override;
	virtual bool IsCurrent() const override;
	virtual bool IsSourceControlled() const override;
	virtual bool IsAdded() const override;
	virtual bool IsDeleted() const override;
	virtual bool IsIgnored() const override;
	virtual bool CanEdit() const override;
	virtual bool CanDelete() const override;
	virtual bool IsUnknown() const override;
	virtual bool IsModified() const override;
	virtual bool CanAdd() const override;
	virtual bool IsConflicted() const override;
	virtual bool CanRevert() const override;
	//~ End ISourceControlState interface

public:
	/** Absolute filename this state represents. */
	FString LocalFilename;

	/** Working-copy state from `lore status`. */
	ELoreWorkingCopyState::Type WorkingCopyState = ELoreWorkingCopyState::Unknown;

	/** Lock state from `lore lock status`. */
	ELoreLockState::Type LockState = ELoreLockState::Unknown;

	/** Owner of a held lock. */
	FString LockUser;

	/** Whether the server holds a newer revision of this file than the working tree. */
	bool bNewerVersionOnServer = false;

	/** Whether the locking workflow is active for this state. */
	bool bUsingLocking = false;

	/** Revision history, populated when an update requests it. */
	TLoreSourceControlHistory History;

#if LORE_UE5_3_OR_LATER
	/** Conflict resolve information, set when the file is conflicted. */
	FResolveInfo PendingResolveInfo;
#endif

	/** Timestamp of last update to this state. */
	FDateTime TimeStamp = FDateTime::MinValue();
};
