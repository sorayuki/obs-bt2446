# Method C: folding the colour-space matrices

Method C of BT.2446-1 (§6.1) is defined as a chain of colour transforms wrapped
around a single one-dimensional tone curve. Every transform in that chain is a
3x3 matrix, and the curve between them never mixes channels, so the whole chain
can be collapsed into one matrix per side. The shader therefore performs two
matrix multiplies instead of six, with no change to the mathematical result.

This document records why that is exact, how much it saves, and what was
verified.

## 1. The pipeline before folding

All values are linear light in cd/m² unless stated otherwise.

```
 v709 ──A──▶ v2020 ──C(α)──▶ ──B──▶ XYZ ──curve──▶ XYZ' ──B⁻¹──▶ v2020' ──C⁻¹──▶ ──A⁻¹──▶ v709'
        (709→2020)   (crosstalk)  (2020→XYZ)          (tone map)      (2020←XYZ)      (inverse crosstalk) (2020→709)
```

- `A` — Rec.709 to Rec.2020 primaries (BT.2087)
- `C(α)` — the crosstalk matrix `(1 − 3α)I + αJ`, with `J` the all-ones matrix.
  Its diagonal is `1 − 2α` and its off-diagonal entries are `α`, which is
  exactly what the shader's `crosstalk()` helper computed.
- `B` — Rec.2020 RGB to CIE XYZ
- `curve` — the Method C luminance transfer function, applied to `Y` only

## 2. The curve is a pure rescale of the XYZ triple

The curve is fed the chromaticity of the pixel and returns a new luminance:

```
xy   = (X, Y) / (X + Y + Z)
Y'   = curve(Y)
XYZ' = ( xy.x / xy.y · Y' ,  Y' ,  (1 − xy.x − xy.y) / xy.y · Y' )
```

Substituting `xy.x / xy.y = X / Y` and `(1 − xy.x − xy.y) / xy.y = Z / Y`:

```
XYZ' = (Y' / Y) · (X, Y, Z) = s · XYZ ,   s = Y' / Y
```

So the curve multiplies the whole triple by one scalar. It never rotates
chromaticity, never mixes channels. That is the property the fold depends on.

The optional highlight desaturation (§6.1.8) sits between the two halves and
does modify `X` and `Z` independently, but it modifies them *before* the curve
recomputes `xy` and `Y'`, and everything downstream of it is the same rescale.
So the argument below holds with or without it — the desaturation stays exactly
where it was, inside the XYZ domain.

## 3. Why the fold is exact

Two facts are needed:

1. **Matrix multiplication is associative.** `B · C · A` is a single linear map,
   and computing it in one step gives the same result as applying `A`, then `C`,
   then `B` — this is pure algebra, not an approximation.
2. **A scalar commutes with a matrix.** `M · (s·x) = s·(M·x)`. Because the curve
   only contributes the scalar `s` (§2), it can be applied before or after any
   linear map without changing the outcome.

Chaining them:

```
input  side:  B · C · A · v709              →  M_in  · v709
output side:  A⁻¹ · C⁻¹ · B⁻¹ · XYZ'        →  M_out · XYZ'
```

and the complete chain

```
A⁻¹ C⁻¹ B⁻¹ · (s · (B C A · v))  =  s · (A⁻¹ C⁻¹ B⁻¹ B C A) · v  =  s · v
```

becomes

```
M_out · ( s · (M_in · v) )
```

which is the same expression with the six matrices replaced by two. Nothing is
linearised, tabulated, or dropped.

### 3.1 The crosstalk inverse

`C = (1 − 3α)I + αJ`. Split any vector into `v = m·𝟙 + d`, where `m` is the mean
and `d` has zero sum. Then `C·𝟙 = 𝟙` (each row sums to `1 − 2α + α + α = 1`)
and `C·d = (1 − 3α)·d` (because `Σd = 0`). Therefore

```
C⁻¹ = (I − J/3) / (1 − 3α) + J/3
```

which is the matrix form of the shader's previous `(v − mean) / (1 − 3α) + mean`,
and `C⁻¹C = I` exactly. The `max(1 − 3α, 0.01)` floor is kept, and is applied to
the diagonal factor in the same place as before.

### 3.2 The early-out branch

The old code returned the Rec.2020 copy of the input when the pixel had no
usable luminance:

```hlsl
return rgb2020 * c_k1 * hdr_peak_nits / 100.0;   // Rec.2020 nits
```

That value then went through `rec2020_to_rec709()` in the pixel shader. Since
`A⁻¹A = I`, the round trip cancels and the result is simply the Rec.709 input
scaled by `c_k1`:

```hlsl
return rgb709_nits * c_k1 / 100.0;               // Rec.709 nits
```

### 3.3 The `hdr_peak_nits` round trip

`map_method_c()` used to receive the input already divided by `hdr_peak_nits`
and multiply it straight back:

