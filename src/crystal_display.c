// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// ============================================================================
//  Crystal Display Management — Implementation
// ============================================================================
//
// This file implements display output enumeration, VRR control, adaptive frame
// timing, direct scanout, gamescope handoff, and PipeWire screencast stubs.
//
// Dependencies:
//   - XRandR (X Resize and Rotate) — for output enumeration and properties.
//   - POSIX (fork, exec, waitpid) — for launching gamescope as a child process.
//   - clock_gettime (CLOCK_MONOTONIC) — for high-resolution frame timing.
//
// All functions log diagnostic messages to stderr with a "[display]" prefix
// so they're easy to filter in log output.
//
// ============================================================================

#define _GNU_SOURCE  // Needed for some POSIX extensions (e.g., setsid)

#include "crystal_display.h"
#include "wm_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

// XRandR — the X11 extension for querying and configuring display outputs.
// It tells us what monitors are connected, their resolutions, refresh rates,
// and properties like VRR capability.
#include <X11/extensions/Xrandr.h>

// XComposite — the X11 extension that controls off-screen redirection.
// Normally, Crystal redirects all windows to off-screen pixmaps so we can
// composite them via OpenGL. For direct scanout, we "unredirect" a single
// fullscreen window so the X server paints it straight to the display,
// bypassing the compositor entirely. This is the same technique picom uses
// for fullscreen unredirection.
#include <X11/extensions/Xcomposite.h>


// ============================================================================
//  Module-level state
// ============================================================================
//
// These static variables hold the display subsystem's state. They're file-
// scoped (static) so nothing outside this module can touch them directly —
// all access goes through the public API functions.

// The X display connection — cached from display_init() so we don't have
// to pass it to every function.
static Display *display_dpy = NULL;

// The X screen number (usually 0 for single-GPU systems).
static int display_screen = 0;

// The root window of the default screen. We need this for XRandR queries
// and for checking window geometry during direct scanout eligibility.
static Window display_root = None;

// Array of discovered display outputs. Allocated on the heap during
// display_init() and reallocated on hotplug. Freed in display_shutdown().
static CrystalOutput *outputs = NULL;

// How many outputs are currently in the array.
static int output_count = 0;

// Maximum number of outputs we've allocated space for. This lets us reuse
// the allocation when re-enumerating instead of malloc/free every time.
#define MAX_OUTPUTS 16

// The current gaming mode — see the GameMode enum in the header.
static GameMode current_game_mode = GAME_MODE_OFF;

// PID of the gamescope child process (0 if not running).
// We track this so we can wait for it to exit when returning from gamescope.
static pid_t gamescope_pid = 0;

// The window currently using direct scanout (None if not active).
static Window direct_scanout_win = None;

// Frame timing state — used by display_begin_frame() / display_end_frame().
static double frame_start = 0.0;     // Timestamp when the current frame began
static int frame_count = 0;          // Frames rendered since last FPS update
static double last_fps_time = 0.0;   // Timestamp of last FPS counter reset

// The current frame metrics, updated every frame by display_end_frame().
static FrameMetrics metrics = {0};


// ============================================================================
//  Internal helpers
// ============================================================================

// get_time() — Returns the current time in seconds as a double.
//
// Uses CLOCK_MONOTONIC, which is a clock that always moves forward at a
// steady rate and is not affected by NTP adjustments or the user changing
// the system clock. This makes it ideal for measuring elapsed time between
// two points (like how long a frame took to render).
//
// The resolution is typically nanosecond-level, but we return seconds as a
// double for convenience. At double precision, we get sub-microsecond
// accuracy for time differences, which is more than enough for frame timing.
static double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9;
}


// ============================================================================
//  Initialization
// ============================================================================

// display_init() — Enumerate all connected display outputs via XRandR.
//
// This function talks to the X server through the XRandR extension to
// discover every connected monitor. For each monitor, it reads:
//   - Name (e.g., "HDMI-1", "eDP-1", "DP-2")
//   - Position in the virtual screen (x, y)
//   - Resolution (width x height)
//   - Current refresh rate in Hz
//   - Whether the output supports VRR (FreeSync / Adaptive Sync)
//   - Whether it's the primary output
//
// XRandR data model (simplified):
//   Screen Resources -> Outputs -> CRTCs -> Modes
//   - An "output" is a physical connector (HDMI port, DP port, etc.)
//   - A "CRTC" is a display controller that drives one output
//   - A "mode" defines resolution + refresh rate
//   - An output is "connected" if a monitor is plugged into that connector
//   - An output is "active" if it has a CRTC assigned (i.e., it's turned on)

