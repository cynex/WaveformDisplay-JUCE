#pragma once

#include <JuceHeader.h>
#include "WaveformParameters.h"

/** Small filled square with a white outline showing a colour parameter's
    current value, drawn just to the left of its label. */
class ColourSwatch : public juce::Component
{
public:
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

    ColourSwatch solidSwatch;
    ColourSwatch lowSwatch;
    ColourSwatch midSwatch;
    ColourSwatch highSwatch;
    ColourSwatch playheadSwatch;

    juce::TextButton solidColourButton{ "..." };
    juce::TextButton lowColourButton{ "..." };
    juce::TextButton midColourButton{ "..." };
    juce::TextButton highColourButton{ "..." };
    juce::TextButton playheadColourButton{ "..." };

    juce::ToggleButton tintingEnabledButton{ "Enable Tinting" };

    juce::Label lowAmountLabel{ {}, "Low Amount" };
    juce::Slider lowAmountSlider;
    juce::Label midAmountLabel{ {}, "Mid Amount" };
    juce::Slider midAmountSlider;
    juce::Label highAmountLabel{ {}, "High Amount" };
    juce::Slider highAmountSlider;

    juce::Label aaLabel{ {}, "AA Width" };
    juce::Slider aaSlider;
    juce::Label smoothingLabel{ {}, "Smoothing" };
    juce::Slider smoothingSlider;
    juce::Label centreLineAlphaLabel{ {}, "Centre Line" };
    juce::Slider centreLineAlphaSlider;

    WaveformParameters params;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParametersPanel)
};
