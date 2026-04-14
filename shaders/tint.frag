// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
//  Color tint fragment shader
// ============================================================================
//
// Blends a texture's color with a solid tint color. The u_amount uniform
// controls how much tinting is applied:
//   0.0 = no tint (original image)
//   1.0 = full tint (original color completely replaced by tint)
//
// This is useful for things like dimming inactive windows with a dark tint,
// or applying a warm/cool color cast to the entire scene.

#version 120

varying vec2 v_texcoord;

uniform sampler2D u_texture;
uniform float u_alpha;
uniform vec4 u_tint_color;  // RGBA tint color
uniform float u_amount;     // 0.0 = no tint, 1.0 = full tint

void main() {
    // Sample the original color at this pixel
    vec4 color = texture2D(u_texture, v_texcoord);

    // Blend the original RGB toward the tint color.
    // mix(a, b, t) returns a*(1-t) + b*t, so at amount=0 the tint has no
    // effect, and at amount=1 the tint fully replaces the original color.
    color.rgb = mix(color.rgb, u_tint_color.rgb, u_amount);

    // Apply global alpha for fade/opacity control
    color.a *= u_alpha;

    gl_FragColor = color;
}