bool display_init(Display *dpy, int screen)
{
    // Validate inputs — can't do anything without a display connection.
    if (!dpy) {
        fprintf(stderr, "[display] ERROR: NULL display pointer\n");
        return false;
    }

    // Cache the display connection and screen for use by other functions
    // in this module, so callers don't have to pass them around everywhere.
    display_dpy = dpy;
    display_screen = screen;
    display_root = RootWindow(dpy, screen);

    // Allocate the output array if this is the first call.
    // On subsequent calls (e.g., from hotplug), we reuse the existing array
    // and just overwrite the entries.
    if (!outputs) {
        outputs = calloc(MAX_OUTPUTS, sizeof(CrystalOutput));
        if (!outputs) {
            fprintf(stderr, "[display] ERROR: Failed to allocate output array\n");
            return false;
        }
    }

    // Ask XRandR for the current screen resources. This is the top-level
    // structure that lists all outputs, CRTCs, and available modes.
    XRRScreenResources *res = XRRGetScreenResources(dpy, display_root);
    if (!res) {
        fprintf(stderr, "[display] ERROR: XRRGetScreenResources failed — "
                "XRandR may not be available\n");
        return false;
    }

    // Reset the output count — we're about to re-enumerate from scratch.
    output_count = 0;

    // Track whether any output is explicitly marked as primary.
    // If none is, we'll designate the first active output as primary.
    bool found_primary = false;

    // The "primary output" according to XRandR. This is a user-configurable
    // setting (e.g., "xrandr --output HDMI-1 --primary").
    RROutput primary_output = XRRGetOutputPrimary(dpy, display_root);

    // Iterate through every output (physical connector) the X server knows
    // about. Most of these will be disconnected — we only care about the
    // ones that have a monitor plugged in AND are actively displaying.
    for (int i = 0; i < res->noutput && output_count < MAX_OUTPUTS; i++) {

        // Get detailed info about this output — name, connection status,
        // which CRTC it's using (if any), supported modes, etc.
        XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!oi) continue;

        // Skip disconnected outputs — no monitor plugged into this port.
        // Also skip outputs with no CRTC (connected but not active/enabled).
        // crtc == None means the output exists but isn't driving a display.
        if (oi->connection != RR_Connected || oi->crtc == None) {
            XRRFreeOutputInfo(oi);
            continue;
        }

        // This output is connected and active — get its CRTC info.
        // The CRTC tells us the position, resolution, and current mode.
        XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
        if (!ci) {
            XRRFreeOutputInfo(oi);
            continue;
        }

        // Build the CrystalOutput entry for this monitor.
        CrystalOutput *out = &outputs[output_count];
        memset(out, 0, sizeof(CrystalOutput));

        // Copy the output name (e.g., "HDMI-1"). Truncate if it's too long
        // for our fixed-size buffer — this should never happen in practice
        // since X output names are short, but we play it safe.
        strncpy(out->name, oi->name, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';

        // Position within the virtual screen. For a single-monitor setup
        // this is (0, 0). For multi-monitor, each monitor is offset.
        out->x = ci->x;
        out->y = ci->y;
        out->width = (int)ci->width;
        out->height = (int)ci->height;

        // Store the XRandR IDs so we can reference this output later
        // (e.g., to set properties on it for VRR control).
        out->crtc_id = (unsigned long)oi->crtc;
        out->output_id = (unsigned long)res->outputs[i];

        // Determine the refresh rate from the current mode.
        // A "mode" in XRandR defines a resolution + timing. The refresh rate
        // isn't stored directly — we calculate it from the dot clock and the
        // total horizontal/vertical timing (including blanking intervals).
        //
        // Formula: refresh_hz = dot_clock / (htotal * vtotal)
        //
        // For example, 1920x1080@60Hz typically has:
        //   dot_clock = 148500000 Hz, htotal = 2200, vtotal = 1125
        //   148500000 / (2200 * 1125) = 60.0 Hz
        out->refresh_hz = 60;  // Default fallback
        for (int m = 0; m < res->nmode; m++) {
            if (res->modes[m].id == ci->mode) {
                XRRModeInfo *mode = &res->modes[m];
                if (mode->hTotal > 0 && mode->vTotal > 0) {
                    // dotClock is in Hz, hTotal and vTotal include blanking
                    out->refresh_hz = (int)((double)mode->dotClock /
                                     ((double)mode->hTotal * (double)mode->vTotal)
                                     + 0.5);  // +0.5 for rounding
                }
                break;
            }
        }

        // Check if this output is the one XRandR considers "primary."
        out->primary = (res->outputs[i] == primary_output);
        if (out->primary) found_primary = true;

        // ── VRR capability detection ──
        //
        // AMD GPUs (the most common VRR-capable hardware on Linux) expose a
        // "vrr_capable" property on XRandR outputs. This property is set by
        // the amdgpu kernel driver when the connected monitor reports FreeSync
        // support in its EDID (Extended Display Identification Data — the
        // metadata block that monitors send to the GPU describing their
        // capabilities).
        //
        // We use XInternAtom to look up the property name, then
        // XRRGetOutputProperty to read its value. If the value is 1, the
        // monitor supports VRR.
        out->vrr_capable = false;
        out->vrr_enabled = false;

        Atom vrr_atom = XInternAtom(dpy, "vrr_capable", True);
        if (vrr_atom != None) {
            unsigned char *prop_data = NULL;
            int actual_format = 0;
            unsigned long nitems = 0, bytes_after = 0;
            Atom actual_type = None;

            // Read the property value from the X server.
            // The property is a single 32-bit integer: 1 = capable, 0 = not.
            int status = XRRGetOutputProperty(
                dpy, res->outputs[i], vrr_atom,
                0, 1,          // offset=0, length=1 (one 32-bit value)
                False,         // don't delete the property after reading
                False,         // don't request a specific type
                AnyPropertyType,
                &actual_type, &actual_format,
                &nitems, &bytes_after, &prop_data
            );

            if (status == Success && prop_data && nitems > 0) {
                // The property value is stored as a 32-bit long.
                // Cast the raw data pointer and check if it's 1 (capable).
                long value = *((long *)prop_data);
                out->vrr_capable = (value == 1);
            }

            if (prop_data) XFree(prop_data);
        }

        fprintf(stderr, "[display] Output %d: %s — %dx%d @ %dHz (pos %d,%d)%s%s\n",
                output_count, out->name, out->width, out->height,
                out->refresh_hz, out->x, out->y,
                out->primary ? " [primary]" : "",
                out->vrr_capable ? " [VRR]" : "");

        output_count++;

        // Free the XRandR info structures — we've copied what we need.
        XRRFreeCrtcInfo(ci);
        XRRFreeOutputInfo(oi);
    }

    // If no output was explicitly marked as primary, default to the first one.
    // This ensures display_get_primary() always returns something useful when
    // monitors are connected.
    if (!found_primary && output_count > 0) {
        outputs[0].primary = true;
        fprintf(stderr, "[display] No primary output set — defaulting to %s\n",
                outputs[0].name);
    }

    // Initialize the frame metrics with sensible defaults.
    CrystalOutput *primary = display_get_primary();
    metrics.target_fps = primary ? primary->refresh_hz : 60;
    metrics.fps = 0;
    metrics.last_frame_time_ms = 0.0;
    metrics.avg_frame_time_ms = 0.0;
    metrics.frame_dropped = false;

    // Initialize the FPS counter timestamp so the first second of measurement
    // starts from now, not from epoch (which would cause a huge initial spike).
    last_fps_time = get_time();
    frame_count = 0;

    // Free the top-level screen resources structure.
    XRRFreeScreenResources(res);

    // Register for XRandR notifications so we get hotplug events.
    // RROutputChangeNotifyMask triggers when monitors are plugged in or removed.
    // RRCrtcChangeNotifyMask triggers when display configuration changes.
    XRRSelectInput(dpy, display_root,
                   RRScreenChangeNotifyMask |
                   RROutputChangeNotifyMask |
                   RRCrtcChangeNotifyMask);

    fprintf(stderr, "[display] Initialized: %d output(s)\n", output_count);
    return true;
}


