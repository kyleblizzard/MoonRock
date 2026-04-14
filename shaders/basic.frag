// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

#version 130

// ============================================================================
// basic.frag — Fragment Shader for Textured Quads
// ============================================================================
//
// Purpose:
//   Samples a texture and applies a global alpha multiplier. This is the
//   default fragment shader used for drawing window surfaces, the dock shelf,
//   panel backgrounds, and any other textured quad.
//
// How it fits into MoonRock Compositor:
//   MoonRock uses premultiplied alpha blending everywhere. That means the
//   RGB values in textures are already multiplied by their alpha channel.
//   The OpenGL blend function is set to:
//     glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)
//   This avoids dark halos around transparent edges and is the standard
//   approach for compositors (Wayland, macOS Quartz, etc.).
//
// Uniforms:
//   u_texture — The 2D texture to sample (a window's pixel buffer, a UI
//               element, etc.). Bound to a texture unit before drawing.
//   u_alpha   — A global opacity multiplier (0.0 = invisible, 1.0 = fully
//               opaque). Used for fade-in/out animations, inactive window
//               dimming, or transparency effects.
//
// Input:
//   v_texcoord — Interpolated texture coordinate from the vertex shader.
//
// Output:
//   frag_color — The final RGBA color for this pixel.
// ============================================================================

// --- Uniforms ---

// The texture to sample. This could be a Wayland client's surface buffer,
// a pre-rendered UI element, or an offscreen framebuffer texture.
uniform sampler2D u_texture;

// Global alpha multiplier. Because we use premultiplied alpha, we multiply
// ALL channels (RGB and A) by this value — not just the alpha channel.
// Multiplying only alpha would cause colors to appear too bright relative
// to their transparency.
uniform float u_alpha;

// --- Input from vertex shader ---

// Texture coordinate interpolated across the quad surface.
// (0,0) is typically the top-left corner, (1,1) is the bottom-right.
in vec2 v_texcoord;

// --- Output ---

// The final color written to the framebuffer for this pixel.
out vec4 frag_color;

void main()
{
    // Sample the texture at the interpolated UV coordinate.
    // texture() is the modern GLSL function (texture2D is deprecated
    // but still works in GLSL 1.30 — we use texture() for correctness).
    vec4 color = texture(u_texture, v_texcoord);

    // Apply the global alpha multiplier to ALL channels.
    //
    // Why multiply RGB too? Because we're using premultiplied alpha.
    // In premultiplied format, a 50% transparent red pixel is stored as
    // (0.5, 0.0, 0.0, 0.5) instead of (1.0, 0.0, 0.0, 0.5).
    // If we want to make the whole quad 50% transparent on top of that,
    // we need (0.25, 0.0, 0.0, 0.25) — hence multiplying everything.
    frag_color = color * u_alpha;
}
