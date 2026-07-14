// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlVersion.h"

// ISourceControlChangelist gained its overridable methods (GetIdentifier, CanDelete, IsDefault) at UE 5.3; on 5.0 through 5.2 it is only a destructor. So the plugin's changelist surface, and UsesChangelists, is 5.3 and later, and the uncontrolled-changelist fallback covers 4.27 through 5.2. Verified by a packaging build: 5.2 fails to override, 5.3 compiles.
#if LORE_UE5_3_OR_LATER

#include "ISourceControlChangelist.h"
#include "LoreChangelistStore.h"

// A plugin changelist identifier the editor's Changes view holds. Keyed by the changelist id string, matching the plugin changelist record. The string-identifier model follows the Plastic provider rather than Perforce's numeric ids; Lore has one staged set and no native changelist.
class FLoreSourceControlChangelist : public ISourceControlChangelist
{
public:
	FLoreSourceControlChangelist() = default;
	explicit FLoreSourceControlChangelist(const FString& InId) : Id(InId) {}

	virtual bool CanDelete() const override { return !IsDefault(); }
	virtual FString GetIdentifier() const override { return Id; }
	virtual bool IsDefault() const override { return Id == LoreChangelists::DefaultId; }

	const FString& GetId() const { return Id; }

private:
	FString Id;
};

typedef TSharedRef<FLoreSourceControlChangelist, ESPMode::ThreadSafe> FLoreSourceControlChangelistRef;
typedef TSharedPtr<FLoreSourceControlChangelist, ESPMode::ThreadSafe> FLoreSourceControlChangelistPtr;

#endif // LORE_UE5_3_OR_LATER
