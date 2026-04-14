// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// ============================================================================
//  Crystal Animation Framework — generalized animation system for Crystal
// ============================================================================
//
// This module replaces the ad-hoc genie animation code with a proper framework
// that supports multiple simultaneous animations of different types.
//
// How it works:
//   The animation system manages a fixed-size array of Animation slots. When
//   you want to animate something (minimize a window, fade in a dialog, slide
//   between workspaces), you call anim_start() or one of the convenience
//   functions. This fills an empty slot with the animation's parameters:
//     - What type of animation (genie, fade, zoom, sheet, slide)
//     - Which texture to animate
//     - Where it starts and where it ends (position, size, opacity)
//     - How long it should take
//     - What easing function to use (linear, ease-in, ease-out, spring, etc.)
//
//   Each frame, Crystal's composite loop calls:
//     1. anim_update() — advances all active animations based on elapsed time
//     2. anim_draw()   — renders each animated window with its current transform
//
//   When an animation's progress reaches 1.0, it is marked inactive and its
//   slot becomes available for reuse.
//
// Easing functions:
//   Real-world motion is not linear — things accelerate and decelerate. Easing
//   functions transform a linear 0-to-1 progress value into a curved one. For
//   example, EASE_OUT_QUAD makes the animation start fast and slow down at the
//   end, which feels natural for something coming to rest. EASE_SPRING adds an
//   overshoot-and-settle effect, like a bouncing spring.
//
// Why a fixed-size array instead of a linked list?
//   With MAX_ANIMATIONS = 16, the array is small enough to scan every frame
//   with zero overhead. No malloc/free churn, no cache misses from pointer
//   chasing, and no risk of memory leaks. 16 slots is more than enough — you
//   would need to minimize, fade, zoom, and slide 16 things at once to run out.
//
// ============================================================================

#ifndef CRYSTAL_ANIM_H
#define CRYSTAL_ANIM_H

#include <stdbool.h>
#include <GL/gl.h>

// Maximum number of animations that can play at the same time.
// If all slots are full, anim_start() returns -1 and the animation is skipped.
#define MAX_ANIMATIONS 16


// ============================================================================
//  Animation types
// ============================================================================
//
// Each type defines a different visual effect. The type determines how
// anim_draw() renders the animated window — some use simple interpolation
// (fade, zoom), while others use mesh distortion (genie).

typedef enum {
    ANIM_NONE = 0,          // Empty slot (no animation)
    ANIM_GENIE_MINIMIZE,    // Window flows into dock icon (mesh distortion)
    ANIM_GENIE_RESTORE,     // Reverse genie (dock icon expands to window)
    ANIM_FADE_IN,           // Window appears (opacity 0 -> 1)
    ANIM_FADE_OUT,          // Window disappears (opacity 1 -> 0)
    ANIM_ZOOM_IN,           // Window zooms from small to full (app launch)
    ANIM_ZOOM_OUT,          // Window zooms from full to small (close to dock)
    ANIM_SHEET_DOWN,        // Dialog unfurls from title bar downward
    ANIM_SLIDE_LEFT,        // Workspace slides left (Spaces transition)
    ANIM_SLIDE_RIGHT,       // Workspace slides right
} AnimType;


// ============================================================================
//  Easing functions
// ============================================================================
//
// Easing transforms a linear progress value (0.0 to 1.0) into a curved one.
// This makes animations feel natural instead of robotic.
//
// "Quad" means the function uses t^2 (quadratic) — moderate curve.
// "Cubic" means t^3 — stronger curve, more dramatic acceleration/deceleration.
// "Spring" overshoots the target and settles back, like a physical spring.

typedef enum {
    EASE_LINEAR,            // Constant speed (no easing)
    EASE_IN_QUAD,           // Starts slow, accelerates (t*t)
    EASE_OUT_QUAD,          // Starts fast, decelerates
    EASE_IN_OUT_QUAD,       // Slow start, fast middle, slow end
    EASE_IN_CUBIC,          // Stronger acceleration than quad
    EASE_OUT_CUBIC,         // Stronger deceleration than quad
    EASE_IN_OUT_CUBIC,      // Stronger version of IN_OUT_QUAD
    EASE_SPRING,            // Overshoots target then bounces back to settle
} EaseType;


