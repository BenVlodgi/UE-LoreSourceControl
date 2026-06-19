// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "Runtime/Launch/Resources/Version.h"

// Engine version gates used across the module.

#define LORE_UE5_0_OR_LATER (ENGINE_MAJOR_VERSION >= 5)
#define LORE_UE5_1_OR_LATER ((ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1))
#define LORE_UE5_2_OR_LATER ((ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2))
#define LORE_UE5_3_OR_LATER ((ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3))
#define LORE_UE5_7_OR_LATER ((ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7))
#define LORE_UE5_8_OR_LATER ((ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8))

// IsAtLatestRevision and GetNumLocalChanges are pure from 5.1 and become final at 5.8.
#define LORE_UE_HAS_LATEST_REVISION_QUERY (LORE_UE5_1_OR_LATER && !LORE_UE5_8_OR_LATER)
