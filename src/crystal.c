// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// AuraOS — Crystal Compositor (Phase 1)
//
// Crystal is AuraOS's built-in OpenGL compositor, replacing picom. Instead of
// relying on an external program to composite windows, Crystal handles it
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
// Crystal's approach:
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

#define _GNU_SOURCE  // Needed for M_PI from <math.h>
#include "crystal.h"
#include "decor.h"
#include "ewmh.h"

// Forward declaration removed — compositor.c is no longer linked.
// crystal_create_argb_visual() now handles fallback internally.

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

    // ── Cached Gaussian shadow texture (Phase 2) ──
    // Instead of re-computing the blurred shadow every frame, we cache the
    // result as an OpenGL texture. The shadow only needs to be regenerated
    // when the window is resized. shadow_tex_w/h store the dimensions the
    // shadow was generated for, so we can detect when it's stale.
    GLuint shadow_tex;       // OpenGL texture holding the pre-blurred shadow
    int shadow_tex_w;        // Width the shadow was generated at (includes padding)
    int shadow_tex_h;        // Height the shadow was generated at (includes padding)
};

// ────────────────────────────────────────────────────────────────────────
// SECTION: Module state (file-scope static)
// ────────────────────────────────────────────────────────────────────────
//
// All Crystal state lives in this single struct. Using a static struct at
// file scope means only crystal.c can access it, keeping the compositor's
// internals private from the rest of the window manager.

static struct {
    bool active;                    // Is Crystal initialized and running?

    // ── GLX / OpenGL context ──
    // A "GLX context" is the bridge between X11 and OpenGL. It holds all
    // OpenGL state (current texture, blend mode, etc.) and must be "made
    // current" before any GL calls will work.
    GLXContext gl_context;

    // FBConfig = "Framebuffer Configuration" — describes the pixel format
    // of the rendering surface (color depth, double buffering, alpha, etc.).
    // We need one that supports texture_from_pixmap for binding X pixmaps.
    GLXFBConfig fb_config;

    // The GLX drawable we render to — this is essentially the root window
    // wrapped in a GLX surface so OpenGL can draw to it.
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
} crystal;

// ────────────────────────────────────────────────────────────────────────
// SECTION: Forward declarations (private helper functions)
// ────────────────────────────────────────────────────────────────────────

static void draw_window_shadow(struct WindowTexture *wt, bool focused);
static void generate_shadow_texture(struct WindowTexture *wt, bool focused);
static void refresh_window_texture(AuraWM *wm, struct WindowTexture *wt);
static void refresh_window_texture_fallback(AuraWM *wm, struct WindowTexture *wt);
static void release_window_texture(AuraWM *wm, struct WindowTexture *wt);
static struct WindowTexture *find_window_texture(Window xwin);
static bool choose_fb_configs(Display *dpy, int screen);
static bool load_glx_extensions(Display *dpy, int screen);
static void sync_tracked_windows(AuraWM *wm);

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
        fprintf(stderr, "[crystal] ERROR: No suitable FBConfig for rendering\n");
        if (configs) XFree(configs);
        return false;
    }

    // Take the first matching config — the X server returns them sorted by
    // preference (fewest extra features first, best match at index 0).
    crystal.fb_config = configs[0];
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
        crystal.pixmap_fb_config = configs[0];
        XFree(configs);
    } else {
        fprintf(stderr, "[crystal] WARNING: No FBConfig for RGBA pixmap binding\n");
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
        crystal.pixmap_fb_config_rgb = configs[0];
        XFree(configs);
    } else {
        fprintf(stderr, "[crystal] WARNING: No FBConfig for RGB pixmap binding\n");
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
        fprintf(stderr, "[crystal] WARNING: Cannot query GLX extensions\n");
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
            crystal.has_texture_from_pixmap = true;
            fprintf(stderr, "[crystal] GLX_EXT_texture_from_pixmap available "
                    "(fast path)\n");
        } else {
            fprintf(stderr, "[crystal] WARNING: texture_from_pixmap functions "
                    "not loadable\n");
        }
    } else {
        fprintf(stderr, "[crystal] GLX_EXT_texture_from_pixmap NOT available "
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
            fprintf(stderr, "[crystal] GLX_EXT_swap_control available (VSync)\n");
        }
    }

    return true;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: ARGB visual discovery
