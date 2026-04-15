// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// ============================================================================
//  MoonRock Shaders — GLSL shader compilation and management for MoonRock
// ============================================================================
//
// MoonRock's rendering pipeline currently uses fixed-function OpenGL calls
// (glBegin/glEnd, glTexCoord2f, etc.). Those work fine for basic textured
// quads, but they cannot do anything fancy — no blur, no color manipulation,
// no mesh warping. To add visual effects like blur-behind, genie animations,
// and HDR tone mapping, we need programmable shaders.
//
// What are shaders?
//   Shaders are small programs that run on the GPU. There are two main types:
//     - Vertex shaders:   transform the positions of vertices (corners of
//                         triangles). We use these for things like the genie
//                         minimize effect, which warps a flat quad into a
//                         curved shape.
//     - Fragment shaders: determine the color of each pixel. We use these for
//                         blur effects, alpha blending, color tinting, etc.
//
//   Together, a vertex shader + fragment shader form a "shader program" that
//   the GPU executes for every triangle we draw.
//
// What this module provides:
//   1. Compiling GLSL source code into GPU shader objects.
//   2. Linking vertex + fragment shaders into complete programs.
//   3. A set of pre-built shader programs for MoonRock's effects.
//   4. Helper functions to set uniform variables (parameters sent to shaders).
//   5. A VBO-based quad renderer to replace glBegin/glEnd.
//   6. FBO (framebuffer object) management for off-screen rendering passes.
//
// Why load from files AND have fallbacks?
//   Loading shaders from .vert/.frag files makes iteration fast — edit a file,
//   restart the compositor, see the change. But we also embed fallback shader
//   source strings in the C code so MoonRock still works even if the shader
//   files are missing (e.g., first install, missing data directory).
//
// ============================================================================

#ifndef MR_SHADERS_H
#define MR_SHADERS_H

#include <GL/gl.h>
#include <stdbool.h>

// ============================================================================
//  Shader program collection
// ============================================================================
//
// Each field is a compiled+linked GLSL program (an opaque GLuint handle).
// A value of 0 means that program failed to compile or hasn't been created.
//
// The compositor picks which program to use before drawing each element:
//   - Window contents:  use 'basic' (just sample a texture and apply alpha)
//   - Blur passes:      use 'blur_h' then 'blur_v' (two-pass Gaussian blur)
//   - Drop shadows:     use 'shadow' (alpha-only rendering)
//   - Solid rectangles: use 'solid' (flat color, no texture needed)
//   - Genie animation:  use 'genie' (vertex shader warps the mesh)

typedef struct {
    GLuint basic;       // Basic textured quad (vertex transform + texture sample)
    GLuint blur_h;      // Horizontal Gaussian blur pass
    GLuint blur_v;      // Vertical Gaussian blur pass
    GLuint shadow;      // Shadow rendering (alpha-only texture)
    GLuint solid;       // Solid color (no texture, just color + alpha)
    GLuint genie;       // Genie mesh distortion (vertex shader warps mesh)
    GLuint desaturate;  // Desaturation effect (grayscale blend)
    GLuint tint;        // Color tint overlay
} ShaderPrograms;


// ============================================================================
//  Lifecycle
// ============================================================================

// Initialize all shader programs.
//
// This compiles every vertex/fragment shader pair MoonRock needs, links them
// into programs, and stores the resulting handles in 'progs'.
//
// shader_dir: path to the directory containing .vert and .frag files.
//             If NULL or the files don't exist, embedded fallback shaders
//             are used instead — so the compositor always works.
//
// Returns true if at least the 'basic' program compiled successfully.
// Individual programs that fail to compile are set to 0 (the compositor
// can check and skip effects that aren't available).
bool shaders_init(ShaderPrograms *progs, const char *shader_dir);

// Destroy all shader programs and free GPU resources.
//
// After this call, all handles in 'progs' are invalid. Call this during
// MoonRock's shutdown sequence.
void shaders_shutdown(ShaderPrograms *progs);


// ============================================================================
//  Program activation
// ============================================================================

