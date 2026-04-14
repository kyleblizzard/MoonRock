// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// ============================================================================
//  Crystal Plugin & Theme Engine — header
// ============================================================================
//
// This module gives AuraOS a way to be customized and extended by the user:
//
//   1. Theme engine  — Reads .theme files that describe window chrome colors,
//                      gradients, border widths, shadow properties, fonts, and
//                      other visual settings. A theme file is a simple INI-style
//                      text file that anyone can edit.
//
//   2. Plugin loader — Loads .so shared libraries at runtime using dlopen().
//                      Each plugin exports a CrystalPlugin struct that hooks
//                      into the compositing pipeline. Plugins can run custom
//                      GL code before/after compositing, or per-window.
//
//   3. Configuration — Parses a config file for keybindings, hot corners, and
//                      per-application window rules (e.g., "Steam should always
//                      use direct scanout when fullscreen").
//
//   4. Effects API   — A set of helper functions (blur, desaturate, tint, scale)
//                      that plugins can call to apply common visual effects
//                      without needing to write their own GLSL shaders.
//
// Why INI-style config files?
//   They are dead simple to read and write, both for humans and for code. No
//   need for a JSON/YAML parser dependency. Section headers in [brackets],
//   key=value pairs underneath. That's it.
//
// Why dlopen for plugins?
//   dlopen() is the POSIX standard for loading shared libraries at runtime. It
//   lets us load a .so file, look up a symbol by name (the CrystalPlugin struct),
//   and call functions from it — all without recompiling the compositor. Plugins
//   are optional: if none are installed, Crystal works exactly the same.
//
// ============================================================================

// SECURITY NOTICE:
// Plugins run in the compositor's address space with full privileges.
// Only load plugins from TRUSTED sources. The validate_path() function
// restricts loading to system and user plugin directories, and rejects
// world-writable files. Future versions should add:
//   - Out-of-process plugins via IPC (isolates faults and limits damage)
//   - seccomp-bpf restrictions after plugin init (limits syscall surface)
//   - Plugin signature verification (cryptographic proof of trusted origin)
// The in-process model is a pragmatic choice shared by every X11 compositor,
// but it means a malicious or buggy plugin can crash or compromise the
// entire compositor process.

#ifndef CRYSTAL_PLUGIN_H
#define CRYSTAL_PLUGIN_H

#include <stdbool.h>
#include <GL/gl.h>


// ============================================================================
//  Limits
// ============================================================================
//
// These caps prevent runaway memory usage if someone dumps a huge number of
// plugins or window rules into the config. 32 plugins and 64 window rules
// should be more than enough for any sane desktop setup.

#define MAX_PLUGINS       32
#define MAX_HOT_CORNERS    4
#define MAX_WINDOW_RULES  64


// ============================================================================
//  Hot corner types
// ============================================================================
//
// Hot corners let the user trigger an action by pushing the mouse cursor into
// a screen corner. macOS has this feature — you can set each corner to show
// Mission Control, the desktop, Launchpad, etc. We replicate that here.
//
// CornerAction is what happens when the cursor hits the corner.
// ScreenCorner identifies which of the four corners we're talking about.

// What action to perform when the user pushes the mouse into a corner
typedef enum {
    CORNER_ACTION_NONE = 0,           // Do nothing (corner is disabled)
    CORNER_ACTION_MISSION_CONTROL,    // Show all windows / workspace overview
    CORNER_ACTION_APP_EXPOSE,         // Show all windows for the current app
    CORNER_ACTION_DESKTOP,            // Hide all windows, show the desktop
    CORNER_ACTION_LAUNCHPAD,          // Open the application launcher
    CORNER_ACTION_SPOTLIGHT,          // Open the search bar
    CORNER_ACTION_CUSTOM,             // Run an arbitrary shell command
} CornerAction;

