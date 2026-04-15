// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// ============================================================================
//  MoonRock Color — Display scaling, HDR, and color management implementation
// ============================================================================
//
// This file implements the color management subsystem described in
// moonrock_color.h. It uses XRandR to detect connected displays, computes
// fractional DPI scale factors, provides tone mapping uniforms for HDR
// shaders, and manages gamma correction.
//
// Key implementation details:
//
//   Display detection:
//     We iterate over XRandR outputs and filter for connected displays.
//     For each one, we pull the CRTC (cathode ray tube controller — a
//     holdover name from CRT days, now just means "the thing driving the
//     output") to get the current resolution and mode. Physical dimensions
//     come from the output info (which reads them from EDID).
//
//   PPI calculation:
//     PPI = width_in_pixels / (width_in_mm / 25.4)
//     25.4 mm per inch. We use the horizontal axis because it's the most
//     reliable (some monitors report inaccurate vertical dimensions).
//
//   Scale factor rounding:
//     Raw PPI values produce ugly scale factors (e.g., 2.947916...). We
//     round to the nearest 0.25 increment (1.0, 1.25, 1.5, 1.75, 2.0, etc.)
//     because fractional scales at those intervals produce clean pixel grids
//     when combined with integer render scaling.
//
//   Tone mapping:
//     Currently a pass-through — we just set the exposure and gamma uniforms
//     on whatever shader program is active. The actual Reinhard/ACES math
//     lives in the fragment shader source. This module owns the parameters.
//
//   GL function pointers:
//     We load glGetUniformLocation and glUniform1f via glXGetProcAddress,
//     just like moonrock_shaders.c does. We need these to set tone mapping
//     uniforms on shader programs.
//
// ============================================================================

#include "moonrock_color.h"

#include <X11/Xlib.h>                  // Display, Window, RootWindow
#include <X11/extensions/Xrandr.h>     // XRandR — display enumeration
#include <GL/gl.h>                     // OpenGL types
#include <GL/glext.h>                  // GL extension function typedefs
#include <GL/glx.h>                    // glXGetProcAddress — runtime GL loader
#include <math.h>                      // roundf, ceilf
#include <stdio.h>                     // fprintf for error/debug logging
#include <string.h>                    // strncpy, memset


// ============================================================================
//  Constants
// ============================================================================

// Maximum number of displays we track. 8 should be more than enough for
// any reasonable multi-monitor setup (most people have 1–3).
#define MAX_DISPLAYS 8

// The "standard" PPI that corresponds to scale factor 1.0x.
// This comes from the classic assumption that a desktop monitor is ~96 DPI.
// macOS uses 72 as its base, but 96 is the Linux/Windows convention.
#define BASE_PPI 96.0f

// Millimeters per inch — used to convert mm to inches for PPI calculation.
#define MM_PER_INCH 25.4f


// ============================================================================
//  GL 2.0+ function pointers
// ============================================================================
//
// We need glGetUniformLocation and glUniform1f to set tone mapping parameters
// on shader programs. These are OpenGL 2.0 functions that aren't available
// in the base GL/gl.h header — we load them at runtime from the GPU driver.
//
// See moonrock_shaders.c for a detailed explanation of why GL functions need
// to be loaded this way.

static PFNGLGETUNIFORMLOCATIONPROC pfn_glGetUniformLocation = NULL;
static PFNGLUNIFORM1FPROC         pfn_glUniform1f           = NULL;


// ============================================================================
//  Module state
// ============================================================================
//
// All state is file-scoped (static) so it's invisible to the rest of the
// codebase. Other modules access it through the public API functions.

// Array of detected displays. Populated during color_init().
static DisplayInfo displays[MAX_DISPLAYS];

// How many displays are currently in the array.
static int display_count = 0;

// Current gamma value. sRGB standard is 2.2.
// This is used for both the tone mapping shader uniform and (optionally)
// the X11 gamma ramp.
static float current_gamma = 2.2f;

// Whether GL function pointers have been loaded.
// We only need to load them once, even if color_init() is called again.
static bool gl_funcs_loaded = false;

// Path to the currently loaded ICC profile, or empty string if none.
// For now we just store the path — full ICC color transformation is future work.
static char icc_profile_path[512] = {0};


// ============================================================================
//  Internal helpers
// ============================================================================

