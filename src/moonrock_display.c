// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/
//
// ============================================================================
//  MoonRock Display Management — Implementation
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

// POSIX extensions require _GNU_SOURCE (provided by meson via -D_GNU_SOURCE).

#include "moonrock_display.h"
#include "wm_compat.h"

// Public contract for the MoonRock -> shell scale bridge. We only need the
// atom name from it here — the parser and subscribe helpers are client-side.
#include "moonrock_scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>

// XRandR — the X11 extension for querying and configuring display outputs.
// It tells us what monitors are connected, their resolutions, refresh rates,
// and properties like VRR capability.
#include <X11/extensions/Xrandr.h>

// XComposite — the X11 extension that controls off-screen redirection.
// Normally, MoonRock redirects all windows to off-screen pixmaps so we can
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
static MROutput *outputs = NULL;

// How many outputs are currently in the array.
static int output_count = 0;

// Maximum number of outputs we've allocated space for. This lets us reuse
// the allocation when re-enumerating instead of malloc/free every time.
#define MAX_OUTPUTS 16

// The current gaming mode — see the GameMode enum in the header.
// OFF = normal compositing. BYPASS = direct scanout on the primary output
// for a single fullscreen client (forward-looking for multi-monitor direct
// scanout). No in-session gamescope path — that's handled by the dedicated
// gaming SDDM session now.
static GameMode current_game_mode = GAME_MODE_OFF;

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
//  Per-output scale: EDID hashing, PPI bands, user-override persistence
// ============================================================================
//
// The CopyCatOS HiDPI mandate (CLAUDE.md → HiDPI & Display Scaling) requires
// per-output scale factors, fractional from day one (1.0, 1.25, 1.5, 1.75,
// 2.0, …), keyed by EDID hash so the same monitor gets the same scale across
// reboots, dock/undock cycles, and cable swaps. This block owns all of that.
//
// Data model:
//
//   outputs[i].edid_hash    — 64-bit FNV-1a hash of the raw EDID blob.
//   outputs[i].mm_width/_height — physical panel size from EDID.
//   outputs[i].default_scale — picked from PPI bands the first time we see
//                               a given monitor. Never written to disk.
//   outputs[i].user_scale    — user's persisted override, or 0.0 if none.
//                               Loaded from disk at init, written from
//                               display_set_scale_for_output().
//   outputs[i].scale         — the effective scale right now
//                               (= user_scale ? user_scale : default_scale).
//
// Persistence file: ~/.local/share/moonrock/display-scales.conf
//
//   # MoonRock per-display scale overrides (EDID hash -> scale)
//   a1b2c3d4e5f60718=1.5
//   deadbeefcafebabe=2.0
//
// Simple text so it's trivial to inspect, hand-edit, or rewrite from the
// future systemcontrol Displays pane. Lines starting with '#' are comments.


// In-memory override table, loaded once at init, rewritten on every set.
// Capped well above what any real machine would plug in over its lifetime
// — we're just guarding against an unbounded grow if a user cycles through
// a lot of monitors.
#define MAX_SCALE_OVERRIDES 64

typedef struct {
    uint8_t edid_hash[8];   // EDID hash this override belongs to
    float   scale;          // user-chosen scale (1.0, 1.25, 1.5, …)
} ScaleOverride;

static ScaleOverride scale_overrides[MAX_SCALE_OVERRIDES];
static int scale_override_count = 0;
static bool scale_overrides_loaded = false;


// FNV-1a 64-bit hash. No dependencies, well-behaved avalanche on
// short inputs (EDIDs are 128+ bytes), and the 64-bit digest is more than
// enough to distinguish every display a user will ever own.
static void fnv1a_64(const uint8_t *data, size_t len, uint8_t out[8])
{
    uint64_t h = 0xcbf29ce484222325ULL;     // FNV-1a 64-bit offset basis
    const uint64_t prime = 0x100000001b3ULL; // FNV-1a 64-bit prime
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= prime;
    }
    // Emit big-endian so the hex string reads left-to-right in the order
    // the bytes are printed. No semantic difference — just keeps the
    // persistence file easier to diff.
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(h & 0xffu);
        h >>= 8;
    }
}

