#pragma once

#include <JuceHeader.h>
#include "WaveformParameters.h"

/** Small filled square with a white outline showing a colour parameter's
    current value, drawn just to the left of its label - and itself the
    clickable control that opens the colour picker (a separate "..." button
    for that was judged unintuitive; the swatch IS the colour, so clicking
    on it to change it is the more obvious affordance). */
class ColourSwatch : public juce::Component
{
public:
    ColourSwatch() { setMouseCursor(juce::MouseCursor::PointingHandCursor); }

    void setColour(juce::Colour newColour)
    {
        colour = newColour;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(colour);
        g.fillRect(bounds);
        g.setColour(juce::Colours::white);
        g.drawRect(bounds, 1.0f);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        // Only on a genuine click (release still within bounds), matching
        // normal button behaviour - a click-drag off the swatch and back
        // shouldn't accidentally fire it, and dragging away and releasing
        // elsewhere shouldn't either.
        if (contains(e.getPosition()) && onClick != nullptr)
            onClick();
    }

    std::function<void()> onClick;

private:
    juce::Colour colour;
};

/** Small side panel exposing the waveform rendering parameters:
    colours (solid / low-freq / high-freq tint) and SDF shaping
    (anti-aliasing width, smoothing). */
class ParametersPanel : public juce::Component
{
public:
    ParametersPanel();

    void setParameters(const WaveformParameters& p);
    std::function<void(const WaveformParameters&)> onChange;

    void resized() override;

private:
    void notify();

    juce::Label solidLabel{ {}, "Solid Colour" };
    juce::Label lowLabel{ {}, "Low-Freq Tint" };
    juce::Label midLabel{ {}, "Mid-Freq Tint" };
    juce::Label highLabel{ {}, "High-Freq Tint" };
    juce::Label playheadLabel{ {}, "Playhead" };
    juce::Label amplitudeLabel{ {}, "Amplitude Colour" };

    ColourSwatch solidSwatch;
    ColourSwatch lowSwatch;
    ColourSwatch midSwatch;
    ColourSwatch highSwatch;
    ColourSwatch playheadSwatch;
    ColourSwatch amplitudeSwatch;

    juce::ToggleButton tintingEnabledButton{ "Enable Tinting" };

    juce::Label lowAmountLabel{ {}, "Low Amount" };
    juce::Slider lowAmountSlider;
    juce::Label midAmountLabel{ {}, "Mid Amount" };
    juce::Slider midAmountSlider;
    juce::Label highAmountLabel{ {}, "High Amount" };
    juce::Slider highAmountSlider;

    juce::Label midPoleLabel{ {}, "Mid Pole" };
    juce::Slider midPoleSlider;

    juce::Label aaLabel{ {}, "AA Width" };
    juce::Slider aaSlider;
    juce::Label smoothingLabel{ {}, "Smoothing" };
    juce::Slider smoothingSlider;
    juce::Label waveformHeightLabel{ {}, "Waveform Height" };
    juce::Slider waveformHeightSlider;
    juce::Label centreLineAlphaLabel{ {}, "Centre Line" };
    juce::Slider centreLineAlphaSlider;

    // Placed after Centre Line, at the bottom of the slider stack - see the
    // constructor/resized() ordering.
    juce::Label amplitudeAmountLabel{ {}, "Amplitude Amount" };
    juce::Slider amplitudeAmountSlider;
    juce::Label amplitudeColorAmountLabel{ {}, "Amplitude Color Amt" };
    juce::Slider amplitudeColorAmountSlider;
    juce::Label amplitudeGlowRadiusLabel{ {}, "Amplitude Glow Radius" };
    juce::Slider amplitudeGlowRadiusSlider;
    juce::Label amplitudeRangeLabel{ {}, "Amplitude Range" };
    juce::Slider amplitudeRangeSlider;
    juce::Label amplitudeSlopeLabel{ {}, "Amplitude Slope" };
    juce::Slider amplitudeSlopeSlider;
    juce::Label amplitudeMinFreqLabel{ {}, "Amp Min Freq" };
    juce::Slider amplitudeMinFreqSlider;
    juce::Label amplitudeMaxFreqLabel{ {}, "Amp Max Freq" };
    juce::Slider amplitudeMaxFreqSlider;

    WaveformParameters params;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParametersPanel)
};