// ============================================================================
//  Animation instance
// ============================================================================
//
// Each active animation occupies one of these structs in the global array.
// The struct stores everything needed to render one frame of the animation:
// the source and destination geometry, current progress, easing type, etc.

typedef struct {
    AnimType type;          // What kind of animation this is
    EaseType easing;        // Which easing curve to apply

    // --- Target texture ---
    // This is the OpenGL texture handle for the window being animated.
    // tex_w and tex_h store its pixel dimensions so we know the aspect ratio.
    GLuint texture;         // The window texture being animated
    int tex_w, tex_h;       // Texture dimensions in pixels

    // --- Timing ---
    // start_time is a monotonic timestamp (from clock_gettime) recorded when
    // the animation began. duration is how long the animation should take in
    // seconds. Together they let us compute progress = elapsed / duration.
    double start_time;      // Monotonic timestamp when animation started
    double duration;        // Total duration in seconds

    // --- Progress ---
    // progress is the raw linear value from 0.0 to 1.0 based on elapsed time.
    // eased is the same value after the easing function has been applied.
    // The eased value is what we actually use for interpolation in anim_draw().
    float progress;         // Raw linear progress (0.0 to 1.0)
    float eased;            // Progress after easing function is applied

    // --- Source geometry ---
    // Where the window starts at the beginning of the animation.
    // For a minimize, this is the window's normal position and size.
    float src_x, src_y, src_w, src_h;

    // --- Destination geometry ---
    // Where the window ends up when the animation finishes.
    // For a minimize, this is the dock icon's position and a small size.
    float dst_x, dst_y, dst_w, dst_h;

    // --- Opacity ---
    // Start and end alpha values. The animation interpolates between them.
    // For fade-in: src_alpha=0.0, dst_alpha=1.0
    // For fade-out: src_alpha=1.0, dst_alpha=0.0
    float src_alpha, dst_alpha;

    // --- State ---
    bool active;            // true while this animation is playing

    // --- Callback data ---
    // An opaque pointer the caller can attach to identify which window or
    // object this animation belongs to. The animation system never dereferences
    // this — it just stores it so the caller can retrieve it later.
    void *userdata;
} Animation;


// ============================================================================
//  Lifecycle
// ============================================================================

// Initialize the animation system.
// Clears all animation slots to inactive. Call once at Crystal startup.
void anim_init(void);

// Shut down the animation system.
// Marks all animations inactive. Call during Crystal shutdown.
void anim_shutdown(void);


// ============================================================================
//  Starting animations
// ============================================================================

// Start a new animation with full control over all parameters.
//
// This is the low-level entry point — it fills an empty slot with the given
// parameters and starts the animation immediately.
//
// Parameters:
//   type      — which animation effect to use (ANIM_GENIE_MINIMIZE, etc.)
//   easing    — which easing curve to apply (EASE_LINEAR, EASE_OUT_CUBIC, etc.)
//   duration  — how long the animation lasts in seconds
//   texture   — OpenGL texture handle for the window being animated
//   tex_w/h   — pixel dimensions of the texture
//   src_*     — starting position, size (where the window is now)
//   dst_*     — ending position, size (where the window should end up)
//   src_alpha — starting opacity (0.0 = transparent, 1.0 = opaque)
//   dst_alpha — ending opacity
//   userdata  — opaque pointer for the caller (can be NULL)
//
// Returns the animation slot index (0 to MAX_ANIMATIONS-1) on success,
// or -1 if all slots are full (animation will not play).
int anim_start(AnimType type, EaseType easing, double duration,
               GLuint texture, int tex_w, int tex_h,
               float src_x, float src_y, float src_w, float src_h,
               float dst_x, float dst_y, float dst_w, float dst_h,
               float src_alpha, float dst_alpha,
               void *userdata);


