// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// ============================================================================
//  MoonRock Robust — production hardening for the MoonRock Compositor
// ============================================================================
//
// This file implements all the "boring but critical" features that turn MoonRock
// from a cool demo into something you can actually use as your daily driver:
//
//   - Structured logging with timestamps, levels, and optional file output
//   - Crash recovery that degrades to unredirected mode on GL failure
//   - Hardware cursor via XFixes so the mouse never lags
//   - Session state persistence (save/restore window positions across reboots)
//   - Power management (detect battery, reduce effects to save power)
//   - AT-SPI accessibility registration (placeholder for screen readers)
//   - Memory safety macros (defined in the header, used throughout MoonRock)
//
// Each subsystem is independent — if one fails to initialize, the others
// still work. The only thing that always succeeds is logging, because we
// need it to report failures in everything else.
//
// ============================================================================

// _GNU_SOURCE is required for some POSIX extensions we use:
//   - clock_gettime() and CLOCK_REALTIME for high-resolution timestamps
//   - Various string functions
#define _GNU_SOURCE

#include "moonrock_robust.h"
#include "wm_compat.h"

#include <stdarg.h>      // va_list, va_start, va_end for variadic logging
#include <stdio.h>       // fprintf, fopen, fclose, fgets, fflush
#include <stdlib.h>      // getenv, free, calloc, malloc
#include <string.h>      // strncmp, strlen, memset, strtok
#include <time.h>        // clock_gettime, localtime, struct timespec/tm
#include <unistd.h>      // access, getenv (HOME directory)
#include <sys/stat.h>    // mkdir for creating config directories
#include <errno.h>       // errno for mkdir error handling

// X11 extension headers for compositing and cursor management
#include <X11/extensions/Xfixes.h>     // XFixesShowCursor, hardware cursor
#include <X11/extensions/Xcomposite.h> // XCompositeUnredirectSubwindows


// ============================================================================
//  Module-level state
// ============================================================================
//
// These static variables hold the state for the logging and crash recovery
// subsystems. They are file-scoped (static) so no other module can
// accidentally modify them.

// The minimum log level — messages below this are silently discarded.
// Defaults to LOG_INFO so debug messages don't clutter normal operation.
static LogLevel current_min_level = LOG_INFO;

// Optional log file. If NULL, output goes to stderr. If set via
// robust_set_log_file(), output goes to this file instead.
static FILE *log_file = NULL;

// Whether we are in fallback mode (compositing disabled due to a fatal error).
// Once set to true, it stays true until the WM is restarted.
static bool fallback_mode = false;

// Whether robust_init() has been called. Prevents double-init and ensures
// robust_shutdown() knows whether there is anything to clean up.
static bool initialized = false;


// ============================================================================
//  Structured logging
// ============================================================================
//
// The logging system is intentionally simple: it writes formatted messages
// to a FILE* (stderr or a log file). No dynamic memory allocation, no
// background threads, no ring buffers. For a window manager, simplicity in
// the logging path is more important than throughput — we log dozens of
// messages per second at most, not thousands.

