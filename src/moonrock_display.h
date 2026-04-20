// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/
//
// ============================================================================
//  MoonRock Display Management — VRR, Multi-Monitor, Direct Scanout
// ============================================================================
//
// This module handles everything related to display outputs and how MoonRock
// interacts with them. While mr.c owns the GL compositing pipeline, this
// module owns the *displays* that pipeline renders to.
//
// What this module does:
//
//   1. DISPLAY ENUMERATION — Uses XRandR to discover all connected monitors,
//      their resolutions, refresh rates, and positions in the virtual screen.
//      Monitors can be added/removed at runtime (hotplug), and this module
//      handles re-enumeration when that happens.
//
//   2. VARIABLE REFRESH RATE (VRR) — Also known as FreeSync (AMD) or Adaptive
//      Sync. Instead of the monitor refreshing at a fixed rate (e.g., 60 Hz),
//      VRR lets the monitor sync to the GPU's actual frame delivery. This
//      eliminates tearing and judder without the latency penalty of traditional
//      VSync. This module detects VRR-capable outputs and manages the enable/
//      disable lifecycle.
//
//   3. ADAPTIVE FRAME TIMING — When VRR is active, we don't need to hit a
//      fixed frame budget (e.g., 16.67ms for 60 Hz). The display_begin_frame()
//      and display_end_frame() functions track frame timing and provide metrics
//      so the compositor can make informed scheduling decisions.
//
//   4. DIRECT SCANOUT — When a fullscreen application covers an entire display
//      (e.g., a game), the compositor is just wasting GPU cycles copying that
//      window's buffer to the screen. Direct scanout tells the display hardware
//      to read directly from the application's buffer, bypassing compositing
//      entirely. This reduces latency and saves power.
//
//   5. PIPEWIRE SCREENCAST — For screen sharing (Discord, OBS, etc.), this
//      module can provide compositor frames to PipeWire. This is a stub for
//      now — the actual PipeWire integration will come later.
//
// NOTE: MoonRock does NOT launch gamescope in-session. The pure-Gamescope
// gaming experience lives in its own SDDM session (copycatos-gaming). This
// module's only relationship with gamescope is that a nested gamescope
// spawned by moonbase (for a Wayland-native app) presents as an X11 client
// MoonRock reparents like any other — no special-case handling here.
//
// ============================================================================

#ifndef MR_DISPLAY_H
#define MR_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include <X11/Xlib.h>

// Forward declaration — full definition lives in wm_compat.h.
// We only need the pointer type here, not the struct internals.
struct CCWM;


// ============================================================================
//  Display output information
// ============================================================================
//
// Each physical monitor connected to the system is represented by a
// MROutput struct. XRandR (X Resize and Rotate) is the X11 extension
// that provides monitor information — resolution, position, refresh rate,
// and hardware capabilities like VRR.
//
// The "virtual screen" is the combined area of all monitors. Each monitor
// occupies a rectangle within this virtual screen, defined by (x, y, width,
// height). For example, two 1920x1080 monitors side-by-side would create a
// 3840x1080 virtual screen, with the left monitor at (0,0) and the right
// at (1920,0).

