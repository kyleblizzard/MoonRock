// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// ============================================================================
//  Crystal Shaders — GLSL shader compilation, linking, and management
// ============================================================================
//
// This module replaces Crystal's fixed-function OpenGL pipeline with
// programmable GLSL shaders. Every visual effect Crystal renders — textured
// windows, Gaussian blur, drop shadows, genie animations — flows through
// a shader program compiled and managed here.
//
// The module handles:
//   1. Loading GL 2.0+ function pointers at runtime (glCreateShader, etc.)
//   2. Compiling vertex and fragment shaders from files or embedded fallbacks
//   3. Linking shader pairs into GPU programs
//   4. Setting uniform variables (projection matrix, alpha, blur params)
//   5. VBO-based quad rendering (replacing slow glBegin/glEnd calls)
//   6. FBO management for off-screen multi-pass effects (blur, etc.)
//
// Why load GL function pointers manually?
//   The base GL/gl.h header only declares OpenGL 1.1 functions. Everything
//   from OpenGL 2.0 onward (shaders, VBOs, FBOs) must be loaded at runtime
//   using glXGetProcAddress(). This is because the actual function addresses
//   depend on the GPU driver installed on the user's system — they aren't
//   known at compile time.
//
// ============================================================================

#define _GNU_SOURCE
#include "crystal_shaders.h"

#include <GL/gl.h>
#include <GL/glext.h>    // GL extension constants (GL_VERTEX_SHADER, etc.)
#include <GL/glx.h>      // glXGetProcAddress — loads GL functions at runtime
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// ============================================================================
//  SECTION: GL 2.0+ function pointers
// ============================================================================
//
// These are loaded at runtime from the GPU driver via glXGetProcAddress().
// Each typedef (PFNGL...PROC) is defined in GL/glext.h and describes the
// exact function signature. We store one function pointer for each GL call
// we need that isn't in the base OpenGL 1.1 API.
//
// Naming convention:
//   pfn_glCreateShader  =>  our local pointer to the real glCreateShader()
//   PFNGLCREATESHADERPROC  =>  the typedef for that function's signature
//     (PFN = "Pointer to FunctioN", PROC = "procedure")

// --- Shader compilation and program linking ---
static PFNGLCREATESHADERPROC         pfn_glCreateShader        = NULL;
static PFNGLSHADERSOURCEPROC         pfn_glShaderSource        = NULL;
static PFNGLCOMPILESHADERPROC        pfn_glCompileShader       = NULL;
static PFNGLGETSHADERIVPROC          pfn_glGetShaderiv         = NULL;
static PFNGLGETSHADERINFOLOGPROC     pfn_glGetShaderInfoLog    = NULL;
static PFNGLDELETESHADERPROC         pfn_glDeleteShader        = NULL;
static PFNGLCREATEPROGRAMPROC        pfn_glCreateProgram       = NULL;
static PFNGLATTACHSHADERPROC         pfn_glAttachShader        = NULL;
static PFNGLLINKPROGRAMPROC          pfn_glLinkProgram         = NULL;
static PFNGLGETPROGRAMIVPROC         pfn_glGetProgramiv        = NULL;
static PFNGLGETPROGRAMINFOLOGPROC    pfn_glGetProgramInfoLog   = NULL;
static PFNGLDELETEPROGRAMPROC        pfn_glDeleteProgram       = NULL;
static PFNGLUSEPROGRAMPROC           pfn_glUseProgram          = NULL;

// --- Uniform variables (sending data from CPU to GPU shaders) ---
static PFNGLGETUNIFORMLOCATIONPROC   pfn_glGetUniformLocation  = NULL;
static PFNGLUNIFORM1IPROC           pfn_glUniform1i           = NULL;
static PFNGLUNIFORM1FPROC           pfn_glUniform1f           = NULL;
static PFNGLUNIFORM2FPROC           pfn_glUniform2f           = NULL;
static PFNGLUNIFORM4FPROC           pfn_glUniform4f           = NULL;
static PFNGLUNIFORMMATRIX4FVPROC    pfn_glUniformMatrix4fv    = NULL;

// --- Framebuffer Objects (off-screen render targets) ---
static PFNGLGENFRAMEBUFFERSPROC          pfn_glGenFramebuffers          = NULL;
static PFNGLDELETEFRAMEBUFFERSPROC       pfn_glDeleteFramebuffers       = NULL;
static PFNGLBINDFRAMEBUFFERPROC          pfn_glBindFramebuffer          = NULL;
static PFNGLFRAMEBUFFERTEXTURE2DPROC     pfn_glFramebufferTexture2D     = NULL;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC   pfn_glCheckFramebufferStatus   = NULL;

// --- Vertex Array Objects (VAO) — group vertex attribute state ---
static PFNGLGENVERTEXARRAYSPROC      pfn_glGenVertexArrays     = NULL;
static PFNGLDELETEVERTEXARRAYSPROC   pfn_glDeleteVertexArrays  = NULL;
static PFNGLBINDVERTEXARRAYPROC      pfn_glBindVertexArray     = NULL;

// --- Vertex Buffer Objects (VBO) and Element Buffer Objects (EBO) ---
static PFNGLGENBUFFERSPROC           pfn_glGenBuffers          = NULL;
static PFNGLDELETEBUFFERSPROC        pfn_glDeleteBuffers       = NULL;
static PFNGLBINDBUFFERPROC           pfn_glBindBuffer          = NULL;
static PFNGLBUFFERDATAPROC           pfn_glBufferData          = NULL;
static PFNGLBUFFERSUBDATAPROC        pfn_glBufferSubData       = NULL;

// --- Vertex attribute setup ---
static PFNGLVERTEXATTRIBPOINTERPROC    pfn_glVertexAttribPointer    = NULL;
static PFNGLENABLEVERTEXATTRIBARRAYPROC pfn_glEnableVertexAttribArray = NULL;


// ============================================================================
//  SECTION: GL function loader
// ============================================================================

