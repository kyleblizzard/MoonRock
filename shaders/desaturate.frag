// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
//  Desaturation fragment shader
// ============================================================================
//
// Blends a texture's color toward grayscale. The u_amount uniform controls
// how much desaturation is applied:
//   0.0 = full color (no change)
//   1.0 = fully grayscale
//
// Luminance is computed using ITU-R BT.709 weights, which match how the
// human eye perceives brightness (green contributes most, blue least).

#version 120

varying vec2 v_texcoord;

uniform sampler2D u_texture;
uniform float u_alpha;
uniform float u_amount;  // 0.0 = full color, 1.0 = full grayscale

void main() {
    // Sample the original color at this pixel
    vec4 color = texture2D(u_texture, v_texcoord);

    // Compute perceived brightness using BT.709 luminance weights.
    // The human eye is most sensitive to green, less to red, least to blue.
    float luma = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));

    // Blend between the original color and the grayscale version.
    // mix(a, b, t) returns a*(1-t) + b*t, so at amount=0 we get the
    // original color, and at amount=1 we get pure grayscale.
    color.rgb = mix(color.rgb, vec3(luma), u_amount);

    // Apply global alpha for fade/opacity control
    color.a *= u_alpha;

    gl_FragColor = color;
}
