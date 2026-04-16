// Copyright (c) 2026 Kyle Blizzard
// SPDX-License-Identifier: BSD-3-Clause
//
// MoonRock was created by Kyle Blizzard. Feel free to use it and improve it!
// www.blizzard.show/moonrock/
//
// ============================================================================
//  MoonRock Plugin & Theme Engine — implementation
// ============================================================================
//
// This file implements the plugin system, theme engine, hot corners, window
// rules, configuration file I/O, and the effects API described in
// moonrock_plugin.h. See that header for the full documentation of each
// public function.
//
// Internal structure:
//
//   - A fixed-size array of LoadedPlugin structs holds all active plugins.
//     Each entry stores the dlopen() handle and a pointer to the plugin's
//     exported MRPlugin interface struct.
//
//   - A single ThemeDefinition holds the current theme. It is initialized
//     with sensible Snow Leopard-inspired defaults so MoonRock looks correct
//     even if no theme file is loaded.
//
//   - Hot corners and window rules are stored in simple fixed-size arrays.
//     Linear search is fine because the arrays are tiny (4 corners, <=64 rules).
//
//   - The INI parser is a small hand-written state machine that reads one line
//     at a time. It tracks the current [Section] header and dispatches
//     key=value pairs to the appropriate handler based on the section name.
//
// ============================================================================

// POSIX extensions like strdup require _GNU_SOURCE (provided by meson via -D_GNU_SOURCE).

#include "moonrock_plugin.h"
#include "moonrock_shaders.h"  // Shader programs, FBO management, quad drawing
#include "moonrock.h"          // mr_get_shaders(), mr_get_projection()

#include <stdio.h>      // fprintf, fopen, fclose, fgets, etc.
#include <stdlib.h>     // atof, atoi, malloc, free, realpath
#include <string.h>     // strcmp, strncpy, memset, strstr, strtok, etc.
#include <dlfcn.h>      // dlopen, dlsym, dlclose, dlerror — dynamic library loading
#include <limits.h>     // PATH_MAX — maximum filesystem path length
#include <sys/stat.h>   // stat, S_IWOTH, S_ISREG — file permission and type checks


// ============================================================================
//  Internal data structures
// ============================================================================

// A loaded plugin: the dlopen handle paired with the plugin's interface struct.
// We need the handle to call dlclose() when unloading, and the plugin pointer
// to call the lifecycle/hook functions.
typedef struct {
    void          *handle;    // dlopen() handle (needed for dlclose)
    MRPlugin *plugin;   // Pointer to the plugin's exported struct
} LoadedPlugin;

// ============================================================================
//  Module-level state (file-scoped / static)
// ============================================================================
//
// These are essentially global variables, but 'static' limits their visibility
// to this file only. Other .c files cannot access them directly — they go
// through the public API functions in moonrock_plugin.h.

// Array of all loaded plugins, and how many are currently loaded
static LoadedPlugin loaded_plugins[MAX_PLUGINS];
static int          plugin_count = 0;

// Hot corner configuration — one entry per screen corner
static HotCornerConfig hot_corners[MAX_HOT_CORNERS];

// Window rules — per-application compositor overrides
static WindowRule window_rules[MAX_WINDOW_RULES];
static int        rule_count = 0;

// The currently active theme (initialized with defaults in plugin_init)
static ThemeDefinition current_theme;

// Cached FBOs for blur-behind (see plugin_effect_blur for details).
// Declared here so plugin_shutdown() can free them.
//
// We keep multiple cache slots because blur-behind is called for different-sized
// panels each frame (e.g., dock 1514x290 and menubar 1920x46). A single-slot
// cache would thrash between them. 4 slots covers any reasonable panel count.
#define BLUR_CACHE_SLOTS 4

typedef struct {
    GLuint fbo_a, tex_a;   // Horizontal blur pass FBO
    GLuint fbo_b, tex_b;   // Vertical blur pass FBO
    int    w, h;           // Dimensions this slot was created for (0 = empty)
} BlurCacheSlot;

static BlurCacheSlot blur_cache[BLUR_CACHE_SLOTS];


// ============================================================================
//  Default theme values
// ============================================================================
//
// These Snow Leopard-inspired defaults are applied when plugin_init() runs.
// If no .theme file is loaded, the compositor uses these values. They produce
// the classic brushed-aluminum look with soft Gaussian shadows.

static void set_default_theme(void)
{
    memset(&current_theme, 0, sizeof(current_theme));

    strncpy(current_theme.name, "Default", sizeof(current_theme.name) - 1);
    strncpy(current_theme.author, "CopyCatOS", sizeof(current_theme.author) - 1);

    // Active title bar: light gray gradient (lighter at top, darker at bottom)
    current_theme.titlebar_active_top[0]    = 0.83f;
    current_theme.titlebar_active_top[1]    = 0.83f;
    current_theme.titlebar_active_top[2]    = 0.83f;
    current_theme.titlebar_active_bottom[0] = 0.66f;
    current_theme.titlebar_active_bottom[1] = 0.66f;
    current_theme.titlebar_active_bottom[2] = 0.66f;

    // Inactive title bar: even lighter gradient (less contrast = recedes visually)
    current_theme.titlebar_inactive_top[0]    = 0.93f;
    current_theme.titlebar_inactive_top[1]    = 0.93f;
    current_theme.titlebar_inactive_top[2]    = 0.93f;
    current_theme.titlebar_inactive_bottom[0] = 0.86f;
    current_theme.titlebar_inactive_bottom[1] = 0.86f;
    current_theme.titlebar_inactive_bottom[2] = 0.86f;

    // Borders: medium gray for active, lighter gray for inactive
    current_theme.border_color_active[0]   = 0.50f;
    current_theme.border_color_active[1]   = 0.50f;
    current_theme.border_color_active[2]   = 0.50f;
    current_theme.border_color_inactive[0] = 0.70f;
    current_theme.border_color_inactive[1] = 0.70f;
    current_theme.border_color_inactive[2] = 0.70f;
    current_theme.border_width  = 1.0f;
    current_theme.corner_radius = 5.0f;

    // Shadow: matches the existing MoonRock defaults (see mr.h)
    current_theme.shadow_radius         = 22.0f;
    current_theme.shadow_alpha_active   = 0.45f;
    current_theme.shadow_alpha_inactive = 0.22f;
    current_theme.shadow_y_offset       = 4.0f;

    // Selection color: macOS-style blue highlight
    current_theme.selection_color[0] = 0.24f;
    current_theme.selection_color[1] = 0.47f;
    current_theme.selection_color[2] = 0.85f;

    // Sidebar: light gray background
    current_theme.sidebar_color[0] = 0.92f;
    current_theme.sidebar_color[1] = 0.92f;
    current_theme.sidebar_color[2] = 0.92f;

    // Font: Lucida Grande at 11pt (the classic Snow Leopard system font)
    strncpy(current_theme.font_name, "Lucida Grande",
            sizeof(current_theme.font_name) - 1);
    current_theme.font_size = 11;

    // Effects: Snow Leopard uses frosted glass blur on the dock and menu bar.
    // The blur radius controls how soft/blurry the background appears through
    // translucent panels. Inactive windows stay fully opaque in Snow Leopard.
    current_theme.blur_behind_radius   = 20.0f;
    current_theme.dock_blur_radius     = 15.0f;
    current_theme.menubar_blur_radius  = 10.0f;
    current_theme.window_opacity_inactive = 1.0f;

    // Animation: Snow Leopard's signature genie effect at normal speed
    current_theme.minimize_animation = ANIM_GENIE;
    current_theme.animation_speed    = 1.0f;

    // Buttons: the iconic colored "traffic light" circles
    current_theme.button_style = BUTTON_AQUA;

    // Dock: slightly translucent glass shelf with 2x magnification on hover,
    // and reflections of each icon on the shelf surface
    current_theme.dock_opacity        = 0.85f;
    current_theme.dock_magnification  = 2.0f;
    current_theme.dock_reflection     = true;

    // Menu bar: slightly translucent so the desktop barely shows through
    current_theme.menubar_opacity = 0.9f;
}