// Which physical corner of the screen
typedef enum {
    CORNER_TOP_LEFT = 0,
    CORNER_TOP_RIGHT,
    CORNER_BOTTOM_LEFT,
    CORNER_BOTTOM_RIGHT,
} ScreenCorner;

// Configuration for a single hot corner
typedef struct {
    ScreenCorner corner;              // Which corner this config applies to
    CornerAction action;              // What to do when the cursor hits it
    char custom_command[256];         // Shell command for CORNER_ACTION_CUSTOM
} HotCornerConfig;


// ============================================================================
//  Window rules
// ============================================================================
//
// Window rules let the user define per-application behavior. Each rule matches
// against a window's WM_CLASS property (which identifies the application —
// e.g., "Firefox", "steam", "konsole"). When a window matches, the rule
// overrides default compositor behavior for that window.
//
// This is how users can say things like:
//   "Make my terminal 95% opaque"
//   "Always put Steam on workspace 3"
//   "Disable shadows on my screen recorder"

typedef struct {
    char wm_class[128];              // Match against the window's WM_CLASS
    bool always_on_top;              // Pin this window above all others
    int  target_space;               // -1 = any workspace, 0+ = specific one
    float opacity;                   // Override opacity (0.0-1.0, 0 = default)
    bool no_shadow;                  // Disable the drop shadow for this window
    bool no_animation;               // Disable animations (minimize/restore)
    bool fullscreen_direct;          // Use direct scanout when fullscreen
} WindowRule;


// ============================================================================
//  Theme definition
// ============================================================================
//
// A ThemeDefinition holds every visual property that can be customized through
// a .theme file. These values feed directly into the compositor's rendering:
// decor.c uses them for title bar gradients and borders, crystal.c uses them
// for shadow parameters, and so on.
//
// All color values are RGB floats in the 0.0-1.0 range. Having separate "top"
// and "bottom" colors for the title bar lets us render a vertical gradient
// (lighter at the top, darker at the bottom), which is the classic Aqua look.

typedef struct {
    char name[64];                   // Human-readable theme name
    char author[64];                 // Who made this theme

    // Title bar gradient colors (top and bottom for vertical gradient)
    float titlebar_active_top[3];    // RGB — top of active window title bar
    float titlebar_active_bottom[3]; // RGB — bottom of active window title bar
    float titlebar_inactive_top[3];  // RGB — top of inactive window title bar
    float titlebar_inactive_bottom[3]; // RGB — bottom of inactive window title bar

    // Window border (the thin line around the window frame)
    float border_color_active[3];    // RGB — border when window is focused
    float border_color_inactive[3];  // RGB — border when window is not focused
    float border_width;              // Border thickness in pixels
    float corner_radius;             // Rounded corner radius in pixels

    // Drop shadow parameters
    float shadow_radius;             // Gaussian blur spread in pixels
    float shadow_alpha_active;       // Shadow opacity for focused windows
    float shadow_alpha_inactive;     // Shadow opacity for unfocused windows
    float shadow_y_offset;           // Downward offset (simulates overhead light)

    // UI accent colors
    float selection_color[3];        // RGB — text selection / highlight color
    float sidebar_color[3];          // RGB — sidebar / panel background

    // Typography
    char font_name[128];             // Font family name (e.g., "Lucida Grande")
    int  font_size;                  // Font size in points
} ThemeDefinition;


// ============================================================================
//  Plugin interface
// ============================================================================
//
// This is the struct that a .so plugin must export as a global symbol named
// "crystal_plugin". The compositor uses dlsym() to find it, then calls the
// lifecycle and hook functions at the appropriate times.
//
// A minimal plugin looks like:
//
//   static bool my_init(void) { return true; }
//   static void my_shutdown(void) { }
//   static void my_post(int w, int h) { /* draw overlay */ }
//
//   CrystalPlugin crystal_plugin = {
//       .name    = "My Plugin",
//       .version = "1.0",
//       .author  = "Someone",
//       .init    = my_init,
//       .shutdown = my_shutdown,
//       .post_composite = my_post,
//   };
//
// Any hook that is NULL is simply skipped — plugins only need to implement
// the hooks they care about.

