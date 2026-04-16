// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/

// ============================================================================
// wm_compat.c — Window Manager Compatibility Implementations
// ============================================================================
//
// These functions replicate the minimal WM helper functions that MoonRock's
// internal code calls. When MoonRock is used inside AuraOS, the real wm.c and
// ewmh.c provide these. When MoonRock is built standalone (libmoonrock.so),
// these implementations are used instead.
//
// The functions are intentionally simple — they do exactly what MoonRock needs
// and nothing more.
// ============================================================================

#define _GNU_SOURCE
#include "wm_compat.h"

// When building inside AuraOS, the real wm.c and ewmh.c provide all these
// functions. This file only compiles its implementations in standalone mode.
#if !defined(MR_EMBEDDED_IN_WM) && !defined(AURA_WM_H)

#include <string.h>
#include <stdio.h>
#include <X11/Xatom.h>

// ============================================================================
// Client lookup functions
// ============================================================================

// Find a client by its application window ID.
// Searches the client array linearly — with MAX_CLIENTS=256, this is fast.
Client *wm_find_client(AuraWM *wm, Window win)
{
    if (!wm || !win) return NULL;

    for (int i = 0; i < wm->num_clients; i++) {
        if (wm->clients[i].client == win) {
            return &wm->clients[i];
        }
    }
    return NULL;
}

// Find a client by its frame window ID.
// The frame is the WM's wrapper window that contains the titlebar + client.
Client *wm_find_client_by_frame(AuraWM *wm, Window frame)
{
    if (!wm || !frame) return NULL;

    for (int i = 0; i < wm->num_clients; i++) {
        if (wm->clients[i].frame == frame) {
            return &wm->clients[i];
        }
    }
    return NULL;
}

// ============================================================================
// EWMH helpers
// ============================================================================

// Get the _NET_WM_WINDOW_TYPE property for a window.
// This tells us whether the window is a normal app, a dock/panel, the desktop
// background, etc. MoonRock uses this for z-order pass assignment.
Atom ewmh_get_window_type(AuraWM *wm, Window w)
{
    if (!wm || !w) return wm ? wm->atom_net_wm_type_normal : None;

    Atom type_ret;
    int fmt;
    unsigned long nitems, after;
    unsigned char *data = NULL;

    // Read the first atom from _NET_WM_WINDOW_TYPE.
    // XGetWindowProperty returns Success on success, and populates data
    // with the property value. We only need the first item (nitems=1).
    if (XGetWindowProperty(wm->dpy, w, wm->atom_net_wm_type,
                           0, 1, False, XA_ATOM,
                           &type_ret, &fmt, &nitems, &after, &data) == Success
        && data && nitems > 0) {
        Atom result = *(Atom *)data;
        XFree(data);
        return result;
    }

    if (data) XFree(data);

    // Default to normal window type if the property isn't set.
    return wm->atom_net_wm_type_normal;
}

// ============================================================================
// Focus management
// ============================================================================

// Focus a client window. Used by Mission Control when the user clicks a
// window to exit the overview. In standalone mode we do a basic raise + focus.
void wm_focus_client(AuraWM *wm, Client *c)
{
    if (!wm || !c || !c->frame) return;

    // Raise the frame to the top of the stacking order
    XRaiseWindow(wm->dpy, c->frame);

    // Set input focus to the client's application window
    XSetInputFocus(wm->dpy, c->client, RevertToPointerRoot, CurrentTime);

    // Update our internal focus tracking
    if (wm->focused) {
        wm->focused->focused = false;
    }
    c->focused = true;
    wm->focused = c;

    XFlush(wm->dpy);
}

// ============================================================================
// Atom initialization
// ============================================================================

// Intern all the EWMH atoms that MoonRock's internal code uses.
// XInternAtom asks the X server for the numeric ID corresponding to an atom
// name string. We do this once at init time and cache the results.
void wm_compat_init_atoms(AuraWM *wm)
{
    if (!wm || !wm->dpy) return;

    Display *dpy = wm->dpy;

    wm->atom_net_wm_type            = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    wm->atom_net_wm_type_normal     = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    wm->atom_net_wm_type_dock       = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    wm->atom_net_wm_type_desktop    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    wm->atom_net_wm_type_dialog     = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    wm->atom_net_wm_type_splash     = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    wm->atom_net_wm_type_utility    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    wm->atom_net_wm_state           = XInternAtom(dpy, "_NET_WM_STATE", False);
    wm->atom_net_wm_state_fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    wm->atom_net_wm_state_hidden    = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
    wm->atom_net_active_window      = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    wm->atom_net_client_list        = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    wm->atom_net_client_list_stacking = XInternAtom(dpy, "_NET_CLIENT_LIST_STACKING", False);
    wm->atom_wm_protocols           = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wm->atom_wm_delete              = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
}

#endif // !MR_EMBEDDED_IN_WM && !AURA_WM_H
