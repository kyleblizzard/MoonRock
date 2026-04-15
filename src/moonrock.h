// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// ============================================================================
//  MoonRock Compositor — custom OpenGL X11 compositor
// ============================================================================
//
// MoonRock replaces the old compositor.h/compositor.c module (which used Cairo
// to paint shadows into oversized ARGB frame windows) and also eliminates the
// need for picom or any external compositor. Everything happens in-process.
//
// Why "MoonRock"?
//   Apple's macOS uses "Quartz Compositor" for its window compositing engine.
//   MoonRock is our playful nod to that — a different kind of clear, beautiful
//   mineral. It fits the MoonRock Compositor aesthetic of recreating the Snow Leopard look
//   and feel on Linux with our own technology.
//
// Why replace picom?
//   picom is a standalone compositor that runs as a separate process. It works
//   fine for general desktop use, but we need total control over the compositing
//   pipeline so we can:
//     1. Render pixel-perfect Snow Leopard drop shadows (Gaussian blur, correct
//        alpha, different intensity for active vs. inactive windows).
//     2. Add custom animations later (genie minimize/restore, window zoom).
//     3. Synchronize compositing with window management events — no race
//        conditions between two separate processes.
//     4. Avoid the overhead and complexity of IPC with an external compositor.
//   By owning the compositor, every visual effect is under our direct control.
//
// How the rendering pipeline works (high level):
//
//   1. REDIRECT — At startup, MoonRock calls XCompositeRedirectSubwindows() in
//      "manual" mode. This tells the X server to stop painting windows directly
//      to the screen. Instead, each window's pixels are rendered into an
//      off-screen pixmap that we can read from.
//
//   2. BIND — For each visible window, we call XCompositeNameWindowPixmap() to
//      get a handle to its off-screen pixmap, then bind that pixmap as an
//      OpenGL texture using glXBindTexImageEXT. Now we can draw the window's
//      contents as a textured quad in our GL scene.
//
//   3. COMPOSITE — Every frame, we clear the screen, draw the desktop
//      background, then iterate through all windows in stacking order
//      (bottom to top). For each window we:
//        a. Draw a Gaussian-blurred shadow behind the window.
//        b. Draw the window's texture (its actual contents) on top.
//      Alpha blending is enabled so transparent regions work correctly.
//
//   4. DAMAGE — We do not re-bind textures every frame (that would be slow).
//      Instead, we listen for XDamage events from the X server. When a window
//      repaints itself, X sends us a DamageNotify event, and we mark that
//      window's texture as "dirty" so it gets re-bound on the next frame.
//
//   5. VSYNC — We swap the back buffer to the screen using glXSwapBuffers(),
//      which (when VSync is enabled via GLX_EXT_swap_control) waits for the
//      monitor's vertical blank interval. This prevents screen tearing.
//
// What is GLX?
//   GLX is the "glue" between OpenGL and the X Window System. OpenGL by itself
//   does not know how to create windows or talk to a display server — it only
//   knows how to draw triangles and textures. GLX provides the bridge:
//     - glXCreateContext()   — Create an OpenGL rendering context.
//     - glXMakeCurrent()     — Attach that context to an X window (drawable).
//     - glXSwapBuffers()     — Present the rendered frame to the screen.
//     - glXBindTexImageEXT() — Bind an X pixmap as an OpenGL texture.
//   We use GLX instead of EGL because we are on X11 (not Wayland), and GLX
//   gives us the most direct and well-tested path for X11 + OpenGL compositing.
//
// ============================================================================

#ifndef MOONROCK_H
#define MOONROCK_H

#include "wm_compat.h"
#include <stdbool.h>

// ============================================================================
//  Shadow parameters
// ============================================================================
//
// These define how many extra pixels the frame extends beyond the actual window
// to make room for the drop shadow. They are kept compatible with the values
// from the old compositor.h so that frame.c and decor.c do not need changes.
//
// SHADOW_RADIUS is the Gaussian blur spread in pixels — bigger means a softer,
// wider shadow.
//
// SHADOW_Y_OFFSET shifts the shadow downward, simulating light coming from
// directly above the screen (like a desk lamp). This means the shadow peeks
// out more at the bottom than the top, which looks natural.

#define SHADOW_RADIUS             22
#define SHADOW_Y_OFFSET            4

// How far the shadow extends past the window chrome on each side.
// Top is reduced because the shadow shifts down; bottom is increased.
#define SHADOW_TOP                (SHADOW_RADIUS - SHADOW_Y_OFFSET)   // 18 px
#define SHADOW_BOTTOM             (SHADOW_RADIUS + SHADOW_Y_OFFSET)   // 26 px
#define SHADOW_LEFT                SHADOW_RADIUS                       // 22 px
#define SHADOW_RIGHT               SHADOW_RADIUS                       // 22 px

