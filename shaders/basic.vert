// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

#version 130

// ============================================================================
// basic.vert — Vertex Shader for Textured Quads
// ============================================================================
//
// Purpose:
//   Transforms a unit quad (0,0)-(1,1) into screen-space coordinates and
//   passes texture coordinates through to the fragment shader.
//
// How it fits into Crystal Compositor:
//   Every visible element (windows, dock shelf, panels, overlays) is drawn
//   as a textured quad. This vertex shader is shared by all of them. The
//   fragment shader is swapped depending on what effect is needed (basic
//   texture sampling, blur, shadow, etc.).
//
// Uniforms:
//   u_projection  — Orthographic projection matrix that maps screen-space
//                    pixel coordinates (e.g. 0..1920, 0..1080) into OpenGL
//                    clip-space coordinates (-1..1).
//   u_transform   — A vec4 encoding (x, y, width, height) in screen pixels.
//                    This positions and sizes the quad without needing a
//                    full model matrix.
//
// Vertex Attributes:
//   a_position    — The vertex position in unit-quad space (0 to 1 on each axis).
//                    A typical quad VBO has 4 vertices: (0,0), (1,0), (1,1), (0,1).
//   a_texcoord    — The texture coordinate for this vertex (also 0 to 1).
//                    Usually identical to a_position for a simple quad.
//
// Output:
//   v_texcoord    — Interpolated texture coordinate, passed to the fragment shader.
//   gl_Position   — Final clip-space position of the vertex.
// ============================================================================

// --- Uniforms (set by the CPU before each draw call) ---

// Orthographic projection matrix: converts pixel coords to clip coords.
// Typically set once per frame via glm::ortho(0, screen_width, screen_height, 0).
// The Y-axis is flipped (top = 0) to match typical 2D/window-manager conventions.
uniform mat4 u_projection;

// Quad transform: (x, y, width, height) in screen pixels.
// This tells us where on screen to draw this quad and how large it should be.
// Using a vec4 instead of a full mat4 keeps things simple for axis-aligned quads.
uniform vec4 u_transform;

// --- Vertex inputs (one per vertex, supplied by the VBO) ---

// Position of this vertex within the unit quad (values are 0.0 or 1.0).
// The unit quad is a 1x1 square at the origin that we scale and translate
// using u_transform to place it on screen.
in vec2 a_position;

// Texture coordinate for this vertex (usually matches a_position).
// These get interpolated across the quad's surface by the GPU, so every
// fragment gets the correct UV to sample the texture.
in vec2 a_texcoord;

// --- Output to fragment shader ---

// The interpolated texture coordinate. The GPU automatically blends this
// between vertices, so the fragment shader receives a smooth UV value
// for each pixel.
out vec2 v_texcoord;

void main()
{
    // Scale the unit quad to the desired pixel size (u_transform.zw = width, height)
    // and translate it to the desired screen position (u_transform.xy = x, y).
    //
    // Example: if u_transform = (100, 200, 800, 600), a vertex at (1, 1)
    // becomes (100 + 1*800, 200 + 1*600) = (900, 800) in screen pixels.
    vec2 pos = a_position * u_transform.zw + u_transform.xy;

    // Apply the orthographic projection to convert from screen pixels
    // to clip-space coordinates that OpenGL expects (-1 to 1 range).
    gl_Position = u_projection * vec4(pos, 0.0, 1.0);

    // Pass the texture coordinate through unchanged — the GPU will
    // interpolate it across the quad for us.
    v_texcoord = a_texcoord;
}