// Hex-encode an EDID hash into a 17-byte string (16 hex chars + '\0').
static void hash_to_hex(const uint8_t h[8], char out[17])
{
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[i * 2 + 0] = hex[(h[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[h[i] & 0xf];
    }
    out[16] = '\0';
}

// Parse a 16-char hex string into 8 bytes. Returns true on success.
static bool hex_to_hash(const char *s, uint8_t out[8])
{
    for (int i = 0; i < 8; i++) {
        unsigned hi = 0, lo = 0;
        char c = s[i * 2 + 0];
        if (c >= '0' && c <= '9') hi = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') hi = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') hi = (unsigned)(c - 'A' + 10);
        else return false;
        c = s[i * 2 + 1];
        if (c >= '0' && c <= '9') lo = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') lo = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') lo = (unsigned)(c - 'A' + 10);
        else return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}


// Derive the default scale factor for a display from its PPI (pixels per
// inch). Bands from CLAUDE.md → HiDPI & Display Scaling:
//
//     ≲160  → 1.00   (desktop monitors, old external displays)
//   160-210 → 1.25
//   210-260 → 1.50   (Legion Go S 7" 1920x1200 ≈ 323 PPI falls higher)
//   260-320 → 1.75
//     ≥320  → 2.00
//
// If we can't determine the PPI (EDID missing physical dimensions, which
// happens on some projectors and virtual monitors), fall back to 1.0 —
// the safest default, better to under-scale than ship a tiny UI.
static float default_scale_from_ppi(int px_w, int px_h,
                                    int mm_w, int mm_h)
{
    if (mm_w <= 0 || mm_h <= 0 || px_w <= 0 || px_h <= 0) return 1.0f;
    // Diagonal PPI so ultra-wide 21:9 panels don't misreport vs. 16:9.
    double inches_w = (double)mm_w / 25.4;
    double inches_h = (double)mm_h / 25.4;
    double diag_px = sqrt((double)px_w * px_w + (double)px_h * px_h);
    double diag_in = sqrt(inches_w * inches_w + inches_h * inches_h);
    if (diag_in <= 0.0) return 1.0f;
    double ppi = diag_px / diag_in;

    if (ppi < 160.0) return 1.00f;
    if (ppi < 210.0) return 1.25f;
    if (ppi < 260.0) return 1.50f;
    if (ppi < 320.0) return 1.75f;
    return 2.00f;
}


// Resolve the override file path into a caller-owned buffer. Honors
// XDG_DATA_HOME, falling back to ~/.local/share as the spec requires.
// Returns true on success.
static bool scale_override_path(char *out, size_t out_len)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        int n = snprintf(out, out_len,
                         "%s/moonrock/display-scales.conf", xdg);
        return n > 0 && (size_t)n < out_len;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        if (!pw || !pw->pw_dir) return false;
        home = pw->pw_dir;
    }
    int n = snprintf(out, out_len,
                     "%s/.local/share/moonrock/display-scales.conf", home);
    return n > 0 && (size_t)n < out_len;
}

// Best-effort mkdir -p of the directory portion of `path`. Silent on errors
// — the caller's subsequent open will fail loudly if directory creation
// didn't work out.
static void ensure_parent_dir(const char *path)
{
    char buf[512];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return;
    memcpy(buf, path, n + 1);
    // Walk forwards, creating each intermediate component.
    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            (void)mkdir(buf, 0755);
            buf[i] = '/';
        }
    }
    // Create the final path component (the file's parent directory itself).
    for (ssize_t i = (ssize_t)n - 1; i >= 0; i--) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            (void)mkdir(buf, 0755);
            break;
        }
    }
}

