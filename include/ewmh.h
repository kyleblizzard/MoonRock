// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// ewmh.h — EWMH Compatibility Shim for MoonRock Standalone
// ============================================================================
//
// MoonRock's internal code (moonrock.c) includes "ewmh.h" for the
// ewmh_get_window_type() function. In AuraOS, this lives in the WM's ewmh.c.
// In standalone MoonRock, the function is provided by wm_compat.c and declared
// in wm_compat.h. This header just forwards to wm_compat.h so the existing
// #include "ewmh.h" in moonrock.c works without modification.
// ============================================================================

#ifndef MR_EWMH_COMPAT_H
#define MR_EWMH_COMPAT_H

#include "wm_compat.h"

#endif // MR_EWMH_COMPAT_H
