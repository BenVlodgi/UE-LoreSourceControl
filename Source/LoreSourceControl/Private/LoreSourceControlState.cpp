// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlState.h"

#if LORE_UE5_2_OR_LATER && SOURCE_CONTROL_WITH_SLATE
#include "RevisionControlStyle/RevisionControlStyle.h"
#endif

#define LOCTEXT_NAMESPACE "LoreSourceControl.State"

int32 FLoreSourceControlState::GetHistorySize() const
{
	return History.Num();
}

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FLoreSourceControlState::GetHistoryItem(int32 HistoryIndex) const
{
	if (History.IsValidIndex(HistoryIndex))
	{
		return History[HistoryIndex];
	}

	return nullptr;
}

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FLoreSourceControlState::FindHistoryRevision(int32 RevisionNumber) const
{
	for (const auto& Revision : History)
	{
		if (Revision->GetRevisionNumber() == RevisionNumber)
		{
			return Revision;
		}
	}

	return nullptr;
}

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FLoreSourceControlState::FindHistoryRevision(const FString& InRevision) const
{
	for (const auto& Revision : History)
	{
		if (Revision->GetRevision() == InRevision || Revision->GetRevisionNumber() == FCString::Atoi(*InRevision))
		{
			return Revision;
		}
	}

	return nullptr;
}

#if LORE_UE5_2_OR_LATER
TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FLoreSourceControlState::GetCurrentRevision() const
{
	if (History.Num() > 0)
	{
		return History[0];
	}

	return nullptr;
}
#endif

#if LORE_UE5_3_OR_LATER
ISourceControlState::FResolveInfo FLoreSourceControlState::GetResolveInfo() const
{
	return PendingResolveInfo;
}
#else
TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FLoreSourceControlState::GetBaseRevForMerge() const
{
	return nullptr;
}
#endif

#if LORE_UE5_0_OR_LATER
#if SOURCE_CONTROL_WITH_SLATE
FSlateIcon FLoreSourceControlState::GetIcon() const
{
#if !LORE_UE5_2_OR_LATER
	// The revision control style set exists from 5.2 on; no icon before that.
	return FSlateIcon();
#else
	const FName StyleSet = FRevisionControlStyleManager::GetStyleSetName();
	if (LockState == ELoreLockState::LockedOther)
	{
		return FSlateIcon(StyleSet, "RevisionControl.CheckedOutByOtherUser");
	}

	if (!IsCurrent())
	{
		return FSlateIcon(StyleSet, "RevisionControl.NotAtHeadRevision");
	}

	switch (WorkingCopyState)
	{
	case ELoreWorkingCopyState::Added:        return FSlateIcon(StyleSet, "RevisionControl.OpenForAdd");
	case ELoreWorkingCopyState::Deleted:      return FSlateIcon(StyleSet, "RevisionControl.MarkedForDelete");
	case ELoreWorkingCopyState::Conflicted:   return FSlateIcon(StyleSet, "RevisionControl.Conflicted");
	case ELoreWorkingCopyState::NotControlled: return FSlateIcon(StyleSet, "RevisionControl.NotInDepot");
	case ELoreWorkingCopyState::Modified:
	case ELoreWorkingCopyState::Moved:
	case ELoreWorkingCopyState::Copied:
		if (LockState == ELoreLockState::Locked)
		{
			return FSlateIcon(StyleSet, "RevisionControl.CheckedOut");
		}
		return FSlateIcon(StyleSet, "RevisionControl.CheckedOut");
	default:
		if (LockState == ELoreLockState::Locked)
		{
			return FSlateIcon(StyleSet, "RevisionControl.CheckedOut");
		}
		return FSlateIcon();
	}
#endif
}
#endif
#else
FName FLoreSourceControlState::GetIconName() const
{
	return NAME_None;
}

FName FLoreSourceControlState::GetSmallIconName() const
{
	return NAME_None;
}
#endif

FText FLoreSourceControlState::GetDisplayName() const
{
	if (LockState == ELoreLockState::LockedOther)
	{
		return FText::Format(LOCTEXT("LockedOther", "Checked out by {0}"), FText::FromString(LockUser));
	}

	switch (WorkingCopyState)
	{
	case ELoreWorkingCopyState::Unchanged:     return LOCTEXT("Unchanged", "Unchanged");
	case ELoreWorkingCopyState::Added:         return LOCTEXT("Added", "Added");
	case ELoreWorkingCopyState::Modified:      return LOCTEXT("Modified", "Modified");
	case ELoreWorkingCopyState::Deleted:       return LOCTEXT("Deleted", "Deleted");
	case ELoreWorkingCopyState::Moved:         return LOCTEXT("Moved", "Moved");
	case ELoreWorkingCopyState::Copied:        return LOCTEXT("Copied", "Copied");
	case ELoreWorkingCopyState::Conflicted:    return LOCTEXT("Conflicted", "Conflicted");
	case ELoreWorkingCopyState::NotControlled: return LOCTEXT("NotControlled", "Not under version control");
	case ELoreWorkingCopyState::Ignored:       return LOCTEXT("Ignored", "Ignored");
	default:                                    return LOCTEXT("Unknown", "Unknown");
	}
}

