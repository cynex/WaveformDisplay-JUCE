#include "MainComponent.h"

MainComponent::MainComponent()
{
    addAndMakeVisible(openButton);
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(rewindButton);
    addAndMakeVisible(followPlayheadButton);
    addAndMakeVisible(presetCombo);
    addAndMakeVisible(presetNameEditor);
    addAndMakeVisible(savePresetButton);
    addAndMakeVisible(loadPresetButton);
    addAndMakeVisible(fileLabel);
    addAndMakeVisible(waveform);
    addAndMakeVisible(scrollbar);
    addAndMakeVisible(parametersPanel);
    addAndMakeVisible(frameTimeLabel);

    fileLabel.setJustificationType(juce::Justification::centredLeft);
    frameTimeLabel.setJustificationType(juce::Justification::topRight);
    frameTimeLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey.withAlpha(0.8f));
    frameTimeLabel.setColour(juce::Label::backgroundColourId, juce::Colours::black.withAlpha(0.4f));
    frameTimeLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    frameTimeLabel.setMinimumHorizontalScale(1.0f);
    frameTimeLabel.setText("-- ms", juce::dontSendNotification);

    openButton.onClick = [this] { openFileChooser(); };
    playButton.onClick = [this] { audioEngine.play(); };
    stopButton.onClick = [this] { audioEngine.stop(); };
    rewindButton.onClick = [this] { audioEngine.setPosition(0.0); waveform.notifyPositionChangedExternally(); };
    followPlayheadButton.onClick = [this] { waveform.setFollowPlayhead(followPlayheadButton.getToggleState()); };
    savePresetButton.onClick = [this] { savePreset(); };
    loadPresetButton.onClick = [this] { loadPreset(); };

    presetCombo.setTextWhenNothingSelected("Presets...");
    presetCombo.setTextWhenNoChoicesAvailable("No presets yet");
    presetCombo.setTooltip("Choose an existing preset to recall or overwrite");
    presetCombo.onChange = [this]
    {
        if (presetCombo.getSelectedId() > 0)
        {
            presetNameEditor.setText(presetCombo.getText(), juce::dontSendNotification);
            loadPreset();
        }
    };
    presetNameEditor.setTextToShowWhenEmpty("New preset name", juce::Colours::grey);
    presetNameEditor.setTooltip("Type a name here, then click Save");
    savePresetButton.setTooltip("Save all current waveform settings to this preset");
    loadPresetButton.setTooltip("Recall the selected preset");

    followPlayheadButton.setToggleState(true, juce::dontSendNotification);
    waveform.setFollowPlayhead(true);

    // These buttons would otherwise grab keyboard focus and treat the space
    // bar as "click me" (JUCE's default Button behaviour) instead of it
    // reaching MainComponent::keyPressed as a play/pause toggle.
    openButton.setWantsKeyboardFocus(false);
    playButton.setWantsKeyboardFocus(false);
    stopButton.setWantsKeyboardFocus(false);
    rewindButton.setWantsKeyboardFocus(false);
    followPlayheadButton.setWantsKeyboardFocus(false);
    savePresetButton.setWantsKeyboardFocus(false);
    loadPresetButton.setWantsKeyboardFocus(false);

    setWantsKeyboardFocus(true);

    waveform.onViewRangeChanged = [this]
    {
        scrollbar.setRange(waveform.getTotalLength(), waveform.getViewStart(), waveform.getViewLength());
    };

    scrollbar.onScroll = [this](double newViewStart)
    {
        waveform.setViewRange(newViewStart, waveform.getViewLength());
        scrollbar.setRange(waveform.getTotalLength(), waveform.getViewStart(), waveform.getViewLength());
    };

    parametersPanel.setParameters(waveform.getParameters());
    parametersPanel.onChange = [this](const WaveformParameters& p)
    {
        waveform.setParameters(p);
    };

    juce::String presetError;
    if (!presetStore.reload(presetError))
    {
        showPresetError(presetError);
    }
    else
    {
        refreshPresetList();

        WaveformParameters startupPreset;
        if (presetStore.recallPreset(presetStore.getLastPresetName(), startupPreset))
        {
            applyPreset(startupPreset);
            presetCombo.setText(presetStore.getLastPresetName(), juce::dontSendNotification);
            presetNameEditor.setText(presetStore.getLastPresetName(), juce::dontSendNotification);
        }
    }

    setSize(1000, 600);
    startTimerHz(30);

    // Deferred rather than called directly: at this point in the
    // constructor MainComponent hasn't been parented into the DocumentWindow
    // yet (Main.cpp's setContentOwned() does that right after constructing
    // this), so grabbing focus now would silently fail. By the time the
    // message loop gets to this callback, it has been.
    juce::MessageManager::callAsync([this] { grabKeyboardFocus(); });
}