void robust_log(LogLevel level, const char *module, const char *fmt, ...)
{
    // Discard messages below the current minimum level.
    // For example, if min_level is LOG_WARN, then LOG_DEBUG and LOG_INFO
    // messages are silently dropped.
    if (level < current_min_level) return;

    // Human-readable names for each log level, indexed by the enum value.
    const char *level_str[] = { "DEBUG", "INFO", "WARN", "ERROR" };

    // ANSI color codes for terminal output. Makes it easy to spot warnings
    // (yellow) and errors (red) in a stream of log output.
    //   Cyan   = DEBUG (low priority, lots of detail)
    //   Green  = INFO  (normal operation)
    //   Yellow = WARN  (something unexpected but not fatal)
    //   Red    = ERROR (something broke)
    const char *colors[] = { "\033[36m", "\033[32m", "\033[33m", "\033[31m" };

    // Reset code to return terminal to its default color after the message.
    const char *reset = "\033[0m";

    // Output destination: the log file if one was set, otherwise stderr.
    FILE *out = log_file ? log_file : stderr;

    // Get the current wall-clock time with millisecond precision.
    // CLOCK_REALTIME gives us the actual time of day (not monotonic uptime),
    // which is more useful in log files for correlating with other events.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    // Convert the seconds portion to a broken-down local time struct so we
    // can extract hours, minutes, and seconds for the timestamp.
    struct tm *tm_info = localtime(&ts.tv_sec);

    // Safety check: localtime can return NULL if the time value is invalid
    // (extremely rare, but we check everything in this module).
    if (!tm_info) {
        // If we can't get the time, print without a timestamp rather than
        // crashing. This should basically never happen.
        fprintf(out, "[???] [%s] [%s] ", level_str[level], module);
    } else {
        // Print the colored prefix: timestamp, level, and module name.
        // The milliseconds come from the nanoseconds field divided by 1M.
        fprintf(out, "%s[%02d:%02d:%02d.%03ld] [%-5s] [%s]%s ",
                colors[level],
                tm_info->tm_hour,
                tm_info->tm_min,
                tm_info->tm_sec,
                ts.tv_nsec / 1000000,  // Convert nanoseconds to milliseconds
                level_str[level],
                module,
                reset);
    }

    // Print the actual message using the caller's format string and arguments.
    // va_list is the standard C mechanism for handling variable-argument
    // functions (like printf). va_start initializes it, vfprintf uses it,
    // and va_end cleans it up.
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    // End the line and flush immediately. Flushing is important because:
    //   1. If the WM crashes, we want all log messages written to disk.
    //   2. When watching logs in real-time (tail -f), buffered output would
    //      appear delayed.
    fprintf(out, "\n");
    fflush(out);
}

void robust_set_log_level(LogLevel min_level)
{
    // Clamp to valid range so a bad value doesn't cause array overruns
    // when we index into level_str[] or colors[].
    if (min_level < LOG_DEBUG) min_level = LOG_DEBUG;
    if (min_level > LOG_ERROR) min_level = LOG_ERROR;

    current_min_level = min_level;
    robust_log(LOG_DEBUG, "robust", "Log level set to %d", min_level);
}

void robust_set_log_file(const char *path)
{
    // Close any previously opened log file to avoid leaking file descriptors.
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }

    // If NULL was passed, revert to stderr output.
    if (!path) {
        robust_log(LOG_INFO, "robust", "Log output reverted to stderr");
        return;
    }

    // Open in append mode ("a") so we don't destroy previous log data.
    // This is important for debugging crashes — the log from the previous
    // session is preserved.
    log_file = fopen(path, "a");
    if (!log_file) {
        // If we can't open the file, fall back to stderr and warn.
        // We do NOT treat this as fatal — logging to stderr is fine.
        fprintf(stderr, "[WARN] [robust] Could not open log file: %s\n", path);
    } else {
        robust_log(LOG_INFO, "robust", "Logging to file: %s", path);
    }
}


// ============================================================================
//  Crash recovery (fallback mode)
// ============================================================================
//
// When MoonRock detects a fatal GL error, it calls robust_enter_fallback_mode()
// to gracefully degrade. This does three things:
//
//   1. Calls XCompositeUnredirectSubwindows() to tell the X server to paint
//      windows directly to the screen again (instead of to off-screen pixmaps
//      that MoonRock was compositing).
//
//   2. Flushes the display to make the change take effect immediately.
//
//   3. Sets a flag so the rest of MoonRock knows to skip all rendering.
//
// The result: the user sees their windows without shadows or animations, but
// everything is still usable. Much better than a black screen or a crash.

