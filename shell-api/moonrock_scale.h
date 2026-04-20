// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/
// ============================================================================
//  MoonRock -> Shell scale bridge — public contract
// ============================================================================
//
// MoonRock is the single source of truth for per-output HiDPI scale. EDID
// defaults, user overrides, and persistence all live inside moonrock_display.c.
//
// Standalone shell components (menubar, dock, desktop, systemcontrol) are
// separate X11 processes and cannot call into MoonRock's C API. To let them
// observe per-output scale without introducing a second daemon or a new IPC
// socket, MoonRock publishes a line-oriented text property on the root window.
//
//     Atom:   _MOONROCK_OUTPUT_SCALES           (type: UTF8_STRING, format 8)
//     Format: one line per connected output, newline-terminated:
//
//         <output_name> <x> <y> <width> <height> <scale>
//
//     Example:
//         eDP-1 0 0 1920 1200 1.500
//         HDMI-1 1920 0 1920 1080 1.000
//
// The property is rewritten whenever the connected-output set changes
// (hotplug) or whenever the user changes a scale through SysPrefs (which in
// turn calls display_set_scale_for_output()). Xlib converts a root-property
// write into a PropertyNotify event on every client that has selected
// PropertyChangeMask on the root window, so subscribers get live updates for
// free.
//
// Geometry is in physical pixels in the virtual-screen coordinate space —
// the same space XRandR uses. This is so clients can pick the output that
// contains a given root-space window origin without a second round-trip.
// The scale is the final effective factor (user override if any, otherwise
// the EDID-derived default).
// ============================================================================

#ifndef MOONROCK_SCALE_H
#define MOONROCK_SCALE_H

#include <X11/Xlib.h>
#include <stdbool.h>

// Atom name for the root-window property. The only shared string between
// publisher (moonrock) and subscribers (shell components) — every other
// constant below is client-side.
#define MOONROCK_SCALE_ATOM_NAME "_MOONROCK_OUTPUT_SCALES"

// Reverse-direction atom for writing a scale change request back to MoonRock.
// systemcontrol's Displays pane writes a single line:
//
//     <output_name> <scale>\n     e.g. "eDP-1 1.500\n"
//
// on this atom (UTF8_STRING, format 8). MoonRock's event loop sees the
// PropertyNewValue notification, parses the line, calls
// display_set_scale_for_output(), and then deletes the property so a
// second write of the same value still generates a PropertyNotify. A scale
// of 0 clears the user override and reverts to the EDID-derived default.
#define MOONROCK_SET_SCALE_ATOM_NAME "_MOONROCK_SET_OUTPUT_SCALE"

// Cap on how many outputs we parse into a single table. Matches
// MAX_OUTPUTS on the publisher side so we never drop a legitimate entry.
#define MOONROCK_SCALE_MAX_OUTPUTS 16

// Fixed-size buffer for each output's human-readable name ("eDP-1",
// "HDMI-1", "DP-2-1", …). XRandR output names rarely exceed 16 bytes so
// 64 is comfortable headroom.
#define MOONROCK_SCALE_NAME_MAX 64


// One parsed line from the property — a snapshot of one connected output.
typedef struct {
    char  name[MOONROCK_SCALE_NAME_MAX];
    int   x, y;                 // top-left in virtual-screen pixels
    int   width, height;        // pixel size
    float scale;                // effective scale (≥ 0.5, ≤ 4.0 in practice)
} MoonRockOutputScale;

// Full parsed table. `count` may be zero if the property is missing (e.g.
// MoonRock hasn't started yet) or malformed; callers should treat that
// case as "scale is 1.0 for all points."
typedef struct {
    MoonRockOutputScale outputs[MOONROCK_SCALE_MAX_OUTPUTS];
    int                 count;
    bool                valid;
} MoonRockScaleTable;


// Intern the atom and enable PropertyChangeMask on the root window so the
// calling process starts receiving PropertyNotify events for the scale
// property. Existing event-mask bits on the root window are preserved —
// we're additive, never destructive. Safe to call multiple times; only
// the first call does work.
//
// Returns true on success, false if `dpy` is NULL.
bool moonrock_scale_init(Display *dpy);

// Return the interned atom for _MOONROCK_OUTPUT_SCALES. Calling this before
// moonrock_scale_init() will intern it on demand. Useful for comparing
// against ev.xproperty.atom in an event loop.
Atom moonrock_scale_atom(Display *dpy);

// Read the current property value from the root window and parse it into
// `out`. Call once during startup (after moonrock_scale_init) and again
// from every PropertyNotify where ev.xproperty.atom == moonrock_scale_atom.
//
// On failure (property missing or malformed) `out->valid` is set to false
// and `out->count` to 0. The function still returns false in that case so
// callers can distinguish "no data" from "got data."
bool moonrock_scale_refresh(Display *dpy, MoonRockScaleTable *out);

// Look up the scale for a point in virtual-screen coordinates. Returns 1.0
// if the table is invalid or the point is outside every output.
float moonrock_scale_for_point(const MoonRockScaleTable *table, int x, int y);

// Look up the scale for an output by its name (e.g. "eDP-1"). Returns 1.0
// if the table is invalid or no output matches.
float moonrock_scale_for_name(const MoonRockScaleTable *table, const char *name);


// ── Requester — systemcontrol Displays pane → MoonRock ──────────────────
// Writes _MOONROCK_SET_OUTPUT_SCALE on the root window so MoonRock picks
// up a user-initiated scale change. MoonRock owns the EDID hash + config
// persistence — the pane only sends the requested pair.
//
//   output_name — the same name MoonRock publishes in the scale table
//                 (e.g. "eDP-1"). Case-sensitive exact match.
//   scale       — desired effective scale (0.5 – 4.0), or 0.0 to clear the
//                 user override and fall back to the EDID-derived default.
//
// Returns true on a successful X write (MoonRock may still reject an
// out-of-range value; check the published scale table on the next
// PropertyNotify to confirm).
bool moonrock_request_scale(Display *dpy, const char *output_name, float scale);

#endif // MOONROCK_SCALE_H