// Load the persisted overrides from disk into scale_overrides[]. Called
// exactly once per process — subsequent display_init()s (e.g. from hotplug)
// skip the disk read since the in-memory table is the source of truth
// after startup.
static void load_scale_overrides_once(void)
{
    if (scale_overrides_loaded) return;
    scale_overrides_loaded = true;
    scale_override_count = 0;

    char path[512];
    if (!scale_override_path(path, sizeof(path))) return;

    FILE *fp = fopen(path, "r");
    if (!fp) return; // First run or permission denied — treat as empty.

    char line[128];
    while (fgets(line, sizeof(line), fp)
        && scale_override_count < MAX_SCALE_OVERRIDES) {
        // Strip leading whitespace + comment + empty lines.
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        // Expected format: 16 hex chars, '=', float, newline.
        if (strlen(p) < 18 || p[16] != '=') continue;

        uint8_t hash[8];
        if (!hex_to_hash(p, hash)) continue;

        float val = strtof(p + 17, NULL);
        if (val <= 0.0f || val > 4.0f) continue; // Reject nonsense.

        ScaleOverride *o = &scale_overrides[scale_override_count++];
        memcpy(o->edid_hash, hash, 8);
        o->scale = val;
    }
    fclose(fp);
    fprintf(stderr,
            "[display] Loaded %d scale override(s) from %s\n",
            scale_override_count, path);
}

// Find a persisted override for the given EDID hash. Returns scale or 0.0
// if no override is set.
static float find_override_scale(const uint8_t edid_hash[8])
{
    for (int i = 0; i < scale_override_count; i++) {
        if (memcmp(scale_overrides[i].edid_hash, edid_hash, 8) == 0) {
            return scale_overrides[i].scale;
        }
    }
    return 0.0f;
}