// ============================================================================
//  Output queries
// ============================================================================

CrystalOutput *display_get_outputs(int *count)
{
    if (count) *count = output_count;
    return outputs;
}

CrystalOutput *display_get_primary(void)
{
    // Scan the output list for the one marked as primary.
    for (int i = 0; i < output_count; i++) {
        if (outputs[i].primary) return &outputs[i];
    }

    // Fallback: return the first output if none is marked primary.
    // This shouldn't happen (display_init ensures one is marked), but
    // defensive coding never hurts.
    if (output_count > 0) return &outputs[0];

    return NULL;
}


// ============================================================================
//  VRR (Variable Refresh Rate) control
// ============================================================================

bool display_enable_vrr(CrystalOutput *output)
{
    if (!output) return false;

    // Can't enable VRR on hardware that doesn't support it.
    if (!output->vrr_capable) {
        fprintf(stderr, "[display] VRR not available on %s — hardware not capable\n",
                output->name);
        return false;
    }

    // On X11/XLibre, VRR is controlled through several layers:
    //
    //   1. Kernel: The GPU driver must support it.
    //      - AMD: amdgpu with dc=1 and freesync_video=1 kernel parameters.
    //      - Intel: i915 with enable_fbc=1 and enable_psr=1 (limited support).
    //      - NVIDIA: proprietary driver has its own G-Sync mechanism.
    //
    //   2. X server: The xorg.conf (or XLibre config) must have:
    //        Option "VariableRefresh" "true"
    //      in the Monitor or Device section.
    //
    //   3. XRandR property: The "vrr_capable" property on the output tells us
    //      the hardware chain supports it. There's no standard "enable VRR"
    //      property to set — on amdgpu with XLibre, VRR activates automatically
    //      per-CRTC when a fullscreen application delivers frames at a variable
    //      rate within the monitor's VRR range.
    //
    // So "enabling" VRR from our side mainly means:
    //   - Confirming the hardware is capable (checked above).
    //   - Setting our internal flag so the frame timing logic knows to
    //     skip fixed-rate pacing and let frames flow freely.
    //   - The actual VRR activation happens at the driver/hardware level
    //     when we start delivering frames without artificial delays.

    // Check that the X server has the vrr_capable atom — this confirms
    // the X server is VRR-aware (as opposed to a very old Xorg build).
    Atom vrr_atom = XInternAtom(display_dpy, "vrr_capable", True);
    if (vrr_atom == None) {
        fprintf(stderr, "[display] VRR not supported by X server on %s\n",
                output->name);
        return false;
    }

    // Mark VRR as enabled in our state. This affects frame timing behavior:
    // display_get_target_frame_time() will return 0 (uncapped) instead of
    // a fixed frame budget.
    output->vrr_enabled = true;
    fprintf(stderr, "[display] VRR enabled on %s (%dHz capable)\n",
            output->name, output->refresh_hz);
    return true;
}

