// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// ============================================================================
//  Crystal Mission Control — unified Expose + Spaces overview (macOS 10.7)
// ============================================================================
//
// This file implements the Mission Control overlay: the bird's-eye view that
// shows all windows tiled in a grid with desktop thumbnails at the top.
//
// See crystal_mission_control.h for the full design overview.
//
// ============================================================================

// _GNU_SOURCE enables POSIX extensions we need, including clock_gettime()
// with CLOCK_MONOTONIC for high-resolution timing.
#define _GNU_SOURCE

#include "crystal_mission_control.h"
#include "crystal_anim.h"
#include "crystal_shaders.h"
#include "crystal.h"

#include <GL/glext.h>       // GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, etc.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


// ============================================================================
//  Timing helper
// ============================================================================
//
// We need a monotonic clock (one that only ever goes forward, unaffected by
// NTP adjustments or manual time changes) to measure animation durations.
// CLOCK_MONOTONIC is perfect for this — it counts seconds since some arbitrary
// starting point and never jumps backward.

// Get the current monotonic time in seconds (with fractional nanoseconds).
// Used to track animation start times and compute elapsed durations.
static double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // Convert seconds + nanoseconds into a single double.
    // 1 second = 1,000,000,000 nanoseconds.
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}


// ============================================================================
//  Global state
// ============================================================================
//
// Mission Control uses a single global state struct. There is only ever one
// Mission Control overlay (you cannot open two simultaneously), so a global
// is simpler and safer than passing pointers around.

static MissionControlState mc;

// Counter for generating unique Space IDs. Starts at 1 because the first
// Space (created in mc_init) gets ID 1. Increments each time a new Space
// is added. Never decrements — IDs are not reused.
static int next_space_id = 1;


// ============================================================================
//  Drag state — for dragging windows between Spaces
// ============================================================================
//
// When the user presses the mouse on a tiled window and moves it, we enter
// drag mode. The dragged window follows the cursor. If the user releases
// the mouse over a Space thumbnail, we move the window to that Space.
// If released elsewhere, the window snaps back to its original position.

static struct {
    bool active;            // True while a window is being dragged
    int window_idx;         // Index into mc.tiled_windows[] of the dragged window
    float offset_x;         // Cursor offset from the window's top-left corner (X)
    float offset_y;         // Cursor offset from the window's top-left corner (Y)
    float current_x;        // Current draw position of the dragged window (X)
    float current_y;        // Current draw position of the dragged window (Y)
    int start_x;            // Mouse X when drag started (for dead-zone detection)
    int start_y;            // Mouse Y when drag started (for dead-zone detection)
    bool past_threshold;    // True once the cursor has moved far enough to start
} drag;

// Minimum number of pixels the cursor must move before a drag begins.
// This prevents accidental drags when the user just wants to click.
#define DRAG_THRESHOLD 8


// ============================================================================
//  Slide animation state — for Space switching transitions
// ============================================================================
//
// When switching Spaces, we animate a horizontal slide. The entire window
// grid slides left or right, and the new Space's windows slide in from the
// opposite side. This gives the illusion of physically moving between desks.

static struct {
    bool active;            // True while a slide animation is playing
    double start_time;      // Monotonic timestamp when the slide started
    double duration;        // How long the slide takes (0.3 seconds)
    float direction;        // -1.0 for slide left, +1.0 for slide right
} slide_anim;


// ============================================================================
//  Thumbnail FBO state — for rendering Space previews off-screen
// ============================================================================
//
// Each Space thumbnail is rendered into a small FBO (framebuffer object).
// We keep a single reusable FBO and re-render into it whenever a Space's
// thumbnail is marked dirty.

// Thumbnail dimensions in pixels. Small enough to be fast, large enough
// to show window layout clearly.
#define THUMB_FBO_W 200
#define THUMB_FBO_H 125

static GLuint thumb_fbo = 0;        // Reusable FBO handle (0 = not created yet)
static GLuint thumb_fbo_tex = 0;    // Texture attached to the FBO (temporary)


// ============================================================================
//  Forward declarations for static helper functions
// ============================================================================

static void compute_tiled_layout(MissionControlState *state, int screen_w, int screen_h);
static void draw_space_thumbnails(AuraWM *wm, GLuint shader, float *projection, float ease);
static int find_space_index(int space_id);
static int find_window_in_space(MCSpace *space, Window win);
static void map_space_windows(AuraWM *wm, int space_index);
static void unmap_space_windows(AuraWM *wm, int space_index);
static void mc_render_space_thumbnail(AuraWM *wm, int space_index);
static void mc_ensure_thumbnails(AuraWM *wm);


// ============================================================================
//  Initialization and shutdown
// ============================================================================

// Initialize Mission Control. Called once during Crystal startup.
//
// Creates the first Space ("Desktop 1") and assigns every currently mapped
// window to it. All animation and hover state is zeroed out.
void mc_init(AuraWM *wm)
{
    // Zero out the entire state struct so all booleans are false,
    // all counts are 0, and all pointers are NULL.
    memset(&mc, 0, sizeof(mc));

    // No window or space is being hovered initially.
    mc.hover_window = -1;
    mc.hover_space  = -1;

    // Create the first Space — every desktop must have at least one.
    mc.space_count   = 1;
    mc.current_space = 0;

    MCSpace *first = &mc.spaces[0];
    first->id = next_space_id++;
    snprintf(first->name, sizeof(first->name), "Desktop 1");
    first->thumbnail_tex   = 0;       // No thumbnail rendered yet
    first->thumbnail_dirty = true;    // Mark for rendering on first draw

    // Allocate the window list for the first Space. We start with capacity
    // for MAX_CLIENTS windows (the max the WM tracks). This avoids frequent
    // realloc calls as windows are added.
    first->windows = calloc(MAX_CLIENTS, sizeof(Window));
    first->window_count = 0;

    // Assign all currently mapped windows to the first Space.
    // This handles the case where Mission Control is initialized after windows
    // already exist (e.g., WM restart or delayed compositor init).
    for (int i = 0; i < wm->num_clients; i++) {
        if (wm->clients[i].mapped) {
            first->windows[first->window_count++] = wm->clients[i].client;
        }
    }

    // Clear drag and slide animation state.
    memset(&drag, 0, sizeof(drag));
    memset(&slide_anim, 0, sizeof(slide_anim));

    fprintf(stderr, "[Mission Control] Initialized with %d windows on Desktop 1\n",
            first->window_count);
}

// Shut down Mission Control and free all allocated resources.
//
// Frees the window arrays in each Space and deletes any thumbnail textures
// that were created for the desktop previews.
void mc_shutdown(AuraWM *wm)
{
    (void)wm;  // wm is unused but kept for API consistency

    for (int i = 0; i < mc.space_count; i++) {
        // Free the dynamically allocated window list for this Space.
        if (mc.spaces[i].windows) {
            free(mc.spaces[i].windows);
            mc.spaces[i].windows = NULL;
        }
        // Delete the thumbnail texture if one was created.
        if (mc.spaces[i].thumbnail_tex) {
            glDeleteTextures(1, &mc.spaces[i].thumbnail_tex);
            mc.spaces[i].thumbnail_tex = 0;
        }
    }

    // Destroy the reusable thumbnail FBO if it exists.
    if (thumb_fbo) {
        shaders_destroy_fbo(thumb_fbo, thumb_fbo_tex);
        thumb_fbo     = 0;
        thumb_fbo_tex = 0;
    }

    // Zero everything so mc_is_active() returns false and any stale
    // references are cleared.
    memset(&mc, 0, sizeof(mc));
    mc.hover_window = -1;
    mc.hover_space  = -1;
    memset(&drag, 0, sizeof(drag));
    memset(&slide_anim, 0, sizeof(slide_anim));

    fprintf(stderr, "[Mission Control] Shut down\n");
}