// Write the override table back to disk. Atomic: write to .tmp + rename.
// Returns true on success, false if either the write or the rename fails.
static bool save_scale_overrides(void)
{
    char path[512];
    if (!scale_override_path(path, sizeof(path))) return false;
    ensure_parent_dir(path);

    char tmp[576];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return false;

    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        fprintf(stderr, "[display] save scale overrides: fopen(%s): %s\n",
                tmp, strerror(errno));
        return false;
    }

    fprintf(fp,
            "# MoonRock per-display scale overrides (EDID hash -> scale)\n"
            "# Written by MoonRock. Hand-edit at your own risk.\n");
    for (int i = 0; i < scale_override_count; i++) {
        char hex[17];
        hash_to_hex(scale_overrides[i].edid_hash, hex);
        fprintf(fp, "%s=%.3f\n", hex, (double)scale_overrides[i].scale);
    }
    if (fflush(fp) != 0 || fclose(fp) != 0) {
        fprintf(stderr, "[display] save scale overrides: write failed\n");
        unlink(tmp);
        return false;
    }

    if (rename(tmp, path) != 0) {
        fprintf(stderr, "[display] save scale overrides: rename(%s,%s): %s\n",
                tmp, path, strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
}

// Read the raw EDID blob for an output into a caller-allocated buffer.
// Returns the number of bytes actually read (0 on failure). EDID blocks
// are 128 bytes; monitors with CEA-861 extensions add blocks of 128 bytes
// each. We pull up to 512 bytes to cover any sane extension count.
static size_t read_edid_blob(Display *dpy, RROutput output,
                             uint8_t *buf, size_t buf_len)
{
    Atom edid_atom = XInternAtom(dpy, "EDID", True);
    if (edid_atom == None) return 0;

    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;

    int status = XRRGetOutputProperty(
        dpy, output, edid_atom,
        0, 128,                 // 128 x 32-bit words covers typical EDIDs
        False, False, AnyPropertyType,
        &actual_type, &actual_format,
        &nitems, &bytes_after, &data);

    if (status != Success || !data || nitems < 128) {
        if (data) XFree(data);
        return 0;
    }

    size_t take = (size_t)nitems;
    if (take > buf_len) take = buf_len;
    memcpy(buf, data, take);
    XFree(data);
    return take;
}


// ============================================================================
//  Publish scales to the root window
// ============================================================================
//
// Shell components (menubar, dock, desktop, systemcontrol) are standalone
// X11 processes. MoonRock is the single source of truth for per-output
// scale, but those components can't link against MoonRock's C API. The
// bridge is a line-oriented UTF-8 property on the root window, written
// under the atom _MOONROCK_OUTPUT_SCALES. Every connected output emits
// one newline-terminated line:
//
//     <name> <x> <y> <width> <height> <scale>
//
// Xlib converts a root-property write into a PropertyNotify event on every
// client that has PropertyChangeMask selected on the root window, so
// subscribers get live updates automatically — no new socket, no new
// daemon. See moonrock/shell-api/moonrock_scale.h for the reader side.
//
// Callers: end of display_init() (covers startup and hotplug, since
// display_handle_hotplug() re-enters display_init) and end of
// display_set_scale_for_output() (covers SysPrefs changing user scale).
static void publish_scales_to_root(void)
{
    if (!display_dpy || display_root == None) return;

    // Assemble the payload. MOONROCK_SCALE_MAX_OUTPUTS * ~80 bytes per
    // line is still comfortably under a few KB — a 4096-byte stack buffer
    // matches what the client side requests in one XGetWindowProperty().
    char buf[4096];
    size_t pos = 0;

    for (int i = 0; i < output_count && pos < sizeof(buf); i++) {
        int n = snprintf(buf + pos, sizeof(buf) - pos,
                         "%s %d %d %d %d %.3f\n",
                         outputs[i].name,
                         outputs[i].x, outputs[i].y,
                         outputs[i].width, outputs[i].height,
                         (double)outputs[i].scale);
        if (n < 0) break;
        if ((size_t)n >= sizeof(buf) - pos) {
            // Line didn't fit — stop here rather than emit a truncated
            // line the parser might mis-read.
            break;
        }
        pos += (size_t)n;
    }

    Atom prop = XInternAtom(display_dpy, MOONROCK_SCALE_ATOM_NAME, False);
    Atom utf8 = XInternAtom(display_dpy, "UTF8_STRING", False);

    XChangeProperty(display_dpy, display_root, prop, utf8, 8,
                    PropModeReplace, (unsigned char *)buf, (int)pos);
    XFlush(display_dpy);

    fprintf(stderr, "[display] Published %d scale entr%s to %s (%zu bytes)\n",
            output_count, output_count == 1 ? "y" : "ies",
            MOONROCK_SCALE_ATOM_NAME, pos);
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
        outputs = calloc(MAX_OUTPUTS, sizeof(MROutput));
        if (!outputs) {
            fprintf(stderr, "[display] ERROR: Failed to allocate output array\n");
            return false;
        }
    }

    // Load persisted per-display scale overrides exactly once. Hotplugs
    // re-enter display_init() but don't re-read the file — the in-memory
    // table stays authoritative so a set from SysPrefs is never clobbered
    // by a concurrent hotplug re-enumeration.
    load_scale_overrides_once();

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

        // Build the MROutput entry for this monitor.
        MROutput *out = &outputs[output_count];
        memset(out, 0, sizeof(MROutput));

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

        // ── Physical size (from EDID via XRROutputInfo) ──
        //
        // The X server parses EDID bytes 21–22 (max image size in cm) and
        // exposes them as mm_width / mm_height on the output info struct.
        // Zero here means EDID didn't advertise physical dimensions, which
        // happens on projectors, KVMs, and some virtual monitors — we'll
        // fall back to 1.0× scale in default_scale_from_ppi() below.
        out->mm_width  = (int)oi->mm_width;
        out->mm_height = (int)oi->mm_height;

        // ── EDID hash for per-monitor persistence ──
        //
        // Read the raw EDID blob and compute a 64-bit FNV-1a hash of it.
        // The hash is the key in our display-scales.conf persistence file:
        // same monitor plugged into any port on any day → same scale.
        // If we can't read EDID (shouldn't happen on real hardware, but
        // does on headless CI and some nested X servers), hash stays zero
        // and the output gets the default scale with no persistence.
        uint8_t edid[512];
        size_t edid_len = read_edid_blob(dpy, res->outputs[i], edid, sizeof(edid));
        memset(out->edid_hash, 0, sizeof(out->edid_hash));
        if (edid_len > 0) {
            fnv1a_64(edid, edid_len, out->edid_hash);
        }

        // ── Scale selection ──
        //
        // default_scale: picked from PPI bands.
        // user_scale:    persisted override if one exists for this EDID.
        // scale:         the effective value (user_scale if non-zero,
        //                otherwise default_scale).
        out->default_scale = default_scale_from_ppi(
            out->width, out->height, out->mm_width, out->mm_height);
        out->user_scale = (edid_len > 0)
                            ? find_override_scale(out->edid_hash)
                            : 0.0f;
        out->scale = (out->user_scale > 0.0f)
                        ? out->user_scale
                        : out->default_scale;

        fprintf(stderr,
                "[display] Output %d: %s — %dx%d @ %dHz (pos %d,%d) "
                "scale=%.2f%s%s%s\n",
                output_count, out->name, out->width, out->height,
                out->refresh_hz, out->x, out->y, (double)out->scale,
                out->user_scale > 0.0f ? " [user]" : " [default]",
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
    MROutput *primary = display_get_primary();
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

    // Publish the per-output scale table to the root window so standalone
    // shell components can pick it up without calling into us. Covers
    // hotplug too because display_handle_hotplug() re-enters display_init.
    publish_scales_to_root();

    fprintf(stderr, "[display] Initialized: %d output(s)\n", output_count);
    return true;
}


// ============================================================================
//  Output queries
// ============================================================================

MROutput *display_get_outputs(int *count)
{
    if (count) *count = output_count;
    return outputs;
}

MROutput *display_get_primary(void)
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

bool display_enable_vrr(MROutput *output)
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

void display_disable_vrr(MROutput *output)
{
    if (!output) return;

    if (output->vrr_enabled) {
        output->vrr_enabled = false;
        fprintf(stderr, "[display] VRR disabled on %s — reverting to %dHz fixed\n",
                output->name, output->refresh_hz);
    }
}

bool display_is_vrr_active(MROutput *output)
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
    MROutput *primary = display_get_primary();

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

int display_get_viewport_for_output(MROutput *output,
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
//  Per-output scale accessors (HiDPI)
// ============================================================================

float display_get_scale_for_output(const MROutput *output)
{
    // Callers forward NULL when the window has no hosting output yet
    // (e.g., orphaned after a hotplug). 1.0 is the sensible fallback —
    // apps treat points and pixels as equivalent at that scale.
    if (!output) return 1.0f;
    return output->scale > 0.0f ? output->scale : 1.0f;
}

float display_get_primary_scale(void)
{
    return display_get_scale_for_output(display_get_primary());
}

float display_scale_at_point(int x, int y)
{
    // Simple rect-containment scan. output_count is tiny (~4 in any sane
    // setup), so the O(N) loop is never a hot path. If the point lands in
    // a gap — which shouldn't happen on a fully configured RandR layout —
    // we fall back to the primary's scale so the caller still gets a
    // sensible density.
    for (int i = 0; i < output_count; i++) {
        MROutput *o = &outputs[i];
        if (x >= o->x && x < o->x + o->width &&
            y >= o->y && y < o->y + o->height) {
            return display_get_scale_for_output(o);
        }
    }
    return display_get_primary_scale();
}

bool display_set_scale_for_output(MROutput *output, float scale)
{
    if (!output) return false;

    // Zero (or negative) clears the override and reverts to the default.
    // Reject out-of-range to keep the persistence file sane if a buggy
    // caller ever reaches here.
    bool clear = (scale <= 0.0f);
    if (!clear && (scale < 0.5f || scale > 4.0f)) {
        fprintf(stderr,
                "[display] Rejecting out-of-range scale %.3f for %s\n",
                (double)scale, output->name);
        return false;
    }

    // Update the in-memory overrides table.
    bool found = false;
    for (int i = 0; i < scale_override_count; i++) {
        if (memcmp(scale_overrides[i].edid_hash,
                   output->edid_hash, 8) == 0) {
            if (clear) {
                // Remove this entry by shifting the tail down. O(N), but
                // N is tiny.
                for (int j = i; j < scale_override_count - 1; j++) {
                    scale_overrides[j] = scale_overrides[j + 1];
                }
                scale_override_count--;
            } else {
                scale_overrides[i].scale = scale;
            }
            found = true;
            break;
        }
    }
    if (!found && !clear) {
        if (scale_override_count >= MAX_SCALE_OVERRIDES) {
            fprintf(stderr,
                    "[display] Scale override table full (%d) — dropping oldest\n",
                    MAX_SCALE_OVERRIDES);
            // Simple FIFO eviction so a user who cycles a lot of monitors
            // doesn't permanently jam the table.
            for (int j = 0; j < scale_override_count - 1; j++) {
                scale_overrides[j] = scale_overrides[j + 1];
            }
            scale_override_count--;
        }
        ScaleOverride *o = &scale_overrides[scale_override_count++];
        memcpy(o->edid_hash, output->edid_hash, 8);
        o->scale = scale;
    }

    // Propagate the change to every currently-enumerated output sharing
    // the same EDID (same physical monitor on a different port, mirrored
    // clones, etc.).
    for (int i = 0; i < output_count; i++) {
        if (memcmp(outputs[i].edid_hash, output->edid_hash, 8) == 0) {
            outputs[i].user_scale = clear ? 0.0f : scale;
            outputs[i].scale = (outputs[i].user_scale > 0.0f)
                                 ? outputs[i].user_scale
                                 : outputs[i].default_scale;
        }
    }

    // Rewrite the shell-facing property so live subscribers (menubar, dock,
    // desktop) see the new scale immediately via PropertyNotify.
    publish_scales_to_root();

    return save_scale_overrides();
}


// ============================================================================
//  Reverse scale-request atom — pane writes, MoonRock reads
// ============================================================================
//
// The Displays pane in systemcontrol lives in a separate X11 process and
// cannot call display_set_scale_for_output() directly. Instead it writes a
// single line to the root-window property _MOONROCK_SET_OUTPUT_SCALE, which
// we read here and dispatch to the setter. This mirrors the forward-
// direction publish atom, so the entire bridge uses one wire shape.

static Atom g_set_scale_atom = None;

Atom display_scale_request_atom(Display *dpy)
{
    if (g_set_scale_atom == None && dpy) {
        g_set_scale_atom = XInternAtom(dpy,
                                       MOONROCK_SET_SCALE_ATOM_NAME, False);
    }
    return g_set_scale_atom;
}

// Name-match lookup. Case-sensitive so the pane can match the exact string
// MoonRock publishes. Returns NULL on no match — callers log and move on.
static MROutput *find_output_by_name(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < output_count; i++) {
        if (strcmp(outputs[i].name, name) == 0) return &outputs[i];
    }
    return NULL;
}

void display_handle_scale_request(Display *dpy, Window root)
{
    if (!dpy) return;
    Atom atom = display_scale_request_atom(dpy);
    if (atom == None) return;

    Atom actual_type = None;
    int  actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = NULL;

    int rc = XGetWindowProperty(dpy, root, atom,
                                0, 256, False, AnyPropertyType,
                                &actual_type, &actual_format,
                                &nitems, &bytes_after, &prop);
    if (rc != Success || !prop) {
        if (prop) XFree(prop);
        return;
    }

    // The pane writes one UTF8_STRING line. We only need the first line
    // (the contract is one request per property write); additional lines
    // are ignored so malformed clients can't drive multiple setters in
    // one write.
    char line[128];
    size_t len = (size_t)nitems;
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, prop, len);
    line[len] = '\0';
    XFree(prop);

    // Strip to the first newline.
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';

    char   name[MOONROCK_SCALE_NAME_MAX];
    double scale = 0.0;
    if (sscanf(line, "%63s %lf", name, &scale) != 2) {
        fprintf(stderr, "[display] Ignoring malformed scale request: '%s'\n",
                line);
        XDeleteProperty(dpy, root, atom);
        return;
    }

    MROutput *o = find_output_by_name(name);
    if (!o) {
        fprintf(stderr,
                "[display] Scale request for unknown output '%s' — ignoring\n",
                name);
        XDeleteProperty(dpy, root, atom);
        return;
    }

    fprintf(stderr, "[display] Scale request: %s → %.3f\n",
            name, scale);
    display_set_scale_for_output(o, (float)scale);

    // Delete the property so an immediate re-write of the same value still
    // produces a PropertyNotify (X11 only fires notifications on value
    // *change* or on a fresh set-after-delete).
    XDeleteProperty(dpy, root, atom);
}