// load_gl_functions — Load all GL 2.0+ function pointers we need.
//
// This must be called AFTER a valid GL context has been created and made
// current (by crystal_init). Without a context, glXGetProcAddress returns
// garbage pointers.
//
// Returns true if all required functions were loaded, false if any failed.
static bool load_gl_functions(void)
{
    // Helper macro: load a function pointer and check for failure.
    // 'name' is the OpenGL function name (e.g., glCreateShader).
    // 'type' is the typedef for its signature (e.g., PFNGLCREATESHADERPROC).
    #define LOAD_GL(name, type) do {                                         \
        pfn_##name = (type)glXGetProcAddress((const GLubyte *)#name);        \
        if (!pfn_##name) {                                                   \
            fprintf(stderr, "[crystal] Failed to load GL function: %s\n",   \
                    #name);                                                  \
            return false;                                                    \
        }                                                                    \
    } while (0)

    // Shader compilation
    LOAD_GL(glCreateShader,        PFNGLCREATESHADERPROC);
    LOAD_GL(glShaderSource,        PFNGLSHADERSOURCEPROC);
    LOAD_GL(glCompileShader,       PFNGLCOMPILESHADERPROC);
    LOAD_GL(glGetShaderiv,         PFNGLGETSHADERIVPROC);
    LOAD_GL(glGetShaderInfoLog,    PFNGLGETSHADERINFOLOGPROC);
    LOAD_GL(glDeleteShader,        PFNGLDELETESHADERPROC);

    // Program linking
    LOAD_GL(glCreateProgram,       PFNGLCREATEPROGRAMPROC);
    LOAD_GL(glAttachShader,        PFNGLATTACHSHADERPROC);
    LOAD_GL(glLinkProgram,         PFNGLLINKPROGRAMPROC);
    LOAD_GL(glGetProgramiv,        PFNGLGETPROGRAMIVPROC);
    LOAD_GL(glGetProgramInfoLog,   PFNGLGETPROGRAMINFOLOGPROC);
    LOAD_GL(glDeleteProgram,       PFNGLDELETEPROGRAMPROC);
    LOAD_GL(glUseProgram,          PFNGLUSEPROGRAMPROC);

    // Uniforms
    LOAD_GL(glGetUniformLocation,  PFNGLGETUNIFORMLOCATIONPROC);
    LOAD_GL(glUniform1i,           PFNGLUNIFORM1IPROC);
    LOAD_GL(glUniform1f,           PFNGLUNIFORM1FPROC);
    LOAD_GL(glUniform2f,           PFNGLUNIFORM2FPROC);
    LOAD_GL(glUniform4f,           PFNGLUNIFORM4FPROC);
    LOAD_GL(glUniformMatrix4fv,    PFNGLUNIFORMMATRIX4FVPROC);

    // Framebuffer objects
    LOAD_GL(glGenFramebuffers,        PFNGLGENFRAMEBUFFERSPROC);
    LOAD_GL(glDeleteFramebuffers,     PFNGLDELETEFRAMEBUFFERSPROC);
    LOAD_GL(glBindFramebuffer,        PFNGLBINDFRAMEBUFFERPROC);
    LOAD_GL(glFramebufferTexture2D,   PFNGLFRAMEBUFFERTEXTURE2DPROC);
    LOAD_GL(glCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC);

    // Vertex array objects
    LOAD_GL(glGenVertexArrays,     PFNGLGENVERTEXARRAYSPROC);
    LOAD_GL(glDeleteVertexArrays,  PFNGLDELETEVERTEXARRAYSPROC);
    LOAD_GL(glBindVertexArray,     PFNGLBINDVERTEXARRAYPROC);

    // Vertex/element buffer objects
    LOAD_GL(glGenBuffers,          PFNGLGENBUFFERSPROC);
    LOAD_GL(glDeleteBuffers,       PFNGLDELETEBUFFERSPROC);
    LOAD_GL(glBindBuffer,          PFNGLBINDBUFFERPROC);
    LOAD_GL(glBufferData,          PFNGLBUFFERDATAPROC);
    LOAD_GL(glBufferSubData,       PFNGLBUFFERSUBDATAPROC);

    // Vertex attributes
    LOAD_GL(glVertexAttribPointer,    PFNGLVERTEXATTRIBPOINTERPROC);
    LOAD_GL(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC);

    #undef LOAD_GL

    fprintf(stderr, "[crystal] All GL 2.0+ functions loaded successfully\n");
    return true;
}


// ============================================================================
//  SECTION: Embedded fallback shader sources
// ============================================================================
//
// These are the GLSL shader source strings compiled into the binary. If the
// external .vert/.frag files can't be found, Crystal uses these instead.
// This guarantees the compositor always starts, even without a data directory.
//
// GLSL version 120 targets OpenGL 2.1, which is the minimum we require.
// Version 120 supports:
//   - 'attribute' for per-vertex inputs (position, texcoord)
//   - 'varying' for data passed from vertex to fragment shader
//   - 'uniform' for CPU-to-GPU parameters
//   - Built-in gl_Position output in vertex shaders
//   - texture2D() for sampling textures

// ---- Basic vertex shader ----
// Transforms a unit quad (0..1) to screen position using a model matrix,
// then to clip space using the projection matrix. Passes texture coordinates
// through to the fragment shader.
static const char *BASIC_VERT_SRC =
    "#version 120\n"
    "\n"
    "// Per-vertex inputs (from the VBO)\n"
    "attribute vec2 a_position;  // Vertex position in unit-quad space (0..1)\n"
    "attribute vec2 a_texcoord;  // Texture coordinate (0..1)\n"
    "\n"
    "// Passed to the fragment shader (interpolated across the triangle)\n"
    "varying vec2 v_texcoord;\n"
    "\n"
    "// Uniforms set by the CPU each draw call\n"
    "uniform mat4 u_projection;  // Orthographic projection (screen -> clip)\n"
    "uniform mat4 u_model;       // Model transform (position + scale the quad)\n"
    "\n"
    "void main() {\n"
    "    // Transform: unit quad -> screen position -> clip space\n"
    "    gl_Position = u_projection * u_model * vec4(a_position, 0.0, 1.0);\n"
    "    // Pass texture coordinate to fragment shader unchanged\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

// ---- Basic fragment shader ----
// Samples a texture and multiplies by a global alpha for opacity control.
static const char *BASIC_FRAG_SRC =
    "#version 120\n"
    "\n"
    "varying vec2 v_texcoord;    // Interpolated texture coordinate from vertex shader\n"
    "\n"
    "uniform sampler2D u_texture;  // The texture to sample (window contents, etc.)\n"
    "uniform float u_alpha;        // Global opacity multiplier (0.0 = invisible, 1.0 = opaque)\n"
    "\n"
    "void main() {\n"
    "    // Sample the texture at this pixel's coordinate\n"
    "    vec4 color = texture2D(u_texture, v_texcoord);\n"
    "    // Apply the global alpha (for fade effects, inactive dimming, etc.)\n"
    "    color.a *= u_alpha;\n"
    "    gl_FragColor = color;\n"
    "}\n";