// load_gl_functions — Load the GL 2.0 function pointers we need.
//
// We call glXGetProcAddress() to get the actual function addresses from the
// GPU driver. This must be done after the GL context is created (which
// mr_init() handles before calling color_init()).
//
// Returns true if all required functions were found.
static bool load_gl_functions(void)
{
    if (gl_funcs_loaded) return true;

    // glXGetProcAddress takes a function name as a string and returns
    // a generic function pointer. We cast it to the correct type.
    pfn_glGetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)
        glXGetProcAddress((const GLubyte *)"glGetUniformLocation");

    pfn_glUniform1f = (PFNGLUNIFORM1FPROC)
        glXGetProcAddress((const GLubyte *)"glUniform1f");

    // Both functions are required for tone mapping to work
    if (!pfn_glGetUniformLocation || !pfn_glUniform1f) {
        fprintf(stderr, "[mr_color] WARNING: Could not load GL uniform "
                "functions. Tone mapping will be unavailable.\n");
        return false;
    }

    gl_funcs_loaded = true;
    return true;
}


// compute_refresh_hz — Extract the refresh rate from an XRandR mode.
//
// XRandR stores refresh rate as dotClock / (hTotal * vTotal), which gives
// the number of full frames per second. The result is in Hz (e.g., 60, 144).
//
// Parameters:
//   res  — The screen resources (contains the mode list).
//   mode — The RRMode ID to look up.
//
// Returns the refresh rate in Hz, or 0 if the mode wasn't found.
static int compute_refresh_hz(XRRScreenResources *res, RRMode mode)
{
    // Walk through all modes in the screen resources to find the one
    // matching the given mode ID
    for (int i = 0; i < res->nmode; i++) {
        XRRModeInfo *mi = &res->modes[i];

        if (mi->id == mode) {
            // Avoid division by zero if the mode info is malformed
            if (mi->hTotal == 0 || mi->vTotal == 0) return 0;

            // dotClock is in Hz (pixels per second).
            // hTotal * vTotal = pixels per frame.
            // dotClock / (hTotal * vTotal) = frames per second.
            //
            // We add 0.5 before casting to int to round to nearest
            // instead of truncating (e.g., 59.94 rounds to 60).
            double rate = (double)mi->dotClock
                        / ((double)mi->hTotal * (double)mi->vTotal);
            return (int)(rate + 0.5);
        }
    }

    return 0;
}


// check_vrr_capable — Check if an XRandR output supports Variable Refresh Rate.
//
// VRR (FreeSync/G-Sync) allows the display to sync its refresh rate to the
// compositor's actual frame rate, eliminating tearing without the latency
// penalty of traditional VSync. We detect this by looking for the
// "vrr_capable" property on the XRandR output.
//
// Parameters:
//   dpy    — The X11 display connection.
//   res    — The XRandR screen resources.
//   output — The RROutput to check.
//
// Returns true if the output has vrr_capable = 1.
static bool check_vrr_capable(Display *dpy, XRRScreenResources *res,
                               RROutput output)
{
    (void)res;  // Not needed for property queries, but kept for consistency

    // Get the list of all properties on this output
    int nprop = 0;
    Atom *props = XRRListOutputProperties(dpy, output, &nprop);
    if (!props) return false;

    bool vrr = false;

    // The "vrr_capable" atom — we look this up from the X server
    Atom vrr_atom = XInternAtom(dpy, "vrr_capable", True);

    // If the atom doesn't exist on this server, VRR isn't supported at all
    if (vrr_atom == None) {
        XFree(props);
        return false;
    }

    // Check each property to see if it's the vrr_capable one
    for (int i = 0; i < nprop; i++) {
        if (props[i] == vrr_atom) {
            // Found the property — now read its value
            Atom actual_type;
            int actual_format;
            unsigned long nitems, bytes_after;
            unsigned char *data = NULL;

            // XRRGetOutputProperty reads the property value.
            // We expect a single 32-bit integer (0 or 1).
            int status = XRRGetOutputProperty(
                dpy, output, vrr_atom,
                0,              // offset: start from the beginning
                1,              // length: we only need one value
                False,          // delete: don't delete after reading
                False,          // pending: get current value, not pending
                AnyPropertyType,// req_type: accept any type
                &actual_type, &actual_format, &nitems, &bytes_after, &data
            );

            if (status == Success && data && nitems > 0) {
                // A value of 1 means VRR is supported
                vrr = (*(unsigned long *)data == 1);
            }

            if (data) XFree(data);
            break;  // Found the property, no need to keep looking
        }
    }

    XFree(props);
    return vrr;
}