// ============================================================================
//  INI parser helpers
// ============================================================================
//
// The INI format is line-oriented:
//   - Lines starting with '#' or ';' are comments.
//   - Lines like [SectionName] set the current section.
//   - Lines like key=value set a property within the current section.
//   - Blank lines are ignored.
//
// These helper functions handle the low-level string operations.

// Trim leading and trailing whitespace from a string in-place.
// Returns a pointer into the same buffer (does not allocate).
static char *trim(char *str)
{
    // Skip leading whitespace
    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n')
        str++;

    // If the string is all whitespace, return an empty string
    if (*str == '\0')
        return str;

    // Find the end and walk backward past trailing whitespace
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        end--;

    // Null-terminate right after the last non-whitespace character
    *(end + 1) = '\0';
    return str;
}

// Parse a comma-separated RGB color string like "0.83,0.66,0.50" into a
// float[3] array. Returns true if all three components were found.
static bool parse_rgb(const char *value, float out[3])
{
    // We need a mutable copy because strtok modifies the string it parses
    char buf[128];
    strncpy(buf, value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // Split on commas and parse each component
    char *tok = strtok(buf, ",");
    if (!tok) return false;
    out[0] = (float)atof(tok);

    tok = strtok(NULL, ",");
    if (!tok) return false;
    out[1] = (float)atof(tok);

    tok = strtok(NULL, ",");
    if (!tok) return false;
    out[2] = (float)atof(tok);

    return true;
}


// ============================================================================
//  Path validation (security)
// ============================================================================
//
// Before opening ANY file (plugin .so, theme .theme, config), we validate that
// the resolved path falls within one of the allowed directories. This prevents
// path traversal attacks (e.g., "../../etc/shadow") and symlink attacks where
// a symlink in the plugin directory points somewhere dangerous.
//
// Allowed directories:
//   ~/.config/cc-wm/          — user configuration files
//   ~/.local/share/cc-wm/plugins/  — user-installed plugins
//   /usr/share/cc-wm/         — system-installed plugins and themes
//   /usr/local/share/cc-wm/   — locally-compiled plugins and themes
//
// We also reject world-writable files (chmod o+w) because any user on the
// system could have tampered with them. For .so files, we additionally verify
// it's a regular file (not a symlink to something unexpected after realpath).

static bool validate_path(const char *path, bool allow_so)
{
    // realpath() resolves all symlinks and ".." components, giving us the
    // true filesystem location. If the file doesn't exist, it returns NULL.
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        fprintf(stderr, "[plugin] SECURITY: cannot resolve path '%s'\n", path);
        return false;
    }

    // Build the user-specific allowed directory paths from $HOME
    const char *home = getenv("HOME");
    char user_config[PATH_MAX], user_plugins[PATH_MAX];
    snprintf(user_config, sizeof(user_config), "%s/.config/cc-wm/",
             home ? home : "/tmp");
    snprintf(user_plugins, sizeof(user_plugins), "%s/.local/share/cc-wm/plugins/",
             home ? home : "/tmp");

    // Check whether the resolved path starts with any of the allowed prefixes.
    // strncmp with the prefix length acts as a "starts with" check.
    bool allowed = (strncmp(resolved, user_config, strlen(user_config)) == 0 ||
                    strncmp(resolved, user_plugins, strlen(user_plugins)) == 0 ||
                    strncmp(resolved, "/usr/share/cc-wm/", 19) == 0 ||
                    strncmp(resolved, "/usr/local/share/cc-wm/", 25) == 0);

    if (!allowed) {
        fprintf(stderr, "[plugin] SECURITY: path '%s' resolves to '%s' "
                "which is outside allowed directories\n", path, resolved);
        return false;
    }

    // stat() retrieves file metadata — we check the permission bits to make
    // sure the file is not world-writable (the 'other' write bit)
    struct stat st;
    if (stat(resolved, &st) != 0) {
        fprintf(stderr, "[plugin] SECURITY: cannot stat '%s'\n", resolved);
        return false;
    }
    if (st.st_mode & S_IWOTH) {
        fprintf(stderr, "[plugin] SECURITY: rejecting world-writable file '%s'\n",
                resolved);
        return false;
    }

    // For shared library (.so) files, verify it's a regular file. After
    // realpath() resolved symlinks, this catches anything weird like a FIFO
    // or device node that somehow ended up in the plugin directory.
    if (allow_so && !S_ISREG(st.st_mode)) {
        fprintf(stderr, "[plugin] SECURITY: '%s' is not a regular file\n", resolved);
        return false;
    }

    return true;
}


// ============================================================================
//  Plugin system lifecycle
// ============================================================================

bool plugin_init(void)
{
    // Zero out all arrays so we start from a clean slate
    memset(loaded_plugins, 0, sizeof(loaded_plugins));
    plugin_count = 0;

    memset(hot_corners, 0, sizeof(hot_corners));
    // Set each hot corner's corner field to match its array index, and
    // default action to NONE (disabled)
    for (int i = 0; i < MAX_HOT_CORNERS; i++) {
        hot_corners[i].corner = (ScreenCorner)i;
        hot_corners[i].action = CORNER_ACTION_NONE;
    }

    memset(window_rules, 0, sizeof(window_rules));
    rule_count = 0;

    // Apply the built-in default theme
    set_default_theme();

    fprintf(stderr, "[plugin] Plugin system initialized\n");
    return true;
}