typedef struct {
    char name[64];           // Human-readable name like "eDP-1", "HDMI-1", "DP-2"
    int x, y;                // Top-left corner in the virtual screen coordinate space
    int width, height;       // Resolution in pixels (e.g., 1920x1080, 2560x1440)
    int refresh_hz;          // Current refresh rate in Hertz (e.g., 60, 120, 144)
    bool vrr_capable;        // True if the hardware supports FreeSync/Adaptive Sync
    bool vrr_enabled;        // True if VRR is currently turned on for this output
    bool primary;            // True if this is the user's primary display
    unsigned long crtc_id;   // XRandR CRTC ID — a CRTC (CRT Controller) is the
                             // hardware block that drives a display output. Each
                             // active monitor is connected through exactly one CRTC.
    unsigned long output_id; // XRandR output ID — represents the physical connector
                             // (HDMI port, DisplayPort, etc.)

    // ── Per-output HiDPI scale (CopyCatOS HiDPI mandate) ──
    //
    // Every output carries its own scale factor. The public MoonBase API
    // measures in points — MoonRock converts points to physical pixels
    // per-output using this scale. Fractional scales (1.25, 1.5, 1.75,
    // …) are first-class — no integer rounding anywhere.
    //
    // edid_hash   — 64-bit FNV-1a hash of the raw EDID blob. Stable
    //               across reboots and cable swaps. The persistence
    //               file keys per-output overrides on this hash, so the
    //               same monitor gets the same scale when plugged back
    //               in, regardless of which port it lands on.
    // mm_width/mm_height — physical panel size in millimeters, as
    //               reported by the EDID (X server parses this into
    //               XRROutputInfo->mm_width / mm_height). Combined with
    //               pixel resolution, gives PPI.
    // default_scale — picked from PPI bands during enumeration. The
    //               fallback the system chooses when the user hasn't
    //               set anything for this particular monitor.
    // user_scale — user's persisted override for this EDID hash, or
    //               0.0 if no override exists. Set from the Displays
    //               pane in SysPrefs (later slice) and written out via
    //               display_set_scale_for_output().
    // scale      — the effective scale to use right now (user_scale if
    //               non-zero, otherwise default_scale). This is the
    //               value window_create replies, backing-scale queries,
    //               and chrome rendering all consult.
    uint8_t       edid_hash[8];
    int           mm_width, mm_height;
    float         default_scale;
    float         user_scale;
    float         scale;
} MROutput;


// ============================================================================
//  Compositor scanout mode
// ============================================================================
//
// Two compositing paths for fullscreen clients:
//
//   OFF     — Normal compositing. Every window goes through MoonRock's GL
//             pipeline. One extra buffer copy, all compositor effects work.
//
//   BYPASS  — Direct scanout. The fullscreen window's buffer is sent directly
//             to display hardware, skipping the compositor entirely. Lowest
//             possible latency. Used for full-screen unredirected clients on
//             any output (multi-monitor direct-scanout fast path).

typedef enum {
    GAME_MODE_OFF,           // Normal compositing — all windows go through MoonRock
    GAME_MODE_BYPASS,        // Direct scanout — buffer goes straight to display
} GameMode;


// ============================================================================
//  Frame timing metrics
// ============================================================================
//
// These metrics help the compositor (and debugging tools) understand how well
// we're hitting our frame targets. A "frame drop" means we took longer than
// the display's refresh interval to produce a frame — the user sees a stutter.
//
// The rolling average uses exponential smoothing (95% old + 5% new) so it
// reacts to trends without being jumpy from frame-to-frame noise.

typedef struct {
    double last_frame_time_ms;   // How long the last composite pass took (ms)
    double avg_frame_time_ms;    // Exponentially smoothed rolling average (ms)
    int fps;                     // Measured frames per second (updated once/sec)
    int target_fps;              // Target FPS based on the display's refresh rate
    bool frame_dropped;          // True if the last frame exceeded its time budget
} FrameMetrics;


// ============================================================================
//  Initialization and shutdown
// ============================================================================

// Initialize the display management subsystem.
//
// This queries XRandR for all connected outputs, reads their properties
// (resolution, refresh rate, VRR capability), and registers for hotplug
// notifications so we're informed when monitors are plugged in or removed.
//
// Parameters:
//   dpy    — The X display connection (same one the WM uses everywhere).
//   screen — The default screen number (usually 0 for single-GPU setups).
//
// Returns true on success, false if XRandR is missing or output enumeration
// fails.
bool display_init(Display *dpy, int screen);

// Shut down display management and free all resources.
//
// Disables VRR on any outputs where we enabled it, disables direct scanout
// if active, shuts down PipeWire screencast, and frees the output list.
void display_shutdown(void);


// ============================================================================
//  Output queries
// ============================================================================

// Get the list of all currently connected and active display outputs.
//
// Parameters:
//   count — Receives the number of outputs in the returned array.
//
// Returns a pointer to an internal array of MROutput structs. This
// pointer is valid until the next call to display_init() or
// display_handle_hotplug(). Do NOT free the returned pointer.
MROutput *display_get_outputs(int *count);

// Get the primary display output.
//
// The primary display is where fullscreen games default to and where direct
// scanout targets. If no output is explicitly marked as primary by XRandR,
// we pick the first one.
//
// Returns NULL if no outputs are available (e.g., all monitors unplugged).
MROutput *display_get_primary(void);