// ---- Gaussian blur fragment shader ----
// Performs a 1D Gaussian blur in a single direction (horizontal OR vertical).
// Two-pass blur: render once with direction=(1,0), then again with direction=(0,1).
// This is much faster than a 2D blur because it's O(n) instead of O(n^2).
//
// The blur kernel weights are computed on the fly using the Gaussian formula.
// We sample (2*radius + 1) texels along the blur direction.
static const char *BLUR_FRAG_SRC =
    "#version 120\n"
    "\n"
    "varying vec2 v_texcoord;\n"
    "\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_alpha;\n"
    "uniform float u_blur_radius;      // Blur spread in pixels\n"
    "uniform vec2 u_blur_direction;     // (1,0) for horizontal, (0,1) for vertical\n"
    "uniform vec2 u_texture_size;       // Texture dimensions in pixels\n"
    "\n"
    "void main() {\n"
    "    // Convert the blur direction from pixels to texture coordinates.\n"
    "    // Texture coords go from 0 to 1, so 1 pixel = 1/texture_size.\n"
    "    vec2 tex_offset = u_blur_direction / u_texture_size;\n"
    "\n"
    "    // Start with the center texel (weight = 1.0, highest contribution)\n"
    "    vec4 result = texture2D(u_texture, v_texcoord);\n"
    "    float total_weight = 1.0;\n"
    "\n"
    "    // Sample texels on both sides of center, applying Gaussian weights.\n"
    "    // The Gaussian weight decreases as we move further from center:\n"
    "    //   weight = exp(-(i*i) / (2 * sigma^2))\n"
    "    // We use sigma = radius/2 for a nice falloff.\n"
    "    float sigma = max(u_blur_radius * 0.5, 1.0);\n"
    "    float two_sigma_sq = 2.0 * sigma * sigma;\n"
    "\n"
    "    // Loop over the blur kernel (both directions from center)\n"
    "    // GLSL 1.20 doesn't support non-constant loop bounds, so we use\n"
    "    // a fixed maximum and break early.\n"
    "    for (int i = 1; i <= 32; i++) {\n"
    "        if (float(i) > u_blur_radius) break;\n"
    "\n"
    "        // Gaussian weight for this sample distance\n"
    "        float w = exp(-float(i * i) / two_sigma_sq);\n"
    "        total_weight += 2.0 * w;  // Two samples (positive + negative)\n"
    "\n"
    "        // Sample in both directions along the blur axis\n"
    "        vec2 offset = tex_offset * float(i);\n"
    "        result += texture2D(u_texture, v_texcoord + offset) * w;\n"
    "        result += texture2D(u_texture, v_texcoord - offset) * w;\n"
    "    }\n"
    "\n"
    "    // Normalize by the total weight so brightness is preserved\n"
    "    result /= total_weight;\n"
    "    result.a *= u_alpha;\n"
    "    gl_FragColor = result;\n"
    "}\n";

// ---- Shadow fragment shader ----
// Renders a shadow by treating the texture as an alpha mask. The shadow color
// is always black — we only care about the shape (alpha channel). A uniform
// controls the shadow's opacity.
static const char *SHADOW_FRAG_SRC =
    "#version 120\n"
    "\n"
    "varying vec2 v_texcoord;\n"
    "\n"
    "uniform sampler2D u_texture;  // Shadow shape texture (only alpha matters)\n"
    "uniform float u_alpha;        // Shadow opacity\n"
    "\n"
    "void main() {\n"
    "    // Use only the alpha channel from the texture as the shadow shape.\n"
    "    // The RGB channels are forced to black (0,0,0) — shadows are always dark.\n"
    "    float shadow_alpha = texture2D(u_texture, v_texcoord).a;\n"
    "    gl_FragColor = vec4(0.0, 0.0, 0.0, shadow_alpha * u_alpha);\n"
    "}\n";

// ---- Solid color fragment shader ----
// No texture sampling at all — just outputs a flat color. Used for drawing
// solid rectangles (selection highlights, debug overlays, color fills).
static const char *SOLID_FRAG_SRC =
    "#version 120\n"
    "\n"
    "uniform vec4 u_color;  // The solid color to output (RGBA)\n"
    "\n"
    "void main() {\n"
    "    gl_FragColor = u_color;\n"
    "}\n";

// ---- Genie vertex shader ----
// Warps a flat grid of vertices into a funnel shape for the minimize animation.
// The distortion is controlled by u_genie_progress (0 = flat, 1 = fully warped)
// and u_genie_target (the screen position the window funnels toward).
static const char *GENIE_VERT_SRC =
    "#version 120\n"
    "\n"
    "attribute vec2 a_position;  // Vertex position in unit-quad space\n"
    "attribute vec2 a_texcoord;\n"
    "\n"
    "varying vec2 v_texcoord;\n"
    "\n"
    "uniform mat4 u_projection;\n"
    "uniform mat4 u_model;\n"
    "uniform float u_genie_progress;  // Animation progress: 0.0 = normal, 1.0 = minimized\n"
    "uniform vec2 u_genie_target;     // Screen position of the dock icon\n"
    "\n"
    "void main() {\n"
    "    // Start with the normal transformed position\n"
    "    vec4 world_pos = u_model * vec4(a_position, 0.0, 1.0);\n"
    "\n"
    "    // The genie effect works by compressing the bottom of the window\n"
    "    // horizontally toward the target X position. The amount of compression\n"
    "    // increases with both the animation progress and how close the vertex\n"
    "    // is to the bottom edge.\n"
    "    //\n"
    "    // 'a_position.y' goes from 0 (top) to 1 (bottom), so we use it\n"
    "    // directly as the vertical factor — bottom vertices warp more.\n"
    "    float vertical_factor = a_position.y;\n"
    "    float warp = vertical_factor * u_genie_progress;\n"
    "\n"
    "    // Lerp (linear interpolate) the X position toward the target.\n"
    "    // At warp=0 the vertex stays put; at warp=1 it snaps to the target X.\n"
    "    world_pos.x = mix(world_pos.x, u_genie_target.x, warp);\n"
    "\n"
    "    // Also move the Y position toward the target, scaling by progress.\n"
    "    // This creates the \"pouring\" effect where the window's bottom edge\n"
    "    // reaches the dock first, then the rest follows.\n"
    "    world_pos.y = mix(world_pos.y, u_genie_target.y, warp * warp);\n"
    "\n"
    "    gl_Position = u_projection * world_pos;\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";


