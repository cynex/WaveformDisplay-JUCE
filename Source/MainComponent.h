#pragma once

#include <JuceHeader.h>
#include "AudioEngine.h"
#include "WaveformComponent.h"
#include "WaveformScrollbar.h"
#include "ParametersPanel.h"
#include "PresetStore.h"

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

    bool keyPressed(const juce::KeyPress& key) override;

    void loadFile(const juce::File& file);

private:
    void openFileChooser();
    void togglePlayPause();
    void savePreset();
    void loadPreset();
    void refreshPresetList();
    void applyPreset(const WaveformParameters& parameters);
    void showPresetError(const juce::String& message);
    void timerCallback() override;

    AudioEngine audioEngine;

    juce::TextButton openButton{ "Open..." };
    juce::TextButton playButton{ "Play" };
    juce::TextButton stopButton{ "Stop" };
    juce::TextButton rewindButton{ "<<" };
    juce::ToggleButton followPlayheadButton{ "Follow Playhead" };
    juce::ComboBox presetCombo;
    juce::TextEditor presetNameEditor;
    juce::TextButton savePresetButton{ "Save" };
    juce::TextButton loadPresetButton{ "Load" };
    juce::Label fileLabel{ {}, "No file loaded" };
    juce::Label frameTimeLabel;

    WaveformComponent waveform{ audioEngine };
    WaveformScrollbar scrollbar;
    ParametersPanel parametersPanel;
    PresetStore presetStore;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