// check_hdr_capable — Check EDID data for HDR capability hints.
//
// HDR support is advertised in the EDID (Extended Display Identification
// Data) block that every monitor sends over the cable. Specifically, we
// look for the "EDID" property on the XRandR output and check for the
// presence of HDR metadata extensions.
//
// This is a simplified check — full EDID parsing would involve decoding
// the CTA-861 extension blocks and looking for the HDR Static Metadata
// Data Block (tag 0x06 in the extended tag codes). For now, we look for
// a marker that indicates HDR10 support.
//
// Parameters:
//   dpy    — The X11 display connection.
//   output — The RROutput to check.
//
// Returns true if HDR capability was detected in the EDID.
static bool check_hdr_capable(Display *dpy, RROutput output)
{
    // Look up the EDID property atom
    Atom edid_atom = XInternAtom(dpy, "EDID", True);
    if (edid_atom == None) return false;

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *edid_data = NULL;

    // Read the EDID property. EDID blocks are typically 128 or 256 bytes.
    // We request up to 512 bytes to catch extended EDID blocks.
    int status = XRRGetOutputProperty(
        dpy, output, edid_atom,
        0, 512, False, False, AnyPropertyType,
        &actual_type, &actual_format, &nitems, &bytes_after, &edid_data
    );

    if (status != Success || !edid_data || nitems < 128) {
        if (edid_data) XFree(edid_data);
        return false;
    }

    bool hdr = false;

    // Basic EDID is 128 bytes. Extended EDID (CEA/CTA-861) adds more blocks
    // starting at byte 128. If we have more than 128 bytes, check the
    // extension blocks for HDR metadata.
    //
    // The CTA-861 extension contains "data blocks" that describe display
    // capabilities. HDR Static Metadata is indicated by Extended Tag Code
    // 0x06 within a data block tagged with code 7 (Use Extended Tag).
    //
    // This is a simplified scan — we look for the byte pattern that
    // indicates HDR metadata presence. A production implementation would
    // do full EDID block parsing.
    if (nitems > 128) {
        // Scan the extension data for HDR metadata indicator.
        // In CTA-861, data blocks start at offset 4 within the extension.
        // Each block has a header byte: (tag << 5) | length.
        // Tag 7 = "Use Extended Tag", and the next byte is the extended tag.
        // Extended tag 0x06 = HDR Static Metadata Data Block.
        for (unsigned long i = 128; i < nitems - 1; i++) {
            unsigned char tag = (edid_data[i] >> 5) & 0x07;
            if (tag == 7 && (i + 1) < nitems && edid_data[i + 1] == 0x06) {
                hdr = true;
                break;
            }
        }
    }

    XFree(edid_data);
    return hdr;
}


// ============================================================================
//  Public API — Initialization and shutdown
// ============================================================================

