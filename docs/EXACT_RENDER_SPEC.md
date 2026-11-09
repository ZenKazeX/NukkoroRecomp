# Exact rendering specification (vendor-independent), v0: particles

Status: v0, 2026-10-30. Scope: the particle pass (90 % of the 5K heavy frames, docs/GPU_FILL.md section 22).
Directive: docs/OPTIMIZATION_GOAL.md "UNIVERSAL EXACT ARCHITECTURE". Evidence from the RTX 4080 (sections 19-23)
informs the choices but nothing here depends on it.

Principle: every value that reaches the framebuffer is defined by this document as integer arithmetic (or IEEE-754
basic operations on the CPU, which are correctly rounded on every conforming CPU: +, -, *, / and sqrt, no fused
contraction). No GPU floating-point operation, interpolator, rasterizer, texture sampler or blend unit decides a
bit. A GPU implementation is correct when it produces, sample for sample, what the portable C++ reference
(tools/exact/exact_particles.h) produces.

## 1. Inputs per particle triangle (CPU setup, the "triangle record")

The game's particle (position, size, rotation, colour, uv0 / uv1, batch axes R / U) becomes two triangles (corners
(0, 1, 2) and (0, 2, 3) as in shaders/particles.hlsl). For each triangle the CPU produces:

- Snapped vertices: X_k, Y_k = roundTiesToEven(screen_k * 256), integers in 1/256 pixel (D3D / Vulkan 8-bit subpixel
  precision). screen_k is computed in IEEE double from the clip position (x / w * 0.5 + 0.5) * width, (0.5 - y / w *
  0.5) * height. The clip position's own computation (rotation, matrix) is part of v1 (section 7, open).
- Orientation: area2 = (X1 - X0)(Y2 - Y0) - (X2 - X0)(Y1 - Y0) (int64). area2 = 0: the triangle covers nothing.
  area2 < 0: vertices 1 and 2 are swapped (no culling for particles), so area2 > 0 below.
- Attribute planes: u and v in texel units with 32 fraction bits, as functions of the HALF-pixel lattice
  (x2 = 2 px + 1, y2 = 2 py + 1 is the pixel centre):
      u(x2, y2) = (Au * x2 + Bu * y2 + Cu) >> 32 ... see 3.
  Au, Bu, Cu are the exact rational plane through (X_k / 256, Y_k / 256, u_k * texture_width) scaled by 2^32 (per
  half-pixel for A, B), rounded to the nearest integer with ties to even (exact 128-bit arithmetic).
  u_k is the corner's texture coordinate (IEEE float, as the game wrote it) converted exactly to the rational value it
  represents.