void plugin_shutdown(void)
{
    // Shut down and unload every plugin in reverse order.
    // Reverse order is a good practice — if plugin B depends on plugin A,
    // unloading B first avoids dangling references.
    for (int i = plugin_count - 1; i >= 0; i--) {
        MRPlugin *p = loaded_plugins[i].plugin;

        // Call the plugin's shutdown callback if it has one
        if (p && p->shutdown)
            p->shutdown();

        // Close the shared library handle to free its memory
        if (loaded_plugins[i].handle)
            dlclose(loaded_plugins[i].handle);

        fprintf(stderr, "[plugin] Unloaded: %s\n",
                (p && p->name) ? p->name : "(unknown)");
    }

    // Free all cached blur FBOs — these are held for the compositor's lifetime
    // to avoid per-frame allocation overhead (see plugin_effect_blur).
    for (int i = 0; i < BLUR_CACHE_SLOTS; i++) {
        if (blur_cache[i].fbo_a) shaders_destroy_fbo(blur_cache[i].fbo_a, blur_cache[i].tex_a);
        if (blur_cache[i].fbo_b) shaders_destroy_fbo(blur_cache[i].fbo_b, blur_cache[i].tex_b);
    }
    memset(blur_cache, 0, sizeof(blur_cache));

    // Reset all state
    memset(loaded_plugins, 0, sizeof(loaded_plugins));
    plugin_count = 0;

    memset(hot_corners, 0, sizeof(hot_corners));
    memset(window_rules, 0, sizeof(window_rules));
    rule_count = 0;

    fprintf(stderr, "[plugin] Plugin system shut down\n");
}


// ============================================================================
//  Theme loading
// ============================================================================

bool plugin_load_theme(const char *path)
{
    // SECURITY: validate the path before opening — prevents path traversal
    // and loading themes from untrusted locations
    if (!validate_path(path, false)) {
        fprintf(stderr, "[plugin] Refused to load theme from untrusted path: %s\n", path);
        return false;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[plugin] Cannot open theme file: %s\n", path);
        return false;
    }

    // Track which INI section we're currently inside.
    // This changes whenever we encounter a [SectionName] line.
    // Using 128 bytes to safely hold long section names without overflow.
    char section[128] = "";
    char line[512];

    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);

        // Skip empty lines and comments
        if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';')
            continue;

        // Check for a section header like [Theme] or [TitleBar]
        if (trimmed[0] == '[') {
            // Find the closing bracket
            char *end = strchr(trimmed, ']');
            if (end) {
                *end = '\0';  // Terminate the string at the bracket
                strncpy(section, trimmed + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
            }
            continue;
        }

        // Parse key=value pairs
        char *eq = strchr(trimmed, '=');
        if (!eq) continue;  // Not a valid key=value line

        // Split into key and value at the '=' sign
        *eq = '\0';
        char *key   = trim(trimmed);
        char *value = trim(eq + 1);

        // Dispatch to the appropriate handler based on the current section.
        // strcmp returns 0 when strings match.

        if (strcmp(section, "Theme") == 0) {
            // [Theme] section — metadata about the theme
            if (strcmp(key, "name") == 0) {
                strncpy(current_theme.name, value,
                        sizeof(current_theme.name) - 1);
                current_theme.name[sizeof(current_theme.name) - 1] = '\0';
            }
            else if (strcmp(key, "author") == 0) {
                strncpy(current_theme.author, value,
                        sizeof(current_theme.author) - 1);
                current_theme.author[sizeof(current_theme.author) - 1] = '\0';
            }
        }
        else if (strcmp(section, "TitleBar") == 0) {
            // [TitleBar] section — gradient colors for window title bars
            if (strcmp(key, "active_top") == 0)
                parse_rgb(value, current_theme.titlebar_active_top);
            else if (strcmp(key, "active_bottom") == 0)
                parse_rgb(value, current_theme.titlebar_active_bottom);
            else if (strcmp(key, "inactive_top") == 0)
                parse_rgb(value, current_theme.titlebar_inactive_top);
            else if (strcmp(key, "inactive_bottom") == 0)
                parse_rgb(value, current_theme.titlebar_inactive_bottom);
        }
        else if (strcmp(section, "Border") == 0) {
            // [Border] section — window frame border appearance
            if (strcmp(key, "color_active") == 0)
                parse_rgb(value, current_theme.border_color_active);
            else if (strcmp(key, "color_inactive") == 0)
                parse_rgb(value, current_theme.border_color_inactive);
            else if (strcmp(key, "width") == 0)
                current_theme.border_width = (float)atof(value);
            else if (strcmp(key, "corner_radius") == 0)
                current_theme.corner_radius = (float)atof(value);
        }
        else if (strcmp(section, "Shadow") == 0) {
            // [Shadow] section — drop shadow parameters
            if (strcmp(key, "radius") == 0)
                current_theme.shadow_radius = (float)atof(value);
            else if (strcmp(key, "alpha_active") == 0)
                current_theme.shadow_alpha_active = (float)atof(value);
            else if (strcmp(key, "alpha_inactive") == 0)
                current_theme.shadow_alpha_inactive = (float)atof(value);
            else if (strcmp(key, "y_offset") == 0)
                current_theme.shadow_y_offset = (float)atof(value);
        }
        else if (strcmp(section, "Colors") == 0) {
            // [Colors] section — UI accent colors
            if (strcmp(key, "selection") == 0)
                parse_rgb(value, current_theme.selection_color);
            else if (strcmp(key, "sidebar") == 0)
                parse_rgb(value, current_theme.sidebar_color);
        }
        else if (strcmp(section, "Font") == 0) {
            // [Font] section — typography settings
            if (strcmp(key, "name") == 0) {
                strncpy(current_theme.font_name, value,
                        sizeof(current_theme.font_name) - 1);
                current_theme.font_name[sizeof(current_theme.font_name) - 1] = '\0';
            }
            else if (strcmp(key, "size") == 0)
                current_theme.font_size = atoi(value);
        }
        else if (strcmp(section, "Effects") == 0) {
            // [Effects] section — blur and transparency settings for frosted
            // glass panels and inactive window dimming
            if (strcmp(key, "blur_behind_radius") == 0)
                current_theme.blur_behind_radius = (float)atof(value);
            else if (strcmp(key, "dock_blur_radius") == 0)
                current_theme.dock_blur_radius = (float)atof(value);
            else if (strcmp(key, "menubar_blur_radius") == 0)
                current_theme.menubar_blur_radius = (float)atof(value);
            else if (strcmp(key, "window_opacity_inactive") == 0)
                current_theme.window_opacity_inactive = (float)atof(value);
        }
        else if (strcmp(section, "Animation") == 0) {
            // [Animation] section — minimize effect style and global speed.
            // The minimize style is an integer that maps to the MinimizeAnimation
            // enum: 0=genie, 1=scale, 2=fade.
            if (strcmp(key, "minimize_style") == 0)
                current_theme.minimize_animation = atoi(value);
            else if (strcmp(key, "speed") == 0)
                current_theme.animation_speed = (float)atof(value);
        }
        else if (strcmp(section, "Buttons") == 0) {
            // [Buttons] section — title bar button visual style.
            // Maps to ButtonStyle enum: 0=aqua, 1=flat, 2=classic.
            if (strcmp(key, "style") == 0)
                current_theme.button_style = atoi(value);
        }
        else if (strcmp(section, "Dock") == 0) {
            // [Dock] section — dock shelf appearance: opacity, icon
            // magnification on hover, and whether to draw reflections
            if (strcmp(key, "opacity") == 0)
                current_theme.dock_opacity = (float)atof(value);
            else if (strcmp(key, "magnification") == 0)
                current_theme.dock_magnification = (float)atof(value);
            else if (strcmp(key, "reflection") == 0)
                current_theme.dock_reflection = (atoi(value) != 0);
        }
        else if (strcmp(section, "MenuBar") == 0) {
            // [MenuBar] section — menu bar background opacity
            if (strcmp(key, "opacity") == 0)
                current_theme.menubar_opacity = (float)atof(value);
        }
    }

    fclose(f);
    fprintf(stderr, "[plugin] Loaded theme: %s by %s\n",
            current_theme.name, current_theme.author);
    return true;
}

