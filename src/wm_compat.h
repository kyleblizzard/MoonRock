// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/
// ============================================================================
// wm_compat.h — Bridge between MoonRock compositor and moonrock types
// ============================================================================
//
// Purpose:
//   The MoonRock compositor files (moonrock.c, moonrock_display.c, etc.) need
//   access to the CCWM and Client structs defined in wm.h. This header provides
//   that bridge so MoonRock files can reference WM types without depending on
//   the full WM implementation.
//
//   When MoonRock is compiled as part of moonrock (the normal case), this simply
//   includes wm.h. The MR_EMBEDDED_IN_WM define is set by the meson build.
// ============================================================================

#ifndef WM_COMPAT_H
#define WM_COMPAT_H

#ifdef MR_EMBEDDED_IN_WM
// Compiled as part of moonrock — include the real WM header
#include "wm.h"
#else
// Standalone build — provide minimal forward declarations
#include <X11/Xlib.h>
#include <stdbool.h>

typedef struct Client Client;
typedef struct CCWM CCWM;

struct Client {
    Window client;
    Window frame;
    int x, y, w, h;
    bool mapped;
    bool focused;
    char title[256];
};

struct CCWM {
    Display *dpy;
    int screen;
    Window root;
    int root_w, root_h;
    bool running;
};
#endif

#endif // WM_COMPAT_H
