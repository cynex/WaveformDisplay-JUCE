#include "ParametersPanel.h"

namespace
{
    void configureSlider(juce::Slider& s, double min, double max, double value)
    {
        s.setRange(min, max, 0.001);
        s.setValue(value, juce::dontSendNotification);
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    }

    void showColourPicker(juce::Component& parent, juce::Colour current, std::function<void(juce::Colour)> callback)
    {
        auto selector = std::make_unique<juce::ColourSelector>(juce::ColourSelector::showColourAtTop
                                                                | juce::ColourSelector::showSliders
                                                                | juce::ColourSelector::showColourspace);
        selector->setCurrentColour(current);
        selector->setSize(300, 400);

        auto* selectorPtr = selector.get();

        juce::CallOutBox::launchAsynchronously(std::move(selector), parent.getScreenBounds(), nullptr);

        // Poll-free approach: use a ChangeListener via a small adaptor.
        struct Listener : juce::ChangeListener
        {
            std::function<void(juce::Colour)> cb;
            juce::ColourSelector* sel;
            void changeListenerCallback(juce::ChangeBroadcaster*) override { cb(sel->getCurrentColour()); }
        };
        static std::vector<std::unique_ptr<Listener>> listeners; // kept alive for app lifetime; panel is small in count
        auto l = std::make_unique<Listener>();
        l->cb = std::move(callback);
        l->sel = selectorPtr;
        selectorPtr->addChangeListener(l.get());
        listeners.push_back(std::move(l));
    }
}

ParametersPanel::ParametersPanel()
{
    addAndMakeVisible(solidLabel);
    addAndMakeVisible(lowLabel);
    addAndMakeVisible(midLabel);
    addAndMakeVisible(highLabel);
    addAndMakeVisible(solidSwatch);
    addAndMakeVisible(lowSwatch);
    addAndMakeVisible(midSwatch);
    addAndMakeVisible(highSwatch);
    addAndMakeVisible(playheadSwatch);
    addAndMakeVisible(solidColourButton);
    addAndMakeVisible(lowColourButton);
    addAndMakeVisible(midColourButton);
    addAndMakeVisible(highColourButton);
    addAndMakeVisible(playheadLabel);
    addAndMakeVisible(playheadColourButton);
    addAndMakeVisible(tintingEnabledButton);

    solidSwatch.setColour(params.solidColour);
    lowSwatch.setColour(params.lowFreqColour);
    midSwatch.setColour(params.midFreqColour);
    highSwatch.setColour(params.highFreqColour);
    playheadSwatch.setColour(params.playheadColour);

    addAndMakeVisible(lowAmountLabel);
    addAndMakeVisible(lowAmountSlider);
    addAndMakeVisible(midAmountLabel);
    addAndMakeVisible(midAmountSlider);
    addAndMakeVisible(highAmountLabel);
    addAndMakeVisible(highAmountSlider);

    addAndMakeVisible(aaLabel);
    addAndMakeVisible(aaSlider);
    addAndMakeVisible(smoothingLabel);
    addAndMakeVisible(smoothingSlider);
    addAndMakeVisible(centreLineAlphaLabel);
    addAndMakeVisible(centreLineAlphaSlider);

    configureSlider(lowAmountSlider, 0.0, 1.0, params.lowFreqAmount);
    configureSlider(midAmountSlider, 0.0, 1.0, params.midFreqAmount);
    configureSlider(highAmountSlider, 0.0, 1.0, params.highFreqAmount);
    configureSlider(aaSlider, 0.1, 8.0, params.aaWidth);
    configureSlider(smoothingSlider, 0.0, 1.0, params.smoothing);
    configureSlider(centreLineAlphaSlider, 0.0, 1.0, params.centreLineAlpha);
    tintingEnabledButton.setToggleState(params.tintingEnabled, juce::dontSendNotification);

    solidColourButton.onClick = [this]
    {
        showColourPicker(*this, params.solidColour, [this](juce::Colour c) { params.solidColour = c; solidSwatch.setColour(c); notify(); });
    };
    lowColourButton.onClick = [this]
    {
        showColourPicker(*this, params.lowFreqColour, [this](juce::Colour c) { params.lowFreqColour = c; lowSwatch.setColour(c); notify(); });
    };
    midColourButton.onClick = [this]
    {
        showColourPicker(*this, params.midFreqColour, [this](juce::Colour c) { params.midFreqColour = c; midSwatch.setColour(c); notify(); });
    };
    highColourButton.onClick = [this]
    {
        showColourPicker(*this, params.highFreqColour, [this](juce::Colour c) { params.highFreqColour = c; highSwatch.setColour(c); notify(); });
    };
    playheadColourButton.onClick = [this]
    {
        showColourPicker(*this, params.playheadColour, [this](juce::Colour c) { params.playheadColour = c; playheadSwatch.setColour(c); notify(); });
    };

    lowAmountSlider.onValueChange = [this] { params.lowFreqAmount = (float) lowAmountSlider.getValue(); notify(); };
    midAmountSlider.onValueChange = [this] { params.midFreqAmount = (float) midAmountSlider.getValue(); notify(); };
    highAmountSlider.onValueChange = [this] { params.highFreqAmount = (float) highAmountSlider.getValue(); notify(); };
    aaSlider.onValueChange = [this] { params.aaWidth = (float) aaSlider.getValue(); notify(); };
    smoothingSlider.onValueChange = [this] { params.smoothing = (float) smoothingSlider.getValue(); notify(); };
    centreLineAlphaSlider.onValueChange = [this] { params.centreLineAlpha = (float) centreLineAlphaSlider.getValue(); notify(); };
    tintingEnabledButton.onClick = [this] { params.tintingEnabled = tintingEnabledButton.getToggleState(); notify(); };
}