// ============================================================================
//  Convenience functions
// ============================================================================
//
// These wrap anim_start() with sensible defaults for common animation types.
// They pick appropriate easing, duration, and alpha values so the caller only
// needs to provide the essential geometry.

// Start a genie minimize animation.
// The window appears to pour into the dock icon like liquid.
// Uses EASE_IN_QUAD easing, 0.5s duration, alpha fades from 1.0 to 0.7.
//
// win_x/y/w/h  — current window position and size (source)
// dock_x/y     — center of the dock icon (destination)
int anim_genie_minimize(GLuint texture, int tex_w, int tex_h,
                        float win_x, float win_y, float win_w, float win_h,
                        float dock_x, float dock_y);

// Start a fade-in animation.
// The window appears from transparent to fully opaque at its current position.
// Uses EASE_OUT_CUBIC easing (fast start, gentle settle).
//
// x/y/w/h  — window position and size (stays constant)
// duration — how long the fade takes in seconds
int anim_fade_in(GLuint texture, int tex_w, int tex_h,
                 float x, float y, float w, float h, double duration);

// Start a fade-out animation.
// The window fades from fully opaque to transparent.
// Uses EASE_OUT_CUBIC easing.
//
// x/y/w/h  — window position and size (stays constant)
// duration — how long the fade takes in seconds
int anim_fade_out(GLuint texture, int tex_w, int tex_h,
                  float x, float y, float w, float h, double duration);

// Start a zoom-in animation (e.g., launching an app from the dock).
// The window expands from a small icon size to its full size.
// Uses EASE_OUT_CUBIC easing, 0.4s duration, alpha 0.0 to 1.0.
//
// icon_x/y     — center of the dock icon (source — where it zooms from)
// win_x/y/w/h  — final window position and size (destination)
int anim_zoom_in(GLuint texture, int tex_w, int tex_h,
                 float icon_x, float icon_y,
                 float win_x, float win_y, float win_w, float win_h);


// ============================================================================
//  Per-frame operations
// ============================================================================

// Update all active animations. Call once per frame before anim_draw().
//
// This advances each animation's progress based on elapsed time since it
// started, applies the easing function, and marks completed animations as
// inactive.
//
// Returns true if any animation is still active (the compositor should keep
// rendering at the refresh rate). Returns false when all animations are done
// (the compositor can go back to damage-driven rendering to save CPU/GPU).
bool anim_update(void);

// Draw all active animations. Call during crystal_composite() after drawing
// normal (non-animated) windows.
//
// For each active animation, this interpolates between the source and
// destination geometry/opacity using the eased progress value, then renders
// the textured quad (or deformed mesh for genie effects).
//
// basic_shader  — the basic textured quad shader program handle
// genie_shader  — the genie mesh distortion shader program handle
// projection    — pointer to the 4x4 orthographic projection matrix (float[16])
void anim_draw(GLuint basic_shader, GLuint genie_shader, float *projection);


// ============================================================================
//  Queries and management
// ============================================================================

// Check if any animation is currently active.
// This is a lightweight check — does not advance time or modify state.
bool anim_any_active(void);

// Cancel all animations that are using a specific texture.
// Call this when a window is destroyed or unmapped to prevent the animation
// system from drawing a texture that no longer exists.
void anim_cancel_for_texture(GLuint texture);


// ============================================================================
//  Easing utility
// ============================================================================

// Apply an easing function to a linear progress value.
//
// Takes a raw linear value t in the range [0.0, 1.0] and returns the eased
// value. The eased value may briefly exceed 1.0 for spring easing (overshoot)
// but will settle back to exactly 1.0 at the end.
//
// This is exposed publicly so other modules can use the same easing math
// for their own interpolation (e.g., smooth scrolling, color transitions).
float anim_ease(EaseType type, float t);

#endif // CRYSTAL_ANIM_H