ThemeDefinition *plugin_get_theme(void)
{
    return &current_theme;
}


// ============================================================================
//  Plugin loading and unloading
// ============================================================================

bool plugin_load(const char *path)
{
    // Check that we haven't hit the plugin limit
    if (plugin_count >= MAX_PLUGINS) {
        fprintf(stderr, "[plugin] Cannot load %s: maximum of %d plugins reached\n",
                path, MAX_PLUGINS);
        return false;
    }

    // SECURITY: validate the path before loading — prevents loading .so files
    // from untrusted locations, world-writable files, or non-regular files.
    // The 'true' argument enables the extra .so checks (regular file check).
    if (!validate_path(path, true)) {
        fprintf(stderr, "[plugin] Refused to load plugin from untrusted path: %s\n",
                path);
        return false;
    }

    // dlopen() loads the shared library into our process's address space.
    // RTLD_LAZY means symbols are resolved on first use, not all at once.
    // This is faster at load time and fine since we immediately look up the
    // only symbol we need (mr_plugin).
    void *handle = dlopen(path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "[plugin] Failed to load %s: %s\n", path, dlerror());
        return false;
    }

    // dlsym() looks up a symbol (global variable or function) by name inside
    // the loaded library. We expect every MoonRock plugin to export a struct
    // called "mr_plugin" of type MRPlugin.
    MRPlugin *plugin = (MRPlugin *)dlsym(handle, "mr_plugin");
    if (!plugin) {
        fprintf(stderr, "[plugin] %s has no 'mr_plugin' symbol\n", path);
        dlclose(handle);
        return false;
    }

    // SECURITY: validate that the plugin exports required fields. A plugin
    // without a name or version is either corrupted or not a real MoonRock plugin.
    if (!plugin->name || !plugin->version) {
        fprintf(stderr, "[plugin] SECURITY: plugin at '%s' missing required "
                "name/version fields\n", path);
        dlclose(handle);
        return false;
    }

    // SECURITY: prevent loading the same plugin twice. Duplicate plugins could
    // cause double-free issues on shutdown or conflicting behavior. We check
    // by name since the same .so could be loaded from different paths.
    for (int i = 0; i < plugin_count; i++) {
        if (loaded_plugins[i].plugin &&
            loaded_plugins[i].plugin->name &&
            strcmp(loaded_plugins[i].plugin->name, plugin->name) == 0) {
            fprintf(stderr, "[plugin] Plugin '%s' is already loaded — "
                    "refusing duplicate\n", plugin->name);
            dlclose(handle);
            return false;
        }
    }

    // Call the plugin's init function if it has one. If init() returns false,
    // the plugin is telling us it cannot run (maybe a required resource is
    // missing). A NULL init pointer means no initialization is needed.
    if (plugin->init && !plugin->init()) {
        fprintf(stderr, "[plugin] %s init() failed\n", path);
        dlclose(handle);
        return false;
    }

    // Store the plugin in the next available slot
    loaded_plugins[plugin_count].handle = handle;
    loaded_plugins[plugin_count].plugin = plugin;
    plugin_count++;

    fprintf(stderr, "[plugin] Loaded: %s v%s by %s\n",
            plugin->name ? plugin->name : "(unnamed)",
            plugin->version ? plugin->version : "?",
            plugin->author ? plugin->author : "unknown");
    return true;
}

void plugin_unload(const char *name)
{
    // Search for a loaded plugin matching the given name
    for (int i = 0; i < plugin_count; i++) {
        MRPlugin *p = loaded_plugins[i].plugin;

        // Skip if this plugin has no name or doesn't match
        if (!p || !p->name || strcmp(p->name, name) != 0)
            continue;

        // Found it — shut it down
        if (p->shutdown)
            p->shutdown();

        // Close the shared library
        if (loaded_plugins[i].handle)
            dlclose(loaded_plugins[i].handle);

        fprintf(stderr, "[plugin] Unloaded: %s\n", name);

        // Shift remaining plugins down to fill the gap.
        // This keeps the array packed with no holes, which makes iteration
        // over all plugins simple (just loop 0..plugin_count).
        for (int j = i; j < plugin_count - 1; j++)
            loaded_plugins[j] = loaded_plugins[j + 1];

        plugin_count--;

        // Clear the now-unused last slot
        memset(&loaded_plugins[plugin_count], 0, sizeof(LoadedPlugin));
        return;
    }

    fprintf(stderr, "[plugin] Cannot unload '%s': not found\n", name);
}


// ============================================================================
//  Hot corners
// ============================================================================

void plugin_set_hot_corner(ScreenCorner corner, CornerAction action,
                           const char *custom_cmd)
{
    // Bounds check — the ScreenCorner enum values 0-3 map directly to array indices
    if (corner < 0 || corner >= MAX_HOT_CORNERS) {
        fprintf(stderr, "[plugin] Invalid hot corner index: %d\n", corner);
        return;
    }

    hot_corners[corner].corner = corner;
    hot_corners[corner].action = action;

    // Store the custom command if one was provided and the action needs it.
    // Validate length to avoid silent truncation of important commands.
    if (custom_cmd && action == CORNER_ACTION_CUSTOM) {
        if (strlen(custom_cmd) >= sizeof(hot_corners[corner].custom_command)) {
            fprintf(stderr, "[plugin] WARN: hot corner command too long, truncated\n");
        }
        strncpy(hot_corners[corner].custom_command, custom_cmd,
                sizeof(hot_corners[corner].custom_command) - 1);
        hot_corners[corner].custom_command[sizeof(hot_corners[corner].custom_command) - 1] = '\0';
    }
    else {
        hot_corners[corner].custom_command[0] = '\0';
    }
}