void robust_enter_fallback_mode(Display *dpy, Window root)
{
    // Guard: don't do this twice. XCompositeUnredirectSubwindows is not
    // idempotent and calling it when windows aren't redirected can cause
    // X errors.
    if (fallback_mode) {
        robust_log(LOG_WARN, "robust", "Already in fallback mode, ignoring");
        return;
    }

    robust_log(LOG_ERROR, "moonrock",
               "FATAL: Entering fallback mode — compositing disabled");

    // Check that we have a valid display and root window before touching X11.
    if (dpy && root != None) {
        // Tell the X server to stop redirecting windows to off-screen pixmaps.
        // CompositeRedirectManual was what we used when setting up compositing
        // in moonrock.c — this reverses it. Windows now paint directly to the
        // screen, bypassing our compositor entirely.
        XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);

        // Flush to make the un-redirect take effect immediately.
        // Without this, the change would be queued in the X11 request buffer
        // and might not apply until the next event cycle — which might never
        // happen if we're in a broken state.
        XSync(dpy, False);
    }

    // Set the flag so all MoonRock rendering code knows to skip compositing.
    fallback_mode = true;

    robust_log(LOG_WARN, "moonrock",
               "Fallback mode active. Windows render directly via X server.");
    robust_log(LOG_WARN, "moonrock",
               "Restart aura-wm to attempt to restore compositing.");
}

bool robust_is_fallback(void)
{
    return fallback_mode;
}


// ============================================================================
//  Hardware cursor
// ============================================================================
//
// X11 compositors have a known issue: the mouse cursor can feel laggy because
// it is redrawn as part of the composited frame. Hardware cursors are drawn
// by the GPU's display controller as an overlay, completely bypassing the
// compositing pipeline. This means the cursor moves at the display's native
// refresh rate regardless of how busy the compositor is.
//
// XFixes is an X11 extension that provides cursor-related utilities. We use
// XFixesShowCursor() to ensure the hardware cursor is visible. Some
// compositors hide it and draw a software cursor instead — we want the
// opposite.

void robust_setup_hardware_cursor(Display *dpy)
{
    MR_CHECK_NULL(dpy, );

    // Check if the XFixes extension is available on this X server.
    // Not all X servers support it (though in practice, every modern one does).
    int fixes_event_base, fixes_error_base;
    if (!XFixesQueryExtension(dpy, &fixes_event_base, &fixes_error_base)) {
        robust_log(LOG_WARN, "robust",
                   "XFixes extension not available — cannot set up hardware cursor");
        return;
    }

    // Query the XFixes version to make sure we have a recent enough version.
    // Version 4.0+ supports the cursor visibility functions we need.
    int major = 0, minor = 0;
    XFixesQueryVersion(dpy, &major, &minor);
    robust_log(LOG_DEBUG, "robust", "XFixes version %d.%d", major, minor);

    if (major < 4) {
        robust_log(LOG_WARN, "robust",
                   "XFixes version %d.%d too old (need 4.0+) — "
                   "hardware cursor may not work", major, minor);
        return;
    }

    // Make sure the hardware cursor is visible. Some compositors (like picom)
    // hide the hardware cursor and draw their own software cursor so they can
    // apply effects to it. We do the opposite — we want the hardware cursor
    // because it is always smooth.
    XFixesShowCursor(dpy, DefaultRootWindow(dpy));

    // Flush to ensure the cursor change takes effect immediately.
    XFlush(dpy);

    robust_log(LOG_INFO, "robust",
               "Hardware cursor enabled via XFixes %d.%d", major, minor);
}


// ============================================================================
//  Helper: ensure a directory exists (create it if it doesn't)
// ============================================================================
//
// Used by the session save/restore code to create ~/.config/aura-wm/ if it
// doesn't already exist. Works like "mkdir -p" but only one level deep.