bool color_init(void *dpy, int screen)
{
    Display *d = (Display *)dpy;
    Window root = RootWindow(d, screen);

    // Reset state from any previous initialization
    display_count = 0;
    memset(displays, 0, sizeof(displays));

    // Load GL function pointers for tone mapping uniform access.
    // This can fail if the GL context isn't ready yet, but we continue
    // anyway — display detection doesn't need GL.
    load_gl_functions();

    // ---- Query XRandR for all outputs (monitors) ----
    //
    // XRRGetScreenResources queries the X server for the current display
    // configuration: what outputs exist, what modes (resolutions) are
    // available, and which CRTCs (display controllers) are driving which
    // outputs.
    XRRScreenResources *res = XRRGetScreenResources(d, root);
    if (!res) {
        fprintf(stderr, "[mr_color] ERROR: XRRGetScreenResources failed. "
                "Cannot detect displays.\n");
        return false;
    }

    // Iterate over all outputs. An "output" in XRandR terminology is a
    // physical connector (HDMI port, DisplayPort, eDP for built-in panel).
    // Not all outputs have monitors connected — we skip disconnected ones.
    for (int i = 0; i < res->noutput && display_count < MAX_DISPLAYS; i++) {
        // Get info about this output (name, connection status, modes, etc.)
        XRROutputInfo *out = XRRGetOutputInfo(d, res, res->outputs[i]);
        if (!out) continue;

        // Skip outputs that don't have a monitor plugged in.
        // RR_Connected means a display is physically connected to this port.
        if (out->connection != RR_Connected) {
            XRRFreeOutputInfo(out);
            continue;
        }

        // We have a connected display — populate its DisplayInfo struct
        DisplayInfo *di = &displays[display_count];
        memset(di, 0, sizeof(DisplayInfo));

        // Copy the output name (e.g., "eDP-1", "HDMI-1", "DP-2").
        // strncpy with size-1 ensures null termination.
        strncpy(di->name, out->name, sizeof(di->name) - 1);
        di->name[sizeof(di->name) - 1] = '\0';

        // Physical dimensions in millimeters, reported by the monitor's EDID.
        // These can be 0 if the monitor doesn't report them (cheap displays).
        di->width_mm = out->mm_width;
        di->height_mm = out->mm_height;

        // ---- Get current resolution from the CRTC ----
        //
        // A CRTC (display controller) drives one or more outputs. The CRTC
        // knows the current resolution, position, and which mode is active.
        // An output without a CRTC is connected but not displaying anything
        // (e.g., a second monitor that hasn't been configured yet).
        if (out->crtc) {
            XRRCrtcInfo *crtc = XRRGetCrtcInfo(d, res, out->crtc);
            if (crtc) {
                di->width_px = crtc->width;
                di->height_px = crtc->height;

                // Get the refresh rate from the current mode.
                // A "mode" in XRandR is a specific resolution + refresh rate
                // combination (e.g., 1920x1080@60Hz, 1920x1080@144Hz).
                di->refresh_hz = compute_refresh_hz(res, crtc->mode);

                XRRFreeCrtcInfo(crtc);
            }
        }

        // ---- Calculate PPI (pixels per inch) ----
        //
        // PPI tells us how dense the pixels are. Higher PPI means smaller
        // physical pixels, which means we need to scale the UI up so
        // elements remain a readable physical size.
        //
        // We only calculate if width_mm is reported (> 0) to avoid
        // division by zero. If the monitor doesn't report physical size,
        // PPI stays at 0 and we default the scale factor to 1.0.
        if (di->width_mm > 0) {
            float width_inches = (float)di->width_mm / MM_PER_INCH;
            di->ppi = (float)di->width_px / width_inches;
        }

        // ---- Compute the scale factor from PPI ----
        //
        // The scale factor is how much we multiply UI coordinates to make
        // them appear the correct physical size on this display.
        //
        //   96 PPI  => 1.0x (reference: a typical 24" 1080p monitor)
        //   144 PPI => 1.5x (e.g., a 14" 1080p laptop)
        //   192 PPI => 2.0x (e.g., a 24" 4K "Retina" display)
        //   283 PPI => ~2.75x -> 3.0x (Legion Go 8" 1920x1200)
        //
        // We round to the nearest 0.25 increment. Why? Fractional values
        // like 1.33x or 2.947x cause misaligned pixels and blurry rendering.
        // 0.25 increments (1.0, 1.25, 1.5, ...) are the smallest steps that
        // still produce clean results when combined with integer supersampling.
        if (di->ppi > 0) {
            di->scale_factor = di->ppi / BASE_PPI;

            // Round to nearest 0.25:
            //   1. Multiply by 4 (so 0.25 increments become integers)
            //   2. Round to nearest integer
            //   3. Divide by 4
            di->scale_factor = roundf(di->scale_factor * 4.0f) / 4.0f;

            // Clamp to sane range: at least 1.0x, at most 4.0x.
            // Below 1.0 would shrink the UI below readable size.
            // Above 4.0 is unrealistic for current hardware.
            if (di->scale_factor < 1.0f) di->scale_factor = 1.0f;
            if (di->scale_factor > 4.0f) di->scale_factor = 4.0f;
        } else {
            // No physical size info — assume standard density
            di->scale_factor = 1.0f;
        }

        // ---- Check for VRR (Variable Refresh Rate) support ----
        //
        // VRR lets the display sync to our actual frame rate instead of
        // requiring us to hit exactly 60Hz (or whatever). This eliminates
        // tearing without the latency penalty of traditional VSync.
        di->has_vrr = check_vrr_capable(d, res, res->outputs[i]);

        // ---- Check for HDR capability via EDID ----
        //
        // We parse the EDID data attached to the output to look for
        // HDR Static Metadata, which indicates the display supports HDR10.
        di->has_hdr = check_hdr_capable(d, res->outputs[i]);

        // Log what we found for debugging
        fprintf(stderr, "[mr_color] Display %d: \"%s\" %dx%d @ %dHz, "
                "%.0f PPI, scale %.2fx%s%s\n",
                display_count, di->name,
                di->width_px, di->height_px, di->refresh_hz,
                di->ppi, di->scale_factor,
                di->has_vrr ? ", VRR" : "",
                di->has_hdr ? ", HDR" : "");

        display_count++;
        XRRFreeOutputInfo(out);
    }

    // Done with XRandR resources
    XRRFreeScreenResources(res);

    if (display_count == 0) {
        fprintf(stderr, "[mr_color] WARNING: No connected displays found. "
                "Scale factor defaults to 1.0x.\n");
        return false;
    }

    fprintf(stderr, "[mr_color] Initialized: %d display(s), primary "
            "scale factor %.2fx\n", display_count, displays[0].scale_factor);
    return true;
}