void display_disable_vrr(CrystalOutput *output)
{
    if (!output) return;

    if (output->vrr_enabled) {
        output->vrr_enabled = false;
        fprintf(stderr, "[display] VRR disabled on %s — reverting to %dHz fixed\n",
                output->name, output->refresh_hz);
    }
}

bool display_is_vrr_active(CrystalOutput *output)
{
    if (!output) return false;

    // VRR is "active" when:
    //   1. The hardware supports it (vrr_capable).
    //   2. We've enabled it in our state (vrr_enabled).
    //   3. There's content being delivered at a variable rate.
    //
    // We can only confirm #1 and #2 from userspace. The actual VRR activation
    // at the hardware level is managed by the kernel driver. For now, we
    // report based on our state — if the hardware is capable and we've enabled
    // it, we assume VRR is active when frames are being delivered.
    return output->vrr_capable && output->vrr_enabled;
}


// ============================================================================
//  Adaptive frame timing
// ============================================================================

void display_begin_frame(void)
{
    // Record the start time of this frame. display_end_frame() will compute
    // the elapsed time to determine how long the composite pass took.
    frame_start = get_time();
}

void display_end_frame(void)
{
    double now = get_time();
    double elapsed = now - frame_start;

    // Update the last frame time (how long this frame took to render).
    metrics.last_frame_time_ms = elapsed * 1000.0;

    // Update the rolling average using exponential smoothing.
    // This gives a stable number that responds to trends without being
    // jerky from frame-to-frame noise. The 0.95/0.05 split means:
    //   - 95% of the average comes from previous history (stability).
    //   - 5% comes from the latest frame (responsiveness).
    // It takes about 20 frames for a sudden change to fully propagate.
    if (metrics.avg_frame_time_ms <= 0.0) {
        // First frame — initialize directly instead of blending with zero.
        metrics.avg_frame_time_ms = metrics.last_frame_time_ms;
    } else {
        metrics.avg_frame_time_ms = metrics.avg_frame_time_ms * 0.95
                                  + metrics.last_frame_time_ms * 0.05;
    }

    // Check if this frame exceeded its time budget.
    // At 60 Hz, budget is 16.67ms. At 120 Hz, budget is 8.33ms.
    // At 144 Hz, budget is 6.94ms.
    double budget = 1000.0 / (double)metrics.target_fps;
    metrics.frame_dropped = (metrics.last_frame_time_ms > budget);

    if (metrics.frame_dropped) {
        fprintf(stderr, "[display] Frame dropped: %.1fms (budget: %.1fms @ %dHz)\n",
                metrics.last_frame_time_ms, budget, metrics.target_fps);
    }

    // Update the FPS counter once per second.
    // We count frames in a rolling one-second window and report the total.
    frame_count++;
    if (now - last_fps_time >= 1.0) {
        metrics.fps = frame_count;
        frame_count = 0;
        last_fps_time = now;
    }
}

int display_get_target_frame_time(void)
{
    // Check if VRR is active on the primary display.
    CrystalOutput *primary = display_get_primary();

    if (primary && display_is_vrr_active(primary)) {
        // VRR is active — return 0 to signal "render as fast as possible."
        // The display will sync to whatever rate we deliver frames at,
        // so there's no fixed budget to hit. This lets the compositor
        // skip any artificial sleep/pacing logic.
        return 0;
    }

    // VRR is not active — return the fixed frame budget in microseconds.
    // The compositor should try to complete each frame within this time
    // to avoid dropped frames.
    int target_fps = primary ? primary->refresh_hz : 60;
    return 1000000 / target_fps;  // e.g., 16667us for 60Hz
}