static bool ensure_directory(const char *dir_path)
{
    MR_CHECK_NULL(dir_path, false);

    // Try to create the directory. If it already exists, mkdir returns -1
    // and errno is set to EEXIST — that's fine, not an error for us.
    // 0755 = owner can read/write/execute, group and others can read/execute.
    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
        robust_log(LOG_ERROR, "session",
                   "Failed to create directory: %s (errno %d)", dir_path, errno);
        return false;
    }

    return true;
}


// ============================================================================
//  Helper: build the default session file path
// ============================================================================
//
// Returns a pointer to a static buffer containing the path
// ~/.config/aura-wm/session.state. The buffer is reused on each call.

static const char *default_session_path(void)
{
    static char path_buf[512];

    // Get the user's home directory from the HOME environment variable.
    // This is set by the login process and should always be available.
    const char *home = getenv("HOME");
    if (!home) {
        robust_log(LOG_ERROR, "session", "HOME environment variable not set");
        return NULL;
    }

    // Build the full path: ~/.config/aura-wm/session.state
    snprintf(path_buf, sizeof(path_buf),
             "%s/.config/aura-wm/session.state", home);

    return path_buf;
}


// ============================================================================
//  Session state persistence — save
// ============================================================================
//
// Iterates through all managed windows and writes their class name, position,
// size, and Space index to a plain-text file. The format is simple enough to
// edit by hand if needed:
//
//   window_class|x|y|w|h|space_index
//
// We identify windows by WM_CLASS (the application name), not by X11 window
// ID, because window IDs change every time the app is started. WM_CLASS is
// stable across sessions — "kate" is always "kate".

bool robust_save_session(AuraWM *wm, const char *path)
{
    MR_CHECK_NULL(wm, false);

    // Use the default path if none was provided.
    if (!path) path = default_session_path();
    MR_CHECK_NULL(path, false);

    // Make sure the parent directory (~/.config/aura-wm/) exists.
    // We need to extract the directory portion of the path.
    char dir_buf[512];
    snprintf(dir_buf, sizeof(dir_buf), "%s", path);

    // Walk backwards to find the last '/' and truncate there to get the dir.
    char *last_slash = strrchr(dir_buf, '/');
    if (last_slash) {
        *last_slash = '\0';

        // Also ensure the parent (~/.config) exists.
        char *parent_slash = strrchr(dir_buf, '/');
        if (parent_slash) {
            *parent_slash = '\0';
            ensure_directory(dir_buf);        // ~/.config
            *parent_slash = '/';
        }
        ensure_directory(dir_buf);            // ~/.config/aura-wm
    }

    // Open the session file for writing. This overwrites any previous content
    // because we are saving the current state, not appending to it.
    FILE *f = fopen(path, "w");
    if (!f) {
        robust_log(LOG_ERROR, "session",
                   "Failed to open session file for writing: %s", path);
        return false;
    }

    // Write a header comment so the file is self-documenting.
    fprintf(f, "# AuraOS session state — saved window positions\n");
    fprintf(f, "# Format: wm_class|x|y|w|h|space_index\n");
    fprintf(f, "# Auto-generated by MoonRock Compositor. Safe to edit by hand.\n");

    int saved_count = 0;

    // Iterate through every managed client in the window manager.
    for (int i = 0; i < wm->num_clients; i++) {
        Client *c = &wm->clients[i];

        // Skip clients that are not currently mapped (visible). There is no
        // point saving the position of a minimized or withdrawn window.
        if (!c->mapped) continue;

        // Skip clients with no WM_CLASS. Without a class name, we have no
        // way to match this entry to a window on the next login.
        if (c->wm_class_name[0] == '\0') continue;

        // Write one line per window. We use the pipe character as a delimiter
        // because it is unlikely to appear in a WM_CLASS name.
        // The space index is 0 for now — when Mission Control integration is
        // fully wired up, we will look up which Space this window belongs to.
        //
        // Note: We save the client's (x, y) which is the frame position,
        // and (w, h) which is the client content size.
        int space_index = 0;  // Default to first space
        fprintf(f, "%s|%d|%d|%d|%d|%d\n",
                c->wm_class_name,
                c->x, c->y,
                c->w, c->h,
                space_index);
        saved_count++;
    }

    fclose(f);

    robust_log(LOG_INFO, "session",
               "Saved %d window positions to %s", saved_count, path);
    return true;
}


