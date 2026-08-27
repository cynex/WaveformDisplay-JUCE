#include "MainComponent.h"

MainComponent::MainComponent()
{
    addAndMakeVisible(openButton);
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(followPlayheadButton);
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
    followPlayheadButton.onClick = [this] { waveform.setFollowPlayhead(followPlayheadButton.getToggleState()); };

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

    setSize(1000, 600);
    startTimerHz(30);
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
    topBar.removeFromLeft(8);
    followPlayheadButton.setBounds(topBar.removeFromLeft(140));
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

    juce::String stats;
    stats << "Draw: " << juce::String(waveform.getLastDrawCallMs(), 3) << " ms"
          << "  (peak " << juce::String(waveform.getPeakDrawCallMs(), 3) << " ms)\n"
          << "Frame: " << juce::String(waveform.getLastFrameMs(), 3) << " ms"
          << "  (peak " << juce::String(waveform.getPeakFrameMs(), 3) << " ms)";
    frameTimeLabel.setText(stats, juce::dontSendNotification);
}