void color_shutdown(void)
{
    // Clear all display state
    memset(displays, 0, sizeof(displays));
    display_count = 0;

    // Reset ICC profile path
    icc_profile_path[0] = '\0';

    // Reset gamma to sRGB default
    current_gamma = 2.2f;

    fprintf(stderr, "[mr_color] Shut down.\n");
}


// ============================================================================
//  Public API — Display queries
// ============================================================================

DisplayInfo *color_get_display(int index)
{
    // Bounds check — return NULL for invalid indices so the caller can
    // handle the "no such display" case gracefully.
    if (index < 0 || index >= display_count) return NULL;
    return &displays[index];
}


int color_get_display_count(void)
{
    return display_count;
}


// ============================================================================
//  Public API — Scale factor
// ============================================================================

float color_get_scale_factor(void)
{
    // If no displays were detected, return 1.0 (no scaling) as a safe
    // default. The compositor can still render — things just won't be
    // scaled for high-DPI.
    if (display_count == 0) return 1.0f;

    // Return the primary display's scale factor. Index 0 is the first
    // connected output, which is typically the built-in panel on a laptop
    // (eDP-1) or the first external monitor on a desktop.
    return displays[0].scale_factor;
}


float color_scale(float value)
{
    // Convert a logical coordinate to physical pixels.
    // A 100-pixel button at 2.0x scale becomes 200 physical pixels.
    return value * color_get_scale_factor();
}


float color_unscale(float value)
{
    // Convert a physical pixel coordinate back to logical coordinates.
    // A mouse click at physical pixel 200 on a 2.0x display is at
    // logical coordinate 100.
    //
    // Guard against division by zero (shouldn't happen since
    // color_get_scale_factor() returns at least 1.0, but belt & suspenders).
    float sf = color_get_scale_factor();
    return sf > 0.0f ? value / sf : value;
}


int color_get_render_scale(void)
{
    // For fractional scaling, we render at the next integer scale up and
    // then downscale. This avoids the blurriness of direct fractional
    // rendering.
    //
    // Examples:
    //   1.0x  -> render at 1x (no extra work)
    //   1.25x -> render at 2x, downscale to 1.25x
    //   1.5x  -> render at 2x, downscale to 1.5x
    //   2.0x  -> render at 2x (exact, no downscale needed)
    //   2.75x -> render at 3x, downscale to 2.75x
    //
    // ceilf rounds up to the next integer.
    float sf = color_get_scale_factor();
    int render_scale = (int)ceilf(sf);

    // Clamp to [1, 4] — rendering at higher than 4x would be wasteful
    // and most GPUs would struggle with it anyway.
    if (render_scale < 1) render_scale = 1;
    if (render_scale > 4) render_scale = 4;

    return render_scale;
}


// ============================================================================
//  Public API — HDR tone mapping
// ============================================================================