typedef struct {
    const char *name;                // Display name of the plugin
    const char *version;             // Version string (e.g., "1.0.0")
    const char *author;              // Author name

    // Lifecycle callbacks
    bool (*init)(void);              // Called once when the plugin is loaded
    void (*shutdown)(void);          // Called once when the plugin is unloaded

    // Composite hooks — called by Crystal at specific points each frame
    void (*pre_composite)(int screen_w, int screen_h);   // Before any windows
    void (*post_composite)(int screen_w, int screen_h);  // After all windows
    void (*window_effect)(GLuint texture, int x, int y, int w, int h); // Per-window
} CrystalPlugin;


// ============================================================================
//  Plugin system lifecycle
// ============================================================================

// Initialize the plugin system.
//
// Sets up internal data structures (plugin array, hot corner array, window
// rules array, default theme). Call this once at startup before loading any
// themes, plugins, or config files.
//
// Returns true on success, false if something went wrong.
bool plugin_init(void);

// Shut down the plugin system and unload everything.
//
// Calls shutdown() on every loaded plugin, then dlclose()s their handles.
// Frees all internal state. Call this during compositor teardown.
void plugin_shutdown(void);


// ============================================================================
//  Theme loading
// ============================================================================

// Load a theme from a .theme file.
//
// The file uses a simple INI format with sections and key=value pairs:
//
//   [Theme]
//   name=Snow Leopard
//   author=Kyle Blizzard
//
//   [TitleBar]
//   active_top=0.83,0.83,0.83
//   active_bottom=0.66,0.66,0.66
//
//   [Shadow]
//   radius=22
//   alpha_active=0.45
//
// Color values are comma-separated RGB floats. Numeric values are parsed
// with atof()/atoi(). Unknown keys are silently ignored.
//
// path: full filesystem path to the .theme file
//
// Returns true if the file was loaded and parsed successfully.
bool plugin_load_theme(const char *path);

// Get a pointer to the currently active theme.
//
// If no theme has been loaded, this returns a pointer to the default theme
// (which uses Snow Leopard-inspired values). The returned pointer is valid
// until plugin_shutdown() is called.
ThemeDefinition *plugin_get_theme(void);


// ============================================================================
//  Plugin loading and unloading
// ============================================================================

// Load a plugin from a .so shared library.
//
// The library must export a global CrystalPlugin struct named "crystal_plugin".
// After loading, the plugin's init() function is called. If init() returns
// false, the plugin is unloaded and this function returns false.
//
// path: full filesystem path to the .so file
//
// Returns true if the plugin was loaded and initialized successfully.
bool plugin_load(const char *path);

// Unload a plugin by name.
//
// Calls the plugin's shutdown() function, then dlclose()s its handle and
// removes it from the active plugin list. If no plugin with the given name
// is loaded, this is a no-op.
//
// name: the plugin's name (as reported by CrystalPlugin.name)
void plugin_unload(const char *name);


// ============================================================================
//  Hot corners
// ============================================================================

// Configure a hot corner.
//
// corner:     which screen corner to configure
// action:     what to do when the cursor reaches that corner
// custom_cmd: shell command to run (only used when action is CORNER_ACTION_CUSTOM;
//             pass NULL for other actions)
void plugin_set_hot_corner(ScreenCorner corner, CornerAction action,
                           const char *custom_cmd);

// Check whether the mouse cursor is currently in a hot corner.
//
// Compares the cursor position against a small threshold near each screen edge.
// If the cursor is within that threshold of a configured corner, returns the
// action for that corner. Otherwise returns CORNER_ACTION_NONE.
//
// mouse_x, mouse_y: current cursor position in screen coordinates
// screen_w, screen_h: screen dimensions in pixels
CornerAction plugin_check_hot_corner(int mouse_x, int mouse_y,
                                     int screen_w, int screen_h);