// ============================================================================
//  Session state persistence — restore
// ============================================================================
//
// Reads the session file and, for each entry, searches for a currently managed
// window with the same WM_CLASS. If found, the window is moved and resized to
// its saved position. Windows without a matching entry are left alone.
//
// This function should be called after all initial windows have been mapped
// and managed (i.e., after the WM has scanned existing windows at startup).

bool robust_restore_session(AuraWM *wm, const char *path)
{
    MR_CHECK_NULL(wm, false);

    // Use the default path if none was provided.
    if (!path) path = default_session_path();
    MR_CHECK_NULL(path, false);

    // Check if the file exists before trying to open it. On a fresh install
    // there won't be a session file, and that's perfectly normal.
    if (access(path, R_OK) != 0) {
        robust_log(LOG_INFO, "session",
                   "No session file found at %s — starting fresh", path);
        return true;  // Not an error, just no saved state
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        robust_log(LOG_ERROR, "session",
                   "Failed to open session file for reading: %s", path);
        return false;
    }

    int restored_count = 0;
    char line[512];

    // Read the file line by line.
    while (fgets(line, sizeof(line), f)) {
        // Skip comment lines (starting with #) and blank lines.
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;

        // Strip the trailing newline if present.
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        // Parse the line. Format: wm_class|x|y|w|h|space_index
        // We use sscanf with a character set that stops at '|' for the class.
        char saved_class[128];
        int sx, sy, sw, sh, space_idx;

        // %127[^|] reads up to 127 chars, stopping at the pipe delimiter.
        // This prevents buffer overflow on the class name.
        if (sscanf(line, "%127[^|]|%d|%d|%d|%d|%d",
                   saved_class, &sx, &sy, &sw, &sh, &space_idx) != 6) {
            robust_log(LOG_WARN, "session",
                       "Malformed session line (skipping): %s", line);
            continue;
        }

        // Sanity check: reject obviously bogus values. Window dimensions
        // should be positive and positions should be reasonable.
        if (sw <= 0 || sh <= 0 || sw > 10000 || sh > 10000) {
            robust_log(LOG_WARN, "session",
                       "Invalid dimensions for %s: %dx%d (skipping)",
                       saved_class, sw, sh);
            continue;
        }

        // Search for a managed window whose WM_CLASS matches the saved entry.
        for (int i = 0; i < wm->num_clients; i++) {
            Client *c = &wm->clients[i];

            // Only consider mapped windows with a matching class name.
            if (!c->mapped) continue;
            if (strcmp(c->wm_class_name, saved_class) != 0) continue;

            // Move and resize the window to its saved position.
            // We update both the frame position and request the X server to
            // move the frame window.
            c->x = sx;
            c->y = sy;
            c->w = sw;
            c->h = sh;

            // Move and resize the frame window. The frame includes the title
            // bar and borders, so its dimensions include TITLEBAR_HEIGHT and
            // BORDER_WIDTH offsets (defined in wm.h).
            XMoveResizeWindow(wm->dpy, c->frame,
                              c->x, c->y,
                              c->w + 2 * BORDER_WIDTH,
                              c->h + TITLEBAR_HEIGHT + BORDER_WIDTH);

            // Also resize the client window inside the frame to match.
            XResizeWindow(wm->dpy, c->client, c->w, c->h);

            robust_log(LOG_DEBUG, "session",
                       "Restored %s to (%d, %d) %dx%d",
                       saved_class, sx, sy, sw, sh);

            restored_count++;
            break;  // Only restore the first matching window per entry
        }
    }

    fclose(f);

    robust_log(LOG_INFO, "session",
               "Restored %d window positions from %s", restored_count, path);
    return true;
}


