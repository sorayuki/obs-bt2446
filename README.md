# OBS ITU-R BT.2446 HDR-to-SDR Filter

An OBS Studio video filter that converts linear HDR video to SDR using the three HDR-to-SDR methods in ITU-R Report BT.2446-1. The filter runs synchronously in OBS's video render path and outputs SDR sRGB.

## Tone-mapping methods

- **Method A — perceptual matching:** ITU-R BT.2446-1 §4.1, Tables 2–3. Uses the report's perceptual-domain three-segment knee and chroma correction. HDR and SDR peak luminance are configurable; the report reference values are 1,000 and 100 cd/m².
- **Method B — robust live conversion:** ITU-R BT.2446-1 §5.2.1–§5.2.4. For HLG sources, the HLG signal is re-interpreted at `Lw = 291 cd/m²` with gamma 1.03 as the report specifies; this is enabled by default. The input's HLG nominal peak `Lw` is configurable (default 1,000 cd/m²). Disabling the re-interpretation is a non-standard extension for linear non-HLG sources. Tone mapping keeps a 1:1 display-light range below the knee and compresses highlights with a logarithmic shoulder (unity slope at the knee); defaults are 55 cd/m² knee and strength 16, which places HDR reference white near 86% SDR. Out-of-volume colours are desaturated to the BT.709 volume in ICtCp, avoiding the hue shifts the report attributes to Yu'v' and L\*a\*b\*.
- **Method C — adjustable skin-tone mapping:** ITU-R BT.2446-1 §6.1.2–§6.1.8. Applies pre/post crosstalk matrices, converts through BT.2020 linear RGB and CIE Yxy, and joins a linear section to a logarithmic highlight curve. The gain, knee, HDR/SDR reference-white relationship and optional highlight desaturation are configurable. The default parameters follow the report's example: 50% HLG skin tone to 70% SDR, 80% SDR knee, 75% HLG reference white and 96% SDR reference-white signal. The SDR signal percentage is converted through the SDR EOTF before solving the linear-light curve.

The method is selected in the filter properties. Parameters relevant to the selected method are shown in the same properties panel.

## Processing and color representation

The current implementation evaluates the selected tone-mapping method directly in OBS's synchronous video pixel shader; it does not use a lookup texture. The input contract is linear scRGB with Rec.709 primaries (`GS_CS_709_SCRGB`, where OBS defines `1.0 = 80 cd/m²`) or OBS's `GS_CS_709_EXTENDED` range (where `1.0` is the configured SDR white level); `GS_CS_709_SCRGB` is requested first so that Windows Graphics Capture, game capture and monitor capture deliver native scRGB instead of an already-normalised extended range. Rec.709↔BT.2020 transforms are internal working-space steps for the report equations only.

The output is linear SDR light normalised to the white level of the render target the filter draws into: `100 cd/m²` for `GS_CS_SRGB`/`GS_CS_SRGB_16F` targets, the configured SDR white level for `GS_CS_709_EXTENDED`, and `80 cd/m²` for `GS_CS_709_SCRGB`. Because the filter reports `GS_CS_SRGB` for HDR inputs, libobs composites the result as a plain SDR source and performs no further colour-space mapping; on an SDR canvas the tone-mapped pixels reach the canvas untouched. On an HDR canvas the compositor still has to place the SDR result in the HDR container, so use an SDR canvas when the intent is a pure SDR output. The filter does not clamp RGB input/output to `[0, 1]`. Method B produces SDR display light in linear form, so the report's §5.2.5 inverse BT.1886 signal encoding is not applied; OBS encodes the linear values when it writes them into an 8-bit sRGB target. Method B performs its specified BT.709 colour-volume reduction.

## Input requirements

The filter expects linear Rec.709/scRGB input in `GS_CS_709_SCRGB`; OBS defines `1.0` as 80 cd/m². The `GS_CS_709_EXTENDED` EDR path uses OBS's configured SDR white level. Values are consumed as linear light; the filter does not infer or decode PQ/HLG/game-specific transfer functions. Sources in other color spaces are passed through unchanged. Method A/C HDR peak defaults to 1,000 cd/m².

## Build

Use the OBS Plugin Template CMake workflow. The plugin requires OBS Studio/libobs development files discoverable by CMake.

## Compatibility

`buildspec.json` pins libobs **32.0.4** with the matching `2025-08-23` obs-deps and Qt6. Some streaming clients ship libobs 32.0.4 and reject any module whose `obs_module_ver()` reports a newer major.minor version, so building against 32.0.4 is what lets the filter load there as well as in OBS Studio. Do not raise the pinned version past 32.0 without checking the host's `obs.dll` file version first; a 32.1+ build is refused with `compiled with newer libobs`.

The effect source is embedded in the module at build time, so a lone `obs-bt2446-tonemap.dll` is enough to create the filter. Such hosts load plugins from `<host>/plugins` and resolve module data under `<host>/data/plugins`, so a per-module data directory next to the DLL is not available there; `data/bt2446.effect` still takes precedence when a host does provide one, which keeps the shader patchable without a rebuild.

## Template provenance

The project started from the Git-tracked files in `D:\dev\obs-dev\obs-plugintemplate` at commit `3e7d7ac3b5342cd7d9b88890b9c70b472d1520fc`. See `TEMPLATE-SOURCE.txt`.