// ============================================================================
//  Gaming mode
// ============================================================================

void display_set_game_mode(GameMode mode)
{
    if (mode == current_game_mode) return;

    const char *mode_names[] = { "OFF", "BYPASS" };
    fprintf(stderr, "[display] Game mode: %s -> %s\n",
            mode_names[current_game_mode], mode_names[mode]);

    // Handle transitions that require cleanup of the previous mode.
    switch (current_game_mode) {
        case GAME_MODE_BYPASS:
            // Leaving bypass mode — disable direct scanout so the compositor
            // takes over rendering again.
            display_disable_direct_scanout();
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
    MROutput *primary = display_get_primary();
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
    // Normally, MoonRock calls XCompositeRedirectSubwindows() during init,
    // which tells the X server to draw every window into an off-screen pixmap
    // instead of directly to the screen. MoonRock then reads those pixmaps as
    // GL textures and composites them together.
    //
    // XCompositeUnredirectWindow() reverses this for a single window. The X
    // server goes back to painting that window directly to the framebuffer,
    // completely bypassing our compositor. The result:
    //   - Zero extra buffer copies (the game's buffer goes straight to the
    //     display via the X server's built-in DRI path).
    //   - One less frame of latency (no waiting for MoonRock to composite).
    //   - Reduced GPU load (MoonRock doesn't need to texture-map this window).
    //
    // CompositeRedirectManual matches the redirect mode we used when setting
    // up redirection in mr.c — the unredirect mode must match the
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
    // window into an off-screen pixmap, and MoonRock will composite it via GL
    // on the next frame.
    //
    // CompositeRedirectManual must match the mode used in both the original
    // redirect (in mr.c) and the unredirect (in enable_direct_scanout).
    XCompositeRedirectWindow(display_dpy, direct_scanout_win,
                             CompositeRedirectManual);

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
bool display_check_direct_scanout(CCWM *wm)
{
    if (!wm || !wm->dpy) return false;

    // ── Case 1: Scanout is currently active — validate the window ──
    if (direct_scanout_win != None) {

        // Check if the window is still eligible for direct scanout.
        // It might have left fullscreen, been covered by another window,
        // or been destroyed entirely.
        if (!display_can_direct_scanout(direct_scanout_win, wm->dpy)) {
            // The window no longer qualifies — disable scanout so MoonRock
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

    // Reset frame timing state.
    frame_start = 0.0;
    frame_count = 0;
    last_fps_time = 0.0;
    memset(&metrics, 0, sizeof(metrics));

    fprintf(stderr, "[display] Display management shut down\n");
}
