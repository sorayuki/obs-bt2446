# OBS ITU-R BT.2446 HDR-to-SDR Filter

An OBS Studio video filter that converts linear HDR video to SDR using the three HDR-to-SDR methods in ITU-R Report BT.2446-1. The filter runs synchronously in OBS's video render path and outputs SDR sRGB.

## Tone-mapping methods

- **Method A — perceptual matching:** ITU-R BT.2446-1 §4.1, Tables 2–3. Uses the report's perceptual-domain three-segment knee and chroma correction. HDR and SDR peak luminance are configurable; the report reference values are 1,000 and 100 cd/m².
- **Method B — robust live conversion:** ITU-R BT.2446-1 §5.2.1–§5.2.4. For HLG sources, the HLG signal is re-interpreted at `Lw = 291 cd/m²` with gamma 1.03 as the report specifies; this is enabled by default. The input's HLG nominal peak `Lw` is configurable (default 1,000 cd/m²). Disabling the re-interpretation is a non-standard extension for linear non-HLG sources. Tone mapping keeps a 1:1 display-light range below the knee and compresses highlights with a logarithmic shoulder (unity slope at the knee); defaults are 55 cd/m² knee and strength 16, which places HDR reference white near 86% SDR. Out-of-volume colours are desaturated to the BT.709 volume in ICtCp, avoiding the hue shifts the report attributes to Yu'v' and L\*a\*b\*.
- **Method C — adjustable skin-tone mapping:** ITU-R BT.2446-1 §6.1.2–§6.1.8. Applies pre/post crosstalk matrices, converts through BT.2020 linear RGB and CIE Yxy, and joins a linear section to a logarithmic highlight curve. The gain, knee, HDR/SDR reference-white relationship and optional highlight desaturation are configurable. The default parameters follow the report's example: 50% HLG skin tone to 70% SDR, 80% SDR knee, 75% HLG reference white and 96% SDR reference-white signal. The SDR signal percentage is converted through the SDR EOTF before solving the linear-light curve.

The method is selected in the filter properties. Parameters relevant to the selected method are shown in the same properties panel.

## Processing and color representation

The current implementation evaluates the selected tone-mapping method directly in OBS's synchronous video pixel shader; it does not use a lookup texture. The input contract is linear scRGB with Rec.709 primaries (`GS_CS_709_SCRGB`), where OBS defines `1.0 = 80 cd/m²`; `GS_CS_709_EXTENDED` uses OBS's configured SDR white scale. Rec.709↔BT.2020 transforms are internal working-space steps for the report equations only. The filter does not clamp RGB input/output to `[0, 1]`; the linear SDR output is returned in scRGB units and uses the high-precision `GS_CS_SRGB_16F` SDR canvas. Method B produces SDR display light in linear scRGB, so the report's §5.2.5 inverse BT.1886 signal encoding is not applied; that encoding belongs to the downstream SDR output path. Method B performs its specified BT.709 colour-volume reduction.

## Input requirements

The filter expects linear Rec.709/scRGB input in `GS_CS_709_SCRGB`; OBS defines `1.0` as 80 cd/m². The `GS_CS_709_EXTENDED` EDR path uses OBS's configured SDR white level. Values are consumed as linear light; the filter does not infer or decode PQ/HLG/game-specific transfer functions. Sources in other color spaces are passed through unchanged. Method A/C HDR peak defaults to 1,000 cd/m².

## Build

Use the OBS Plugin Template CMake workflow. The plugin requires OBS Studio/libobs development files discoverable by CMake.

## Template provenance

The project started from the Git-tracked files in `D:\dev\obs-dev\obs-plugintemplate` at commit `3e7d7ac3b5342cd7d9b88890b9c70b472d1520fc`. See `TEMPLATE-SOURCE.txt`.
