// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

// The cbindgen-generated lore.h has no C linkage guard (cpp_compat is off), so wrap it for C++.
extern "C"
{
#include "lore.h"
}