// ============================================================================
//  Activation — entering and exiting Mission Control
// ============================================================================

// Toggle Mission Control on or off.
//
// This is the main entry point called by keyboard shortcuts (F9) and hot
// corners. It checks the current state and does the right thing:
//   - If hidden: enter Mission Control (zoom out to overview).
//   - If shown or animating in: exit Mission Control (zoom back).
//   - If already animating out: do nothing (let the exit finish).
void mc_toggle(AuraWM *wm)
{
    if (mc.animating_out) {
        // Already exiting — don't interrupt the animation.
        return;
    }

    if (mc.active || mc.animating_in) {
        // Currently shown or entering — exit back to normal.
        mc_exit(wm, None);
    } else {
        // Currently hidden — enter the overview.
        mc_enter(wm);
    }
}

// Enter Mission Control (animate into the overview).
//
// Steps:
//   1. Snapshot every mapped window's geometry and GL texture.
//   2. Compute the tiled grid layout for the current Space.
//   3. Start the 0.3-second enter animation.
//   4. Grab the keyboard and pointer so all input routes through mc_handle_event().
void mc_enter(AuraWM *wm)
{
    // Don't enter if we're already active or animating.
    if (mc.active || mc.animating_in || mc.animating_out) return;

    fprintf(stderr, "[Mission Control] Entering overview\n");

    // --- Step 1: Snapshot all visible windows ---
    // Walk through the WM's client list and record every mapped window's
    // current position, size, and texture handle. We store these in
    // mc.tiled_windows[] so we can animate them into the grid.
    mc.tiled_count = 0;

    // Get the current Space so we only tile windows that belong to it.
    MCSpace *space = &mc.spaces[mc.current_space];

    for (int i = 0; i < wm->num_clients && mc.tiled_count < 256; i++) {
        Client *c = &wm->clients[i];
        if (!c->mapped) continue;

        // Check if this window belongs to the current Space.
        // (If Spaces are active, we only show windows from the active desktop.)
        bool in_current_space = false;
        for (int j = 0; j < space->window_count; j++) {
            if (space->windows[j] == c->client) {
                in_current_space = true;
                break;
            }
        }
        if (!in_current_space) continue;

        // Skip dock-type windows (they stay visible across all Spaces and
        // shouldn't be tiled in the overview).
        if (c->wm_type == wm->atom_net_wm_type_dock) continue;
        if (c->wm_type == wm->atom_net_wm_type_desktop) continue;

        // Record this window's snapshot.
        int idx = mc.tiled_count++;
        mc.tiled_windows[idx].xwin   = c->client;
        mc.tiled_windows[idx].orig_x = (float)c->x;
        mc.tiled_windows[idx].orig_y = (float)c->y;
        mc.tiled_windows[idx].orig_w = (float)c->w;
        mc.tiled_windows[idx].orig_h = (float)c->h;

        // Look up the GL texture for this window. Crystal created the texture
        // when the window was first mapped (crystal_window_mapped). We retrieve
        // it by the frame window ID since that's what Crystal binds textures to.
        // If the window has no texture yet (e.g., it was just created), we get
        // 0 back, which mc_draw() handles by showing a placeholder rectangle.
        GLuint tex = crystal_get_window_texture_id(c->frame);
        if (tex == 0) {
            // Fall back to the client window ID — some windows may be tracked
            // by their client window rather than the frame.
            tex = crystal_get_window_texture_id(c->client);
        }
        mc.tiled_windows[idx].texture = tex;
    }

    // --- Step 2: Compute the tiled grid layout ---
    // This arranges all snapshotted windows into a grid that fits the screen,
    // with space reserved at the top for desktop thumbnails.
    compute_tiled_layout(&mc, wm->root_w, wm->root_h);

    // --- Step 2.5: Render Space thumbnails ---
    // Pre-render a miniaturized preview of each Space's desktop into a small
    // texture. These thumbnails are shown at the top of the Mission Control
    // overlay so the user can see what's on each virtual desktop at a glance.
    mc_ensure_thumbnails(wm);

    // Clear any leftover drag state from a previous Mission Control session.
    memset(&drag, 0, sizeof(drag));

    // --- Step 3: Start the enter animation ---
    mc.animating_in  = true;
    mc.animating_out = false;
    mc.active        = false;  // Not fully active until animation finishes
    mc.anim_start    = get_time();
    mc.anim_duration = 0.3;    // 300ms for a smooth, snappy transition

    // Reset hover state.
    mc.hover_window = -1;
    mc.hover_space  = -1;

    // --- Step 4: Grab keyboard and pointer ---
    // While Mission Control is shown, we want ALL input to go through our
    // event handler (mc_handle_event) instead of being dispatched to windows.
    // XGrabKeyboard and XGrabPointer achieve this by routing all events to us.
    XGrabKeyboard(wm->dpy, wm->root, True,
                  GrabModeAsync, GrabModeAsync, CurrentTime);
    XGrabPointer(wm->dpy, wm->root, True,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);

    fprintf(stderr, "[Mission Control] Tiled %d windows into grid\n", mc.tiled_count);
}

// Exit Mission Control (animate back to normal desktop view).
//
// focus_window: if not None, this window will be raised and focused after
//               the animation finishes. Pass None to keep the current focus.
void mc_exit(AuraWM *wm, Window focus_window)
{
    // Don't exit if we're already exiting or not in Mission Control.
    if (mc.animating_out) return;
    if (!mc.active && !mc.animating_in) return;

    fprintf(stderr, "[Mission Control] Exiting overview\n");

    // Start the exit animation (reverse of the enter animation).
    mc.animating_out = true;
    mc.animating_in  = false;
    mc.active        = false;
    mc.anim_start    = get_time();
    mc.anim_duration = 0.3;  // Same duration as enter for symmetry

    // If the user clicked a specific window, focus it after the animation.
    // We store the window ID so mc_update() can apply the focus when done.
    // We re-use hover_window as a "pending focus" indicator by setting it
    // to the index of the clicked window, or -1 for no change.
    if (focus_window != None) {
        // Find the tiled window index for this X window.
        for (int i = 0; i < mc.tiled_count; i++) {
            if (mc.tiled_windows[i].xwin == focus_window) {
                mc.hover_window = i;
                break;
            }
        }

        // Raise and focus the window immediately so it's on top when the
        // animation finishes and everything returns to normal positions.
        Client *c = wm_find_client(wm, focus_window);
        if (c) {
            wm_focus_client(wm, c);
            XRaiseWindow(wm->dpy, c->frame);
        }
    }

    (void)wm;  // Suppress unused warning in case no focus logic runs
}