// ============================================================================
//  SECTION: Uniform location cache
// ============================================================================
//
// Every time we want to set a uniform variable in a shader, we need its
// "location" — an integer handle that identifies where in the program that
// uniform lives. Calling glGetUniformLocation() every frame is wasteful
// because the location never changes after the program is linked.
//
// We cache locations in a simple struct, one per shader program. The cache
// is populated lazily: on first use, we look up the location and store it.
// A location of -1 means "not found" (the uniform doesn't exist in that
// program, which is normal — not every program uses every uniform).

// Maximum number of shader programs we track caches for.
// We have 6 programs (basic, blur_h, blur_v, shadow, solid, genie), but
// leave room for future additions.
#define MAX_CACHED_PROGRAMS 16

typedef struct {
    GLuint program;          // Which program these locations belong to
    GLint  projection;       // u_projection
    GLint  model;            // u_model
    GLint  texture;          // u_texture
    GLint  alpha;            // u_alpha
    GLint  color;            // u_color
    GLint  blur_radius;      // u_blur_radius
    GLint  blur_direction;   // u_blur_direction
    GLint  texture_size;     // u_texture_size
    GLint  genie_progress;   // u_genie_progress
    GLint  genie_target;     // u_genie_target
} UniformCache;

// Our cache array. Entries with program==0 are unused.
static UniformCache uniform_cache[MAX_CACHED_PROGRAMS];
static int uniform_cache_count = 0;

// get_cache — Find or create a uniform cache entry for a shader program.
//
// On first call for a given program, this looks up all uniform locations
// and stores them. On subsequent calls, it just returns the cached entry.
static UniformCache *get_cache(GLuint program)
{
    // Search for an existing cache entry for this program
    for (int i = 0; i < uniform_cache_count; i++) {
        if (uniform_cache[i].program == program) {
            return &uniform_cache[i];
        }
    }

    // Not found — create a new entry (if we have room)
    if (uniform_cache_count >= MAX_CACHED_PROGRAMS) {
        // SECURITY FIX: Don't silently reuse the first entry — that corrupts
        // its uniform locations. Instead, evict the oldest entry (LRU-style)
        // by shifting the array and reusing the last slot.
        fprintf(stderr, "[crystal] Uniform cache full — evicting oldest entry\n");
        memmove(&uniform_cache[0], &uniform_cache[1],
                (MAX_CACHED_PROGRAMS - 1) * sizeof(uniform_cache[0]));
        uniform_cache_count = MAX_CACHED_PROGRAMS - 1;
    }

    // Look up all uniform locations for this program.
    // glGetUniformLocation returns -1 if the uniform doesn't exist in the
    // program — that's fine, we just won't set it.
    UniformCache *c = &uniform_cache[uniform_cache_count++];
    c->program        = program;
    c->projection     = pfn_glGetUniformLocation(program, "u_projection");
    c->model          = pfn_glGetUniformLocation(program, "u_model");
    c->texture        = pfn_glGetUniformLocation(program, "u_texture");
    c->alpha          = pfn_glGetUniformLocation(program, "u_alpha");
    c->color          = pfn_glGetUniformLocation(program, "u_color");
    c->blur_radius    = pfn_glGetUniformLocation(program, "u_blur_radius");
    c->blur_direction = pfn_glGetUniformLocation(program, "u_blur_direction");
    c->texture_size   = pfn_glGetUniformLocation(program, "u_texture_size");
    c->genie_progress = pfn_glGetUniformLocation(program, "u_genie_progress");
    c->genie_target   = pfn_glGetUniformLocation(program, "u_genie_target");

    return c;
}


// ============================================================================
//  SECTION: Quad VBO state
// ============================================================================
//
// A single unit quad (two triangles forming a 0,0 to 1,1 rectangle) stored
// on the GPU. Every quad we draw — windows, shadows, blur passes — reuses
// this same geometry, just with different transforms and shaders.

static GLuint quad_vao = 0;   // Vertex Array Object — stores attribute layout
static GLuint quad_vbo = 0;   // Vertex Buffer Object — stores vertex positions + UVs
static GLuint quad_ebo = 0;   // Element Buffer Object — stores triangle indices


// ============================================================================
//  SECTION: Shader file loading
// ============================================================================

char *shaders_load_file(const char *path)
{
    // Open the file in binary mode to get an accurate byte count
    FILE *f = fopen(path, "rb");
    if (!f) {
        // File not found is expected — we'll use the fallback shader
        return NULL;
    }

    // Get the file size by seeking to the end
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return NULL;
    }

    // Allocate a buffer and read the entire file into it
    // +1 for the null terminator (GLSL source must be null-terminated)
    char *buf = malloc(size + 1);
    if (!buf) {
        fprintf(stderr, "[crystal] Out of memory loading shader: %s\n", path);
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, size, f);
    fclose(f);
    buf[read] = '\0';  // Null-terminate the source string

    return buf;
}


// ============================================================================
//  SECTION: Shader compilation
// ============================================================================

GLuint shaders_compile(GLenum type, const char *source)
{
    if (!pfn_glCreateShader) {
        fprintf(stderr, "[crystal] GL functions not loaded — call shaders_init first\n");
        return 0;
    }

    // Ask the GPU driver to allocate a new shader object.
    // 'type' is either GL_VERTEX_SHADER or GL_FRAGMENT_SHADER.
    GLuint shader = pfn_glCreateShader(type);
    if (!shader) {
        fprintf(stderr, "[crystal] glCreateShader failed\n");
        return 0;
    }

    // Upload the GLSL source code to the GPU.
    // Parameters: shader handle, number of source strings, array of strings,
    // array of lengths (NULL = each string is null-terminated).
    pfn_glShaderSource(shader, 1, &source, NULL);

    // Compile the source into GPU machine code.
    // This is where syntax errors in the GLSL are caught.
    pfn_glCompileShader(shader);

    // Check if compilation succeeded
    GLint success;
    pfn_glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        // Compilation failed — get the error log from the driver.
        // This log contains human-readable error messages with line numbers.
        char log[1024];
        pfn_glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        const char *type_str = (type == GL_VERTEX_SHADER) ? "vertex" : "fragment";
        fprintf(stderr, "[crystal] %s shader compile error:\n%s\n", type_str, log);
        pfn_glDeleteShader(shader);
        return 0;
    }

    return shader;
}


// ============================================================================
//  SECTION: Program linking
// ============================================================================

