// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/
//
// CopyCatOS — MoonRock Compositor (Phase 1)
//
// MoonRock is CopyCatOS's built-in OpenGL compositor, replacing picom. Instead of
// relying on an external program to composite windows, MoonRock handles it
// directly inside the window manager using OpenGL for GPU-accelerated rendering.
//
// How compositing works at a high level:
//   Every window on your screen is normally drawn directly to the display by
//   the X server. A compositor intercepts this — it tells X to draw each window
//   to an off-screen image (called a "pixmap") instead. The compositor then
//   takes all those images, layers them on top of each other (back-to-front,
//   like stacking transparencies), adds effects like shadows, and draws the
//   final combined image to the screen.
//
// MoonRock's approach:
//   1. XComposite redirects all windows to off-screen pixmaps (Manual mode —
//      we handle ALL rendering, unlike the old compositor's Automatic mode).
//   2. Each window's pixmap is bound as an OpenGL texture using the
//      GLX_EXT_texture_from_pixmap extension (or a CPU fallback if unavailable).
//   3. Every frame, we draw textured quads (rectangles) for each window at its
//      screen position, back-to-front, with alpha blending for transparency.
//   4. Shadows use a cached Gaussian blur (3-pass box blur approximation).
//   5. Double-buffered via GLX — we draw to a back buffer, then swap it to the
//      screen, preventing flicker.
//   6. VSync keeps us locked to the monitor's refresh rate (typically 60 Hz),
//      preventing screen tearing.

// M_PI requires _GNU_SOURCE (provided by meson via -D_GNU_SOURCE).
#include "moonrock.h"
#include "moonrock_shaders.h"
#include "moonrock_display.h"
#include "moonrock_color.h"
#include "moonrock_robust.h"
#include "moonrock_plugin.h"
#include "moonrock_anim.h"
#include "moonrock_mission_control.h"
#include "moonrock_touch.h"
#include "moonbase_host.h"
#include "ewmh.h"

// Forward declaration removed — compositor.c is no longer linked.
// mr_create_argb_visual() now handles fallback internally.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// OpenGL and GLX headers — these give us access to GPU rendering on X11.
// GL/gl.h:      Core OpenGL functions (drawing, textures, blending)
// GL/glx.h:     GLX — the bridge between OpenGL and X11 (contexts, surfaces)
// GL/glxext.h:  GLX extensions (texture_from_pixmap, swap control, etc.)
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>

// X11 extension headers for compositing, damage tracking, and input shapes
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

// ────────────────────────────────────────────────────────────────────────
// SECTION: GLX extension function pointers
// ────────────────────────────────────────────────────────────────────────
//
// OpenGL extensions aren't part of the base API — their functions must be
// loaded at runtime using glXGetProcAddress(). We store pointers to these
// functions so we can call them later. If the extension isn't available,
// the pointer stays NULL and we skip that feature.
//
// "PFNGLX...PROC" is the naming convention for OpenGL function pointer types:
//   PFN = "Pointer to FunctioN"
//   GLX = the GLX subsystem
//   ...EXT = it's an extension function

// glXBindTexImageEXT: Binds an X pixmap to an OpenGL texture. This is the
// key function that lets us use window contents as GPU textures without
// copying pixel data through the CPU.
static PFNGLXBINDTEXIMAGEEXTPROC glXBindTexImageEXT_func = NULL;

// glXReleaseTexImageEXT: Releases a pixmap that was bound as a texture.
// Must be called before re-binding or destroying the pixmap.
static PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT_func = NULL;

// glXSwapIntervalEXT: Controls VSync (vertical synchronization).
// Setting the interval to 1 means "wait for one monitor refresh before
// swapping buffers," which prevents screen tearing.
static PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT_func = NULL;

// ────────────────────────────────────────────────────────────────────────
// SECTION: Per-window texture tracking
// ────────────────────────────────────────────────────────────────────────
//
// Each visible window on screen needs a corresponding OpenGL texture so we
// can draw it. This struct tracks the relationship between an X window and
// its GPU texture, plus metadata we need for rendering.

struct WindowTexture {
    Window xwin;            // The X11 window ID (the frame window, not client)
    Pixmap pixmap;          // The off-screen pixmap from XComposite
    GLXPixmap glx_pixmap;   // GLX wrapper around the pixmap (for texture binding)
    GLuint texture;         // OpenGL texture ID (the GPU-side handle)
    Damage damage;          // XDamage handle — notifies us when contents change
    int x, y, w, h;        // Window position and size on screen
    bool dirty;             // True if the texture needs refreshing (contents changed)
    bool has_alpha;         // True if the window uses a 32-bit ARGB visual (transparency)
    bool bound;             // True if the texture is currently bound to a pixmap
    bool override_redirect; // True if the window has override_redirect set (menus, tooltips, popups)

    // Set to true while a genie minimize animation is playing for this window.
    // When true, the normal composite pass skips this window entirely — the
    // animation system's anim_draw() renders it instead (as the deformed genie
    // shape). Without this flag, the window would appear twice each frame:
    // once in its original position and once as the animated genie effect.
    bool animating_out;

    // ── Cached Gaussian shadow texture (Phase 2) ──
    // Instead of re-computing the blurred shadow every frame, we cache the
    // result as an OpenGL texture. The shadow only needs to be regenerated
    // when the window is resized. shadow_tex_w/h store the dimensions the
    // shadow was generated for, so we can detect when it's stale.
    GLuint shadow_tex;       // OpenGL texture holding the pre-blurred shadow
    int shadow_tex_w;        // Width the shadow was generated at (includes padding)
    int shadow_tex_h;        // Height the shadow was generated at (includes padding)
    float shadow_scale;      // Backing scale at generation time. If the window
                             // migrates to an output of a different scale (e.g.
                             // dragged from a 1.0× external onto the 1.75×
                             // Legion panel) the cached blur is wrong density
                             // and must be regenerated.
};

// ────────────────────────────────────────────────────────────────────────
// SECTION: Module state (file-scope static)
// ────────────────────────────────────────────────────────────────────────
//
// All MoonRock state lives in this single struct. Using a static struct at
// file scope means only mr.c can access it, keeping the compositor's
// internals private from the rest of the window manager.

// Global flag indicating whether compositing is active. In CopyCatOS this is
// defined in decor.c and shared with frame.c. In standalone MoonRock we
// define it here since there is no external WM module providing it.
#if !defined(MR_EMBEDDED_IN_WM)
bool compositor_active = false;
#else
extern bool compositor_active;
#endif

static struct {
    bool active;                    // Is MoonRock initialized and running?

    // ── Compositor selection ownership ──
    // A compositor must claim the _NET_WM_CM_S<screen> selection so that
    // other compositors (and apps querying COMPOSITE_MANAGER) know a
    // compositor is active. We create a tiny invisible window just to hold
    // the selection — X requires a window as the selection owner.
    Window cm_owner_win;

    // ── XComposite overlay window ──
    // The overlay window is a special child of root that sits above all
    // normal windows. We render into it instead of root directly, which is
    // the correct approach for a real compositor. Input is passed through
    // via XFixes so the overlay doesn't eat mouse/keyboard events.
    Window overlay_win;

    // ── GLX / OpenGL context ──
    // A "GLX context" is the bridge between X11 and OpenGL. It holds all
    // OpenGL state (current texture, blend mode, etc.) and must be "made
    // current" before any GL calls will work.
    GLXContext gl_context;

    // FBConfig = "Framebuffer Configuration" — describes the pixel format
    // of the rendering surface (color depth, double buffering, alpha, etc.).
    // We need one that supports texture_from_pixmap for binding X pixmaps.
    GLXFBConfig fb_config;

    // The GLX drawable we render to — wraps the overlay window so OpenGL
    // can draw to it via GLX.
    GLXWindow gl_window;

    // Screen dimensions — needed for the orthographic projection matrix
    // that maps pixel coordinates to OpenGL's coordinate system.
    int root_w, root_h;

    // ── XDamage event tracking ──
    // XDamage events don't have a fixed event type number. Instead, the X
    // server assigns a base number at runtime. A DamageNotify event has type
    // (damage_event_base + XDamageNotify). We store the base here.
    int damage_event_base;
    int damage_error_base;

    // ── Extension support flags ──
    // Not all GPU drivers support GLX_EXT_texture_from_pixmap. If they don't,
    // we fall back to reading pixmap data through the CPU (slower but universal).
    bool has_texture_from_pixmap;

    // ── Per-window texture array ──
    // We track up to MAX_CLIENTS windows (matching the WM's client limit).
    // Each entry maps an X window to its OpenGL texture.
    struct WindowTexture windows[MAX_CLIENTS];
    int window_count;

    // ── ARGB visual for transparent frame windows ──
    // Frame windows need a 32-bit visual (with alpha channel) so the shadow
    // regions can be semi-transparent. We find this visual during init and
    // share it with the frame creation code.
    Visual *argb_visual;
    Colormap argb_colormap;

    // ── FBConfig for pixmap binding ──
    // A separate FBConfig used specifically for creating GLX pixmaps from
    // X pixmaps. This may differ from the rendering FBConfig because pixmap
    // binding has its own requirements (GLX_BIND_TO_TEXTURE_RGBA_EXT, etc.).
    GLXFBConfig pixmap_fb_config;
    GLXFBConfig pixmap_fb_config_rgb;  // For non-alpha (24-bit) windows

    // ── Desktop background color ──
    // Cleared to this color each frame. Dark blue-gray by default.
    float bg_r, bg_g, bg_b;

    // ── Shader pipeline (Phase 3) ──
    // GLSL shader programs for all rendering. Replaces fixed-function
    // glBegin/glEnd with modern VBO + shader draws.
    ShaderPrograms shaders;

    // ── Per-frame projection matrix ──
    // 4x4 orthographic projection computed once per frame and passed to
    // every shader program as the u_projection uniform.
    float projection[16];
} mr;

// ────────────────────────────────────────────────────────────────────────
// SECTION: Forward declarations (private helper functions)
// ────────────────────────────────────────────────────────────────────────

static void draw_window_shadow(struct WindowTexture *wt, bool focused);
static void generate_shadow_texture(struct WindowTexture *wt, bool focused);
static void refresh_window_texture(CCWM *wm, struct WindowTexture *wt);
static void refresh_window_texture_fallback(CCWM *wm, struct WindowTexture *wt);
static void release_window_texture(CCWM *wm, struct WindowTexture *wt);
static struct WindowTexture *find_window_texture(Window xwin);
static bool choose_fb_configs(Display *dpy, int screen);
static bool load_glx_extensions(Display *dpy, int screen);
static void sync_tracked_windows(CCWM *wm);

// ────────────────────────────────────────────────────────────────────────
// SECTION: FBConfig selection
// ────────────────────────────────────────────────────────────────────────
//
// An FBConfig (Framebuffer Configuration) describes how pixels are stored
// in a rendering surface: how many bits for red, green, blue, alpha,
// whether it's double-buffered, etc.
//
// We need TWO kinds of FBConfig:
//   1. A "rendering" FBConfig for the GLX context (what we draw to)
//   2. A "pixmap" FBConfig for binding X pixmaps as textures
//
// These often differ because the pixmap config needs GLX_BIND_TO_TEXTURE_*
// support, which not all configs provide.

