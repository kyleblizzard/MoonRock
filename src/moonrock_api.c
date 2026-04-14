// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// moonrock_api.c — Public API Implementation for MoonRock Compositor
// ============================================================================
//
// This file implements the public API declared in moonrock_api.h. It acts as
// an adapter between the clean, WM-agnostic public interface and MoonRock's
// internal modules which were originally written for AuraOS's window manager.
//
// The translation works like this:
//
//   1. mr_api_init() creates an internal AuraWM struct (the compat shim)
//      and passes it to mr_init(). External callers never see AuraWM.
//
//   2. mr_api_composite() receives an array of MRWindow structs
//      from the WM. It translates each one into the internal Client format,
//      updates the compat AuraWM's client list, then calls mr_composite().
//
//   3. Animation, Mission Control, and event functions delegate directly to
//      the internal modules with the stored AuraWM pointer.
//
// This means ANY X11 window manager can use MoonRock — not just AuraOS.
// The WM just fills in MRWindow structs and calls the API.
// ============================================================================

#define _GNU_SOURCE
#include "moonrock_api.h"
#include "wm_compat.h"
#include "moonrock.h"
#include "moonrock_anim.h"
#include "moonrock_mission_control.h"
#include "moonrock_touch.h"
#include "moonrock_display.h"
#include "moonrock_color.h"
#include "moonrock_plugin.h"
#include "moonrock_robust.h"
#include "moonrock_shaders.h"

#include <string.h>
#include <stdio.h>
#include <X11/Xutil.h>

// ============================================================================
// Module state
// ============================================================================
//
// MoonRock's public API manages a single compositor instance. The internal
// AuraWM struct is our bridge to the internal code — we own it, populate it
// from MRWindow arrays, and pass it to the internal functions.

static struct {
    bool initialized;        // Has mr_api_init() been called successfully?
    AuraWM wm;               // Internal WM compat struct (owned by us)
    bool needs_composite;    // Does the next frame need a composite pass?
    bool needs_restack;      // Has the window list changed?
} api_state = {0};

// ============================================================================
// Lifecycle
// ============================================================================

bool mr_api_init(Display *dpy, int screen)
{
    if (api_state.initialized) {
        fprintf(stderr, "[moonrock_api] Already initialized\n");
        return true;
    }

    if (!dpy) {
        fprintf(stderr, "[moonrock_api] NULL display\n");
        return false;
    }

    // Zero out the compat WM struct and populate it with the display info.
    memset(&api_state.wm, 0, sizeof(api_state.wm));
    api_state.wm.dpy    = dpy;
    api_state.wm.screen = screen;
    api_state.wm.root   = RootWindow(dpy, screen);
    api_state.wm.root_w = DisplayWidth(dpy, screen);
    api_state.wm.root_h = DisplayHeight(dpy, screen);

    // Intern all the EWMH atoms MoonRock's internal code needs.
    wm_compat_init_atoms(&api_state.wm);

    // Initialize the core compositor (GLX context, XComposite redirect,
    // damage tracking, shadow kernel, etc.)
    if (!mr_init(&api_state.wm)) {
        fprintf(stderr, "[moonrock_api] mr_init() failed\n");
        return false;
    }

    // Initialize the animation framework
    anim_init();

    api_state.initialized = true;
    api_state.needs_composite = true;

    fprintf(stderr, "[moonrock_api] MoonRock Compositor initialized "
            "(screen %d, %dx%d)\n",
            screen, api_state.wm.root_w, api_state.wm.root_h);

    return true;
}

void mr_api_shutdown(void)
{
    if (!api_state.initialized) return;

    // Shut down subsystems in reverse order of initialization
    mc_shutdown(&api_state.wm);
    mr_shutdown(&api_state.wm);

    memset(&api_state, 0, sizeof(api_state));
    fprintf(stderr, "[moonrock_api] MoonRock Compositor shut down\n");
}

bool mr_api_is_active(void)
{
    return api_state.initialized && mr_is_active();
}