GLuint shaders_link(GLuint vert, GLuint frag)
{
    if (!vert || !frag) {
        fprintf(stderr, "[crystal] Cannot link program: invalid shader handle(s)\n");
        return 0;
    }

    // Create a program object — this will hold the linked vertex + fragment pair
    GLuint program = pfn_glCreateProgram();
    if (!program) {
        fprintf(stderr, "[crystal] glCreateProgram failed\n");
        return 0;
    }

    // Attach both shaders to the program.
    // The GPU driver needs both a vertex shader (to transform positions) and
    // a fragment shader (to determine pixel colors) for a complete pipeline.
    pfn_glAttachShader(program, vert);
    pfn_glAttachShader(program, frag);

    // Link the program — this connects the vertex shader's outputs (varyings)
    // to the fragment shader's inputs and resolves all uniform references.
    pfn_glLinkProgram(program);

    // Check if linking succeeded
    GLint success;
    pfn_glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[1024];
        pfn_glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "[crystal] Shader link error:\n%s\n", log);
        pfn_glDeleteProgram(program);
        return 0;
    }

    return program;
}


// ============================================================================
//  SECTION: Helper — compile a program from vertex + fragment source strings
// ============================================================================

// compile_program — Compile vertex + fragment source, link into a program.
//
// This is the common pattern: compile vert, compile frag, link, delete the
// individual shader objects (the program keeps its own copy of the code).
//
// Returns the program handle, or 0 on failure.
static GLuint compile_program(const char *vert_src, const char *frag_src,
                              const char *name)
{
    fprintf(stderr, "[crystal] Compiling shader program: %s\n", name);

    GLuint vert = shaders_compile(GL_VERTEX_SHADER, vert_src);
    if (!vert) {
        fprintf(stderr, "[crystal]   -> vertex shader failed for '%s'\n", name);
        return 0;
    }

    GLuint frag = shaders_compile(GL_FRAGMENT_SHADER, frag_src);
    if (!frag) {
        fprintf(stderr, "[crystal]   -> fragment shader failed for '%s'\n", name);
        pfn_glDeleteShader(vert);
        return 0;
    }

    GLuint program = shaders_link(vert, frag);

    // After linking, the individual shader objects are no longer needed.
    // The linked program has its own copy of the compiled code.
    pfn_glDeleteShader(vert);
    pfn_glDeleteShader(frag);

    if (!program) {
        fprintf(stderr, "[crystal]   -> linking failed for '%s'\n", name);
        return 0;
    }

    fprintf(stderr, "[crystal]   -> '%s' compiled successfully (program %u)\n",
            name, program);
    return program;
}


// ============================================================================
//  SECTION: Helper — try loading shader source from file, fall back to embedded
// ============================================================================

// try_load_source — Attempt to load shader source from a file. If the file
// doesn't exist or can't be read, return the embedded fallback string.
//
// If the file source was loaded (malloc'd), sets *needs_free to true so the
// caller knows to free it. If the fallback was returned, *needs_free is false
// (fallback strings are static — must not be freed).
static const char *try_load_source(const char *shader_dir, const char *filename,
                                   const char *fallback, bool *needs_free)
{
    *needs_free = false;

    if (!shader_dir) {
        return fallback;
    }

    // Build the full path: shader_dir + "/" + filename
    // snprintf ensures we don't overflow the buffer
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", shader_dir, filename);

    char *file_src = shaders_load_file(path);
    if (file_src) {
        fprintf(stderr, "[crystal] Loaded shader from file: %s\n", path);
        *needs_free = true;
        return file_src;
    }

    // File not found — use the embedded fallback
    fprintf(stderr, "[crystal] Using embedded fallback for: %s\n", filename);
    return fallback;
}


// ============================================================================
//  SECTION: Initialization
// ============================================================================

bool shaders_init(ShaderPrograms *progs, const char *shader_dir)
{
    // Zero out all program handles — 0 means "not compiled"
    memset(progs, 0, sizeof(ShaderPrograms));

    // Reset the uniform location cache
    uniform_cache_count = 0;
    memset(uniform_cache, 0, sizeof(uniform_cache));

    // Load GL 2.0+ functions from the driver.
    // This MUST happen after a GL context is current (crystal_init does that).
    if (!load_gl_functions()) {
        fprintf(stderr, "[crystal] Cannot initialize shaders: GL function loading failed\n");
        return false;
    }

    // ---- Compile each shader program ----
    // For each program, we try to load source from files first, falling back
    // to the embedded strings if the files aren't available.

    bool vert_free, frag_free;

    // --- basic: textured quad with alpha ---
    // Vertex: basic.vert (transform position)
    // Fragment: basic.frag (sample texture, apply alpha)
    const char *basic_vert = try_load_source(shader_dir, "basic.vert",
                                             BASIC_VERT_SRC, &vert_free);
    const char *basic_frag = try_load_source(shader_dir, "basic.frag",
                                             BASIC_FRAG_SRC, &frag_free);
    progs->basic = compile_program(basic_vert, basic_frag, "basic");
    if (vert_free) free((char *)basic_vert);
    if (frag_free) free((char *)basic_frag);

    // --- blur_h: horizontal Gaussian blur ---
    // Uses the basic vertex shader (same quad transform) but the blur
    // fragment shader with direction set to horizontal.
    const char *blur_vert = try_load_source(shader_dir, "basic.vert",
                                            BASIC_VERT_SRC, &vert_free);
    const char *blur_frag = try_load_source(shader_dir, "blur.frag",
                                            BLUR_FRAG_SRC, &frag_free);
    progs->blur_h = compile_program(blur_vert, blur_frag, "blur_h");
    if (vert_free) free((char *)blur_vert);

    // --- blur_v: vertical Gaussian blur ---
    // Same shaders as blur_h — the direction is set via a uniform at draw time,
    // so both passes use the same compiled program. However, we create two
    // separate program objects so each can cache its own uniform state.
    const char *blur_vert2 = try_load_source(shader_dir, "basic.vert",
                                             BASIC_VERT_SRC, &vert_free);
    progs->blur_v = compile_program(blur_vert2, blur_frag, "blur_v");
    if (vert_free) free((char *)blur_vert2);
    if (frag_free) free((char *)blur_frag);

    // --- shadow: alpha-only shadow rendering ---
    const char *shadow_frag = try_load_source(shader_dir, "shadow.frag",
                                              SHADOW_FRAG_SRC, &frag_free);
    const char *shadow_vert = try_load_source(shader_dir, "basic.vert",
                                              BASIC_VERT_SRC, &vert_free);
    progs->shadow = compile_program(shadow_vert, shadow_frag, "shadow");
    if (vert_free) free((char *)shadow_vert);
    if (frag_free) free((char *)shadow_frag);

    // --- solid: flat color, no texture ---
    // The solid fragment shader is simple enough that we always use the
    // embedded version (no file to load).
    const char *solid_vert = try_load_source(shader_dir, "basic.vert",
                                             BASIC_VERT_SRC, &vert_free);
    progs->solid = compile_program(solid_vert, SOLID_FRAG_SRC, "solid");
    if (vert_free) free((char *)solid_vert);

    // --- genie: mesh distortion for minimize animation ---
    // Uses a custom vertex shader that warps vertices into a funnel shape.
    // The fragment shader is the basic texture sampler.
    const char *genie_vert = try_load_source(shader_dir, "genie.vert",
                                             GENIE_VERT_SRC, &vert_free);
    const char *genie_frag = try_load_source(shader_dir, "basic.frag",
                                             BASIC_FRAG_SRC, &frag_free);
    progs->genie = compile_program(genie_vert, genie_frag, "genie");
    if (vert_free) free((char *)genie_vert);
    if (frag_free) free((char *)genie_frag);

    // The 'basic' program is essential — if it failed, we can't render at all
    if (!progs->basic) {
        fprintf(stderr, "[crystal] FATAL: basic shader program failed to compile\n");
        shaders_shutdown(progs);
        return false;
    }

    fprintf(stderr, "[crystal] Shader initialization complete. Programs: "
            "basic=%u blur_h=%u blur_v=%u shadow=%u solid=%u genie=%u\n",
            progs->basic, progs->blur_h, progs->blur_v,
            progs->shadow, progs->solid, progs->genie);

    return true;
}