// ============================================================================
//  Tiling algorithm
// ============================================================================
//
// This computes where each window should be placed in the grid overview.
// The screen is divided into cells based on the number of windows. Each
// window is scaled to fit its cell while preserving its aspect ratio.
//
// Layout structure:
//   - Top 80px: reserved for Space thumbnails (desktop previews)
//   - Remaining area: divided into a grid of equal-sized cells
//   - 20px padding on all sides and between windows
//
// The grid dimensions (rows x columns) are chosen to best fill the available
// space. We use sqrt() to approximate a square grid, then adjust based on
// the screen's aspect ratio.

static void compute_tiled_layout(MissionControlState *state, int screen_w, int screen_h)
{
    int n = state->tiled_count;
    if (n == 0) return;

    // Reserve space: 80px at top for Space thumbnails, 40px at bottom for padding.
    int area_y = 80;                         // Y coordinate where the grid starts
    int area_h = screen_h - area_y - 40;     // Available height for windows
    int area_w = screen_w - 40;              // Available width (20px padding each side)

    // Calculate grid dimensions.
    // We want a grid that approximates the screen's aspect ratio.
    // cols = ceil(sqrt(n * aspect_ratio)) gives a good starting point.
    // rows = ceil(n / cols) fills in the remaining dimension.
    int cols = (int)ceilf(sqrtf((float)n * (float)area_w / (float)area_h));
    int rows = (int)ceilf((float)n / (float)cols);

    // Size of each cell in the grid (all cells are the same size).
    float cell_w = (float)area_w / (float)cols;
    float cell_h = (float)area_h / (float)rows;

    // Padding between windows inside each cell (in pixels).
    float padding = 20.0f;

    for (int i = 0; i < n; i++) {
        // Figure out which row and column this window goes in.
        int col = i % cols;
        int row = i / cols;

        // Calculate the center of this cell in screen coordinates.
        // 20px left margin + column offset + half a cell width.
        float cx = 20.0f + (float)col * cell_w + cell_w / 2.0f;
        float cy = (float)area_y + (float)row * cell_h + cell_h / 2.0f;

        // Scale the window to fit inside the cell while keeping its original
        // aspect ratio. We compute the scale factor for both dimensions and
        // pick the smaller one so the window fits entirely within the cell.
        float win_w = state->tiled_windows[i].orig_w;
        float win_h = state->tiled_windows[i].orig_h;

        // Avoid division by zero for zero-sized windows.
        if (win_w < 1.0f) win_w = 1.0f;
        if (win_h < 1.0f) win_h = 1.0f;

        float scale_x = (cell_w - padding) / win_w;  // Scale to fit cell width
        float scale_y = (cell_h - padding) / win_h;  // Scale to fit cell height
        float scale   = fminf(scale_x, scale_y);     // Use the tighter constraint
        scale = fminf(scale, 1.0f);                   // Never scale UP (keep 1:1 max)

        // Apply the scale and center the window in its cell.
        state->tiled_windows[i].target_w = win_w * scale;
        state->tiled_windows[i].target_h = win_h * scale;
        state->tiled_windows[i].target_x = cx - state->tiled_windows[i].target_w / 2.0f;
        state->tiled_windows[i].target_y = cy - state->tiled_windows[i].target_h / 2.0f;
    }
}


// ============================================================================
//  Space thumbnail rendering
// ============================================================================
//
// Each Space (virtual desktop) gets a small thumbnail preview drawn at the
// top of Mission Control. These thumbnails are rendered off-screen into an
// FBO at a reduced resolution, then stored as a texture on the MCSpace struct.
//
// We use a single shared FBO and re-render it for each Space that needs an
// update. The result is copied into the Space's own thumbnail_tex.

