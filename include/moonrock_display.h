// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/
//
// ============================================================================
//  MoonRock Display Management — VRR, Multi-Monitor, Direct Scanout, Gaming
// ============================================================================
//
// This module handles everything related to display outputs and how MoonRock
// interacts with them. While moonrock.c owns the GL compositing pipeline, this
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
//   5. GAMESCOPE INTEGRATION — gamescope is Valve's micro-compositor designed
//      for gaming on Linux (used in SteamOS/Steam Deck). When a game launches,
//      MoonRock can hand off display control to gamescope, which provides its
//      own optimized compositing, scaling, and VRR management. When the game
//      exits, control returns to MoonRock.
//
//   6. PIPEWIRE SCREENCAST — For screen sharing (Discord, OBS, etc.), this
//      module can provide compositor frames to PipeWire. This is a stub for
//      now — the actual PipeWire integration will come later.
//
// ============================================================================

#ifndef MR_DISPLAY_H
#define MR_DISPLAY_H

#include <stdbool.h>
#include <X11/Xlib.h>

// Forward declaration — full definition lives in wm_compat.h.
// We only need the pointer type here, not the struct internals.
struct AuraWM;


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
} MROutput;


// ============================================================================
//  Gaming mode
// ============================================================================
//
// Gaming mode controls how MoonRock handles fullscreen games. There are three
// strategies, each with different trade-offs:
//
//   OFF     — Normal compositing. Every window (including the game) goes through
//             MoonRock's GL pipeline. This adds a small amount of latency (one
//             extra buffer copy) but keeps all compositor effects working.
//
//   BYPASS  — Direct scanout. The game's buffer is sent directly to the display
//             hardware, skipping the compositor entirely. Lowest possible latency,
//             but overlays (notifications, Steam overlay) won't render on top of
//             the game unless they use the game's own overlay mechanism.
//
//   GAMESCOPE — MoonRock hands off display control to Valve's gamescope compositor,
//              which is purpose-built for gaming. Gamescope handles its own VRR,
//              scaling (FSR), and frame limiting. When the game exits, MoonRock
//              takes control back.

typedef enum {
    GAME_MODE_OFF,           // Normal compositing — all windows go through MoonRock
    GAME_MODE_BYPASS,        // Direct scanout — game buffer goes straight to display
    GAME_MODE_GAMESCOPE,     // Handed off to gamescope for dedicated game compositing
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
// Disables VRR on any outputs where we enabled it, kills any running
// gamescope process, shuts down PipeWire screencast if active, and frees
// the output list.
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
// The primary display is where fullscreen games default to, where direct
// scanout targets, and where gamescope launches. If no output is explicitly
// marked as primary by XRandR, we pick the first one.
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
//  Gaming mode control
// ============================================================================

// Set the current gaming mode.
//
// Switching modes has side effects:
//   - OFF -> BYPASS: enables direct scanout on the focused fullscreen window.
//   - OFF -> GAMESCOPE: launches gamescope and suspends MoonRock compositing.
//   - BYPASS -> OFF: disables direct scanout, resumes normal compositing.
//   - GAMESCOPE -> OFF: waits for gamescope to exit, resumes MoonRock.
void display_set_game_mode(GameMode mode);

// Get the current gaming mode.
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
bool display_check_direct_scanout(struct AuraWM *wm);


// ============================================================================
//  gamescope integration
// ============================================================================
//
// gamescope is Valve's micro-compositor for gaming on Linux (SteamOS, Steam
// Deck). It provides its own VRR management, FSR upscaling, frame limiting,
// and overlay support — all optimized for a single game.
//
// When we "hand off" to gamescope, MoonRock stops compositing and gamescope
// takes over the display. When the game exits, gamescope exits, and MoonRock
// resumes.

// Launch gamescope with a specific game command.
//
// Builds a gamescope command line using the primary display's resolution and
// refresh rate, then forks a child process to run it. MoonRock switches to
// GAME_MODE_GAMESCOPE.
//
// Parameters:
//   game_command — The shell command to launch the game (passed to gamescope
//                  after the "--" separator).
//
// Returns true if gamescope was launched successfully, false on fork failure
// or if gamescope is not installed.
bool display_launch_gamescope(const char *game_command);

// Return from gamescope to normal MoonRock compositing.
//
// Waits for the gamescope child process to exit (non-blocking check), then
// switches back to GAME_MODE_OFF and resumes MoonRock compositing.
void display_return_from_gamescope(void);


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