FText FLoreSourceControlState::GetDisplayTooltip() const
{
	if (LockState == ELoreLockState::LockedOther)
	{
		return FText::Format(LOCTEXT("LockedOtherTooltip", "Locked for edit by {0}"), FText::FromString(LockUser));
	}

	if (!IsCurrent())
	{
		return LOCTEXT("NotCurrentTooltip", "The server holds a newer revision of this file");
	}

	return GetDisplayName();
}

const FString& FLoreSourceControlState::GetFilename() const { return LocalFilename; }
const FDateTime& FLoreSourceControlState::GetTimeStamp() const { return TimeStamp; }

bool FLoreSourceControlState::CanCheckIn() const
{
	if (IsConflicted() || !IsCurrent())
	{
		return false;
	}

	if (bUsingLocking)
	{
		return LockState == ELoreLockState::Locked || IsAdded() || IsDeleted();
	}

	return IsModified() || IsAdded() || IsDeleted();
}

bool FLoreSourceControlState::CanCheckout() const
{
	if (!bUsingLocking)
	{
		return false;
	}

	const bool bEditable = WorkingCopyState == ELoreWorkingCopyState::Unchanged || WorkingCopyState == ELoreWorkingCopyState::Modified;
	return IsSourceControlled() && bEditable && LockState == ELoreLockState::NotLocked && IsCurrent();
}

bool FLoreSourceControlState::IsCheckedOut() const
{
	return bUsingLocking ? (LockState == ELoreLockState::Locked) : false;
}

bool FLoreSourceControlState::IsCheckedOutOther(FString* Who) const
{
	if (LockState == ELoreLockState::LockedOther)
	{
		if (Who != nullptr)
		{
			*Who = LockUser;
		}

		return true;
	}

	return false;
}

bool FLoreSourceControlState::IsCheckedOutInOtherBranch(const FString& CurrentBranch) const { return false; }
bool FLoreSourceControlState::IsModifiedInOtherBranch(const FString& CurrentBranch) const { return false; }
bool FLoreSourceControlState::IsCheckedOutOrModifiedInOtherBranch(const FString& CurrentBranch) const { return false; }
TArray<FString> FLoreSourceControlState::GetCheckedOutBranches() const { return TArray<FString>(); }
FString FLoreSourceControlState::GetOtherUserBranchCheckedOuts() const { return FString(); }
bool FLoreSourceControlState::GetOtherBranchHeadModification(FString& HeadBranchOut, FString& ActionOut, int32& HeadChangeListOut) const { return false; }

bool FLoreSourceControlState::IsCurrent() const { return !bNewerVersionOnServer; }

bool FLoreSourceControlState::IsSourceControlled() const
{
	return WorkingCopyState != ELoreWorkingCopyState::NotControlled
		&& WorkingCopyState != ELoreWorkingCopyState::Unknown
		&& WorkingCopyState != ELoreWorkingCopyState::Ignored;
}

bool FLoreSourceControlState::IsAdded() const { return WorkingCopyState == ELoreWorkingCopyState::Added; }
bool FLoreSourceControlState::IsDeleted() const { return WorkingCopyState == ELoreWorkingCopyState::Deleted; }
bool FLoreSourceControlState::IsIgnored() const { return WorkingCopyState == ELoreWorkingCopyState::Ignored; }

bool FLoreSourceControlState::CanEdit() const
{
	if (!bUsingLocking)
	{
		return true;
	}

	return LockState == ELoreLockState::Locked || IsAdded() || !IsSourceControlled();
}

bool FLoreSourceControlState::CanDelete() const
{
	return IsSourceControlled() && !IsCheckedOutOther();
}

bool FLoreSourceControlState::IsUnknown() const { return WorkingCopyState == ELoreWorkingCopyState::Unknown; }

bool FLoreSourceControlState::IsModified() const
{
	return WorkingCopyState == ELoreWorkingCopyState::Added
		|| WorkingCopyState == ELoreWorkingCopyState::Modified
		|| WorkingCopyState == ELoreWorkingCopyState::Deleted
		|| WorkingCopyState == ELoreWorkingCopyState::Moved
		|| WorkingCopyState == ELoreWorkingCopyState::Copied
		|| WorkingCopyState == ELoreWorkingCopyState::Conflicted;
}

bool FLoreSourceControlState::CanAdd() const { return WorkingCopyState == ELoreWorkingCopyState::NotControlled; }

bool FLoreSourceControlState::IsConflicted() const
{
#if LORE_UE5_3_OR_LATER
	return WorkingCopyState == ELoreWorkingCopyState::Conflicted || PendingResolveInfo.IsValid();
#else
	return WorkingCopyState == ELoreWorkingCopyState::Conflicted;
#endif
}

bool FLoreSourceControlState::CanRevert() const
{
	return IsModified() || LockState == ELoreLockState::Locked;
}

#undef LOCTEXT_NAMESPACE
