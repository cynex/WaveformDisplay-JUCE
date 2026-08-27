#pragma once

#include <JuceHeader.h>

/** All the tweakable knobs that control how the waveform is rendered.
    Passed by value/reference into WaveformComponent and mirrored by
    the ParametersPanel UI. */
struct WaveformParameters
{
    // Base colour of the waveform. The low/mid/high colours act as
    // multipliers on top of this colour, weighted by the amount of
    // low-frequency / mid-frequency / high-frequency energy present in each
    // analysed block.
    juce::Colour solidColour   { juce::Colours::white };
    juce::Colour lowFreqColour { juce::Colour(0xff0007ff) };
    juce::Colour midFreqColour { juce::Colour(0xffff00ff) };
    juce::Colour highFreqColour{ juce::Colour(0xffffcc00) };

    // How strongly the low/mid/high colours are allowed to tint the solid colour.
    float lowFreqAmount  = 1.0f;
    float midFreqAmount  = 1.0f;
    float highFreqAmount = 1.0f;

    // Master switch for the low/mid/high frequency tinting above - when
    // false the waveform renders as a flat solidColour regardless of the
    // amount sliders.
    bool tintingEnabled = true;

    // Signed-distance-field shaping.
    // aaWidth   - width (in pixels) of the anti-aliased edge transition.
    // smoothing - additional feathering applied on top of the AA edge,
    //             used to soften the waveform silhouette (e.g. when zoomed
    //             far out and many samples are collapsed per pixel).
    float aaWidth    = 1.5f;
    float smoothing  = 0.0f;

    // Background colour behind the waveform.
    juce::Colour backgroundColour{ juce::Colours::black };

    // Colour of the playback position indicator.
    juce::Colour playheadColour{ juce::Colours::red };

    // Opacity (0..1) of a solid white 1px horizontal line drawn through the
    // vertical centre of the waveform (the zero-amplitude axis). 0 hides it entirely.
    float centreLineAlpha = 0.3f;
};