// ============================================================================
//  Multi-monitor support
// ============================================================================

void display_handle_hotplug(Display *dpy)
{
    // A monitor was plugged in or removed. Re-enumerate all outputs
    // from scratch by calling display_init() again. It reuses the existing
    // output array and overwrites all entries.
    int old_count = output_count;

    // Flush any pending X events before re-querying, otherwise we might
    // get stale data from the server's cache.
    XSync(dpy, False);

    display_init(dpy, display_screen);

    // Log the configuration change so it's visible in debug output.
    if (output_count != old_count) {
        fprintf(stderr, "[display] Monitor configuration changed: "
                "%d -> %d output(s)\n", old_count, output_count);
    } else {
        fprintf(stderr, "[display] Monitor hotplug event processed — "
                "output count unchanged (%d)\n", output_count);
    }
}

int display_get_viewport_for_output(CrystalOutput *output,
                                    int *x, int *y, int *w, int *h)
{
    if (!output) return -1;

    // Simply copy the output's position and size. The caller uses this to
    // know which region of the composited scene corresponds to a particular
    // physical monitor — useful for rendering per-monitor backgrounds,
    // placing windows on the correct monitor, etc.
    if (x) *x = output->x;
    if (y) *y = output->y;
    if (w) *w = output->width;
    if (h) *h = output->height;

    return 0;
}


// ============================================================================
//  Gaming mode
// ============================================================================

void display_set_game_mode(GameMode mode)
{
    if (mode == current_game_mode) return;

    const char *mode_names[] = { "OFF", "BYPASS", "GAMESCOPE" };
    fprintf(stderr, "[display] Game mode: %s -> %s\n",
            mode_names[current_game_mode], mode_names[mode]);

    // Handle transitions that require cleanup of the previous mode.
    switch (current_game_mode) {
        case GAME_MODE_BYPASS:
            // Leaving bypass mode — disable direct scanout so the compositor
            // takes over rendering again.
            display_disable_direct_scanout();
            break;

        case GAME_MODE_GAMESCOPE:
            // Leaving gamescope mode — make sure the gamescope process is
            // cleaned up. This is a non-blocking check; if gamescope is still
            // running, display_return_from_gamescope() will handle it.
            display_return_from_gamescope();
            break;

        case GAME_MODE_OFF:
            // Nothing to clean up when leaving normal mode.
            break;
    }

    current_game_mode = mode;
}

GameMode display_get_game_mode(void)
{
    return current_game_mode;
}


// ============================================================================
//  Direct scanout
// ============================================================================

bool display_can_direct_scanout(Window win, Display *dpy)
{
    if (win == None || !dpy) return false;

    // Read the window's geometry and attributes.
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, win, &wa)) {
        return false;
    }

    // The window must be mapped (visible on screen).
    if (wa.map_state != IsViewable) return false;

    // Find the primary display — direct scanout targets the primary monitor.
    CrystalOutput *primary = display_get_primary();
    if (!primary) return false;

    // ── Condition 1: The window must cover the entire primary display. ──
    //
    // If the window is smaller than the display, or offset from (0,0),
    // there would be regions of the display with nothing to show — the
    // hardware can't scan out a partial window.
    if (wa.x != primary->x || wa.y != primary->y ||
        wa.width != primary->width || wa.height != primary->height) {
        return false;
    }

    // ── Condition 2: The window must be opaque (no alpha channel). ──
    //
    // A 32-bit depth means the window has an alpha channel (ARGB visual).
    // Direct scanout can't blend transparent windows with what's behind them —
    // that's what the compositor is for. Only fully opaque windows (24-bit
    // RGB, no alpha) can be scanned out directly.
    if (wa.depth == 32) return false;

    // ── Condition 3: The window should be the topmost visible window. ──
    //
    // If another window is above the candidate, the display hardware would
    // show the wrong window. We check the stacking order by querying the
    // root window's children (which are in bottom-to-top stacking order)
    // and verifying our target window is on top.
    Window root_return, parent_return;
    Window *children = NULL;
    unsigned int nchildren = 0;

    if (XQueryTree(dpy, display_root, &root_return, &parent_return,
                   &children, &nchildren)) {

        bool is_topmost = false;

        // Walk the stacking order from top to bottom.
        // The last element in the children array is the topmost window.
        for (int i = (int)nchildren - 1; i >= 0; i--) {
            XWindowAttributes child_wa;
            if (!XGetWindowAttributes(dpy, children[i], &child_wa)) continue;

            // Skip unmapped (invisible) windows — they don't count.
            if (child_wa.map_state != IsViewable) continue;

            // The first visible window we encounter (from the top) should
            // be our candidate. If it's not, something is above us.
            if (children[i] == win) {
                is_topmost = true;
            }
            break;  // Only check the topmost visible window
        }

        if (children) XFree(children);

        if (!is_topmost) return false;
    }

    // All conditions met — this window is eligible for direct scanout.
    return true;
}