// ────────────────────────────────────────────────────────────────────────
//
// This is the same logic as compositor_create_argb_visual() from the old
// compositor, but stored in Crystal's state. We need a 32-bit visual with
// an alpha channel so frame windows can have semi-transparent shadow regions.

static bool find_argb_visual(AuraWM *wm)
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
    crystal.argb_visual = visuals[0].visual;

    // Every visual needs a colormap (X11 requirement). AllocNone means
    // "don't reserve any color cells" — it's just a formality for TrueColor.
    crystal.argb_colormap = XCreateColormap(wm->dpy, wm->root,
                                             crystal.argb_visual, AllocNone);

    XFree(visuals);
    return true;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Initialization
// ────────────────────────────────────────────────────────────────────────

bool crystal_init(AuraWM *wm)
{
    if (!wm || !wm->dpy) return false;

    // Zero out all state so we start clean
    memset(&crystal, 0, sizeof(crystal));

    // Default background color — a dark neutral gray. This is what you see
    // behind all windows if there's no wallpaper.
    crystal.bg_r = 0.15f;
    crystal.bg_g = 0.15f;
    crystal.bg_b = 0.18f;

    fprintf(stderr, "[crystal] Initializing Crystal Compositor...\n");

    // ── Step 1: Check for GLX extension ──
    // GLX is the glue between X11 and OpenGL. Without it, we can't do any
    // GPU-accelerated rendering at all.
    int glx_error_base, glx_event_base;
    if (!glXQueryExtension(wm->dpy, &glx_error_base, &glx_event_base)) {
        fprintf(stderr, "[crystal] ERROR: GLX extension not available. "
                "Cannot initialize OpenGL compositor.\n");
        return false;
    }

    // Check GLX version — we need at least 1.3 for FBConfig support
    int glx_major = 0, glx_minor = 0;
    if (!glXQueryVersion(wm->dpy, &glx_major, &glx_minor)) {
        fprintf(stderr, "[crystal] ERROR: Cannot query GLX version\n");
        return false;
    }
    fprintf(stderr, "[crystal] GLX version: %d.%d\n", glx_major, glx_minor);

    if (glx_major < 1 || (glx_major == 1 && glx_minor < 3)) {
        fprintf(stderr, "[crystal] ERROR: GLX 1.3+ required, got %d.%d\n",
                glx_major, glx_minor);
        return false;
    }

    // ── Step 2: Check for XComposite ──
    // XComposite lets us redirect window rendering to off-screen pixmaps.
    // We need version 0.2+ for XCompositeNameWindowPixmap().
    int composite_major = 0, composite_minor = 0;
    if (!XCompositeQueryVersion(wm->dpy, &composite_major, &composite_minor)) {
        fprintf(stderr, "[crystal] ERROR: XComposite not available\n");
        return false;
    }
    if (composite_major == 0 && composite_minor < 2) {
        fprintf(stderr, "[crystal] ERROR: XComposite 0.2+ required, got %d.%d\n",
                composite_major, composite_minor);
        return false;
    }
    fprintf(stderr, "[crystal] XComposite %d.%d\n", composite_major, composite_minor);

    // ── Step 3: Check for XDamage ──
    // XDamage tells us when a window's contents have changed, so we only
    // refresh textures that actually need updating (not every window every frame).
    int damage_major = 0, damage_minor = 0;
    if (!XDamageQueryVersion(wm->dpy, &damage_major, &damage_minor)) {
        fprintf(stderr, "[crystal] ERROR: XDamage not available\n");
        return false;
    }
    // Store the event base — we need it to identify DamageNotify events
    XDamageQueryExtension(wm->dpy, &crystal.damage_event_base,
                          &crystal.damage_error_base);
    fprintf(stderr, "[crystal] XDamage %d.%d (event base: %d)\n",
            damage_major, damage_minor, crystal.damage_event_base);

    // ── Step 4: Check for XFixes ──
    // XFixes provides input shape manipulation — we use it to make shadow
    // regions click-through (clicks pass to windows behind them).
    int fixes_major = 0, fixes_minor = 0;
    if (!XFixesQueryVersion(wm->dpy, &fixes_major, &fixes_minor)) {
        fprintf(stderr, "[crystal] ERROR: XFixes not available\n");
        return false;
    }
    fprintf(stderr, "[crystal] XFixes %d.%d\n", fixes_major, fixes_minor);

    // ── Step 5: Choose FBConfigs ──
    // FBConfigs describe pixel formats. We need configs for both rendering
    // (the output surface) and pixmap binding (input textures from windows).
    if (!choose_fb_configs(wm->dpy, wm->screen)) {
        fprintf(stderr, "[crystal] ERROR: Cannot find suitable FBConfigs\n");
        return false;
    }
    fprintf(stderr, "[crystal] FBConfigs selected\n");

    // ── Step 6: Create GLX context ──
    // A GLX context holds all OpenGL state. We create it from our rendering
    // FBConfig, then "make it current" so GL calls go to this context.
    // NULL = no shared context (we only have one).
    // True = direct rendering (bypass X server for GL calls — much faster).
    crystal.gl_context = glXCreateNewContext(wm->dpy, crystal.fb_config,
                                             GLX_RGBA_TYPE, NULL, True);
    if (!crystal.gl_context) {
        fprintf(stderr, "[crystal] ERROR: Cannot create GLX context\n");
        return false;
    }
    fprintf(stderr, "[crystal] GLX context created (direct: %s)\n",
            glXIsDirect(wm->dpy, crystal.gl_context) ? "yes" : "no");

    // ── Step 7: Create a GLX window from the root window ──
    // We can't render directly to an X window — we need a GLX wrapper.
    // This creates a GLX drawable backed by the root window, which is where
    // our composited output will appear.
    crystal.gl_window = glXCreateWindow(wm->dpy, crystal.fb_config,
                                         wm->root, NULL);
    if (!crystal.gl_window) {
        fprintf(stderr, "[crystal] ERROR: Cannot create GLX window\n");
        glXDestroyContext(wm->dpy, crystal.gl_context);
        crystal.gl_context = NULL;
        return false;
    }

    // ── Step 8: Make the context current ──
    // "Making current" binds the GLX context to the current thread and the
    // GLX window. All subsequent OpenGL calls will render to this window
    // through this context. Think of it as "activating" the context.
    if (!glXMakeContextCurrent(wm->dpy, crystal.gl_window, crystal.gl_window,
                                crystal.gl_context)) {
        fprintf(stderr, "[crystal] ERROR: Cannot make GLX context current\n");
        glXDestroyWindow(wm->dpy, crystal.gl_window);
        glXDestroyContext(wm->dpy, crystal.gl_context);
        crystal.gl_window = 0;
        crystal.gl_context = NULL;
        return false;
    }

    // ── Step 9: Load GLX extension functions ──
    load_glx_extensions(wm->dpy, wm->screen);

    // ── Step 10: Enable VSync ──
    // Without VSync, the GPU renders as fast as possible and tears appear on
    // screen where one frame ends and the next begins. VSync synchronizes
    // buffer swaps with the monitor's refresh rate.
    if (glXSwapIntervalEXT_func) {
        glXSwapIntervalEXT_func(wm->dpy, crystal.gl_window, 1);
        fprintf(stderr, "[crystal] VSync enabled (swap interval = 1)\n");
    } else {
        fprintf(stderr, "[crystal] WARNING: VSync not available "
                "(may see tearing)\n");
    }

    // ── Step 11: Set up XComposite redirection ──
    // Manual mode: Crystal is responsible for compositing all window contents
    // onto the screen. Windows are redirected to off-screen pixmaps, and
    // crystal_composite() draws them via OpenGL each frame.
    XCompositeRedirectSubwindows(wm->dpy, wm->root, CompositeRedirectManual);
    fprintf(stderr, "[crystal] XComposite redirect set (MANUAL mode — Crystal compositing)\n");

    // ── Step 12: Find ARGB visual ──
    // Needed for frame windows that want semi-transparent shadow regions.
    if (find_argb_visual(wm)) {
        fprintf(stderr, "[crystal] Found 32-bit ARGB visual\n");
    } else {
        fprintf(stderr, "[crystal] WARNING: No 32-bit ARGB visual "
                "(shadows may not render correctly)\n");
    }

    // ── Step 13: Store screen dimensions ──
    crystal.root_w = wm->root_w;
    crystal.root_h = wm->root_h;

    // ── Step 14: Initialize OpenGL state ──
    //
    // OpenGL is a state machine — you set modes (like blending, texturing)
    // and they stay active until you change them. Here we set up the initial
    // state that Crystal needs.

    // Set the "clear color" — the background that shows through when no
    // windows cover a region. This fills the screen before we draw anything.
    glClearColor(crystal.bg_r, crystal.bg_g, crystal.bg_b, 1.0f);

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
    glOrtho(0, crystal.root_w, crystal.root_h, 0, -1, 1);

    // GL_MODELVIEW positions objects in the scene. We start with identity
    // (no transformation) since our coordinates are already in screen space.
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Disable depth testing — we don't need it for 2D compositing. We
    // handle z-order ourselves by drawing back-to-front.
    glDisable(GL_DEPTH_TEST);

    // Mark Crystal as active
    crystal.active = true;

    // Also set the global compositor_active flag so the rest of the WM
    // (frame.c, decor.c) knows compositing is available. This flag controls
    // whether frame windows get ARGB visuals and shadow padding.
    compositor_active = true;

    fprintf(stderr, "[crystal] Crystal Compositor initialized successfully\n");
    fprintf(stderr, "[crystal] OpenGL renderer: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "[crystal] OpenGL version: %s\n", glGetString(GL_VERSION));
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
    for (int i = 0; i < crystal.window_count; i++) {
        if (crystal.windows[i].xwin == xwin) {
            return &crystal.windows[i];
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
static void refresh_window_texture(AuraWM *wm, struct WindowTexture *wt)
{
    if (!crystal.has_texture_from_pixmap) {
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
        fprintf(stderr, "[crystal] WARNING: Cannot get pixmap for window 0x%lx\n",
                wt->xwin);
        return;
    }

    // Choose the right FBConfig based on whether the window has alpha.
    // ARGB windows need the RGBA config; regular windows use RGB.
    GLXFBConfig fb = wt->has_alpha ? crystal.pixmap_fb_config
                                   : crystal.pixmap_fb_config_rgb;

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
        fprintf(stderr, "[crystal] GLX pixmap failed for 0x%lx, using CPU fallback\n",
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
static void refresh_window_texture_fallback(AuraWM *wm, struct WindowTexture *wt)
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

    wt->pixmap = XCompositeNameWindowPixmap(wm->dpy, wt->xwin);
    if (!wt->pixmap) return;

    // XGetImage reads pixel data from a drawable (pixmap) into CPU memory.
    // ZPixmap format returns the image as a packed pixel array (as opposed to
    // XYPixmap which separates color planes).
    // AllPlanes means "read all color channels."
    XImage *img = XGetImage(wm->dpy, wt->pixmap, 0, 0,
                            wt->w, wt->h, AllPlanes, ZPixmap);
    if (!img) {
        fprintf(stderr, "[crystal] WARNING: XGetImage failed for 0x%lx\n",
                wt->xwin);
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
static void release_window_texture(AuraWM *wm, struct WindowTexture *wt)
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

void crystal_window_mapped(AuraWM *wm, Client *c)
{
    if (!crystal.active || !wm || !c || !c->frame) return;

    // Don't add duplicates — check if we're already tracking this window
    if (find_window_texture(c->frame)) return;

    // Make sure we haven't hit the window limit
    if (crystal.window_count >= MAX_CLIENTS) {
        fprintf(stderr, "[crystal] WARNING: Maximum window count reached, "
                "cannot track window 0x%lx\n", c->frame);
        return;
    }

    // Get the window's visual to determine if it has an alpha channel.
    // Windows created with our ARGB visual will have depth 32.
    XWindowAttributes wa;
    if (!XGetWindowAttributes(wm->dpy, c->frame, &wa)) {
        fprintf(stderr, "[crystal] WARNING: Cannot get attributes for 0x%lx\n",
                c->frame);
        return;
    }

    // Create a new tracking entry for this window
    struct WindowTexture *wt = &crystal.windows[crystal.window_count];
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

    crystal.window_count++;

    if (getenv("AURA_DEBUG")) {
        fprintf(stderr, "[crystal] Mapped window 0x%lx (%dx%d at %d,%d, "
                "alpha=%d)\n", c->frame, wt->w, wt->h, wt->x, wt->y,
                wt->has_alpha);
    }
}

void crystal_window_unmapped(AuraWM *wm, Client *c)
{
    if (!crystal.active || !wm || !c) return;

    // Find the window in our tracking array
    for (int i = 0; i < crystal.window_count; i++) {
        if (crystal.windows[i].xwin == c->frame) {
            // Release all GPU and X resources for this window
            release_window_texture(wm, &crystal.windows[i]);

            // Remove from the array by shifting everything after it left.
            // This maintains the z-order (array order = stacking order).
            int remaining = crystal.window_count - i - 1;
            if (remaining > 0) {
                memmove(&crystal.windows[i], &crystal.windows[i + 1],
                        remaining * sizeof(struct WindowTexture));
            }
            crystal.window_count--;

            if (getenv("AURA_DEBUG")) {
                fprintf(stderr, "[crystal] Unmapped window 0x%lx\n", c->frame);
            }
            return;
        }
    }
}

void crystal_window_damaged(AuraWM *wm, Client *c)
{
    if (!crystal.active || !wm || !c) return;

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
void crystal_window_resized(AuraWM *wm, Client *c)
{
    if (!crystal.active || !wm || !c) return;

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
// Crystal needs to composite ALL visible windows, including unmanaged ones
// (dock, desktop, menubar, spotlight) that the WM does not frame and does
// not create Client structs for. Instead of requiring every window lifecycle
// path to call crystal_window_mapped(), we use XQueryTree each frame to
// discover all children of the root window and auto-track any new ones.
//
// This also handles cleanup: if a tracked window is no longer a child of
// root (or is no longer viewable), we remove its tracking entry.

static void sync_tracked_windows(AuraWM *wm)
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
            int idx = (int)(wt - crystal.windows);
            if (idx >= 0 && idx < crystal.window_count) {
                seen[idx] = true;
            }
            continue;
        }

        // New window — add a tracking entry if we have room
        if (crystal.window_count >= MAX_CLIENTS) continue;

        wt = &crystal.windows[crystal.window_count];
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
        seen[crystal.window_count] = true;
        crystal.window_count++;

        if (getenv("AURA_DEBUG")) {
            fprintf(stderr, "[crystal] Auto-tracked window 0x%lx (%dx%d at %d,%d%s)\n",
                    children[i], wa.width, wa.height, wa.x, wa.y,
                    wa.override_redirect ? " override-redirect" : "");
        }
    }

    // ── Pass 3: Remove tracked windows that are no longer visible ──
    // Iterate backwards so removals don't shift indices we haven't checked yet.
    for (int i = crystal.window_count - 1; i >= 0; i--) {
        if (!seen[i]) {
            if (getenv("AURA_DEBUG")) {
                fprintf(stderr, "[crystal] Removing stale window 0x%lx\n",
                        crystal.windows[i].xwin);
            }
            // If this was an override-redirect window, unredirect it since
            // we explicitly redirected it in the tracking pass above.
            // This is safe even if the window is already destroyed — X11
            // silently ignores the call for non-existent windows.
            if (crystal.windows[i].override_redirect) {
                XCompositeUnredirectWindow(wm->dpy, crystal.windows[i].xwin,
                                           CompositeRedirectManual);
            }

            release_window_texture(wm, &crystal.windows[i]);

            int remaining = crystal.window_count - i - 1;
            if (remaining > 0) {
                memmove(&crystal.windows[i], &crystal.windows[i + 1],
                        remaining * sizeof(struct WindowTexture));
            }
            crystal.window_count--;
        }
    }

    // ── Pass 4: Reorder tracked windows to match XQueryTree stacking ──
    // XQueryTree returns children in bottom-to-top order, which is exactly
    // the order we need for back-to-front compositing. Rebuild the array
    // to match this stacking order so windows overlap correctly.
    if (crystal.window_count > 1) {
        struct WindowTexture reordered[MAX_CLIENTS];
        int count = 0;

        for (unsigned int i = 0; i < nchildren && count < crystal.window_count; i++) {
            struct WindowTexture *wt = find_window_texture(children[i]);
            if (wt) {
                reordered[count++] = *wt;
            }
        }

        // Copy the reordered array back
        if (count == crystal.window_count) {
            memcpy(crystal.windows, reordered,
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
    // radius:     how far the shadow extends from the window edge.
    //             Focused windows get a larger radius for emphasis.
    // peak_alpha: maximum opacity of the shadow directly under the window.
    //             Focused = darker (0.45), unfocused = lighter (0.22).
    // pad:        extra pixels around the window rect to give the blur room
    //             to fade out. We use 2x the radius to avoid clipping.
    int radius = focused ? SHADOW_RADIUS : SHADOW_RADIUS_INACTIVE;
    float peak_alpha = focused ? (float)SHADOW_ALPHA_ACTIVE
                               : (float)SHADOW_ALPHA_INACTIVE;

    // The chrome is the visible window area (excluding shadow padding in the
    // frame). The shadow is generated around this chrome rectangle.
    int chrome_w = wt->w - SHADOW_LEFT - SHADOW_RIGHT;
    int chrome_h = wt->h - SHADOW_TOP - SHADOW_BOTTOM;
    if (chrome_w <= 0 || chrome_h <= 0) return;

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
    // the window is resized (handled in crystal_window_resized).
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

    int radius = focused ? SHADOW_RADIUS : SHADOW_RADIUS_INACTIVE;
    int pad = radius * 2;
    int expected_w = chrome_w + pad * 2;
    int expected_h = chrome_h + pad * 2;

    // Generate or regenerate the shadow texture if needed.
    // This happens on first draw (shadow_tex == 0) or after a resize
    // (dimensions no longer match).
    if (!wt->shadow_tex ||
        wt->shadow_tex_w != expected_w ||
        wt->shadow_tex_h != expected_h) {
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
    // an additional y_offset downward to simulate a top-down light.
    int chrome_x = wt->x + SHADOW_LEFT;
    int chrome_y = wt->y + SHADOW_TOP;
    float sx = (float)(chrome_x - pad);
    float sy = (float)(chrome_y - pad + SHADOW_Y_OFFSET);

    // Enable texturing and bind our cached shadow texture
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, wt->shadow_tex);

    // Set color to white — the actual shadow color and opacity are baked
    // into the texture's alpha channel. With pre-multiplied alpha and our
    // blend mode (GL_ONE, GL_ONE_MINUS_SRC_ALPHA), white color means the
    // texture pixels are used as-is.
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    // Draw a textured quad covering the shadow area.
    // Texture coords (0,0)→(1,1) map the full shadow texture onto the quad.
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

    // Unbind the shadow texture and disable texturing so subsequent
    // rendering code starts from a clean state.
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Main compositing pass
// ────────────────────────────────────────────────────────────────────────
//
// This is the heart of Crystal — called every frame from the event loop.
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

void crystal_composite(AuraWM *wm)
{
    if (!crystal.active || !wm) return;

    // Make our GL context current. This is technically redundant if we're
    // the only GL user, but it's good practice — other code might have
    // changed the current context.
    glXMakeContextCurrent(wm->dpy, crystal.gl_window, crystal.gl_window,
                          crystal.gl_context);

    // ── Step 1: Clear the screen ──
    // GL_COLOR_BUFFER_BIT tells OpenGL to fill the entire framebuffer with
    // the clear color (set in crystal_init). This erases the previous frame.
    glClear(GL_COLOR_BUFFER_BIT);

    // ── Step 2: Set up 2D projection ──
    // Reset the projection matrix each frame (in case the screen was resized).
    // glOrtho maps (0,0) to the top-left corner and (root_w, root_h) to the
    // bottom-right, matching X11's coordinate convention.
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, crystal.root_w, crystal.root_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // ── Step 3: Enable blending for transparent windows ──
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
        for (int i = 0; i < crystal.window_count; i++) {
            struct WindowTexture *wt = &crystal.windows[i];

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

            // Skip zero-size windows (shouldn't happen, but be defensive)
            if (wt->w <= 0 || wt->h <= 0) continue;

            // ── Refresh dirty textures ──
            // If the window's contents have changed since the last frame,
            // update the OpenGL texture with the new pixel data.
            // Override-redirect windows (menus, tooltips) are always refreshed
            // because they don't use XDamage tracking — their contents are
            // read directly from the window each frame.
            if (wt->dirty || wt->override_redirect) {
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

            // ── Draw the window contents ──
            // Bind the window's texture and draw a quad that covers the
            // window's screen rectangle.
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, wt->texture);

            // Reset color to white so the texture colors aren't modified.
            // When GL_TEXTURE_2D is enabled, the final pixel color is:
            //   texture_color * glColor
            // With glColor = (1,1,1,1), the texture appears as-is.
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

            // Draw a textured quad.
            // glTexCoord2f sets the texture coordinate for the next vertex.
            // Texture coordinates go from (0,0) at the top-left of the image
            // to (1,1) at the bottom-right. By mapping these to the window's
            // screen rectangle, we stretch the texture to fill the quad.
            //
            // Vertices are specified in clockwise order:
            //   (0,0)──────(1,0)
            //     │  texture  │
            //   (0,1)──────(1,1)
            glBegin(GL_QUADS);
                glTexCoord2f(0.0f, 0.0f);
                glVertex2f((float)wt->x, (float)wt->y);                 // Top-left

                glTexCoord2f(1.0f, 0.0f);
                glVertex2f((float)(wt->x + wt->w), (float)wt->y);      // Top-right

                glTexCoord2f(1.0f, 1.0f);
                glVertex2f((float)(wt->x + wt->w), (float)(wt->y + wt->h));  // Bottom-right

                glTexCoord2f(0.0f, 1.0f);
                glVertex2f((float)wt->x, (float)(wt->y + wt->h));       // Bottom-left
            glEnd();

            glDisable(GL_TEXTURE_2D);
        }
    }

    // ── Step 5: Swap buffers ──
    // We've been drawing to the "back buffer" (an invisible off-screen surface).
    // glXSwapBuffers swaps the back buffer with the front buffer (what's on
    // screen), making our new frame visible instantaneously. This is called
    // "double buffering" and prevents flicker — the user never sees a
    // half-drawn frame.
    //
    // If VSync is enabled, this call blocks until the next vertical blank
    // period, limiting us to the monitor's refresh rate (typically 60 FPS).
    glXSwapBuffers(wm->dpy, crystal.gl_window);
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Event handling
// ────────────────────────────────────────────────────────────────────────

bool crystal_handle_event(AuraWM *wm, XEvent *e)
{
    if (!crystal.active || !wm || !e) return false;

    // Check if this is an XDamage event.
    // XDamage events have type == (damage_event_base + XDamageNotify).
    // XDamageNotify is 0, so the type is just damage_event_base.
    if (e->type == crystal.damage_event_base + XDamageNotify) {
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

    return false;  // Not a Crystal event
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
// here for consistency since Crystal owns compositing now.

void crystal_set_input_shape(AuraWM *wm, Client *c)
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
        fprintf(stderr, "[crystal] Set input shape for '%s': "
                "clickable at (%d,%d) %dx%d\n",
                c->title, rect.x, rect.y, rect.width, rect.height);
    }
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: ARGB visual access
// ────────────────────────────────────────────────────────────────────────
//
// These functions expose Crystal's ARGB visual and colormap to the rest of
// the window manager (especially frame.c, which needs them to create frame
// windows with alpha support).
//
// They maintain compatibility with the old compositor's
// compositor_create_argb_visual() interface.

bool crystal_create_argb_visual(AuraWM *wm, Visual **out_visual,
                                Colormap *out_colormap)
{
    if (!out_visual || !out_colormap) return false;

    // If Crystal found an ARGB visual during init, return it
    if (crystal.argb_visual && crystal.argb_colormap) {
        *out_visual = crystal.argb_visual;
        *out_colormap = crystal.argb_colormap;
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

bool crystal_is_active(void)
{
    return crystal.active;
}

int crystal_get_damage_event_base(void)
{
    return crystal.damage_event_base;
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Screen resize handling
// ────────────────────────────────────────────────────────────────────────

static void crystal_screen_resized(AuraWM *wm)
{
    if (!crystal.active || !wm) return;

    // Update our stored screen dimensions
    crystal.root_w = wm->root_w;
    crystal.root_h = wm->root_h;

    // Update the GL viewport to match the new screen size.
    // The viewport maps OpenGL's normalized coordinates to pixel coordinates.
    // (0, 0) is the bottom-left corner; (root_w, root_h) is the top-right.
    glViewport(0, 0, crystal.root_w, crystal.root_h);

    fprintf(stderr, "[crystal] Screen resized to %dx%d\n",
            crystal.root_w, crystal.root_h);
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Shutdown
// ────────────────────────────────────────────────────────────────────────

void crystal_shutdown(AuraWM *wm)
{
    if (!wm || !wm->dpy) return;

    fprintf(stderr, "[crystal] Shutting down Crystal Compositor...\n");

    // Release all tracked window textures
    for (int i = 0; i < crystal.window_count; i++) {
        release_window_texture(wm, &crystal.windows[i]);
    }
    crystal.window_count = 0;

    // Undo XComposite redirection — let X go back to drawing windows directly.
    // This is critical for a clean shutdown. If we don't do this and the WM
    // crashes, the screen goes blank because windows are still redirected to
    // off-screen pixmaps that nobody is compositing.
    if (crystal.active) {
        XCompositeUnredirectSubwindows(wm->dpy, wm->root,
                                       CompositeRedirectManual);
        fprintf(stderr, "[crystal] Unredirected subwindows\n");
    }

    // Destroy the GLX context and window.
    // Order matters: release the context first, then destroy the window.
    if (crystal.gl_context) {
        // Make sure nothing is current before destroying
        glXMakeContextCurrent(wm->dpy, None, None, NULL);

        if (crystal.gl_window) {
            glXDestroyWindow(wm->dpy, crystal.gl_window);
            crystal.gl_window = 0;
        }

        glXDestroyContext(wm->dpy, crystal.gl_context);
        crystal.gl_context = NULL;
    }

    // Free the ARGB colormap (the visual is owned by X, not us)
    if (crystal.argb_colormap) {
        XFreeColormap(wm->dpy, crystal.argb_colormap);
        crystal.argb_colormap = 0;
    }
    crystal.argb_visual = NULL;

    // Clear flags
    crystal.active = false;
    compositor_active = false;

    fprintf(stderr, "[crystal] Crystal Compositor shut down\n");
}

// ────────────────────────────────────────────────────────────────────────
// SECTION: Animation stubs (Phase 4+)
// ────────────────────────────────────────────────────────────────────────
//
// These are placeholder functions for future animation support. The genie
// minimize effect will warp the window's texture using a mesh distortion
// in the GL pipeline. For now, these are no-ops so the rest of the codebase
// can call them without conditional compilation.

void crystal_animate_minimize(AuraWM *wm, Client *c,
                              int dock_icon_x, int dock_icon_y)
{
    // Phase 4: Genie minimize animation.
    // Will distort the window texture over multiple frames to create the
    // classic macOS "sucking into the dock" effect.
    (void)wm;
    (void)c;
    (void)dock_icon_x;
    (void)dock_icon_y;
}

void crystal_animate_restore(AuraWM *wm, Client *c,
                             int dock_icon_x, int dock_icon_y)
{
    // Phase 4: Genie restore animation (reverse of minimize).
    // The window texture expands from the dock icon back to full size.
    (void)wm;
    (void)c;
    (void)dock_icon_x;
    (void)dock_icon_y;
}

bool crystal_animation_active(AuraWM *wm)
{
    // Phase 4: Returns true if any animation is in progress.
    // When active, the main loop should keep calling crystal_composite()
    // at the refresh rate to advance the animation smoothly.
    (void)wm;
    return false;
}
