// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlVersion.h"

// Gated to 5.3 with the changelist identifier class, since the changelist surface as a whole needs ISourceControlChangelist's 5.3 methods.
#if LORE_UE5_3_OR_LATER

#include "ISourceControlChangelistState.h"
#include "LoreSourceControlChangelist.h"

// One changelist's state for the editor's Changes view: its title, its description (the work-in-progress commit message), and the resolved states of its files. The provider populates the file states from its cache after reconciling membership. Lore has no shelve, so the shelved set is always empty.
class FLoreSourceControlChangelistState : public ISourceControlChangelistState
{
public:
	explicit FLoreSourceControlChangelistState(const FLoreSourceControlChangelistRef& InChangelist)
		: Changelist(InChangelist)
	{
	}

	virtual FName GetIconName() const override { return NAME_None; }
	virtual FName GetSmallIconName() const override { return NAME_None; }
	virtual FText GetDisplayText() const override { return FText::FromString(Title.IsEmpty() ? Changelist->GetId() : Title); }
	virtual FText GetDescriptionText() const override { return FText::FromString(Description); }
	virtual FText GetDisplayTooltip() const override { return FText::FromString(Description); }
	virtual const FDateTime& GetTimeStamp() const override { return TimeStamp; }

	// The two getters return by value from 5.4 onward and by reference before; the two count methods exist only from 5.4. Verified by reading every installed engine header.
#if LORE_UE5_4_OR_LATER
	virtual const TArray<FSourceControlStateRef> GetFilesStates() const override { return Files; }
	virtual int32 GetFilesStatesNum() const override { return Files.Num(); }
	virtual const TArray<FSourceControlStateRef> GetShelvedFilesStates() const override { return ShelvedFiles; }
	virtual int32 GetShelvedFilesStatesNum() const override { return ShelvedFiles.Num(); }
#else
	virtual const TArray<FSourceControlStateRef>& GetFilesStates() const override { return Files; }
	virtual const TArray<FSourceControlStateRef>& GetShelvedFilesStates() const override { return ShelvedFiles; }
#endif

	virtual FSourceControlChangelistRef GetChangelist() const override { return Changelist; }

	// The Default changelist has no persistent description to edit.
	virtual bool SupportsPersistentDescription() const override { return !Changelist->IsDefault(); }

	FLoreSourceControlChangelistRef Changelist;
	FString Title;
	FString Description;
	TArray<FSourceControlStateRef> Files;
	TArray<FSourceControlStateRef> ShelvedFiles;
	FDateTime TimeStamp = FDateTime::Now();
};

typedef TSharedRef<FLoreSourceControlChangelistState, ESPMode::ThreadSafe> FLoreSourceControlChangelistStateRef;
typedef TSharedPtr<FLoreSourceControlChangelistState, ESPMode::ThreadSafe> FLoreSourceControlChangelistStatePtr;

#endif // LORE_UE5_3_OR_LATER