// ============================================================================
// Window list translation
// ============================================================================
//
// The public API receives MRWindow arrays from the WM. We translate
// these into the internal Client format so the existing moonrock.c code can
// use them without modification.
//
// This is called at the start of each mr_api_composite() call.

static void sync_clients_from_api(MRWindow *windows, int count)
{
    // Clamp to our internal limit
    if (count > MAX_CLIENTS) {
        count = MAX_CLIENTS;
    }

    // Rebuild the internal client list from the MRWindow array.
    // We also track which client is focused so the shadow intensity is correct.
    api_state.wm.num_clients = count;
    api_state.wm.focused = NULL;

    for (int i = 0; i < count; i++) {
        Client *c = &api_state.wm.clients[i];
        MRWindow *w = &windows[i];

        c->frame   = w->window_id;
        c->client  = w->window_id;  // In API mode, frame == client
        c->x       = w->x;
        c->y       = w->y;
        c->w       = w->w;
        c->h       = w->h;
        c->mapped  = true;          // Only visible windows are passed to us
        c->focused = w->focused;

        // Translate MRWindowType to EWMH atom for internal z-order logic
        switch (w->type) {
            case MR_WINDOW_DESKTOP:
                c->wm_type = api_state.wm.atom_net_wm_type_desktop;
                break;
            case MR_WINDOW_DOCK:
                c->wm_type = api_state.wm.atom_net_wm_type_dock;
                break;
            case MR_WINDOW_POPUP:
                c->wm_type = api_state.wm.atom_net_wm_type_normal;
                break;
            case MR_WINDOW_NORMAL:
            default:
                c->wm_type = api_state.wm.atom_net_wm_type_normal;
                break;
        }

        c->wm_class[0] = '\0';
        c->wm_class_name[0] = '\0';

        if (w->focused) {
            api_state.wm.focused = c;
        }
    }
}

// ============================================================================
// Per-frame compositing
// ============================================================================

void mr_api_composite(MRWindow *windows, int count)
{
    if (!api_state.initialized) return;

    // Translate the public MRWindow array into the internal Client list
    sync_clients_from_api(windows, count);

    // Let the internal compositor do its thing — it reads from api_state.wm
    // to find windows, textures, stacking order, etc.
    mr_composite(&api_state.wm);

    // Reset the dirty flag after compositing
    api_state.needs_composite = false;
}

bool mr_api_needs_composite(void)
{
    if (!api_state.initialized) return false;

    // We need to composite if:
    // 1. Something marked us dirty (window map/unmap/resize/damage)
    // 2. An animation is currently playing
    // 3. Mission Control is active (continuous rendering for transitions)
    return api_state.needs_composite
        || anim_any_active()
        || mc_is_active();
}

void mr_api_mark_dirty(void)
{
    api_state.needs_composite = true;
}

// ============================================================================
// Animations
// ============================================================================

void mr_api_minimize(Window texture_window, int dock_x, int dock_y)
{
    if (!api_state.initialized) return;

    // Find the internal Client for this window
    Client *c = wm_find_client(&api_state.wm, texture_window);
    if (!c) {
        c = wm_find_client_by_frame(&api_state.wm, texture_window);
    }

    if (c) {
        mr_animate_minimize(&api_state.wm, c, dock_x, dock_y);
        api_state.needs_composite = true;
    }
}

void mr_api_restore(Window texture_window, int dock_x, int dock_y)
{
    if (!api_state.initialized) return;

    Client *c = wm_find_client(&api_state.wm, texture_window);
    if (!c) {
        c = wm_find_client_by_frame(&api_state.wm, texture_window);
    }

    if (c) {
        mr_animate_restore(&api_state.wm, c, dock_x, dock_y);
        api_state.needs_composite = true;
    }
}

void mr_api_fade_in(Window texture_window, double duration_sec)
{
    if (!api_state.initialized) return;

    // Use the animation framework's fade-in. We find the window texture
    // and start a fade animation on it.
    // The internal anim system works with GL texture IDs, so we delegate
    // through the framework.
    (void)texture_window;
    (void)duration_sec;
    // TODO: Wire to anim_start() with ANIM_FADE type once the internal
    // animation system supports it directly via Window ID lookup.
    api_state.needs_composite = true;
}

