// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/
//
// ============================================================================
//  MoonRock Robust — production hardening for the MoonRock Compositor
// ============================================================================
//
// This module is the "last mile" that makes MoonRock ready for daily use. It
// wraps up all the small-but-critical things a real compositor needs:
//
//   1. Crash recovery — if OpenGL dies, gracefully fall back to unredirected
//      mode so the user still has a usable desktop (just without effects).
//   2. Hardware cursor — uses XFixes to ensure the mouse cursor is always
//      rendered by the GPU, so it stays smooth even during heavy compositing.
//   3. Session state — saves window positions and Space assignments to disk
//      at logout, and restores them on next login. No more rearranging
//      windows every time you reboot.
//   4. AT-SPI accessibility — registers with the accessibility bus (DBus) so
//      screen readers like Orca can discover and read our windows.
//   5. Power management — reads battery state from sysfs and reduces effects
//      when the laptop is unplugged and running low.
//   6. Structured logging — proper log levels (debug/info/warn/error) with
//      timestamps, color output, and optional log file.
//   7. Memory safety macros — null-pointer and bounds-checking helpers that
//      log errors instead of segfaulting.
//
// None of this is glamorous, but it is what separates a demo from software
// you can actually use every day.
//
// ============================================================================

#ifndef MR_ROBUST_H
#define MR_ROBUST_H

#include <stdbool.h>
#include <X11/Xlib.h>
#include "wm_compat.h"  // For CCWM and Client structs (needed by session save/restore)

// ============================================================================
//  Log levels
// ============================================================================
//
// These control how much output the logging system produces. Each level
// includes everything above it — setting the minimum to LOG_WARN means you
// will see WARN and ERROR messages, but not DEBUG or INFO.
//
// In development you will want LOG_DEBUG to see everything. In production,
// LOG_INFO or LOG_WARN keeps things quiet.

typedef enum {
    LOG_DEBUG = 0,   // Verbose tracing information for development
    LOG_INFO  = 1,   // Normal operational messages ("cursor enabled", etc.)
    LOG_WARN  = 2,   // Something unexpected but recoverable happened
    LOG_ERROR = 3,   // Something broke — action required
} LogLevel;


// ============================================================================
//  Initialization and shutdown
// ============================================================================

// Initialize all robustness subsystems. Call this once during MoonRock startup,
// after the X11 display and screen are available but before the main loop.
//
// Parameters:
//   dpy    — the X11 display connection (from XOpenDisplay)
//   screen — the default screen number (from DefaultScreen)
//
// Returns true if initialization succeeded, false if a critical subsystem
// could not be started (logging will always work regardless).
bool robust_init(Display *dpy, int screen);

// Clean up all robustness resources. Call during shutdown — closes log files,
// frees any internal state. Safe to call even if robust_init() was not called.
void robust_shutdown(void);


// ============================================================================
//  Structured logging
// ============================================================================
//
// Every log message includes:
//   - A timestamp with millisecond precision (HH:MM:SS.mmm)
//   - The log level (DEBUG/INFO/WARN/ERROR)
//   - A module name (which subsystem produced the message)
//   - The message itself
//
// Output goes to stderr by default. If robust_set_log_file() is called, output
// goes to that file instead (colors are still included — pipe through `cat` or
// use a terminal to see them, or strip with `sed`).

// Log a message at the given level. Uses printf-style formatting.
//
// Parameters:
//   level  — severity of the message (LOG_DEBUG through LOG_ERROR)
//   module — short string identifying the source (e.g. "moonrock", "session")
//   fmt    — printf-style format string
//   ...    — arguments matching the format string
void robust_log(LogLevel level, const char *module, const char *fmt, ...);

// Set the minimum log level. Messages below this level are silently discarded.
// Defaults to LOG_INFO.
void robust_set_log_level(LogLevel min_level);

// Redirect all log output to a file instead of stderr.
// Pass NULL to revert to stderr. The file is opened in append mode so
// previous log data is preserved across restarts.
void robust_set_log_file(const char *path);


// ============================================================================
//  Crash recovery (fallback mode)
// ============================================================================
//
// If MoonRock encounters a fatal GL error (shader compilation failure, context
// loss, etc.), calling robust_enter_fallback_mode() gracefully degrades the
// desktop. It un-redirects all windows so the X server paints them directly
// to the screen — no compositing effects, but the user can still work.
//
// The user can restart cc-wm to attempt to restore full compositing.

// Enter fallback mode — disable compositing and let windows render directly.
// This is a one-way trip: once in fallback mode, compositing stays off until
// the window manager is restarted.
//
// Parameters:
//   dpy  — the X11 display connection
//   root — the root window to un-redirect
void robust_enter_fallback_mode(Display *dpy, Window root);

// Check whether we are currently in fallback mode.
// Returns true if compositing has been disabled due to an error.
bool robust_is_fallback(void);