static bool choose_fb_configs(Display *dpy, int screen)
{
    // ── Rendering FBConfig ──
    // This is for the main drawing surface. We want:
    //   - RGBA color (8 bits per channel)
    //   - Double buffering (draw to back buffer, then swap — no flicker)
    //   - Window drawable type (we're rendering to a window, not a pixmap)
    int render_attrs[] = {
        GLX_RENDER_TYPE,    GLX_RGBA_BIT,      // Color channels: R, G, B, A
        GLX_DRAWABLE_TYPE,  GLX_WINDOW_BIT,     // We'll render to a window
        GLX_DOUBLEBUFFER,   True,               // Double buffer to avoid flicker
        GLX_RED_SIZE,       8,                  // 8 bits per color channel
        GLX_GREEN_SIZE,     8,
        GLX_BLUE_SIZE,      8,
        GLX_ALPHA_SIZE,     8,                  // Alpha channel for transparency
        None                                    // Null terminator for the list
    };

    int num_configs = 0;
    GLXFBConfig *configs = glXChooseFBConfig(dpy, screen, render_attrs, &num_configs);
    if (!configs || num_configs == 0) {
        fprintf(stderr, "[moonrock] ERROR: No suitable FBConfig for rendering\n");
        if (configs) XFree(configs);
        return false;
    }

    // Take the first matching config — the X server returns them sorted by
    // preference (fewest extra features first, best match at index 0).
    mr.fb_config = configs[0];
    XFree(configs);

    // ── Pixmap FBConfig (RGBA — for 32-bit windows with alpha) ──
    // For binding X pixmaps as OpenGL textures, we need configs that declare
    // support for GLX_BIND_TO_TEXTURE_RGBA_EXT. This is a separate query
    // because not all rendering configs support pixmap binding.
    int pixmap_attrs_rgba[] = {
        GLX_RENDER_TYPE,                GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE,              GLX_PIXMAP_BIT,     // Must support pixmap drawables
        GLX_BIND_TO_TEXTURE_RGBA_EXT,   True,               // Can bind RGBA pixmaps as textures
        GLX_RED_SIZE,                   8,
        GLX_GREEN_SIZE,                 8,
        GLX_BLUE_SIZE,                  8,
        GLX_ALPHA_SIZE,                 8,
        None
    };

    configs = glXChooseFBConfig(dpy, screen, pixmap_attrs_rgba, &num_configs);
    if (configs && num_configs > 0) {
        mr.pixmap_fb_config = configs[0];
        XFree(configs);
    } else {
        fprintf(stderr, "[moonrock] WARNING: No FBConfig for RGBA pixmap binding\n");
        if (configs) XFree(configs);
        // Not fatal — we'll fall back to CPU texture upload
    }

    // ── Pixmap FBConfig (RGB — for 24-bit windows without alpha) ──
    int pixmap_attrs_rgb[] = {
        GLX_RENDER_TYPE,                GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE,              GLX_PIXMAP_BIT,
        GLX_BIND_TO_TEXTURE_RGB_EXT,    True,               // Can bind RGB pixmaps as textures
        GLX_RED_SIZE,                   8,
        GLX_GREEN_SIZE,                 8,
        GLX_BLUE_SIZE,                  8,
        None
    };

    configs = glXChooseFBConfig(dpy, screen, pixmap_attrs_rgb, &num_configs);
    if (configs && num_configs > 0) {
        mr.pixmap_fb_config_rgb = configs[0];
        XFree(configs);
    } else {
        fprintf(stderr, "[moonrock] WARNING: No FBConfig for RGB pixmap binding\n");
        if (configs) XFree(configs);
    }

    return true;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: GLX extension loading
// ────────────────────────────────────────────────────────────────────────
//
// GLX extensions add capabilities beyond the base GLX spec. We need to:
//   1. Check which extensions are available (by searching a string list)
//   2. Load the function pointers for those extensions
//
// This is standard practice in OpenGL programming — extensions are always
// loaded at runtime because different drivers support different extensions.

static bool load_glx_extensions(Display *dpy, int screen)
{
    // glXQueryExtensionsString returns a space-separated list of all GLX
    // extensions supported by this driver. We search it for the ones we need.
    const char *exts = glXQueryExtensionsString(dpy, screen);
    if (!exts) {
        fprintf(stderr, "[moonrock] WARNING: Cannot query GLX extensions\n");
        return false;
    }

    // ── GLX_EXT_texture_from_pixmap ──
    // This is the most important extension for a compositor. It lets us bind
    // an X pixmap (off-screen window contents) directly as an OpenGL texture,
    // avoiding a slow CPU copy. Without it, we must use XGetImage() to read
    // the pixmap into CPU memory, then upload it to the GPU with glTexImage2D.
    if (strstr(exts, "GLX_EXT_texture_from_pixmap")) {
        // Load the function pointers using glXGetProcAddress.
        // The (const GLubyte*) cast is required by the API signature.
        glXBindTexImageEXT_func = (PFNGLXBINDTEXIMAGEEXTPROC)
            glXGetProcAddress((const GLubyte *)"glXBindTexImageEXT");
        glXReleaseTexImageEXT_func = (PFNGLXRELEASETEXIMAGEEXTPROC)
            glXGetProcAddress((const GLubyte *)"glXReleaseTexImageEXT");

        // Both functions must be present for the extension to work
        if (glXBindTexImageEXT_func && glXReleaseTexImageEXT_func) {
            mr.has_texture_from_pixmap = true;
            fprintf(stderr, "[moonrock] GLX_EXT_texture_from_pixmap available "
                    "(fast path)\n");
        } else {
            fprintf(stderr, "[moonrock] WARNING: texture_from_pixmap functions "
                    "not loadable\n");
        }
    } else {
        fprintf(stderr, "[moonrock] GLX_EXT_texture_from_pixmap NOT available "
                "(using CPU fallback)\n");
    }

    // ── GLX_EXT_swap_control ──
    // Controls VSync. When enabled (interval=1), the buffer swap waits for
    // the monitor's vertical blank period. This prevents "screen tearing" —
    // a visual artifact where the top half of the screen shows one frame and
    // the bottom half shows the next.
    if (strstr(exts, "GLX_EXT_swap_control")) {
        glXSwapIntervalEXT_func = (PFNGLXSWAPINTERVALEXTPROC)
            glXGetProcAddress((const GLubyte *)"glXSwapIntervalEXT");
        if (glXSwapIntervalEXT_func) {
            fprintf(stderr, "[moonrock] GLX_EXT_swap_control available (VSync)\n");
        }
    }

    return true;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: ARGB visual discovery
// ────────────────────────────────────────────────────────────────────────
//
// This is the same logic as compositor_create_argb_visual() from the old
// compositor, but stored in MoonRock's state. We need a 32-bit visual with
// an alpha channel so frame windows can have semi-transparent shadow regions.

static bool find_argb_visual(CCWM *wm)
{
    // Ask X for all 32-bit TrueColor visuals on this screen.
    // "TrueColor" means direct RGB values (not palette-indexed).
    // "32-bit" means 8 bits each for R, G, B, and A (alpha).
    XVisualInfo templ;
    templ.screen = wm->screen;
    templ.depth = 32;
    templ.class = TrueColor;

    int num_visuals = 0;
    XVisualInfo *visuals = XGetVisualInfo(wm->dpy,
                                          VisualScreenMask | VisualDepthMask |
                                          VisualClassMask,
                                          &templ, &num_visuals);

    if (!visuals || num_visuals == 0) {
        if (visuals) XFree(visuals);
        return false;
    }

    // Use the first match (usually there's exactly one 32-bit TrueColor visual)
    mr.argb_visual = visuals[0].visual;

    // Every visual needs a colormap (X11 requirement). AllocNone means
    // "don't reserve any color cells" — it's just a formality for TrueColor.
    mr.argb_colormap = XCreateColormap(wm->dpy, wm->root,
                                             mr.argb_visual, AllocNone);

    XFree(visuals);
    return true;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Initialization
// ────────────────────────────────────────────────────────────────────────

bool mr_init(CCWM *wm)
{
    if (!wm || !wm->dpy) return false;

    // Zero out all state so we start clean
    memset(&mr, 0, sizeof(mr));

    // Default background color — a dark neutral gray. This is what you see
    // behind all windows if there's no wallpaper.
    mr.bg_r = 0.15f;
    mr.bg_g = 0.15f;
    mr.bg_b = 0.18f;

    fprintf(stderr, "[moonrock] Initializing MoonRock Compositor...\n");

    // ── Step 1: Check for GLX extension ──
    // GLX is the glue between X11 and OpenGL. Without it, we can't do any
    // GPU-accelerated rendering at all.
    int glx_error_base, glx_event_base;
    if (!glXQueryExtension(wm->dpy, &glx_error_base, &glx_event_base)) {
        fprintf(stderr, "[moonrock] ERROR: GLX extension not available. "
                "Cannot initialize OpenGL compositor.\n");
        return false;
    }

    // Check GLX version — we need at least 1.3 for FBConfig support
    int glx_major = 0, glx_minor = 0;
    if (!glXQueryVersion(wm->dpy, &glx_major, &glx_minor)) {
        fprintf(stderr, "[moonrock] ERROR: Cannot query GLX version\n");
        return false;
    }
    fprintf(stderr, "[moonrock] GLX version: %d.%d\n", glx_major, glx_minor);

    if (glx_major < 1 || (glx_major == 1 && glx_minor < 3)) {
        fprintf(stderr, "[moonrock] ERROR: GLX 1.3+ required, got %d.%d\n",
                glx_major, glx_minor);
        return false;
    }

    // ── Step 2: Check for XComposite ──
    // XComposite lets us redirect window rendering to off-screen pixmaps.
    // We need version 0.2+ for XCompositeNameWindowPixmap().
    int composite_major = 0, composite_minor = 0;
    if (!XCompositeQueryVersion(wm->dpy, &composite_major, &composite_minor)) {
        fprintf(stderr, "[moonrock] ERROR: XComposite not available\n");
        return false;
    }
    if (composite_major == 0 && composite_minor < 2) {
        fprintf(stderr, "[moonrock] ERROR: XComposite 0.2+ required, got %d.%d\n",
                composite_major, composite_minor);
        return false;
    }
    fprintf(stderr, "[moonrock] XComposite %d.%d\n", composite_major, composite_minor);

    // ── Step 3: Check for XDamage ──
    // XDamage tells us when a window's contents have changed, so we only
    // refresh textures that actually need updating (not every window every frame).
    int damage_major = 0, damage_minor = 0;
    if (!XDamageQueryVersion(wm->dpy, &damage_major, &damage_minor)) {
        fprintf(stderr, "[moonrock] ERROR: XDamage not available\n");
        return false;
    }
    // Store the event base — we need it to identify DamageNotify events
    XDamageQueryExtension(wm->dpy, &mr.damage_event_base,
                          &mr.damage_error_base);
    fprintf(stderr, "[moonrock] XDamage %d.%d (event base: %d)\n",
            damage_major, damage_minor, mr.damage_event_base);

    // ── Step 4: Check for XFixes ──
    // XFixes provides input shape manipulation — we use it to make shadow
    // regions click-through (clicks pass to windows behind them).
    int fixes_major = 0, fixes_minor = 0;
    if (!XFixesQueryVersion(wm->dpy, &fixes_major, &fixes_minor)) {
        fprintf(stderr, "[moonrock] ERROR: XFixes not available\n");
        return false;
    }
    fprintf(stderr, "[moonrock] XFixes %d.%d\n", fixes_major, fixes_minor);

    // ── Step 5: Choose FBConfigs ──
    // FBConfigs describe pixel formats. We need configs for both rendering
    // (the output surface) and pixmap binding (input textures from windows).
    if (!choose_fb_configs(wm->dpy, wm->screen)) {
        fprintf(stderr, "[moonrock] ERROR: Cannot find suitable FBConfigs\n");
        return false;
    }
    fprintf(stderr, "[moonrock] FBConfigs selected\n");

    // ── Step 6: Create GLX context ──
    // A GLX context holds all OpenGL state. We create it from our rendering
    // FBConfig, then "make it current" so GL calls go to this context.
    // NULL = no shared context (we only have one).
    // True = direct rendering (bypass X server for GL calls — much faster).
    mr.gl_context = glXCreateNewContext(wm->dpy, mr.fb_config,
                                             GLX_RGBA_TYPE, NULL, True);
    if (!mr.gl_context) {
        fprintf(stderr, "[moonrock] ERROR: Cannot create GLX context\n");
        return false;
    }
    fprintf(stderr, "[moonrock] GLX context created (direct: %s)\n",
            glXIsDirect(wm->dpy, mr.gl_context) ? "yes" : "no");

    // ── Step 7: Get the XComposite overlay window and create a GLX surface ──
    //
    // The overlay window is a special X11 window managed by the XComposite
    // extension that sits above ALL other windows on the screen. Rendering
    // into it (rather than directly into root) is the correct approach for a
    // real compositor because:
    //   1. It does not interfere with root's background pixmap or other WM state.
    //   2. The X server already knows to display it on top of everything.
    //   3. It avoids FBConfig BadMatch errors that can occur when binding root.
    //
    // After getting the overlay window we:
    //   a. Wrap it in a GLX surface so OpenGL can render to it.
    //   b. Use XFixes to set an empty input shape so mouse/keyboard events
    //      pass straight through to the windows beneath — the overlay must
    //      NEVER eat input events.
    mr.overlay_win = XCompositeGetOverlayWindow(wm->dpy, wm->root);
    if (!mr.overlay_win) {
        fprintf(stderr, "[moonrock] ERROR: Cannot get XComposite overlay window\n");
        glXDestroyContext(wm->dpy, mr.gl_context);
        mr.gl_context = NULL;
        return false;
    }
    fprintf(stderr, "[moonrock] Overlay window obtained (0x%lx)\n", mr.overlay_win);

    // Allow input events to pass through the overlay to windows underneath.
    // An empty XserverRegion means "no input area" — the overlay is invisible
    // to the input system.
    XserverRegion empty_region = XFixesCreateRegion(wm->dpy, NULL, 0);
    XFixesSetWindowShapeRegion(wm->dpy, mr.overlay_win,
                               ShapeInput, 0, 0, empty_region);
    XFixesDestroyRegion(wm->dpy, empty_region);

    // Wrap the overlay window in a GLX drawable so OpenGL can render to it.
    mr.gl_window = glXCreateWindow(wm->dpy, mr.fb_config,
                                   mr.overlay_win, NULL);
    if (!mr.gl_window) {
        fprintf(stderr, "[moonrock] ERROR: Cannot create GLX window from overlay\n");
        XCompositeReleaseOverlayWindow(wm->dpy, wm->root);
        mr.overlay_win = 0;
        glXDestroyContext(wm->dpy, mr.gl_context);
        mr.gl_context = NULL;
        return false;
    }

    // ── Step 8: Make the context current ──
    // "Making current" binds the GLX context to the current thread and the
    // GLX window. All subsequent OpenGL calls will render to this window
    // through this context. Think of it as "activating" the context.
    if (!glXMakeContextCurrent(wm->dpy, mr.gl_window, mr.gl_window,
                                mr.gl_context)) {
        fprintf(stderr, "[moonrock] ERROR: Cannot make GLX context current\n");
        glXDestroyWindow(wm->dpy, mr.gl_window);
        glXDestroyContext(wm->dpy, mr.gl_context);
        mr.gl_window = 0;
        mr.gl_context = NULL;
        return false;
    }

    // ── Step 9: Load GLX extension functions ──
    load_glx_extensions(wm->dpy, wm->screen);

    // ── Step 10: Enable VSync ──
    // Without VSync, the GPU renders as fast as possible and tears appear on
    // screen where one frame ends and the next begins. VSync synchronizes
    // buffer swaps with the monitor's refresh rate.
    if (glXSwapIntervalEXT_func) {
        glXSwapIntervalEXT_func(wm->dpy, mr.gl_window, 1);
        fprintf(stderr, "[moonrock] VSync enabled (swap interval = 1)\n");
    } else {
        fprintf(stderr, "[moonrock] WARNING: VSync not available "
                "(may see tearing)\n");
    }

    // ── Step 10.8: Claim the compositor selection _NET_WM_CM_S<n> ──
    // X11 convention: any program that wants to act as a compositor must
    // claim this selection on the screen it manages. Other compositors check
    // for this selection before starting — if it's already owned, they back
    // off. Apps can also query it to know whether compositing is active.
    //
    // We create a minimal invisible window just to hold the selection.
    // X11 requires a window as the selection owner; the window itself
    // doesn't need to be visible or functional.
    {
        char sel_name[32];
        snprintf(sel_name, sizeof(sel_name), "_NET_WM_CM_S%d", wm->screen);
        Atom cm_atom = XInternAtom(wm->dpy, sel_name, False);

        // Warn if something else already owns it (another compositor running)
        Window existing = XGetSelectionOwner(wm->dpy, cm_atom);
        if (existing != None) {
            fprintf(stderr, "[moonrock] WARNING: %s already owned by window 0x%lx — "
                    "another compositor may be active\n", sel_name, existing);
        }

        // Create the owner window (1x1 px, off-screen at -10,-10)
        mr.cm_owner_win = XCreateSimpleWindow(wm->dpy, wm->root,
                                               -10, -10, 1, 1, 0, 0, 0);
        XSetSelectionOwner(wm->dpy, cm_atom, mr.cm_owner_win, CurrentTime);

        if (XGetSelectionOwner(wm->dpy, cm_atom) == mr.cm_owner_win) {
            fprintf(stderr, "[moonrock] Compositor selection %s claimed\n", sel_name);
        } else {
            fprintf(stderr, "[moonrock] WARNING: Failed to claim %s\n", sel_name);
        }
    }

    // ── Step 11: Set up XComposite redirection ──
    // Manual mode: MoonRock is responsible for compositing all window contents
    // onto the screen. Windows are redirected to off-screen pixmaps, and
    // mr_composite() draws them via OpenGL each frame.
    XCompositeRedirectSubwindows(wm->dpy, wm->root, CompositeRedirectManual);
    fprintf(stderr, "[moonrock] XComposite redirect set (MANUAL mode — MoonRock compositing)\n");

    // ── Step 12: Find ARGB visual ──
    // Needed for frame windows that want semi-transparent shadow regions.
    if (find_argb_visual(wm)) {
        fprintf(stderr, "[moonrock] Found 32-bit ARGB visual\n");
    } else {
        fprintf(stderr, "[moonrock] WARNING: No 32-bit ARGB visual "
                "(shadows may not render correctly)\n");
    }

    // ── Step 13: Store screen dimensions ──
    mr.root_w = wm->root_w;
    mr.root_h = wm->root_h;

    // ── Step 14: Initialize OpenGL state ──
    //
    // OpenGL is a state machine — you set modes (like blending, texturing)
    // and they stay active until you change them. Here we set up the initial
    // state that MoonRock needs.

    // Set the "clear color" — the background that shows through when no
    // windows cover a region. This fills the screen before we draw anything.
    glClearColor(mr.bg_r, mr.bg_g, mr.bg_b, 1.0f);

    // Enable 2D texturing — required for drawing window contents as textures
    // on quads. When disabled, quads are drawn as solid colors.
    glEnable(GL_TEXTURE_2D);

    // Enable alpha blending — this is how transparency works in OpenGL.
    // When blending is enabled, each pixel's final color is computed by
    // combining the source (new pixel) with the destination (existing pixel)
    // using a blend function.
    glEnable(GL_BLEND);

    // Set the blend function for premultiplied alpha.
    // "Premultiplied alpha" means the RGB values have already been multiplied
    // by the alpha value. For example, a 50% transparent red pixel would be
    // stored as (0.5, 0.0, 0.0, 0.5) instead of (1.0, 0.0, 0.0, 0.5).
    //
    // GL_ONE: use the source color as-is (it's already premultiplied)
    // GL_ONE_MINUS_SRC_ALPHA: scale the destination by (1 - source alpha)
    //
    // Final color = src_color * 1 + dst_color * (1 - src_alpha)
    // This correctly blends transparent windows over whatever is behind them.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // Set up an orthographic projection matrix.
    //
    // OpenGL normally uses a 3D coordinate system with perspective (objects
    // farther away appear smaller). For a 2D compositor, we want a flat,
    // non-perspective view where pixel coordinates map directly to screen
    // positions.
    //
    // glOrtho(left, right, bottom, top, near, far) defines the visible volume:
    //   left=0, right=root_w:    x-axis matches screen pixels
    //   top=0, bottom=root_h:    y-axis goes DOWN (screen convention)
    //   near=-1, far=1:          z-axis range (we don't use depth)
    //
    // GL_PROJECTION is the "lens" of our virtual camera.
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, mr.root_w, mr.root_h, 0, -1, 1);

    // GL_MODELVIEW positions objects in the scene. We start with identity
    // (no transformation) since our coordinates are already in screen space.
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Disable depth testing — we don't need it for 2D compositing. We
    // handle z-order ourselves by drawing back-to-front.
    glDisable(GL_DEPTH_TEST);

    // ── Step 15: Initialize the shader pipeline ──
    //
    // Load and compile all GLSL shader programs (basic, blur, shadow, genie).
    // This replaces the fixed-function glBegin/glEnd pipeline with modern
    // programmable shaders that run on the GPU. Every visual effect MoonRock
    // renders flows through these shader programs.
    //
    // shader_dir can be NULL — embedded fallback shaders are compiled into
    // moonrock_shaders.c so the compositor works even without shader files.
    if (!shaders_init(&mr.shaders, NULL)) {
        fprintf(stderr, "[moonrock] WARNING: Shader initialization failed, "
                "falling back to fixed-function GL\n");
        // We continue anyway — the compositor can still work with shaders.basic=0
        // by falling back to fixed-function for individual draw calls.
    } else {
        fprintf(stderr, "[moonrock] Shader pipeline initialized "
                "(basic=%u, blur_h=%u, blur_v=%u, shadow=%u)\n",
                mr.shaders.basic, mr.shaders.blur_h,
                mr.shaders.blur_v, mr.shaders.shadow);
    }

    // Create the VBO/VAO for quad rendering. This is a single unit quad (0..1)
    // that gets scaled and positioned by the model matrix uniform. All windows,
    // shadows, and effects are drawn as quads through this one VBO.
    shaders_init_quad_vbo();

    // Build the initial orthographic projection matrix
    shaders_ortho(mr.projection,
                  0, (float)mr.root_w,
                  (float)mr.root_h, 0,
                  -1.0f, 1.0f);

    // Mark MoonRock as active
    mr.active = true;

    // Also set the global compositor_active flag so the rest of the WM
    // (frame.c, decor.c) knows compositing is available. This flag controls
    // whether frame windows get ARGB visuals and shadow padding.
    compositor_active = true;

    // ── Step 16: Initialize MoonRock subsystems ──
    //
    // Each subsystem is independent — if one fails, the compositor still works,
    // just without that feature. Order matters: display and color must come
    // before anything that queries scale factors or monitor geometry.

    // Display management — enumerates connected monitors via XRandR, tracks
    // output geometry for multi-monitor support and direct scanout bypass.
    if (!display_init(wm->dpy, wm->screen)) {
        fprintf(stderr, "[moonrock] WARNING: Display subsystem init failed "
                "(multi-monitor features unavailable)\n");
    }

    // Color management — detects display PPI and computes scale factors for
    // HiDPI rendering. Also loads GL uniform functions for tone mapping.
    if (!color_init(wm->dpy, wm->screen)) {
        fprintf(stderr, "[moonrock] WARNING: Color subsystem init failed "
                "(scale factor defaults to 1.0x)\n");
    }

    // Robustness layer — sets up hardware cursor (smooth mouse regardless of
    // compositor load), crash recovery fallback, session persistence, and
    // power management detection.
    if (!robust_init(wm->dpy, wm->screen)) {
        fprintf(stderr, "[moonrock] WARNING: Robust subsystem init failed "
                "(hardware cursor and crash recovery unavailable)\n");
    }

    // Plugin system — initializes the theme engine with Snow Leopard defaults,
    // hot corner array, and window rule storage. Must come before any code
    // that queries plugin_get_theme() (e.g., blur-behind in mr_composite).
    if (!plugin_init()) {
        fprintf(stderr, "[moonrock] WARNING: Plugin subsystem init failed "
                "(theming unavailable)\n");
    }

    // Animation framework — zeroes the fixed-size animation slot array.
    // Must be ready before mr_composite() calls anim_update()/anim_draw().
    anim_init();

    // Mission Control — creates the initial Space and assigns existing windows.
    // Must come after the WM has scanned existing windows (wm->clients populated).
    mc_init(wm);

    // Touch input — finds XInput2 touchscreen devices and registers for
    // multitouch events. Non-fatal if no touchscreen is present (e.g., desktop).
    if (!touch_init(wm->dpy, wm->root)) {
        fprintf(stderr, "[moonrock] NOTE: Touch input not available "
                "(no touchscreen detected)\n");
    }

    fprintf(stderr, "[moonrock] MoonRock Compositor initialized successfully\n");
    fprintf(stderr, "[moonrock] OpenGL renderer: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "[moonrock] OpenGL version: %s\n", glGetString(GL_VERSION));
    return true;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Window texture management
// ────────────────────────────────────────────────────────────────────────
//
// When a window is mapped (becomes visible), we create a texture for it.
// When it's unmapped (hidden/closed), we destroy the texture. When its
// contents change (damaged), we refresh the texture data.

// Find a window's texture entry in our tracking array.
// Returns NULL if the window isn't tracked (e.g., hasn't been mapped yet).
static struct WindowTexture *find_window_texture(Window xwin)
{
    for (int i = 0; i < mr.window_count; i++) {
        if (mr.windows[i].xwin == xwin) {
            return &mr.windows[i];
        }
    }
    return NULL;
}

// Refresh a window's texture using the GLX_EXT_texture_from_pixmap extension.
// This is the "fast path" — the GPU reads the pixmap directly from video
// memory without going through the CPU.
//
// The texture_from_pixmap extension works by creating a GLX pixmap wrapper
// around the X pixmap, then binding that as a texture source. The GPU can
// then sample from it directly when drawing.
static void refresh_window_texture(CCWM *wm, struct WindowTexture *wt)
{
    if (!mr.has_texture_from_pixmap) {
        // Fall back to CPU-based texture upload
        refresh_window_texture_fallback(wm, wt);
        return;
    }

    // Override-redirect windows (tooltips, menus, popups) often use a 32-bit
    // ARGB visual that doesn't match the GLX FBConfig we chose for pixmap
    // binding. Rather than trying (and failing) the fast GLX path — which
    // generates X errors and leaves the texture blank — go straight to the
    // CPU fallback. These windows are small and transient, so the performance
    // cost is negligible.
    if (wt->override_redirect) {
        refresh_window_texture_fallback(wm, wt);
        return;
    }

    // If we already have a bound texture, release it first.
    // You can't re-bind without releasing — it's like unlocking a file before
    // another program can read it.
    if (wt->bound && wt->glx_pixmap) {
        glXReleaseTexImageEXT_func(wm->dpy, wt->glx_pixmap, GLX_FRONT_EXT);
        wt->bound = false;
    }

    // Destroy the old GLX pixmap wrapper if it exists.
    // We need a new one because the underlying X pixmap may have changed
    // (e.g., the window was resized).
    if (wt->glx_pixmap) {
        glXDestroyPixmap(wm->dpy, wt->glx_pixmap);
        wt->glx_pixmap = 0;
    }

    // Destroy the old X pixmap
    if (wt->pixmap) {
        XFreePixmap(wm->dpy, wt->pixmap);
        wt->pixmap = 0;
    }

    // Get a fresh pixmap from XComposite.
    // XCompositeNameWindowPixmap returns a handle to the off-screen buffer
    // where the X server is rendering this window's contents.
    wt->pixmap = XCompositeNameWindowPixmap(wm->dpy, wt->xwin);
    if (!wt->pixmap) {
        // Pixmap creation can fail if the window was just created and the
        // compositor redirect hasn't fully propagated. Fall back to the CPU
        // path which reads directly from the window drawable.
        refresh_window_texture_fallback(wm, wt);
        return;
    }

    // Choose the right FBConfig based on whether the window has alpha.
    // ARGB windows need the RGBA config; regular windows use RGB.
    // If the matching config wasn't found during init (null), go straight
    // to the CPU fallback rather than passing null to glXCreatePixmap.
    GLXFBConfig fb = wt->has_alpha ? mr.pixmap_fb_config
                                   : mr.pixmap_fb_config_rgb;

    if (!fb) {
        // No matching FBConfig — use the CPU fallback
        refresh_window_texture_fallback(wm, wt);
        return;
    }

    // Create a GLX pixmap — this wraps the X pixmap so GLX can use it.
    // The attributes tell GLX how to interpret the pixmap data:
    //   GLX_TEXTURE_TARGET_EXT: treat it as a 2D texture
    //   GLX_TEXTURE_FORMAT_EXT: RGBA or RGB depending on alpha support
    int pixmap_attrs[] = {
        GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
        GLX_TEXTURE_FORMAT_EXT, wt->has_alpha
            ? GLX_TEXTURE_FORMAT_RGBA_EXT
            : GLX_TEXTURE_FORMAT_RGB_EXT,
        None
    };

    wt->glx_pixmap = glXCreatePixmap(wm->dpy, fb, wt->pixmap, pixmap_attrs);
    if (!wt->glx_pixmap) {
        // GLX pixmap creation can fail for override-redirect windows (tooltips,
        // menus, popups) if their visual doesn't match the FBConfig. Fall back
        // to the CPU-based texture upload which reads pixels via XGetImage and
        // uploads them with glTexImage2D — slower but works with any visual.
        fprintf(stderr, "[moonrock] GLX pixmap failed for 0x%lx, using CPU fallback\n",
                wt->xwin);
        refresh_window_texture_fallback(wm, wt);
        return;
    }

    // Bind the GLX pixmap as the source data for our OpenGL texture.
    // After this call, the texture contains the window's current contents.
    // GLX_FRONT_EXT means "read from the front buffer" of the pixmap.
    glBindTexture(GL_TEXTURE_2D, wt->texture);
    glXBindTexImageEXT_func(wm->dpy, wt->glx_pixmap, GLX_FRONT_EXT, NULL);
    wt->bound = true;

    // Set texture filtering — how OpenGL samples the texture when it's
    // scaled up or down.
    // GL_LINEAR = bilinear interpolation (smooth, slightly blurry at edges)
    // GL_NEAREST would be pixel-perfect but looks jagged when scaled.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // GL_CLAMP_TO_EDGE prevents texture coordinates outside [0,1] from
    // wrapping around. Without this, you might see repeating artifacts at
    // window edges.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// CPU fallback for texture refresh.
// Used when GLX_EXT_texture_from_pixmap is not available. This is slower
// because we must:
//   1. Read the pixmap into CPU memory using XGetImage()
//   2. Upload the pixel data to the GPU using glTexImage2D()
//
// On a modern system, this copies data over the PCIe bus twice (GPU→CPU→GPU),
// which is much slower than the zero-copy texture_from_pixmap approach.
static void refresh_window_texture_fallback(CCWM *wm, struct WindowTexture *wt)
{
    // For override-redirect windows (tooltips, menus, popups), we read pixels
    // directly from the window instead of using XComposite pixmaps. These
    // windows are not reliably redirected by XComposite (especially when
    // created after the initial RedirectSubwindows call), and their pixmaps
    // often fail to bind via GLX. Reading directly from the window is slower
    // but always works, and these windows are small enough that the cost is
    // negligible.
    if (wt->override_redirect) {
        // Read pixels directly from the window drawable
        XImage *img = XGetImage(wm->dpy, wt->xwin, 0, 0,
                                wt->w, wt->h, AllPlanes, ZPixmap);
        if (!img) {
            // Window might not be mapped yet or might have been destroyed
            return;
        }

        glBindTexture(GL_TEXTURE_2D, wt->texture);
        glTexImage2D(GL_TEXTURE_2D, 0,
                     wt->has_alpha ? GL_RGBA : GL_RGB,
                     wt->w, wt->h, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, img->data);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        XDestroyImage(img);
        return;
    }

    // Destroy and recreate the X pixmap to get fresh contents
    if (wt->pixmap) {
        XFreePixmap(wm->dpy, wt->pixmap);
        wt->pixmap = 0;
    }

    // Try to get the composite pixmap first. If that fails (window just
    // created, redirect not yet active, or window is being destroyed),
    // fall back to reading directly from the window drawable.
    Drawable read_from = 0;
    wt->pixmap = XCompositeNameWindowPixmap(wm->dpy, wt->xwin);
    if (wt->pixmap) {
        read_from = wt->pixmap;
    } else {
        // Direct window read as last resort — same approach used for
        // override-redirect windows. This is reliable but means we're
        // reading from the window's front buffer, which may show
        // tearing during updates.
        read_from = wt->xwin;
    }

    // XGetImage reads pixel data from a drawable (pixmap or window) into
    // CPU memory. ZPixmap format returns a packed pixel array.
    XImage *img = XGetImage(wm->dpy, read_from, 0, 0,
                            wt->w, wt->h, AllPlanes, ZPixmap);
    if (!img) {
        // Window might have been destroyed between our check and read
        return;
    }

    // Upload the pixel data to our OpenGL texture.
    // GL_BGRA: X11 stores pixels as Blue-Green-Red-Alpha (on little-endian),
    //          which matches GL_BGRA byte order.
    // GL_UNSIGNED_BYTE: each color channel is one byte (0-255).
    // glTexImage2D replaces the entire texture contents.
    glBindTexture(GL_TEXTURE_2D, wt->texture);
    glTexImage2D(GL_TEXTURE_2D,     // Target: 2D texture
                 0,                  // Mipmap level 0 (base, full resolution)
                 wt->has_alpha ? GL_RGBA : GL_RGB,  // Internal format on GPU
                 wt->w, wt->h,      // Dimensions
                 0,                  // Border (must be 0)
                 GL_BGRA,            // Format of the input data
                 GL_UNSIGNED_BYTE,   // Data type per channel
                 img->data);         // Pointer to the pixel data

    // Set texture parameters (same as the fast path)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Free the XImage — we've uploaded the data to the GPU, no need to keep
    // a CPU copy.
    XDestroyImage(img);
}

// Release all GPU and X resources associated with a window's texture.
// Called when a window is unmapped or when shutting down.
static void release_window_texture(CCWM *wm, struct WindowTexture *wt)
{
    // Release the texture binding (must happen before destroying the pixmap)
    if (wt->bound && wt->glx_pixmap && glXReleaseTexImageEXT_func) {
        glXReleaseTexImageEXT_func(wm->dpy, wt->glx_pixmap, GLX_FRONT_EXT);
        wt->bound = false;
    }

    // Destroy the cached shadow texture (Phase 2 Gaussian blur cache)
    if (wt->shadow_tex) {
        glDeleteTextures(1, &wt->shadow_tex);
        wt->shadow_tex = 0;
        wt->shadow_tex_w = 0;
        wt->shadow_tex_h = 0;
    }

    // Cancel any animations still referencing this texture BEFORE deleting it.
    // anim_draw() draws both active AND just-completed animations (progress==1.0).
    // If the texture is deleted while a completed animation slot still references
    // it, the next composite frame would try to bind a deleted GL texture handle.
    anim_cancel_for_texture(wt->texture);

    // Destroy the OpenGL texture on the GPU
    if (wt->texture) {
        glDeleteTextures(1, &wt->texture);
        wt->texture = 0;
    }

    // Destroy the GLX pixmap wrapper
    if (wt->glx_pixmap) {
        glXDestroyPixmap(wm->dpy, wt->glx_pixmap);
        wt->glx_pixmap = 0;
    }

    // Free the X pixmap (the off-screen buffer)
    if (wt->pixmap) {
        XFreePixmap(wm->dpy, wt->pixmap);
        wt->pixmap = 0;
    }

    // Destroy the XDamage tracker for this window
    if (wt->damage) {
        XDamageDestroy(wm->dpy, wt->damage);
        wt->damage = 0;
    }
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Window lifecycle (map/unmap/damage)
// ────────────────────────────────────────────────────────────────────────

void mr_window_mapped(CCWM *wm, Client *c)
{
    if (!mr.active || !wm || !c || !c->frame) return;

    // Don't add duplicates — check if we're already tracking this window
    if (find_window_texture(c->frame)) return;

    // Make sure we haven't hit the window limit
    if (mr.window_count >= MAX_CLIENTS) {
        fprintf(stderr, "[moonrock] WARNING: Maximum window count reached, "
                "cannot track window 0x%lx\n", c->frame);
        return;
    }

    // Get the window's visual to determine if it has an alpha channel.
    // Windows created with our ARGB visual will have depth 32.
    XWindowAttributes wa;
    if (!XGetWindowAttributes(wm->dpy, c->frame, &wa)) {
        fprintf(stderr, "[moonrock] WARNING: Cannot get attributes for 0x%lx\n",
                c->frame);
        return;
    }

    // Create a new tracking entry for this window
    struct WindowTexture *wt = &mr.windows[mr.window_count];
    memset(wt, 0, sizeof(*wt));

    wt->xwin = c->frame;
    wt->has_alpha = (wa.depth == 32);

    // Calculate the window's on-screen geometry.
    // The frame position (c->x, c->y) is in root window coordinates. If the
    // compositor is active, the frame includes shadow padding, so the actual
    // screen position needs to account for that offset.
    if (compositor_active) {
        wt->x = c->x - SHADOW_LEFT;
        wt->y = c->y - SHADOW_TOP;
        wt->w = wa.width;
        wt->h = wa.height;
    } else {
        wt->x = c->x;
        wt->y = c->y;
        wt->w = wa.width;
        wt->h = wa.height;
    }

    // Generate an OpenGL texture ID for this window.
    // glGenTextures creates "names" (integer IDs) for textures on the GPU.
    // The texture doesn't have any data yet — it's just a handle.
    glGenTextures(1, &wt->texture);

    // Mark the texture as dirty so it gets filled with actual window content
    // on the next composite pass.
    wt->dirty = true;

    // Register an XDamage monitor on the frame window.
    // XDamageReportNonEmpty means "notify me whenever the damage region becomes
    // non-empty" — i.e., as soon as any pixel changes, we get notified.
    // We then acknowledge the damage and refresh the texture.
    wt->damage = XDamageCreate(wm->dpy, c->frame, XDamageReportNonEmpty);

    mr.window_count++;

    if (getenv("AURA_DEBUG")) {
        fprintf(stderr, "[moonrock] Mapped window 0x%lx (%dx%d at %d,%d, "
                "alpha=%d)\n", c->frame, wt->w, wt->h, wt->x, wt->y,
                wt->has_alpha);
    }
}

void mr_window_unmapped(CCWM *wm, Client *c)
{
    if (!mr.active || !wm || !c) return;

    // Find the window in our tracking array
    for (int i = 0; i < mr.window_count; i++) {
        if (mr.windows[i].xwin == c->frame) {
            // Release all GPU and X resources for this window
            release_window_texture(wm, &mr.windows[i]);

            // Remove from the array by shifting everything after it left.
            // This maintains the z-order (array order = stacking order).
            int remaining = mr.window_count - i - 1;
            if (remaining > 0) {
                memmove(&mr.windows[i], &mr.windows[i + 1],
                        remaining * sizeof(struct WindowTexture));
            }
            mr.window_count--;

            if (getenv("AURA_DEBUG")) {
                fprintf(stderr, "[moonrock] Unmapped window 0x%lx\n", c->frame);
            }
            return;
        }
    }
}

void mr_window_damaged(CCWM *wm, Client *c)
{
    if (!mr.active || !wm || !c) return;

    // Try the frame window first (that's what we track), then the client window
    struct WindowTexture *wt = find_window_texture(c->frame);
    if (!wt) return;

    // Mark the texture as dirty so it gets refreshed on the next frame
    wt->dirty = true;

    // Acknowledge the damage — tell the X server "I've seen this change."
    // Without this, the server keeps re-sending the same damage notification.
    // Passing None for both regions means "acknowledge all damage."
    if (wt->damage) {
        XDamageSubtract(wm->dpy, wt->damage, None, None);
    }
}

// Called when a window is resized — update our tracking info and refresh
// the texture since the old pixmap is now invalid.
void mr_window_resized(CCWM *wm, Client *c)
{
    if (!mr.active || !wm || !c) return;

    struct WindowTexture *wt = find_window_texture(c->frame);
    if (!wt) return;

    // Get the updated geometry
    XWindowAttributes wa;
    if (!XGetWindowAttributes(wm->dpy, c->frame, &wa)) return;

    // Update position (accounting for shadow padding)
    if (compositor_active) {
        wt->x = c->x - SHADOW_LEFT;
        wt->y = c->y - SHADOW_TOP;
    } else {
        wt->x = c->x;
        wt->y = c->y;
    }

    // If the size changed, we need to refresh the texture because the old
    // pixmap was for the previous size.
    bool size_changed = (wt->w != wa.width || wt->h != wa.height);
    wt->w = wa.width;
    wt->h = wa.height;

    if (size_changed) {
        wt->dirty = true;

        // Invalidate the cached shadow texture so it gets regenerated at
        // the new size on the next composite pass.
        if (wt->shadow_tex) {
            glDeleteTextures(1, &wt->shadow_tex);
            wt->shadow_tex = 0;
            wt->shadow_tex_w = 0;
            wt->shadow_tex_h = 0;
        }
    }
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Auto-discovery of visible windows via XQueryTree
// ────────────────────────────────────────────────────────────────────────
//
// MoonRock needs to composite ALL visible windows, including unmanaged ones
// (dock, desktop, menubar, spotlight) that the WM does not frame and does
// not create Client structs for. Instead of requiring every window lifecycle
// path to call mr_window_mapped(), we use XQueryTree each frame to
// discover all children of the root window and auto-track any new ones.
//
// This also handles cleanup: if a tracked window is no longer a child of
// root (or is no longer viewable), we remove its tracking entry.

static void sync_tracked_windows(CCWM *wm)
{
    Window root_ret, parent_ret;
    Window *children = NULL;
    unsigned int nchildren = 0;

    // XQueryTree returns all immediate children of the root window in
    // bottom-to-top stacking order — exactly the order we want for
    // back-to-front compositing.
    if (!XQueryTree(wm->dpy, wm->root, &root_ret, &parent_ret,
                    &children, &nchildren)) {
        return;
    }

    // ── Pass 1: Mark all currently tracked windows as "not seen" ──
    // We use a temporary flag by checking each tracked window against
    // the XQueryTree results. Any tracked window not found in the list
    // has been destroyed or reparented away and should be removed.
    bool seen[MAX_CLIENTS];
    memset(seen, 0, sizeof(seen));

    // ── Pass 2: For each visible root child, ensure it is tracked ──
    for (unsigned int i = 0; i < nchildren; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(wm->dpy, children[i], &wa)) continue;

        // Skip windows that are not currently visible on screen
        if (wa.map_state != IsViewable) continue;

        // Skip zero-size windows
        if (wa.width <= 0 || wa.height <= 0) continue;

        // Check if we already track this window
        struct WindowTexture *wt = find_window_texture(children[i]);
        if (wt) {
            // Already tracked — update its position and size in case it moved.
            // This keeps our rendering in sync without needing ConfigureNotify
            // for every window (especially unmanaged ones).
            wt->x = wa.x;
            wt->y = wa.y;
            if (wt->w != wa.width || wt->h != wa.height) {
                // Size changed — need to re-bind the texture
                wt->w = wa.width;
                wt->h = wa.height;
                wt->dirty = true;
            }

            // Mark this entry as still alive
            int idx = (int)(wt - mr.windows);
            if (idx >= 0 && idx < mr.window_count) {
                seen[idx] = true;
            }
            continue;
        }

        // New window — add a tracking entry if we have room
        if (mr.window_count >= MAX_CLIENTS) continue;

        wt = &mr.windows[mr.window_count];
        memset(wt, 0, sizeof(*wt));

        wt->xwin = children[i];
        wt->has_alpha = (wa.depth == 32);
        wt->override_redirect = wa.override_redirect ? true : false;
        wt->x = wa.x;
        wt->y = wa.y;
        wt->w = wa.width;
        wt->h = wa.height;

        // Create an OpenGL texture handle for this window
        glGenTextures(1, &wt->texture);
        wt->dirty = true;

        if (wa.override_redirect) {
            // Override-redirect windows (tooltips, menus, popups) are read
            // directly from the window drawable rather than via XComposite
            // pixmaps. We do NOT redirect them through XComposite and do NOT
            // create XDamage tracking — instead, they are always marked dirty
            // so their texture is refreshed every frame. This is reliable
            // across all X servers and these windows are small enough that
            // the per-frame XGetImage cost is negligible.
            wt->damage = 0;
        } else {
            // Normal windows: redirect through XComposite and track damage
            // so we only refresh the texture when contents actually change.
            wt->damage = XDamageCreate(wm->dpy, children[i], XDamageReportNonEmpty);
        }

        // Mark this new entry as seen
        seen[mr.window_count] = true;
        mr.window_count++;

        if (getenv("AURA_DEBUG")) {
            fprintf(stderr, "[moonrock] Auto-tracked window 0x%lx (%dx%d at %d,%d%s)\n",
                    children[i], wa.width, wa.height, wa.x, wa.y,
                    wa.override_redirect ? " override-redirect" : "");
        }
    }

    // ── Pass 3: Remove tracked windows that are no longer visible ──
    // Iterate backwards so removals don't shift indices we haven't checked yet.
    for (int i = mr.window_count - 1; i >= 0; i--) {
        if (!seen[i]) {
            if (getenv("AURA_DEBUG")) {
                fprintf(stderr, "[moonrock] Removing stale window 0x%lx\n",
                        mr.windows[i].xwin);
            }
            // If this was an override-redirect window, unredirect it since
            // we explicitly redirected it in the tracking pass above.
            // This is safe even if the window is already destroyed — X11
            // silently ignores the call for non-existent windows.
            if (mr.windows[i].override_redirect) {
                XCompositeUnredirectWindow(wm->dpy, mr.windows[i].xwin,
                                           CompositeRedirectManual);
            }

            release_window_texture(wm, &mr.windows[i]);

            int remaining = mr.window_count - i - 1;
            if (remaining > 0) {
                memmove(&mr.windows[i], &mr.windows[i + 1],
                        remaining * sizeof(struct WindowTexture));
            }
            mr.window_count--;
        }
    }

    // ── Pass 4: Reorder tracked windows to match XQueryTree stacking ──
    // XQueryTree returns children in bottom-to-top order, which is exactly
    // the order we need for back-to-front compositing. Rebuild the array
    // to match this stacking order so windows overlap correctly.
    if (mr.window_count > 1) {
        struct WindowTexture reordered[MAX_CLIENTS];
        int count = 0;

        for (unsigned int i = 0; i < nchildren && count < mr.window_count; i++) {
            struct WindowTexture *wt = find_window_texture(children[i]);
            if (wt) {
                reordered[count++] = *wt;
            }
        }

        // Copy the reordered array back
        if (count == mr.window_count) {
            memcpy(mr.windows, reordered,
                   count * sizeof(struct WindowTexture));
        }
    }

    if (children) XFree(children);
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Shadow rendering (Phase 2 — Gaussian blur via box blur)
// ────────────────────────────────────────────────────────────────────────
//
// Phase 2 replaces the old concentric-rectangle shadows with a real
// Gaussian blur, producing the soft, diffuse shadows seen in Snow Leopard.
//
// How it works:
//   A true Gaussian blur is expensive (O(n^2) per pixel for radius n).
//   However, a well-known trick in graphics is that applying a box blur
//   (simple averaging filter) three times in succession closely approximates
//   a Gaussian blur. Each box blur pass is O(1) per pixel when implemented
//   as a sliding window (running sum).
//
//   The algorithm:
//     1. Create a grayscale buffer the size of the window plus padding.
//     2. Fill in a solid white rectangle where the window sits.
//     3. Run 3 passes of separable box blur (horizontal, then vertical).
//        "Separable" means we blur in one direction at a time — this turns
//        a 2D blur into two 1D blurs, which is much faster.
//     4. The resulting buffer is a smooth alpha mask — bright in the center
//        (under the window) and fading smoothly to zero at the edges.
//     5. Upload the mask as an OpenGL texture and draw it as a semi-
//        transparent black quad behind the window.
//
//   The shadow texture is cached per-window and only regenerated when the
//   window is resized, so the CPU cost is near-zero during normal operation.

// ── generate_shadow_texture ──
// Builds the blurred shadow alpha mask on the CPU and uploads it to an
// OpenGL texture. Called once when a window is first composited and again
// whenever the window is resized.
//
// Parameters:
//   wt      — the window to generate a shadow for
//   focused — whether the window is focused (focused windows get larger,
//             darker shadows to make them "pop" visually)

static void generate_shadow_texture(struct WindowTexture *wt, bool focused)
{
    if (!wt || wt->w <= 0 || wt->h <= 0) return;

    // ── Shadow parameters ──
    // radius:     how far the shadow extends from the window edge, in
    //             physical pixels. Focused windows get a larger radius.
    //             SHADOW_RADIUS / SHADOW_RADIUS_INACTIVE are in points,
    //             so we multiply by the backing scale of the output
    //             hosting this window — a 20 pt shadow is 20 px at 1.0×
    //             and 35 px at 1.75× (Legion panel), keeping the shadow's
    //             visual weight constant across densities.
    // peak_alpha: maximum opacity of the shadow directly under the window.
    //             Focused = darker (0.45), unfocused = lighter (0.22).
    // pad:        extra pixels around the window rect to give the blur room
    //             to fade out. We use 2x the radius to avoid clipping.

    // The chrome is the visible window area (excluding shadow padding in the
    // frame). The shadow is generated around this chrome rectangle.
    int chrome_w = wt->w - SHADOW_LEFT - SHADOW_RIGHT;
    int chrome_h = wt->h - SHADOW_TOP - SHADOW_BOTTOM;
    if (chrome_w <= 0 || chrome_h <= 0) return;

    // Query the scale at the chrome's center so a window that straddles
    // two outputs keys off the one it's mostly on. A 1× fallback is baked
    // into display_scale_at_point for the hotplug-transient gap case.
    float scale = display_scale_at_point(wt->x + SHADOW_LEFT + chrome_w / 2,
                                         wt->y + SHADOW_TOP  + chrome_h / 2);
    if (scale < 0.5f) scale = 1.0f;
    wt->shadow_scale = scale;

    int radius_pts = focused ? SHADOW_RADIUS : SHADOW_RADIUS_INACTIVE;
    int radius = (int)((float)radius_pts * scale + 0.5f);
    if (radius < 1) radius = 1;
    float peak_alpha = focused ? (float)SHADOW_ALPHA_ACTIVE
                               : (float)SHADOW_ALPHA_INACTIVE;

    int pad = radius * 2;
    int shadow_w = chrome_w + pad * 2;
    int shadow_h = chrome_h + pad * 2;

    // ── Step 1: Create the shadow shape ──
    // Allocate a single-channel (grayscale) buffer. Start with all zeros
    // (fully transparent). calloc zero-initializes memory for us.
    unsigned char *shadow = calloc((size_t)shadow_w * shadow_h, 1);
    if (!shadow) return;

    // Fill in a solid white rectangle where the chrome would be. This is
    // the "seed" shape that the blur will spread outward from.
    // The rectangle is centered in the buffer (offset by 'pad' on all sides).
    for (int y = pad; y < pad + chrome_h; y++) {
        // memset is faster than a pixel-by-pixel loop for filling a row
        memset(&shadow[y * shadow_w + pad], 255, (size_t)chrome_w);
    }

    // ── Step 2: Three-pass separable box blur ──
    //
    // A box blur averages all pixels within a fixed radius of each pixel.
    // By itself, a box blur looks blocky (square edges). But repeating it
    // 3 times produces a smooth bell-curve falloff that closely matches a
    // true Gaussian distribution. This is a well-known approximation used
    // in real compositors (e.g., CSS blur, Photoshop Gaussian blur).
    //
    // Each pass uses radius/3 — three passes of radius R/3 approximate one
    // Gaussian blur of radius R. We cap the minimum at 1 to avoid a no-op.
    int blur_radius = radius / 3;
    if (blur_radius < 1) blur_radius = 1;

    // The "kernel width" is the number of pixels in the averaging window:
    // from -blur_radius to +blur_radius inclusive = (2*blur_radius + 1).
    int kernel_width = blur_radius * 2 + 1;

    // Temporary buffer for intermediate blur results. We ping-pong between
    // shadow[] and temp[] across the horizontal and vertical passes.
    unsigned char *temp = calloc((size_t)shadow_w * shadow_h, 1);
    if (!temp) { free(shadow); return; }

    for (int pass = 0; pass < 3; pass++) {
        // ── Horizontal blur pass ──
        // For each row, slide a window of width kernel_width across the
        // row, keeping a running sum. This makes each pixel the average
        // of its horizontal neighbors — O(1) per pixel regardless of radius.
        for (int y = 0; y < shadow_h; y++) {
            // Build the initial sum for the window centered at x=0.
            // Pixels outside the buffer are handled by clamping to the edge
            // (this is equivalent to "extend edge" boundary mode).
            int sum = 0;
            for (int k = -blur_radius; k <= blur_radius; k++) {
                // Clamp: if the index would go out of bounds, use the
                // nearest valid pixel instead
                int sx = k < 0 ? 0 : (k >= shadow_w ? shadow_w - 1 : k);
                sum += shadow[y * shadow_w + sx];
            }
            temp[y * shadow_w + 0] = (unsigned char)(sum / kernel_width);

            // Slide the window one pixel to the right for each subsequent x.
            // Add the new pixel entering on the right, remove the old pixel
            // leaving on the left. This keeps the sum accurate in O(1) time.
            for (int x = 1; x < shadow_w; x++) {
                int add_x = x + blur_radius;
                if (add_x >= shadow_w) add_x = shadow_w - 1;
                sum += shadow[y * shadow_w + add_x];

                int rem_x = x - blur_radius - 1;
                if (rem_x < 0) rem_x = 0;
                sum -= shadow[y * shadow_w + rem_x];

                temp[y * shadow_w + x] = (unsigned char)(sum / kernel_width);
            }
        }

        // ── Vertical blur pass ──
        // Same sliding-window technique, but operating on columns instead
        // of rows. Reads from temp[] (horizontal result) into shadow[].
        for (int x = 0; x < shadow_w; x++) {
            int sum = 0;
            for (int k = -blur_radius; k <= blur_radius; k++) {
                int sy = k < 0 ? 0 : (k >= shadow_h ? shadow_h - 1 : k);
                sum += temp[sy * shadow_w + x];
            }
            shadow[0 * shadow_w + x] = (unsigned char)(sum / kernel_width);

            for (int y = 1; y < shadow_h; y++) {
                int add_y = y + blur_radius;
                if (add_y >= shadow_h) add_y = shadow_h - 1;
                sum += temp[add_y * shadow_w + x];

                int rem_y = y - blur_radius - 1;
                if (rem_y < 0) rem_y = 0;
                sum -= temp[rem_y * shadow_w + x];

                shadow[y * shadow_w + x] = (unsigned char)(sum / kernel_width);
            }
        }
    }
    free(temp);

    // ── Step 3: Upload as an OpenGL texture ──
    //
    // The blur result is a single-channel alpha mask (0 = transparent,
    // 255 = fully opaque). We need to convert it to RGBA for OpenGL.
    // The RGB channels are all 0 (black), and the alpha channel carries
    // the blurred shadow value scaled by peak_alpha.

    // Delete old shadow texture if it exists (e.g., regenerating after resize)
    if (wt->shadow_tex) {
        glDeleteTextures(1, &wt->shadow_tex);
        wt->shadow_tex = 0;
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    // GL_LINEAR filtering smooths the texture when it's drawn slightly
    // scaled, preventing any remaining pixelation at the shadow edges.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Clamp to edge so the shadow doesn't wrap around at the borders
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Build the RGBA pixel buffer. Each pixel is (0, 0, 0, blurred_alpha).
    unsigned char *rgba = calloc((size_t)shadow_w * shadow_h * 4, 1);
    if (!rgba) { free(shadow); glDeleteTextures(1, &tex); return; }

    for (int i = 0; i < shadow_w * shadow_h; i++) {
        // Scale the blur value by peak_alpha to get the final shadow opacity.
        // A pixel directly under the window will be ~255 after blurring, so
        // its alpha becomes peak_alpha * 255. Edge pixels fade toward zero.
        float a = (shadow[i] / 255.0f) * peak_alpha;

        // Pre-multiply the alpha into the color channels. Our blend mode
        // (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) expects pre-multiplied alpha:
        //   final = src_color * 1 + dst_color * (1 - src_alpha)
        // For a black shadow (R=G=B=0), pre-multiplied RGB stays 0.
        unsigned char alpha_byte = (unsigned char)(a * 255.0f);
        rgba[i * 4 + 0] = 0;           // R — black
        rgba[i * 4 + 1] = 0;           // G — black
        rgba[i * 4 + 2] = 0;           // B — black
        rgba[i * 4 + 3] = alpha_byte;  // A — the blurred shadow
    }

    // Upload the RGBA data to the GPU. After this call, the texture lives
    // in video memory and we can free the CPU-side buffers.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, shadow_w, shadow_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    free(rgba);
    free(shadow);

    // Unbind texture to leave GL state clean
    glBindTexture(GL_TEXTURE_2D, 0);

    // Cache the texture handle and dimensions so we can reuse it every
    // frame without recomputing. The shadow only needs regeneration when
    // the window is resized (handled in mr_window_resized).
    wt->shadow_tex = tex;
    wt->shadow_tex_w = shadow_w;
    wt->shadow_tex_h = shadow_h;
}

// ── draw_window_shadow ──
// Draws the pre-computed Gaussian blur shadow behind a window. If the
// shadow texture hasn't been generated yet (first frame) or the window
// was resized, it regenerates the texture first.
//
// The shadow quad is positioned behind the window chrome with a slight
// downward offset (SHADOW_Y_OFFSET) to simulate a light source above.

static void draw_window_shadow(struct WindowTexture *wt, bool focused)
{
    if (!wt || wt->w <= 0 || wt->h <= 0) return;

    // Calculate chrome dimensions (the visible window area inside the frame)
    int chrome_w = wt->w - SHADOW_LEFT - SHADOW_RIGHT;
    int chrome_h = wt->h - SHADOW_TOP - SHADOW_BOTTOM;
    if (chrome_w <= 0 || chrome_h <= 0) return;

    // Scale at the chrome center — same point the generator samples. The
    // chrome rect is frame-pixel coordinates, so SHADOW_LEFT/TOP are
    // unchanged here even at HiDPI.
    float scale = display_scale_at_point(wt->x + SHADOW_LEFT + chrome_w / 2,
                                         wt->y + SHADOW_TOP  + chrome_h / 2);
    if (scale < 0.5f) scale = 1.0f;

    int radius_pts = focused ? SHADOW_RADIUS : SHADOW_RADIUS_INACTIVE;
    int radius = (int)((float)radius_pts * scale + 0.5f);
    if (radius < 1) radius = 1;
    int pad = radius * 2;
    int expected_w = chrome_w + pad * 2;
    int expected_h = chrome_h + pad * 2;

    // Generate or regenerate the shadow texture if needed.
    // Triggers: first draw (shadow_tex == 0), window resized (dims
    // changed), or the window migrated to an output of a different scale
    // (cached blur is now the wrong density). Epsilon compare guards
    // against float drift from repeated multiply/divide paths.
    if (!wt->shadow_tex ||
        wt->shadow_tex_w != expected_w ||
        wt->shadow_tex_h != expected_h ||
        fabsf(wt->shadow_scale - scale) > 0.01f) {
        generate_shadow_texture(wt, focused);
    }

    // If generation failed (out of memory), bail silently
    if (!wt->shadow_tex) return;

    // ── Draw the shadow quad ──
    // The shadow texture covers the chrome area plus 'pad' pixels on all
    // sides. Position it so the chrome-sized center aligns with the actual
    // window chrome on screen.
    //
    // chrome_x/chrome_y = top-left of the chrome area in screen coordinates.
    // The shadow quad starts 'pad' pixels above-left of the chrome, with
    // an additional y_offset downward to simulate a top-down light. The
    // y_offset scales with backing density so the "light from above"
    // reads the same at 1.0× and 1.75×.
    int chrome_x = wt->x + SHADOW_LEFT;
    int chrome_y = wt->y + SHADOW_TOP;
    int y_offset_px = (int)((float)SHADOW_Y_OFFSET * scale + 0.5f);
    if (y_offset_px < 1) y_offset_px = 1;
    float sx = (float)(chrome_x - pad);
    float sy = (float)(chrome_y - pad + y_offset_px);

    // ── Draw the shadow quad using the shader pipeline ──
    // Activate the shadow shader which reads the alpha channel from the
    // shadow texture and outputs premultiplied black with that alpha.
    // The u_alpha uniform is set to 1.0 because shadow intensity is
    // already baked into the cached texture.
    if (mr.shaders.shadow) {
        shaders_use(mr.shaders.shadow);
        shaders_set_projection(mr.shaders.shadow, mr.projection);
        shaders_set_texture(mr.shaders.shadow, 0);
        shaders_set_alpha(mr.shaders.shadow, 1.0f);
        glBindTexture(GL_TEXTURE_2D, wt->shadow_tex);
        shaders_draw_quad(sx, sy,
                          (float)wt->shadow_tex_w, (float)wt->shadow_tex_h);
    } else {
        // Fallback: fixed-function GL if shadow shader failed to compile
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, wt->shadow_tex);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 0.0f);
            glVertex2f(sx, sy);
            glTexCoord2f(1.0f, 0.0f);
            glVertex2f(sx + (float)wt->shadow_tex_w, sy);
            glTexCoord2f(1.0f, 1.0f);
            glVertex2f(sx + (float)wt->shadow_tex_w, sy + (float)wt->shadow_tex_h);
            glTexCoord2f(0.0f, 1.0f);
            glVertex2f(sx, sy + (float)wt->shadow_tex_h);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    }
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Main compositing pass
// ────────────────────────────────────────────────────────────────────────
//
// This is the heart of MoonRock — called every frame from the event loop.
// It draws all visible windows to the screen using OpenGL.
//
// The rendering process is:
//   1. Clear the screen to the background color
//   2. Set up a 2D projection matching pixel coordinates
//   3. For each window (back-to-front):
//      a. Draw its shadow behind it
//      b. Bind its texture (the window's contents)
//      c. Draw a textured quad at the window's screen position
//   4. Swap buffers to show the frame

void mr_composite(CCWM *wm)
{
    if (!mr.active || !wm) return;

    // ── Step 0: Check for direct scanout bypass ──
    // If a single fullscreen opaque window covers the entire display (e.g.,
    // a game), we can skip compositing entirely and let the X server present
    // the window's buffer directly. This eliminates compositor latency and
    // saves GPU power — critical for gaming on the Legion Go S.
    if (display_check_direct_scanout(wm)) {
        return;  // Game is presenting directly, nothing for us to do
    }

    // Make our GL context current. This is technically redundant if we're
    // the only GL user, but it's good practice — other code might have
    // changed the current context.
    glXMakeContextCurrent(wm->dpy, mr.gl_window, mr.gl_window,
                          mr.gl_context);

    // ── Step 1: Clear the screen ──
    glClear(GL_COLOR_BUFFER_BIT);

    // ── Step 2: Update the projection matrix ──
    // Rebuild the orthographic projection each frame in case the screen was
    // resized. This maps pixel coordinates to OpenGL clip space:
    //   (0,0) = top-left, (root_w, root_h) = bottom-right.
    shaders_ortho(mr.projection,
                  0, (float)mr.root_w,
                  (float)mr.root_h, 0,
                  -1.0f, 1.0f);

    // ── Step 3: Enable blending for transparent windows ──
    // Premultiplied alpha: src * 1 + dst * (1 - src_alpha)
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // ── Step 3.5: Sync tracked windows with actual X11 state ──
    //
    // Use XQueryTree to discover ALL visible windows (including unmanaged
    // ones like docks, desktops, and menubars) and ensure they have
    // texture tracking entries. This also removes entries for windows
    // that have disappeared and reorders entries to match stacking order.
    sync_tracked_windows(wm);

    // ── Step 4: Draw all windows back-to-front ──
    //
    // We use a 3-pass approach to respect the natural z-ordering of different
    // window types:
    //   Pass 0: Desktop-type windows (_NET_WM_WINDOW_TYPE_DESKTOP)
    //   Pass 1: Normal application windows
    //   Pass 2: Dock/panel windows (_NET_WM_WINDOW_TYPE_DOCK)
    //
    // This ensures the desktop is always at the bottom, panels are always on
    // top, and normal windows are sandwiched between them.

    for (int pass = 0; pass < 3; pass++) {
        // ── MoonBase surfaces ──
        // MoonBase windows are compositor-internal surfaces (not X
        // windows), so they don't appear in mr.windows[]. We render
        // them alongside normal X application windows — between the
        // desktop pass and the dock/panel pass — so that Aqua chrome
        // drawn by moonrock still lands above them in slice 3c.2b and
        // panels stay on top.
        if (pass == 1) {
            mb_host_render(mr.shaders.basic, mr.projection);
        }

        for (int i = 0; i < mr.window_count; i++) {
            struct WindowTexture *wt = &mr.windows[i];

            // Determine which pass this window belongs to by checking
            // the _NET_WM_WINDOW_TYPE property. Dock/panel windows go in
            // pass 2 (always on top), desktop windows in pass 0 (always
            // behind), and everything else in pass 1.
            //
            // Override-redirect windows (menus, tooltips, popups) don't set
            // _NET_WM_WINDOW_TYPE, so we check the override_redirect flag
            // directly. These always render in pass 2 (on top of everything)
            // because they are transient UI elements that must be visible
            // above all other content.
            int window_pass = 1;
            if (wt->override_redirect) {
                window_pass = 2;
            } else {
                Atom wtype = ewmh_get_window_type(wm, wt->xwin);
                if (wtype == wm->atom_net_wm_type_desktop) {
                    window_pass = 0;
                } else if (wtype == wm->atom_net_wm_type_dock) {
                    window_pass = 2;
                }
            }

            // Skip windows that don't belong to this pass
            if (window_pass != pass) continue;

            // Skip windows that are currently being animated out (genie minimize).
            // During the animation, anim_draw() renders this window as a deformed
            // genie shape. If we drew it here too, it would appear twice each frame:
            // once in its normal position, once as the animated genie effect.
            if (wt->animating_out) continue;

            // Skip zero-size windows (shouldn't happen, but be defensive)
            if (wt->w <= 0 || wt->h <= 0) continue;

            // ── Refresh dirty textures ──
            // If the window's contents have changed since the last frame,
            // update the OpenGL texture with the new pixel data.
            //
            // Override-redirect windows (menus, tooltips) and dock/panel
            // windows (pass 2) are always refreshed because:
            //   - Override-redirect windows don't use XDamage tracking
            //   - Dock/panel windows may lose their XDamage tracker when
            //     killed and restarted (the new window gets a fresh ID but
            //     damage events may not be delivered reliably). These are
            //     small enough that per-frame refresh is negligible.
            if (wt->dirty || wt->override_redirect || window_pass == 2) {
                refresh_window_texture(wm, wt);
                wt->dirty = false;
            }

            // ── Draw the shadow ──
            // Shadows are drawn BEHIND the window. Only framed application
            // windows (pass 1) get shadows — dock, desktop, and menubar
            // windows are unframed and have no shadow padding baked into
            // their geometry, so drawing shadows on them would look wrong.
            if (window_pass == 1) {
                Client *c = wm_find_client_by_frame(wm, wt->xwin);
                bool focused = c ? c->focused : false;
                draw_window_shadow(wt, focused);
            }

            // ── Apply blur-behind for translucent panels ──
            // Dock and panel windows (pass 2) can have frosted glass blur.
            //
            // The technique:
            //   1. Capture the current framebuffer contents at the panel's
            //      screen region into a temporary GL texture. This texture
            //      contains everything rendered so far (desktop + normal
            //      windows) — exactly what should appear blurred behind
            //      the panel.
            //   2. Pass that texture to plugin_effect_blur, which runs a
            //      two-pass Gaussian blur and draws the result back to the
            //      screen at the same position.
            //   3. Delete the temporary texture — it is only needed this frame.
            //   4. The panel's own window texture is then drawn on top,
            //      creating the frosted glass look.
            //
            // The key fix here: we used to pass texture ID 0 (the GL default
            // "no texture" state), which sampled nothing. Now we capture the
            // real scene content first.
            if (window_pass == 2 && mr.shaders.blur_h && mr.shaders.blur_v) {
                ThemeDefinition *theme = plugin_get_theme();
                float blur_radius = theme ? theme->blur_behind_radius : 0.0f;
                // Only blur narrow panel windows (menubar ~46px).
                // The dock window is ~160px tall — its glass shelf look comes
                // from the scurve-xl-opaque.png texture, not blur-behind.
                // Blurring the whole dock rectangle creates a dark haze over
                // the transparent icon area above the shelf.
                if (blur_radius > 0.0f && wt->h > 80) blur_radius = 0.0f;
                if (blur_radius > 0.0f) {
                    // Capture the framebuffer region behind this panel.
                    // glCopyTexImage2D reads from the current read buffer
                    // (the default framebuffer here) into a new texture.
                    //
                    // GL coordinate origin is bottom-left, screen coordinates
                    // are top-left, so we flip the Y: gl_y = root_h - y - h.
                    GLuint capture_tex = 0;
                    glGenTextures(1, &capture_tex);
                    glBindTexture(GL_TEXTURE_2D, capture_tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    int gl_y = mr.root_h - wt->y - wt->h;
                    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                                     wt->x, gl_y, wt->w, wt->h, 0);
                    glBindTexture(GL_TEXTURE_2D, 0);

                    plugin_effect_blur(capture_tex, wt->x, wt->y,
                                       wt->w, wt->h, blur_radius);

                    // Done — the blurred result is already on screen.
                    // Free the temporary capture texture.
                    glDeleteTextures(1, &capture_tex);
                }
            }

            // ── Draw the window contents ──
            // Activate the basic shader program, which samples the window's
            // texture and applies alpha blending. Then draw a quad covering
            // the window's screen rectangle using the VBO pipeline.
            if (mr.shaders.basic) {
                shaders_use(mr.shaders.basic);
                shaders_set_projection(mr.shaders.basic, mr.projection);
                shaders_set_texture(mr.shaders.basic, 0);
                shaders_set_alpha(mr.shaders.basic, 1.0f);
                glBindTexture(GL_TEXTURE_2D, wt->texture);
                shaders_draw_quad((float)wt->x, (float)wt->y,
                                  (float)wt->w, (float)wt->h);
            } else {
                // Fallback: fixed-function GL if shaders failed to compile.
                // This keeps the compositor functional even without shaders.
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, wt->texture);
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                glBegin(GL_QUADS);
                    glTexCoord2f(0.0f, 0.0f);
                    glVertex2f((float)wt->x, (float)wt->y);
                    glTexCoord2f(1.0f, 0.0f);
                    glVertex2f((float)(wt->x + wt->w), (float)wt->y);
                    glTexCoord2f(1.0f, 1.0f);
                    glVertex2f((float)(wt->x + wt->w), (float)(wt->y + wt->h));
                    glTexCoord2f(0.0f, 1.0f);
                    glVertex2f((float)wt->x, (float)(wt->y + wt->h));
                glEnd();
                glDisable(GL_TEXTURE_2D);
            }
        }
    }

    // Deactivate shaders before overlay passes
    shaders_use_none();

    // ── Step 5: Animations ──
    // Advance all active animations (genie minimize, fade in/out, zoom) by
    // computing elapsed time and applying easing curves. Then draw them on
    // top of normal windows — animations are overlays that temporarily replace
    // the window's static texture with a moving/fading version.
    anim_update();
    anim_draw(mr.shaders.basic, mr.shaders.genie, mr.projection);

    // ── Step 6: Mission Control overlay ──
    // If Mission Control is active (user triggered the bird's-eye view),
    // update its animation state and draw the tiled window grid + Space
    // thumbnails on top of everything. Mission Control takes over the entire
    // screen when active, so it draws last (except for the buffer swap).
    if (mc_is_active()) {
        mc_update(wm);
        mc_draw(wm, mr.shaders.basic, mr.projection);
    }

    // ── Step 7: Swap buffers ──
    // We've been drawing to the "back buffer" (an invisible off-screen surface).
    // glXSwapBuffers swaps the back buffer with the front buffer (what's on
    // screen), making our new frame visible instantaneously. This is called
    // "double buffering" and prevents flicker — the user never sees a
    // half-drawn frame.
    //
    // If VSync is enabled, this call blocks until the next vertical blank
    // period, limiting us to the monitor's refresh rate (typically 60 FPS).
    glXSwapBuffers(wm->dpy, mr.gl_window);
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Event handling
// ────────────────────────────────────────────────────────────────────────

bool mr_handle_event(CCWM *wm, XEvent *e)
{
    if (!mr.active || !wm || !e) return false;

    // Check if this is an XDamage event.
    // XDamage events have type == (damage_event_base + XDamageNotify).
    // XDamageNotify is 0, so the type is just damage_event_base.
    if (e->type == mr.damage_event_base + XDamageNotify) {
        // Cast the generic XEvent to the damage-specific struct
        XDamageNotifyEvent *dev = (XDamageNotifyEvent *)e;

        // Find which window was damaged
        struct WindowTexture *wt = find_window_texture(dev->drawable);
        if (wt) {
            wt->dirty = true;

            // Acknowledge the damage so X stops re-sending this notification
            XDamageSubtract(wm->dpy, dev->damage, None, None);
        } else {
            // The damaged window might be a client window (not a frame).
            // Try to find the client and damage its frame instead.
            Client *c = wm_find_client(wm, dev->drawable);
            if (c && c->frame) {
                wt = find_window_texture(c->frame);
                if (wt) {
                    wt->dirty = true;
                }
            }
            // Always acknowledge damage, even for unknown windows
            XDamageSubtract(wm->dpy, dev->damage, None, None);
        }

        return true;  // Event was handled
    }

    return false;  // Not a MoonRock event
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Input shape passthrough
// ────────────────────────────────────────────────────────────────────────
//
// Shadow regions should not intercept mouse clicks. We use XFixes input
// shapes to define which parts of the frame window are "clickable."
// Only the chrome area (title bar + borders) responds to clicks; the
// shadow padding lets clicks fall through to windows behind.
//
// This is the same logic as the old compositor_set_input_shape(), kept
// here for consistency since MoonRock owns compositing now.

void mr_set_input_shape(CCWM *wm, Client *c)
{
    if (!wm || !c || !c->frame) return;

    // Calculate the chrome (clickable) area within the frame.
    // The chrome starts at (SHADOW_LEFT, SHADOW_TOP) and covers the
    // title bar, borders, and client content area.
    int chrome_w = c->w + 2 * BORDER_WIDTH;
    int chrome_h = c->h + TITLEBAR_HEIGHT + BORDER_WIDTH;

    // Create an XFixes region that covers just the chrome area
    XRectangle rect;
    rect.x = SHADOW_LEFT;
    rect.y = SHADOW_TOP;
    rect.width = chrome_w;
    rect.height = chrome_h;

    XserverRegion region = XFixesCreateRegion(wm->dpy, &rect, 1);

    // ShapeInput controls which parts of the window receive mouse events.
    // ShapeSet replaces the entire input shape with our rectangle.
    XFixesSetWindowShapeRegion(wm->dpy, c->frame, ShapeInput, 0, 0, region);

    // Clean up — the X server made its own copy of the region
    XFixesDestroyRegion(wm->dpy, region);

    if (getenv("AURA_DEBUG")) {
        fprintf(stderr, "[moonrock] Set input shape for '%s': "
                "clickable at (%d,%d) %dx%d\n",
                c->title, rect.x, rect.y, rect.width, rect.height);
    }
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: ARGB visual access
// ────────────────────────────────────────────────────────────────────────
//
// These functions expose MoonRock's ARGB visual and colormap to the rest of
// the window manager (especially frame.c, which needs them to create frame
// windows with alpha support).
//
// They maintain compatibility with the old compositor's
// compositor_create_argb_visual() interface.

bool mr_create_argb_visual(CCWM *wm, Visual **out_visual,
                                Colormap *out_colormap)
{
    if (!out_visual || !out_colormap) return false;

    // If MoonRock found an ARGB visual during init, return it
    if (mr.argb_visual && mr.argb_colormap) {
        *out_visual = mr.argb_visual;
        *out_colormap = mr.argb_colormap;
        return true;
    }

    // Fall back to searching for a 32-bit ARGB visual manually.
    // This is the same logic that was in the old compositor.c — look for
    // a TrueColor visual with 32-bit depth on the default screen.
    if (!wm || !wm->dpy) return false;

    XVisualInfo templ;
    templ.screen = wm->screen;
    templ.depth = 32;
    templ.class = TrueColor;

    int num_visuals = 0;
    XVisualInfo *visuals = XGetVisualInfo(wm->dpy,
                                          VisualScreenMask | VisualDepthMask |
                                          VisualClassMask,
                                          &templ, &num_visuals);

    if (!visuals || num_visuals == 0) {
        if (visuals) XFree(visuals);
        return false;
    }

    // Use the first matching 32-bit TrueColor visual
    *out_visual = visuals[0].visual;

    // X11 requires a colormap for every visual. AllocNone means we don't
    // need to allocate color cells (TrueColor uses direct RGB encoding).
    *out_colormap = XCreateColormap(wm->dpy, wm->root,
                                    *out_visual, AllocNone);

    XFree(visuals);
    return true;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Status queries
// ────────────────────────────────────────────────────────────────────────

bool mr_is_active(void)
{
    return mr.active;
}

int mr_get_damage_event_base(void)
{
    return mr.damage_event_base;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Screen resize handling
// ────────────────────────────────────────────────────────────────────────

static void mr_screen_resized(CCWM *wm)
{
    if (!mr.active || !wm) return;

    // Update our stored screen dimensions
    mr.root_w = wm->root_w;
    mr.root_h = wm->root_h;

    // Update the GL viewport to match the new screen size.
    // The viewport maps OpenGL's normalized coordinates to pixel coordinates.
    // (0, 0) is the bottom-left corner; (root_w, root_h) is the top-right.
    glViewport(0, 0, mr.root_w, mr.root_h);

    // Rebuild the orthographic projection matrix for the new dimensions
    shaders_ortho(mr.projection,
                  0, (float)mr.root_w,
                  (float)mr.root_h, 0,
                  -1.0f, 1.0f);

    fprintf(stderr, "[moonrock] Screen resized to %dx%d\n",
            mr.root_w, mr.root_h);
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Shutdown
// ────────────────────────────────────────────────────────────────────────

void mr_shutdown(CCWM *wm)
{
    if (!wm || !wm->dpy) return;

    fprintf(stderr, "[moonrock] Shutting down MoonRock Compositor...\n");

    // ── Shut down subsystems in reverse init order ──
    // Reverse order ensures dependencies are respected — subsystems that were
    // initialized last (and may depend on earlier ones) are torn down first.

    // Touch input — releases accelerometer claim, dismisses OSK, clears state
    touch_shutdown();

    // Mission Control — frees Space window lists and thumbnail textures
    mc_shutdown(wm);

    // Animation framework — marks all slots inactive
    anim_shutdown();

    // Plugin system — unloads plugins, closes shared library handles
    plugin_shutdown();

    // Robustness layer — closes log file, flushes buffered data
    robust_shutdown();

    // Color management — clears display info and resets gamma
    color_shutdown();

    // Display management — frees output array and screencast resources
    display_shutdown();

    // Destroy shader programs and VBO before releasing GL context
    shaders_shutdown(&mr.shaders);
    shaders_shutdown_quad_vbo();

    // Release all tracked window textures
    for (int i = 0; i < mr.window_count; i++) {
        release_window_texture(wm, &mr.windows[i]);
    }
    mr.window_count = 0;

    // Undo XComposite redirection — let X go back to drawing windows directly.
    // This is critical for a clean shutdown. If we don't do this and the WM
    // crashes, the screen goes blank because windows are still redirected to
    // off-screen pixmaps that nobody is compositing.
    if (mr.active) {
        XCompositeUnredirectSubwindows(wm->dpy, wm->root,
                                       CompositeRedirectManual);
        fprintf(stderr, "[moonrock] Unredirected subwindows\n");
    }

    // Destroy the GLX context and window.
    // Order matters: release the context first, then destroy the GLX surface,
    // then release the overlay window back to the X server.
    if (mr.gl_context) {
        // Detach the context from all drawables before destroying it.
        // This is required — destroying a context while it is current is
        // undefined behavior in GLX.
        glXMakeContextCurrent(wm->dpy, None, None, NULL);

        if (mr.gl_window) {
            glXDestroyWindow(wm->dpy, mr.gl_window);
            mr.gl_window = 0;
        }

        glXDestroyContext(wm->dpy, mr.gl_context);
        mr.gl_context = NULL;
    }

    // Release the XComposite overlay window back to the X server.
    // Without this, the overlay window stays mapped after we exit and
    // blocks input to all other windows.
    if (mr.overlay_win) {
        XCompositeReleaseOverlayWindow(wm->dpy, wm->root);
        mr.overlay_win = 0;
        fprintf(stderr, "[moonrock] Overlay window released\n");
    }

    // Release the compositor selection by destroying the owner window.
    // When the owner window is destroyed, X automatically clears the
    // selection so other compositors can claim it.
    if (mr.cm_owner_win) {
        XDestroyWindow(wm->dpy, mr.cm_owner_win);
        mr.cm_owner_win = 0;
        fprintf(stderr, "[moonrock] Compositor selection released\n");
    }

    // Free the ARGB colormap (the visual is owned by X, not us)
    if (mr.argb_colormap) {
        XFreeColormap(wm->dpy, mr.argb_colormap);
        mr.argb_colormap = 0;
    }
    mr.argb_visual = NULL;

    // Clear flags
    mr.active = false;
    compositor_active = false;

    fprintf(stderr, "[moonrock] MoonRock Compositor shut down\n");
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Genie minimize animation
// ────────────────────────────────────────────────────────────────────────
//
// The genie minimize effect works in three stages:
//
//   1. mr_animate_minimize() — Called when the user clicks the yellow button.
//      Finds the window's OpenGL texture, sets wt->animating_out = true so
//      the normal composite pass skips it, then starts an ANIM_GENIE_MINIMIZE
//      animation. A completion callback is registered to hide the window after
//      the animation finishes.
//
//   2. anim_draw() — Runs every composite frame while the animation is active.
//      Draws the window texture as a deformed trapezoid that narrows toward
//      the dock icon position, creating the "pouring liquid" genie effect.
//
//   3. on_genie_minimize_done() — Called by the animation system when progress
//      reaches 1.0. Unmaps the window frame (hiding it from the screen),
//      sets c->minimized = true, and frees the callback data.
//
// Restore (un-minimize) is handled by clicking the window's minimized
// representation in the dock — that path calls mr_animate_restore(), which
// re-maps the frame and plays the reverse genie animation.

// Heap-allocated userdata passed to the genie minimize completion callback.
// Contains everything the callback needs to hide the window after animation.
typedef struct {
    CCWM   *wm;   // Window manager — needed for XUnmapWindow and ewmh_update
    Client *c;    // The client being minimized
    Window  frame; // Captured frame window ID (in case c is freed before callback)
} GenieMinimizeData;

// Called by the animation system when the genie minimize animation completes.
// At this point the animation has finished drawing — we now actually hide
// the window by unmapping its frame.
static void on_genie_minimize_done(void *userdata)
{
    GenieMinimizeData *data = (GenieMinimizeData *)userdata;
    if (!data) return;

    CCWM   *wm    = data->wm;
    Client *c     = data->c;
    Window  frame = data->frame;
    free(data);

    // Safety: make sure the client still exists and has the same frame.
    // If the app quit during the animation, c might be dangling — we compare
    // the frame window ID to detect this. If c is gone, we have nothing to do
    // (the window was already cleaned up by on_destroy_notify).
    if (!c || c->frame != frame) {
        fprintf(stderr, "[moonrock] Genie callback: client gone, skipping hide\n");
        return;
    }

    // Unmap the frame window. This hides the window from the screen.
    // The client window (a child of the frame) is automatically unmapped too.
    //
    // on_unmap_notify will receive an UnmapNotify for the client, but checks
    // c->minimized before calling unframe_window — so the frame survives and
    // the window can be restored later.
    c->minimized = true;
    c->mapped    = false;
    XUnmapWindow(wm->dpy, c->frame);

    // Tell the taskbar/dock that the window list changed so they can update
    // their minimized-window indicators.
    ewmh_update_client_list(wm);

    fprintf(stderr, "[moonrock] Genie minimize complete: hid window '%s'\n",
            c->title);
}

void mr_animate_minimize(CCWM *wm, Client *c,
                         int dock_icon_x, int dock_icon_y)
{
    if (!wm || !c || !c->frame) return;

    // Look up this window's OpenGL texture from the compositor's tracked list.
    // The texture was created when the window was mapped — it holds the current
    // rendered content of the window that we will distort into the genie shape.
    struct WindowTexture *wt = find_window_texture(c->frame);
    if (!wt || !wt->texture) {
        // No texture available (compositor may not have processed this window
        // yet). Fall back to immediately hiding the window without animation.
        fprintf(stderr, "[moonrock] Genie minimize: no texture for 0x%lx, "
                "falling back to immediate hide\n", c->frame);
        XUnmapWindow(wm->dpy, c->frame);
        c->mapped    = false;
        c->minimized = true;
        return;
    }

    // Allocate the completion callback data.
    // This struct is passed to on_genie_minimize_done() when the animation
    // finishes — it carries both the WM and the client so we can hide it.
    GenieMinimizeData *data = malloc(sizeof(GenieMinimizeData));
    if (!data) {
        // Out of memory — fall back to immediate hide.
        XUnmapWindow(wm->dpy, c->frame);
        c->mapped    = false;
        c->minimized = true;
        return;
    }
    data->wm    = wm;
    data->c     = c;
    data->frame = c->frame;  // Capture frame ID for stale-check in callback

    // Mark this window as animating out. The normal composite draw loop skips
    // windows with animating_out=true so we don't see the window in both its
    // original position AND as the genie effect simultaneously.
    wt->animating_out = true;

    // Compute source geometry: where the window is right now on screen.
    // wt->x/y/w/h are kept in sync by sync_tracked_windows().
    float src_x = (float)wt->x;
    float src_y = (float)wt->y;
    float src_w = (float)wt->w;
    float src_h = (float)wt->h;

    // Compute destination: the center of the dock icon the window pours into.
    // The destination is a tiny 48x4 strip — fully squished at the dock edge.
    float dst_w = 48.0f;
    float dst_h = 4.0f;
    float dst_x = (float)dock_icon_x - dst_w / 2.0f;
    float dst_y = (float)dock_icon_y - dst_h / 2.0f;

    // Start the genie animation using the raw anim_start() call so we can
    // attach our completion callback. We set userdata to the GenieMinimizeData
    // struct allocated above — the callback will free it when it runs.
    int slot = anim_start(
        ANIM_GENIE_MINIMIZE,
        EASE_IN_QUAD,
        0.5,                         // 0.5 seconds — matches macOS timing
        wt->texture, wt->w, wt->h,
        src_x, src_y, src_w, src_h,  // Source: window's current screen position
        dst_x, dst_y, dst_w, dst_h,  // Destination: dock icon area
        1.0f, 0.7f,                   // Alpha: full to slightly faded (liquid feel)
        data                          // Passed to on_genie_minimize_done()
    );

    if (slot < 0) {
        // All animation slots are full — hide immediately without animation.
        fprintf(stderr, "[moonrock] Genie minimize: no animation slots, "
                "falling back to immediate hide\n");
        free(data);
        wt->animating_out = false;  // Clear since we're not animating
        XUnmapWindow(wm->dpy, c->frame);
        c->mapped    = false;
        c->minimized = true;
        return;
    }

    // Wire up the completion callback. When progress reaches 1.0, the animation
    // system calls on_genie_minimize_done(data) which hides the window.
    anim_set_on_complete(slot, on_genie_minimize_done);

    fprintf(stderr, "[moonrock] Genie minimize started: window '%s' "
            "src=(%.0f,%.0f,%.0f,%.0f) dst=(%.0f,%.0f)\n",
            c->title, src_x, src_y, src_w, src_h,
            (float)dock_icon_x, (float)dock_icon_y);
}

void mr_animate_restore(CCWM *wm, Client *c,
                             int dock_icon_x, int dock_icon_y)
{
    // Phase 5: Genie restore animation (reverse of minimize).
    // The window texture expands from the dock icon back to full size.
    (void)wm;
    (void)c;
    (void)dock_icon_x;
    (void)dock_icon_y;
}

bool mr_animation_active(CCWM *wm)
{
    // Returns true if any animation is currently in progress.
    // The main composite loop uses this to keep rendering at 60Hz while
    // animations are playing (rather than waiting for X events to drive redraws).
    (void)wm;
    return anim_any_active();
}


// ────────────────────────────────────────────────────────────────────────
// SECTION: Shader and texture accessors
// ────────────────────────────────────────────────────────────────────────
//
// These let other MoonRock modules (Mission Control, animations, plugins)
// access the shader programs and window textures without reaching into
// the static mr struct directly.

ShaderPrograms *mr_get_shaders(void)
{
    return &mr.shaders;
}

float *mr_get_projection(void)
{
    return mr.projection;
}

// Look up a window's GL texture handle by its X window ID.
// Returns the texture handle, or 0 if the window isn't tracked.
// This is used by Mission Control to draw window thumbnails.
GLuint mr_get_window_texture_id(Window xwin)
{
    struct WindowTexture *wt = find_window_texture(xwin);
    if (wt && wt->texture) {
        return wt->texture;
    }
    return 0;
}