// ============================================================================
//  Power management
// ============================================================================
//
// Linux exposes battery information through sysfs — a virtual filesystem
// mounted at /sys. Each power supply (battery, AC adapter) appears as a
// directory under /sys/class/power_supply/.
//
// For a battery, the key files are:
//   status   — "Charging", "Discharging", "Full", or "Not charging"
//   capacity — integer 0-100 representing the charge percentage
//
// We check BAT0 first (most common), then BAT1 (some laptops have two
// batteries). On desktops, neither file exists, so we return safe defaults.

bool robust_on_battery(void)
{
    // Try to read the battery status. BAT0 is the most common name for the
    // primary battery on Linux laptops.
    FILE *f = fopen("/sys/class/power_supply/BAT0/status", "r");

    // If BAT0 doesn't exist, try BAT1 (some systems number differently).
    if (!f) f = fopen("/sys/class/power_supply/BAT1/status", "r");

    // If neither exists, this is probably a desktop — not on battery.
    if (!f) return false;

    // Read the status string. It will be one of:
    //   "Charging"      — plugged in and charging
    //   "Discharging"   — running on battery
    //   "Full"          — plugged in and fully charged
    //   "Not charging"  — plugged in but not charging (battery full or issue)
    char status[32];
    if (!fgets(status, sizeof(status), f)) {
        fclose(f);
        return false;
    }
    fclose(f);

    // "Discharging" means the system is running on battery power.
    // We compare only the first 11 characters in case there is a trailing
    // newline or whitespace in the sysfs file.
    return strncmp(status, "Discharging", 11) == 0;
}

float robust_battery_level(void)
{
    // Read the battery capacity (charge percentage, 0-100).
    FILE *f = fopen("/sys/class/power_supply/BAT0/capacity", "r");
    if (!f) f = fopen("/sys/class/power_supply/BAT1/capacity", "r");

    // No battery found — report 100% (full) so effects are never reduced
    // on a desktop system.
    if (!f) return 1.0f;

    int capacity = 100;
    if (fscanf(f, "%d", &capacity) != 1) {
        // If we can't parse the capacity, assume full to be safe.
        fclose(f);
        return 1.0f;
    }
    fclose(f);

    // Clamp to [0, 100] in case sysfs returns something unexpected.
    if (capacity < 0)   capacity = 0;
    if (capacity > 100)  capacity = 100;

    // Convert from 0-100 integer to 0.0-1.0 float.
    return (float)capacity / 100.0f;
}

bool robust_should_reduce_effects(void)
{
    // The heuristic is simple: reduce effects when running on battery AND
    // the charge is below 30%. This avoids being overly aggressive — if
    // the battery is at 80% and the user unplugged briefly, we don't want
    // to strip away all the visual polish.
    //
    // Also reduce effects if we are in fallback mode (compositing already
    // broken), since there are no effects to render anyway.
    if (fallback_mode) return true;

    return robust_on_battery() && robust_battery_level() < 0.30f;
}


// ============================================================================
//  Accessibility (AT-SPI)
// ============================================================================
//
// AT-SPI (Assistive Technology Service Provider Interface) is the Linux
// accessibility standard. Screen readers like Orca use it to:
//
//   1. Discover all windows on the screen.
//   2. Read window titles and content.
//   3. Get notified when focus changes.
//   4. Navigate widgets and controls.
//
// Full AT-SPI support requires:
//   - Linking against libdbus-1 or sd-bus
//   - Implementing the org.a11y.atspi.Application interface
//   - Publishing our windows on the accessibility bus
//
// This is a significant amount of code, so for now we provide a placeholder
// that logs the intent. The architecture is in place — the actual DBus calls
// will be added when we integrate libdbus as a dependency.

