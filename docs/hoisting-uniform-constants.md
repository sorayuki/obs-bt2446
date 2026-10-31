# Hoisting per-draw constants out of the pixel shader

The BT.2446 equations are normally written in terms of the configured HDR peak,
SDR peak, HLG input white level, knee shape and reference white. Those values do
not vary from pixel to pixel, but the shader used to recompute their powers,
logarithms, divisions and Lab peak for every sample.

They are now calculated once in `build_derived_parameters()` whenever filter
settings change. The shader receives only the resulting constants, while the
BT.2446 equations and their clamps remain in place.

This document lists every hoisted value, the identity used, and the numerical
consequences.

## 1. Method A: the perceptual curve's endpoints

BT.2446-1 §4.1, Table 2 defines

```
rho_hdr = 1 + 32 . (hdr_peak / 10000)^(1/2.4)
rho_sdr = 1 + 32 . (sdr_peak / 10000)^(1/2.4)
```

The shader evaluated both per pixel, then used `log(rho_hdr)` as a divisor and
`rho_sdr - 1` as another divisor:

```
y_perceptual = log(1 + (rho_hdr - 1) . y) / log(rho_hdr)
y_out        = (rho_sdr^y_perceptual - 1) / (rho_sdr - 1)
```

Only `y` is pixel data. The CPU now supplies:

| uniform | value |
|---|---|
| `a_rho_hdr` | `rho_hdr` |
| `a_inv_log_rho_hdr` | `1 / log(rho_hdr)` |
| `a_rho_sdr` | `rho_sdr` |
| `a_inv_rho_sdr_minus_one` | `1 / (rho_sdr - 1)` |

The shader changes a division into a multiply and removes one `pow()` and one
`log()` per pixel. The curve itself, including the `0.7399` / `0.9909` segment
boundaries and coefficients, is untouched.

## 2. Method B: HLG decoding at Lw = 291 cd/m²

§5.2.1 computes a system gamma from the configured HLG input white `Lw`:

```
gamma = 1.2 + 0.42 . log10(Lw / 1000)
y_scene = (y / Lw)^(1/gamma)
ratio   = (291 / Lw) . y_scene^(1.03 - gamma)
```

Everything except `y` is a setting. `build_derived_parameters()` evaluates
`gamma` once with the natural-log form already used in the shader
(`log(x) . 0.4342944819`) and uploads:

| uniform | value | replaces |
|---|---|---|
| `b_inv_lw_nits` | `1 / Lw` | `y / Lw` |
| `b_hlg_inv_gamma` | `1 / gamma` | exponent |
| `b_hlg_exponent` | `1.03 - gamma` | second exponent |
| `b_hlg_ratio` | `291 / Lw` | constant factor |

The two content-dependent `pow()` calls remain. The `max(y_scene, 1e-5)` guard
and the `y <= 0` early-out also remain.

The luminance knee in §5.2.3 uses `strength / slope` and `slope / strength`.
Both are settings, so they become `b_strength_over_slope` and
`b_slope_over_strength`. This removes two divisions from the knee expression
without changing its shape.

## 3. Method C: reference white and peak lightness

§6.1.8 builds the D65 white for the configured HDR reference white:

```
white = (ref . 192.93/203, ref, ref . 221.05/203)
```

and the peak lightness used to normalise the highlight correction:

```
peak_lightness = max(116 . f(hdr_peak / ref) - 16, 100.0001)
```

with `f()` the CIE L*a*b* cube-root helper. Both depend only on settings. They
are uploaded as `c_white_x_nits`, `c_white_y_nits`, `c_white_z_nits` and
`c_peak_lightness`. The pixel-varying `f(X/white.x)`, `f(Y/white.y)` and
`f(Z/white.z)` still run in the shader, as does the correction itself.

The CPU evaluates `f()` in double precision and casts only the four final
values to float, so the per-pixel path no longer repeats a cube root for the
peak.

## 4. Reciprocal peaks

`max(sdr_peak, 1e-4)` was used as a divisor in the BT.709 volume test (up to
seven times per out-of-gamut pixel) and again after Method B's tone curve.
`max(hdr_peak, 1e-4)` normalised Method A's input. These become
`inv_sdr_peak_nits` and `inv_hdr_peak_nits`.

This is the standard `x / c -> x . (1/c)` transformation. The clamp is applied
when the reciprocal is built, so invalid zero peaks cannot produce an
infinity in the shader.

## 5. Why this is allowed

All hoisted expressions are pure functions of uniforms that are constant for
the whole draw:

- peaks and reference white come from filter settings;
- HLG input white and the knee shape come from filter settings;
- no expression depends on `uv`, sampled colour, or a render-target property.

`build_derived_parameters()` runs from the filter's update callback, i.e. when
settings change, not once per frame. The cost is a handful of double-precision
`pow()`/`log()` calls on the CPU, outside the render thread's per-frame work.

No lookup table or approximation is introduced. Every replacement is an
algebraic identity:

```
log(a.x) / log(a)       = log(a.x) . (1/log(a))
(a^y - 1) / (a - 1)     = (a^y - 1) . (1/(a-1))
(y / Lw)^e              = (y . (1/Lw))^e
strength/slope . log(.) = constant . log(.)
x / peak                = x . (1/peak)
```

## 6. Measured effect

Compiled with `fxc` 10.0.26100 as separate `ps_5_0` entry points, with the
method branch removed so each number reflects only that method:

| method | before | after | saved |
|---|---:|---:|---:|
| A | 93 slots | 78 slots | 15 (16%) |
| B | 468 slots | 458 slots | 10 (2%) |
| C | 105 slots | 88 slots | 17 (16%) |

The `dp3` counts are unchanged; the saving is in scalar `mul`, `div`, `log` and
`pow` instructions. Method B gains least because its two content-dependent HLG
`pow()` calls and the six ICtCp probes dominate its cost.

The uniform block grows because the derived values are uploaded alongside the
settings. That is a per-draw constant-buffer cost, not a per-pixel one.

## 7. Numeric consequences

The CPU builds the constants in double precision; the old shader evaluated them
in binary32. The uploaded values are still binary32, so the only differences
are the usual rounding of a slightly more accurate intermediate, plus the
replacement of a division by a multiply with a rounded reciprocal.

The algebraic forms are identical. For the shipped ranges (HDR peak 100-10000
cd/m², SDR peak 40-300 cd/m², HLG Lw 100-10000 cd/m²), a binary32 simulation of
the old per-pixel evaluation against the new hoisted form measured:

| output | maximum difference |
|---|---:|
| Method A normalised luminance | `6.0e-7` (about `0.00015` of an 8-bit code) |
| Method B HLG ratio | `8.9e-7` relative |
| Method C peak lightness | `1.4e-7` relative |

These are below one 8-bit code value by three orders of magnitude and do not
alter which side of a knee boundary a normal pixel falls on. As with the other
folds, the reason to keep this change is that it removes repeated transcendental
work without changing the result.

## 8. What did not change

- The BT.2446 equations, segment boundaries, coefficients and early-outs.
- Every per-pixel value: `y`, ICtCp coefficients, Lab correction, knee
  position and gamut search.
- The method selection and all property ranges/defaults.
- The shader's white-paper section references. Comments identify each hoisted
  value so the original equation can still be matched to the code.