// Peak alpha (opacity) for the shadow.
// Active (focused) windows get a stronger, more prominent shadow.
// Inactive windows get a softer shadow so they recede visually.
// Values: 0.0 = invisible, 1.0 = fully opaque black.
#define SHADOW_ALPHA_ACTIVE        0.45
#define SHADOW_ALPHA_INACTIVE      0.22

// Inactive windows also use a smaller blur radius for a tighter, subtler look.
#define SHADOW_RADIUS_INACTIVE     16


// ============================================================================
//  Initialization and shutdown
// ============================================================================

// Initialize the MoonRock Compositor.
//
// This does all the heavy lifting to get compositing running:
//   1. Checks that the X server supports XComposite, XDamage, and XFixes.
//   2. Finds a GLX framebuffer config with an ARGB visual (32-bit color + alpha).
//   3. Creates an OpenGL context and makes it current on the root window.
//   4. Calls XCompositeRedirectSubwindows() in manual mode so we take over
//      painting all top-level windows.
//   5. Sets up XDamage tracking on each existing window.
//   6. Enables VSync via GLX_EXT_swap_control (if available).
//   7. Pre-computes the Gaussian blur kernel used for shadows.
//
// Call this once at WM startup, after wm_init() has run.
//
// Returns true on success, false if a required extension is missing or
// the GL context could not be created.
bool mr_init(CCWM *wm);

// Shut down MoonRock and clean up all resources.
//
// This undoes everything mr_init() set up:
//   - Calls XCompositeUnredirectSubwindows() so windows paint normally again.
//   - Destroys all OpenGL textures we created for window pixmaps.
//   - Destroys the GLX context.
//   - Frees the Gaussian blur kernel and any other allocated memory.
//
// After this call, windows will render directly to the screen (no compositing).
void mr_shutdown(CCWM *wm);


// ============================================================================
//  Per-frame rendering
// ============================================================================

// Composite all visible windows onto the root window. Call once per frame.
//
// This is the main render loop entry point. It does the following each frame:
//   1. Clears the back buffer.
//   2. Draws the desktop background (solid color or wallpaper texture).
//   3. Iterates through all clients in stacking order (bottom to top):
//      a. Re-binds the window's texture if it was marked dirty by XDamage.
//      b. Draws the Gaussian blur shadow behind the window.
//      c. Draws the window's texture as a textured quad with alpha blending.
//   4. Draws any active animations (minimize/restore effects).
//   5. Calls glXSwapBuffers() to present the frame (VSync'd if supported).
//
// The caller (main event loop) should call this whenever there is damage to
// process, or whenever an animation is in progress.
void mr_composite(CCWM *wm);


// ============================================================================
//  Window lifecycle
// ============================================================================
//
// These functions keep MoonRock in sync with the window manager. Every time a
// window appears, disappears, changes size, or repaints, the WM must notify
// MoonRock so it can create, destroy, or update the corresponding GL texture.

// A new window has been mapped (made visible on screen).
//
// MoonRock will:
//   - Call XCompositeNameWindowPixmap() to get the window's off-screen pixmap.
//   - Bind that pixmap as an OpenGL texture via glXBindTexImageEXT().
//   - Register an XDamage object on the window to track content changes.
//
// Call this from the MapNotify handler after framing the window.
void mr_window_mapped(CCWM *wm, Client *c);

// A window has been unmapped (hidden) or destroyed.
//
// MoonRock will:
//   - Release the GL texture bound to the window's pixmap.
//   - Free the XComposite pixmap.
//   - Destroy the XDamage tracking object for this window.
//
// Call this from the UnmapNotify or DestroyNotify handler.
void mr_window_unmapped(CCWM *wm, Client *c);

// A window's contents have changed (we received an XDamage event for it).
//
// MoonRock marks the window's texture as "dirty." On the next call to
// mr_composite(), the texture will be re-bound from the updated pixmap
// before drawing. This avoids re-binding every texture every frame.
//
// Call this from the DamageNotify event handler.
void mr_window_damaged(CCWM *wm, Client *c);

// A window has been resized (ConfigureNotify with new dimensions).
//
// When a window changes size, its off-screen pixmap is invalidated by the X
// server and a new one is allocated. MoonRock must:
//   - Release the old GL texture.
//   - Get the new pixmap via XCompositeNameWindowPixmap().
//   - Bind the new pixmap as a fresh GL texture.
//
// Call this from the ConfigureNotify handler after updating the Client's
// geometry (c->w and c->h).
void mr_window_resized(CCWM *wm, Client *c);


// ============================================================================
//  ARGB visual support
// ============================================================================
//
// Frame windows need a 32-bit ARGB visual so that the shadow region around
// each window can be semi-transparent. A normal 24-bit visual has no alpha
// channel, so anything outside the window chrome would be solid black.
//
// These functions are used by frame.c when creating frame windows.