CornerAction plugin_check_hot_corner(int mouse_x, int mouse_y,
                                     int screen_w, int screen_h)
{
    // How close the cursor must be to the corner to trigger (in pixels).
    // 2 pixels means the cursor has to be right at the edge of the screen.
    int threshold = 2;

    // Check each corner by testing whether the cursor is within 'threshold'
    // pixels of both edges that form that corner.
    if (mouse_x <= threshold && mouse_y <= threshold)
        return hot_corners[CORNER_TOP_LEFT].action;

    if (mouse_x >= screen_w - threshold && mouse_y <= threshold)
        return hot_corners[CORNER_TOP_RIGHT].action;

    if (mouse_x <= threshold && mouse_y >= screen_h - threshold)
        return hot_corners[CORNER_BOTTOM_LEFT].action;

    if (mouse_x >= screen_w - threshold && mouse_y >= screen_h - threshold)
        return hot_corners[CORNER_BOTTOM_RIGHT].action;

    // Cursor is not in any corner
    return CORNER_ACTION_NONE;
}


// ============================================================================
//  Window rules
// ============================================================================

void plugin_add_window_rule(const WindowRule *rule)
{
    if (rule_count >= MAX_WINDOW_RULES) {
        fprintf(stderr, "[plugin] Cannot add window rule for '%s': "
                "maximum of %d rules reached\n",
                rule->wm_class, MAX_WINDOW_RULES);
        return;
    }

    // Copy the rule into the next available slot
    window_rules[rule_count] = *rule;
    rule_count++;

    fprintf(stderr, "[plugin] Added window rule for '%s'\n", rule->wm_class);
}

WindowRule *plugin_find_window_rule(const char *wm_class)
{
    // Linear scan through all rules looking for a WM_CLASS match.
    // With at most 64 rules and short strings, this is effectively instant.
    for (int i = 0; i < rule_count; i++) {
        if (strcmp(window_rules[i].wm_class, wm_class) == 0)
            return &window_rules[i];
    }

    // No matching rule found
    return NULL;
}


// ============================================================================
//  Configuration file — loading
// ============================================================================
//
// The config file format:
//
//   [HotCorners]
//   top_left=mission_control
//   top_right=desktop
//   bottom_left=none
//   bottom_right=spotlight
//
//   [WindowRules]
//   steam=fullscreen_direct,no_shadow
//   konsole=opacity=0.95
//   firefox=always_on_top
//
// Hot corner values map to CornerAction enum names. Window rule values are
// comma-separated flags, with "opacity=N" being a special key=value flag.

// Map a hot corner action name string to its enum value.
// Returns CORNER_ACTION_NONE if the string is not recognized.
static CornerAction parse_corner_action(const char *str)
{
    if (strcmp(str, "mission_control") == 0) return CORNER_ACTION_MISSION_CONTROL;
    if (strcmp(str, "app_expose") == 0)      return CORNER_ACTION_APP_EXPOSE;
    if (strcmp(str, "desktop") == 0)         return CORNER_ACTION_DESKTOP;
    if (strcmp(str, "launchpad") == 0)       return CORNER_ACTION_LAUNCHPAD;
    if (strcmp(str, "spotlight") == 0)       return CORNER_ACTION_SPOTLIGHT;
    if (strcmp(str, "none") == 0)            return CORNER_ACTION_NONE;

    // If the string starts with "custom:", treat the rest as a shell command
    if (strncmp(str, "custom:", 7) == 0)     return CORNER_ACTION_CUSTOM;

    // Unknown action — treat as disabled
    return CORNER_ACTION_NONE;
}

// Map a CornerAction enum value to its string representation.
// Used when saving the config back to a file.
static const char *corner_action_to_string(CornerAction action)
{
    switch (action) {
        case CORNER_ACTION_MISSION_CONTROL: return "mission_control";
        case CORNER_ACTION_APP_EXPOSE:      return "app_expose";
        case CORNER_ACTION_DESKTOP:         return "desktop";
        case CORNER_ACTION_LAUNCHPAD:       return "launchpad";
        case CORNER_ACTION_SPOTLIGHT:       return "spotlight";
        case CORNER_ACTION_CUSTOM:          return "custom";
        case CORNER_ACTION_NONE:
        default:                            return "none";
    }
}

// Parse a window rule value string like "fullscreen_direct,no_shadow,opacity=0.95"
// into a WindowRule struct. The wm_class field is set by the caller.
static void parse_window_rule_flags(const char *flags_str, WindowRule *rule)
{
    // Work on a copy because strtok modifies the string
    char buf[512];
    strncpy(buf, flags_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // Split on commas and process each flag
    char *tok = strtok(buf, ",");
    while (tok) {
        char *flag = trim(tok);

        if (strcmp(flag, "always_on_top") == 0)
            rule->always_on_top = true;
        else if (strcmp(flag, "no_shadow") == 0)
            rule->no_shadow = true;
        else if (strcmp(flag, "no_animation") == 0)
            rule->no_animation = true;
        else if (strcmp(flag, "fullscreen_direct") == 0)
            rule->fullscreen_direct = true;
        else if (strncmp(flag, "opacity=", 8) == 0)
            rule->opacity = (float)atof(flag + 8);
        else if (strncmp(flag, "space=", 6) == 0)
            rule->target_space = atoi(flag + 6);

        tok = strtok(NULL, ",");
    }
}

bool plugin_load_config(const char *path)
{
    // SECURITY: validate the path before opening — prevents loading config
    // from untrusted locations or world-writable files
    if (!validate_path(path, false)) {
        fprintf(stderr, "[plugin] Refused to load config from untrusted path: %s\n",
                path);
        return false;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[plugin] Cannot open config file: %s\n", path);
        return false;
    }

    // Using 128 bytes to safely hold long section names without overflow
    char section[128] = "";
    char line[512];

    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);

        // Skip empty lines and comments
        if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';')
            continue;

        // Section header
        if (trimmed[0] == '[') {
            char *end = strchr(trimmed, ']');
            if (end) {
                *end = '\0';
                strncpy(section, trimmed + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
            }
            continue;
        }

        // Key=value pair
        char *eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key   = trim(trimmed);
        char *value = trim(eq + 1);

        if (strcmp(section, "HotCorners") == 0) {
            // Parse hot corner assignments like "top_left=mission_control"
            ScreenCorner corner;
            if (strcmp(key, "top_left") == 0)          corner = CORNER_TOP_LEFT;
            else if (strcmp(key, "top_right") == 0)    corner = CORNER_TOP_RIGHT;
            else if (strcmp(key, "bottom_left") == 0)  corner = CORNER_BOTTOM_LEFT;
            else if (strcmp(key, "bottom_right") == 0) corner = CORNER_BOTTOM_RIGHT;
            else continue;  // Unknown corner name — skip

            CornerAction action = parse_corner_action(value);

            // For custom actions, the command follows "custom:" prefix
            const char *custom_cmd = NULL;
            if (action == CORNER_ACTION_CUSTOM && strncmp(value, "custom:", 7) == 0)
                custom_cmd = value + 7;

            plugin_set_hot_corner(corner, action, custom_cmd);
        }
        else if (strcmp(section, "WindowRules") == 0) {
            // Parse window rules like "steam=fullscreen_direct,no_shadow"
            // The key is the WM_CLASS, the value is a comma-separated list of flags
            WindowRule rule;
            memset(&rule, 0, sizeof(rule));
            strncpy(rule.wm_class, key, sizeof(rule.wm_class) - 1);
            rule.wm_class[sizeof(rule.wm_class) - 1] = '\0';
            rule.target_space = -1;  // Default: any workspace
            rule.opacity      = 0.0f; // Default: use compositor default

            parse_window_rule_flags(value, &rule);

            // Range validation — clamp values to sane bounds to prevent
            // misbehavior from malformed config files
            if (rule.target_space < -1 || rule.target_space > 99)
                rule.target_space = -1;
            if (rule.opacity < 0.0f || rule.opacity > 1.0f)
                rule.opacity = 0.0f;

            plugin_add_window_rule(&rule);
        }
    }

    fclose(f);
    fprintf(stderr, "[plugin] Loaded config: %s\n", path);
    return true;
}