// ============================================================================
//  SECTION: Shutdown
// ============================================================================

void shaders_shutdown(ShaderPrograms *progs)
{
    // Helper macro: delete a program if it exists (handle != 0)
    #define DELETE_PROG(p) do {              \
        if (progs->p) {                     \
            pfn_glDeleteProgram(progs->p);  \
            progs->p = 0;                   \
        }                                   \
    } while (0)

    DELETE_PROG(basic);
    DELETE_PROG(blur_h);
    DELETE_PROG(blur_v);
    DELETE_PROG(shadow);
    DELETE_PROG(solid);
    DELETE_PROG(genie);

    #undef DELETE_PROG

    // Clear the uniform cache — all cached locations are now invalid
    uniform_cache_count = 0;
    memset(uniform_cache, 0, sizeof(uniform_cache));

    fprintf(stderr, "[crystal] Shader programs destroyed\n");
}


// ============================================================================
//  SECTION: Program activation
// ============================================================================

void shaders_use(GLuint program)
{
    // Activate the given shader program. All subsequent glDraw* calls will
    // use this program's vertex and fragment shaders.
    pfn_glUseProgram(program);
}

void shaders_use_none(void)
{
    // Passing 0 deactivates all shader programs, reverting to the fixed-function
    // pipeline (glBegin/glEnd, glColor, etc.). This is needed when legacy code
    // that hasn't been ported to shaders needs to draw something.
    pfn_glUseProgram(0);
}


// ============================================================================
//  SECTION: Common uniform setters
// ============================================================================
//
// Each function looks up the cached uniform location for the given program,
// then sends the value to the GPU. If the uniform doesn't exist in the
// program (location == -1), the call is silently skipped.

void shaders_set_projection(GLuint program, float *matrix4x4)
{
    UniformCache *c = get_cache(program);
    if (c->projection >= 0) {
        // glUniformMatrix4fv parameters:
        //   location: which uniform to set
        //   count: number of matrices (1)
        //   transpose: GL_FALSE because our matrix is already column-major
        //              (the format OpenGL expects)
        //   value: pointer to 16 floats
        pfn_glUniformMatrix4fv(c->projection, 1, GL_FALSE, matrix4x4);
    }
}

void shaders_set_texture(GLuint program, int texture_unit)
{
    UniformCache *c = get_cache(program);
    if (c->texture >= 0) {
        // Texture uniforms take an integer — the texture unit number.
        // GL_TEXTURE0 = unit 0, GL_TEXTURE1 = unit 1, etc.
        pfn_glUniform1i(c->texture, texture_unit);
    }
}

void shaders_set_alpha(GLuint program, float alpha)
{
    UniformCache *c = get_cache(program);
    if (c->alpha >= 0) {
        pfn_glUniform1f(c->alpha, alpha);
    }
}

void shaders_set_color(GLuint program, float r, float g, float b, float a)
{
    UniformCache *c = get_cache(program);
    if (c->color >= 0) {
        pfn_glUniform4f(c->color, r, g, b, a);
    }
}


// ============================================================================
//  SECTION: Blur-specific uniform setters
// ============================================================================

void shaders_set_blur_radius(GLuint program, float radius)
{
    UniformCache *c = get_cache(program);
    if (c->blur_radius >= 0) {
        pfn_glUniform1f(c->blur_radius, radius);
    }
}

void shaders_set_blur_direction(GLuint program, float dx, float dy)
{
    UniformCache *c = get_cache(program);
    if (c->blur_direction >= 0) {
        pfn_glUniform2f(c->blur_direction, dx, dy);
    }
}

void shaders_set_texture_size(GLuint program, float width, float height)
{
    UniformCache *c = get_cache(program);
    if (c->texture_size >= 0) {
        pfn_glUniform2f(c->texture_size, width, height);
    }
}


// ============================================================================
//  SECTION: Genie-specific uniform setters
// ============================================================================

void shaders_set_genie_progress(GLuint program, float t)
{
    UniformCache *c = get_cache(program);
    if (c->genie_progress >= 0) {
        pfn_glUniform1f(c->genie_progress, t);
    }
}

void shaders_set_genie_target(GLuint program, float target_x, float target_y)
{
    UniformCache *c = get_cache(program);
    if (c->genie_target >= 0) {
        pfn_glUniform2f(c->genie_target, target_x, target_y);
    }
}


// ============================================================================
//  SECTION: Orthographic projection matrix
// ============================================================================

