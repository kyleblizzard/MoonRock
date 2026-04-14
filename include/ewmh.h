// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// ewmh.h — EWMH Compatibility Shim for Crystal Standalone
// ============================================================================
//
// Crystal's internal code (crystal.c) includes "ewmh.h" for the
// ewmh_get_window_type() function. In AuraOS, this lives in the WM's ewmh.c.
// In standalone Crystal, the function is provided by wm_compat.c and declared
// in wm_compat.h. This header just forwards to wm_compat.h so the existing
// #include "ewmh.h" in crystal.c works without modification.
// ============================================================================

#ifndef CRYSTAL_EWMH_COMPAT_H
#define CRYSTAL_EWMH_COMPAT_H

#include "wm_compat.h"

#endif // CRYSTAL_EWMH_COMPAT_H