void display_enable_direct_scanout(Window win, Display *dpy)
{
    if (win == None || !dpy) return;

    // Verify the window is actually eligible before enabling.
    if (!display_can_direct_scanout(win, dpy)) {
        fprintf(stderr, "[display] Cannot enable direct scanout — "
                "window does not meet requirements\n");
        return;
    }

    // If we're already scanning out a different window, disable that first
    // so it gets re-redirected back to off-screen compositing.
    if (direct_scanout_win != None && direct_scanout_win != win) {
        display_disable_direct_scanout();
    }

    // ── Unredirect the window via XComposite ──
    //
    // Normally, Crystal calls XCompositeRedirectSubwindows() during init,
    // which tells the X server to draw every window into an off-screen pixmap
    // instead of directly to the screen. Crystal then reads those pixmaps as
    // GL textures and composites them together.
    //
    // XCompositeUnredirectWindow() reverses this for a single window. The X
    // server goes back to painting that window directly to the framebuffer,
    // completely bypassing our compositor. The result:
    //   - Zero extra buffer copies (the game's buffer goes straight to the
    //     display via the X server's built-in DRI path).
    //   - One less frame of latency (no waiting for Crystal to composite).
    //   - Reduced GPU load (Crystal doesn't need to texture-map this window).
    //
    // CompositeRedirectManual matches the redirect mode we used when setting
    // up redirection in crystal.c — the unredirect mode must match the
    // original redirect mode.
    XCompositeUnredirectWindow(dpy, win, CompositeRedirectManual);

    // Raise the window to the top of the stacking order. This ensures the
    // X server displays it above everything else — since we're not compositing,
    // stacking order is handled by the X server's painter's algorithm.
    XRaiseWindow(dpy, win);

    // Record that direct scanout is now active. The compositor's main loop
    // checks direct_scanout_win to know whether to skip rendering.
    direct_scanout_win = win;

    // Flush the connection so the unredirect takes effect immediately.
    // Without this, the request might sit in Xlib's send buffer for a few
    // milliseconds, causing a visible glitch frame where the compositor still
    // draws the old content.
    XFlush(dpy);

    fprintf(stderr, "[display] Direct scanout enabled for window 0x%lx "
            "(XComposite unredirect)\n", (unsigned long)win);
}

void display_disable_direct_scanout(void)
{
    if (direct_scanout_win == None) return;

    if (!display_dpy) {
        // If the display connection is gone (e.g., during shutdown), just
        // clear the state — we can't talk to the X server anymore.
        direct_scanout_win = None;
        return;
    }

    // ── Re-redirect the window back to off-screen compositing ──
    //
    // This reverses the XCompositeUnredirectWindow() call from
    // display_enable_direct_scanout(). The X server will once again draw this
    // window into an off-screen pixmap, and Crystal will composite it via GL
    // on the next frame.
    //
    // CompositeRedirectManual must match the mode used in both the original
    // redirect (in crystal.c) and the unredirect (in enable_direct_scanout).
    //
    // Check that the window still exists before redirecting — it may have
    // been destroyed between the scanout enable and this disable call.
    XWindowAttributes wa;
    if (XGetWindowAttributes(display_dpy, direct_scanout_win, &wa)) {
        XCompositeRedirectWindow(display_dpy, direct_scanout_win,
                                 CompositeRedirectManual);
    } else {
        fprintf(stderr, "[crystal_display] Scanout window 0x%lx already destroyed, "
                "skipping redirect\n", (unsigned long)direct_scanout_win);
    }

    fprintf(stderr, "[display] Direct scanout disabled for window 0x%lx — "
            "resuming compositing\n", (unsigned long)direct_scanout_win);

    direct_scanout_win = None;

    // Flush so the re-redirect takes effect before the next composite pass.
    // This prevents a blank frame between the scanout window being removed
    // from direct display and the compositor picking it back up.
    XFlush(display_dpy);
}