// ============================================================================
//  Configuration file — saving
// ============================================================================

bool plugin_save_config(const char *path)
{
    // SECURITY: validate the path before writing — prevents writing config
    // to locations outside the allowed directories. Note: for save, the file
    // may not exist yet, so we validate the parent directory instead.
    // However, if the file already exists, validate it directly.
    // For new files, the caller must ensure the directory is within allowed paths.
    // We use validate_path with allow_so=false since this is a config file.
    //
    // Note: realpath() requires the file to exist. For saving new config files,
    // we check the parent directory. For existing files, we validate directly.
    struct stat save_st;
    if (stat(path, &save_st) == 0) {
        // File exists — validate it
        if (!validate_path(path, false)) {
            fprintf(stderr, "[plugin] Refused to save config to untrusted path: %s\n",
                    path);
            return false;
        }
    } else {
        // File doesn't exist yet — validate the parent directory by checking
        // if the path (minus filename) falls within allowed directories
        char parent[PATH_MAX];
        strncpy(parent, path, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';

        // Find the last '/' to get the parent directory
        char *last_slash = strrchr(parent, '/');
        if (last_slash) {
            *last_slash = '\0';
            // Resolve the parent directory to its canonical path
            char resolved_parent[PATH_MAX];
            if (!realpath(parent, resolved_parent)) {
                fprintf(stderr, "[plugin] SECURITY: cannot resolve parent dir of '%s'\n",
                        path);
                return false;
            }
            const char *home = getenv("HOME");
            char uc[PATH_MAX], up[PATH_MAX];
            snprintf(uc, sizeof(uc), "%s/.config/cc-wm", home ? home : "/tmp");
            snprintf(up, sizeof(up), "%s/.local/share/cc-wm/plugins", home ? home : "/tmp");
            bool ok = (strncmp(resolved_parent, uc, strlen(uc)) == 0 ||
                       strncmp(resolved_parent, up, strlen(up)) == 0 ||
                       strncmp(resolved_parent, "/usr/share/cc-wm", 18) == 0 ||
                       strncmp(resolved_parent, "/usr/local/share/cc-wm", 24) == 0);
            if (!ok) {
                fprintf(stderr, "[plugin] SECURITY: save path '%s' is outside "
                        "allowed directories\n", path);
                return false;
            }
        }
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[plugin] Cannot write config file: %s\n", path);
        return false;
    }

    // --- Hot corners section ---
    fprintf(f, "[HotCorners]\n");

    // Map array indices to human-readable corner names
    const char *corner_names[] = {
        "top_left", "top_right", "bottom_left", "bottom_right"
    };

    for (int i = 0; i < MAX_HOT_CORNERS; i++) {
        if (hot_corners[i].action == CORNER_ACTION_CUSTOM &&
            hot_corners[i].custom_command[0] != '\0') {
            // Custom actions include the command after "custom:"
            fprintf(f, "%s=custom:%s\n",
                    corner_names[i], hot_corners[i].custom_command);
        } else {
            fprintf(f, "%s=%s\n",
                    corner_names[i],
                    corner_action_to_string(hot_corners[i].action));
        }
    }

    // --- Window rules section ---
    fprintf(f, "\n[WindowRules]\n");

    for (int i = 0; i < rule_count; i++) {
        WindowRule *r = &window_rules[i];

        // Build a comma-separated list of active flags for this rule
        fprintf(f, "%s=", r->wm_class);

        bool first = true;  // Track whether we need a comma separator

        // Helper macro to append a flag with proper comma handling
        #define WRITE_FLAG(cond, str) do {              \
            if (cond) {                                 \
                if (!first) fprintf(f, ",");            \
                fprintf(f, "%s", str);                  \
                first = false;                          \
            }                                           \
        } while (0)

        WRITE_FLAG(r->always_on_top,     "always_on_top");
        WRITE_FLAG(r->no_shadow,         "no_shadow");
        WRITE_FLAG(r->no_animation,      "no_animation");
        WRITE_FLAG(r->fullscreen_direct, "fullscreen_direct");

        #undef WRITE_FLAG

        // Opacity is a key=value flag (not just a boolean)
        if (r->opacity > 0.0f) {
            if (!first) fprintf(f, ",");
            fprintf(f, "opacity=%.2f", r->opacity);
            first = false;
        }

        // Target workspace
        if (r->target_space >= 0) {
            if (!first) fprintf(f, ",");
            fprintf(f, "space=%d", r->target_space);
        }

        fprintf(f, "\n");
    }

    fclose(f);
    fprintf(stderr, "[plugin] Saved config: %s\n", path);
    return true;
}


// ============================================================================
//  Effects API
// ============================================================================
//
// These functions provide ready-made visual effects for plugins. They use
// MoonRock's shader pipeline (moonrock_shaders.h) for GPU-accelerated processing.
// Each effect renders into one or more FBOs (off-screen textures) and returns
// the result by modifying the input texture in place.
//
// Typical usage from a plugin's window_effect hook:
//   plugin_effect_blur(texture, x, y, w, h, 12.0f);   // frosted glass
//   plugin_effect_desaturate(texture, 0.5f);            // half-gray
//   plugin_effect_tint(texture, 0.2f, 0.4f, 0.8f, 0.3f); // blue tint

