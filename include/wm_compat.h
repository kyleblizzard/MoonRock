// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/

// ============================================================================
// wm_compat.h — Window Manager Compatibility Shim for MoonRock Standalone
// ============================================================================
//
// MoonRock was originally developed inside AuraOS's window manager (aura-wm),
// where it had direct access to the WM's AuraWM and Client structs. When
// MoonRock was split into a standalone library, the internal code still
// referenced those types throughout ~11,000 lines of C.
//
// Rather than rewriting every internal module to remove AuraWM/Client
// references (which would be error-prone and break the existing code), we
// provide this compatibility shim. It defines minimal versions of the types
// that MoonRock's internal code needs, with only the fields that are actually
// accessed.
//
// IMPORTANT: When building inside AuraOS (where wm.h defines the full
// AuraWM and Client structs), this header detects that wm.h was already
// included and skips its own struct definitions to avoid conflicts. It only
// provides the struct definitions when building MoonRock standalone.
//
// The public API (moonrock_api.h) never exposes these types — users of
// libmoonrock only interact with MRWindow structs. The moonrock_api.c
// adapter translates between the public API and these internal types.
//
// This header is INTERNAL to MoonRock. Window managers should include
// moonrock_api.h instead.
// ============================================================================

#ifndef WM_COMPAT_H
#define WM_COMPAT_H

#include <stdbool.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

// ============================================================================
// Detect if we're building inside AuraOS
// ============================================================================
//
// When building MoonRock modules as part of AuraOS (not standalone), the WM
// provides the real AuraWM and Client structs in wm.h. In that case, define
// MR_EMBEDDED_IN_WM at compile time (via -DMR_EMBEDDED_IN_WM) and
// this header becomes a no-op — all types and functions come from the WM.
//
// We also check for AURA_WM_H (wm.h's include guard) as a fallback in case
// wm.h was included before this header.

// When embedded in AuraOS, include the real wm.h (which provides AuraWM,
// Client, and all WM functions) and skip our minimal definitions.
#if defined(MR_EMBEDDED_IN_WM) && !defined(AURA_WM_H)
#include "wm.h"
#include "ewmh.h"
#endif

#if !defined(MR_EMBEDDED_IN_WM) && !defined(AURA_WM_H)

// ── Standalone mode: define our own minimal types ─────────────────────

// Maximum number of windows MoonRock can track simultaneously.
#define MAX_CLIENTS 256

// Frame geometry constants used by MoonRock's session restore code.
#define TITLEBAR_HEIGHT  22
#define BORDER_WIDTH      1

typedef struct Client Client;
typedef struct AuraWM AuraWM;

// Client — minimal window description.
// MoonRock only accesses these fields from the full AuraOS Client struct.
struct Client {
    Window client;       // The application's actual window
    Window frame;        // The WM's frame window (parent of client)
    int x, y;            // Frame position on screen
    int w, h;            // Client content size
    bool mapped;         // Is the window currently visible?
    bool focused;        // Is this the focused/active window?
    Atom wm_type;        // _NET_WM_WINDOW_TYPE value
    char wm_class[128];  // WM_CLASS instance name (for window rules)
    char wm_class_name[128]; // WM_CLASS class name
};

// AuraWM — minimal window manager state.
// MoonRock needs the X display, screen info, client list, and EWMH atoms.
struct AuraWM {
    Display *dpy;
    int screen;
    Window root;
    int root_w, root_h;

    // Client tracking
    Client clients[MAX_CLIENTS];
    int num_clients;
    Client *focused;

    // EWMH atoms that MoonRock's internal code references
    Atom atom_net_wm_type;
    Atom atom_net_wm_type_normal;
    Atom atom_net_wm_type_dock;
    Atom atom_net_wm_type_desktop;
    Atom atom_net_wm_type_dialog;
    Atom atom_net_wm_type_splash;
    Atom atom_net_wm_type_utility;
    Atom atom_net_wm_state;
    Atom atom_net_wm_state_fullscreen;
    Atom atom_net_wm_state_hidden;
    Atom atom_net_active_window;
    Atom atom_net_client_list;
    Atom atom_net_client_list_stacking;
    Atom atom_wm_protocols;
    Atom atom_wm_delete;
};

// ── Standalone helper functions ───────────────────────────────────────
// These are only needed in standalone mode. When building inside AuraOS,
// the real wm.c and ewmh.c provide these functions.

// Find a client by its application window ID.
Client *wm_find_client(AuraWM *wm, Window win);

// Find a client by its frame window ID.
Client *wm_find_client_by_frame(AuraWM *wm, Window frame);

// Get the _NET_WM_WINDOW_TYPE property for a window.
Atom ewmh_get_window_type(AuraWM *wm, Window w);

// Focus a client window (used by Mission Control on exit).
void wm_focus_client(AuraWM *wm, Client *c);

// Initialize EWMH atoms on the given display.
void wm_compat_init_atoms(AuraWM *wm);

#endif // !MR_EMBEDDED_IN_WM && !AURA_WM_H

#endif // WM_COMPAT_H