// ============================================================================
//  VRR (Variable Refresh Rate) control
// ============================================================================
//
// VRR lets the monitor's refresh rate match the GPU's frame output rate,
// instead of running at a fixed interval. Without VRR, if the GPU can only
// produce 50 FPS on a 60 Hz monitor, every few frames one frame is displayed
// twice (causing judder) or tearing occurs.
//
// On Linux with AMD GPUs (the most common VRR-capable setup):
//   - The kernel driver (amdgpu) must have dc=1 and freesync_video=1.
//   - The X server (XLibre/Xorg) must have "VariableRefresh" enabled.
//   - XRandR exposes a "vrr_capable" property on capable outputs.
//   - VRR activates automatically when a fullscreen application runs at a
//     frame rate within the monitor's VRR range.

// Enable VRR on a specific output.
//
// Checks that the output is VRR-capable and that the X server supports it.
// Returns true if VRR was successfully enabled, false otherwise.
bool display_enable_vrr(MROutput *output);

// Disable VRR on a specific output, returning to fixed refresh rate.
void display_disable_vrr(MROutput *output);

// Check if VRR is currently active (not just enabled, but actually varying
// the refresh rate because content is being delivered at variable intervals).
bool display_is_vrr_active(MROutput *output);


// ============================================================================
//  Adaptive frame timing
// ============================================================================
//
// These functions bracket each compositor frame. They're used to:
//   - Track how long each frame takes to render.
//   - Update the rolling frame time average and FPS counter.
//   - Detect dropped frames (exceeded the time budget).
//
// When VRR is active, the "target frame time" is 0, meaning "render as fast
// as you can and let the display sync to you." When VRR is off, the target
// is derived from the display's refresh rate (e.g., 16667 microseconds for
// 60 Hz).

// Call at the start of each composite pass, before any rendering.
void display_begin_frame(void);

// Call at the end of each composite pass, after SwapBuffers.
void display_end_frame(void);

// Get the target frame time in microseconds.
//
// Returns 0 when VRR is active (render as fast as possible, the display
// adapts). Returns the per-frame budget otherwise (e.g., 16667us for 60Hz,
// 8333us for 120Hz, 6944us for 144Hz).
int display_get_target_frame_time(void);


// ============================================================================
//  Multi-monitor support
// ============================================================================

// Handle a monitor hotplug event (RRNotify from XRandR).
//
// When a monitor is plugged in or removed, the X server sends an RRNotify
// event. This function re-enumerates all outputs and updates the internal
// list. The compositor should re-layout its rendering after this call.
//
// Parameters:
//   dpy — The X display connection.
void display_handle_hotplug(Display *dpy);

// ============================================================================
//  Per-output scale (HiDPI)
// ============================================================================
//
// Apps measure in points. MoonRock maps points -> physical pixels per output
// using the scale returned here. Backing scale for a MoonBase window is the
// scale of the output currently hosting it.

// Effective scale for the given output (user override if set, otherwise the
// default picked from EDID-derived PPI during enumeration). Returns 1.0 if
// output is NULL so callers never have to null-check.
float display_get_scale_for_output(const MROutput *output);

// Effective scale for the primary output — shortcut for the common case where
// a brand-new window lands on the primary display.
float display_get_primary_scale(void);

// Persist a user-override scale for the output's EDID. Updates the in-memory
// user_scale + scale of every connected output that shares that EDID hash,
// then rewrites the persistence file at
// ~/.local/share/moonrock/display-scales.conf. A scale of 0.0 clears the
// override (reverts to the default). Returns true on a successful write.
bool display_set_scale_for_output(MROutput *output, float scale);


// ── Reverse scale-request atom: pane → MoonRock ────────────────────────
// The Displays pane in systemcontrol writes its chosen scale to the
// _MOONROCK_SET_OUTPUT_SCALE root-window property (see moonrock_scale.h).
// The WM event loop calls display_handle_scale_request() from the
// PropertyNotify dispatch; this parses the line, looks up the named
// output, and applies the change via display_set_scale_for_output(). The
// property is deleted after handling so a second write of the same value
// still produces a PropertyNotify.

// Atom for _MOONROCK_SET_OUTPUT_SCALE. Interned lazily on first call.
// Safe to invoke from any thread that already has the display lock.
Atom display_scale_request_atom(Display *dpy);

// Read, parse, dispatch, and delete the request property on `root`.
// No-op if the property is missing or malformed.
void display_handle_scale_request(Display *dpy, Window root);


