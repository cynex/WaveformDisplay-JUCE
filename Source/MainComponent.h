#pragma once

#include <JuceHeader.h>
#include "AudioEngine.h"
#include "WaveformComponent.h"
#include "WaveformScrollbar.h"
#include "ParametersPanel.h"

class MainComponent : public juce::Component,
                       public juce::FileDragAndDropTarget,
                       private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    void loadFile(const juce::File& file);

private:
    void openFileChooser();
    void timerCallback() override;

    AudioEngine audioEngine;

    juce::TextButton openButton{ "Open..." };
    juce::TextButton playButton{ "Play" };
    juce::TextButton stopButton{ "Stop" };
    juce::TextButton rewindButton{ "<<" };
    juce::ToggleButton followPlayheadButton{ "Follow Playhead" };
    juce::Label fileLabel{ {}, "No file loaded" };
    juce::Label frameTimeLabel;

    WaveformComponent waveform{ audioEngine };
    WaveformScrollbar scrollbar;
    ParametersPanel parametersPanel;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