MainComponent::~MainComponent()
{
    fileChooser.reset();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::darkgrey.darker());
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    auto topBar = area.removeFromTop(36).reduced(4);
    openButton.setBounds(topBar.removeFromLeft(80));
    topBar.removeFromLeft(4);
    playButton.setBounds(topBar.removeFromLeft(70));
    topBar.removeFromLeft(4);
    stopButton.setBounds(topBar.removeFromLeft(70));
    topBar.removeFromLeft(4);
    rewindButton.setBounds(topBar.removeFromLeft(40));
    topBar.removeFromLeft(8);
    followPlayheadButton.setBounds(topBar.removeFromLeft(140));
    topBar.removeFromLeft(8);
    presetCombo.setBounds(topBar.removeFromLeft(110));
    topBar.removeFromLeft(4);
    presetNameEditor.setBounds(topBar.removeFromLeft(120));
    topBar.removeFromLeft(4);
    savePresetButton.setBounds(topBar.removeFromLeft(52));
    topBar.removeFromLeft(4);
    loadPresetButton.setBounds(topBar.removeFromLeft(52));
    topBar.removeFromLeft(8);
    fileLabel.setBounds(topBar);

    auto paramsArea = area.removeFromRight(240);
    parametersPanel.setBounds(paramsArea);

    auto scrollbarArea = area.removeFromBottom(20).reduced(4, 2);
    scrollbar.setBounds(scrollbarArea);

    waveform.setBounds(area.reduced(4));

    // Bottom-right corner of the whole window, overlaid on top of whatever
    // else is there (params panel / scrollbar). Tall enough for the two
    // lines of draw-call/frame timing stats.
    auto fullBounds = getLocalBounds();
    frameTimeLabel.setBounds(fullBounds.removeFromBottom(40).removeFromRight(230));
}

void MainComponent::savePreset()
{
    const auto name = presetNameEditor.getText().trim();
    juce::String error;
    if (!presetStore.savePreset(name, waveform.getParameters(), error))
    {
        showPresetError(error);
        return;
    }

    refreshPresetList();
    presetCombo.setText(name, juce::dontSendNotification);
    presetNameEditor.setText(name, juce::dontSendNotification);
}

void MainComponent::loadPreset()
{
    const auto name = presetCombo.getText().trim();
    WaveformParameters parameters;
    if (!presetStore.recallPreset(name, parameters))
    {
        showPresetError(name.isEmpty() ? "Choose a preset to load."
                                       : "No preset named \"" + name + "\" was found.");
        return;
    }

    applyPreset(parameters);

    juce::String error;
    if (!presetStore.rememberLastPreset(name, error))
        showPresetError(error);
}

void MainComponent::refreshPresetList()
{
    const auto selectedText = presetCombo.getText();
    presetCombo.clear(juce::dontSendNotification);

    const auto names = presetStore.getPresetNames();
    for (int i = 0; i < names.size(); ++i)
        presetCombo.addItem(names[i], i + 1);

    presetCombo.setText(selectedText, juce::dontSendNotification);
}

void MainComponent::applyPreset(const WaveformParameters& parameters)
{
    waveform.setParameters(parameters);
    parametersPanel.setParameters(parameters);
}

void MainComponent::showPresetError(const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                           "Preset Error",
                                           message);
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (auto& f : files)
    {
        auto ext = juce::File(f).getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".flac" || ext == ".ogg" || ext == ".mp3" || ext == ".aiff" || ext == ".aif")
            return true;
    }
    return false;
}

void MainComponent::filesDropped(const juce::StringArray& files, int, int)
{
    if (files.size() > 0)
        loadFile(juce::File(files[0]));
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        togglePlayPause();
        return true;
    }

    return false;
}

void MainComponent::togglePlayPause()
{
    if (!audioEngine.hasFileLoaded())
        return;

    if (audioEngine.isPlaying())
        audioEngine.stop();
    else
        audioEngine.play();
}

void MainComponent::openFileChooser()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Select an audio file...",
        juce::File(),
        "*.wav;*.flac;*.ogg;*.mp3;*.aiff;*.aif");

    auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync(chooserFlags, [this](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file != juce::File())
            loadFile(file);
    });
}

void MainComponent::loadFile(const juce::File& file)
{
    if (audioEngine.loadFile(file))
    {
        fileLabel.setText(file.getFileName(), juce::dontSendNotification);
        waveform.notifyFileChanged();
        waveform.setViewRange(0.0, (double) audioEngine.getTotalNumSamples());
        scrollbar.setRange(waveform.getTotalLength(), waveform.getViewStart(), waveform.getViewLength());
    }
    else
    {
        fileLabel.setText("Failed to load: " + file.getFileName(), juce::dontSendNotification);
    }
}

void MainComponent::timerCallback()
{
    playButton.setEnabled(audioEngine.hasFileLoaded());
    stopButton.setEnabled(audioEngine.hasFileLoaded());
    rewindButton.setEnabled(audioEngine.hasFileLoaded());

    juce::String stats;
    stats << "Draw: " << juce::String(waveform.getLastDrawCallMs(), 3) << " ms"
          << "  (peak " << juce::String(waveform.getPeakDrawCallMs(), 3) << " ms)\n"
          << "Frame: " << juce::String(waveform.getLastFrameMs(), 3) << " ms"
          << "  (peak " << juce::String(waveform.getPeakFrameMs(), 3) << " ms)";
    frameTimeLabel.setText(stats, juce::dontSendNotification);
}