// Render (or re-render) the thumbnail for a single Space.
//
// This creates a miniaturized view of all the windows on the given Space by:
//   1. Binding a small FBO (200x125 pixels).
//   2. Setting up an orthographic projection that maps the full screen
//      dimensions into the tiny FBO resolution (effectively a zoom-out).
//   3. Drawing each window in the Space at its scaled-down position.
//   4. Copying the FBO's texture into the Space's thumbnail_tex.
//
// space_index: the index into mc.spaces[] of the Space to render.
static void mc_render_space_thumbnail(AuraWM *wm, int space_index)
{
    MCSpace *space = &mc.spaces[space_index];

    // Get the shader programs and projection from Crystal's compositor.
    ShaderPrograms *progs = crystal_get_shaders();
    if (!progs || !progs->basic) return;  // Shaders not ready yet

    // Create the shared FBO on first use. All thumbnails share the same FBO
    // — we render into it one Space at a time and copy the result out.
    if (thumb_fbo == 0) {
        thumb_fbo = shaders_create_fbo(THUMB_FBO_W, THUMB_FBO_H, &thumb_fbo_tex);
        if (thumb_fbo == 0) {
            fprintf(stderr, "[Mission Control] Failed to create thumbnail FBO\n");
            return;
        }
    }

    // If this Space does not have its own thumbnail texture yet, create one.
    // This texture persists across frames — we only re-render it when the
    // Space's window layout changes (thumbnail_dirty flag).
    if (space->thumbnail_tex == 0) {
        glGenTextures(1, &space->thumbnail_tex);
        glBindTexture(GL_TEXTURE_2D, space->thumbnail_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, THUMB_FBO_W, THUMB_FBO_H,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    // --- Render into the FBO ---
    shaders_bind_fbo(thumb_fbo);
    glViewport(0, 0, THUMB_FBO_W, THUMB_FBO_H);

    // Clear to a dark background (the Space backdrop color).
    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Build a scaled-down orthographic projection. This maps the full screen
    // coordinate range (0..root_w, 0..root_h) into the tiny FBO. Any window
    // drawn at its real screen coordinates will automatically appear scaled.
    float thumb_proj[16];
    shaders_ortho(thumb_proj,
                  0.0f, (float)wm->root_w,    // left, right (full screen width)
                  (float)wm->root_h, 0.0f,    // bottom, top (flipped Y for GL)
                  -1.0f, 1.0f);               // near, far (2D compositing)

    // Draw each window that belongs to this Space.
    for (int i = 0; i < space->window_count; i++) {
        // Find the Client struct for this window so we can get its position.
        Client *c = wm_find_client(wm, space->windows[i]);
        if (!c) continue;

        // Look up the window's GL texture from Crystal's texture cache.
        GLuint tex = crystal_get_window_texture_id(c->frame);
        if (tex == 0) tex = crystal_get_window_texture_id(c->client);

        if (tex != 0) {
            // Draw the window texture at its real screen coordinates.
            // The orthographic projection handles the scaling automatically.
            shaders_use(progs->basic);
            shaders_set_projection(progs->basic, thumb_proj);
            shaders_set_texture(progs->basic, 0);
            shaders_set_alpha(progs->basic, 1.0f);
            glBindTexture(GL_TEXTURE_2D, tex);
            shaders_draw_quad((float)c->x, (float)c->y,
                              (float)c->w, (float)c->h);
        } else {
            // No texture available — draw a placeholder rectangle.
            shaders_use(progs->basic);
            shaders_set_projection(progs->basic, thumb_proj);
            shaders_set_color(progs->basic, 0.3f, 0.3f, 0.3f, 0.8f);
            shaders_draw_quad((float)c->x, (float)c->y,
                              (float)c->w, (float)c->h);
        }
    }

    // --- Copy the FBO result into the Space's persistent thumbnail texture ---
    // We bind the Space's thumbnail texture and copy the FBO's contents into it
    // using glCopyTexSubImage2D, which reads from the current framebuffer.
    glBindTexture(GL_TEXTURE_2D, space->thumbnail_tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, THUMB_FBO_W, THUMB_FBO_H);

    // --- Restore normal rendering state ---
    shaders_unbind_fbo();
    glViewport(0, 0, wm->root_w, wm->root_h);

    // Mark the thumbnail as up-to-date.
    space->thumbnail_dirty = false;
}

// Ensure all Space thumbnails are up to date.
//
// Iterates through all Spaces and re-renders the thumbnail for any that
// have been marked dirty (thumbnail_dirty = true). This is called from
// mc_enter() and whenever a Space's window list changes.
static void mc_ensure_thumbnails(AuraWM *wm)
{
    for (int i = 0; i < mc.space_count; i++) {
        if (mc.spaces[i].thumbnail_dirty) {
            mc_render_space_thumbnail(wm, i);
        }
    }
}


// ============================================================================
//  Per-frame update
// ============================================================================

// Advance Mission Control's animation state.
//
// This is called once per frame from the main compositor loop. It checks
// whether the enter or exit animation has finished and transitions the state
// accordingly.
//
// Returns true if Mission Control still needs rendering (animation in progress
// or overlay is active). Returns false when fully dismissed.
bool mc_update(AuraWM *wm)
{
    // Nothing to do if Mission Control is completely idle.
    if (!mc.active && !mc.animating_in && !mc.animating_out) {
        return false;
    }

    double now = get_time();
    double elapsed = now - mc.anim_start;

    // --- Handle enter animation completion ---
    if (mc.animating_in && elapsed >= mc.anim_duration) {
        mc.animating_in = false;
        mc.active = true;  // Fully shown — accept hover/click input now
        fprintf(stderr, "[Mission Control] Enter animation complete\n");
    }

    // --- Handle exit animation completion ---
    if (mc.animating_out && elapsed >= mc.anim_duration) {
        mc.animating_out = false;
        mc.active = false;

        // Release the keyboard and pointer grabs so normal input resumes.
        XUngrabKeyboard(wm->dpy, CurrentTime);
        XUngrabPointer(wm->dpy, CurrentTime);

        fprintf(stderr, "[Mission Control] Exit animation complete\n");

        // Mission Control is fully dismissed — no more rendering needed.
        return false;
    }

    // --- Handle slide animation completion ---
    // The slide animation plays when switching between Spaces. It runs
    // independently of the enter/exit animation.
    if (slide_anim.active) {
        double slide_elapsed = now - slide_anim.start_time;
        if (slide_elapsed >= slide_anim.duration) {
            slide_anim.active = false;
        }
    }

    // Still animating or fully active — keep rendering.
    return true;
}


// ============================================================================
//  Drawing — Space thumbnails
// ============================================================================
//
// Draws a row of small rectangles at the top of the screen, one for each
// Space (virtual desktop). The active Space is highlighted. Each thumbnail
// shows a miniaturized preview of that desktop's windows.

static void draw_space_thumbnails(AuraWM *wm, GLuint shader, float *projection,
                                  float ease)
{
    // Each thumbnail is a small rectangle at the top of the screen.
    // Width scales with the number of spaces so they all fit.
    float thumb_h = 60.0f;                          // Height of each thumbnail
    float thumb_w = 100.0f;                         // Width of each thumbnail
    float gap     = 10.0f;                          // Gap between thumbnails
    float total_w = (float)mc.space_count * (thumb_w + gap) - gap;  // Total row width
    float start_x = ((float)wm->root_w - total_w) / 2.0f;  // Center horizontally
    float y       = 10.0f;                          // 10px from the top of the screen

    // Fade the entire thumbnail row in/out with the Mission Control animation.
    // "ease" goes from 0 (hidden) to 1 (fully visible).
    float alpha = 0.8f * ease;

    for (int i = 0; i < mc.space_count; i++) {
        float x = start_x + (float)i * (thumb_w + gap);

        // Draw the thumbnail background.
        // The active Space gets a brighter background so the user can tell
        // which desktop they're currently on.
        shaders_use(shader);
        shaders_set_projection(shader, projection);

        if (i == mc.current_space) {
            // Active Space: slightly brighter background (white at 30% opacity).
            shaders_set_color(shader, 1.0f, 1.0f, 1.0f, 0.3f * ease);
        } else if (i == mc.hover_space) {
            // Hovered Space: subtle highlight (white at 20% opacity).
            shaders_set_color(shader, 1.0f, 1.0f, 1.0f, 0.2f * ease);
        } else {
            // Inactive Space: dark background (white at 10% opacity).
            shaders_set_color(shader, 1.0f, 1.0f, 1.0f, 0.1f * ease);
        }

        shaders_draw_quad(x, y, thumb_w, thumb_h);

        // Draw a border around each thumbnail for clarity.
        // Thin white lines at low opacity create a subtle frame.
        shaders_set_color(shader, 1.0f, 1.0f, 1.0f, 0.4f * ease);
        // Top edge
        shaders_draw_quad(x, y, thumb_w, 1.0f);
        // Bottom edge
        shaders_draw_quad(x, y + thumb_h - 1.0f, thumb_w, 1.0f);
        // Left edge
        shaders_draw_quad(x, y, 1.0f, thumb_h);
        // Right edge
        shaders_draw_quad(x + thumb_w - 1.0f, y, 1.0f, thumb_h);

        // If this Space has a pre-rendered thumbnail texture, draw it inside
        // the border so the user can see a miniaturized version of the desktop.
        if (mc.spaces[i].thumbnail_tex != 0) {
            shaders_use(shader);
            shaders_set_projection(shader, projection);
            shaders_set_texture(shader, 0);
            shaders_set_alpha(shader, alpha);
            glBindTexture(GL_TEXTURE_2D, mc.spaces[i].thumbnail_tex);
            shaders_draw_quad(x + 2.0f, y + 2.0f, thumb_w - 4.0f, thumb_h - 4.0f);
        }
    }

    // Draw the "+" button to the right of the last thumbnail.
    // Clicking this adds a new Space.
    float plus_x = start_x + (float)mc.space_count * (thumb_w + gap);
    float plus_w = 30.0f;
    float plus_h = thumb_h;

    // Background for the + button.
    shaders_use(shader);
    shaders_set_projection(shader, projection);
    shaders_set_color(shader, 1.0f, 1.0f, 1.0f, 0.1f * ease);
    shaders_draw_quad(plus_x, y, plus_w, plus_h);

    // Draw a "+" sign using two thin rectangles (a cross shape).
    shaders_set_color(shader, 1.0f, 1.0f, 1.0f, 0.5f * ease);
    // Horizontal bar of the "+"
    shaders_draw_quad(plus_x + 8.0f, y + plus_h / 2.0f - 1.0f, plus_w - 16.0f, 2.0f);
    // Vertical bar of the "+"
    shaders_draw_quad(plus_x + plus_w / 2.0f - 1.0f, y + 15.0f, 2.0f, plus_h - 30.0f);
}


// ============================================================================
//  Drawing — main overlay
// ============================================================================

// Draw the full Mission Control overlay.
//
// This is the main drawing function, called from crystal_composite() when
// Mission Control is visible or animating. It layers the overlay on top of
// the normal desktop:
//   1. Dark semi-transparent backdrop (dims the wallpaper).
//   2. Space thumbnails along the top.
//   3. Tiled windows at their animated positions.
//   4. Hover highlight on the window/space under the cursor.
void mc_draw(AuraWM *wm, GLuint basic_shader, float *projection)
{
    // Don't draw anything if Mission Control is fully hidden.
    if (!mc.active && !mc.animating_in && !mc.animating_out) return;

    // --- Calculate animation progress ---
    // "t" is the raw linear progress from 0 to 1.
    // "ease" is the smoothed value after applying an easing curve.
    double now = get_time();
    float t = (float)((now - mc.anim_start) / mc.anim_duration);
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;

    // Apply cubic ease-in-out for a smooth acceleration/deceleration.
    // This uses the same easing function as the Crystal animation framework.
    float ease = anim_ease(EASE_IN_OUT_CUBIC, t);

    // When exiting, reverse the easing so the animation plays backward
    // (from overview back to normal positions).
    if (mc.animating_out) ease = 1.0f - ease;

    // --- Layer 1: Dark overlay ---
    // A semi-transparent black rectangle covering the entire screen.
    // This visually separates the tiled windows from the desktop wallpaper.
    // The opacity fades from 0 to 40% as Mission Control enters.
    shaders_use(basic_shader);
    shaders_set_projection(basic_shader, projection);
    shaders_set_color(basic_shader, 0.0f, 0.0f, 0.0f, 0.4f * ease);
    shaders_draw_quad(0.0f, 0.0f, (float)wm->root_w, (float)wm->root_h);

    // --- Layer 2: Space thumbnails ---
    // Row of desktop previews at the top of the screen.
    draw_space_thumbnails(wm, basic_shader, projection, ease);

    // --- Layer 3: Tiled windows ---
    // Each window interpolates between its original desktop position and its
    // grid cell position based on the animation progress.
    for (int i = 0; i < mc.tiled_count; i++) {
        // Skip the window being dragged — we draw it last so it appears on top
        // of all other windows, giving clear visual feedback during the drag.
        if (drag.active && drag.past_threshold && i == drag.window_idx) continue;

        // Lerp (linear interpolation) between original and target positions.
        // When ease = 0, the window is at its original position.
        // When ease = 1, the window is at its grid cell position.
        float x = mc.tiled_windows[i].orig_x
                + (mc.tiled_windows[i].target_x - mc.tiled_windows[i].orig_x) * ease;
        float y = mc.tiled_windows[i].orig_y
                + (mc.tiled_windows[i].target_y - mc.tiled_windows[i].orig_y) * ease;
        float w = mc.tiled_windows[i].orig_w
                + (mc.tiled_windows[i].target_w - mc.tiled_windows[i].orig_w) * ease;
        float h = mc.tiled_windows[i].orig_h
                + (mc.tiled_windows[i].target_h - mc.tiled_windows[i].orig_h) * ease;

        // --- Hover highlight ---
        // If the mouse is over this window, draw a blue border around it
        // so the user knows they can click to focus it.
        if (i == mc.hover_window && (mc.active || mc.animating_in)) {
            // AuraOS blue highlight (slightly translucent).
            shaders_set_color(basic_shader, 0.22f, 0.46f, 0.84f, 0.5f * ease);
            // Draw a rectangle slightly larger than the window (4px border).
            shaders_draw_quad(x - 4.0f, y - 4.0f, w + 8.0f, h + 8.0f);
        }

        // --- Window texture ---
        // Draw the actual window contents using its GL texture.
        // If the texture is 0 (not yet bound), we draw a placeholder rectangle.
        if (mc.tiled_windows[i].texture != 0) {
            // Activate the basic shader and configure it to sample from
            // texture unit 0 at full opacity. Then bind the window's texture
            // and draw a quad at the interpolated position.
            shaders_use(basic_shader);
            shaders_set_projection(basic_shader, projection);
            shaders_set_texture(basic_shader, 0);
            shaders_set_alpha(basic_shader, ease);
            glBindTexture(GL_TEXTURE_2D, mc.tiled_windows[i].texture);
            shaders_draw_quad(x, y, w, h);
        } else {
            // Placeholder: a dark gray rectangle with a border.
            // This shows up when the window texture hasn't been bound yet.
            shaders_set_color(basic_shader, 0.2f, 0.2f, 0.2f, 0.8f * ease);
            shaders_draw_quad(x, y, w, h);

            // Light border so the placeholder is visible against the overlay.
            shaders_set_color(basic_shader, 0.5f, 0.5f, 0.5f, 0.6f * ease);
            shaders_draw_quad(x, y, w, 1.0f);           // Top
            shaders_draw_quad(x, y + h - 1.0f, w, 1.0f); // Bottom
            shaders_draw_quad(x, y, 1.0f, h);           // Left
            shaders_draw_quad(x + w - 1.0f, y, 1.0f, h); // Right
        }
    }

    // --- Layer 4: Dragged window ---
    // If the user is dragging a window to a different Space, draw it on top
    // of everything else at the cursor position. A slight scale-up and a
    // semi-transparent blue tint indicate the window is "lifted" from the grid.
    if (drag.active && drag.past_threshold && drag.window_idx >= 0
        && drag.window_idx < mc.tiled_count) {
        int di = drag.window_idx;
        float dw = mc.tiled_windows[di].target_w * 0.85f;  // Slightly smaller for "lifted" look
        float dh = mc.tiled_windows[di].target_h * 0.85f;
        float dx = drag.current_x - dw / 2.0f;
        float dy = drag.current_y - dh / 2.0f;

        // Draw a subtle shadow behind the dragged window.
        shaders_use(basic_shader);
        shaders_set_projection(basic_shader, projection);
        shaders_set_color(basic_shader, 0.0f, 0.0f, 0.0f, 0.3f);
        shaders_draw_quad(dx + 4.0f, dy + 4.0f, dw, dh);

        // Draw the window texture (or placeholder if no texture).
        if (mc.tiled_windows[di].texture != 0) {
            shaders_use(basic_shader);
            shaders_set_projection(basic_shader, projection);
            shaders_set_texture(basic_shader, 0);
            shaders_set_alpha(basic_shader, 0.85f);
            glBindTexture(GL_TEXTURE_2D, mc.tiled_windows[di].texture);
            shaders_draw_quad(dx, dy, dw, dh);
        } else {
            shaders_set_color(basic_shader, 0.2f, 0.2f, 0.2f, 0.7f);
            shaders_draw_quad(dx, dy, dw, dh);
        }

        // Blue highlight border on the dragged window.
        shaders_set_color(basic_shader, 0.22f, 0.46f, 0.84f, 0.7f);
        shaders_draw_quad(dx, dy, dw, 2.0f);             // Top
        shaders_draw_quad(dx, dy + dh - 2.0f, dw, 2.0f); // Bottom
        shaders_draw_quad(dx, dy, 2.0f, dh);             // Left
        shaders_draw_quad(dx + dw - 2.0f, dy, 2.0f, dh); // Right
    }
}


// ============================================================================
//  Input handling
// ============================================================================
//
// When Mission Control is active, all input is grabbed (keyboard + pointer)
// and routed through this function. It handles:
//   - Mouse motion:  update which window/space the cursor is hovering over.
//   - Mouse click:   focus a window, switch a space, or exit.
//   - Keyboard:      Escape exits, arrow keys cycle spaces.

bool mc_handle_event(AuraWM *wm, XEvent *ev)
{
    // If Mission Control is not active and not animating, don't consume events.
    if (!mc.active && !mc.animating_in && !mc.animating_out) {
        return false;
    }

    // During animations (before fully active), only handle Escape to cancel.
    // Don't process hovers or clicks while windows are still moving.
    if (!mc.active && mc.animating_in) {
        if (ev->type == KeyPress) {
            KeySym key = XLookupKeysym(&ev->xkey, 0);
            if (key == XK_Escape) {
                mc_exit(wm, None);
                return true;
            }
        }
        return true;  // Consume all events during enter animation
    }

    switch (ev->type) {

    // --- Mouse motion: update hover state and handle drag ---
    case MotionNotify: {
        int mx = ev->xmotion.x;
        int my = ev->xmotion.y;

        // --- Drag handling ---
        // If a drag is in progress, update the dragged window's position and
        // check which Space thumbnail the cursor is hovering over (so we can
        // highlight the drop target).
        if (drag.active) {
            // Update the dragged window's draw position to follow the cursor.
            drag.current_x = (float)mx;
            drag.current_y = (float)my;

            // Check if the cursor has moved far enough from the initial click
            // to count as a real drag (not just a sloppy click). This prevents
            // accidental drags when the user just wants to click a window.
            if (!drag.past_threshold) {
                int dx = mx - drag.start_x;
                int dy = my - drag.start_y;
                if (dx * dx + dy * dy > DRAG_THRESHOLD * DRAG_THRESHOLD) {
                    drag.past_threshold = true;
                }
            }

            // While dragging, check if the cursor is over a Space thumbnail
            // so we can highlight it as a potential drop target.
            mc.hover_space = -1;
            if (my >= 10 && my <= 70) {
                float thumb_w = 100.0f;
                float gap     = 10.0f;
                float total_w = (float)mc.space_count * (thumb_w + gap) - gap;
                float start_x = ((float)wm->root_w - total_w) / 2.0f;

                for (int i = 0; i < mc.space_count; i++) {
                    float sx = start_x + (float)i * (thumb_w + gap);
                    if ((float)mx >= sx && (float)mx <= sx + thumb_w) {
                        mc.hover_space = i;
                        break;
                    }
                }
            }

            return true;
        }

        // --- Normal hover tracking (no drag active) ---
        // Reset hover state each frame and re-check what's under the cursor.
        mc.hover_window = -1;
        mc.hover_space  = -1;

        // Check if the cursor is over any tiled window.
        // Iterate in reverse order so windows drawn on top (higher index)
        // take priority — matching visual stacking order.
        for (int i = mc.tiled_count - 1; i >= 0; i--) {
            float wx = mc.tiled_windows[i].target_x;
            float wy = mc.tiled_windows[i].target_y;
            float ww = mc.tiled_windows[i].target_w;
            float wh = mc.tiled_windows[i].target_h;

            if ((float)mx >= wx && (float)mx <= wx + ww &&
                (float)my >= wy && (float)my <= wy + wh) {
                mc.hover_window = i;
                break;  // Found the top-most window under cursor
            }
        }

        // Check if the cursor is over any Space thumbnail.
        // Thumbnails are at the top of the screen (y = 10, height = 60).
        if (my >= 10 && my <= 70) {
            float thumb_w = 100.0f;
            float gap     = 10.0f;
            float total_w = (float)mc.space_count * (thumb_w + gap) - gap;
            float start_x = ((float)wm->root_w - total_w) / 2.0f;

            for (int i = 0; i < mc.space_count; i++) {
                float sx = start_x + (float)i * (thumb_w + gap);
                if ((float)mx >= sx && (float)mx <= sx + thumb_w) {
                    mc.hover_space = i;
                    break;
                }
            }

            // Check if cursor is over the "+" button (add new Space).
            float plus_x = start_x + (float)mc.space_count * (thumb_w + gap);
            float plus_w = 30.0f;
            if ((float)mx >= plus_x && (float)mx <= plus_x + plus_w) {
                // Use space_count as a sentinel value meaning "hovering the + button"
                mc.hover_space = mc.space_count;
            }
        }

        return true;  // Event consumed
    }

    // --- Mouse button press: start a drag or handle Space/button clicks ---
    case ButtonPress: {
        int mx = ev->xbutton.x;
        int my = ev->xbutton.y;

        // Did the user click on a Space thumbnail?
        // Space thumbnails are in the top row (y = 10 to y = 70).
        if (my >= 10 && my <= 70) {
            float thumb_w = 100.0f;
            float gap     = 10.0f;
            float total_w = (float)mc.space_count * (thumb_w + gap) - gap;
            float start_x = ((float)wm->root_w - total_w) / 2.0f;

            // Check each Space thumbnail.
            for (int i = 0; i < mc.space_count; i++) {
                float sx = start_x + (float)i * (thumb_w + gap);
                if ((float)mx >= sx && (float)mx <= sx + thumb_w) {
                    // Switch to this Space and exit Mission Control.
                    if (i != mc.current_space) {
                        mc_switch_space(wm, mc.spaces[i].id);
                    }
                    mc_exit(wm, None);
                    return true;
                }
            }

            // Check the "+" button — adds a new Space (virtual desktop).
            float plus_x = start_x + (float)mc.space_count * (thumb_w + gap);
            float plus_w = 30.0f;
            if ((float)mx >= plus_x && (float)mx <= plus_x + plus_w) {
                mc_add_space(wm);
                // Stay in Mission Control so the user can drag windows
                // to the newly created Space.
                return true;
            }
        }

        // Did the user click on a tiled window?
        // Instead of immediately exiting, we start a drag operation. If the
        // user releases the mouse without moving past the threshold, we treat
        // it as a click (focus the window and exit). If they drag past the
        // threshold, the window follows the cursor and can be dropped on a
        // Space thumbnail.
        for (int i = mc.tiled_count - 1; i >= 0; i--) {
            float wx = mc.tiled_windows[i].target_x;
            float wy = mc.tiled_windows[i].target_y;
            float ww = mc.tiled_windows[i].target_w;
            float wh = mc.tiled_windows[i].target_h;

            if ((float)mx >= wx && (float)mx <= wx + ww &&
                (float)my >= wy && (float)my <= wy + wh) {
                // Begin tracking a potential drag on this window.
                drag.active         = true;
                drag.window_idx     = i;
                drag.start_x        = mx;
                drag.start_y        = my;
                drag.current_x      = (float)mx;
                drag.current_y      = (float)my;
                drag.past_threshold = false;

                // Record the offset from the cursor to the window's center
                // so the window doesn't jump when the drag starts.
                drag.offset_x = (float)mx - (wx + ww / 2.0f);
                drag.offset_y = (float)my - (wy + wh / 2.0f);

                return true;
            }
        }

        // Clicked on empty space (not on a window or thumbnail).
        // Exit Mission Control without changing focus.
        mc_exit(wm, None);
        return true;
    }

    // --- Mouse button release: complete a drag or treat as a click ---
    case ButtonRelease: {
        int mx = ev->xbutton.x;
        int my = ev->xbutton.y;

        if (drag.active) {
            int dragged_idx = drag.window_idx;
            bool was_real_drag = drag.past_threshold;

            // End the drag operation.
            drag.active = false;
            drag.past_threshold = false;

            if (was_real_drag) {
                // The user completed a real drag (moved past the threshold).
                // Check if they released over a Space thumbnail — if so, move
                // the window to that Space.
                if (my >= 10 && my <= 70) {
                    float thumb_w = 100.0f;
                    float gap     = 10.0f;
                    float total_w = (float)mc.space_count * (thumb_w + gap) - gap;
                    float start_x = ((float)wm->root_w - total_w) / 2.0f;

                    for (int i = 0; i < mc.space_count; i++) {
                        float sx = start_x + (float)i * (thumb_w + gap);
                        if ((float)mx >= sx && (float)mx <= sx + thumb_w) {
                            // Move the dragged window to this Space.
                            Window win = mc.tiled_windows[dragged_idx].xwin;
                            mc_move_window_to_space(wm, win, mc.spaces[i].id);

                            // Mark both the source and destination Space
                            // thumbnails as dirty so they get re-rendered.
                            mc.spaces[mc.current_space].thumbnail_dirty = true;
                            mc.spaces[i].thumbnail_dirty = true;
                            mc_ensure_thumbnails(wm);

                            // Remove the window from the tiled layout since
                            // it no longer belongs to the current Space.
                            for (int j = dragged_idx; j < mc.tiled_count - 1; j++) {
                                mc.tiled_windows[j] = mc.tiled_windows[j + 1];
                            }
                            mc.tiled_count--;

                            // Recompute the grid layout to fill the gap.
                            compute_tiled_layout(&mc, wm->root_w, wm->root_h);

                            fprintf(stderr, "[Mission Control] Dragged window "
                                    "0x%lx to %s\n",
                                    (unsigned long)win, mc.spaces[i].name);
                            return true;
                        }
                    }
                }

                // Released over empty space or outside a thumbnail — the
                // window snaps back to its original grid position (which
                // happens automatically since we stop drawing it at the
                // drag position).
                return true;
            } else {
                // The user didn't move past the threshold — treat as a click.
                // Focus the clicked window and exit Mission Control.
                if (dragged_idx >= 0 && dragged_idx < mc.tiled_count) {
                    mc_exit(wm, mc.tiled_windows[dragged_idx].xwin);
                }
                return true;
            }
        }

        return true;  // Consume the event
    }

    // --- Keyboard: Escape to exit, arrow keys to cycle Spaces ---
    case KeyPress: {
        KeySym key = XLookupKeysym(&ev->xkey, 0);

        switch (key) {
        case XK_Escape:
            // Cancel any active drag and exit Mission Control.
            drag.active = false;
            mc_exit(wm, None);
            return true;

        case XK_Left:
            // Switch to the previous Space (wrap around to the last one).
            if (mc.space_count > 1) {
                int prev = (mc.current_space - 1 + mc.space_count) % mc.space_count;
                mc_switch_space(wm, mc.spaces[prev].id);
                // Re-tile windows for the new Space.
                mc_enter(wm);
            }
            return true;

        case XK_Right:
            // Switch to the next Space (wrap around to the first one).
            if (mc.space_count > 1) {
                int next = (mc.current_space + 1) % mc.space_count;
                mc_switch_space(wm, mc.spaces[next].id);
                // Re-tile windows for the new Space.
                mc_enter(wm);
            }
            return true;

        default:
            break;
        }

        return true;  // Consume all key events while MC is active
    }

    default:
        // Consume all events while Mission Control is active so they don't
        // leak through to windows underneath.
        return true;
    }
}


// ============================================================================
//  State query
// ============================================================================

// Check if Mission Control is currently active (visible or animating).
//
// This is used by the main compositor loop to decide whether to call mc_draw()
// and mc_update(), and by the event loop to route events through
// mc_handle_event().
bool mc_is_active(void)
{
    return mc.active || mc.animating_in || mc.animating_out;
}


// ============================================================================
//  Space management — helper functions
// ============================================================================

// Find the array index of a Space by its unique ID.
// Returns the index (0 to space_count-1), or -1 if the ID is not found.
static int find_space_index(int space_id)
{
    for (int i = 0; i < mc.space_count; i++) {
        if (mc.spaces[i].id == space_id) {
            return i;
        }
    }
    return -1;
}

// Find a window in a Space's window list.
// Returns the index within the Space's window array, or -1 if not found.
static int find_window_in_space(MCSpace *space, Window win)
{
    for (int i = 0; i < space->window_count; i++) {
        if (space->windows[i] == win) {
            return i;
        }
    }
    return -1;
}

// Map (make visible) all windows belonging to a given Space.
// Called when switching to this Space so its windows appear on screen.
static void map_space_windows(AuraWM *wm, int space_index)
{
    MCSpace *space = &mc.spaces[space_index];
    for (int i = 0; i < space->window_count; i++) {
        Client *c = wm_find_client(wm, space->windows[i]);
        if (c && !c->mapped) {
            // Map the frame window (which is the parent of the client window).
            // This makes both the frame and client visible.
            XMapWindow(wm->dpy, c->frame);
            XMapWindow(wm->dpy, c->client);
            c->mapped = true;
        }
    }
}

// Unmap (hide) all windows belonging to a given Space.
// Called when switching away from this Space so its windows disappear.
static void unmap_space_windows(AuraWM *wm, int space_index)
{
    MCSpace *space = &mc.spaces[space_index];
    for (int i = 0; i < space->window_count; i++) {
        Client *c = wm_find_client(wm, space->windows[i]);
        if (c && c->mapped) {
            // Unmap the frame window. This hides both the frame and the
            // client window inside it.
            XUnmapWindow(wm->dpy, c->frame);
            c->mapped = false;
        }
    }
}


// ============================================================================
//  Space management — public API
// ============================================================================

// Add a new empty virtual desktop (Space).
//
// The new Space starts with no windows. The user can then drag windows into
// it from the Mission Control overview.
//
// Returns the new Space's unique ID, or -1 if the maximum number of Spaces
// (MC_MAX_SPACES = 16) has been reached.
int mc_add_space(AuraWM *wm)
{
    (void)wm;  // Not directly needed but kept for API consistency

    if (mc.space_count >= MC_MAX_SPACES) {
        fprintf(stderr, "[Mission Control] Cannot add Space: maximum (%d) reached\n",
                MC_MAX_SPACES);
        return -1;
    }

    // Initialize the new Space at the end of the array.
    MCSpace *space = &mc.spaces[mc.space_count];
    space->id = next_space_id++;
    snprintf(space->name, sizeof(space->name), "Desktop %d", space->id);

    // Allocate the window list with room for MAX_CLIENTS windows.
    space->windows      = calloc(MAX_CLIENTS, sizeof(Window));
    space->window_count = 0;

    // No thumbnail yet — mark as dirty so it gets rendered.
    space->thumbnail_tex   = 0;
    space->thumbnail_dirty = true;

    mc.space_count++;

    fprintf(stderr, "[Mission Control] Added %s (id=%d, total=%d)\n",
            space->name, space->id, mc.space_count);

    return space->id;
}

// Remove a virtual desktop (Space) and move its windows elsewhere.
//
// Windows that belonged to the removed Space are transferred to the Space
// that comes before it in the array (or the first Space if the removed one
// was first). You cannot remove the last remaining Space.
//
// space_id: the unique ID of the Space to remove.
void mc_remove_space(AuraWM *wm, int space_id)
{
    // Find the Space to remove.
    int idx = find_space_index(space_id);
    if (idx < 0) {
        fprintf(stderr, "[Mission Control] Cannot remove Space: id %d not found\n",
                space_id);
        return;
    }

    // There must always be at least one Space.
    if (mc.space_count <= 1) {
        fprintf(stderr, "[Mission Control] Cannot remove last Space\n");
        return;
    }

    // Figure out where to move the orphaned windows.
    // Prefer the Space before the removed one; if removing index 0, use index 1.
    int dest_idx = (idx > 0) ? idx - 1 : 1;
    MCSpace *dest = &mc.spaces[dest_idx];
    MCSpace *src  = &mc.spaces[idx];

    // Move all windows from the removed Space to the destination Space.
    for (int i = 0; i < src->window_count; i++) {
        // Add the window to the destination Space's list.
        if (dest->window_count < MAX_CLIENTS) {
            dest->windows[dest->window_count++] = src->windows[i];
        }
    }
    dest->thumbnail_dirty = true;

    // Free the removed Space's resources.
    free(src->windows);
    src->windows = NULL;
    if (src->thumbnail_tex) {
        glDeleteTextures(1, &src->thumbnail_tex);
    }

    // Shift all Spaces after the removed one down by one position in the array
    // to fill the gap.
    for (int i = idx; i < mc.space_count - 1; i++) {
        mc.spaces[i] = mc.spaces[i + 1];
    }
    // Zero out the now-unused last slot.
    memset(&mc.spaces[mc.space_count - 1], 0, sizeof(MCSpace));
    mc.space_count--;

    // Fix up current_space index if it was affected by the removal.
    if (mc.current_space >= mc.space_count) {
        mc.current_space = mc.space_count - 1;
    }
    if (mc.current_space == idx) {
        // We removed the active Space — switch to the destination.
        mc.current_space = (dest_idx < mc.space_count) ? dest_idx : 0;
    } else if (mc.current_space > idx) {
        // Indices shifted down — adjust.
        mc.current_space--;
    }

    // Make sure the windows from the removed Space are visible if they
    // ended up on the current (active) Space.
    if (dest_idx == mc.current_space) {
        map_space_windows(wm, mc.current_space);
    }

    fprintf(stderr, "[Mission Control] Removed Space id=%d, windows moved to %s\n",
            space_id, dest->name);
}

// Switch to a different virtual desktop (Space).
//
// This hides all windows on the current Space and shows all windows on the
// target Space. A horizontal slide animation plays during the transition
// (windows slide left or right depending on the direction of the switch).
//
// space_id: the unique ID of the Space to switch to.
void mc_switch_space(AuraWM *wm, int space_id)
{
    int idx = find_space_index(space_id);
    if (idx < 0) {
        fprintf(stderr, "[Mission Control] Cannot switch: Space id %d not found\n",
                space_id);
        return;
    }

    // Don't switch if we're already on this Space.
    if (idx == mc.current_space) return;

    fprintf(stderr, "[Mission Control] Switching from %s to %s\n",
            mc.spaces[mc.current_space].name, mc.spaces[idx].name);

    // Start a slide animation for the transition.
    // The direction determines whether windows slide left or right:
    //   - Moving to a higher-index Space: slide left (windows move left, new ones enter from right).
    //   - Moving to a lower-index Space: slide right (windows move right, new ones enter from left).
    slide_anim.active     = true;
    slide_anim.start_time = get_time();
    slide_anim.duration   = 0.3;  // 300ms — matches the enter/exit animation speed
    slide_anim.direction  = (idx > mc.current_space) ? -1.0f : 1.0f;

    // Mark the outgoing Space's thumbnail as dirty so it gets refreshed
    // the next time Mission Control is opened (its window layout may have
    // changed since the last render).
    mc.spaces[mc.current_space].thumbnail_dirty = true;

    // Hide windows on the old Space.
    unmap_space_windows(wm, mc.current_space);

    // Update the current space index.
    mc.current_space = idx;

    // Show windows on the new Space.
    map_space_windows(wm, mc.current_space);

    // Mark the new Space's thumbnail as dirty (needs re-render).
    mc.spaces[mc.current_space].thumbnail_dirty = true;
}

// Move a window from its current Space to a different one.
//
// The window is removed from its current Space's window list and added to the
// destination Space's list. If the destination Space is not the active Space,
// the window is unmapped (hidden) since it now belongs to a hidden desktop.
// If the destination IS the active Space, the window stays visible.
//
// win:      the X11 window ID of the window to move.
// space_id: the unique ID of the destination Space.
void mc_move_window_to_space(AuraWM *wm, Window win, int space_id)
{
    int dest_idx = find_space_index(space_id);
    if (dest_idx < 0) {
        fprintf(stderr, "[Mission Control] Cannot move window: Space id %d not found\n",
                space_id);
        return;
    }

    // Find which Space currently owns this window and remove it.
    bool found = false;
    for (int s = 0; s < mc.space_count; s++) {
        int w_idx = find_window_in_space(&mc.spaces[s], win);
        if (w_idx >= 0) {
            // Remove the window by shifting the remaining windows down.
            // This maintains the array order without leaving gaps.
            for (int j = w_idx; j < mc.spaces[s].window_count - 1; j++) {
                mc.spaces[s].windows[j] = mc.spaces[s].windows[j + 1];
            }
            mc.spaces[s].window_count--;
            mc.spaces[s].thumbnail_dirty = true;
            found = true;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "[Mission Control] Window 0x%lx not found in any Space\n",
                (unsigned long)win);
        return;
    }

    // Add the window to the destination Space.
    MCSpace *dest = &mc.spaces[dest_idx];
    if (dest->window_count < MAX_CLIENTS) {
        dest->windows[dest->window_count++] = win;
        dest->thumbnail_dirty = true;
    }

    // If the window moved to a non-active Space, hide it.
    // If it moved to the active Space, make sure it's visible.
    Client *c = wm_find_client(wm, win);
    if (c) {
        if (dest_idx != mc.current_space) {
            // Moving to a hidden Space — unmap the window.
            XUnmapWindow(wm->dpy, c->frame);
            c->mapped = false;
        } else {
            // Moving to the active Space — make sure it's mapped.
            if (!c->mapped) {
                XMapWindow(wm->dpy, c->frame);
                XMapWindow(wm->dpy, c->client);
                c->mapped = true;
            }
        }
    }

    fprintf(stderr, "[Mission Control] Moved window 0x%lx to %s\n",
            (unsigned long)win, dest->name);
}
