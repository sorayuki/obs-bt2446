# Method B: folding LMS to Rec.709 and returning the result directly

Method B of BT.2446-1 (§5.2) tone maps in Rec.2020 and then hard-clips to the
BT.709 volume in ICtCp. Its gamut search only needs to know whether a candidate
triple fits inside BT.709, and the pixel shader immediately converted the final
Rec.2020 result back to Rec.709 as well. Both paths were evaluating a
Rec.2020-to-Rec.709 matrix that can be multiplied into the preceding
LMS-to-Rec.2020 matrix once.

This document records why that fold is exact, what changes at the gamut
boundary, and how the replacement was checked.

## 1. The paths before the change

Each ICtCp probe during the six-step binary search was decoded as:

```
ICtCp -> LMS' -> PQ decode -> LMS -> Rec.2020 -> Rec.709 -> volume test
                                      M2020       P
```

`M2020` is the constant LMS-to-Rec.2020 matrix already in the shader and `P` is
Rec.2020 to Rec.709 (BT.2087). The final probe was decoded the same way, then
`map_method_b()` converted Rec.2020 nits to SDR, and `PSDefault()` applied `P`
once more before writing to the target.

The intermediate Rec.2020 RGB triple was never used by the search: the volume
test consumed only Rec.709. The tone mapping itself still needs Rec.2020, so
only the search/output representation changes.

## 2. Why the matrix can be folded

For a column vector `v`:

```
P · (M2020 · v) = (P · M2020) · v
```

This is associativity of matrix multiplication, not an approximation. The
shader now uses the single matrix `M709 = P · M2020`:

```
    6.1733627370  -5.3203805696   0.1470178219
   -1.3243115043   2.5602264189  -0.2359149247
   -0.0118169027  -0.2646720292   1.2764889331
```

The same matrix replaces the LMS-to-Rec.2020 multiplication for every search
probe and for the final decode. The first in-volume test still needs one
Rec.2020-to-Rec.709 conversion because the input arrives in Rec.2020, but the
six probes no longer perform two matrix stages each.

## 3. What changes mathematically, and what does not

- Luminance mapping, HLG shaping, the knee, and the ICtCp representation are
  unchanged and remain defined in Rec.2020, as §5.2 requires.
- The binary search still searches the same ICtCp chroma interval. Only the
  decoded candidate is represented in Rec.709 instead of Rec.2020 followed by a
  conversion.
- The early in-volume result is now returned in Rec.709 rather than Rec.2020.
  It is mathematically the same colour; `PSDefault()` no longer applies `P` to
  Method B's result.
- The final result of `gamut_map_ictcp()` is now explicitly Rec.709, because its
  boundary and its output are both the BT.709 volume. Comments and variable
  names call this out to avoid implying that it still returns Rec.2020.

## 4. Instruction effect

On the decoded path, one matrix application is three `dp3` instructions. The
change removes one such application from each of the six probes and one from
the final output path, i.e. up to 21 `dp3` instructions per out-of-gamut pixel
before compiler-level constant folding. Pixels already inside BT.709 skip the
search and save the final output conversion only.

Measured with `fxc` 10.0.26100 on a Method B-only `ps_5_0` pixel shader, the
whole body fell from 481 to 468 instruction slots (about 2.7%). The shader as a
whole used 21 `dp3` instructions before and 3 after; in the new form the
compiler folded the large folded-matrix coefficients into `mad` operations, so
the slot count does not drop by the full 18 `dp3` that the source suggests.
That is still a real saving, but it is not the dominant cost of Method B.

## 5. Numeric verification

The folded matrix was calculated in double precision from the exact constants
already present in the shader, then rounded to ten decimal places for HLSL.
Comparing the old two-stage conversion against the folded matrix over 100,000
random ICtCp probes gave:

| metric | result |
|---|---|
| maximum absolute difference | `3.6e-6` cd/m² |
| maximum relative difference | `1.3e-6` |

The worst case occurs after PQ decode of extreme, near-black or very bright
probes where the matrix has large cancellation, and remains far below one
8-bit code value at normal SDR levels. The difference comes from rounding the
folded constants; the algebraic form itself is identical.

## 6. What did not change

- The six binary-search iterations and their ICtCp hue-preserving construction.
- `pq_encode()`, `pq_decode_channel()`, `rgb2020_to_lms()`, and the ICtCp
  transform.
- HLG shaping, the luminance knee, and the SDR saturation guard.
- Method A and Method C output paths.