// Find a 32-bit ARGB visual on the default screen.
//
// An ARGB visual has 8 bits each for red, green, blue, and alpha — 32 bits
// total. The alpha channel lets us make parts of the frame window transparent
// (the shadow region) while keeping the title bar and borders opaque.
//
// out_visual:   receives a pointer to the 32-bit Visual.
// out_colormap: receives a Colormap created for that visual (X requires each
//               visual to have its own colormap).
//
// Returns true if a suitable visual was found, false otherwise.
bool mr_create_argb_visual(CCWM *wm, Visual **out_visual,
                                Colormap *out_colormap);

// Set the X input shape on a frame window so mouse clicks pass through the
// shadow region.
//
// The shadow is purely visual — if someone clicks in the shadow area, the
// click should go to whatever window (or the desktop) is underneath, not to
// the frame. We use the XShape extension (specifically the "input shape") to
// define a clickable rectangle that covers only the actual window chrome,
// excluding the shadow padding on all four sides.
//
// Call this after framing a window and after any resize.
void mr_set_input_shape(CCWM *wm, Client *c);


// ============================================================================
//  Event handling
// ============================================================================

// Process an X event that might be compositor-related.
//
// MoonRock handles these event types:
//   - DamageNotify: a window's contents changed; mark its texture dirty.
//   - (Future) Any custom events from GLX or other extensions.
//
// Returns true if MoonRock consumed the event (caller should not process it
// further), or false if the event is not compositor-related and should be
// handled by the normal WM event loop.
//
// Usage in the main loop:
//   if (mr_handle_event(wm, &event)) continue;  // MoonRock handled it
//   // ... normal event dispatch ...
bool mr_handle_event(CCWM *wm, XEvent *e);


// ============================================================================
//  Animation API (Phase 4+ — stubs for now)
// ============================================================================
//
// These are placeholder functions for future animation support. The genie
// effect is the classic macOS minimize animation where the window appears to
// pour into the dock icon like liquid. We will implement this by distorting
// the window's texture with a mesh warp in the GL pipeline.
//
// For now, these are no-ops. They exist so the rest of the codebase can call
// them without #ifdefs, and we fill in the real implementation later.

// Start the genie minimize animation for a window.
//
// The window's texture will be warped over several frames to appear as if it
// is being sucked into the dock at (dock_icon_x, dock_icon_y). Once the
// animation finishes, the window is fully hidden.
//
// dock_icon_x, dock_icon_y: the screen coordinates of the target dock icon.
void mr_animate_minimize(CCWM *wm, Client *c,
                              int dock_icon_x, int dock_icon_y);

// Start the genie restore animation (un-minimize) for a window.
//
// The reverse of minimize: the window texture appears to expand out from the
// dock icon position back to its normal size and location.
//
// dock_icon_x, dock_icon_y: the screen coordinates of the source dock icon.
void mr_animate_restore(CCWM *wm, Client *c,
                             int dock_icon_x, int dock_icon_y);

// Check if any animation is currently in progress.
//
// When an animation is active, the main loop should keep calling
// mr_composite() at the display's refresh rate (typically 60 Hz) to
// advance the animation smoothly. When no animation is active, we only need
// to composite when there is XDamage (saving CPU/GPU).
//
// Returns true if at least one animation is still playing.
bool mr_animation_active(CCWM *wm);


// ============================================================================
//  State query
// ============================================================================

// Check whether MoonRock initialized successfully and is actively compositing.
//
// Other modules (e.g., frame.c, decor.c) use this to decide whether to create
// ARGB frame windows with shadow padding, or fall back to simple frames
// without compositing effects.
//
// Returns true if mr_init() succeeded, false otherwise.
bool mr_is_active(void);

// Get the XDamage event base number.
//
// X extensions define their own event types starting at a "base" offset that
// is assigned at runtime (it varies between X servers and depends on which
// extensions are loaded). To identify a DamageNotify event in the main loop,
// we compare: event.type == mr_get_damage_event_base() + XDamageNotify.
//
// The main event loop uses this to route damage events to mr_handle_event().
int mr_get_damage_event_base(void);


// ============================================================================
//  Shader and texture accessors
// ============================================================================
//
// These let other MoonRock modules (Mission Control, animations, plugins)
// access the compositor's shader programs and per-frame projection matrix
// without reaching into the static mr struct directly.

#include "moonrock_shaders.h"

// Get a pointer to MoonRock's compiled shader programs.
// Returns NULL if MoonRock is not initialized.
ShaderPrograms *mr_get_shaders(void);

// Get the current frame's 4x4 orthographic projection matrix.
// Returns a pointer to a float[16] in column-major order.
float *mr_get_projection(void);

// Look up a window's GL texture handle by its X window ID.
// Returns the GL texture handle, or 0 if the window isn't tracked.
// Used by Mission Control to render window thumbnails in the overview.
GLuint mr_get_window_texture_id(Window xwin);

#endif // MOONROCK_H
