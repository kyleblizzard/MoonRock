// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// ============================================================================
//  MoonRock Color — Display scaling, HDR, and color management for MoonRock
// ============================================================================
//
// The MoonRock compositor needs to render correctly across wildly different
// displays. The Lenovo Legion Go has a 1920x1200 8-inch screen at 283 PPI,
// while a typical external 1080p monitor at 24" sits around 92 PPI, and a
// 27" 4K display lands near 163 PPI. Without proper scaling, UI elements
// designed for one display will be comically small or absurdly large on
// another.
//
// What this module does:
//
//   1. DISPLAY DETECTION — Uses XRandR (X Resize and Rotate extension) to
//      enumerate every connected monitor and read its resolution, physical
//      size, refresh rate, and EDID data. EDID (Extended Display Identification
//      Data) is a block of information that every monitor sends over the cable
//      describing its capabilities.
//
//   2. FRACTIONAL DPI SCALING — Computes a scale factor for each display
//      based on its PPI (pixels per inch). A 96 PPI display gets 1.0x, a
//      283 PPI display gets ~2.75x (rounded to nearest 0.25 for clean
//      rendering). All UI elements and input coordinates are scaled through
//      this module so everything appears the correct physical size.
//
//   3. HDR PREPARATION — Even if the current display is SDR (standard dynamic
//      range), we build the infrastructure for HDR rendering: 10/12-bit
//      framebuffer support, exposure control, and tone mapping shader uniforms.
//      When an HDR monitor is connected, the pipeline is ready.
//
//   4. COLOR MANAGEMENT — ICC profile loading, EDID-based color info, and
//      gamma correction. ICC profiles describe a display's exact color
//      behavior so we can compensate for differences between monitors.
//      Gamma correction converts linear light values to the non-linear
//      curve that displays expect (typically sRGB, gamma 2.2).
//
// How scaling works for the compositor:
//
//   Fractional scaling is tricky. If the scale factor is 1.5x, you can't just
//   multiply every pixel coordinate by 1.5 — that gives you half-pixel offsets
//   that cause blurry text and misaligned edges. The standard approach is:
//
//     1. Render at integer scale (e.g., 2x for a 1.5x factor).
//     2. Downscale the result to the actual display resolution.
//
//   This module provides color_get_render_scale() which returns the integer
//   scale to render at, and color_get_scale_factor() which returns the exact
//   fractional value for coordinate math.
//
// ============================================================================

#ifndef MR_COLOR_H
#define MR_COLOR_H

#include <stdbool.h>


// ============================================================================
//  Display information structure
// ============================================================================
//
// One of these is populated for each connected display during color_init().
// The data comes from XRandR queries (resolution, physical size, refresh)
// and EDID parsing (HDR capability, VRR support).
//
// Fields:
//   name         — XRandR output name like "eDP-1" (laptop panel), "HDMI-1",
//                  "DP-2", etc. Useful for logging and user-facing config.
//   width_px/height_px  — Current resolution in pixels.
//   width_mm/height_mm  — Physical panel size in millimeters (from EDID).
//                          Used to calculate PPI.
//   ppi          — Pixels per inch, calculated as:
//                  width_px / (width_mm / 25.4)
//                  25.4 is the number of millimeters in one inch.
//   scale_factor — The UI scale multiplier derived from PPI:
//                  96 PPI = 1.0x, 192 PPI = 2.0x, etc.
//                  Rounded to nearest 0.25 for clean pixel alignment.
//   refresh_hz   — Display refresh rate in Hz (e.g., 60, 144, 165).
//   has_hdr      — True if the display advertises HDR capability (HDR10,
//                  Dolby Vision, etc.) via EDID.
//   has_vrr      — True if the display supports Variable Refresh Rate
//                  (FreeSync/G-Sync). Detected via XRandR output properties.

typedef struct {
    char name[64];          // XRandR output name: "eDP-1", "HDMI-1", etc.
    int width_px, height_px;   // Current resolution in pixels
    int width_mm, height_mm;   // Physical size in millimeters (from EDID)
    float ppi;              // Pixels per inch (computed from above)
    float scale_factor;     // UI scale: 1.0, 1.25, 1.5, 2.0, etc.
    int refresh_hz;         // Refresh rate in Hz
    bool has_hdr;           // Display advertises HDR support
    bool has_vrr;           // Display supports variable refresh rate
} DisplayInfo;


// ============================================================================
//  Initialization and shutdown
// ============================================================================

// Initialize the color management subsystem.
//
// This is the main entry point. It:
//   1. Queries XRandR for all connected outputs (monitors).
//   2. Reads each output's resolution, physical size, and refresh rate.
//   3. Computes PPI and scale factors for each display.
//   4. Checks for VRR capability via XRandR output properties.
//   5. Parses EDID data for HDR capability hints.
//   6. Loads the default ICC profile if one exists.
//
// Parameters:
//   dpy    — The X11 Display pointer (cast to void* to avoid requiring
//            X11 headers in every file that includes this header).
//   screen — The X11 screen number (usually 0 for single-GPU setups).
//
// Returns true if at least one display was detected, false on failure.
bool color_init(void *dpy, int screen);