bool robust_init_accessibility(void)
{
    // Check if the AT-SPI bus is running by looking for its environment variable.
    // AT-SPI sets AT_SPI_BUS_ADDRESS when the accessibility bus is active.
    const char *at_spi_bus = getenv("AT_SPI_BUS_ADDRESS");

    if (at_spi_bus) {
        robust_log(LOG_INFO, "a11y",
                   "AT-SPI bus detected: %s", at_spi_bus);
        robust_log(LOG_INFO, "a11y",
                   "Accessibility: AT-SPI registration placeholder active");
        robust_log(LOG_INFO, "a11y",
                   "Full implementation requires libdbus or sd-bus integration");
    } else {
        // No AT-SPI bus — this is normal on systems without accessibility
        // software installed. Not an error.
        robust_log(LOG_DEBUG, "a11y",
                   "AT-SPI bus not detected (AT_SPI_BUS_ADDRESS not set)");
        robust_log(LOG_DEBUG, "a11y",
                   "Screen reader support not available in this session");
    }

    // Always return true — we never fail startup because accessibility
    // is not available. It would be wrong to prevent someone from using
    // their computer because the accessibility bus isn't running.
    return true;
}


// ============================================================================
//  Initialization
// ============================================================================
//
// robust_init() sets up all the subsystems in order. Each step logs its
// status so the user (or developer) can see exactly what was initialized.
// If any optional subsystem fails, we continue — the WM still works, just
// without that feature.

bool robust_init(Display *dpy, int screen)
{
    if (initialized) {
        robust_log(LOG_WARN, "robust", "Already initialized — ignoring");
        return true;
    }

    // Logging is always available (it was set up statically), so we can
    // log right from the start.
    robust_log(LOG_INFO, "robust",
               "Initializing MoonRock robustness layer (screen %d)", screen);

    // --- Hardware cursor ---
    // Set up the hardware cursor early so it is smooth from the very first
    // frame of compositing.
    if (dpy) {
        robust_setup_hardware_cursor(dpy);
    }

    // --- Accessibility ---
    // Try to connect to the AT-SPI bus. This is a no-op placeholder for now
    // but the call establishes the pattern for the future implementation.
    robust_init_accessibility();

    // --- Power management ---
    // Log the current power state so it appears in the startup output.
    if (robust_on_battery()) {
        float level = robust_battery_level();
        robust_log(LOG_INFO, "robust",
                   "Running on battery (%.0f%% charge)", level * 100.0f);
        if (robust_should_reduce_effects()) {
            robust_log(LOG_INFO, "robust",
                       "Battery low — effects will be reduced");
        }
    } else {
        robust_log(LOG_DEBUG, "robust",
                   "Running on AC power (or no battery detected)");
    }

    // --- Session restore ---
    // Check if a saved session exists. We don't restore it here because
    // windows may not be managed yet — the caller should call
    // robust_restore_session() after all initial windows are set up.
    const char *session_path = default_session_path();
    if (session_path && access(session_path, R_OK) == 0) {
        robust_log(LOG_INFO, "robust",
                   "Saved session found at %s (will restore after window scan)",
                   session_path);
    }

    // Reset fallback mode on fresh startup. If the previous run ended in
    // fallback mode, the user restarted the WM to try again.
    fallback_mode = false;

    initialized = true;
    robust_log(LOG_INFO, "robust", "Robustness layer initialized");

    return true;
}


// ============================================================================
//  Shutdown
// ============================================================================
//
// Clean up resources. Called when the window manager exits. The main thing
// we need to do is close the log file (if one was opened) so all buffered
// data is flushed to disk.

void robust_shutdown(void)
{
    if (!initialized) return;

    robust_log(LOG_INFO, "robust", "Shutting down robustness layer");

    // Close the log file if one was opened. This flushes any remaining
    // buffered data to disk.
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }

    initialized = false;
}