// Activate a shader program for subsequent draw calls.
//
// This is a thin wrapper around glUseProgram(). All geometry drawn after this
// call will be processed by the given program's vertex and fragment shaders.
//
// program: the GLuint handle of the shader program to activate (e.g.,
//          progs.basic, progs.blur_h, etc.)
void shaders_use(GLuint program);

// Deactivate all shader programs, reverting to fixed-function OpenGL.
//
// Call this when you need to use legacy glBegin/glEnd rendering (e.g., for
// compatibility code that hasn't been ported to shaders yet).
void shaders_use_none(void);


// ============================================================================
//  Common uniform setters
// ============================================================================
//
// Uniforms are variables that you send from the CPU to the GPU. They stay
// constant for all vertices/pixels in a single draw call. For example, the
// projection matrix is a uniform — it's the same for every vertex.
//
// Each setter looks up the uniform's location in the given program by name,
// then sends the value. Uniform locations are cached internally so we don't
// call glGetUniformLocation every frame (that would be slow).

// Set the 4x4 projection matrix (typically an orthographic projection).
// This transforms vertex positions from screen coordinates (pixels) into
// the -1..+1 range that OpenGL expects (called "clip space").
void shaders_set_projection(GLuint program, float *matrix4x4);

// Set which texture unit to sample from (usually 0 for GL_TEXTURE0).
// The fragment shader uses this to know which bound texture to read pixels from.
void shaders_set_texture(GLuint program, int texture_unit);

// Set the global alpha (opacity) multiplier.
// 1.0 = fully opaque, 0.0 = fully transparent. Used for fade animations
// and inactive window dimming.
void shaders_set_alpha(GLuint program, float alpha);

// Set a solid color (for the 'solid' shader or tinting).
// (r, g, b) are color channels 0.0..1.0, 'a' is alpha.
void shaders_set_color(GLuint program, float r, float g, float b, float a);


// ============================================================================
//  Blur-specific uniform setters
// ============================================================================
//
// Gaussian blur is done in two passes: horizontal then vertical. Each pass
// reads from a texture and writes to an FBO. The blur shader needs to know
// the blur radius (how many pixels to sample), the direction (horizontal or
// vertical), and the texture dimensions (to convert pixel offsets to UV coords).

// Set the blur radius in pixels (how far the blur spreads).
void shaders_set_blur_radius(GLuint program, float radius);

// Set the blur sampling direction as a 2D vector.
// Horizontal pass: (1.0, 0.0) — sample left/right neighbors.
// Vertical pass:   (0.0, 1.0) — sample up/down neighbors.
void shaders_set_blur_direction(GLuint program, float dx, float dy);

// Set the texture dimensions so the shader can convert pixel offsets to
// normalized texture coordinates (0.0..1.0 range).
void shaders_set_texture_size(GLuint program, float width, float height);


// ============================================================================
//  Genie-specific uniform setters
// ============================================================================
//
// The genie effect is macOS's classic minimize animation where a window
// appears to pour into the dock like liquid. We achieve this by distorting
// the window's geometry in the vertex shader — warping a flat grid of
// triangles into a funnel shape.

// Set the animation progress (0.0 = normal window, 1.0 = fully minimized).
// The vertex shader interpolates between the flat quad and the funnel shape
// based on this value.
void shaders_set_genie_progress(GLuint program, float t);

// Set the target position where the window minimizes to (the dock icon).
// The vertex shader funnels the bottom of the window toward this point.
void shaders_set_genie_target(GLuint program, float target_x, float target_y);


// ============================================================================
//  Desaturate / Tint uniform setters
// ============================================================================
//
// These uniforms control the desaturate and tint fragment shaders. Both use
// a "u_amount" uniform that blends between the original image and the effect
// (0.0 = no effect, 1.0 = full effect). The tint shader also takes a color.

// Set the effect amount for the desaturate or tint shader.
// 0.0 = no effect (original image), 1.0 = full effect.
void shaders_set_amount(GLuint program, float amount);

// Set the tint color (RGBA) for the tint shader.
// r, g, b: color channels 0.0..1.0, a: alpha (usually 1.0).
void shaders_set_tint_color(GLuint program, float r, float g, float b, float a);


// ============================================================================
//  Shader compilation utilities
// ============================================================================