// ============================================================================
//  Window rules
// ============================================================================

// Add a window rule.
//
// The rule is copied into the internal array. If the array is full
// (MAX_WINDOW_RULES reached), the rule is silently dropped and a warning
// is printed to stderr.
void plugin_add_window_rule(const WindowRule *rule);

// Find a window rule that matches the given WM_CLASS.
//
// Searches the rule list for a case-sensitive match against the wm_class field.
// Returns a pointer to the matching rule, or NULL if no rule matches.
//
// The returned pointer is valid until the next call to plugin_shutdown().
WindowRule *plugin_find_window_rule(const char *wm_class);


// ============================================================================
//  Configuration file
// ============================================================================

// Load a configuration file.
//
// The config file uses the same INI format as theme files, but with different
// sections:
//
//   [HotCorners]
//   top_left=mission_control
//   bottom_right=spotlight
//
//   [WindowRules]
//   steam=fullscreen_direct,no_shadow
//   konsole=opacity=0.95
//
// path: full filesystem path to the config file
//
// Returns true if the file was loaded and parsed successfully.
bool plugin_load_config(const char *path);

// Save the current configuration to a file.
//
// Writes the current hot corners and window rules back to an INI-format file.
// This lets the compositor persist user changes (e.g., from a settings GUI).
//
// path: full filesystem path to write to
//
// Returns true if the file was written successfully.
bool plugin_save_config(const char *path);


// ============================================================================
//  Effects API
// ============================================================================
//
// These functions provide common visual effects that plugins can call without
// writing their own shaders. They operate on the currently bound GL texture.
//
// Plugins call these from their hook functions (pre_composite, post_composite,
// or window_effect) to apply effects to the scene or individual windows.

// Apply a Gaussian blur to a texture region.
//
// texture: GL texture handle to blur
// x, y:    top-left corner of the region (screen coordinates)
// w, h:    width and height of the region
// radius:  blur radius in pixels (higher = more blurry)
void plugin_effect_blur(GLuint texture, int x, int y, int w, int h,
                        float radius);

// Desaturate (remove color from) a texture.
//
// texture: GL texture handle to desaturate
// amount:  0.0 = full color (no change), 1.0 = fully grayscale
void plugin_effect_desaturate(GLuint texture, float amount);

// Apply a color tint to a texture.
//
// texture:  GL texture handle to tint
// r, g, b:  tint color (0.0-1.0 per channel)
// amount:   0.0 = no tint, 1.0 = full tint color replaces original
void plugin_effect_tint(GLuint texture, float r, float g, float b,
                        float amount);

// Scale a texture (zoom in or out).
//
// texture: GL texture handle to scale
// scale:   1.0 = no change, >1.0 = zoom in, <1.0 = zoom out
void plugin_effect_scale(GLuint texture, float scale);


// ============================================================================
//  Plugin hooks — called by Crystal's composite loop
// ============================================================================
//
// These functions iterate over all loaded plugins and call the corresponding
// hook function on each one. Crystal calls these at the appropriate point in
// its per-frame rendering:
//
//   crystal_composite() {
//       plugin_run_pre_composite(w, h);    // Before drawing any windows
//       for each window:
//           plugin_run_window_effect(tex, x, y, w, h);  // Per-window effects
//       plugin_run_post_composite(w, h);   // After drawing all windows
//   }

// Run every loaded plugin's pre_composite hook.
void plugin_run_pre_composite(int screen_w, int screen_h);

// Run every loaded plugin's post_composite hook.
void plugin_run_post_composite(int screen_w, int screen_h);

// Run every loaded plugin's window_effect hook on a single window.
void plugin_run_window_effect(GLuint texture, int x, int y, int w, int h);

#endif // CRYSTAL_PLUGIN_H