// ============================================================================
//  Hardware cursor
// ============================================================================
//
// By default, X11 draws the cursor in software — as part of the composited
// frame. This means the cursor moves at the compositor's frame rate, which
// can feel laggy during heavy rendering.
//
// XFixes provides a way to keep the cursor as a hardware sprite that the GPU
// overlays on top of everything. This makes cursor movement feel instant and
// smooth regardless of compositor load.

// Enable hardware cursor rendering via XFixes.
// Call once after the display is open. Safe to call even if XFixes is not
// available — it will just log a warning and return.
void robust_setup_hardware_cursor(Display *dpy);


// ============================================================================
//  Session state persistence
// ============================================================================
//
// When the user logs out (or the WM shuts down cleanly), we save each window's
// class name, position, size, and Space assignment to a plain-text file. On
// next login, we read it back and reposition any windows whose class name
// matches.
//
// File format (one window per line):
//   window_class|x|y|w|h|space_index
//
// The default path is ~/.config/cc-wm/session.state

// Save the current session state (all managed window positions and Spaces) to
// the given file path. Creates parent directories if they don't exist.
//
// Parameters:
//   wm   — pointer to the global CCWM state (for reading window info)
//   path — file path to write to (NULL for default ~/.config/cc-wm/session.state)
//
// Returns true on success, false if the file could not be written.
bool robust_save_session(CCWM *wm, const char *path);

// Restore a previously saved session. For each entry in the file, if a window
// with a matching WM_CLASS is currently managed, it is moved to the saved
// position and size. Windows that don't match any saved entry are left alone.
//
// Parameters:
//   wm   — pointer to the global CCWM state (for moving/resizing windows)
//   path — file path to read from (NULL for default)
//
// Returns true if the file was read successfully (even if no windows matched).
bool robust_restore_session(CCWM *wm, const char *path);


// ============================================================================
//  Power management
// ============================================================================
//
// On laptops, we want to reduce compositor effects when running on battery
// to save power. These functions read battery state from Linux's sysfs
// (/sys/class/power_supply/BAT*).
//
// On desktop systems (no battery), robust_on_battery() returns false and
// robust_battery_level() returns 1.0, so effects always run at full quality.

// Returns true if the system is currently running on battery power.
bool robust_on_battery(void);

// Returns the current battery charge level as a float from 0.0 (empty) to
// 1.0 (full). Returns 1.0 if no battery is detected (desktop system).
float robust_battery_level(void);

// Returns true if effects should be reduced to save power.
// The heuristic is: on battery AND charge below 30%.
bool robust_should_reduce_effects(void);


// ============================================================================
//  Accessibility (AT-SPI)
// ============================================================================
//
// AT-SPI (Assistive Technology Service Provider Interface) is the standard
// accessibility framework on Linux. Screen readers like Orca use it to
// discover windows, read their titles, and announce focus changes.
//
// Full AT-SPI integration requires linking against libdbus or sd-bus and
// implementing the org.a11y.atspi interfaces. For now, this is a placeholder
// that logs the intent — enough to validate the architecture without pulling
// in a heavy dependency.

// Register with the AT-SPI accessibility bus via DBus.
// Returns true on success, or true with a warning if AT-SPI is not available
// (we never fail startup because accessibility is missing).
bool robust_init_accessibility(void);


// ============================================================================
//  Memory safety macros
// ============================================================================
//
// These macros add null-pointer and array bounds checks to hot paths in the
// compositor. Instead of segfaulting on a bad pointer, they log an error and
// return a safe default value.
//
// Usage:
//   MR_CHECK_NULL(ptr, return_value)
//     — If ptr is NULL, log the error with file/line and return return_value.
//
//   MR_CHECK_BOUNDS(index, max, return_value)
//     — If index is out of range [0, max), log and return return_value.
//
// The "ret" parameter lets you use these in functions with different return
// types. For void functions, pass nothing:
//   MR_CHECK_NULL(ptr, );       // returns void
//   MR_CHECK_NULL(ptr, false);  // returns false
//   MR_CHECK_NULL(ptr, -1);     // returns -1

// Check that a pointer is not NULL. If it is, log an error with the variable
// name, file, and line number, then return the given value.
#define MR_CHECK_NULL(ptr, ret) \
    do { \
        if (!(ptr)) { \
            robust_log(LOG_ERROR, "moonrock", \
                       "NULL pointer: %s at %s:%d", \
                       #ptr, __FILE__, __LINE__); \
            return ret; \
        } \
    } while (0)

// Check that an index is within bounds [0, max). If it is out of range,
// log an error with the index value and maximum, then return the given value.
#define MR_CHECK_BOUNDS(idx, max, ret) \
    do { \
        if ((idx) < 0 || (idx) >= (max)) { \
            robust_log(LOG_ERROR, "moonrock", \
                       "Bounds check failed: %s=%d, max=%d at %s:%d", \
                       #idx, (idx), (max), __FILE__, __LINE__); \
            return ret; \
        } \
    } while (0)


#endif // MR_ROBUST_H
