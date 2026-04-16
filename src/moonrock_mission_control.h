// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/
//
// ============================================================================
//  MoonRock Mission Control — unified Expose + Spaces overview (macOS 10.7)
// ============================================================================
//
// Mission Control is the bird's-eye view of your desktop. When triggered
// (F9, hot corner, or three-finger swipe up), every window on the current
// desktop scales down and tiles into a grid overview. A row of desktop
// thumbnails appears along the top, showing all virtual desktops (Spaces).
//
// From this view the user can:
//   - Click a window to focus it and exit Mission Control.
//   - Drag a window to a different Space thumbnail to move it there.
//   - Click a Space thumbnail to switch to that desktop.
//   - Press Escape or click empty space to return to normal view.
//
// The enter/exit transitions are animated over 0.3 seconds using the MoonRock
// animation framework's easing functions. Windows smoothly interpolate between
// their real positions and their tiled grid positions.
//
// How Spaces work:
//   Each Space is a virtual desktop. Windows belong to exactly one Space at a
//   time. Only windows on the active Space are visible in normal view. Switching
//   Spaces animates a horizontal slide transition (like swiping between pages).
//   The user starts with one Space and can add more from Mission Control.
//
// ============================================================================

#ifndef MR_MISSION_CONTROL_H
#define MR_MISSION_CONTROL_H

#include "wm_compat.h"
#include <stdbool.h>
#include <GL/gl.h>

// Maximum number of virtual desktops (Spaces) the user can create.
// 16 is more than enough — most people use 2-4.
#define MC_MAX_SPACES 16


// ============================================================================
//  MCSpace — a single virtual desktop
// ============================================================================
//
// Each Space tracks which windows belong to it and maintains a thumbnail
// texture for the desktop preview shown at the top of Mission Control.

typedef struct {
    int id;                     // Unique identifier for this Space
    char name[64];              // Human-readable name ("Desktop 1", "Desktop 2", etc.)
    Window *windows;            // Dynamic array of X11 window IDs assigned to this Space
    int window_count;           // How many windows are currently in this Space
    GLuint thumbnail_tex;       // Pre-rendered thumbnail texture for the desktop preview
    bool thumbnail_dirty;       // True if the thumbnail needs to be re-rendered
} MCSpace;


// ============================================================================
//  MissionControlState — all state for the Mission Control overlay
// ============================================================================
//
// This struct holds everything needed to display and interact with Mission
// Control: the list of Spaces, the tiled window layout, hover state, and
// animation timing. A single global instance lives in the .c file.

typedef struct {
    // --- Activation state ---
    bool active;                // True when Mission Control is fully shown
    bool animating_in;          // True during the zoom-into-overview transition
    bool animating_out;         // True during the zoom-out-of-overview transition
    double anim_start;          // Monotonic timestamp when the current animation began
    double anim_duration;       // How long the transition takes (0.3 seconds)

    // --- Spaces ---
    MCSpace spaces[MC_MAX_SPACES];  // Array of all virtual desktops
    int space_count;                // How many Spaces currently exist
    int current_space;              // Index of the Space the user is currently viewing

    // --- Hover tracking ---
    // These track what the mouse cursor is hovering over so we can draw
    // highlight borders. -1 means nothing is being hovered.
    int hover_window;           // Index into tiled_windows[] under cursor (-1 = none)
    int hover_space;            // Index into spaces[] under cursor (-1 = none)

    // --- Tiled window layout ---
    // When entering Mission Control, we snapshot every visible window's position
    // and texture, then compute a grid layout. During the transition, each window
    // interpolates between its original position (orig_*) and its grid cell
    // position (target_*).
    struct {
        Window xwin;            // The X11 window ID (so we can focus it on click)
        float target_x, target_y, target_w, target_h;  // Grid cell position
        float orig_x, orig_y, orig_w, orig_h;          // Real desktop position
        GLuint texture;         // Snapshot of the window's contents (GL texture)
    } tiled_windows[256];       // Up to 256 windows (matches MAX_CLIENTS in wm.h)
    int tiled_count;            // How many windows are in the tiled layout
} MissionControlState;


// ============================================================================
//  Initialization and shutdown
// ============================================================================

// Initialize Mission Control. Call once during MoonRock startup.
//
// Sets up the initial Space ("Desktop 1"), assigns all existing windows to it,
// and zeroes out all state. Does NOT activate Mission Control — that happens
// when the user triggers it.
void mc_init(CCWM *wm);

