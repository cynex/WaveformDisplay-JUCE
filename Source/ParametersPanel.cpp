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
    addAndMakeVisible(backgroundLabel);
    addAndMakeVisible(backgroundSwatch);
    addAndMakeVisible(solidLabel);
    addAndMakeVisible(lowLabel);
    addAndMakeVisible(midLabel);
    addAndMakeVisible(highLabel);
    addAndMakeVisible(solidSwatch);
    addAndMakeVisible(lowSwatch);
    addAndMakeVisible(midSwatch);
    addAndMakeVisible(highSwatch);
    addAndMakeVisible(playheadSwatch);
    addAndMakeVisible(playheadLabel);
    addAndMakeVisible(amplitudeLowSwatch);
    addAndMakeVisible(amplitudeLowLabel);
    addAndMakeVisible(amplitudeHighSwatch);
    addAndMakeVisible(amplitudeHighLabel);
    addAndMakeVisible(tintingEnabledButton);

    backgroundSwatch.setColour(params.backgroundColour);
    solidSwatch.setColour(params.solidColour);
    lowSwatch.setColour(params.lowFreqColour);
    midSwatch.setColour(params.midFreqColour);
    highSwatch.setColour(params.highFreqColour);
    playheadSwatch.setColour(params.playheadColour);
    amplitudeLowSwatch.setColour(params.amplitudeLowColour);
    amplitudeHighSwatch.setColour(params.amplitudeHighColour);

    addAndMakeVisible(lowAmountLabel);
    addAndMakeVisible(lowAmountSlider);
    addAndMakeVisible(midAmountLabel);
    addAndMakeVisible(midAmountSlider);
    addAndMakeVisible(highAmountLabel);
    addAndMakeVisible(highAmountSlider);

    addAndMakeVisible(midPoleLabel);
    addAndMakeVisible(midPoleSlider);

    addAndMakeVisible(aaLabel);
    addAndMakeVisible(aaSlider);
    addAndMakeVisible(smoothingLabel);
    addAndMakeVisible(smoothingSlider);
    addAndMakeVisible(waveformHeightLabel);
    addAndMakeVisible(waveformHeightSlider);
    addAndMakeVisible(centreLineAlphaLabel);
    addAndMakeVisible(centreLineAlphaSlider);

    addAndMakeVisible(amplitudeAmountLabel);
    addAndMakeVisible(amplitudeAmountSlider);
    addAndMakeVisible(amplitudeColorAmountLabel);
    addAndMakeVisible(amplitudeColorAmountSlider);
    addAndMakeVisible(amplitudeGlowRadiusLabel);
    addAndMakeVisible(amplitudeGlowRadiusSlider);
    addAndMakeVisible(amplitudeRangeLabel);
    addAndMakeVisible(amplitudeRangeSlider);
    addAndMakeVisible(amplitudeSlopeLabel);
    addAndMakeVisible(amplitudeSlopeSlider);
    addAndMakeVisible(amplitudeMinFreqLabel);
    addAndMakeVisible(amplitudeMinFreqSlider);
    addAndMakeVisible(amplitudeMaxFreqLabel);
    addAndMakeVisible(amplitudeMaxFreqSlider);

    configureSlider(lowAmountSlider, 0.0, 1.0, params.lowFreqAmount);
    configureSlider(midAmountSlider, 0.0, 1.0, params.midFreqAmount);
    configureSlider(highAmountSlider, 0.0, 1.0, params.highFreqAmount);
    configureSlider(midPoleSlider, 0.01, 0.99, params.midPole);
    configureSlider(aaSlider, 0.1, 8.0, params.aaWidth);
    configureSlider(smoothingSlider, 0.0, 1.0, params.smoothing);
    configureSlider(waveformHeightSlider, 0.1, 1.0, params.waveformHeight);
    configureSlider(centreLineAlphaSlider, 0.0, 1.0, params.centreLineAlpha);
    configureSlider(amplitudeAmountSlider, 0.0, 1.0, params.amplitudeAmount);
    configureSlider(amplitudeColorAmountSlider, 0.0, 4.0, params.amplitudeColorAmount);
    configureSlider(amplitudeGlowRadiusSlider, 0.0, 250.0, params.amplitudeGlowRadius);
    configureSlider(amplitudeRangeSlider, 0.0001, 0.25, params.amplitudeRange);
    configureSlider(amplitudeSlopeSlider, 1.0, 12.0, params.amplitudeSlope);

    // Frequency sliders are log-skewed (setSkewFactorFromMidPoint) rather
    // than linear - a linear 20-20000Hz slider would spend almost all of
    // its travel above a few kHz, making the low end (where the fixed
    // 300Hz low/mid split point actually lives) nearly unusable to dial in.
    configureSlider(amplitudeMinFreqSlider, 20.0, 20000.0, params.amplitudeMinFrequencyHz);
    amplitudeMinFreqSlider.setSkewFactorFromMidPoint(1000.0);
    amplitudeMinFreqSlider.setTextValueSuffix(" Hz");
    configureSlider(amplitudeMaxFreqSlider, 20.0, 20000.0, params.amplitudeMaxFrequencyHz);
    amplitudeMaxFreqSlider.setSkewFactorFromMidPoint(1000.0);
    amplitudeMaxFreqSlider.setTextValueSuffix(" Hz");

    tintingEnabledButton.setToggleState(params.tintingEnabled, juce::dontSendNotification);

    backgroundSwatch.onClick = [this]
    {
        showColourPicker(*this, params.backgroundColour, [this](juce::Colour c) { params.backgroundColour = c; backgroundSwatch.setColour(c); notify(); });
    };
    solidSwatch.onClick = [this]
    {
        showColourPicker(*this, params.solidColour, [this](juce::Colour c) { params.solidColour = c; solidSwatch.setColour(c); notify(); });
    };
    lowSwatch.onClick = [this]
    {
        showColourPicker(*this, params.lowFreqColour, [this](juce::Colour c) { params.lowFreqColour = c; lowSwatch.setColour(c); notify(); });
    };
    midSwatch.onClick = [this]
    {
        showColourPicker(*this, params.midFreqColour, [this](juce::Colour c) { params.midFreqColour = c; midSwatch.setColour(c); notify(); });
    };
    highSwatch.onClick = [this]
    {
        showColourPicker(*this, params.highFreqColour, [this](juce::Colour c) { params.highFreqColour = c; highSwatch.setColour(c); notify(); });
    };
    playheadSwatch.onClick = [this]
    {
        showColourPicker(*this, params.playheadColour, [this](juce::Colour c) { params.playheadColour = c; playheadSwatch.setColour(c); notify(); });
    };
    amplitudeLowSwatch.onClick = [this]
    {
        showColourPicker(*this, params.amplitudeLowColour, [this](juce::Colour c) { params.amplitudeLowColour = c; amplitudeLowSwatch.setColour(c); notify(); });
    };
    amplitudeHighSwatch.onClick = [this]
    {
        showColourPicker(*this, params.amplitudeHighColour, [this](juce::Colour c) { params.amplitudeHighColour = c; amplitudeHighSwatch.setColour(c); notify(); });
    };

    lowAmountSlider.onValueChange = [this] { params.lowFreqAmount = (float) lowAmountSlider.getValue(); notify(); };
    midAmountSlider.onValueChange = [this] { params.midFreqAmount = (float) midAmountSlider.getValue(); notify(); };
    highAmountSlider.onValueChange = [this] { params.highFreqAmount = (float) highAmountSlider.getValue(); notify(); };
    midPoleSlider.onValueChange = [this] { params.midPole = (float) midPoleSlider.getValue(); notify(); };
    aaSlider.onValueChange = [this] { params.aaWidth = (float) aaSlider.getValue(); notify(); };
    smoothingSlider.onValueChange = [this] { params.smoothing = (float) smoothingSlider.getValue(); notify(); };
    waveformHeightSlider.onValueChange = [this] { params.waveformHeight = (float) waveformHeightSlider.getValue(); notify(); };
    centreLineAlphaSlider.onValueChange = [this] { params.centreLineAlpha = (float) centreLineAlphaSlider.getValue(); notify(); };
    amplitudeAmountSlider.onValueChange = [this] { params.amplitudeAmount = (float) amplitudeAmountSlider.getValue(); notify(); };
    amplitudeColorAmountSlider.onValueChange = [this] { params.amplitudeColorAmount = (float) amplitudeColorAmountSlider.getValue(); notify(); };
    amplitudeGlowRadiusSlider.onValueChange = [this] { params.amplitudeGlowRadius = (float) amplitudeGlowRadiusSlider.getValue(); notify(); };
    amplitudeRangeSlider.onValueChange = [this] { params.amplitudeRange = (float) amplitudeRangeSlider.getValue(); notify(); };
    amplitudeSlopeSlider.onValueChange = [this] { params.amplitudeSlope = (float) amplitudeSlopeSlider.getValue(); notify(); };
    amplitudeMinFreqSlider.onValueChange = [this] { params.amplitudeMinFrequencyHz = (float) amplitudeMinFreqSlider.getValue(); notify(); };
    amplitudeMaxFreqSlider.onValueChange = [this] { params.amplitudeMaxFrequencyHz = (float) amplitudeMaxFreqSlider.getValue(); notify(); };
    tintingEnabledButton.onClick = [this] { params.tintingEnabled = tintingEnabledButton.getToggleState(); notify(); };
}

