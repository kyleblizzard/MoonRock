// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// ============================================================================
//  Crystal Animation Framework — implementation
// ============================================================================
//
// This file implements the generalized animation system described in
// crystal_anim.h. The core idea is simple:
//
//   1. A fixed-size array holds up to MAX_ANIMATIONS active animations.
//   2. Each frame, anim_update() computes how far along each animation is
//      (based on wall-clock time), applies the easing function, and marks
//      finished animations as inactive.
//   3. anim_draw() renders each active animation by interpolating between
//      the source and destination geometry/opacity.
//
// Timing uses clock_gettime(CLOCK_MONOTONIC) for frame-rate-independent
// animation. CLOCK_MONOTONIC is not affected by system clock changes (like
// NTP adjustments or the user changing the time), so animations won't glitch
// if the clock jumps.
//
// ============================================================================

// _GNU_SOURCE is needed so <math.h> defines M_PI (the constant for pi).
// Without this, M_PI is not available on strict C99/C11 compilers.
#define _GNU_SOURCE

#include "crystal_anim.h"
#include "crystal_shaders.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>


// ============================================================================
//  Global animation array
// ============================================================================
//
// All animation slots live in this static array. We never allocate or free
// individual animations — we just flip the 'active' flag on and off.
// This makes the system very predictable: no malloc failures, no fragmentation.

static Animation animations[MAX_ANIMATIONS];


// ============================================================================
//  Internal helper: get current time in seconds
// ============================================================================
//
// Returns a monotonic timestamp as a double (seconds + fractional seconds).
// We use CLOCK_MONOTONIC because it ticks at a constant rate regardless of
// system clock adjustments. This ensures animation timing is smooth even if
// the system clock is changed by NTP or the user.

static double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    // Combine seconds and nanoseconds into a single double.
    // 1 second = 1,000,000,000 nanoseconds.
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}


// ============================================================================
//  Easing functions
// ============================================================================
//
// These transform a linear 0-to-1 progress value into a curved one so that
// animations feel natural. Each function implements a different motion curve:
//
//   Linear:     constant speed — feels robotic, but useful for simple slides.
//   Quad:       based on t^2 — gentle curve, good general-purpose easing.
//   Cubic:      based on t^3 — more dramatic acceleration/deceleration.
//   Spring:     overshoots the target then bounces back — gives a "snappy" feel.
//
// "In" means the easing applies at the start (slow start, fast end).
// "Out" means the easing applies at the end (fast start, slow end).
// "InOut" combines both (slow start, fast middle, slow end).