void mr_api_fade_out(Window texture_window, double duration_sec)
{
    if (!api_state.initialized) return;

    (void)texture_window;
    (void)duration_sec;
    // TODO: Wire to anim_start() with ANIM_FADE type (reverse direction)
    api_state.needs_composite = true;
}

bool mr_api_animation_active(void)
{
    if (!api_state.initialized) return false;
    return anim_any_active();
}

// ============================================================================
// Mission Control
// ============================================================================

void mr_api_toggle_mission_control(void)
{
    if (!api_state.initialized) return;
    mc_toggle(&api_state.wm);
    api_state.needs_composite = true;
}

bool mr_api_mission_control_active(void)
{
    if (!api_state.initialized) return false;
    return mc_is_active();
}

int mr_api_add_space(void)
{
    if (!api_state.initialized) return -1;
    return mc_add_space(&api_state.wm);
}

void mr_api_switch_space(int space_id)
{
    if (!api_state.initialized) return;
    mc_switch_space(&api_state.wm, space_id);
    api_state.needs_composite = true;
}

void mr_api_move_to_space(Window win, int space_id)
{
    if (!api_state.initialized) return;
    mc_move_window_to_space(&api_state.wm, win, space_id);
    api_state.needs_composite = true;
}

// ============================================================================
// Event handling
// ============================================================================

bool mr_api_handle_event(XEvent *ev)
{
    if (!api_state.initialized || !ev) return false;

    // Let the core compositor handle damage events
    if (mr_handle_event(&api_state.wm, ev)) {
        api_state.needs_composite = true;
        return true;
    }

    // Let Mission Control handle keyboard/mouse events
    if (mc_is_active() && mc_handle_event(&api_state.wm, ev)) {
        api_state.needs_composite = true;
        return true;
    }

    return false;
}

// ============================================================================
// Display
// ============================================================================

float mr_api_get_scale_factor(void)
{
    if (!api_state.initialized) return 1.0f;

    // Use the color/display module's DPI detection if available,
    // otherwise calculate from the X server's DPI.
    int dpi = (int)(((double)api_state.wm.root_w * 25.4) /
                    (double)DisplayWidthMM(api_state.wm.dpy, api_state.wm.screen));

    // Standard DPI is 96. Anything above that is HiDPI.
    if (dpi <= 0) return 1.0f;
    return (float)dpi / 96.0f;
}

void mr_api_set_rotation(int degrees)
{
    if (!api_state.initialized) return;

    // Validate rotation to one of the four cardinal angles
    if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
        fprintf(stderr, "[moonrock_api] Invalid rotation: %d (must be 0/90/180/270)\n",
                degrees);
        return;
    }

    // The touch module handles rotation of the GL projection matrix
    // and input coordinate transformation. RotationAngle enum values
    // match degree values (0, 90, 180, 270) so this cast is safe.
    touch_set_rotation((RotationAngle)degrees);
    api_state.needs_composite = true;
}

// ============================================================================
// ARGB visual (for transparent frame windows)
// ============================================================================

bool mr_api_get_argb_visual(Visual **out_visual, Colormap *out_colormap)
{
    if (!api_state.initialized) return false;
    return mr_create_argb_visual(&api_state.wm, out_visual, out_colormap);
}

void mr_api_set_input_shape(Window frame, int chrome_x, int chrome_y,
                                  int chrome_w, int chrome_h)
{
    if (!api_state.initialized) return;

    // The internal function takes a Client*, but for the public API we
    // construct a temporary one with just the fields that set_input_shape needs.
    Client temp = {0};
    temp.frame = frame;
    temp.x = chrome_x;
    temp.y = chrome_y;
    temp.w = chrome_w;
    temp.h = chrome_h;

    mr_set_input_shape(&api_state.wm, &temp);
}
