// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

#version 130

// ============================================================================
// blur.frag — Separable Gaussian Blur Fragment Shader
// ============================================================================
//
// Purpose:
//   Applies a one-dimensional Gaussian blur along a single axis. To get a
//   full 2D blur, this shader is run twice — once horizontally and once
//   vertically — using a ping-pong between two framebuffer textures.
//
// Why separable?
//   A naive 2D blur with radius R would need (2R+1)^2 texture samples per
//   pixel. A separable blur splits this into two 1D passes, each needing
//   only (2R+1) samples. For R=16 that's 33 samples per pass instead of
//   1,089 samples in a single pass. Massive performance win.
//
// How it fits into Crystal Compositor:
//   Crystal uses blur for two things:
//   1. Background blur behind translucent panels (like the dock shelf)
//   2. Pre-generating shadow textures (blur a solid shape to create soft edges)
//
//   The compositor renders the scene (or a portion of it) to an offscreen
//   framebuffer, then runs this shader in two passes to blur it, then
//   composites the blurred result back into the scene.
//
// Uniforms:
//   u_texture   — The texture to blur (an offscreen framebuffer attachment).
//   u_direction — The blur direction in texel coordinates:
//                  (1.0/texture_width, 0.0) for horizontal pass
//                  (0.0, 1.0/texture_height) for vertical pass
//   u_radius    — The blur radius in texels. Controls how far out we sample.
//                  Larger values = more spread = softer blur.
//                  Capped at 16 for performance (33 taps max per pass).
//
// Input:
//   v_texcoord — Interpolated texture coordinate from the vertex shader.
//
// Output:
//   frag_color — The blurred color for this pixel.
//
// Math background:
//   A Gaussian (bell curve) weight function is: exp(-x^2 / (2 * sigma^2))
//   where sigma controls the width of the bell curve. We set sigma = radius/3
//   so that samples at the edge of the kernel (x = radius) have near-zero
//   weight, giving a smooth falloff without wasting samples on negligible
//   contributions.
// ============================================================================

// --- Uniforms ---

// The source texture to blur. This is always an offscreen framebuffer texture,
// never a client window directly (we copy first, then blur).
uniform sampler2D u_texture;

// Blur direction expressed as a texel step vector.
// For a horizontal pass on a 1920px-wide texture: (1.0/1920.0, 0.0)
// For a vertical pass on a 1080px-tall texture: (0.0, 1.0/1080.0)
// This lets us use the same shader for both directions without branching.
uniform vec2 u_direction;

// Blur radius in texels. Determines how many neighboring pixels contribute
// to the result. Higher values produce a softer, more spread-out blur.
// The actual number of texture samples is (2 * ceil(radius) + 1), capped
// at 33 samples (radius = 16) to stay GPU-friendly.
uniform float u_radius;

// --- Input from vertex shader ---
in vec2 v_texcoord;

// --- Output ---
out vec4 frag_color;

void main()
{
    // Accumulator for the weighted color sum
    vec4 result = vec4(0.0);

    // Running total of all weights — we divide by this at the end to
    // normalize the result (ensures brightness is preserved)
    float total_weight = 0.0;

    // Convert the floating-point radius to an integer tap count.
    // ceil() ensures we don't under-sample (e.g. radius 4.5 uses 5 taps each side).
    int r = int(ceil(u_radius));

    // Cap the radius at 16 to prevent excessive texture fetches.
    // 16 texels each side + center = 33 taps per pass, which runs well
    // even on integrated GPUs like the Legion Go's AMD RDNA 3 iGPU.
    if (r > 16) r = 16;

    // Compute sigma for the Gaussian weight function.
    // sigma = radius/3 means the Gaussian drops to ~1% at the edges,
    // giving a natural-looking falloff without harsh cutoffs.
    float sigma = u_radius / 3.0;

    // Guard against division by zero if radius is extremely small
    if (sigma < 0.001) sigma = 0.001;

    // Pre-compute the denominator of the Gaussian exponent: 2 * sigma^2
    // Doing this once outside the loop avoids redundant multiplication.
    float inv_two_sigma_sq = 1.0 / (2.0 * sigma * sigma);

    // Sample the texture at each integer offset from -r to +r.
    // Each sample is weighted by a Gaussian function centered at 0.
    for (int i = -r; i <= r; i++)
    {
        float x = float(i);

        // Gaussian weight: the classic bell curve formula.
        // Samples near the center (i=0) get high weight, samples near
        // the edges (i=±r) get very low weight.
        float w = exp(-(x * x) * inv_two_sigma_sq);

        // Offset the texture coordinate along the blur direction.
        // u_direction is already in texel-space (1/width or 1/height),
        // so multiplying by x gives us the correct UV offset.
        vec2 offset = u_direction * x;

        // Sample the texture and accumulate the weighted result
        result += texture(u_texture, v_texcoord + offset) * w;

        // Track the total weight for normalization
        total_weight += w;
    }

    // Normalize: divide by the sum of all weights so the overall brightness
    // of the image is preserved. Without this, the blur would darken or
    // brighten the image depending on the kernel size.
    frag_color = result / total_weight;
}