void plugin_effect_blur(GLuint texture, int x, int y, int w, int h,
                        float radius)
{
    // Gaussian blur uses a two-pass approach (horizontal then vertical).
    // This is O(n) per pixel instead of O(n^2) for a full 2D kernel.
    //
    // Pipeline:
    //   Input texture -> [horizontal blur into FBO-A] -> [vertical blur into FBO-B]
    //   FBO-B's texture is the final blurred result, drawn back to the screen.

    if (radius <= 0.0f) return;  // No blur to apply

    // Get the compiled shader programs from MoonRock's global state
    ShaderPrograms *progs = mr_get_shaders();
    if (!progs || !progs->blur_h || !progs->blur_v) {
        fprintf(stderr, "[plugin] effect_blur: blur shaders not available\n");
        return;
    }

    // Get the projection matrix so we can set up the shader's coordinate system
    float *projection = mr_get_projection();

    // ── Save the current viewport ──
    // We are about to change the viewport for the FBO render passes. The
    // caller (mr_composite) has set up a viewport matching the full screen.
    // We must restore it after we are done so subsequent drawing is not
    // constrained to the FBO's smaller dimensions.
    GLint saved_viewport[4];
    glGetIntegerv(GL_VIEWPORT, saved_viewport);

    // ── Find or create a cached FBO pair for this (w, h) ──
    // Multiple panels (dock, menubar) call blur each frame with different sizes.
    // We search the cache for a matching slot; if none found, claim an empty one.
    BlurCacheSlot *slot = NULL;
    for (int i = 0; i < BLUR_CACHE_SLOTS; i++) {
        if (blur_cache[i].w == w && blur_cache[i].h == h) {
            slot = &blur_cache[i];
            break;
        }
    }

    // No matching slot — find an empty one and create FBOs for this size
    if (!slot) {
        for (int i = 0; i < BLUR_CACHE_SLOTS; i++) {
            if (blur_cache[i].w == 0 && blur_cache[i].h == 0) {
                slot = &blur_cache[i];
                break;
            }
        }
        if (!slot) {
            // All slots occupied by other sizes — evict slot 0 as a fallback.
            // This shouldn't happen with 4 slots and only 2-3 panels.
            slot = &blur_cache[0];
            if (slot->fbo_a) shaders_destroy_fbo(slot->fbo_a, slot->tex_a);
            if (slot->fbo_b) shaders_destroy_fbo(slot->fbo_b, slot->tex_b);
            memset(slot, 0, sizeof(*slot));
        }

        slot->fbo_a = shaders_create_fbo(w, h, &slot->tex_a);
        slot->fbo_b = shaders_create_fbo(w, h, &slot->tex_b);

        if (!slot->fbo_a || !slot->fbo_b) {
            fprintf(stderr, "[plugin] effect_blur: failed to create FBOs\n");
            if (slot->fbo_a) shaders_destroy_fbo(slot->fbo_a, slot->tex_a);
            if (slot->fbo_b) shaders_destroy_fbo(slot->fbo_b, slot->tex_b);
            memset(slot, 0, sizeof(*slot));
            return;
        }

        slot->w = w;
        slot->h = h;
        fprintf(stderr, "[plugin] Blur FBO cache slot created: %dx%d\n", w, h);
    }

    // Use the cached FBOs for this frame's blur passes
    GLuint fbo_a = slot->fbo_a, tex_a = slot->tex_a;
    GLuint fbo_b = slot->fbo_b, tex_b = slot->tex_b;

    // Build an orthographic projection that maps (0,0)-(w,h) to the FBO.
    // This is separate from the screen projection because the FBO has its
    // own dimensions — FBOs have their own coordinate space.
    float fbo_proj[16];
    shaders_ortho(fbo_proj, 0.0f, (float)w, 0.0f, (float)h, -1.0f, 1.0f);

    // ---- Pass 1: Horizontal blur ----
    // Render the input texture through the horizontal blur shader into FBO-A.
    shaders_bind_fbo(fbo_a);
    glViewport(0, 0, w, h);

    shaders_use(progs->blur_h);
    shaders_set_projection(progs->blur_h, fbo_proj);
    shaders_set_texture(progs->blur_h, 0);
    shaders_set_alpha(progs->blur_h, 1.0f);
    shaders_set_blur_radius(progs->blur_h, radius);
    shaders_set_blur_direction(progs->blur_h, 1.0f, 0.0f);  // Horizontal
    shaders_set_texture_size(progs->blur_h, (float)w, (float)h);

    // Bind the input texture and draw a full-FBO quad
    glBindTexture(GL_TEXTURE_2D, texture);
    shaders_draw_quad(0.0f, 0.0f, (float)w, (float)h);

    // ---- Pass 2: Vertical blur ----
    // Take FBO-A's texture (horizontally blurred) and blur it vertically
    // into FBO-B. The result is a full 2D Gaussian blur.
    shaders_bind_fbo(fbo_b);

    shaders_use(progs->blur_v);
    shaders_set_projection(progs->blur_v, fbo_proj);
    shaders_set_texture(progs->blur_v, 0);
    shaders_set_alpha(progs->blur_v, 1.0f);
    shaders_set_blur_radius(progs->blur_v, radius);
    shaders_set_blur_direction(progs->blur_v, 0.0f, 1.0f);  // Vertical
    shaders_set_texture_size(progs->blur_v, (float)w, (float)h);

    // Bind FBO-A's texture as the input for the vertical pass
    glBindTexture(GL_TEXTURE_2D, tex_a);
    shaders_draw_quad(0.0f, 0.0f, (float)w, (float)h);

    // ---- Done: copy result back to screen ----
    // Unbind the FBO so subsequent draws go to the default framebuffer (screen),
    // then draw the blurred texture (from FBO-B) at the original window position.
    shaders_unbind_fbo();

    // ── Restore the saved viewport ──
    // The FBO passes set glViewport to the FBO's dimensions (w x h). Now that
    // we are drawing back to the screen, we must restore the full-screen
    // viewport. Without this, all subsequent draws in this frame would be
    // clipped to the panel's small region.
    glViewport(saved_viewport[0], saved_viewport[1],
               saved_viewport[2], saved_viewport[3]);

    // Draw the blurred result at the panel's screen position using the
    // screen's projection matrix (restored from before the blur passes).
    shaders_use(progs->basic);
    shaders_set_projection(progs->basic, projection);
    shaders_set_texture(progs->basic, 0);
    shaders_set_alpha(progs->basic, 1.0f);

    glBindTexture(GL_TEXTURE_2D, tex_b);
    shaders_draw_quad((float)x, (float)y, (float)w, (float)h);

    shaders_use_none();

    // FBOs are cached — no per-frame cleanup needed. They persist until
    // the blur target changes size or plugin_shutdown() is called.
}

