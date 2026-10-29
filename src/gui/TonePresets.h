#pragma once
// Preview tone presets and tone-map operator labels (UI only).

#include "../core/Types.h"

namespace hdr::gui {

inline const char* toneMapOpName(ToneMapOp op) {
    switch (op) {
        case ToneMapOp::None: return "None (Linear)";
        case ToneMapOp::Reinhard: return "Reinhard (Classic)";
        case ToneMapOp::Filmic: return "Filmic (ACES)";
        case ToneMapOp::ReinhardLuma: return "Reinhard (Luma Preserving)";
        case ToneMapOp::Hable: return "Hable / Uncharted 2 (Filmic)";
        case ToneMapOp::AgX: return "AgX (Blender 4+)";
        case ToneMapOp::Lottes: return "Lottes (AMD)";
        case ToneMapOp::Uchimura: return "Uchimura (Gran Turismo)";
        default: return "Unknown";
    }
}

inline const char* toneMapOpTooltip(ToneMapOp op) {
    switch (op) {
        case ToneMapOp::None: return "Direct linear sensor radiance clamped to 1.0. Hard clipping for raw analysis.";
        case ToneMapOp::Reinhard: return "Standard asymptotic compression x/(1+x). Soft highlight roll-off.";
        case ToneMapOp::Filmic: return "Krzysztof Narkowicz ACES filmic S-curve. Rich contrast and natural shoulder.";
        case ToneMapOp::ReinhardLuma: return "Luminance-only extended Reinhard. Preserves color saturation and hits pure white.";
        case ToneMapOp::Hable: return "John Hable / Uncharted 2 curve. Distinct shoulder, linear midtones, and toe.";
        case ToneMapOp::AgX: return "Troy Sobotka AgX tone mapping. Avoids hue shifts in high-exposure highlights.";
        case ToneMapOp::Lottes: return "Timothy Lottes (AMD) sigmoid. Crisp midtones and clean highlight compression.";
        case ToneMapOp::Uchimura: return "Hajime Uchimura / Gran Turismo curve. Emulates physical camera & film response.";
        default: return "";
    }
}

enum class TonePreset {
    Default = 0,
    MaxDynamicRange,
    ShadowReveal,
    DeepShadowBoost,
    HighlightRecovery,
    ExtremeHighlights,
    LandscapeHDR,
    SunsetGoldenHour,
    ArchitectureCrisp,
    VividNature,
    Dramatic,
    SoftMatte,
    BlackAndWhiteHDR,
    ReinhardSmooth,
    LinearRaw,
    Count
};

inline ToneParams tonePreset(TonePreset p) {
    ToneParams t;
    switch (p) {
        case TonePreset::Default:
            t.toneMap = ToneMapOp::Filmic;
            break;
        case TonePreset::MaxDynamicRange:
            t.exposure = 0.0f;
            t.highlights = -100.0f;
            t.shadows = 100.0f;
            t.whites = -25.0f;
            t.blacks = 25.0f;
            t.contrast = -20.0f;
            t.vibrance = 20.0f;
            t.toneMap = ToneMapOp::Filmic;
            break;
        case TonePreset::ShadowReveal:
            t.exposure = 0.35f;
            t.highlights = -20.0f;
            t.shadows = 85.0f;
            t.whites = 0.0f;
            t.blacks = 30.0f;
            t.contrast = -15.0f;
            t.vibrance = 10.0f;
            t.toneMap = ToneMapOp::Filmic;
            break;
        case TonePreset::DeepShadowBoost:
            t.exposure = 1.0f;
            t.highlights = -40.0f;
            t.shadows = 100.0f;
            t.whites = -10.0f;
            t.blacks = 45.0f;
            t.contrast = -15.0f;
            t.vibrance = 15.0f;
            t.toneMap = ToneMapOp::Filmic;
            break;
        case TonePreset::HighlightRecovery:
            t.exposure = -0.70f;
            t.highlights = -100.0f;
            t.shadows = 25.0f;
            t.whites = -50.0f;
            t.blacks = 0.0f;
            t.contrast = 10.0f;
            t.vibrance = 15.0f;
            t.toneMap = ToneMapOp::Filmic;
            break;
        case TonePreset::ExtremeHighlights:
            t.exposure = -1.50f;
            t.highlights = -100.0f;
            t.shadows = 40.0f;
            t.whites = -75.0f;
            t.blacks = 0.0f;
            t.contrast = 15.0f;
            t.vibrance = 20.0f;
            t.toneMap = ToneMapOp::Filmic;
            break;
        case TonePreset::LandscapeHDR:
            t.exposure = 0.0f;
            t.highlights = -65.0f;
            t.shadows = 55.0f;
            t.whites = 15.0f;
            t.blacks = -10.0f;
            t.contrast = 15.0f;
            t.vibrance = 25.0f;
            t.saturation = 10.0f;
            t.toneMap = ToneMapOp::Filmic;
            break;
        case TonePreset::SunsetGoldenHour:
            t.exposure = 0.0f;
            t.temperature = 25.0f;
            t.tint = 10.0f;
            t.highlights = -75.0f;
            t.shadows = 60.0f;
            t.whites = 20.0f;
            t.blacks = -10.0f;
            t.contrast = 20.0f;
            t.vibrance = 35.0f;
            t.saturation = 15.0f;
            t.toneMap = ToneMapOp::Filmic;
            break;
        case TonePreset::ArchitectureCrisp:
            t.exposure = 0.0f;
            t.highlights = -50.0f;
            t.shadows = 45.0f;
            t.whites = 25.0f;
            t.blacks = -15.0f;
            t.contrast = 25.0f;
            t.vibrance = 15.0f;
            t.toneMap = ToneMapOp::Filmic;
            break;
        case TonePreset::VividNature:
            t.exposure = 0.0f;
            t.temperature = -5.0f;
            t.highlights = -60.0f;
            t.shadows = 50.0f;
            t.whites = 10.0f;
            t.blacks = -10.0f;
            t.contrast = 15.0f;
            t.vibrance = 40.0f;
            t.saturation = 15.0f;
            t.toneMap = ToneMapOp::Filmic;
            break;
        case TonePreset::Dramatic:
            t.exposure = 0.0f;
            t.highlights = -30.0f;
            t.shadows = -15.0f;
            t.whites = 25.0f;
            t.blacks = -30.0f;
            t.contrast = 40.0f;
            t.vibrance = 15.0f;
            t.saturation = 5.0f;
            t.toneMap = ToneMapOp::Filmic;
            break;
        case TonePreset::SoftMatte:
            t.exposure = 0.0f;
            t.highlights = -40.0f;
            t.shadows = 40.0f;
            t.whites = -20.0f;
            t.blacks = 35.0f;
            t.contrast = -25.0f;
            t.vibrance = 10.0f;
            t.saturation = -10.0f;
            t.toneMap = ToneMapOp::Filmic;
            break;
        case TonePreset::BlackAndWhiteHDR:
            t.exposure = 0.0f;
            t.highlights = -70.0f;
            t.shadows = 70.0f;
            t.whites = 30.0f;
            t.blacks = -25.0f;
            t.contrast = 35.0f;
            t.saturation = -100.0f;
            t.toneMap = ToneMapOp::Filmic;
            break;
        case TonePreset::ReinhardSmooth:
            t.exposure = 0.0f;
            t.highlights = -35.0f;
            t.shadows = 35.0f;
            t.whites = 10.0f;
            t.blacks = 0.0f;
            t.contrast = 10.0f;
            t.vibrance = 15.0f;
            t.toneMap = ToneMapOp::Reinhard;
            break;
        case TonePreset::LinearRaw:
            t.toneMap = ToneMapOp::None;
            break;
        default:
            break;
    }
    return t;
}

inline const char* tonePresetName(TonePreset p) {
    switch (p) {
        case TonePreset::Default: return "Default (Natural)";
        case TonePreset::MaxDynamicRange: return "Max Dynamic Range (Full DR)";
        case TonePreset::ShadowReveal: return "Shadow Reveal (Lift Dark Areas)";
        case TonePreset::DeepShadowBoost: return "Deep Shadow Boost (+1.0 EV)";
        case TonePreset::HighlightRecovery: return "Highlight Recovery (Recover Sky/Sun)";
        case TonePreset::ExtremeHighlights: return "Extreme Highlight Recovery (-1.5 EV)";
        case TonePreset::LandscapeHDR: return "Landscape HDR (Vibrant & Punchy)";
        case TonePreset::SunsetGoldenHour: return "Sunset & Golden Hour (Warm)";
        case TonePreset::ArchitectureCrisp: return "Architecture & Crisp Detail";
        case TonePreset::VividNature: return "Vivid Nature & Foliage";
        case TonePreset::Dramatic: return "Dramatic Contrast";
        case TonePreset::SoftMatte: return "Soft Matte (Fine Art)";
        case TonePreset::BlackAndWhiteHDR: return "Black & White HDR";
        case TonePreset::ReinhardSmooth: return "Reinhard Smooth Roll-off";
        case TonePreset::LinearRaw: return "Linear RAW (No Tone Curve)";
        default: return "Custom";
    }
}

inline const char* tonePresetTooltip(TonePreset p) {
    switch (p) {
        case TonePreset::Default: return "Natural filmic tone curve with neutral zeroed sliders.";
        case TonePreset::MaxDynamicRange: return "Maximum highlight suppression (-100) and shadow lift (+100) to inspect full bracketed range.";
        case TonePreset::ShadowReveal: return "Significantly lifts dark shadows and ground details while preserving highlights.";
        case TonePreset::DeepShadowBoost: return "Lifts shadows (+100) with +1.0 EV exposure to thoroughly inspect shadow noise and dark detail.";
        case TonePreset::HighlightRecovery: return "Pulls down exposure and highlights to recover sky and cloud texture.";
        case TonePreset::ExtremeHighlights: return "Drops exposure by -1.5 EV and pulls highlights to inspect extreme sun and sky saturation.";
        case TonePreset::LandscapeHDR: return "Punchy scenic look with controlled highlights, boosted shadows, and enhanced vibrance.";
        case TonePreset::SunsetGoldenHour: return "Warm golden tones, vibrant sky gradients, and rich shadows.";
        case TonePreset::ArchitectureCrisp: return "Crisp geometry, balanced facades, and architectural clarity.";
        case TonePreset::VividNature: return "Lush greens and blues with rich landscape contrast.";
        case TonePreset::Dramatic: return "Deep contrast, rich blacks, and saturated midtones.";
        case TonePreset::SoftMatte: return "Gentle low-contrast fine art look with lifted blacks.";
        case TonePreset::BlackAndWhiteHDR: return "Monochrome Ansel-Adams-style full zone system dynamic range.";
        case TonePreset::ReinhardSmooth: return "Asymptotic Reinhard compression for ultra-smooth highlight roll-off.";
        case TonePreset::LinearRaw: return "Pure unmapped linear sensor radiance without tone curves.";
        default: return "Custom slider settings.";
    }
}

inline TonePreset matchingTonePreset(const ToneParams& params) {
    for (int i = 0; i < static_cast<int>(TonePreset::Count); ++i) {
        auto p = static_cast<TonePreset>(i);
        if (params == tonePreset(p)) return p;
    }
    return TonePreset::Count;
}

}  // namespace hdr::gui