float anim_ease(EaseType type, float t)
{
    switch (type) {

    case EASE_LINEAR:
        // No transformation — output equals input.
        return t;

    case EASE_IN_QUAD:
        // Starts slow, accelerates. The curve is a simple parabola.
        // At t=0.5 the output is only 0.25 (halfway through time, only 25% done).
        return t * t;

    case EASE_OUT_QUAD:
        // Starts fast, decelerates. This is the "flipped" version of ease-in.
        // Derived by reflecting ease-in around the diagonal: t*(2-t).
        return t * (2.0f - t);

    case EASE_IN_OUT_QUAD:
        // Two halves stitched together: ease-in for the first half, ease-out
        // for the second half. Creates a smooth S-curve.
        if (t < 0.5f)
            return 2.0f * t * t;
        else
            return -1.0f + (4.0f - 2.0f * t) * t;

    case EASE_IN_CUBIC:
        // Like ease-in-quad but sharper — stays slow longer, then accelerates
        // more aggressively at the end.
        return t * t * t;

    case EASE_OUT_CUBIC:
        // Like ease-out-quad but sharper — decelerates more dramatically.
        // Uses the identity: (t-1)^3 + 1 = 1 - (1-t)^3.
        {
            float u = t - 1.0f;
            return u * u * u + 1.0f;
        }

    case EASE_IN_OUT_CUBIC:
        // S-curve with cubic sharpness. More pronounced than the quad version.
        if (t < 0.5f)
            return 4.0f * t * t * t;
        else
            return (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;

    case EASE_SPRING:
        // Overshoots the target value then settles back. This creates a "bouncy"
        // or "snappy" feel that is great for UI elements popping into place.
        //
        // The constant 's' (1.70158) is the "overshoot amount" — it controls
        // how far past the target the animation goes before settling. This
        // specific value is known as "Penner's magic number" and produces a
        // 10% overshoot, which feels natural without being distracting.
        {
            float s = 1.70158f;
            float u = t - 1.0f;
            return u * u * ((s + 1.0f) * u + s) + 1.0f;
        }

    default:
        // Unknown easing type — fall back to linear as a safe default.
        return t;
    }
}


// ============================================================================
//  Initialization and shutdown
// ============================================================================

void anim_init(void)
{
    // Zero out the entire array. This sets every slot's 'active' flag to false
    // and every 'type' to ANIM_NONE, so all slots are available.
    memset(animations, 0, sizeof(animations));
    fprintf(stderr, "[crystal_anim] Animation system initialized (%d slots)\n",
            MAX_ANIMATIONS);
}

void anim_shutdown(void)
{
    // Mark every slot inactive. We don't need to free anything because the
    // array is statically allocated — it lives for the entire process lifetime.
    for (int i = 0; i < MAX_ANIMATIONS; i++) {
        animations[i].active = false;
        animations[i].type = ANIM_NONE;
    }
    fprintf(stderr, "[crystal_anim] Animation system shut down\n");
}


// ============================================================================
//  Starting animations
// ============================================================================

int anim_start(AnimType type, EaseType easing, double duration,
               GLuint texture, int tex_w, int tex_h,
               float src_x, float src_y, float src_w, float src_h,
               float dst_x, float dst_y, float dst_w, float dst_h,
               float src_alpha, float dst_alpha,
               void *userdata)
{
    // Find the first empty (inactive) slot in the array.
    int slot = -1;
    for (int i = 0; i < MAX_ANIMATIONS; i++) {
        if (!animations[i].active) {
            slot = i;
            break;
        }
    }

    // If all slots are occupied, we cannot start a new animation.
    // This should be rare — 16 simultaneous animations is a lot.
    if (slot < 0) {
        fprintf(stderr, "[crystal_anim] WARNING: all %d animation slots full, "
                "cannot start animation type %d\n", MAX_ANIMATIONS, type);
        return -1;
    }

    // Fill in the slot with the caller's parameters.
    Animation *a = &animations[slot];

    a->type      = type;
    a->easing    = easing;
    a->texture   = texture;
    a->tex_w     = tex_w;
    a->tex_h     = tex_h;
    a->duration  = duration;
    a->start_time = get_time();    // Record when the animation begins

    // Start at zero progress — the animation has just begun.
    a->progress  = 0.0f;
    a->eased     = 0.0f;

    // Source geometry (where the window is now)
    a->src_x = src_x;
    a->src_y = src_y;
    a->src_w = src_w;
    a->src_h = src_h;

    // Destination geometry (where the window should end up)
    a->dst_x = dst_x;
    a->dst_y = dst_y;
    a->dst_w = dst_w;
    a->dst_h = dst_h;

    // Opacity range
    a->src_alpha = src_alpha;
    a->dst_alpha = dst_alpha;

    // Caller's opaque pointer (may be NULL)
    a->userdata = userdata;

    // Activate the slot — anim_update() and anim_draw() will now process it.
    a->active = true;

    fprintf(stderr, "[crystal_anim] Started animation type %d in slot %d "
            "(duration=%.2fs, easing=%d)\n", type, slot, duration, easing);

    return slot;
}


// ============================================================================
//  Convenience functions
// ============================================================================
//
// Each of these wraps anim_start() with sensible defaults for a common
// animation scenario. They exist to keep calling code clean — instead of
// passing 16 arguments, you pass just the essential geometry.

int anim_genie_minimize(GLuint texture, int tex_w, int tex_h,
                        float win_x, float win_y, float win_w, float win_h,
                        float dock_x, float dock_y)
{
    // The genie effect contracts the window down to the dock icon's position.
    // We use a small destination size (48x4) to represent the "fully minimized"
    // state — the window has been squished into a tiny strip at the dock.
    //
    // EASE_IN_QUAD makes the window start moving slowly and accelerate as it
    // gets sucked into the dock, which matches the macOS feel.
    //
    // Alpha goes from 1.0 (fully visible) to 0.7 (slightly faded) — the window
    // does not fully disappear, it just becomes slightly transparent as it
    // contracts, which adds to the "flowing liquid" illusion.
    float dst_w = 48.0f;   // Width of the "minimized" strip
    float dst_h = 4.0f;    // Height of the "minimized" strip
    float dst_x = dock_x - dst_w / 2.0f;   // Center on the dock icon
    float dst_y = dock_y - dst_h / 2.0f;

    return anim_start(
        ANIM_GENIE_MINIMIZE,
        EASE_IN_QUAD,
        0.5,                // 0.5 seconds — matches macOS timing
        texture, tex_w, tex_h,
        win_x, win_y, win_w, win_h,       // Source: window's current position
        dst_x, dst_y, dst_w, dst_h,       // Destination: dock icon
        1.0f, 0.7f,                        // Alpha: fully visible to slightly faded
        NULL                               // No userdata needed
    );
}

int anim_fade_in(GLuint texture, int tex_w, int tex_h,
                 float x, float y, float w, float h, double duration)
{
    // Fade-in: the window stays in place (source and destination geometry are
    // identical) but its opacity goes from 0 (invisible) to 1 (fully visible).
    //
    // EASE_OUT_CUBIC gives a fast start and gentle settle — the window quickly
    // becomes visible and then smoothly reaches full opacity. This feels snappy
    // without being jarring.
    return anim_start(
        ANIM_FADE_IN,
        EASE_OUT_CUBIC,
        duration,
        texture, tex_w, tex_h,
        x, y, w, h,       // Source: same position
        x, y, w, h,       // Destination: same position (no movement)
        0.0f, 1.0f,       // Alpha: transparent to opaque
        NULL
    );
}

int anim_fade_out(GLuint texture, int tex_w, int tex_h,
                  float x, float y, float w, float h, double duration)
{
    // Fade-out: the reverse of fade-in. Window stays in place but becomes
    // transparent. Same easing for consistency.
    return anim_start(
        ANIM_FADE_OUT,
        EASE_OUT_CUBIC,
        duration,
        texture, tex_w, tex_h,
        x, y, w, h,       // Source: same position
        x, y, w, h,       // Destination: same position (no movement)
        1.0f, 0.0f,       // Alpha: opaque to transparent
        NULL
    );
}

int anim_zoom_in(GLuint texture, int tex_w, int tex_h,
                 float icon_x, float icon_y,
                 float win_x, float win_y, float win_w, float win_h)
{
    // Zoom-in: the window expands from a tiny size (centered on the dock icon)
    // to its full size. This is used for app launch animations.
    //
    // The source is a small 48x48 square centered on the dock icon — this
    // matches the typical dock icon size so the zoom appears to come from
    // the icon itself.
    //
    // EASE_OUT_CUBIC gives a fast expansion that slows down as the window
    // reaches its final size, which feels like the window is "landing" in place.
    //
    // Alpha goes from 0 (invisible at the dock) to 1 (fully visible), so the
    // window both grows and fades in simultaneously.
    float icon_size = 48.0f;
    float src_x = icon_x - icon_size / 2.0f;
    float src_y = icon_y - icon_size / 2.0f;

    return anim_start(
        ANIM_ZOOM_IN,
        EASE_OUT_CUBIC,
        0.4,                // 0.4 seconds — slightly faster than genie
        texture, tex_w, tex_h,
        src_x, src_y, icon_size, icon_size,   // Source: small, at dock icon
        win_x, win_y, win_w, win_h,           // Destination: full window size
        0.0f, 1.0f,                            // Alpha: invisible to fully visible
        NULL
    );
}


// ============================================================================
//  Per-frame update
// ============================================================================

bool anim_update(void)
{
    double now = get_time();
    bool any_active = false;

    // Walk through every slot and update active animations.
    for (int i = 0; i < MAX_ANIMATIONS; i++) {
        Animation *a = &animations[i];

        // Skip inactive slots.
        if (!a->active)
            continue;

        // Calculate how much time has passed since the animation started.
        double elapsed = now - a->start_time;

        // Convert elapsed time to a 0.0-to-1.0 progress value.
        // progress = elapsed / duration, clamped to [0, 1].
        a->progress = (float)(elapsed / a->duration);

        // Check if the animation has finished.
        if (a->progress >= 1.0f) {
            a->progress = 1.0f;
            a->eased = 1.0f;

            // Mark the slot as inactive — it is done and can be reused.
            a->active = false;

            // Don't count this as "active" since it just completed.
            continue;
        }

        // Apply the easing function to get the curved progress value.
        // This is what we use for interpolation in anim_draw().
        a->eased = anim_ease(a->easing, a->progress);

        // At least one animation is still playing.
        any_active = true;
    }

    return any_active;
}


// ============================================================================
//  Per-frame drawing
// ============================================================================
//
// For each active animation, we interpolate between the source and destination
// geometry/opacity using the eased progress value, then draw the result.
//
// Most animation types use simple linear interpolation (lerp) on position,
// size, and opacity. The genie effect is special — it uses differential
// interpolation rates for the top and bottom edges to create a "funnel" shape.

void anim_draw(GLuint basic_shader, GLuint genie_shader, float *projection)
{
    for (int i = 0; i < MAX_ANIMATIONS; i++) {
        Animation *a = &animations[i];

        // We draw animations that are either active or just completed this frame
        // (progress == 1.0 but active was set to false in anim_update).
        // Skip slots that have never been used or were completed on a prior frame.
        if (!a->active && a->progress < 1.0f)
            continue;

        // Skip animations with no texture — nothing to draw.
        if (a->texture == 0)
            continue;

        // 't' is the eased progress value (0.0 at start, 1.0 at end).
        // We use this to interpolate (lerp) between source and destination.
        float t = a->eased;

        // -----------------------------------------------------------------
        //  Interpolate geometry and opacity
        // -----------------------------------------------------------------
        // Linear interpolation: value = start + (end - start) * t
        // At t=0 we get the source value, at t=1 we get the destination value.
        float x     = a->src_x     + (a->dst_x     - a->src_x)     * t;
        float y     = a->src_y     + (a->dst_y     - a->src_y)     * t;
        float w     = a->src_w     + (a->dst_w     - a->src_w)     * t;
        float h     = a->src_h     + (a->dst_h     - a->src_h)     * t;
        float alpha = a->src_alpha + (a->dst_alpha - a->src_alpha) * t;

        // -----------------------------------------------------------------
        //  Draw based on animation type
        // -----------------------------------------------------------------

        if (a->type == ANIM_GENIE_MINIMIZE || a->type == ANIM_GENIE_RESTORE) {
            // ============================================================
            //  Genie effect — mesh distortion
            // ============================================================
            //
            // The genie effect makes the window look like it is being poured
            // into (or out of) the dock. The key visual trick is that the
            // bottom edge contracts faster than the top edge, creating a
            // funnel/trapezoid shape that progressively narrows.
            //
            // We achieve this by computing separate positions for the top
            // and bottom edges of the quad, using a "squeeze" factor that
            // includes a sine wave component. The sine wave adds a subtle
            // nonlinearity that makes the motion feel organic rather than
            // mechanical.

            // Squeeze factor: the bottom edge moves faster than linear.
            // The sinf(t * M_PI) term adds a bulge in the middle of the
            // animation — the bottom contracts quickly, pauses slightly,
            // then finishes contracting. This mimics the fluid motion of
            // the macOS genie effect.
            float squeeze = t + 0.3f * sinf(t * (float)M_PI);
            if (squeeze > 1.0f) squeeze = 1.0f;

            // Top edge: moves toward the destination at the eased rate.
            // Width shrinks to 30% of original by the end (1.0 - t*0.7).
            float top_x = a->src_x + t * (a->dst_x - a->src_x);
            float top_y = a->src_y + t * (a->dst_y - a->src_y);
            float top_w = a->src_w * (1.0f - t * 0.7f);

            // Bottom edge: moves faster (using squeeze instead of t).
            // Width shrinks to 10% of original (1.0 - squeeze*0.9), creating
            // the narrow "spout" at the bottom of the funnel.
            float bot_x = a->src_x + squeeze * (a->dst_x - a->src_x);
            float bot_y = (a->src_y + a->src_h) +
                          t * (a->dst_y - (a->src_y + a->src_h));
            float bot_w = a->src_w * (1.0f - squeeze * 0.9f);

            // Set up the basic shader for rendering.
            // (The genie vertex shader will be used in a future update when
            // we implement proper mesh subdivision. For now, we draw a
            // manually deformed quad using the basic shader.)
            shaders_use(basic_shader);
            shaders_set_projection(basic_shader, projection);
            shaders_set_alpha(basic_shader, alpha);

            // Bind the window's texture so the shader can sample it.
            glBindTexture(GL_TEXTURE_2D, a->texture);

            // Draw a deformed quad (trapezoid) using immediate mode.
            //
            // NOTE: glBegin/glEnd is deprecated in modern OpenGL and will be
            // replaced with a proper genie mesh shader in a future update.
            // For now, this gives us the visual effect while we build out
            // the full shader-based mesh distortion pipeline.
            //
            // The four vertices define a trapezoid:
            //   Top-left  -> Top-right  (wider, near original position)
            //   Bot-right -> Bot-left   (narrower, near dock icon)
            //
            // Texture coordinates (0,0)-(1,1) map the full window texture
            // onto this deformed shape, stretching it to fit.
            glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 0.0f);
            glVertex2f(top_x, top_y);

            glTexCoord2f(1.0f, 0.0f);
            glVertex2f(top_x + top_w, top_y);

            glTexCoord2f(1.0f, 1.0f);
            glVertex2f(bot_x + bot_w, bot_y);

            glTexCoord2f(0.0f, 1.0f);
            glVertex2f(bot_x, bot_y);
            glEnd();

        } else {
            // ============================================================
            //  Standard interpolated quad (fade, zoom, sheet, slide)
            // ============================================================
            //
            // For all non-genie animations, we simply draw a normal textured
            // quad at the interpolated position, size, and opacity. The
            // easing function handles making the motion feel natural.

            shaders_use(basic_shader);
            shaders_set_projection(basic_shader, projection);
            shaders_set_alpha(basic_shader, alpha);

            // Bind the window texture.
            glBindTexture(GL_TEXTURE_2D, a->texture);

            // Draw the quad using the VBO-based renderer (faster than
            // glBegin/glEnd and compatible with modern OpenGL).
            shaders_draw_quad(x, y, w, h);
        }
    }
}


// ============================================================================
//  Queries and management
// ============================================================================

bool anim_any_active(void)
{
    // Simple scan: return true as soon as we find any active slot.
    for (int i = 0; i < MAX_ANIMATIONS; i++) {
        if (animations[i].active)
            return true;
    }
    return false;
}

void anim_cancel_for_texture(GLuint texture)
{
    // When a window is destroyed, we must stop any animations that reference
    // its texture — otherwise anim_draw() would try to bind a deleted texture,
    // which would either show garbage or crash.
    for (int i = 0; i < MAX_ANIMATIONS; i++) {
        if (animations[i].active && animations[i].texture == texture) {
            animations[i].active = false;
            animations[i].type = ANIM_NONE;
            fprintf(stderr, "[crystal_anim] Cancelled animation in slot %d "
                    "(texture %u destroyed)\n", i, texture);
        }
    }
}