// Get the viewport rectangle for a specific output.
//
// Fills in the position and size of the given output within the virtual
// screen. This is useful for determining which region of the composited
// scene corresponds to a particular physical monitor.
//
// Parameters:
//   output — The output to query.
//   x, y   — Receives the top-left corner in virtual screen coordinates.
//   w, h   — Receives the width and height in pixels.
//
// Returns 0 on success, -1 if the output pointer is NULL.
int display_get_viewport_for_output(MROutput *output,
                                    int *x, int *y, int *w, int *h);


// ============================================================================
//  Compositor scanout mode control
// ============================================================================

// Set the current scanout mode.
//
// Switching modes has side effects:
//   - OFF -> BYPASS: enables direct scanout on the focused fullscreen window.
//   - BYPASS -> OFF: disables direct scanout, resumes normal compositing.
void display_set_game_mode(GameMode mode);

// Get the current scanout mode.
GameMode display_get_game_mode(void);


// ============================================================================
//  Direct scanout
// ============================================================================
//
// Direct scanout is an optimization where the display hardware reads pixel
// data directly from an application's buffer, bypassing the compositor
// entirely. This eliminates one full buffer copy per frame, reducing latency
// by one frame's worth of time (e.g., ~16ms at 60Hz).
//
// For direct scanout to work, several conditions must be met:
//   - The window must cover the entire display (no partial coverage).
//   - The window must be opaque (no alpha channel / transparency).
//   - The window must be the topmost visible window on that display.
//   - The window's buffer format must match what the display hardware expects.

// Check if a specific window is eligible for direct scanout.
//
// Parameters:
//   win — The X Window to check.
//   dpy — The X display connection.
//
// Returns true if all conditions for direct scanout are met.
bool display_can_direct_scanout(Window win, Display *dpy);

// Enable direct scanout for a fullscreen window.
//
// The compositor stops rendering frames for the display this window covers.
// The display hardware reads directly from the window's buffer.
void display_enable_direct_scanout(Window win, Display *dpy);

// Disable direct scanout and return to normal compositing.
//
// Called when the window is no longer fullscreen, another window is raised
// above it, or gaming mode is switched off.
void display_disable_direct_scanout(void);

// Check if direct scanout should be enabled or disabled this frame.
//
// This is the main entry point for automatic direct scanout management.
// Call it once per frame (typically from the compositor's main loop). It
// examines the current window state and decides whether to enable or
// disable direct scanout:
//
//   - If scanout is currently active, it checks whether the scanout window
//     is still eligible (still fullscreen, still topmost, etc.). If not,
//     it disables scanout so the compositor resumes.
//
//   - If scanout is not active, it scans the client list for any window
//     that qualifies for direct scanout. If one is found, it enables
//     scanout for that window.
//
// Parameters:
//   wm — The window manager state, which provides the client list and
//         X display connection needed to check window properties.
//
// Returns true if direct scanout is currently active (the compositor
// should skip rendering). Returns false if the compositor should render
// normally.
bool display_check_direct_scanout(struct CCWM *wm);


// ============================================================================
//  PipeWire screencast (stub)
// ============================================================================
//
// PipeWire is the modern Linux multimedia framework that handles screen
// sharing for applications like Discord, OBS, and browser-based video calls.
// A compositor needs to provide frames to PipeWire for screen sharing to work.
//
// These are stubs for now — the actual PipeWire integration will be built
// when we add screen sharing support. The API is defined early so other
// modules can reference it.

// Initialize PipeWire screencast support.
// Returns true if PipeWire is available and the screencast stream was set up.
bool display_init_screencast(void);

// Provide a rendered frame to the PipeWire screencast stream.
//
// Parameters:
//   texture — The OpenGL texture ID containing the composited frame.
//   width   — Frame width in pixels.
//   height  — Frame height in pixels.
void display_provide_frame(unsigned int texture, int width, int height);

// Shut down the PipeWire screencast stream and release resources.
void display_shutdown_screencast(void);


// ============================================================================
//  Metrics
// ============================================================================

// Get the current frame timing metrics.
//
// Returns a snapshot of the latest metrics. The returned struct is a copy,
// so it's safe to read without worrying about concurrent updates.
FrameMetrics display_get_metrics(void);


#endif // MR_DISPLAY_H