// Shut down color management and free all resources.
//
// Clears the display list and releases any loaded ICC profile data.
// Call this during MoonRock's shutdown sequence.
void color_shutdown(void);


// ============================================================================
//  Display queries
// ============================================================================

// Get the DisplayInfo for a specific monitor by index.
//
// Index 0 is the primary display (usually the first connected output).
// Returns NULL if the index is out of range.
DisplayInfo *color_get_display(int index);

// Get the number of detected displays.
//
// Returns the count of connected monitors found during color_init().
int color_get_display_count(void);


// ============================================================================
//  Scale factor API
// ============================================================================
//
// These functions handle coordinate scaling between "logical" coordinates
// (what the UI thinks in) and "physical" coordinates (actual pixels on the
// display). On a 2x display, a 100-pixel-wide logical button occupies 200
// physical pixels.

// Get the scale factor of the primary display.
//
// Returns 1.0 for a standard 96 PPI monitor, 2.0 for ~192 PPI, and
// fractional values like 1.5 or 1.75 for displays in between.
// Returns 1.0 if no displays are detected.
float color_get_scale_factor(void);

// Scale a logical coordinate to physical pixels.
//
// Example: color_scale(100.0) on a 2.0x display returns 200.0.
// Used when converting UI layout coordinates to GL rendering coordinates.
float color_scale(float value);

// Unscale a physical pixel coordinate back to logical coordinates.
//
// Example: color_unscale(200.0) on a 2.0x display returns 100.0.
// Used when converting raw input coordinates (mouse position from X11)
// into logical UI coordinates.
float color_unscale(float value);

// Get the integer render scale for the shader pipeline.
//
// Fractional scaling works by rendering at a higher integer resolution and
// then downscaling. For a 1.5x scale factor, we render at 2x and downscale.
// For 1.0x, we render at 1x (no extra work).
//
// Returns the ceiling of the fractional scale factor, clamped to [1, 4].
int color_get_render_scale(void);


// ============================================================================
//  HDR tone mapping
// ============================================================================
//
// HDR (High Dynamic Range) displays can show a wider range of brightness
// than standard displays. When rendering to an HDR display (or preparing
// HDR-ready content), the compositor renders in linear light with high
// precision, then applies "tone mapping" to compress the brightness range
// into what the display can actually show.
//
// Tone mapping algorithms (Reinhard, ACES, etc.) are implemented in the
// fragment shader. This function just sends the control parameters to the
// GPU as shader uniforms.

// Apply tone mapping parameters to a shader program.
//
// This sets the u_exposure and u_gamma uniforms on the given shader.
// The actual tone mapping math happens in the fragment shader.
//
// shader_program — The compiled GLSL program handle to set uniforms on.
// exposure       — Brightness multiplier. 1.0 = neutral, >1.0 = brighter,
//                  <1.0 = darker. Used to adjust for different HDR content.
void color_apply_tone_mapping(unsigned int shader_program, float exposure);


// ============================================================================
//  Gamma correction
// ============================================================================
//
// Gamma describes the non-linear relationship between a pixel's numeric
// value and the actual brightness the display produces. Most displays follow
// an approximate power curve:
//
//   brightness = value ^ gamma
//
// The standard sRGB gamma is 2.2. A gamma of 1.0 means linear (no curve).
// Raising gamma makes the image darker; lowering it makes it brighter.
//
// We track gamma as a float and pass it to the tone mapping shader. We also
// (optionally) set the X11 gamma ramp via XRandR so the correction applies
// to the whole display output, not just MoonRock's rendered content.

// Set the gamma correction value.
//
// gamma — The gamma exponent. Standard sRGB is 2.2.
//         Values below 2.2 brighten the image; above 2.2 darken it.
void color_set_gamma(float gamma);

// Get the current gamma correction value.
//
// Returns the gamma exponent most recently set by color_set_gamma(),
// or 2.2 (sRGB default) if it has never been changed.
float color_get_gamma(void);


// ============================================================================
//  ICC profile support
// ============================================================================
//
// ICC (International Color Consortium) profiles describe the exact color
// behavior of a display — its gamut (range of colors it can produce), its
// whitepoint, its transfer function (gamma curve), etc. By loading a
// display's ICC profile, we can adjust our rendering to produce accurate
// colors on that specific panel.
//
// For now, this is a placeholder that loads and stores the profile path.
// Full ICC color transformation (using lcms2 or similar) is a future task.

// Load an ICC profile from disk.
//
// path — Absolute path to a .icc or .icm file.
//
// Returns true if the file was loaded successfully, false on error.
bool color_load_icc_profile(const char *path);


#endif // MR_COLOR_H
