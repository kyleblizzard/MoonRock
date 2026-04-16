// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/

// ============================================================================
// moonrock_api.h — MoonRock Compositor Public API
// ============================================================================
//
// This is the ONLY header a window manager needs to include to use MoonRock.
// It provides a clean interface with no dependencies on any WM's internal
// structures. The WM describes windows to MoonRock, MoonRock composites them.
//
// MoonRock is a standalone X11 OpenGL compositor. It handles:
//   - Window compositing with proper alpha blending and z-order
//   - Gaussian blur shadows (cached per-window)
//   - Frosted glass blur-behind for translucent panels
//   - Genie minimize and other animations (8 easing curves)
//   - Mission Control (Exposé + Spaces)
//   - Multitouch input and gesture recognition
//   - Display rotation, VRR, HDR, fractional DPI scaling
//   - Direct scanout bypass for gaming
//   - Plugin API with theme engine
//   - Damage-based render-on-demand
//
// Usage:
//   1. Call mr_api_init() after opening the X display
//   2. Each frame, build an array of MRWindow structs
//   3. Call mr_api_composite() with the array
//   4. MoonRock handles everything else (shadows, blur, animations)
//
// Named after the mineral that Quartz is made of.
// A play on Apple's Quartz Compositor — both transparent, both foundational.
// ============================================================================

#ifndef MR_API_H
#define MR_API_H

#include <stdbool.h>
#include <X11/Xlib.h>

// ── Window description ────────────────────────────────────────────
// The WM fills in these fields for each visible window.
// MoonRock uses them to composite the desktop.

typedef enum {
    MR_WINDOW_DESKTOP = 0,   // Desktop background (lowest z-order)
    MR_WINDOW_NORMAL  = 1,   // Regular application window
    MR_WINDOW_DOCK    = 2,   // Panel, dock, menu bar (highest z-order)
    MR_WINDOW_POPUP   = 3,   // Override-redirect popup (menus, tooltips)
} MRWindowType;

typedef struct {
    Window window_id;             // X11 window ID (the frame window, not the client)
    int x, y;                     // Position on screen (pixels)
    int w, h;                     // Size (pixels)
    bool has_alpha;               // True if window uses 32-bit ARGB visual
    bool focused;                 // True if this is the focused/active window
    bool override_redirect;       // True for popups (menus, tooltips)
    MRWindowType type;       // Window type for z-order sorting
} MRWindow;

// ── Lifecycle ─────────────────────────────────────────────────────

// Initialize MoonRock. Call once after opening the X display.
// Returns true on success, false if required GL extensions are missing.
bool mr_api_init(Display *dpy, int screen);

// Shut down MoonRock. Releases all GL resources and undoes compositing.
void mr_api_shutdown(void);

// Is MoonRock active? (Did init succeed?)
bool mr_api_is_active(void);

// ── Per-frame compositing ─────────────────────────────────────────

// Composite all windows onto the screen.
// The WM provides an array of MRWindow structs describing every
// visible window. MoonRock handles textures, shadows, blur, and animations.
// Call this once per frame (or only when mr_api_needs_composite() is true).
void mr_api_composite(MRWindow *windows, int count);

// Check if MoonRock needs a composite pass (damage occurred, animation active).
// The WM can skip calling mr_api_composite() when this returns false.
bool mr_api_needs_composite(void);

// Mark MoonRock as needing a repaint (call on window map/unmap/resize/damage).
void mr_api_mark_dirty(void);

// ── Animations ────────────────────────────────────────────────────

// Start genie minimize animation for a window.
// texture_window: the X window whose texture to animate
// dock_x, dock_y: screen position of the dock icon to flow into
void mr_api_minimize(Window texture_window, int dock_x, int dock_y);

// Start genie restore animation (un-minimize).
void mr_api_restore(Window texture_window, int dock_x, int dock_y);

// Start fade-in animation for a newly mapped window.
void mr_api_fade_in(Window texture_window, double duration_sec);

// Start fade-out animation for a window being closed.
void mr_api_fade_out(Window texture_window, double duration_sec);

// Is any animation currently playing?
bool mr_api_animation_active(void);

// ── Mission Control ───────────────────────────────────────────────

// Toggle Mission Control (Exposé + Spaces overview).
void mr_api_toggle_mission_control(void);

// Is Mission Control currently active?
bool mr_api_mission_control_active(void);

// Add a new virtual desktop (Space).
int mr_api_add_space(void);

// Switch to a specific Space.
void mr_api_switch_space(int space_id);

// Move a window to a different Space.
void mr_api_move_to_space(Window win, int space_id);

// ── Event handling ────────────────────────────────────────────────

// Process X events that MoonRock needs (XDamage, XInput2 touch, etc.).
// Returns true if the event was consumed by MoonRock.
bool mr_api_handle_event(XEvent *ev);

// ── Display ───────────────────────────────────────────────────────

// Get the current DPI scale factor (1.0 for 96 PPI, 2.0 for 192 PPI).
float mr_api_get_scale_factor(void);

// Set display rotation (0, 90, 180, 270 degrees).
void mr_api_set_rotation(int degrees);

// ── ARGB visual (for transparent frame windows) ───────────────────

// Find a 32-bit ARGB visual for transparent windows.
bool mr_api_get_argb_visual(Visual **out_visual, Colormap *out_colormap);

// Set X input shape so clicks pass through shadow regions.
void mr_api_set_input_shape(Window frame, int chrome_x, int chrome_y,
                                  int chrome_w, int chrome_h);

// ── Shadow parameters ─────────────────────────────────────────────

// These define how much extra space the WM should add to frame windows
// to accommodate the shadow. The WM creates frames this much larger than
// the client window on each side.
#define MR_SHADOW_RADIUS        22
#define MR_SHADOW_Y_OFFSET       4
#define MR_SHADOW_TOP           (MR_SHADOW_RADIUS - MR_SHADOW_Y_OFFSET)
#define MR_SHADOW_BOTTOM        (MR_SHADOW_RADIUS + MR_SHADOW_Y_OFFSET)
#define MR_SHADOW_LEFT           MR_SHADOW_RADIUS
#define MR_SHADOW_RIGHT          MR_SHADOW_RADIUS

#endif // MR_API_H