```hlsl
normalized2020 = linear2020 / hdr_peak_nits;     // in PSDefault
rgb_x = crosstalk(rgb2020 * hdr_peak_nits, ...); // in map_method_c
```

Since the matrices are linear, both operations cancel. Method C now takes the
Rec.709 nits directly and the two extra ALU ops per pixel are gone. Methods A
and B still need the normalised value and are unchanged.

## 4. How much it actually saves

The obvious estimate — "six matrix applications become two, so two thirds of the
matrix work disappears" — is wrong, because it counts a matrix multiply as nine
multiplies plus six adds. D3D compiles `dot(a, vector)` to a **single `dp3`**, so
one 3x3 matrix on a three-component vector costs **three instructions**, not
fifteen. Measured on the compiled shader (`fxc` 10.0.26100, `ps_5_0`, Method C
only, σ branch left in but not executed):

| | before | after |
|---|---|---|
| instructions in the shader body | 96 | 88 |
| `fxc` "instruction slots used" | 128 | **105** (−18%) |
| matrix stages | 6 | 2 |
| constant-buffer registers reserved | `CB0[9]` | `CB0[15]` |

The extra six constants (each `float3` occupies a full 16-byte register, so 96
bytes in total) replace the single `c_alpha` float. That is a one-off cost paid
per draw call, not per pixel.

The saving is real but modest: roughly one fifth of the Method C pixel shader,
concentrated in the two ends (the crosstalk and its inverse, which were 7 and 6
scalar instructions respectively, are now absorbed into the folded matrices).

For scale, on a 1920x1200 30 fps source this shader runs 69 Mpx/s. Eighteen
percent of a shader that is a few percent of the GPU is not going to show up in
a utilisation graph; the reason to keep the change is that it is free, exact,
and removes two redundant conversion helpers.


## 5. Numeric verification

Both versions were reimplemented and compared, once in double precision and once
with every intermediate rounded to binary32 (which is pessimistic — real GPU
code uses FFMA, so it rounds once per multiply-add instead of twice):

| α | max difference, double | max difference, binary32 | fp32 error vs exact, old / new |
|---|---|---|---|
| 0.00 | 5e-13 nit | 0.01 / 255 code | 0.015 / 0.008 |
| 0.10 (default) | 5e-13 nit | 0.04 / 255 code | 0.055 / 0.098 |
| 0.20 | 5e-13 nit | 0.03 / 255 code | 0.008 / 0.026 |
| 0.33 (slider max) | 5e-13 nit | 0.79 / 255 code | 0.35 / 0.58 |

In double precision the two forms agree to 5e-13 over 200 000 random colours,
i.e. they are the same function. In binary32 the folded form is slightly noisier
at large α because `M_out` carries the `1/(1 − 3α)` factor, so its rows reach
magnitudes of ~4.5 at α = 0.1 and ~450 at α = 0.33 and the subtraction that
recovers a small result from large operands loses a few bits. At the default
α = 0.1 this is 0.04 of an 8-bit code value; only the extreme slider position on
strongly saturated colours approaches one code value, and both the old and the
new form are already in that range there (0.35 vs 0.58).

The sample colours above include fully saturated ones (single channel at zero),
which are the worst case for the chromaticity division; ordinary content sits
well below these numbers.

## 6. What did not change

- The highlight desaturation branch (§6.1.8), its threshold and its formula.
  It still operates on XYZ, between the two folded matrices.
- `max()`/`saturate()` guards: `sum ≤ 1e-5`, `max(xy.y, 1e-5)`,
  `max(1 − 3α, 0.01)`, `max(value − k3, 1e-4)`.
- The early-out condition itself.
- Methods A and B, and the shared Rec.2020 ↔ Rec.709 helpers they use.

## 7. Re-deriving the matrices

`src/bt2446-filter.c` holds the four source matrices (they are the constants
that previously sat inline in the effect) and multiplies them in
`build_method_c_matrices()`, which runs once per settings change, never per
frame. The result is uploaded as six `float3` uniforms (`c_matrix_in_0..2`,
`c_matrix_out_0..2`), each one row of the folded matrix.

For α = 0.1 the folded matrices are:

```
M_in  =  0.3564518392   0.3773753345   0.2166728377
         0.2201365530   0.6343028545   0.1455606222
         0.0911742002   0.2290497273   0.7688760757

M_out =  4.4775757790  -2.3703436852  -0.8130500317
        -1.5371816158   2.5059895515  -0.0412215479
        -0.0731054619  -0.4654445648   1.4093078375
```

Note that `M_in` maps straight from Rec.709 to XYZ, skipping Rec.2020 entirely:
it is `B · C · A`, so the crosstalk still happens in the Rec.2020 primaries, as
the recommendation requires — the intermediate representation is merely not
materialised.