// display_check_direct_scanout() — Per-frame check for direct scanout state.
//
// This function is the "brain" of the direct scanout system. It runs once per
// frame and makes the decision: should the compositor be rendering, or should
// we let a fullscreen game bypass us entirely?
//
// The logic is straightforward:
//   - If scanout is already active, verify the window is still eligible.
//     Games can exit fullscreen, get covered by a notification, or close
//     entirely — any of those means we need to take compositing back.
//   - If scanout is not active, scan the client list for a window that
//     qualifies. This catches the moment a game goes fullscreen.
//
// Returns true if direct scanout is active (compositor should skip rendering).
bool display_check_direct_scanout(AuraWM *wm)
{
    if (!wm || !wm->dpy) return false;

    // ── Case 1: Scanout is currently active — validate the window ──
    if (direct_scanout_win != None) {

        // Check if the window is still eligible for direct scanout.
        // It might have left fullscreen, been covered by another window,
        // or been destroyed entirely.
        if (!display_can_direct_scanout(direct_scanout_win, wm->dpy)) {
            // The window no longer qualifies — disable scanout so Crystal
            // takes over compositing again on the next frame.
            fprintf(stderr, "[display] Scanout window 0x%lx no longer eligible "
                    "— disabling direct scanout\n",
                    (unsigned long)direct_scanout_win);
            display_disable_direct_scanout();
            return false;
        }

        // Still eligible — keep scanout active, compositor skips rendering.
        return true;
    }

    // ── Case 2: Scanout is not active — look for an eligible window ──
    //
    // Walk the client list and check each window. In practice, at most one
    // window can qualify (it must cover the entire display and be topmost),
    // so this loop typically exits on the first fullscreen window it finds.
    for (int i = 0; i < wm->num_clients; i++) {
        Window win = wm->clients[i].client;
        if (win == None) continue;

        if (display_can_direct_scanout(win, wm->dpy)) {
            // Found a fullscreen, opaque, topmost window — enable scanout.
            fprintf(stderr, "[display] Window 0x%lx qualifies for direct "
                    "scanout — enabling bypass\n", (unsigned long)win);
            display_enable_direct_scanout(win, wm->dpy);

            // If enable succeeded (the window state was set), return true.
            // If it failed (e.g., a race condition changed the window between
            // our check and the enable call), return false so we composite.
            return (direct_scanout_win != None);
        }
    }

    // No window qualifies — normal compositing continues.
    return false;
}


// ============================================================================
//  gamescope integration
// ============================================================================

bool display_launch_gamescope(const char *game_command)
{
    if (!game_command || game_command[0] == '\0') {
        fprintf(stderr, "[display] ERROR: No game command provided for gamescope\n");
        return false;
    }

    // Don't launch a second gamescope if one is already running.
    if (gamescope_pid > 0) {
        // Check if the previous gamescope is still alive.
        int status;
        pid_t result = waitpid(gamescope_pid, &status, WNOHANG);
        if (result == 0) {
            // Still running — refuse to launch another.
            fprintf(stderr, "[display] gamescope is already running (PID %d)\n",
                    gamescope_pid);
            return false;
        }
        // It exited — clear the PID so we can launch a new one.
        gamescope_pid = 0;
    }

    CrystalOutput *primary = display_get_primary();
    if (!primary) {
        fprintf(stderr, "[display] ERROR: No primary output for gamescope\n");
        return false;
    }

    // Build the gamescope command line.
    //
    // gamescope flags:
    //   -W <width>    — Output width (match the primary display).
    //   -H <height>   — Output height (match the primary display).
    //   -r <rate>     — Refresh rate limit (match the display's max Hz).
    //   --adaptive-sync — Enable FreeSync/VRR within gamescope.
    //   -- <command>  — Everything after "--" is the game to launch.
    // SECURITY: Build argv array instead of passing through a shell.
    // Using "sh -c" with user-supplied strings is a command injection
    // vulnerability. Instead, we use execvp() with a proper argv array
    // where game_command is passed as a single argument, not interpreted
    // by a shell.
    char w_str[16], h_str[16], r_str[16];
    snprintf(w_str, sizeof(w_str), "%d", primary->width);
    snprintf(h_str, sizeof(h_str), "%d", primary->height);
    snprintf(r_str, sizeof(r_str), "%d", primary->refresh_hz);

    fprintf(stderr, "[display] Launching gamescope: -W %s -H %s -r %s --adaptive-sync -- %s\n",
            w_str, h_str, r_str, game_command);

    // Fork a child process to run gamescope.
    pid_t pid = fork();

    if (pid < 0) {
        perror("[display] fork failed");
        return false;
    }

    if (pid == 0) {
        // ── Child process ──
        setsid();

        // SECURITY: Use execvp with an argv array — NO shell interpretation.
        // game_command is passed as a single argument after "--", so even if
        // it contains shell metacharacters (;, |, $, etc.), they are treated
        // as literal characters, not shell commands.
        execlp("gamescope", "gamescope",
               "-W", w_str, "-H", h_str, "-r", r_str,
               "--adaptive-sync", "--", game_command, NULL);

        // If we get here, execlp() failed
        perror("[display] execlp failed");
        _exit(1);
    }

    // ── Parent process ──
    //
    // Store the child PID so we can check on it later (waitpid) and so
    // display_return_from_gamescope() knows what to wait for.
    gamescope_pid = pid;
    display_set_game_mode(GAME_MODE_GAMESCOPE);

    fprintf(stderr, "[display] gamescope launched (PID %d)\n", gamescope_pid);
    return true;
}