// Shut down Mission Control and free all resources.
//
// Frees the window arrays in each Space and releases any thumbnail textures.
// Call during MoonRock shutdown.
void mc_shutdown(CCWM *wm);


// ============================================================================
//  Activation
// ============================================================================

// Toggle Mission Control on or off.
//
// If Mission Control is currently hidden, this calls mc_enter(). If it is
// currently shown (or animating in), this calls mc_exit() with no specific
// window to focus.
void mc_toggle(CCWM *wm);

// Enter Mission Control (animate into the overview).
//
// This does the following:
//   1. Snapshots every mapped window's position, size, and GL texture.
//   2. Computes the tiled grid layout (where each window will land).
//   3. Starts the 0.3-second zoom-out animation.
//   4. Grabs keyboard and pointer so all input goes through mc_handle_event().
void mc_enter(CCWM *wm);

// Exit Mission Control (animate back to normal view).
//
// focus_window: if non-zero, this window will be focused and raised after
//               the exit animation completes. If zero (None), the previously
//               focused window remains focused.
//
// Starts the 0.3-second reverse animation. When it finishes, mc_update()
// sets active = false and ungrabs input.
void mc_exit(CCWM *wm, Window focus_window);


// ============================================================================
//  Per-frame operations
// ============================================================================

// Update Mission Control animations. Call once per frame.
//
// Advances the enter/exit animation progress. When the animation finishes:
//   - For enter: sets active = true, animating_in = false.
//   - For exit: sets active = false, animating_out = false, ungrabs input.
//
// Returns true if Mission Control needs another frame (animation still playing
// or overlay is active), false if Mission Control is fully dismissed.
bool mc_update(CCWM *wm);

// Draw the Mission Control overlay.
//
// This is called from mr_composite() when Mission Control is active or
// animating. It draws:
//   1. A semi-transparent dark overlay (dims the wallpaper).
//   2. Space thumbnails along the top of the screen.
//   3. All tiled windows at their interpolated positions.
//   4. Hover highlights on the window/space under the cursor.
//
// basic_shader: the GL shader program for textured quads.
// projection:   pointer to the 4x4 orthographic projection matrix.
void mc_draw(CCWM *wm, GLuint basic_shader, float *projection);


// ============================================================================
//  Input handling
// ============================================================================

// Handle an X event while Mission Control is active.
//
// This processes mouse motion (hover tracking), mouse clicks (window focus,
// space switching, exit), and keyboard input (Escape to exit, arrow keys to
// cycle spaces).
//
// Returns true if the event was consumed by Mission Control (caller should
// skip normal event processing). Returns false if Mission Control is not
// active or the event is not relevant.
bool mc_handle_event(CCWM *wm, XEvent *ev);


// ============================================================================
//  State query
// ============================================================================

// Check if Mission Control is currently active (shown or animating).
//
// Other modules use this to know whether to route input through Mission Control
// and whether to keep rendering at the refresh rate.
bool mc_is_active(void);


// ============================================================================
//  Space management
// ============================================================================

// Add a new empty Space (virtual desktop).
//
// The new Space is named "Desktop N" where N is the next sequential number.
// Returns the ID of the newly created Space, or -1 if MC_MAX_SPACES has been
// reached.
int mc_add_space(CCWM *wm);

// Remove a Space and redistribute its windows to the previous Space.
//
// space_id: the ID of the Space to remove.
//
// You cannot remove the last remaining Space — there must always be at least
// one desktop. Windows that belonged to the removed Space are moved to the
// Space that comes before it (or the first Space if the removed one was first).
void mc_remove_space(CCWM *wm, int space_id);

// Switch to a different Space (virtual desktop).
//
// space_id: the ID of the Space to switch to.
//
// This unmaps all windows on the current Space and maps all windows on the
// target Space. If called during Mission Control, it also updates the
// current_space indicator and re-tiles windows for the new Space.
void mc_switch_space(CCWM *wm, int space_id);

// Move a window from its current Space to a different one.
//
// win:      the X11 window ID to move.
// space_id: the ID of the destination Space.
//
// If the destination Space is not the active Space, the window is unmapped
// (hidden) after being moved. If it IS the active Space, the window remains
// visible.
void mc_move_window_to_space(CCWM *wm, Window win, int space_id);

#endif // MR_MISSION_CONTROL_H