void ParametersPanel::setParameters(const WaveformParameters& p)
{
    params = p;
    lowAmountSlider.setValue(params.lowFreqAmount, juce::dontSendNotification);
    midAmountSlider.setValue(params.midFreqAmount, juce::dontSendNotification);
    highAmountSlider.setValue(params.highFreqAmount, juce::dontSendNotification);
    aaSlider.setValue(params.aaWidth, juce::dontSendNotification);
    smoothingSlider.setValue(params.smoothing, juce::dontSendNotification);
    centreLineAlphaSlider.setValue(params.centreLineAlpha, juce::dontSendNotification);
    tintingEnabledButton.setToggleState(params.tintingEnabled, juce::dontSendNotification);

    solidSwatch.setColour(params.solidColour);
    lowSwatch.setColour(params.lowFreqColour);
    midSwatch.setColour(params.midFreqColour);
    highSwatch.setColour(params.highFreqColour);
    playheadSwatch.setColour(params.playheadColour);
}

void ParametersPanel::notify()
{
    if (onChange != nullptr)
        onChange(params);
}

void ParametersPanel::resized()
{
    auto area = getLocalBounds().reduced(8);
    const int rowH = 24;
    const int gap = 6;
    const int swatchSize = rowH - 4;

    auto row = [&](juce::Component& label, juce::Component& control)
    {
        auto r = area.removeFromTop(rowH);
        label.setBounds(r.removeFromLeft(100));
        control.setBounds(r);
        area.removeFromTop(gap);
    };

    auto colourRow = [&](ColourSwatch& swatch, juce::Component& label, juce::Component& control)
    {
        auto r = area.removeFromTop(rowH);
        auto swatchArea = r.removeFromLeft(swatchSize);
        swatch.setBounds(swatchArea.withSizeKeepingCentre(swatchSize, swatchSize));
        r.removeFromLeft(gap);
        label.setBounds(r.removeFromLeft(100));
        control.setBounds(r);
        area.removeFromTop(gap);
    };

    colourRow(solidSwatch, solidLabel, solidColourButton);

    tintingEnabledButton.setBounds(area.removeFromTop(rowH));
    area.removeFromTop(gap);

    colourRow(lowSwatch, lowLabel, lowColourButton);
    colourRow(midSwatch, midLabel, midColourButton);
    colourRow(highSwatch, highLabel, highColourButton);
    colourRow(playheadSwatch, playheadLabel, playheadColourButton);

    row(lowAmountLabel, lowAmountSlider);
    row(midAmountLabel, midAmountSlider);
    row(highAmountLabel, highAmountSlider);
    row(aaLabel, aaSlider);
    row(smoothingLabel, smoothingSlider);
    row(centreLineAlphaLabel, centreLineAlphaSlider);
}