void display_return_from_gamescope(void)
{
    if (gamescope_pid <= 0) return;

    // Check if gamescope has already exited.
    // WNOHANG means "don't block — just check and return immediately."
    int status;
    pid_t result = waitpid(gamescope_pid, &status, WNOHANG);

    if (result == 0) {
        // gamescope is still running. Send it SIGTERM (polite shutdown request)
        // and wait for it to exit gracefully.
        fprintf(stderr, "[display] Asking gamescope (PID %d) to exit...\n",
                gamescope_pid);
        kill(gamescope_pid, SIGTERM);

        // Wait up to 5 seconds for gamescope to shut down.
        // We check every 100ms to avoid blocking the WM for too long.
        for (int i = 0; i < 50; i++) {
            usleep(100000);  // 100ms
            result = waitpid(gamescope_pid, &status, WNOHANG);
            if (result != 0) break;
        }

        // If it still hasn't exited after 5 seconds, force-kill it.
        if (result == 0) {
            fprintf(stderr, "[display] gamescope did not exit — sending SIGKILL\n");
            kill(gamescope_pid, SIGKILL);
            waitpid(gamescope_pid, &status, 0);  // Block until dead
        }
    }

    // Log the exit status for debugging.
    if (WIFEXITED(status)) {
        fprintf(stderr, "[display] gamescope exited with code %d\n",
                WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[display] gamescope killed by signal %d\n",
                WTERMSIG(status));
    }

    gamescope_pid = 0;

    // Switch back to normal compositing mode if we were in gamescope mode.
    // Note: we set current_game_mode directly here instead of calling
    // display_set_game_mode() to avoid infinite recursion (set_game_mode
    // calls return_from_gamescope when transitioning out of GAMESCOPE mode).
    if (current_game_mode == GAME_MODE_GAMESCOPE) {
        current_game_mode = GAME_MODE_OFF;
        fprintf(stderr, "[display] Resumed Crystal compositing\n");
    }
}


// ============================================================================
//  PipeWire screencast (stubs)
// ============================================================================
//
// These are placeholder implementations. The real PipeWire integration will
// be added when we build screen sharing support. For now, they log a message
// and return failure/no-op so the rest of the codebase can call them without
// crashing.

bool display_init_screencast(void)
{
    // TODO: Initialize PipeWire connection, create a screencast stream,
    // negotiate buffer format (DMA-BUF preferred for zero-copy GPU access).
    fprintf(stderr, "[display] PipeWire screencast not yet implemented\n");
    return false;
}

void display_provide_frame(unsigned int texture, int width, int height)
{
    // Suppress unused parameter warnings — these will be used when we
    // implement the actual PipeWire frame delivery.
    (void)texture;
    (void)width;
    (void)height;

    // TODO: Copy the GL texture into a PipeWire buffer and enqueue it
    // for delivery to screen sharing clients.
}

void display_shutdown_screencast(void)
{
    // TODO: Disconnect from PipeWire and free screencast resources.
    fprintf(stderr, "[display] PipeWire screencast shutdown (no-op)\n");
}


// ============================================================================
//  Metrics
// ============================================================================

FrameMetrics display_get_metrics(void)
{
    // Return a copy of the current metrics struct. Since this is a plain
    // struct copy (not a pointer), the caller gets a snapshot that won't
    // change out from under them if another frame completes while they're
    // reading the values.
    return metrics;
}


// ============================================================================
//  Shutdown
// ============================================================================

void display_shutdown(void)
{
    fprintf(stderr, "[display] Shutting down display management...\n");

    // Disable VRR on all outputs where we enabled it, so the displays
    // return to their default fixed refresh rate.
    for (int i = 0; i < output_count; i++) {
        if (outputs[i].vrr_enabled) {
            display_disable_vrr(&outputs[i]);
        }
    }

    // Clean up any active gaming mode state.
    if (current_game_mode == GAME_MODE_BYPASS) {
        display_disable_direct_scanout();
    } else if (current_game_mode == GAME_MODE_GAMESCOPE) {
        display_return_from_gamescope();
    }
    current_game_mode = GAME_MODE_OFF;

    // Shut down PipeWire screencast if it was initialized.
    display_shutdown_screencast();

    // Free the output array.
    if (outputs) {
        free(outputs);
        outputs = NULL;
    }
    output_count = 0;

    // Clear cached state.
    display_dpy = NULL;
    display_root = None;
    direct_scanout_win = None;
    gamescope_pid = 0;

    // Reset frame timing state.
    frame_start = 0.0;
    frame_count = 0;
    last_fps_time = 0.0;
    memset(&metrics, 0, sizeof(metrics));

    fprintf(stderr, "[display] Display management shut down\n");
}