void color_apply_tone_mapping(unsigned int shader_program, float exposure)
{
    // Can't set uniforms without GL functions
    if (!gl_funcs_loaded || !pfn_glGetUniformLocation || !pfn_glUniform1f) {
        return;
    }

    // Set the exposure uniform on the shader program.
    //
    // "u_exposure" is the name of the uniform variable in the fragment
    // shader's GLSL source. The shader uses this to scale pixel brightness:
    //   color = color * exposure;
    //
    // glGetUniformLocation returns -1 if the uniform doesn't exist in the
    // shader (which is fine — not every shader has tone mapping).
    int loc = pfn_glGetUniformLocation(shader_program, "u_exposure");
    if (loc >= 0) {
        pfn_glUniform1f(loc, exposure);
    }

    // Set the gamma uniform so the shader can apply gamma correction.
    //
    // The shader applies: color = pow(color, 1.0 / gamma)
    // This converts from linear light (which is correct for math) to the
    // non-linear curve that displays expect.
    int gamma_loc = pfn_glGetUniformLocation(shader_program, "u_gamma");
    if (gamma_loc >= 0) {
        pfn_glUniform1f(gamma_loc, current_gamma);
    }
}


// ============================================================================
//  Public API — Gamma correction
// ============================================================================

void color_set_gamma(float gamma)
{
    // Store the new gamma value for use in tone mapping shader uniforms
    current_gamma = gamma;

    // NOTE: We could also set the X11 gamma ramp here using
    // XRRSetCrtcGamma(). That would apply gamma correction to the entire
    // display output (not just MoonRock's rendering), which affects all
    // applications. For now we only use the shader-based approach, which
    // is more precise and doesn't interfere with other programs.
    //
    // To implement X11 gamma ramp correction:
    //   1. Get the current ramp: XRRGetCrtcGamma(dpy, crtc)
    //   2. Compute new ramp values: value = pow(i / 65535.0, 1.0 / gamma) * 65535
    //   3. Set it: XRRSetCrtcGamma(dpy, crtc, new_ramp)
    //   4. Free: XRRFreeGamma(new_ramp)
    //
    // This is left as future work because it requires storing the Display*
    // and CRTC handles beyond init time, and we need to be careful to restore
    // the original ramp on shutdown.

    fprintf(stderr, "[mr_color] Gamma set to %.2f\n", gamma);
}


float color_get_gamma(void)
{
    return current_gamma;
}


// ============================================================================
//  Public API — ICC profile
// ============================================================================

bool color_load_icc_profile(const char *path)
{
    if (!path) {
        fprintf(stderr, "[mr_color] ERROR: NULL ICC profile path.\n");
        return false;
    }

    // For now, we just store the path. A full implementation would:
    //   1. Open and parse the ICC profile file (the format is defined by
    //      the International Color Consortium spec, ICC.1:2022).
    //   2. Extract the display's color gamut, whitepoint, and transfer
    //      function (TRC — Tone Reproduction Curve).
    //   3. Build a 3D LUT (lookup table) or matrix that transforms sRGB
    //      colors to the display's native color space.
    //   4. Upload the LUT to the GPU as a texture or set of uniforms.
    //   5. Apply the transform in the compositor's output shader.
    //
    // Libraries like lcms2 (Little CMS) handle the heavy lifting of ICC
    // profile parsing and color space conversion. We would add it as a
    // dependency when this feature is fully implemented.
    //
    // For now, storing the path is enough to prove the API works and to
    // let other modules query whether a profile is loaded.

    // Verify the file exists by trying to open it
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[mr_color] ERROR: Cannot open ICC profile: %s\n",
                path);
        return false;
    }

    // Read the first 4 bytes to do a basic sanity check.
    // ICC profiles start with a 4-byte size field (big-endian uint32).
    // The size should be at least 128 bytes (the minimum profile header).
    unsigned char header[4];
    size_t read_count = fread(header, 1, 4, f);
    fclose(f);

    if (read_count < 4) {
        fprintf(stderr, "[mr_color] ERROR: ICC profile too small: %s\n",
                path);
        return false;
    }

    // Decode the 4-byte big-endian size
    unsigned int profile_size = ((unsigned int)header[0] << 24)
                              | ((unsigned int)header[1] << 16)
                              | ((unsigned int)header[2] << 8)
                              | ((unsigned int)header[3]);

    if (profile_size < 128) {
        fprintf(stderr, "[mr_color] ERROR: ICC profile size too small "
                "(%u bytes): %s\n", profile_size, path);
        return false;
    }

    // Store the path
    strncpy(icc_profile_path, path, sizeof(icc_profile_path) - 1);
    icc_profile_path[sizeof(icc_profile_path) - 1] = '\0';

    fprintf(stderr, "[mr_color] Loaded ICC profile: %s (%u bytes)\n",
            path, profile_size);
    return true;
}