// Compile a single shader (vertex or fragment) from GLSL source code.
//
// type:   GL_VERTEX_SHADER or GL_FRAGMENT_SHADER
// source: null-terminated GLSL source string
//
// Returns a non-zero shader handle on success, or 0 on compile error
// (error details are printed to stderr).
GLuint shaders_compile(GLenum type, const char *source);

// Link a vertex shader and a fragment shader into a complete program.
//
// vert: handle from shaders_compile() with GL_VERTEX_SHADER
// frag: handle from shaders_compile() with GL_FRAGMENT_SHADER
//
// Returns a non-zero program handle on success, or 0 on link error.
// The individual shader handles can be deleted after linking.
GLuint shaders_link(GLuint vert, GLuint frag);

// Load a shader source file into a malloc'd string.
//
// path: full path to a .vert or .frag file
//
// Returns a null-terminated string (caller must free), or NULL if the
// file could not be read.
char *shaders_load_file(const char *path);

// Build a standard orthographic projection matrix.
//
// An orthographic projection maps a rectangular volume directly to clip space
// without any perspective distortion — things far away appear the same size
// as things close up. This is exactly what we want for 2D compositing where
// everything is at the same depth.
//
// matrix: pointer to a float[16] array (column-major, as OpenGL expects)
// left, right, bottom, top: the edges of the visible area in screen pixels
// near, far: the depth range (typically -1.0 to 1.0 for 2D)
void shaders_ortho(float *matrix, float left, float right,
                   float bottom, float top, float near, float far);


// ============================================================================
//  Quad rendering (VBO-based replacement for glBegin/glEnd)
// ============================================================================
//
// Fixed-function glBegin/glEnd is deprecated in modern OpenGL and is slow
// because each vertex requires a separate function call from CPU to GPU.
// VBOs (Vertex Buffer Objects) upload all vertex data to the GPU in one shot,
// which is much faster.
//
// We use a single unit quad (0,0 to 1,1) stored in a VBO, then scale and
// position it using a uniform matrix. This means we only need one VBO for
// all quad rendering — windows, shadows, blur passes, everything.

// Create the quad VBO and VAO. Call once during MoonRock initialization.
void shaders_init_quad_vbo(void);

// Draw a textured quad at the given screen position and size.
// Uses the currently active shader program.
// x, y: top-left corner in screen coordinates
// w, h: width and height in pixels
void shaders_draw_quad(float x, float y, float w, float h);

// Draw a textured quad with custom texture coordinates.
// Used when sampling a sub-rectangle of a texture (e.g., for blur passes
// that only process a portion of the screen).
// s0, t0: texture coordinate of the top-left corner
// s1, t1: texture coordinate of the bottom-right corner
void shaders_draw_quad_tc(float x, float y, float w, float h,
                          float s0, float t0, float s1, float t1);

// Destroy the quad VBO and VAO. Call during MoonRock shutdown.
void shaders_shutdown_quad_vbo(void);


// ============================================================================
//  FBO management (off-screen render targets)
// ============================================================================
//
// An FBO (Framebuffer Object) is an off-screen surface that you can render
// into, just like the screen. We use FBOs for multi-pass effects like blur:
//   Pass 1: Render the scene into FBO-A (horizontal blur)
//   Pass 2: Render FBO-A's texture into FBO-B (vertical blur)
//   Pass 3: Render FBO-B's texture to the screen (final result)
//
// Each FBO has an attached texture that receives the rendered pixels. After
// rendering into the FBO, we can bind that texture and use it as input for
// the next pass.

// Create an FBO with an attached RGBA texture of the given dimensions.
//
// width, height: size of the off-screen surface in pixels
// out_texture:   receives the handle of the texture attached to the FBO
//
// Returns the FBO handle (non-zero on success, 0 on failure).
GLuint shaders_create_fbo(int width, int height, GLuint *out_texture);

// Destroy an FBO and its attached texture, freeing GPU memory.
void shaders_destroy_fbo(GLuint fbo, GLuint texture);

// Bind an FBO as the current render target (all subsequent draws go into it).
void shaders_bind_fbo(GLuint fbo);

// Unbind the current FBO, restoring rendering to the default framebuffer
// (the screen).
void shaders_unbind_fbo(void);

#endif // MR_SHADERS_H