void ParametersPanel::setParameters(const WaveformParameters& p)
{
    params = p;
    lowAmountSlider.setValue(params.lowFreqAmount, juce::dontSendNotification);
    midAmountSlider.setValue(params.midFreqAmount, juce::dontSendNotification);
    highAmountSlider.setValue(params.highFreqAmount, juce::dontSendNotification);
    midPoleSlider.setValue(params.midPole, juce::dontSendNotification);
    aaSlider.setValue(params.aaWidth, juce::dontSendNotification);
    smoothingSlider.setValue(params.smoothing, juce::dontSendNotification);
    waveformHeightSlider.setValue(params.waveformHeight, juce::dontSendNotification);
    centreLineAlphaSlider.setValue(params.centreLineAlpha, juce::dontSendNotification);
    amplitudeAmountSlider.setValue(params.amplitudeAmount, juce::dontSendNotification);
    amplitudeColorAmountSlider.setValue(params.amplitudeColorAmount, juce::dontSendNotification);
    amplitudeGlowRadiusSlider.setValue(params.amplitudeGlowRadius, juce::dontSendNotification);
    amplitudeRangeSlider.setValue(params.amplitudeRange, juce::dontSendNotification);
    amplitudeSlopeSlider.setValue(params.amplitudeSlope, juce::dontSendNotification);
    amplitudeMinFreqSlider.setValue(params.amplitudeMinFrequencyHz, juce::dontSendNotification);
    amplitudeMaxFreqSlider.setValue(params.amplitudeMaxFrequencyHz, juce::dontSendNotification);
    tintingEnabledButton.setToggleState(params.tintingEnabled, juce::dontSendNotification);

    backgroundSwatch.setColour(params.backgroundColour);
    solidSwatch.setColour(params.solidColour);
    lowSwatch.setColour(params.lowFreqColour);
    midSwatch.setColour(params.midFreqColour);
    highSwatch.setColour(params.highFreqColour);
    playheadSwatch.setColour(params.playheadColour);
    amplitudeLowSwatch.setColour(params.amplitudeLowColour);
    amplitudeHighSwatch.setColour(params.amplitudeHighColour);
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

    auto colourRow = [&](ColourSwatch& swatch, juce::Component& label)
    {
        auto r = area.removeFromTop(rowH);
        auto swatchArea = r.removeFromLeft(swatchSize);
        swatch.setBounds(swatchArea.withSizeKeepingCentre(swatchSize, swatchSize));
        r.removeFromLeft(gap);
        label.setBounds(r);
        area.removeFromTop(gap);
    };

    colourRow(backgroundSwatch, backgroundLabel);
    colourRow(solidSwatch, solidLabel);

    tintingEnabledButton.setBounds(area.removeFromTop(rowH));
    area.removeFromTop(gap);

    colourRow(lowSwatch, lowLabel);
    colourRow(midSwatch, midLabel);
    colourRow(highSwatch, highLabel);
    colourRow(playheadSwatch, playheadLabel);
    colourRow(amplitudeLowSwatch, amplitudeLowLabel);
    colourRow(amplitudeHighSwatch, amplitudeHighLabel);

    row(lowAmountLabel, lowAmountSlider);
    row(midAmountLabel, midAmountSlider);
    row(highAmountLabel, highAmountSlider);
    row(midPoleLabel, midPoleSlider);
    row(aaLabel, aaSlider);
    row(smoothingLabel, smoothingSlider);
    row(waveformHeightLabel, waveformHeightSlider);
    row(centreLineAlphaLabel, centreLineAlphaSlider);
    row(amplitudeAmountLabel, amplitudeAmountSlider);
    row(amplitudeColorAmountLabel, amplitudeColorAmountSlider);
    row(amplitudeGlowRadiusLabel, amplitudeGlowRadiusSlider);
    row(amplitudeRangeLabel, amplitudeRangeSlider);
    row(amplitudeSlopeLabel, amplitudeSlopeSlider);
    row(amplitudeMinFreqLabel, amplitudeMinFreqSlider);
    row(amplitudeMaxFreqLabel, amplitudeMaxFreqSlider);
}