void shaders_ortho(float *m, float l, float r, float b, float t,
                   float n, float f)
{
    // Build a 4x4 orthographic projection matrix in column-major order.
    //
    // Orthographic projection maps a box-shaped volume (defined by left, right,
    // bottom, top, near, far) directly to clip space (-1 to +1 on all axes).
    // Unlike perspective projection, there's no foreshortening — objects far
    // away appear the same size as objects close up. This is perfect for 2D
    // window compositing where everything is flat.
    //
    // The matrix layout (column-major, as OpenGL expects):
    //
    //   [ 2/(r-l)    0         0        -(r+l)/(r-l) ]
    //   [   0      2/(t-b)     0        -(t+b)/(t-b) ]
    //   [   0        0       -2/(f-n)   -(f+n)/(f-n) ]
    //   [   0        0         0              1       ]
    //
    // Column-major means we fill columns first:
    //   m[0..3] = first column, m[4..7] = second column, etc.

    memset(m, 0, 16 * sizeof(float));

    m[0]  =  2.0f / (r - l);          // Scale X to fit [-1, +1]
    m[5]  =  2.0f / (t - b);          // Scale Y to fit [-1, +1]
    m[10] = -2.0f / (f - n);          // Scale Z to fit [-1, +1] (negated for GL)
    m[12] = -(r + l) / (r - l);       // Translate X center to origin
    m[13] = -(t + b) / (t - b);       // Translate Y center to origin
    m[14] = -(f + n) / (f - n);       // Translate Z center to origin
    m[15] = 1.0f;                     // Homogeneous coordinate (always 1 for affine)
}


// ============================================================================
//  SECTION: Quad VBO setup
// ============================================================================

void shaders_init_quad_vbo(void)
{
    // Define a unit quad: a 1x1 rectangle from (0,0) to (1,1).
    //
    // Each vertex has 4 floats:
    //   - 2 for position (x, y)
    //   - 2 for texture coordinate (s, t)
    //
    // The quad is made of 4 vertices forming 2 triangles:
    //
    //   3 --- 2       Vertex 0: bottom-left  (0,0)  tex (0,0)
    //   |   / |       Vertex 1: bottom-right (1,0)  tex (1,0)
    //   |  /  |       Vertex 2: top-right    (1,1)  tex (1,1)
    //   | /   |       Vertex 3: top-left     (0,1)  tex (0,1)
    //   0 --- 1
    //
    // We use an Element Buffer Object (EBO) with indices to define the two
    // triangles: {0,1,2} and {0,2,3}. This avoids duplicating vertex data.

    float vertices[] = {
        // position    texcoord
        0.0f, 0.0f,   0.0f, 0.0f,   // vertex 0: bottom-left
        1.0f, 0.0f,   1.0f, 0.0f,   // vertex 1: bottom-right
        1.0f, 1.0f,   1.0f, 1.0f,   // vertex 2: top-right
        0.0f, 1.0f,   0.0f, 1.0f,   // vertex 3: top-left
    };

    GLuint indices[] = {
        0, 1, 2,   // First triangle (bottom-left, bottom-right, top-right)
        0, 2, 3,   // Second triangle (bottom-left, top-right, top-left)
    };

    // --- Create VAO ---
    // A Vertex Array Object stores the "layout" of vertex data — which
    // attributes exist, how they're packed, and which VBO they come from.
    // Once set up, we only need to bind the VAO to restore all that state.
    pfn_glGenVertexArrays(1, &quad_vao);
    pfn_glBindVertexArray(quad_vao);

    // --- Create VBO (vertex data) ---
    // A Vertex Buffer Object stores the actual vertex data on the GPU.
    // GL_STATIC_DRAW tells the driver this data won't change often, so it
    // can place it in fast GPU memory.
    pfn_glGenBuffers(1, &quad_vbo);
    pfn_glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    pfn_glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // --- Create EBO (index data) ---
    // The Element Buffer Object stores triangle indices. Instead of listing
    // 6 vertices (3 per triangle x 2 triangles), we list 4 unique vertices
    // and 6 indices pointing into them. This saves memory and bandwidth.
    pfn_glGenBuffers(1, &quad_ebo);
    pfn_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quad_ebo);
    pfn_glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // --- Set up vertex attribute layout ---
    // Tell OpenGL how to interpret the data in our VBO:
    //
    // Attribute 0 (a_position): 2 floats, starting at byte offset 0
    // Attribute 1 (a_texcoord): 2 floats, starting at byte offset 8 (2 floats * 4 bytes)
    //
    // The "stride" (16 bytes = 4 floats * 4 bytes) is the distance between
    // consecutive vertices in the buffer.

    // Attribute 0: position (x, y)
    pfn_glVertexAttribPointer(
        0,                             // attribute index (matches 'layout(location=0)' or
                                       // the order in the shader)
        2,                             // number of components (x, y)
        GL_FLOAT,                      // data type
        GL_FALSE,                      // don't normalize
        4 * sizeof(float),             // stride: bytes between consecutive vertices
        (void *)0                      // offset: position starts at byte 0
    );
    pfn_glEnableVertexAttribArray(0);  // Enable attribute 0

    // Attribute 1: texture coordinate (s, t)
    pfn_glVertexAttribPointer(
        1,                             // attribute index
        2,                             // number of components (s, t)
        GL_FLOAT,                      // data type
        GL_FALSE,                      // don't normalize
        4 * sizeof(float),             // stride: same as above
        (void *)(2 * sizeof(float))    // offset: texcoord starts after 2 position floats
    );
    pfn_glEnableVertexAttribArray(1);  // Enable attribute 1

    // Unbind the VAO (good practice — prevents accidental modification)
    pfn_glBindVertexArray(0);

    fprintf(stderr, "[crystal] Quad VBO initialized (VAO=%u VBO=%u EBO=%u)\n",
            quad_vao, quad_vbo, quad_ebo);
}

void shaders_shutdown_quad_vbo(void)
{
    // Delete GPU resources in reverse order of creation
    if (quad_ebo) {
        pfn_glDeleteBuffers(1, &quad_ebo);
        quad_ebo = 0;
    }
    if (quad_vbo) {
        pfn_glDeleteBuffers(1, &quad_vbo);
        quad_vbo = 0;
    }
    if (quad_vao) {
        pfn_glDeleteVertexArrays(1, &quad_vao);
        quad_vao = 0;
    }

    fprintf(stderr, "[crystal] Quad VBO destroyed\n");
}


// ============================================================================
//  SECTION: Quad drawing
// ============================================================================

// build_model_matrix — Create a 4x4 model matrix that scales and positions
// a unit quad (0..1) to the given screen rectangle (x, y, w, h).
//
// The model matrix combines translation and scaling:
//   Scale by (w, h, 1) to set the size,
//   then translate to (x, y, 0) to set the position.
//
// Column-major layout (as OpenGL expects):
//   [ w  0  0  x ]
//   [ 0  h  0  y ]
//   [ 0  0  1  0 ]
//   [ 0  0  0  1 ]
static void build_model_matrix(float *m, float x, float y, float w, float h)
{
    memset(m, 0, 16 * sizeof(float));
    m[0]  = w;     // Scale X
    m[5]  = h;     // Scale Y
    m[10] = 1.0f;  // Scale Z (unchanged)
    m[12] = x;     // Translate X
    m[13] = y;     // Translate Y
    m[15] = 1.0f;  // Homogeneous coordinate
}