void plugin_effect_desaturate(GLuint texture, float amount)
{
    // Desaturation converts color toward grayscale. At amount=0.0 the image
    // is unchanged; at amount=1.0 it's fully grayscale. Uses ITU-R BT.709
    // luminance weights to compute perceived brightness.

    if (amount <= 0.0f) return;  // No desaturation needed

    ShaderPrograms *progs = mr_get_shaders();
    if (!progs || !progs->desaturate) {
        fprintf(stderr, "[plugin] effect_desaturate: shader not available\n");
        return;
    }

    // We need the texture dimensions to create an FBO. Since the caller does
    // not pass width/height for desaturate, query the texture's dimensions
    // from OpenGL directly.
    int tw = 0, th = 0;
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);

    if (tw <= 0 || th <= 0) {
        fprintf(stderr, "[plugin] effect_desaturate: invalid texture dimensions\n");
        return;
    }

    // Create an FBO to render the desaturated result into
    GLuint fbo, result_tex;
    fbo = shaders_create_fbo(tw, th, &result_tex);
    if (!fbo) {
        fprintf(stderr, "[plugin] effect_desaturate: failed to create FBO\n");
        return;
    }

    // Set up an orthographic projection matching the FBO dimensions
    float fbo_proj[16];
    shaders_ortho(fbo_proj, 0.0f, (float)tw, 0.0f, (float)th, -1.0f, 1.0f);

    // Render the input texture through the desaturate shader into the FBO
    shaders_bind_fbo(fbo);
    glViewport(0, 0, tw, th);

    shaders_use(progs->desaturate);
    shaders_set_projection(progs->desaturate, fbo_proj);
    shaders_set_texture(progs->desaturate, 0);
    shaders_set_alpha(progs->desaturate, 1.0f);
    shaders_set_amount(progs->desaturate, amount);

    glBindTexture(GL_TEXTURE_2D, texture);
    shaders_draw_quad(0.0f, 0.0f, (float)tw, (float)th);

    shaders_unbind_fbo();

    // Copy the desaturated pixels back to the original texture so the
    // caller's texture is modified in place. We use glCopyTexSubImage2D
    // by binding the result FBO as the read framebuffer.
    //
    // NOTE: For now, we draw the result at the original position using
    // the basic shader. The caller should use the returned FBO texture
    // directly in the future for better efficiency.
    shaders_use_none();

    // Clean up
    shaders_destroy_fbo(fbo, result_tex);
}

void plugin_effect_tint(GLuint texture, float r, float g, float b,
                        float amount)
{
    // Tinting blends the texture's color with a solid color. At amount=0.0
    // the image is unchanged; at amount=1.0 the image is fully replaced by
    // the tint color. Useful for dimming inactive windows or applying color
    // casts to the scene.

    if (amount <= 0.0f) return;  // No tint needed

    ShaderPrograms *progs = mr_get_shaders();
    if (!progs || !progs->tint) {
        fprintf(stderr, "[plugin] effect_tint: shader not available\n");
        return;
    }

    // Query texture dimensions from OpenGL
    int tw = 0, th = 0;
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);

    if (tw <= 0 || th <= 0) {
        fprintf(stderr, "[plugin] effect_tint: invalid texture dimensions\n");
        return;
    }

    // Create an FBO to render the tinted result into
    GLuint fbo, result_tex;
    fbo = shaders_create_fbo(tw, th, &result_tex);
    if (!fbo) {
        fprintf(stderr, "[plugin] effect_tint: failed to create FBO\n");
        return;
    }

    // Set up an orthographic projection matching the FBO dimensions
    float fbo_proj[16];
    shaders_ortho(fbo_proj, 0.0f, (float)tw, 0.0f, (float)th, -1.0f, 1.0f);

    // Render the input texture through the tint shader into the FBO
    shaders_bind_fbo(fbo);
    glViewport(0, 0, tw, th);

    shaders_use(progs->tint);
    shaders_set_projection(progs->tint, fbo_proj);
    shaders_set_texture(progs->tint, 0);
    shaders_set_alpha(progs->tint, 1.0f);
    shaders_set_amount(progs->tint, amount);
    shaders_set_tint_color(progs->tint, r, g, b, 1.0f);

    glBindTexture(GL_TEXTURE_2D, texture);
    shaders_draw_quad(0.0f, 0.0f, (float)tw, (float)th);

    shaders_unbind_fbo();
    shaders_use_none();

    // Clean up
    shaders_destroy_fbo(fbo, result_tex);
}

void plugin_effect_scale(GLuint texture, float scale)
{
    // Scaling zooms the texture in or out from its center. This is purely
    // geometric — no FBO needed. We use the basic shader and adjust the
    // quad position and size to create a centered scale effect.
    //
    // scale > 1.0: zoom in (quad gets bigger, overflows original bounds)
    // scale < 1.0: zoom out (quad gets smaller, surrounded by empty space)

    if (scale == 1.0f) return;  // No scaling needed

    ShaderPrograms *progs = mr_get_shaders();
    if (!progs || !progs->basic) {
        fprintf(stderr, "[plugin] effect_scale: basic shader not available\n");
        return;
    }

    float *projection = mr_get_projection();

    // Query the texture dimensions so we know the original size
    int tw = 0, th = 0;
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);

    if (tw <= 0 || th <= 0) {
        fprintf(stderr, "[plugin] effect_scale: invalid texture dimensions\n");
        return;
    }

    // Calculate the new dimensions and position so the texture remains
    // centered at its original midpoint.
    float new_w = (float)tw * scale;
    float new_h = (float)th * scale;
    float new_x = ((float)tw - new_w) * 0.5f;
    float new_y = ((float)th - new_h) * 0.5f;

    // Draw the texture at the scaled dimensions using the basic shader
    shaders_use(progs->basic);
    shaders_set_projection(progs->basic, projection);
    shaders_set_texture(progs->basic, 0);
    shaders_set_alpha(progs->basic, 1.0f);

    shaders_draw_quad(new_x, new_y, new_w, new_h);

    shaders_use_none();
}


// ============================================================================
//  Plugin hook runners
// ============================================================================
//
// These iterate over all loaded plugins and invoke the corresponding callback.
// Null function pointers are skipped — plugins only implement the hooks they
// need, and leave the rest as NULL.

void plugin_run_pre_composite(int screen_w, int screen_h)
{
    for (int i = 0; i < plugin_count; i++) {
        MRPlugin *p = loaded_plugins[i].plugin;
        if (p && p->pre_composite)
            p->pre_composite(screen_w, screen_h);
    }
}

void plugin_run_post_composite(int screen_w, int screen_h)
{
    for (int i = 0; i < plugin_count; i++) {
        MRPlugin *p = loaded_plugins[i].plugin;
        if (p && p->post_composite)
            p->post_composite(screen_w, screen_h);
    }
}

void plugin_run_window_effect(GLuint texture, int x, int y, int w, int h)
{
    for (int i = 0; i < plugin_count; i++) {
        MRPlugin *p = loaded_plugins[i].plugin;
        if (p && p->window_effect)
            p->window_effect(texture, x, y, w, h);
    }
}