- Colour: RGBA8 (the particle's colour, or the batch colour), converted to unorm16 by c16 = c8 * 257.
- Texture: an RGBA8 level (v0: level 0 only), address modes wrap / clamp / mirror per axis.

Particles are camera-facing: their four corners have the same w by intent, so the interpolation is affine in screen
space (no perspective division). The old renderer interpolated perspective-correctly with corner w values that
differ only by float rounding; the difference is documented as a semantic choice (section 8).

## 2. Coverage (4x)

Sample positions, in 1/16 pixel from the pixel centre, the D3D / Vulkan standard 4-sample pattern:
s0 (-2, -6), s1 (6, -2), s2 (-6, 2), s3 (2, 6). Sample point in 1/256 pixel: Px = 256 px + 128 + 16 ox,
Py = 256 py + 128 + 16 oy.

Edge functions (int64), for the counter-clockwise (area2 > 0) triangle in y-down screen space:
    E0 = (X2 - X1)(Py - Y1) - (Y2 - Y1)(Px - X1)   (opposite vertex 0)
    E1 = (X0 - X2)(Py - Y2) - (Y0 - Y2)(Px - X2)
    E2 = (X1 - X0)(Py - Y0) - (Y1 - Y0)(Px - X0)
A sample is covered when every Ei > 0, or Ei = 0 and edge i is a top or left edge. Edge i runs from vertex a to
vertex b ((1, 2), (2, 0), (0, 1)); with dx = Xb - Xa, dy = Yb - Ya it is top-left when dy < 0, or dy = 0 and dx > 0.
(The sign convention follows from E's orientation; the reference checks it against hardware coverage as evidence,
tools/exact/exact_particles_test.cpp --coverage-check.)
Vertex coordinates are limited to |X|, |Y| < 2^30 (guard band 2^22 pixels): every product fits in int64.

## 3. Attribute evaluation (once per pixel, at the pixel centre)

    U = (Au * x2 + Bu * y2 + Cu)            int64 (|U| < 2^63 by the guard band and texture size limits)
    u8 = U >> 24                            floor: texel units with 8 fraction bits
(same for v). Evaluated at the pixel centre even when the centre lies outside the triangle (pixel-frequency
shading, as the game's shaders: no centroid).

## 4. Texture filtering (bilinear, level 0 in v0)

    ut = u8 - 128 (texel centres), i0 = ut >> 8 (floor), fx = ut & 255; same for v (j0, fy)
    address: wrap i mod W (non-negative), clamp to [0, W-1], mirror as D3D (period 2W, reflected)
    w00 = (256 - fx)(256 - fy), w10 = fx (256 - fy), w01 = (256 - fx) fy, w11 = fx fy        (sum 65536)
    t16 = (w00 t00 + w10 t10 + w01 t01 + w11 t11) * 257 + 32768) >> 16     per channel, t in 0..255
Result 0..65535 per channel (a filtered texel of 255 everywhere is exactly 65535).

## 5. Fragment colour

    S_ch = (c16_ch * t16_ch + 32767) / 65535     (integer division), channels R, G, B, A
Untextured: t16 = 65535. Discard when S_A <= cutoff16 (cutoff16 = the batch cutoff as unorm16, roundTiesToEven;
negative cutoff: never) or, with kSkipClear, when S_A = 0. Fog (v1, section 7).

## 6. Blend and storage (SrcAlpha / InvSrcAlpha into R16G16B16A16 unorm, 4 samples)

For every covered sample, in triangle order:
    D_ch = (S_ch * S_A + D_ch * (65535 - S_A) + 32767) / 65535      ch = R, G, B;   D_A unchanged
The same one-rounding form the RTX 4080's blend unit uses on unorm16 (GPU_FILL.md section 20 D), applied to the
already-integer source. Additive (One / One): D_ch = min(S_ch + D_ch, 65535). No coalescing, no reordering: order
is the triangle order of the draw and of the draws.

## 7. Not yet specified (v1)

- The clip position: rotation (sin / cos) and the view-projection product, defined on the CPU (IEEE double basic
  operations + a specified sin / cos), and near-plane handling (particles crossing w <= near).
- Mip selection (per triangle: affine planes have constant derivatives; LOD from Au, Bu, Av, Bv with an integer
  log2) and trilinear blending; the authored RenderWare filter modes (nearest / linear / mip*) - see 8.
- Depth test against the scene depth (the scene depth comes from the mesh pass: cross-vendor exactness of a whole
  frame needs the meshes in this specification too).
- Fog (linear in eye depth; eye depth constant per particle for billboards).
- The resolve (mean of 4 unorm16 samples, rounding) and everything after it.

## 7b. Implementation limits and proven identities (v0)

- Texel coordinates: |U >> 32| < 2^30 texels (texel indices fit 32-bit signed); the setup must reject or clip
  triangles beyond it (v0: the reference and the kernel assume it).
- floor(x / 65535) may be computed as (x + 1 + (x >> 16)) >> 16 in 32-bit unsigned arithmetic for every x the
  specification divides (x <= 65535^2 + 32767): tools/exact/div65535_proof.cpp, exhaustive.
- Wrap / mirror on power-of-two sizes may use masks: i & (n - 1) equals the non-negative i mod n for every two's
  complement i.
- A tile may skip per-sample coverage when the four corners of its sample box are strictly inside every edge, and
  skip the triangle when all four are strictly outside one edge (linearity + convexity; strict inequalities avoid the
  tie rule).

## 8. Semantic decisions to confirm

- Anisotropic filtering: the current renderer draws every non-point RenderWare filter as 16x anisotropic (an
  enhancement over the console); anisotropic filtering is vendor-defined. v0 uses bilinear (camera-facing particles
  are isotropic); a specified anisotropic filter, or the authored modes, is a user decision.
- Perspective vs affine for billboards (section 1).
- The canonical reference is this specification, not historical captures: the old renderer's blended output is
  scheduling-dependent (GPU_FILL.md section 19).

## 9. v1: the game's particle pass (tools/exact/exact_render.h is normative where this summary is short)

9.1 Portable sine / cosine: sincos_spec() - Cody-Waite reduction by pi/2 in three parts, Taylor polynomials (sine to
r^17, cosine to r^18) by Horner, IEEE double basic operations only; |x| < 2^20.
9.2 Particle setup (CPU, IEEE double): W = size.x / 2 (c R - s U), H = size.y / 2 (s R + c U); corners p - W - H,
p + W - H, p + W + H, p - W + H with texture corners (u1, v0), (u0, v0), (u0, v1), (u1, v1); triangles (0, 1, 2),
(0, 2, 3). clip = M (x, y, z, 1) row-major with the float matrix entries widened to double. The particle's centre gives
z / w (culled unless 0 <= z / w <= 1 and w > 0), depth = fp32(z / w) (round to nearest even), fog weight
f16 = roundTiesToEven(65536 clamp((w - start) / max(end - start, 1e-6), 0, 1)). Corners projected individually and
snapped (section 1); a corner with w <= 0 culls the particle.
9.3 Level of detail: n = max(Au^2 + Av^2, Bu^2 + Bv^2) (exact 128-bit), lambda8 = log2_q7(n) - 7936 (= floor(256 lambda),
lambda = log2 of texels per pixel); lambda8 <= 0 or one level: level 0; else l0 = lambda8 >> 8, frac = lambda8 & 255;
l0 >= levels - 1: the last level alone; frac > 0: blend l0 and l0 + 1. log2_q7: bit length, then 7 squarings of the
62-fraction-bit mantissa, each truncated.
9.4 Trilinear: level l coordinates U >> (24 + l); bilinear per level as section 4 (wrap); t = (t0 (256 - frac) + t1
frac + 128) >> 8 per channel.
9.5 Fragment: S = div65535(c8 * 257 * t16 + 32767) per channel; discard when S_A <= cutoff16 (cutoff16 =
roundTiesToEven(cutoff * 65535) for cutoff > 0) or (skip clear, no depth write) S_A = 0; fog on R, G, B:
S = (S (65536 - f16) + F16 f16 + 32768) >> 16, F16 = fog8 * 257.
9.6 Depth and blend per covered sample in draw order: depth test passes when depth >= stored (integer compare of the
non-negative float bits = float order); blend SrcA/InvSrcA div65535(S A + D (65535 - A) + 32767), SrcA/One
min(div65535(S A + 32767) + D, 65535), One/One min(S + D, 65535); depth write (alpha-tested draws) stores depth for
the covered samples of fragments not discarded.
9.7 Semantic choices to confirm: trilinear isotropic (the renderer currently maps these RenderWare modes to 16x
anisotropic), affine billboards, unorm16 alpha test (the old shader compared float alpha).
