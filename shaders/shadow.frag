// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

#version 130

// ============================================================================
// shadow.frag — Fragment Shader for Window Shadows
// ============================================================================
//
// Purpose:
//   Renders a pre-blurred shadow texture behind a window. The shadow texture
//   is an alpha-only image where brighter pixels = more shadow opacity.
//   We sample the alpha and output black with that alpha level.
//
// How it fits into MoonRock Compositor:
//   Window shadows are generated once (on window map or resize) by blurring
//   a white rectangle through the blur shader + FBO pipeline. The result is
//   a texture where the alpha channel encodes the shadow falloff — fully
//   opaque near the window edge, fading to transparent at the shadow's extent.
//
//   This shader draws that pre-blurred texture as a black overlay, giving
//   windows the characteristic Snow Leopard "floating" look. Active windows
//   get a stronger shadow (higher u_alpha), inactive windows get softer.
//
// Uniforms:
//   u_texture — The pre-blurred shadow texture (alpha channel is shadow data)
//   u_alpha   — Peak shadow opacity. Active windows: ~0.45, inactive: ~0.22
//
// Input:
//   v_texcoord — Interpolated texture coordinate from the vertex shader
//
// Output:
//   frag_color — Black with the shadow alpha (premultiplied: RGB=0, A=shadow)
// ============================================================================

uniform sampler2D u_texture;
uniform float u_alpha;

in vec2 v_texcoord;
out vec4 frag_color;

void main()
{
    // Sample the shadow texture. We only care about the alpha channel —
    // it encodes the blur falloff (how dark the shadow is at this pixel).
    float shadow_alpha = texture(u_texture, v_texcoord).a;

    // Output premultiplied black with the shadow alpha.
    // Since we're using premultiplied blending (GL_ONE, GL_ONE_MINUS_SRC_ALPHA),
    // a premultiplied black pixel is just (0, 0, 0, alpha).
    // The u_alpha uniform scales the overall shadow intensity —
    // higher for focused windows, lower for unfocused.
    frag_color = vec4(0.0, 0.0, 0.0, shadow_alpha * u_alpha);
}