void shaders_draw_quad(float x, float y, float w, float h)
{
    // Build and upload the model matrix for this quad's position/size.
    // The model matrix transforms the unit quad (0..1) to screen coordinates.
    float model[16];
    build_model_matrix(model, x, y, w, h);

    // Set the u_model uniform in the currently active shader program.
    // We need the current program ID to look up the uniform location.
    GLint current_program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
    if (current_program > 0) {
        UniformCache *c = get_cache((GLuint)current_program);
        if (c->model >= 0) {
            pfn_glUniformMatrix4fv(c->model, 1, GL_FALSE, model);
        }
    }

    // Bind the quad VAO (this restores all vertex attribute state we set up
    // in shaders_init_quad_vbo) and draw the two triangles.
    pfn_glBindVertexArray(quad_vao);
    glDrawElements(
        GL_TRIANGLES,        // primitive type: triangles
        6,                   // number of indices (2 triangles x 3 vertices each)
        GL_UNSIGNED_INT,     // index data type
        (void *)0            // offset into the EBO (start from the beginning)
    );
    pfn_glBindVertexArray(0);  // Unbind VAO
}

void shaders_draw_quad_tc(float x, float y, float w, float h,
                          float s0, float t0, float s1, float t1)
{
    // Similar to shaders_draw_quad, but with custom texture coordinates.
    // Instead of sampling the full texture (0..1), we sample a sub-rectangle
    // defined by (s0,t0) to (s1,t1). This is useful for effects that only
    // need part of a texture (e.g., blur passes on a specific screen region).

    // Build the model matrix for positioning
    float model[16];
    build_model_matrix(model, x, y, w, h);

    GLint current_program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
    if (current_program > 0) {
        UniformCache *c = get_cache((GLuint)current_program);
        if (c->model >= 0) {
            pfn_glUniformMatrix4fv(c->model, 1, GL_FALSE, model);
        }
    }

    // Update the VBO with custom texture coordinates.
    // We temporarily modify the vertex data to use the caller's UV range.
    float vertices[] = {
        // position    texcoord
        0.0f, 0.0f,   s0, t0,   // bottom-left
        1.0f, 0.0f,   s1, t0,   // bottom-right
        1.0f, 1.0f,   s1, t1,   // top-right
        0.0f, 1.0f,   s0, t1,   // top-left
    };

    // Upload the modified vertex data to the VBO.
    // GL_DYNAMIC_DRAW would be more appropriate if we did this every frame,
    // but since the VBO was created with GL_STATIC_DRAW, we use glBufferSubData
    // to update it in place without reallocating.
    pfn_glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    pfn_glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);

    // Draw with the updated texture coordinates
    pfn_glBindVertexArray(quad_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (void *)0);
    pfn_glBindVertexArray(0);

    // Restore the default texture coordinates (0..1) so future calls to
    // shaders_draw_quad() work correctly.
    float default_verts[] = {
        0.0f, 0.0f,   0.0f, 0.0f,
        1.0f, 0.0f,   1.0f, 0.0f,
        1.0f, 1.0f,   1.0f, 1.0f,
        0.0f, 1.0f,   0.0f, 1.0f,
    };
    pfn_glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(default_verts), default_verts);
    pfn_glBindBuffer(GL_ARRAY_BUFFER, 0);
}


// ============================================================================
//  SECTION: FBO management
// ============================================================================

GLuint shaders_create_fbo(int width, int height, GLuint *out_texture)
{
    // An FBO (Framebuffer Object) is an off-screen render target. Instead of
    // drawing to the screen, we can draw into an FBO and capture the result
    // as a texture. This is essential for multi-pass effects like blur:
    //
    //   Pass 1: Draw scene -> FBO (horizontal blur)
    //   Pass 2: Draw FBO texture -> another FBO (vertical blur)
    //   Pass 3: Draw final FBO texture -> screen
    //
    // Each FBO needs an attached texture to store the rendered pixels.

    GLuint fbo, tex;

    // Create the FBO
    pfn_glGenFramebuffers(1, &fbo);
    pfn_glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    // Create a texture to attach as the FBO's color buffer.
    // This texture will receive all the pixels we render into the FBO.
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    // Allocate the texture storage (RGBA, 8 bits per channel).
    // Passing NULL for the data pointer means "allocate memory but don't
    // fill it with anything" — we'll render into it instead.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    // Set texture filtering to LINEAR (bilinear interpolation).
    // This makes the texture look smooth when sampled at non-integer positions,
    // which happens during blur passes. Without this, you'd see blocky artifacts.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Clamp texture coordinates to the edge to prevent wrapping artifacts.
    // Without this, sampling near the edge of the texture during blur could
    // wrap around and sample from the opposite edge.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Attach the texture to the FBO's color attachment point.
    // GL_COLOR_ATTACHMENT0 means "this texture receives color output from
    // fragment shaders."
    pfn_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex, 0);

    // Verify the FBO is complete (all attachments are valid and compatible).
    // An incomplete FBO will silently produce no output, which is very hard
    // to debug — so we check explicitly.
    GLenum status = pfn_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[crystal] FBO creation failed (status=0x%X, size=%dx%d)\n",
                status, width, height);
        pfn_glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        pfn_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return 0;
    }

    // Unbind the FBO — we don't want to accidentally render into it
    pfn_glBindFramebuffer(GL_FRAMEBUFFER, 0);

    *out_texture = tex;

    fprintf(stderr, "[crystal] FBO created (fbo=%u tex=%u size=%dx%d)\n",
            fbo, tex, width, height);
    return fbo;
}

void shaders_destroy_fbo(GLuint fbo, GLuint texture)
{
    if (texture) {
        glDeleteTextures(1, &texture);
    }
    if (fbo) {
        pfn_glDeleteFramebuffers(1, &fbo);
    }
}

void shaders_bind_fbo(GLuint fbo)
{
    // Redirect all subsequent rendering into this FBO instead of the screen.
    // After this call, glClear, glDraw*, etc. all affect the FBO's attached
    // texture, not the visible framebuffer.
    pfn_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

void shaders_unbind_fbo(void)
{
    // Bind framebuffer 0, which is the default framebuffer — the screen.
    // After this call, rendering goes to the visible display again.
    pfn_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
